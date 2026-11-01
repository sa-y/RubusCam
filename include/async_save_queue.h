#ifndef ASYNC_SAVE_QUEUE_H
#define ASYNC_SAVE_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dcf_storage.h"
#include "exif_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Maximum number of high-resolution still frames buffered in the save queue.
 */
#define ASYNC_SAVE_QUEUE_CAPACITY 8

/*
 * Single background JPEG + Exif save task.
 */
typedef struct AsyncSaveJob {
  bool valid;
  bool cancelled;
  bool protect_after_save;
  char filepath[512];
  uint8_t *raw_data; /* Heap-allocated RGB24/BGR24 buffer owned by the job */
  size_t data_size;
  int width;
  int height;
  int stride;
  bool is_bgr;
  int jpeg_quality;
  ExifMetadata exif;
} AsyncSaveJob;

/*
 * Thread-safe background JPEG encoding and SD card write queue.
 */
typedef struct AsyncSaveQueue {
  pthread_t worker_thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond_not_empty;
  pthread_cond_t cond_idle;

  bool initialized;
  bool stop_requested;
  bool worker_busy;
  char active_filepath[512];
  bool active_cancelled;
  bool active_protect;

  AsyncSaveJob jobs[ASYNC_SAVE_QUEUE_CAPACITY];
  size_t head;
  size_t tail;
  size_t count;
} AsyncSaveQueue;

/*
 * Initialize the background save queue and spawn its worker thread.
 * Returns true on success, false on failure.
 */
bool async_save_queue_init(AsyncSaveQueue *q);

/*
 * Flush all pending save jobs, stop the worker thread, and release resources.
 */
void async_save_queue_destroy(AsyncSaveQueue *q);

/*
 * Enqueue a captured raw RGB24/BGR24 image buffer for background JPEG encoding,
 * Exif APP1 metadata injection, and disk writing.
 *
 * Ownership of `raw_data` (allocated via malloc) is transferred to the queue
 * when this function returns true. If this function returns false, the caller
 * retains ownership of `raw_data` and must free() it.
 */
bool async_save_queue_push(AsyncSaveQueue *q, const char *filepath,
                           uint8_t *raw_data, size_t data_size, int width,
                           int height, int stride, bool is_bgr,
                           int jpeg_quality, const ExifMetadata *exif);

/*
 * Return the total number of save tasks currently queued or being written.
 */
int async_save_queue_get_pending_count(AsyncSaveQueue *q);

/*
 * Block until all queued and active save tasks have finished writing to disk.
 */
void async_save_queue_wait_idle(AsyncSaveQueue *q);

/*
 * Cancel a pending/active save job for `filepath`, or delete the file from disk
 * if it has already been written. Safe to call during Quick Review discard.
 */
bool async_save_queue_cancel_or_delete(AsyncSaveQueue *q, DcfStorage *dcf,
                                       const char *filepath);

/*
 * Mark a pending/active save job to be protected (read-only) upon completion,
 * or immediately update file protection on disk if already written.
 */
bool async_save_queue_set_protected(AsyncSaveQueue *q, const char *filepath,
                                    bool protect);

#ifdef __cplusplus
}
#endif

#endif /* ASYNC_SAVE_QUEUE_H */
