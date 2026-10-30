#ifndef CAMERA_STREAM_H
#define CAMERA_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque handle representing the camera stream.
 */
typedef struct CameraStream CameraStream;

/*
 * Create and destroy a camera stream instance.
 */
CameraStream *camera_stream_create(void);
void camera_stream_destroy(CameraStream *cam);

/*
 * Optional: Set target still capture resolution before camera_stream_init().
 * Pass (0, 0) to use the camera sensor's native maximum resolution (default).
 */
void camera_stream_set_still_size(CameraStream *cam, int still_width,
                                  int still_height);

/*
 * Initialize libcamera with requested preview resolution (e.g. 480x320 or
 * 1024x600) and high-resolution still capture configuration. Returns true on
 * success, false on failure.
 */
bool camera_stream_init(CameraStream *cam, int width, int height);

/*
 * Start streaming preview frames.
 * Returns true on success, false on failure.
 */
bool camera_stream_start(CameraStream *cam);

/*
 * Stop streaming preview frames and release camera resources.
 */
void camera_stream_stop(CameraStream *cam);

/*
 * Check if autofocus (Continuous AF / One-shot AF) is supported by the camera
 * module.
 */
bool camera_stream_is_af_supported(const CameraStream *cam);

/*
 * Return the detected camera model or sensor identifier string (e.g. "imx708").
 */
const char *camera_stream_get_model_name(const CameraStream *cam);

/*
 * Preview stream dimension & format queries.
 */
int camera_stream_get_width(const CameraStream *cam);
int camera_stream_get_height(const CameraStream *cam);
int camera_stream_get_stride(const CameraStream *cam);
size_t camera_stream_get_frame_size(const CameraStream *cam);
bool camera_stream_is_bgr(const CameraStream *cam);

/*
 * High-resolution still stream dimension queries.
 */
int camera_stream_get_still_width(const CameraStream *cam);
int camera_stream_get_still_height(const CameraStream *cam);

/*
 * Thread-safe copy of the latest preview frame into out_buffer.
 * buffer_size should be at least camera_stream_get_frame_size(cam).
 * out_sequence receives the monotonic sequence number of the copied frame.
 * Returns true if a new frame was copied, false if no new frame is available.
 */
bool camera_stream_get_latest_frame(CameraStream *cam, uint8_t *out_buffer,
                                    size_t buffer_size, uint64_t *out_sequence);

/*
 * Exposure status containing both user settings and actual metadata values.
 */
typedef struct CameraExposureStatus {
  float ev_compensation;      /* User EV setting (-3.0f ~ +3.0f) */
  int iso_setting;            /* User ISO setting (0 = Auto, 100 ~ 3200) */
  int32_t shutter_us_setting; /* User shutter speed in us (0 = Auto) */
  float actual_analogue_gain; /* Actual analogue gain reported by sensor */
  int32_t actual_exposure_us; /* Actual exposure time in us reported by sensor */
} CameraExposureStatus;

/*
 * Set exposure compensation in EV (-3.0f to +3.0f).
 * Returns true on success, false on failure.
 */
bool camera_stream_set_ev(CameraStream *cam, float ev);

/*
 * Set manual ISO sensitivity (0 = Auto, or 100, 200, 400, 800, 1600, 3200).
 * Maps ISO 100 to AnalogueGain 1.0f.
 * Returns true on success, false on failure.
 */
bool camera_stream_set_iso(CameraStream *cam, int iso);

/*
 * Set manual shutter speed in microseconds (0 = Auto, >0 = manual exposure
 * time). Automatically adjusts FrameDurationLimits for long exposures (>1/30s).
 * Returns true on success, false on failure.
 */
bool camera_stream_set_shutter_speed(CameraStream *cam,
                                     int32_t exposure_time_us);

/*
 * Retrieve current exposure settings and actual sensor metadata values.
 */
void camera_stream_get_exposure_status(const CameraStream *cam,
                                       CameraExposureStatus *out_status);

/*
 * Trigger one-shot autofocus cycle (equivalent to 'F' key in pycamera).
 * Returns true if focused successfully, false otherwise.
 */
bool camera_stream_trigger_af(CameraStream *cam);

/*
 * Callback function type for encoding and saving a captured still image buffer.
 * Returns true on success, false on failure.
 */
typedef bool (*CameraStillSaveCallback)(const char *filename,
                                        const uint8_t *data, int width,
                                        int height, int stride, bool is_bgr);

/*
 * Capture a high-resolution still image (equivalent to 'Space' key).
 * Uses the dedicated still stream (dual-stream or mode-switch fallback) and
 * invokes save_cb to encode and write the image to filename.
 * Returns true on success, false on failure.
 */
bool camera_stream_capture_still(CameraStream *cam, const char *filename,
                                 CameraStillSaveCallback save_cb);

/*
 * High-resolution captured still frame returned by non-blocking capture API.
 * Ownership of `data` (heap-allocated via malloc) is transferred to the caller,
 * who must release it with free().
 */
typedef struct CameraCapturedStill {
  uint8_t *data;
  size_t size;
  int width;
  int height;
  int stride;
  bool is_bgr;
  CameraExposureStatus exp_status;
} CameraCapturedStill;

/*
 * Request an asynchronous high-resolution still capture without blocking the
 * preview loop (in dual-stream mode). Poll completion via
 * camera_stream_poll_captured_still().
 * Returns true if capture request was accepted, false otherwise.
 */
bool camera_stream_request_still(CameraStream *cam);

/*
 * Return true if an asynchronous still capture is currently in flight or ready
 * to be polled.
 */
bool camera_stream_is_still_pending(const CameraStream *cam);

/*
 * Poll for a completed asynchronous still capture.
 * If a captured frame is ready, populates *out_still (transferring ownership of
 * out_still->data to the caller) and returns true. Otherwise returns false
 * immediately without blocking.
 */
bool camera_stream_poll_captured_still(CameraStream *cam,
                                       CameraCapturedStill *out_still);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_STREAM_H */
