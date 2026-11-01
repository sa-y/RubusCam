#include "async_save_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

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

  /* Encode to a temporary staging file first so incomplete JPEGs are never
   * visible under the target .JPG name */
  char enc_tmp_path[540];
  int written =
      snprintf(enc_tmp_path, sizeof(enc_tmp_path), "%s.enc.tmp", job->filepath);
  if (written < 0 || (size_t)written >= sizeof(enc_tmp_path)) {
    SDL_FreeSurface(surface);
    return false;
  }

  int ret = IMG_SaveJPG(surface, enc_tmp_path, quality);
  SDL_FreeSurface(surface);

  if (ret != 0) {
    fprintf(stderr, "[エラー] IMG_SaveJPG による保存に失敗しました: %s\n",
            IMG_GetError());
    unlink(enc_tmp_path);
    return false;
  }

  /* Read encoded JPEG stream and atomically write final file with Exif APP1 */
  FILE *fp = fopen(enc_tmp_path, "rb");
  if (!fp) {
    unlink(enc_tmp_path);
    return false;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    unlink(enc_tmp_path);
    return false;
  }
  long file_len = ftell(fp);
  if (file_len < 4 || fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    unlink(enc_tmp_path);
    return false;
  }

  uint8_t *jpeg_buf = (uint8_t *)malloc((size_t)file_len);
  if (!jpeg_buf) {
    fclose(fp);
    unlink(enc_tmp_path);
    return false;
  }

  size_t nread = fread(jpeg_buf, 1, (size_t)file_len, fp);
  fclose(fp);
  unlink(enc_tmp_path);

  if (nread != (size_t)file_len) {
    free(jpeg_buf);
    return false;
  }

  bool ok =
      exif_write_jpeg_with_app1(job->filepath, jpeg_buf, nread, &job->exif);
  free(jpeg_buf);
  return ok;
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
      if (q->count == 0 && !q->worker_busy) {
        pthread_cond_broadcast(&q->cond_idle);
      }
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

    if (q->count == 0 && !q->worker_busy) {
      pthread_cond_broadcast(&q->cond_idle);
    }
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
