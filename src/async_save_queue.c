#include "async_save_queue.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

typedef struct MemRwBuffer {
  uint8_t *data;
  size_t size;
  size_t pos;
  size_t capacity;
  bool error;
} MemRwBuffer;

static Sint64 SDLCALL mem_rw_size(SDL_RWops *context) {
  const MemRwBuffer *buf = (const MemRwBuffer *)context->hidden.unknown.data1;
  return buf ? (Sint64)buf->size : -1;
}

static Sint64 SDLCALL mem_rw_seek(SDL_RWops *context, Sint64 offset,
                                  int whence) {
  MemRwBuffer *buf = (MemRwBuffer *)context->hidden.unknown.data1;
  if (!buf) {
    return -1;
  }

  Sint64 new_pos = 0;
  switch (whence) {
  case RW_SEEK_SET:
    new_pos = offset;
    break;
  case RW_SEEK_CUR:
    new_pos = (Sint64)buf->pos + offset;
    break;
  case RW_SEEK_END:
    new_pos = (Sint64)buf->size + offset;
    break;
  default:
    return -1;
  }

  if (new_pos < 0) {
    return -1;
  }
  buf->pos = (size_t)new_pos;
  return (Sint64)buf->pos;
}

static size_t SDLCALL mem_rw_read(SDL_RWops *context, void *ptr, size_t size,
                                  size_t maxnum) {
  MemRwBuffer *buf = (MemRwBuffer *)context->hidden.unknown.data1;
  if (!buf || !ptr || size == 0 || maxnum == 0 || buf->pos >= buf->size) {
    return 0;
  }

  size_t avail = (buf->size - buf->pos) / size;
  size_t count = maxnum < avail ? maxnum : avail;
  if (count > 0) {
    size_t bytes = count * size;
    memcpy(ptr, buf->data + buf->pos, bytes);
    buf->pos += bytes;
  }
  return count;
}

static size_t SDLCALL mem_rw_write(SDL_RWops *context, const void *ptr,
                                   size_t size, size_t num) {
  MemRwBuffer *buf = (MemRwBuffer *)context->hidden.unknown.data1;
  if (!buf || !ptr || size == 0 || num == 0 || buf->error) {
    return 0;
  }

  if (num > (SIZE_MAX / size)) {
    buf->error = true;
    return 0;
  }
  size_t total = size * num;
  if (total > (SIZE_MAX - buf->pos)) {
    buf->error = true;
    return 0;
  }
  size_t end_pos = buf->pos + total;

  if (end_pos > buf->capacity) {
    size_t new_cap = buf->capacity > 0 ? buf->capacity : (256U * 1024U);
    while (new_cap < end_pos) {
      if (new_cap > (SIZE_MAX / 2U)) {
        new_cap = end_pos;
        break;
      }
      new_cap *= 2U;
    }
    uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
    if (!new_data) {
      buf->error = true;
      return 0;
    }
    if (buf->pos > buf->size) {
      memset(new_data + buf->size, 0, buf->pos - buf->size);
    }
    buf->data = new_data;
    buf->capacity = new_cap;
  }

  memcpy(buf->data + buf->pos, ptr, total);
  buf->pos += total;
  if (buf->pos > buf->size) {
    buf->size = buf->pos;
  }
  return num;
}

static int SDLCALL mem_rw_close(SDL_RWops *context) {
  if (context) {
    SDL_FreeRW(context);
  }
  return 0;
}

static bool encode_surface_to_jpeg_mem(SDL_Surface *surface, int quality,
                                       size_t init_capacity, uint8_t **out_jpeg,
                                       size_t *out_size) {
  if (!surface || !out_jpeg || !out_size) {
    return false;
  }
  *out_jpeg = NULL;
  *out_size = 0;

  MemRwBuffer mem_buf;
  memset(&mem_buf, 0, sizeof(mem_buf));
  mem_buf.capacity = init_capacity > 0 ? init_capacity : (64U * 1024U);
  mem_buf.data = (uint8_t *)malloc(mem_buf.capacity);
  if (!mem_buf.data) {
    return false;
  }

  SDL_RWops *rw = SDL_AllocRW();
  if (!rw) {
    free(mem_buf.data);
    return false;
  }
  rw->size = mem_rw_size;
  rw->seek = mem_rw_seek;
  rw->read = mem_rw_read;
  rw->write = mem_rw_write;
  rw->close = mem_rw_close;
  rw->type = SDL_RWOPS_UNKNOWN;
  rw->hidden.unknown.data1 = &mem_buf;

  int ret = IMG_SaveJPG_RW(surface, rw, 0, quality);
  SDL_FreeRW(rw);

  if (ret != 0 || mem_buf.error || mem_buf.size < 4) {
    free(mem_buf.data);
    return false;
  }

  *out_jpeg = mem_buf.data;
  *out_size = mem_buf.size;
  return true;
}

static bool build_thumbnail_jpeg(const uint8_t *raw_data, int width, int height,
                                 int stride, bool is_bgr, uint8_t **out_jpeg,
                                 size_t *out_size) {
  if (!raw_data || width <= 0 || height <= 0 || !out_jpeg || !out_size) {
    return false;
  }
  *out_jpeg = NULL;
  *out_size = 0;

  float scale_x = (float)EXIF_THUMB_MAX_W / (float)width;
  float scale_y = (float)EXIF_THUMB_MAX_H / (float)height;
  float scale = scale_x < scale_y ? scale_x : scale_y;
  if (scale > 1.0f) {
    scale = 1.0f;
  }

  int thumb_w = (int)((float)width * scale + 0.5f);
  int thumb_h = (int)((float)height * scale + 0.5f);
  if (thumb_w < 1) {
    thumb_w = 1;
  }
  if (thumb_h < 1) {
    thumb_h = 1;
  }

  int effective_stride = stride > 0 ? stride : (width * 3);
  int thumb_stride = thumb_w * 3;
  uint8_t *thumb_rgb =
      (uint8_t *)malloc((size_t)thumb_stride * (size_t)thumb_h);
  if (!thumb_rgb) {
    return false;
  }

  const int r_idx = is_bgr ? 2 : 0;
  const int g_idx = 1;
  const int b_idx = is_bgr ? 0 : 2;

  /* Fast 2x2 box-sampled downscaling directly from raw_data to RGB24 */
  for (int ty = 0; ty < thumb_h; ++ty) {
    int sy0 = (ty * height) / thumb_h;
    int sy1 = ((ty + 1) * height) / thumb_h - 1;
    if (sy1 < sy0) {
      sy1 = sy0;
    }
    int sy_mid = (sy0 + sy1) >> 1;

    const uint8_t *row0 = raw_data + (size_t)sy0 * (size_t)effective_stride;
    const uint8_t *row1 = raw_data + (size_t)sy_mid * (size_t)effective_stride;
    uint8_t *dst = thumb_rgb + (size_t)ty * (size_t)thumb_stride;

    for (int tx = 0; tx < thumb_w; ++tx) {
      int sx0 = (tx * width) / thumb_w;
      int sx1 = ((tx + 1) * width) / thumb_w - 1;
      if (sx1 < sx0) {
        sx1 = sx0;
      }
      int sx_mid = (sx0 + sx1) >> 1;

      const uint8_t *p00 = row0 + sx0 * 3;
      const uint8_t *p10 = row0 + sx_mid * 3;
      const uint8_t *p01 = row1 + sx0 * 3;
      const uint8_t *p11 = row1 + sx_mid * 3;

      dst[0] = (uint8_t)(((unsigned int)p00[r_idx] + (unsigned int)p10[r_idx] +
                          (unsigned int)p01[r_idx] + (unsigned int)p11[r_idx] +
                          2U) >>
                         2);
      dst[1] = (uint8_t)(((unsigned int)p00[g_idx] + (unsigned int)p10[g_idx] +
                          (unsigned int)p01[g_idx] + (unsigned int)p11[g_idx] +
                          2U) >>
                         2);
      dst[2] = (uint8_t)(((unsigned int)p00[b_idx] + (unsigned int)p10[b_idx] +
                          (unsigned int)p01[b_idx] + (unsigned int)p11[b_idx] +
                          2U) >>
                         2);
      dst += 3;
    }
  }

  SDL_Surface *thumb_surf = SDL_CreateRGBSurfaceWithFormatFrom(
      thumb_rgb, thumb_w, thumb_h, 24, thumb_stride, SDL_PIXELFORMAT_RGB24);
  if (!thumb_surf) {
    free(thumb_rgb);
    return false;
  }

  bool ok = encode_surface_to_jpeg_mem(thumb_surf, EXIF_THUMB_JPEG_QUALITY,
                                       16U * 1024U, out_jpeg, out_size);
  SDL_FreeSurface(thumb_surf);
  free(thumb_rgb);
  return ok;
}

static bool encode_and_save_job(const AsyncSaveJob *job) {
  if (!job || !job->raw_data || job->width <= 0 || job->height <= 0 ||
      job->filepath[0] == '\0') {
    return false;
  }

  int effective_stride = job->stride > 0 ? job->stride : (job->width * 3);
  Uint32 format = job->is_bgr ? SDL_PIXELFORMAT_BGR24 : SDL_PIXELFORMAT_RGB24;

  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
      (void *)job->raw_data, job->width, job->height, 24, effective_stride,
      format);
  if (!surface) {
    fprintf(stderr, "[エラー] JPEG保存用サーフェスの作成に失敗しました: %s\n",
            SDL_GetError());
    return false;
  }

  int quality = job->jpeg_quality;
  if (quality < 10) {
    quality = 10;
  } else if (quality > 100) {
    quality = 100;
  }

  /* Encode full-resolution JPEG in memory */
  uint8_t *jpeg_data = NULL;
  size_t jpeg_size = 0;
  bool enc_ok = encode_surface_to_jpeg_mem(surface, quality, 512U * 1024U,
                                           &jpeg_data, &jpeg_size);
  SDL_FreeSurface(surface);

  if (!enc_ok || !jpeg_data || jpeg_size < 4) {
    fprintf(stderr, "[エラー] IMG_SaveJPG_RW による保存に失敗しました: %s\n",
            IMG_GetError());
    free(jpeg_data);
    return false;
  }

  /* Encode small Exif IFD1 thumbnail JPEG in memory for fast Grid playback */
  uint8_t *thumb_data = NULL;
  size_t thumb_size = 0;
  build_thumbnail_jpeg(job->raw_data, job->width, job->height, effective_stride,
                       job->is_bgr, &thumb_data, &thumb_size);

  bool ok = exif_write_jpeg_with_app1_and_thumbnail(
      job->filepath, jpeg_data, jpeg_size, &job->exif, thumb_data, thumb_size);
  free(thumb_data);
  free(jpeg_data);
  return ok;
}

bool async_save_queue_save_sync(const char *filepath, const uint8_t *raw_data,
                                int width, int height, int stride, bool is_bgr,
                                int jpeg_quality, const ExifMetadata *exif) {
  if (!filepath || filepath[0] == '\0' || !raw_data || width <= 0 ||
      height <= 0) {
    return false;
  }

  AsyncSaveJob job;
  memset(&job, 0, sizeof(job));
  job.valid = true;
  snprintf(job.filepath, sizeof(job.filepath), "%s", filepath);
  job.raw_data = (uint8_t *)raw_data;
  job.width = width;
  job.height = height;
  job.stride = stride;
  job.is_bgr = is_bgr;
  job.jpeg_quality = jpeg_quality;
  if (exif) {
    job.exif = *exif;
  }

  return encode_and_save_job(&job);
}

static void *save_worker_thread_main(void *arg) {
  AsyncSaveQueue *q = (AsyncSaveQueue *)arg;

  for (;;) {
    AsyncSaveJob job;
    memset(&job, 0, sizeof(job));

    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->stop_requested) {
      pthread_cond_wait(&q->cond_not_empty, &q->mutex);
    }

    if (q->count == 0 && q->stop_requested) {
      pthread_mutex_unlock(&q->mutex);
      break;
    }

    /* Dequeue next valid job */
    job = q->jobs[q->head];
    memset(&q->jobs[q->head], 0, sizeof(AsyncSaveJob));
    q->head = (q->head + 1) % ASYNC_SAVE_QUEUE_CAPACITY;
    q->count--;

    if (!job.valid || job.cancelled) {
      if (job.raw_data) {
        free(job.raw_data);
      }
      pthread_cond_broadcast(&q->cond_idle);
      pthread_mutex_unlock(&q->mutex);
      continue;
    }

    q->worker_busy = true;
    snprintf(q->active_filepath, sizeof(q->active_filepath), "%s",
             job.filepath);
    q->active_cancelled = false;
    q->active_protect = job.protect_after_save;
    pthread_mutex_unlock(&q->mutex);

    /* Perform heavy JPEG encoding + Exif APP1 injection + SD write outside lock
     */
    bool saved = encode_and_save_job(&job);
    free(job.raw_data);
    job.raw_data = NULL;

    pthread_mutex_lock(&q->mutex);
    bool was_cancelled = q->active_cancelled;
    bool should_protect = q->active_protect;
    q->worker_busy = false;
    q->active_filepath[0] = '\0';
    q->active_cancelled = false;
    q->active_protect = false;

    if (saved) {
      if (was_cancelled) {
        unlink(job.filepath);
        printf("[-] バックグラウンド保存をキャンセル・破棄しました: %s\n",
               job.filepath);
      } else {
        if (should_protect) {
          dcf_storage_set_protected(job.filepath, true);
        }
        printf("[★保存完了] %s (%dx%d, Q:%d, Exif付加)\n", job.filepath,
               job.width, job.height, job.jpeg_quality);
      }
    } else if (!was_cancelled) {
      fprintf(stderr, "[エラー] バックグラウンド画像保存に失敗しました: %s\n",
              job.filepath);
    }

    pthread_cond_broadcast(&q->cond_idle);
    pthread_mutex_unlock(&q->mutex);
  }

  return NULL;
}

bool async_save_queue_init(AsyncSaveQueue *q) {
  if (!q) {
    return false;
  }
  memset(q, 0, sizeof(*q));

  if (pthread_mutex_init(&q->mutex, NULL) != 0) {
    return false;
  }
  if (pthread_cond_init(&q->cond_not_empty, NULL) != 0) {
    pthread_mutex_destroy(&q->mutex);
    return false;
  }
  if (pthread_cond_init(&q->cond_idle, NULL) != 0) {
    pthread_cond_destroy(&q->cond_not_empty);
    pthread_mutex_destroy(&q->mutex);
    return false;
  }

  if (pthread_create(&q->worker_thread, NULL, save_worker_thread_main, q) !=
      0) {
    pthread_cond_destroy(&q->cond_idle);
    pthread_cond_destroy(&q->cond_not_empty);
    pthread_mutex_destroy(&q->mutex);
    return false;
  }

  q->initialized = true;
  return true;
}

void async_save_queue_destroy(AsyncSaveQueue *q) {
  if (!q || !q->initialized) {
    return;
  }

  pthread_mutex_lock(&q->mutex);
  q->stop_requested = true;
  pthread_cond_broadcast(&q->cond_not_empty);
  pthread_mutex_unlock(&q->mutex);

  pthread_join(q->worker_thread, NULL);

  /* Free any remaining unconsumed buffers */
  for (size_t i = 0; i < ASYNC_SAVE_QUEUE_CAPACITY; ++i) {
    if (q->jobs[i].raw_data) {
      free(q->jobs[i].raw_data);
      q->jobs[i].raw_data = NULL;
    }
  }

  pthread_cond_destroy(&q->cond_idle);
  pthread_cond_destroy(&q->cond_not_empty);
  pthread_mutex_destroy(&q->mutex);
  q->initialized = false;
}

bool async_save_queue_push(AsyncSaveQueue *q, const char *filepath,
                           uint8_t *raw_data, size_t data_size, int width,
                           int height, int stride, bool is_bgr,
                           int jpeg_quality, const ExifMetadata *exif) {
  if (!q || !q->initialized || !filepath || !raw_data || width <= 0 ||
      height <= 0) {
    return false;
  }

  pthread_mutex_lock(&q->mutex);
  if (q->stop_requested || q->count >= ASYNC_SAVE_QUEUE_CAPACITY) {
    pthread_mutex_unlock(&q->mutex);
    fprintf(stderr,
            "[警告] バックグラウンド保存キューが満杯です (%zu / %d)。\n",
            q->count, ASYNC_SAVE_QUEUE_CAPACITY);
    return false;
  }

  AsyncSaveJob *slot = &q->jobs[q->tail];
  memset(slot, 0, sizeof(*slot));
  slot->valid = true;
  slot->cancelled = false;
  slot->protect_after_save = false;
  snprintf(slot->filepath, sizeof(slot->filepath), "%s", filepath);
  slot->raw_data = raw_data;
  slot->data_size = data_size;
  slot->width = width;
  slot->height = height;
  slot->stride = stride;
  slot->is_bgr = is_bgr;
  slot->jpeg_quality = jpeg_quality;
  if (exif) {
    slot->exif = *exif;
  }

  q->tail = (q->tail + 1) % ASYNC_SAVE_QUEUE_CAPACITY;
  q->count++;

  pthread_cond_signal(&q->cond_not_empty);
  pthread_mutex_unlock(&q->mutex);
  return true;
}

int async_save_queue_get_pending_count(AsyncSaveQueue *q) {
  if (!q || !q->initialized) {
    return 0;
  }
  pthread_mutex_lock(&q->mutex);
  int total = (int)q->count + (q->worker_busy ? 1 : 0);
  pthread_mutex_unlock(&q->mutex);
  return total;
}

void async_save_queue_wait_idle(AsyncSaveQueue *q) {
  if (!q || !q->initialized) {
    return;
  }
  pthread_mutex_lock(&q->mutex);
  while (q->count > 0 || q->worker_busy) {
    pthread_cond_wait(&q->cond_idle, &q->mutex);
  }
  pthread_mutex_unlock(&q->mutex);
}

bool async_save_queue_cancel_or_delete(AsyncSaveQueue *q, DcfStorage *dcf,
                                       const char *filepath) {
  if (!filepath || filepath[0] == '\0') {
    return false;
  }

  if (q && q->initialized) {
    pthread_mutex_lock(&q->mutex);

    /* 1. Check if the file is still waiting in the ring queue */
    for (size_t i = 0; i < q->count; ++i) {
      size_t idx = (q->head + i) % ASYNC_SAVE_QUEUE_CAPACITY;
      AsyncSaveJob *job = &q->jobs[idx];
      if (job->valid && !job->cancelled &&
          strcmp(job->filepath, filepath) == 0) {
        if (job->protect_after_save) {
          pthread_mutex_unlock(&q->mutex);
          return false;
        }
        job->cancelled = true;
        if (job->raw_data) {
          free(job->raw_data);
          job->raw_data = NULL;
        }
        pthread_mutex_unlock(&q->mutex);
        if (dcf) {
          dcf_storage_resync(dcf);
        }
        return true;
      }
    }

    /* 2. Check if the worker thread is currently encoding/writing this file */
    if (q->worker_busy && strcmp(q->active_filepath, filepath) == 0) {
      if (q->active_protect) {
        pthread_mutex_unlock(&q->mutex);
        return false;
      }
      q->active_cancelled = true;
      while (q->worker_busy && strcmp(q->active_filepath, filepath) == 0) {
        pthread_cond_wait(&q->cond_idle, &q->mutex);
      }
      pthread_mutex_unlock(&q->mutex);
      if (dcf) {
        dcf_storage_resync(dcf);
      }
      return true;
    }

    pthread_mutex_unlock(&q->mutex);
  }

  /* 3. Already written to disk: delete via DCF storage */
  return dcf_storage_delete_file(dcf, filepath);
}

bool async_save_queue_set_protected(AsyncSaveQueue *q, const char *filepath,
                                    bool protect) {
  if (!filepath || filepath[0] == '\0') {
    return false;
  }

  if (q && q->initialized) {
    pthread_mutex_lock(&q->mutex);

    /* 1. Check queued jobs */
    for (size_t i = 0; i < q->count; ++i) {
      size_t idx = (q->head + i) % ASYNC_SAVE_QUEUE_CAPACITY;
      AsyncSaveJob *job = &q->jobs[idx];
      if (job->valid && !job->cancelled &&
          strcmp(job->filepath, filepath) == 0) {
        job->protect_after_save = protect;
        pthread_mutex_unlock(&q->mutex);
        return true;
      }
    }

    /* 2. Check active job */
    if (q->worker_busy && strcmp(q->active_filepath, filepath) == 0) {
      q->active_protect = protect;
      pthread_mutex_unlock(&q->mutex);
      return true;
    }

    pthread_mutex_unlock(&q->mutex);
  }

  /* 3. Already on disk */
  return dcf_storage_set_protected(filepath, protect);
}
