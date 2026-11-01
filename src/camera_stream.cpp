#include "camera_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <libcamera/control_ids.h>
#include <libcamera/libcamera.h>
#include <libcamera/property_ids.h>

using namespace std::chrono_literals;

namespace {
struct MappedPlane {
  void *address = nullptr;
  size_t length = 0;
};

// Approximate Raspberry Pi IMX708 / IMX219 Red & Blue colour gains for a given
// colour temperature in Kelvin (2500K .. 10000K) using piecewise linear
// interpolation across calibrated reference points.
void kelvinToColourGains(int kelvin, float &outRed, float &outBlue) {
  struct GainPoint {
    int k;
    float r;
    float b;
  };
  static constexpr GainPoint kPoints[] = {
      {2500, 1.15f, 2.75f}, {3000, 1.32f, 2.38f}, {4000, 1.62f, 1.95f},
      {5000, 1.88f, 1.68f}, {5500, 2.00f, 1.56f}, {6500, 2.22f, 1.40f},
      {7500, 2.42f, 1.28f}, {8500, 2.60f, 1.18f}, {10000, 2.82f, 1.08f},
  };
  constexpr size_t kNumPoints = sizeof(kPoints) / sizeof(kPoints[0]);

  int clamped = std::clamp(kelvin, kPoints[0].k, kPoints[kNumPoints - 1].k);
  for (size_t i = 0; i + 1 < kNumPoints; ++i) {
    if (clamped >= kPoints[i].k && clamped <= kPoints[i + 1].k) {
      float t = static_cast<float>(clamped - kPoints[i].k) /
                static_cast<float>(kPoints[i + 1].k - kPoints[i].k);
      outRed = kPoints[i].r + t * (kPoints[i + 1].r - kPoints[i].r);
      outBlue = kPoints[i].b + t * (kPoints[i + 1].b - kPoints[i].b);
      return;
    }
  }
  outRed = 2.00f;
  outBlue = 1.56f;
}
} // namespace

// C++ internal representation for CameraStream
struct CameraStream {
  // Viewfinder (preview) stream properties
  int m_requestedVfWidth = 480;
  int m_requestedVfHeight = 320;
  int m_width = 480;
  int m_height = 320;
  int m_stride = 0;
  // True when pixel bytes in memory are ordered [B, G, R] (SDL_PIXELFORMAT_BGR24).
  // Note: libcamera/DRM pixel format names follow little-endian 24-bit word
  // conventions where `formats::RGB888` stores [B, G, R] in memory and
  // `formats::BGR888` stores [R, G, B] in memory.
  bool m_memoryOrderIsBGR = false;

  // High-resolution still stream properties (0 = auto sensor maximum)
  int m_requestedStillWidth = 0;
  int m_requestedStillHeight = 0;
  int m_stillWidth = 0;
  int m_stillHeight = 0;
  int m_stillStride = 0;
  bool m_stillMemoryOrderIsBGR = false;
  bool m_dualStreamActive = false;

  bool m_afSupported = false;
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_afTriggerPending{false};
  std::atomic<bool> m_afContinuousPending{false};

  std::unique_ptr<libcamera::CameraManager> m_cameraManager;
  std::shared_ptr<libcamera::Camera> m_camera;
  std::unique_ptr<libcamera::CameraConfiguration> m_config;
  std::unique_ptr<libcamera::FrameBufferAllocator> m_allocator;

  libcamera::Stream *m_stream = nullptr; // Viewfinder stream
  libcamera::Stream *m_stillStream =
      nullptr; // High-res still stream (when dual-stream active)

  std::vector<std::unique_ptr<libcamera::Request>> m_requests;
  std::map<libcamera::Request *, libcamera::FrameBuffer *> m_requestVfBuffers;
  std::vector<libcamera::FrameBuffer *> m_stillBuffers;
  std::map<int, MappedPlane> m_mappedBuffers;

  // Mutex & double-buffering for thread-safe preview access from C main loop
  std::mutex m_frameMutex;
  std::vector<uint8_t> m_latestFrame;
  uint64_t m_frameSequence = 0;
  bool m_hasNewFrame = false;

  // Synchronization & buffer for high-resolution still capture
  std::mutex m_stillMutex;
  std::condition_variable m_stillCv;
  std::atomic<bool> m_stillCapturePending{false};
  std::atomic<bool> m_asyncStillInFlight{false};
  std::chrono::steady_clock::time_point m_asyncStillStartTime{};
  bool m_stillFrameReady = false;
  std::vector<uint8_t> m_stillFrame;
  int m_capturedStillWidth = 0;
  int m_capturedStillHeight = 0;
  int m_capturedStillStride = 0;
  bool m_capturedStillMemoryOrderIsBGR = false;
  CameraExposureStatus m_capturedStillExpStatus{};

  std::string m_cameraModel = "RubusCam (libcamera)";

  // AF cycle & lens tracking
  std::atomic<int> m_afState{0};
  std::atomic<float> m_lastLensPosition{0.0f};
  std::atomic<bool> m_hasLensPosition{false};

  // Exposure, White Balance, and Metering control settings & live sensor
  // metadata
  std::atomic<float> m_evCompensation{0.0f};
  std::atomic<int> m_isoSetting{0};           // 0 = Auto, 100 ~ 3200
  std::atomic<int32_t> m_shutterUsSetting{0}; // 0 = Auto, >0 = manual us
  std::atomic<int> m_wbMode{CAMERA_WB_MODE_AUTO};
  std::atomic<int> m_wbKelvin{5500}; // 2500 ~ 10000 K
  std::atomic<bool> m_awbLocked{false};
  std::atomic<float> m_lockedRedGain{2.00f};
  std::atomic<float> m_lockedBlueGain{1.56f};
  std::atomic<int> m_meteringMode{CAMERA_METERING_MATRIX};

  std::atomic<int> m_evPendingCount{0};
  std::atomic<int> m_isoPendingCount{0};
  std::atomic<int> m_shutterPendingCount{0};
  std::atomic<int> m_wbPendingCount{0};
  std::atomic<int> m_meteringPendingCount{0};

  std::atomic<float> m_actualGain{1.0f};
  std::atomic<int32_t> m_actualExposureUs{0};
  std::atomic<int32_t> m_actualColourTempK{5500};
  std::atomic<float> m_actualRedGain{2.00f};
  std::atomic<float> m_actualBlueGain{1.56f};

  void applyWhiteBalanceToControlList(libcamera::ControlList &ctrls) const;
  void applyMeteringToControlList(libcamera::ControlList &ctrls) const;
  void populateExposureStatus(CameraExposureStatus *out_status) const;
  void applyStartExposureControls(libcamera::ControlList &ctrls) const;
  void applyPendingExposureControls(libcamera::ControlList &ctrls);
  void requestCompleted(libcamera::Request *request);
  void unmapBuffers();
  bool allocateAndMapStreams();
  void teardownStreams();
  bool configureDualOrSingleStream();
  bool captureStillByModeSwitch(std::vector<uint8_t> &outBuffer, int &outWidth,
                                int &outHeight, int &outStride,
                                bool &outMemoryOrderIsBGR);
};

void CameraStream::applyWhiteBalanceToControlList(
    libcamera::ControlList &ctrls) const {
  if (!m_camera) {
    return;
  }
  const auto &camCtrls = m_camera->controls();
  const bool hasAwbEnable =
      camCtrls.find(&libcamera::controls::AwbEnable) != camCtrls.end();
  const bool hasAwbMode =
      camCtrls.find(&libcamera::controls::AwbMode) != camCtrls.end();
  const bool hasColourGains =
      camCtrls.find(&libcamera::controls::ColourGains) != camCtrls.end();
  const bool hasColourTemp =
      camCtrls.find(&libcamera::controls::ColourTemperature) != camCtrls.end();

  if (m_awbLocked.load()) {
    if (hasAwbEnable) {
      ctrls.set(libcamera::controls::AwbEnable, false);
    }
    if (hasColourGains) {
      float gains[2] = {m_lockedRedGain.load(), m_lockedBlueGain.load()};
      ctrls.set(libcamera::controls::ColourGains, gains);
    }
    return;
  }

  const auto mode = static_cast<CameraWbMode>(m_wbMode.load());
  switch (mode) {
  case CAMERA_WB_MODE_AUTO:
    if (hasAwbEnable) {
      ctrls.set(libcamera::controls::AwbEnable, true);
    }
    if (hasAwbMode) {
      ctrls.set(libcamera::controls::AwbMode, libcamera::controls::AwbAuto);
    }
    break;

  case CAMERA_WB_MODE_DAYLIGHT:
    if (hasAwbEnable) {
      ctrls.set(libcamera::controls::AwbEnable, true);
    }
    if (hasAwbMode) {
      ctrls.set(libcamera::controls::AwbMode, libcamera::controls::AwbDaylight);
    }
    break;

  case CAMERA_WB_MODE_CLOUDY:
    if (hasAwbEnable) {
      ctrls.set(libcamera::controls::AwbEnable, true);
    }
    if (hasAwbMode) {
      ctrls.set(libcamera::controls::AwbMode, libcamera::controls::AwbCloudy);
    }
    break;

  case CAMERA_WB_MODE_TUNGSTEN:
    if (hasAwbEnable) {
      ctrls.set(libcamera::controls::AwbEnable, true);
    }
    if (hasAwbMode) {
      ctrls.set(libcamera::controls::AwbMode, libcamera::controls::AwbTungsten);
    }
    break;

  case CAMERA_WB_MODE_FLUORESCENT:
    if (hasAwbEnable) {
      ctrls.set(libcamera::controls::AwbEnable, true);
    }
    if (hasAwbMode) {
      ctrls.set(libcamera::controls::AwbMode,
                libcamera::controls::AwbFluorescent);
    }
    break;

  case CAMERA_WB_MODE_SHADE:
  case CAMERA_WB_MODE_KELVIN: {
    const int targetK =
        (mode == CAMERA_WB_MODE_SHADE) ? 7500 : m_wbKelvin.load();
    if (hasAwbEnable) {
      ctrls.set(libcamera::controls::AwbEnable, false);
    }
    if (hasColourTemp) {
      ctrls.set(libcamera::controls::ColourTemperature, targetK);
    }
    if (hasColourGains) {
      float rGain = 2.00f;
      float bGain = 1.56f;
      kelvinToColourGains(targetK, rGain, bGain);
      float gains[2] = {rGain, bGain};
      ctrls.set(libcamera::controls::ColourGains, gains);
    }
    break;
  }
  }
}

void CameraStream::applyMeteringToControlList(
    libcamera::ControlList &ctrls) const {
  if (!m_camera) {
    return;
  }
  const auto &camCtrls = m_camera->controls();
  if (camCtrls.find(&libcamera::controls::AeMeteringMode) == camCtrls.end()) {
    return;
  }

  const auto mode = static_cast<CameraMeteringMode>(m_meteringMode.load());
  int32_t libcamMetering = libcamera::controls::MeteringMatrix;
  switch (mode) {
  case CAMERA_METERING_CENTRE_WEIGHTED:
    libcamMetering = libcamera::controls::MeteringCentreWeighted;
    break;
  case CAMERA_METERING_SPOT:
    libcamMetering = libcamera::controls::MeteringSpot;
    break;
  case CAMERA_METERING_MATRIX:
  default:
    libcamMetering = libcamera::controls::MeteringMatrix;
    break;
  }
  ctrls.set(libcamera::controls::AeMeteringMode, libcamMetering);
}

void CameraStream::populateExposureStatus(
    CameraExposureStatus *out_status) const {
  if (!out_status) {
    return;
  }
  out_status->ev_compensation = m_evCompensation.load();
  out_status->iso_setting = m_isoSetting.load();
  out_status->shutter_us_setting = m_shutterUsSetting.load();
  out_status->actual_analogue_gain = m_actualGain.load();
  out_status->actual_exposure_us = m_actualExposureUs.load();
  out_status->wb_mode = static_cast<CameraWbMode>(m_wbMode.load());
  out_status->wb_kelvin_setting = m_wbKelvin.load();
  out_status->awb_locked = m_awbLocked.load();
  out_status->actual_colour_temp_k = m_actualColourTempK.load();
  out_status->actual_red_gain = m_actualRedGain.load();
  out_status->actual_blue_gain = m_actualBlueGain.load();
  out_status->metering_mode =
      static_cast<CameraMeteringMode>(m_meteringMode.load());
}

void CameraStream::applyStartExposureControls(
    libcamera::ControlList &ctrls) const {
  const float ev = m_evCompensation.load();
  const int iso = m_isoSetting.load();
  const int32_t shutterUs = m_shutterUsSetting.load();

  // Ensure full sensor Field of View (ScalerCrop) matching the still capture
  // aspect ratio so preview and captured photos cover the exact same scene area
  if (m_camera) {
    if (const auto maxCropOpt = m_camera->properties().get(
            libcamera::properties::ScalerCropMaximum)) {
      libcamera::Rectangle crop = *maxCropOpt;
      if (crop.width > 0 && crop.height > 0 && m_stillWidth > 0 &&
          m_stillHeight > 0) {
        const double stillAspect = static_cast<double>(m_stillWidth) /
                                   static_cast<double>(m_stillHeight);
        const double sensorAspect =
            static_cast<double>(crop.width) / static_cast<double>(crop.height);
        if (sensorAspect > stillAspect) {
          unsigned int newW =
              static_cast<unsigned int>(crop.height * stillAspect + 0.5) & ~1u;
          if (newW > 0 && newW < crop.width) {
            crop.x += static_cast<int>((crop.width - newW) / 2);
            crop.width = newW;
          }
        } else if (stillAspect > sensorAspect) {
          unsigned int newH =
              static_cast<unsigned int>(crop.width / stillAspect + 0.5) & ~1u;
          if (newH > 0 && newH < crop.height) {
            crop.y += static_cast<int>((crop.height - newH) / 2);
            crop.height = newH;
          }
        }
      }
      if (m_camera->controls().find(&libcamera::controls::ScalerCrop) !=
          m_camera->controls().end()) {
        ctrls.set(libcamera::controls::ScalerCrop, crop);
      }
    }
  }

  // Always apply EV compensation on camera start
  ctrls.set(libcamera::controls::ExposureValue, ev);

  // Only set manual AnalogueGain if ISO is manually specified; otherwise let
  // default Auto Gain run without conflicting control overrides
  if (iso > 0) {
    float gain = static_cast<float>(iso) / 100.0f;
    ctrls.set(libcamera::controls::AnalogueGain, gain);
  }

  // Only set manual ExposureTime if shutter speed is manually specified
  if (shutterUs > 0) {
    ctrls.set(libcamera::controls::ExposureTime, shutterUs);
    int64_t minFrameDur =
        shutterUs > 33333 ? static_cast<int64_t>(shutterUs) : 100;
    int64_t maxFrameDur =
        shutterUs > 33333 ? static_cast<int64_t>(shutterUs) : 33333;
    std::int64_t frameDurationLimits[2] = {minFrameDur, maxFrameDur};
    ctrls.set(libcamera::controls::FrameDurationLimits, frameDurationLimits);
  }

  // Apply White Balance and AE Metering Mode on camera start
  applyWhiteBalanceToControlList(ctrls);
  applyMeteringToControlList(ctrls);
}

void CameraStream::applyPendingExposureControls(libcamera::ControlList &ctrls) {
  // 1. Apply EV compensation only when EV was modified
  int evRem = m_evPendingCount.load();
  while (evRem > 0 &&
         !m_evPendingCount.compare_exchange_weak(evRem, evRem - 1)) {
  }
  if (evRem > 0) {
    ctrls.set(libcamera::controls::ExposureValue, m_evCompensation.load());
  }

  // 2. Apply ISO (AnalogueGain) only when ISO was modified
  int isoRem = m_isoPendingCount.load();
  while (isoRem > 0 &&
         !m_isoPendingCount.compare_exchange_weak(isoRem, isoRem - 1)) {
  }
  if (isoRem > 0) {
    const int iso = m_isoSetting.load();
    if (iso > 0) {
      float gain = static_cast<float>(iso) / 100.0f;
      ctrls.set(libcamera::controls::AnalogueGain, gain);
    } else {
      ctrls.set(libcamera::controls::AnalogueGain, 0.0f);
    }
  }

  // 3. Apply Shutter Speed (ExposureTime & FrameDurationLimits) only when
  // shutter speed was modified
  int shutterRem = m_shutterPendingCount.load();
  while (shutterRem > 0 && !m_shutterPendingCount.compare_exchange_weak(
                               shutterRem, shutterRem - 1)) {
  }
  if (shutterRem > 0) {
    const int32_t shutterUs = m_shutterUsSetting.load();
    if (shutterUs > 0) {
      ctrls.set(libcamera::controls::ExposureTime, shutterUs);
      int64_t minFrameDur =
          shutterUs > 33333 ? static_cast<int64_t>(shutterUs) : 100;
      int64_t maxFrameDur =
          shutterUs > 33333 ? static_cast<int64_t>(shutterUs) : 33333;
      std::int64_t frameDurationLimits[2] = {minFrameDur, maxFrameDur};
      ctrls.set(libcamera::controls::FrameDurationLimits, frameDurationLimits);
    } else {
      ctrls.set(libcamera::controls::ExposureTime, 0);
      std::int64_t defaultFrameLimits[2] = {100, 33333};
      ctrls.set(libcamera::controls::FrameDurationLimits, defaultFrameLimits);
    }
  }

  // 4. Apply White Balance / AWB Lock when modified
  int wbRem = m_wbPendingCount.load();
  while (wbRem > 0 &&
         !m_wbPendingCount.compare_exchange_weak(wbRem, wbRem - 1)) {
  }
  if (wbRem > 0) {
    applyWhiteBalanceToControlList(ctrls);
  }

  // 5. Apply AE Metering Mode when modified
  int metRem = m_meteringPendingCount.load();
  while (metRem > 0 &&
         !m_meteringPendingCount.compare_exchange_weak(metRem, metRem - 1)) {
  }
  if (metRem > 0) {
    applyMeteringToControlList(ctrls);
  }
}

void CameraStream::unmapBuffers() {
  for (auto &[fd, mapped] : m_mappedBuffers) {
    if (mapped.address && mapped.address != MAP_FAILED) {
      munmap(mapped.address, mapped.length);
    }
  }
  m_mappedBuffers.clear();
}

void CameraStream::teardownStreams() {
  m_requests.clear();
  m_requestVfBuffers.clear();
  m_stillBuffers.clear();
  unmapBuffers();
  m_allocator.reset();
  m_stream = nullptr;
  m_stillStream = nullptr;
}

bool CameraStream::allocateAndMapStreams() {
  m_allocator = std::make_unique<libcamera::FrameBufferAllocator>(m_camera);

  // 1. Allocate Viewfinder buffers
  int ret = m_allocator->allocate(m_stream);
  if (ret < 0) {
    std::cerr << "[エラー] プレビュー用フレームバッファの確保に失敗しました。"
              << std::endl;
    teardownStreams();
    return false;
  }

  // 2. Allocate StillCapture buffers if dual-stream is active
  if (m_dualStreamActive && m_stillStream) {
    ret = m_allocator->allocate(m_stillStream);
    if (ret < 0) {
      std::cerr
          << "[警告] 2系統同時バッファ確保に失敗しました (CMAメモリ不足等)。"
          << " 単一ストリーム＋モード切替方式へフォールバックします。"
          << std::endl;
      teardownStreams();
      return false;
    }

    const auto &stillAllocated = m_allocator->buffers(m_stillStream);
    for (const auto &buffer : stillAllocated) {
      const auto &plane = buffer->planes()[0];
      void *address = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE,
                           MAP_SHARED, plane.fd.get(), 0);
      if (address == MAP_FAILED) {
        std::cerr << "[警告] 静止画フレームバッファの mmap に失敗しました。"
                  << std::endl;
        teardownStreams();
        return false;
      }
      m_mappedBuffers[plane.fd.get()] = {address, plane.length};
      m_stillBuffers.push_back(buffer.get());
    }
  }

  // 3. Memory-map Viewfinder buffers & create preview requests
  const auto &vfAllocated = m_allocator->buffers(m_stream);
  for (const auto &buffer : vfAllocated) {
    const auto &plane = buffer->planes()[0];
    void *address = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE,
                         MAP_SHARED, plane.fd.get(), 0);
    if (address == MAP_FAILED) {
      std::cerr << "[エラー] プレビューフレームバッファの mmap に失敗しました。"
                << std::endl;
      teardownStreams();
      return false;
    }
    m_mappedBuffers[plane.fd.get()] = {address, plane.length};

    std::unique_ptr<libcamera::Request> request = m_camera->createRequest();
    if (!request) {
      std::cerr << "[エラー] リクエスト作成に失敗しました。" << std::endl;
      teardownStreams();
      return false;
    }

    ret = request->addBuffer(m_stream, buffer.get());
    if (ret < 0) {
      std::cerr << "[エラー] バッファのリクエスト登録に失敗しました。"
                << std::endl;
      teardownStreams();
      return false;
    }

    m_requestVfBuffers[request.get()] = buffer.get();
    m_requests.push_back(std::move(request));
  }

  return true;
}

bool CameraStream::configureDualOrSingleStream() {
  // Step 1: Probe native maximum StillCapture resolution if not already known
  int targetStillW = m_requestedStillWidth;
  int targetStillH = m_requestedStillHeight;

  if (targetStillW <= 0 || targetStillH <= 0) {
    auto probeConfig =
        m_camera->generateConfiguration({libcamera::StreamRole::StillCapture});
    if (probeConfig && !probeConfig->empty()) {
      targetStillW = probeConfig->at(0).size.width;
      targetStillH = probeConfig->at(0).size.height;
    } else {
      targetStillW = m_requestedVfWidth > 0 ? m_requestedVfWidth : m_width;
      targetStillH = m_requestedVfHeight > 0 ? m_requestedVfHeight : m_height;
    }
  }

  // Fit Viewfinder resolution inside the requested preview bounding box
  // (m_requestedVfWidth x m_requestedVfHeight) while matching the exact aspect
  // ratio of StillCapture so ISP never crops the preview frame.
  int maxVfW = m_requestedVfWidth > 0 ? m_requestedVfWidth : m_width;
  int maxVfH = m_requestedVfHeight > 0 ? m_requestedVfHeight : m_height;
  int targetVfW = maxVfW;
  int targetVfH = maxVfH;
  if (targetStillW > 0 && targetStillH > 0 && maxVfW > 0 && maxVfH > 0) {
    const double stillAspect =
        static_cast<double>(targetStillW) / static_cast<double>(targetStillH);
    const double boxAspect =
        static_cast<double>(maxVfW) / static_cast<double>(maxVfH);
    if (boxAspect > stillAspect) {
      targetVfH = maxVfH;
      targetVfW = static_cast<int>(maxVfH * stillAspect + 0.5);
    } else {
      targetVfW = maxVfW;
      targetVfH = static_cast<int>(maxVfW / stillAspect + 0.5);
    }
    // Align to even pixel dimensions for ISP/DMA compatibility
    targetVfW = std::max(2, (targetVfW + 1) & ~1);
    targetVfH = std::max(2, (targetVfH + 1) & ~1);
    if (targetVfW > targetStillW) {
      targetVfW = targetStillW;
    }
    if (targetVfH > targetStillH) {
      targetVfH = targetStillH;
    }
  }

  // Step 2: Try Dual-Stream Configuration ({ StillCapture, Viewfinder })
  // Note: On Raspberry Pi ISP (vc4/pisp), the higher-resolution stream must be
  // Output 0.
  bool canTryDual =
      (targetStillW >= targetVfW && targetStillH >= targetVfH) &&
      (targetStillW > targetVfW || targetStillH > targetVfH);

  if (canTryDual) {
    auto dualConfig =
        m_camera->generateConfiguration({libcamera::StreamRole::StillCapture,
                                         libcamera::StreamRole::Viewfinder});

    if (dualConfig && dualConfig->size() >= 2) {
      libcamera::StreamConfiguration &stillCfg = dualConfig->at(0);
      stillCfg.size.width = targetStillW;
      stillCfg.size.height = targetStillH;
      stillCfg.pixelFormat = libcamera::formats::BGR888;
      stillCfg.bufferCount =
          1; // Minimize CMA footprint for on-demand still capture

      libcamera::StreamConfiguration &vfCfg = dualConfig->at(1);
      vfCfg.size.width = targetVfW;
      vfCfg.size.height = targetVfH;
      vfCfg.pixelFormat = libcamera::formats::BGR888;
      vfCfg.bufferCount = 4;

      auto status = dualConfig->validate();

      // Fallback to RGB888 if BGR888 was adjusted to a non-RGB format
      auto isRgbFormat = [](const libcamera::PixelFormat &fmt) {
        return fmt == libcamera::formats::BGR888 ||
               fmt == libcamera::formats::RGB888;
      };

      if (status == libcamera::CameraConfiguration::Invalid ||
          !isRgbFormat(stillCfg.pixelFormat) ||
          !isRgbFormat(vfCfg.pixelFormat)) {
        if (!isRgbFormat(stillCfg.pixelFormat)) {
          stillCfg.pixelFormat = libcamera::formats::RGB888;
        }
        if (!isRgbFormat(vfCfg.pixelFormat)) {
          vfCfg.pixelFormat = libcamera::formats::RGB888;
        }
        status = dualConfig->validate();
      }

      if (status != libcamera::CameraConfiguration::Invalid &&
          isRgbFormat(stillCfg.pixelFormat) && isRgbFormat(vfCfg.pixelFormat)) {
        if (m_camera->configure(dualConfig.get()) == 0) {
          m_config = std::move(dualConfig);
          libcamera::StreamConfiguration &activeStillCfg = m_config->at(0);
          libcamera::StreamConfiguration &activeVfCfg = m_config->at(1);

          m_stillStream = activeStillCfg.stream();
          m_stillWidth = activeStillCfg.size.width;
          m_stillHeight = activeStillCfg.size.height;
          m_stillStride = activeStillCfg.stride;
          // In DRM/libcamera conventions, formats::RGB888 lays out bytes in
          // memory as [B, G, R] (which corresponds to SDL_PIXELFORMAT_BGR24),
          // whereas formats::BGR888 lays out bytes as [R, G, B]
          // (SDL_PIXELFORMAT_RGB24).
          m_stillMemoryOrderIsBGR =
              (activeStillCfg.pixelFormat == libcamera::formats::RGB888);

          m_stream = activeVfCfg.stream();
          m_width = activeVfCfg.size.width;
          m_height = activeVfCfg.size.height;
          m_stride = activeVfCfg.stride;
          m_memoryOrderIsBGR =
              (activeVfCfg.pixelFormat == libcamera::formats::RGB888);
          m_dualStreamActive = true;

          if (allocateAndMapStreams()) {
            std::cout
                << "[-] マルチストリーム構成 (2系統同時) を有効化しました:"
                << std::endl;
            std::cout << "    * Viewfinder : " << m_width << "x" << m_height
                      << " (" << activeVfCfg.pixelFormat.toString()
                      << ", Stride: " << m_stride << ")" << std::endl;
            std::cout << "    * Still      : " << m_stillWidth << "x"
                      << m_stillHeight << " ("
                      << activeStillCfg.pixelFormat.toString()
                      << ", Stride: " << m_stillStride << ")" << std::endl;
            return true;
          }
          // If buffer allocation failed, reset and fall through to single
          // stream
          m_dualStreamActive = false;
        }
      }
    }
  }

  // Step 3: Single-Stream Viewfinder configuration + Mode-Switch Still Capture
  m_dualStreamActive = false;
  m_config =
      m_camera->generateConfiguration({libcamera::StreamRole::Viewfinder});
  if (!m_config || m_config->empty()) {
    std::cerr << "[エラー] カメラストリーム構成の生成に失敗しました。"
              << std::endl;
    return false;
  }

  libcamera::StreamConfiguration &streamConfig = m_config->at(0);
  streamConfig.size.width = targetVfW;
  streamConfig.size.height = targetVfH;
  streamConfig.pixelFormat = libcamera::formats::BGR888;
  streamConfig.bufferCount = 4;

  libcamera::CameraConfiguration::Status status = m_config->validate();
  if (status == libcamera::CameraConfiguration::Invalid ||
      (streamConfig.pixelFormat != libcamera::formats::BGR888 &&
       streamConfig.pixelFormat != libcamera::formats::RGB888)) {
    streamConfig.pixelFormat = libcamera::formats::RGB888;
    status = m_config->validate();
    if (status == libcamera::CameraConfiguration::Invalid) {
      std::cerr << "[エラー] 有効なカメラストリーム構成が見つかりませんでした。"
                << std::endl;
      return false;
    }
  }

  // DRM/libcamera formats::RGB888 stores bytes in memory as [B, G, R]
  // (SDL_PIXELFORMAT_BGR24); formats::BGR888 stores bytes as [R, G, B]
  // (SDL_PIXELFORMAT_RGB24).
  m_memoryOrderIsBGR =
      (streamConfig.pixelFormat == libcamera::formats::RGB888);
  m_width = streamConfig.size.width;
  m_height = streamConfig.size.height;
  m_stride = streamConfig.stride;

  m_stillWidth = targetStillW;
  m_stillHeight = targetStillH;

  if (m_camera->configure(m_config.get()) < 0) {
    std::cerr << "[エラー] カメラの構成 (configure) に失敗しました。"
              << std::endl;
    return false;
  }

  m_stream = streamConfig.stream();
  m_stillStream = nullptr;

  if (!allocateAndMapStreams()) {
    return false;
  }

  std::cout << "[-] ストリーム構成 (プレビュー + 撮影時高解像度切替モード):"
            << std::endl;
  std::cout << "    * Viewfinder : " << m_width << "x" << m_height << " ("
            << streamConfig.pixelFormat.toString() << ", Stride: " << m_stride
            << ")" << std::endl;
  std::cout << "    * Still      : " << m_stillWidth << "x" << m_stillHeight
            << " (目標解像度)" << std::endl;

  return true;
}

void CameraStream::requestCompleted(libcamera::Request *request) {
  if (request->status() == libcamera::Request::RequestCancelled || !m_running) {
    return;
  }

  // Update AF state, lens position & exposure metadata if present
  const libcamera::ControlList &metadata = request->metadata();
  if (const auto afState = metadata.get(libcamera::controls::AfState)) {
    m_afState = *afState;
  }
  if (const auto lensPos = metadata.get(libcamera::controls::LensPosition)) {
    m_lastLensPosition = *lensPos;
    m_hasLensPosition = true;
  }
  if (const auto expTime = metadata.get(libcamera::controls::ExposureTime)) {
    m_actualExposureUs = *expTime;
  }
  if (const auto gain = metadata.get(libcamera::controls::AnalogueGain)) {
    m_actualGain = *gain;
  }
  if (const auto colourTemp =
          metadata.get(libcamera::controls::ColourTemperature)) {
    m_actualColourTempK = *colourTemp;
  }
  if (const auto colourGains = metadata.get(libcamera::controls::ColourGains)) {
    if (colourGains->size() >= 2) {
      m_actualRedGain = (*colourGains)[0];
      m_actualBlueGain = (*colourGains)[1];
    }
  }

  const auto &buffers = request->buffers();

  // 1. Extract Viewfinder (preview) image data
  auto vfIt = buffers.find(m_stream);
  if (vfIt != buffers.end()) {
    libcamera::FrameBuffer *buffer = vfIt->second;
    const auto &plane = buffer->planes()[0];
    auto mapIt = m_mappedBuffers.find(plane.fd.get());
    if (mapIt != m_mappedBuffers.end()) {
      const void *dataPtr = mapIt->second.address;
      size_t bytesUsed = buffer->metadata().planes()[0].bytesused;
      if (bytesUsed == 0) {
        bytesUsed = mapIt->second.length;
      }

      {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_latestFrame.size() < bytesUsed) {
          m_latestFrame.resize(bytesUsed);
        }
        std::memcpy(m_latestFrame.data(), dataPtr, bytesUsed);
        m_frameSequence++;
        m_hasNewFrame = true;
      }
    }
  }

  // 2. Extract high-resolution StillCapture image data if present in this
  // request
  bool hadStillBuffer = false;
  if (m_dualStreamActive && m_stillStream) {
    auto stillIt = buffers.find(m_stillStream);
    if (stillIt != buffers.end()) {
      hadStillBuffer = true;
      libcamera::FrameBuffer *stillBuf = stillIt->second;
      const auto &plane = stillBuf->planes()[0];
      auto mapIt = m_mappedBuffers.find(plane.fd.get());
      if (mapIt != m_mappedBuffers.end()) {
        const void *dataPtr = mapIt->second.address;
        size_t bytesUsed = stillBuf->metadata().planes()[0].bytesused;
        if (bytesUsed == 0) {
          bytesUsed = mapIt->second.length;
        }

        {
          std::lock_guard<std::mutex> lock(m_stillMutex);
          m_stillFrame.resize(bytesUsed);
          std::memcpy(m_stillFrame.data(), dataPtr, bytesUsed);
          m_capturedStillWidth = m_stillWidth;
          m_capturedStillHeight = m_stillHeight;
          m_capturedStillStride = m_stillStride;
          m_capturedStillMemoryOrderIsBGR = m_stillMemoryOrderIsBGR;
          populateExposureStatus(&m_capturedStillExpStatus);
          m_stillFrameReady = true;
        }
        m_stillCv.notify_one();
      }
    }
  }

  // 3. Re-prepare request for continuous streaming
  if (hadStillBuffer) {
    // Clear all attached buffers so the next cycle returns to Viewfinder-only
    request->reuse();
    auto reqVfIt = m_requestVfBuffers.find(request);
    if (reqVfIt != m_requestVfBuffers.end()) {
      request->addBuffer(m_stream, reqVfIt->second);
    }
  } else {
    request->reuse(libcamera::Request::ReuseBuffers);
  }

  // Attach on-demand StillCapture buffer if a still capture was requested
  if (m_dualStreamActive && m_stillStream && !m_stillBuffers.empty()) {
    if (m_stillCapturePending.exchange(false)) {
      request->addBuffer(m_stillStream, m_stillBuffers[0]);
    }
  }

  // Apply autofocus controls if requested
  if (m_afSupported) {
    if (m_afTriggerPending.exchange(false)) {
      request->controls().set(libcamera::controls::AfMode,
                              libcamera::controls::AfModeAuto);
      request->controls().set(libcamera::controls::AfTrigger,
                              libcamera::controls::AfTriggerStart);
    } else if (m_afContinuousPending.exchange(false)) {
      request->controls().set(libcamera::controls::AfMode,
                              libcamera::controls::AfModeContinuous);
    }
  }

  // Apply only the modified exposure controls across the request ring
  applyPendingExposureControls(request->controls());

  m_camera->queueRequest(request);
}

bool CameraStream::captureStillByModeSwitch(std::vector<uint8_t> &outBuffer,
                                            int &outWidth, int &outHeight,
                                            int &outStride,
                                            bool &outMemoryOrderIsBGR) {
  if (!m_camera)
    return false;

  // 1. Stop current Viewfinder streaming
  m_running = false;
  m_camera->stop();
  m_camera->requestCompleted.disconnect(this, &CameraStream::requestCompleted);
  teardownStreams();

  // 2. Generate and configure StillCapture stream
  bool captured = false;
  auto stillConfig =
      m_camera->generateConfiguration({libcamera::StreamRole::StillCapture});
  if (stillConfig && !stillConfig->empty()) {
    libcamera::StreamConfiguration &sc = stillConfig->at(0);
    if (m_requestedStillWidth > 0 && m_requestedStillHeight > 0) {
      sc.size.width = m_requestedStillWidth;
      sc.size.height = m_requestedStillHeight;
    }
    sc.pixelFormat = libcamera::formats::BGR888;
    sc.bufferCount = 1;

    auto status = stillConfig->validate();
    if (status == libcamera::CameraConfiguration::Invalid ||
        (sc.pixelFormat != libcamera::formats::BGR888 &&
         sc.pixelFormat != libcamera::formats::RGB888)) {
      sc.pixelFormat = libcamera::formats::RGB888;
      status = stillConfig->validate();
    }

    if (status != libcamera::CameraConfiguration::Invalid &&
        m_camera->configure(stillConfig.get()) == 0) {
      m_stillWidth = sc.size.width;
      m_stillHeight = sc.size.height;
      libcamera::Stream *stillStream = sc.stream();
      auto allocator =
          std::make_unique<libcamera::FrameBufferAllocator>(m_camera);
      if (allocator->allocate(stillStream) >= 0) {
        const auto &buffers = allocator->buffers(stillStream);
        if (!buffers.empty()) {
          const auto &buf = buffers[0];
          const auto &plane = buf->planes()[0];
          void *addr = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, plane.fd.get(), 0);
          if (addr != MAP_FAILED) {
            auto req = m_camera->createRequest();
            if (req && req->addBuffer(stillStream, buf.get()) == 0) {
              std::mutex capMtx;
              std::condition_variable capCv;
              bool done = false;

              struct StillCallbackHelper {
                CameraStream *self;
                std::mutex *mtx;
                std::condition_variable *cv;
                bool *doneFlag;
                void onComplete(libcamera::Request *r) {
                  if (r->status() == libcamera::Request::RequestCancelled)
                    return;
                  const libcamera::ControlList &meta = r->metadata();
                  if (const auto expTime =
                          meta.get(libcamera::controls::ExposureTime)) {
                    self->m_actualExposureUs = *expTime;
                  }
                  if (const auto gain =
                          meta.get(libcamera::controls::AnalogueGain)) {
                    self->m_actualGain = *gain;
                  }
                  if (const auto colourTemp =
                          meta.get(libcamera::controls::ColourTemperature)) {
                    self->m_actualColourTempK = *colourTemp;
                  }
                  if (const auto colourGains =
                          meta.get(libcamera::controls::ColourGains)) {
                    if (colourGains->size() >= 2) {
                      self->m_actualRedGain = (*colourGains)[0];
                      self->m_actualBlueGain = (*colourGains)[1];
                    }
                  }
                  {
                    std::lock_guard<std::mutex> lk(*mtx);
                    *doneFlag = true;
                  }
                  cv->notify_one();
                }
              } helper{this, &capMtx, &capCv, &done};

              m_camera->requestCompleted.connect(
                  &helper, &StillCallbackHelper::onComplete);

              libcamera::ControlList startControls(m_camera->controls());
              if (m_afSupported && m_hasLensPosition.load()) {
                startControls.set(libcamera::controls::AfMode,
                                  libcamera::controls::AfModeManual);
                startControls.set(libcamera::controls::LensPosition,
                                  m_lastLensPosition.load());
              }
              applyStartExposureControls(startControls);

              if (m_camera->start(&startControls) == 0) {
                m_camera->queueRequest(req.get());
                {
                  int64_t waitMs = std::max<int64_t>(
                      3000, m_shutterUsSetting.load() / 1000 + 2500);
                  std::unique_lock<std::mutex> lk(capMtx);
                  capCv.wait_for(lk, std::chrono::milliseconds(waitMs),
                                 [&] { return done; });
                }
                if (done) {
                  size_t bytesUsed = buf->metadata().planes()[0].bytesused;
                  if (bytesUsed == 0) {
                    bytesUsed = plane.length;
                  }
                  outBuffer.resize(bytesUsed);
                  std::memcpy(outBuffer.data(), addr, bytesUsed);
                  outWidth = sc.size.width;
                  outHeight = sc.size.height;
                  outStride = sc.stride;
                  // DRM/libcamera formats::RGB888 stores bytes in memory as
                  // [B, G, R] (SDL_PIXELFORMAT_BGR24).
                  outMemoryOrderIsBGR =
                      (sc.pixelFormat == libcamera::formats::RGB888);
                  m_stillWidth = outWidth;
                  m_stillHeight = outHeight;
                  captured = true;
                }
                m_camera->stop();
              }
              m_camera->requestCompleted.disconnect(
                  &helper, &StillCallbackHelper::onComplete);
            }
            munmap(addr, plane.length);
          }
        }
      }
    }
  }

  // 3. Restore Viewfinder stream and restart continuous preview
  if (configureDualOrSingleStream()) {
    m_camera->requestCompleted.connect(this, &CameraStream::requestCompleted);
    camera_stream_start(this);
  } else {
    std::cerr << "[エラー] プレビューストリームへの復帰に失敗しました。"
              << std::endl;
  }

  return captured;
}

// -------------------------------------------------------------
// extern "C" wrapper functions callable from pure C code
// -------------------------------------------------------------
extern "C" {

CameraStream *camera_stream_create(void) {
  return new (std::nothrow) CameraStream();
}

void camera_stream_destroy(CameraStream *cam) {
  if (!cam)
    return;
  camera_stream_stop(cam);
  delete cam;
}

void camera_stream_set_still_size(CameraStream *cam, int still_width,
                                  int still_height) {
  if (!cam)
    return;
  cam->m_requestedStillWidth = still_width > 0 ? still_width : 0;
  cam->m_requestedStillHeight = still_height > 0 ? still_height : 0;
}

bool camera_stream_init(CameraStream *cam, int width, int height) {
  if (!cam)
    return false;

  cam->m_requestedVfWidth = width > 0 ? width : 1024;
  cam->m_requestedVfHeight = height > 0 ? height : 600;
  cam->m_width = cam->m_requestedVfWidth;
  cam->m_height = cam->m_requestedVfHeight;

  // 1. Initialize CameraManager
  cam->m_cameraManager = std::make_unique<libcamera::CameraManager>();
  int ret = cam->m_cameraManager->start();
  if (ret) {
    std::cerr << "[エラー] CameraManager の起動に失敗しました (ret: " << ret
              << ")" << std::endl;
    return false;
  }

  auto cameras = cam->m_cameraManager->cameras();
  if (cameras.empty()) {
    std::cerr << "[エラー] 接続されているカメラが見つかりません。" << std::endl;
    cam->m_cameraManager->stop();
    cam->m_cameraManager.reset();
    return false;
  }

  // Acquire the first camera
  cam->m_camera = cameras[0];
  ret = cam->m_camera->acquire();
  if (ret) {
    std::cerr << "[エラー] カメラの取得 (acquire) に失敗しました: "
              << cam->m_camera->id() << std::endl;
    cam->m_camera.reset();
    cam->m_cameraManager->stop();
    cam->m_cameraManager.reset();
    return false;
  }

  std::cout << "[-] カメラを検出しました: " << cam->m_camera->id() << std::endl;
  if (const auto modelProp =
          cam->m_camera->properties().get(libcamera::properties::Model)) {
    cam->m_cameraModel = std::string("Raspberry Pi ") + std::string(*modelProp);
    std::cout << "[-] センサーモデル: " << cam->m_cameraModel << std::endl;
  } else {
    cam->m_cameraModel = cam->m_camera->id();
  }

  // 2. Configure Dual-Stream (StillCapture + Viewfinder) or Single-Stream with
  // Mode-Switch
  if (!cam->configureDualOrSingleStream()) {
    cam->m_camera->release();
    cam->m_camera.reset();
    cam->m_cameraManager->stop();
    cam->m_cameraManager.reset();
    return false;
  }

  // 3. Check Autofocus support (Camera Module 3, etc.)
  const auto &controls = cam->m_camera->controls();
  if (controls.find(libcamera::controls::AfMode.id()) != controls.end()) {
    cam->m_afSupported = true;
    std::cout << "[-] オートフォーカス (Continuous AF) に対応しています。"
              << std::endl;
  } else {
    cam->m_afSupported = false;
    std::cout
        << "[-] オートフォーカス非対応、または固定フォーカスとして動作します。"
        << std::endl;
  }

  // Connect requestCompleted signal
  cam->m_camera->requestCompleted.connect(cam, &CameraStream::requestCompleted);

  // Prepare internal double-buffering vector for preview frames
  size_t expectedSize = static_cast<size_t>(cam->m_stride) * cam->m_height;
  if (expectedSize == 0) {
    expectedSize = static_cast<size_t>(cam->m_width) * cam->m_height * 3;
  }
  cam->m_latestFrame.resize(expectedSize, 0);

  return true;
}

bool camera_stream_start(CameraStream *cam) {
  if (!cam || !cam->m_camera)
    return false;

  // Controls applied on camera start
  libcamera::ControlList controlsList(cam->m_camera->controls());
  if (cam->m_afSupported) {
    controlsList.set(libcamera::controls::AfMode,
                     libcamera::controls::AfModeContinuous);
  }
  cam->applyStartExposureControls(controlsList);

  int ret = cam->m_camera->start(&controlsList);
  if (ret < 0) {
    std::cerr << "[エラー] カメラの開始 (start) に失敗しました。" << std::endl;
    return false;
  }

  cam->m_running = true;

  // Queue initial requests
  for (auto &req : cam->m_requests) {
    cam->m_camera->queueRequest(req.get());
  }

  std::cout << "[-] カメラストリームを開始しました。" << std::endl;
  return true;
}

void camera_stream_stop(CameraStream *cam) {
  if (!cam)
    return;

  cam->m_running = false;

  if (cam->m_camera) {
    cam->m_camera->stop();
    cam->m_camera->requestCompleted.disconnect(cam,
                                               &CameraStream::requestCompleted);
  }

  cam->teardownStreams();

  if (cam->m_camera) {
    cam->m_camera->release();
    cam->m_camera.reset();
  }

  if (cam->m_cameraManager) {
    cam->m_cameraManager->stop();
    cam->m_cameraManager.reset();
  }
}

bool camera_stream_is_af_supported(const CameraStream *cam) {
  return cam ? cam->m_afSupported : false;
}

const char *camera_stream_get_model_name(const CameraStream *cam) {
  if (!cam) {
    return "RubusCam";
  }
  return cam->m_cameraModel.c_str();
}

int camera_stream_get_width(const CameraStream *cam) {
  return cam ? cam->m_width : 0;
}

int camera_stream_get_height(const CameraStream *cam) {
  return cam ? cam->m_height : 0;
}

int camera_stream_get_stride(const CameraStream *cam) {
  return cam ? cam->m_stride : 0;
}

size_t camera_stream_get_frame_size(const CameraStream *cam) {
  if (!cam)
    return 0;
  size_t stride = cam->m_stride ? static_cast<size_t>(cam->m_stride)
                                : (static_cast<size_t>(cam->m_width) * 3);
  return stride * cam->m_height;
}

bool camera_stream_is_bgr(const CameraStream *cam) {
  return cam ? cam->m_memoryOrderIsBGR : false;
}

int camera_stream_get_still_width(const CameraStream *cam) {
  return cam ? cam->m_stillWidth : 0;
}

int camera_stream_get_still_height(const CameraStream *cam) {
  return cam ? cam->m_stillHeight : 0;
}

bool camera_stream_get_latest_frame(CameraStream *cam, uint8_t *out_buffer,
                                    size_t buffer_size,
                                    uint64_t *out_sequence) {
  if (!cam || !out_buffer)
    return false;

  std::lock_guard<std::mutex> lock(cam->m_frameMutex);
  if (!cam->m_hasNewFrame && out_sequence &&
      *out_sequence == cam->m_frameSequence) {
    return false;
  }

  size_t copySize = std::min(buffer_size, cam->m_latestFrame.size());
  std::memcpy(out_buffer, cam->m_latestFrame.data(), copySize);

  if (out_sequence) {
    *out_sequence = cam->m_frameSequence;
  }
  cam->m_hasNewFrame = false;
  return true;
}

bool camera_stream_set_ev(CameraStream *cam, float ev) {
  if (!cam)
    return false;
  float clamped = std::clamp(ev, -3.0f, 3.0f);
  cam->m_evCompensation = clamped;
  cam->m_evPendingCount = 4;
  return true;
}

bool camera_stream_set_iso(CameraStream *cam, int iso) {
  if (!cam)
    return false;
  int clamped = iso > 0 ? std::clamp(iso, 100, 3200) : 0;
  if (cam->m_isoSetting.exchange(clamped) != clamped) {
    cam->m_isoPendingCount = 4;
  }
  return true;
}

bool camera_stream_set_shutter_speed(CameraStream *cam,
                                     int32_t exposure_time_us) {
  if (!cam)
    return false;
  int32_t clamped = exposure_time_us > 0 ? exposure_time_us : 0;
  if (cam->m_shutterUsSetting.exchange(clamped) != clamped) {
    cam->m_shutterPendingCount = 4;
  }
  return true;
}

bool camera_stream_set_wb_mode(CameraStream *cam, CameraWbMode mode) {
  if (!cam)
    return false;
  int clamped = std::clamp(static_cast<int>(mode),
                           static_cast<int>(CAMERA_WB_MODE_AUTO),
                           static_cast<int>(CAMERA_WB_MODE_KELVIN));
  cam->m_awbLocked = false;
  cam->m_wbMode = clamped;
  cam->m_wbPendingCount = 4;
  return true;
}

bool camera_stream_set_wb_kelvin(CameraStream *cam, int kelvin) {
  if (!cam)
    return false;
  int clamped = std::clamp(kelvin, 2500, 10000);
  cam->m_wbKelvin = clamped;
  cam->m_awbLocked = false;
  cam->m_wbMode = static_cast<int>(CAMERA_WB_MODE_KELVIN);
  cam->m_wbPendingCount = 4;
  return true;
}

bool camera_stream_set_awb_lock(CameraStream *cam, bool locked) {
  if (!cam)
    return false;
  if (locked) {
    float curR = cam->m_actualRedGain.load();
    float curB = cam->m_actualBlueGain.load();
    if (curR <= 0.1f || curB <= 0.1f) {
      int curK = cam->m_actualColourTempK.load();
      if (curK < 2500 || curK > 10000) {
        curK = cam->m_wbKelvin.load();
      }
      kelvinToColourGains(curK, curR, curB);
    }
    cam->m_lockedRedGain = curR;
    cam->m_lockedBlueGain = curB;
  }
  cam->m_awbLocked = locked;
  cam->m_wbPendingCount = 4;
  return true;
}

bool camera_stream_set_metering_mode(CameraStream *cam,
                                     CameraMeteringMode mode) {
  if (!cam)
    return false;
  int clamped = std::clamp(static_cast<int>(mode),
                           static_cast<int>(CAMERA_METERING_MATRIX),
                           static_cast<int>(CAMERA_METERING_SPOT));
  cam->m_meteringMode = clamped;
  cam->m_meteringPendingCount = 4;
  return true;
}

void camera_stream_get_exposure_status(const CameraStream *cam,
                                       CameraExposureStatus *out_status) {
  if (!out_status)
    return;
  if (!cam) {
    std::memset(out_status, 0, sizeof(*out_status));
    return;
  }
  cam->populateExposureStatus(out_status);
}

bool camera_stream_trigger_af(CameraStream *cam) {
  if (!cam || !cam->m_afSupported || !cam->m_running) {
    return false;
  }

  std::cout << "[-] オートフォーカスを調整中..." << std::endl;

  // Reset state & flag trigger
  cam->m_afState = 0;
  cam->m_afTriggerPending = true;

  // Wait up to 2.5 seconds for AF cycle to finish (AfStateFocused or
  // AfStateFailed)
  auto startTime = std::chrono::steady_clock::now();
  bool focused = false;

  while (std::chrono::steady_clock::now() - startTime < 2500ms) {
    int state = cam->m_afState.load();
    if (state == libcamera::controls::AfStateFocused) {
      focused = true;
      break;
    } else if (state == libcamera::controls::AfStateFailed) {
      break;
    }
    std::this_thread::sleep_for(20ms);
  }

  if (focused) {
    std::cout << "[★成功] フォーカスが合いました。" << std::endl;
  } else {
    std::cout << "[警告] フォーカス調整が完了しませんでした。" << std::endl;
  }

  // Restore Continuous AF mode on subsequent requests
  if (cam->m_afSupported) {
    cam->m_afContinuousPending = true;
  }

  return focused;
}

bool camera_stream_capture_still(CameraStream *cam, const char *filename,
                                 CameraStillSaveCallback save_cb) {
  if (!cam || !filename || !save_cb)
    return false;

  std::cout << "[-] 高解像度静止画を撮影中 (" << cam->m_stillWidth << "x"
            << cam->m_stillHeight << ")..." << std::endl;

  std::vector<uint8_t> stillCopy;
  int width = 0;
  int height = 0;
  int stride = 0;
  bool isBGR = false;

  // Path A: Dual-stream on-demand still buffer capture
  if (cam->m_dualStreamActive && cam->m_stillStream &&
      !cam->m_stillBuffers.empty()) {
    {
      std::lock_guard<std::mutex> lock(cam->m_stillMutex);
      cam->m_stillFrameReady = false;
    }
    cam->m_stillCapturePending = true;

    int64_t waitMs =
        std::max<int64_t>(2000, cam->m_shutterUsSetting.load() / 1000 + 2000);
    std::unique_lock<std::mutex> lock(cam->m_stillMutex);
    bool ready =
        cam->m_stillCv.wait_for(lock, std::chrono::milliseconds(waitMs),
                                [&] { return cam->m_stillFrameReady; });
    if (ready && !cam->m_stillFrame.empty()) {
      stillCopy = std::move(cam->m_stillFrame);
      cam->m_stillFrameReady = false;
      width = cam->m_stillWidth;
      height = cam->m_stillHeight;
      stride = cam->m_stillStride;
      isBGR = cam->m_stillMemoryOrderIsBGR;
    } else {
      cam->m_stillCapturePending = false;
      std::cerr << "[警告] "
                   "マルチストリーム静止画取得がタイムアウトしました。モード切"
                   "替キャプチャを試行します。"
                << std::endl;
    }
  }

  // Path B: Mode-switch high-resolution still capture
  if (stillCopy.empty()) {
    if (!cam->captureStillByModeSwitch(stillCopy, width, height, stride,
                                       isBGR)) {
      std::cerr << "[警告] "
                   "高解像度モード切替撮影に失敗しました。プレビューフレームか"
                   "ら保存します。"
                << std::endl;
      std::lock_guard<std::mutex> lock(cam->m_frameMutex);
      stillCopy = cam->m_latestFrame;
      width = cam->m_width;
      height = cam->m_height;
      stride = cam->m_stride;
      isBGR = cam->m_memoryOrderIsBGR;
    }
  }

  if (stillCopy.empty()) {
    std::cerr << "[エラー] キャプチャ用フレームバッファが空です。" << std::endl;
    return false;
  }

  if (!save_cb(filename, stillCopy.data(), width, height, stride, isBGR)) {
    std::cerr << "[エラー] 画像ファイルの保存に失敗しました: " << filename
              << std::endl;
    return false;
  }

  std::cout << "[★成功] 保存しました: " << filename << " (" << width << "x"
            << height << ")" << std::endl;

  // Re-apply Continuous AF controls if supported
  if (cam->m_afSupported) {
    cam->m_afContinuousPending = true;
  }

  return true;
}

bool camera_stream_request_still(CameraStream *cam) {
  if (!cam || !cam->m_running) {
    return false;
  }

  bool expected = false;
  if (!cam->m_asyncStillInFlight.compare_exchange_strong(expected, true)) {
    return false;
  }

  std::cout << "[-] 非同期高解像度キャプチャを要求しました ("
            << cam->m_stillWidth << "x" << cam->m_stillHeight << ")..."
            << std::endl;

  // Path A: Dual-stream non-blocking still buffer request
  if (cam->m_dualStreamActive && cam->m_stillStream &&
      !cam->m_stillBuffers.empty()) {
    {
      std::lock_guard<std::mutex> lock(cam->m_stillMutex);
      cam->m_stillFrameReady = false;
      cam->m_stillFrame.clear();
    }
    cam->m_asyncStillStartTime = std::chrono::steady_clock::now();
    cam->m_stillCapturePending = true;
    return true;
  }

  // Path B: Single-stream mode-switch capture (encoding still happens in
  // background save queue)
  std::vector<uint8_t> stillCopy;
  int width = 0;
  int height = 0;
  int stride = 0;
  bool isBGR = false;
  if (!cam->captureStillByModeSwitch(stillCopy, width, height, stride, isBGR)) {
    std::lock_guard<std::mutex> lock(cam->m_frameMutex);
    stillCopy = cam->m_latestFrame;
    width = cam->m_width;
    height = cam->m_height;
    stride = cam->m_stride;
    isBGR = cam->m_memoryOrderIsBGR;
  }

  if (stillCopy.empty()) {
    cam->m_asyncStillInFlight = false;
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(cam->m_stillMutex);
    cam->m_stillFrame = std::move(stillCopy);
    cam->m_capturedStillWidth = width;
    cam->m_capturedStillHeight = height;
    cam->m_capturedStillStride = stride;
    cam->m_capturedStillMemoryOrderIsBGR = isBGR;
    camera_stream_get_exposure_status(cam, &cam->m_capturedStillExpStatus);
    cam->m_stillFrameReady = true;
  }
  return true;
}

bool camera_stream_is_still_pending(const CameraStream *cam) {
  return cam ? cam->m_asyncStillInFlight.load() : false;
}

bool camera_stream_poll_captured_still(CameraStream *cam,
                                       CameraCapturedStill *out_still) {
  if (!cam || !out_still || !cam->m_asyncStillInFlight.load()) {
    return false;
  }

  std::vector<uint8_t> localBuf;
  int width = 0;
  int height = 0;
  int stride = 0;
  bool isBGR = false;
  CameraExposureStatus expStatus{};
  bool ready = false;

  {
    std::lock_guard<std::mutex> lock(cam->m_stillMutex);
    if (cam->m_stillFrameReady && !cam->m_stillFrame.empty()) {
      localBuf = std::move(cam->m_stillFrame);
      cam->m_stillFrameReady = false;
      width = cam->m_capturedStillWidth;
      height = cam->m_capturedStillHeight;
      stride = cam->m_capturedStillStride;
      isBGR = cam->m_capturedStillMemoryOrderIsBGR;
      expStatus = cam->m_capturedStillExpStatus;
      ready = true;
    }
  }

  if (!ready) {
    // Check if dual-stream capture timed out; if so, fall back to mode-switch
    int64_t timeoutMs =
        std::max<int64_t>(2000, cam->m_shutterUsSetting.load() / 1000 + 2000);
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() -
                         cam->m_asyncStillStartTime)
                         .count();
    if (elapsedMs < timeoutMs) {
      return false;
    }

    cam->m_stillCapturePending = false;
    std::cerr << "[警告] 非同期マルチストリーム取得がタイムアウトしました。"
                 "モード切替キャプチャへフォールバックします。"
              << std::endl;
    if (!cam->captureStillByModeSwitch(localBuf, width, height, stride,
                                       isBGR)) {
      std::lock_guard<std::mutex> lock(cam->m_frameMutex);
      localBuf = cam->m_latestFrame;
      width = cam->m_width;
      height = cam->m_height;
      stride = cam->m_stride;
      isBGR = cam->m_memoryOrderIsBGR;
    }
    camera_stream_get_exposure_status(cam, &expStatus);
    if (localBuf.empty()) {
      cam->m_asyncStillInFlight = false;
      return false;
    }
  }

  uint8_t *heapBuf = static_cast<uint8_t *>(std::malloc(localBuf.size()));
  if (!heapBuf) {
    cam->m_asyncStillInFlight = false;
    return false;
  }
  std::memcpy(heapBuf, localBuf.data(), localBuf.size());

  out_still->data = heapBuf;
  out_still->size = localBuf.size();
  out_still->width = width;
  out_still->height = height;
  out_still->stride = stride;
  out_still->is_bgr = isBGR;
  out_still->exp_status = expStatus;

  cam->m_asyncStillInFlight = false;
  if (cam->m_afSupported) {
    cam->m_afContinuousPending = true;
  }
  return true;
}

} // extern "C"
