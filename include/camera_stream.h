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
 * Trigger one-shot autofocus cycle (equivalent to 'F' key in pycamera).
 * Returns true if focused successfully, false otherwise.
 */
bool camera_stream_trigger_af(CameraStream *cam);

/*
 * Capture a high-resolution still JPEG image to disk (equivalent to 'Space'
 * key). Uses the dedicated still stream (dual-stream or mode-switch fallback).
 * Returns true on success, false on failure.
 */
bool camera_stream_capture_still(CameraStream *cam, const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* CAMERA_STREAM_H */
