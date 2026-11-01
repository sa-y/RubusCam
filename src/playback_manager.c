#include "playback_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "text_renderer.h"

static const float ZOOM_FACTORS[] = {1.0f, 2.0f, 4.0f, 8.0f};
static const int ZOOM_STEP_COUNT =
    (int)(sizeof(ZOOM_FACTORS) / sizeof(ZOOM_FACTORS[0]));

static void set_toast(PlaybackManager *pm, const char *msg,
                      uint32_t duration_ms) {
  if (!pm || !msg) {
    return;
  }
  snprintf(pm->toast_message, sizeof(pm->toast_message), "%s", msg);
  pm->toast_expire_ms = SDL_GetTicks() + duration_ms;
}

static void free_current_texture(PlaybackManager *pm) {
  if (!pm) {
    return;
  }
  if (pm->current_texture) {
    SDL_DestroyTexture(pm->current_texture);
    pm->current_texture = NULL;
  }
  pm->current_tex_w = 0;
  pm->current_tex_h = 0;
}

static void free_grid_textures(PlaybackManager *pm) {
  if (!pm) {
    return;
  }
  for (int i = 0; i < PLAYBACK_MAX_GRID_CELLS; ++i) {
    if (pm->grid_textures[i]) {
      SDL_DestroyTexture(pm->grid_textures[i]);
      pm->grid_textures[i] = NULL;
    }
    pm->grid_tex_w[i] = 0;
    pm->grid_tex_h[i] = 0;
  }
  pm->cached_grid_page = -1;
}

static void calculate_aspect_fit_rect(int src_w, int src_h,
                                      const SDL_Rect *bounds,
                                      SDL_Rect *out_rect) {
  if (src_w <= 0 || src_h <= 0 || bounds->w <= 0 || bounds->h <= 0) {
    *out_rect = *bounds;
    return;
  }

  float src_aspect = (float)src_w / (float)src_h;
  float dst_aspect = (float)bounds->w / (float)bounds->h;

  if (dst_aspect > src_aspect) {
    out_rect->h = bounds->h;
    out_rect->w = (int)((float)bounds->h * src_aspect + 0.5f);
    out_rect->x = bounds->x + (bounds->w - out_rect->w) / 2;
    out_rect->y = bounds->y;
  } else {
    out_rect->w = bounds->w;
    out_rect->h = (int)((float)bounds->w / src_aspect + 0.5f);
    out_rect->x = bounds->x;
    out_rect->y = bounds->y + (bounds->h - out_rect->h) / 2;
  }
}

/*
 * Create a GPU texture from an SDL_Surface, automatically downscaling if the
 * surface exceeds the hardware renderer's max_texture_width/max_texture_height
 * (e.g. 4096px limit on Raspberry Pi 4 VideoCore VI vs 4608px IMX708 still).
 */
static SDL_Texture *
create_safe_texture_from_surface(SDL_Renderer *renderer, SDL_Surface *surf,
                                 int max_target_w, int max_target_h,
                                 int *out_tex_w, int *out_tex_h) {
  if (!renderer || !surf) {
    return NULL;
  }

  int limit_w = 4096;
  int limit_h = 4096;
  SDL_RendererInfo info;
  if (SDL_GetRendererInfo(renderer, &info) == 0) {
    if (info.max_texture_width > 0) {
      limit_w = info.max_texture_width;
    }
    if (info.max_texture_height > 0) {
      limit_h = info.max_texture_height;
    }
  }

  if (max_target_w > 0 && max_target_w < limit_w) {
    limit_w = max_target_w;
  }
  if (max_target_h > 0 && max_target_h < limit_h) {
    limit_h = max_target_h;
  }

  if (surf->w <= limit_w && surf->h <= limit_h) {
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
      if (out_tex_w) {
        *out_tex_w = surf->w;
      }
      if (out_tex_h) {
        *out_tex_h = surf->h;
      }
      return tex;
    }
  }

  /* Scale down proportionally to fit within (limit_w, limit_h) */
  float scale_x = (float)limit_w / (float)surf->w;
  float scale_y = (float)limit_h / (float)surf->h;
  float scale = scale_x < scale_y ? scale_x : scale_y;
  int scaled_w = (int)((float)surf->w * scale);
  int scaled_h = (int)((float)surf->h * scale);
  if (scaled_w < 1) {
    scaled_w = 1;
  }
  if (scaled_h < 1) {
    scaled_h = 1;
  }

  SDL_Surface *rgb_surf =
      SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGB24, 0);
  if (!rgb_surf) {
    return NULL;
  }

  SDL_Surface *scaled_surf = SDL_CreateRGBSurfaceWithFormat(
      0, scaled_w, scaled_h, 24, SDL_PIXELFORMAT_RGB24);
  if (!scaled_surf) {
    SDL_FreeSurface(rgb_surf);
    return NULL;
  }

  SDL_BlitScaled(rgb_surf, NULL, scaled_surf, NULL);
  SDL_FreeSurface(rgb_surf);

  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, scaled_surf);
  if (tex) {
    if (out_tex_w) {
      *out_tex_w = scaled_w;
    }
    if (out_tex_h) {
      *out_tex_h = scaled_h;
    }
  }
  SDL_FreeSurface(scaled_surf);
  return tex;
}

/* --------------------------------------------------------------------------
 * Lightweight JPEG / Exif APP1 metadata parser
 * -------------------------------------------------------------------------- */

static uint16_t read_u16(const uint8_t *p, bool little_endian) {
  if (little_endian) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  }
  return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t read_u32(const uint8_t *p, bool little_endian) {
  if (little_endian) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
  }
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void parse_exif_ifd(const uint8_t *tiff_base, size_t tiff_len,
                           uint32_t ifd_offset, bool le, PhotoMetadata *meta,
                           int depth) {
  if (depth > 3 || ifd_offset + 2 > tiff_len) {
    return;
  }

  uint16_t num_entries = read_u16(tiff_base + ifd_offset, le);
  size_t pos = ifd_offset + 2;

  for (uint16_t i = 0; i < num_entries; ++i, pos += 12) {
    if (pos + 12 > tiff_len) {
      break;
    }
    uint16_t tag = read_u16(tiff_base + pos, le);
    uint16_t type = read_u16(tiff_base + pos + 2, le);
    uint32_t count = read_u32(tiff_base + pos + 4, le);
    uint32_t val_offset = read_u32(tiff_base + pos + 8, le);

    if (tag == 0x8769 && (type == 4 || type == 3)) {
      /* Exif SubIFD pointer */
      parse_exif_ifd(tiff_base, tiff_len, val_offset, le, meta, depth + 1);
    } else if (tag == 0x0110 && type == 2 && count > 0) {
      /* Camera Model (ASCII) */
      const uint8_t *str_ptr = (count <= 4) ? (tiff_base + pos + 8)
                                            : (val_offset + count <= tiff_len
                                                   ? tiff_base + val_offset
                                                   : NULL);
      if (str_ptr) {
        size_t copy_len = count < sizeof(meta->camera_model) - 1
                              ? count
                              : sizeof(meta->camera_model) - 1;
        memcpy(meta->camera_model, str_ptr, copy_len);
        meta->camera_model[copy_len] = '\0';
        meta->has_exif = true;
      }
    } else if ((tag == 0x9003 || tag == 0x0132) && type == 2 && count > 0) {
      /* DateTimeOriginal (0x9003) or DateTime (0x0132) */
      if (tag == 0x0132 && meta->datetime_str[0] != '\0') {
        continue;
      }
      const uint8_t *str_ptr = (count <= 4) ? (tiff_base + pos + 8)
                                            : (val_offset + count <= tiff_len
                                                   ? tiff_base + val_offset
                                                   : NULL);
      if (str_ptr) {
        size_t copy_len = count < sizeof(meta->datetime_str) - 1
                              ? count
                              : sizeof(meta->datetime_str) - 1;
        memcpy(meta->datetime_str, str_ptr, copy_len);
        meta->datetime_str[copy_len] = '\0';
        meta->has_exif = true;
      }
    } else if (tag == 0x8827) {
      /* ISOSpeedRatings (SHORT or LONG) */
      if (type == 3) {
        meta->iso_value = (int)read_u16(tiff_base + pos + 8, le);
      } else if (type == 4) {
        meta->iso_value = (int)val_offset;
      }
      meta->has_exif = true;
    } else if (tag == 0x829A && type == 5 && val_offset + 8 <= tiff_len) {
      /* ExposureTime (RATIONAL: num / den seconds) */
      uint32_t num = read_u32(tiff_base + val_offset, le);
      uint32_t den = read_u32(tiff_base + val_offset + 4, le);
      if (den > 0) {
        double secs = (double)num / (double)den;
        meta->exposure_us = (int32_t)(secs * 1000000.0 + 0.5);
        meta->has_exif = true;
      }
    } else if (tag == 0x829D && type == 5 && val_offset + 8 <= tiff_len) {
      /* FNumber (RATIONAL) */
      uint32_t num = read_u32(tiff_base + val_offset, le);
      uint32_t den = read_u32(tiff_base + val_offset + 4, le);
      if (den > 0) {
        meta->f_number = (float)num / (float)den;
        meta->has_exif = true;
      }
    } else if (tag == 0x9204 && type == 10 && val_offset + 8 <= tiff_len) {
      /* ExposureBiasValue (SRATIONAL) */
      int32_t num = (int32_t)read_u32(tiff_base + val_offset, le);
      int32_t den = (int32_t)read_u32(tiff_base + val_offset + 4, le);
      if (den != 0) {
        meta->ev_bias = (float)num / (float)den;
        meta->has_exif = true;
      }
    } else if (tag == 0x9207 && type == 3) {
      /* MeteringMode (SHORT: 2=Center, 3=Spot, 5=Pattern) */
      meta->metering_mode = read_u16(tiff_base + pos + 8, le);
      meta->has_exif = true;
    } else if (tag == 0x9208 && type == 3) {
      /* LightSource (SHORT) */
      meta->light_source = read_u16(tiff_base + pos + 8, le);
      meta->has_exif = true;
    } else if (tag == 0x920A && type == 5 && val_offset + 8 <= tiff_len) {
      /* FocalLength (RATIONAL) */
      uint32_t num = read_u32(tiff_base + val_offset, le);
      uint32_t den = read_u32(tiff_base + val_offset + 4, le);
      if (den > 0) {
        meta->focal_length_mm = (float)num / (float)den;
        meta->has_exif = true;
      }
    } else if (tag == 0xA403 && type == 3) {
      /* WhiteBalance (SHORT: 0 = Auto, 1 = Manual) */
      meta->wb_mode = read_u16(tiff_base + pos + 8, le);
      meta->has_exif = true;
    }
  }
}

static void parse_jpeg_metadata(PlaybackManager *pm, const DcfMediaEntry *entry,
                                PhotoMetadata *out_meta) {
  memset(out_meta, 0, sizeof(*out_meta));
  if (!entry) {
    return;
  }

  /* 1. Check in-session capture metadata cache first as baseline */
  if (pm) {
    for (size_t i = 0; i < PLAYBACK_META_CACHE_SIZE; ++i) {
      const CaptureMetaRecord *rec = &pm->meta_cache[i];
      if (rec->filepath[0] != '\0' &&
          strcmp(rec->filepath, entry->filepath) == 0) {
        out_meta->width = rec->width;
        out_meta->height = rec->height;
        out_meta->exposure_us = rec->exp_status.shutter_us_setting > 0
                                    ? rec->exp_status.shutter_us_setting
                                    : rec->exp_status.actual_exposure_us;
        out_meta->iso_value =
            rec->exp_status.iso_setting > 0
                ? rec->exp_status.iso_setting
                : (int)(rec->exp_status.actual_analogue_gain * 100.0f + 0.5f);
        out_meta->ev_bias = rec->exp_status.ev_compensation;
        out_meta->wb_mode = (rec->exp_status.awb_locked ||
                             rec->exp_status.wb_mode != CAMERA_WB_MODE_AUTO)
                                ? 1
                                : 0;
        switch (rec->exp_status.metering_mode) {
        case CAMERA_METERING_CENTRE_WEIGHTED:
          out_meta->metering_mode = 2;
          break;
        case CAMERA_METERING_SPOT:
          out_meta->metering_mode = 3;
          break;
        case CAMERA_METERING_MATRIX:
        default:
          out_meta->metering_mode = 5;
          break;
        }
        snprintf(out_meta->camera_model, sizeof(out_meta->camera_model),
                 "RubusCam (libcamera)");
        break;
      }
    }
  }

  /* 2. Inspect JPEG markers (SOF0/SOF2 for dimensions, APP1 for Exif) */
  FILE *fp = fopen(entry->filepath, "rb");
  if (!fp) {
    return;
  }

  uint8_t buf[65536];
  size_t nread = fread(buf, 1, sizeof(buf), fp);
  fclose(fp);

  if (nread >= 4 && buf[0] == 0xFF && buf[1] == 0xD8) {
    size_t pos = 2;
    while (pos + 4 <= nread) {
      if (buf[pos] != 0xFF) {
        pos++;
        continue;
      }
      while (pos < nread && buf[pos] == 0xFF) {
        pos++;
      }
      if (pos >= nread) {
        break;
      }
      uint8_t marker = buf[pos++];
      if (marker == 0xD9 || marker == 0xDA) {
        /* EOI or SOS (Start of Scan) */
        break;
      }
      if (pos + 2 > nread) {
        break;
      }
      uint16_t seg_len = ((uint16_t)buf[pos] << 8) | (uint16_t)buf[pos + 1];
      if (seg_len < 2 || pos + seg_len > nread) {
        break;
      }

      const uint8_t *seg_data = buf + pos + 2;
      size_t seg_payload_len = seg_len - 2;

      if ((marker == 0xC0 || marker == 0xC2) && seg_payload_len >= 5) {
        /* SOF0 (Baseline) or SOF2 (Progressive) */
        out_meta->height = ((int)seg_data[1] << 8) | (int)seg_data[2];
        out_meta->width = ((int)seg_data[3] << 8) | (int)seg_data[4];
      } else if (marker == 0xE1 && seg_payload_len > 14 &&
                 memcmp(seg_data, "Exif\0\0", 6) == 0) {
        /* APP1 Exif header */
        const uint8_t *tiff_base = seg_data + 6;
        size_t tiff_len = seg_payload_len - 6;
        bool le = (tiff_base[0] == 'I' && tiff_base[1] == 'I');
        bool be = (tiff_base[0] == 'M' && tiff_base[1] == 'M');
        if ((le || be) && read_u16(tiff_base + 2, le) == 0x002A) {
          uint32_t ifd0_off = read_u32(tiff_base + 4, le);
          parse_exif_ifd(tiff_base, tiff_len, ifd0_off, le, out_meta, 0);
        }
      }

      pos += seg_len;
    }
  }

  /* 3. Format fallback timestamp from file mtime if Exif DateTime was absent */
  if (out_meta->datetime_str[0] == '\0' && entry->mtime_sec > 0) {
    time_t t = (time_t)entry->mtime_sec;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);
    strftime(out_meta->datetime_str, sizeof(out_meta->datetime_str),
             "%Y:%m:%d %H:%M:%S", &tm_buf);
  }
}

static void record_capture_metadata(PlaybackManager *pm, const char *filepath,
                                    int width, int height,
                                    const CameraExposureStatus *exp_status) {
  if (!pm || !filepath) {
    return;
  }
  CaptureMetaRecord *rec = &pm->meta_cache[pm->meta_cache_next];
  pm->meta_cache_next = (pm->meta_cache_next + 1) % PLAYBACK_META_CACHE_SIZE;

  snprintf(rec->filepath, sizeof(rec->filepath), "%s", filepath);
  rec->width = width;
  rec->height = height;
  if (exp_status) {
    rec->exp_status = *exp_status;
  } else {
    memset(&rec->exp_status, 0, sizeof(rec->exp_status));
  }
}

static void format_exposure_time(int32_t exposure_us, char *buf,
                                 size_t buf_size) {
  if (exposure_us <= 0) {
    snprintf(buf, buf_size, "---");
    return;
  }
  if (exposure_us >= 1000000) {
    snprintf(buf, buf_size, "%.1fs", (double)exposure_us / 1000000.0);
  } else {
    int denom = (int)(1000000.0 / (double)exposure_us + 0.5);
    if (denom <= 1) {
      snprintf(buf, buf_size, "1.0s");
    } else {
      snprintf(buf, buf_size, "1/%ds", denom);
    }
  }
}

/* --------------------------------------------------------------------------
 * Image & Thumbnail Loading
 * -------------------------------------------------------------------------- */

static void reset_zoom_and_pan(PlaybackManager *pm) {
  if (!pm) {
    return;
  }
  pm->zoom_step_index = 0;
  pm->pan_center_x = 0.5f;
  pm->pan_center_y = 0.5f;
}

static void clamp_pan_center(PlaybackManager *pm) {
  if (!pm) {
    return;
  }
  float zoom = ZOOM_FACTORS[pm->zoom_step_index];
  if (zoom <= 1.0f) {
    pm->pan_center_x = 0.5f;
    pm->pan_center_y = 0.5f;
    return;
  }
  float half_span = 0.5f / zoom;
  if (pm->pan_center_x < half_span) {
    pm->pan_center_x = half_span;
  } else if (pm->pan_center_x > 1.0f - half_span) {
    pm->pan_center_x = 1.0f - half_span;
  }
  if (pm->pan_center_y < half_span) {
    pm->pan_center_y = half_span;
  } else if (pm->pan_center_y > 1.0f - half_span) {
    pm->pan_center_y = 1.0f - half_span;
  }
}

static bool load_current_photo(PlaybackManager *pm, SDL_Renderer *renderer) {
  if (!pm || !renderer) {
    return false;
  }

  free_current_texture(pm);
  reset_zoom_and_pan(pm);
  memset(&pm->current_meta, 0, sizeof(pm->current_meta));

  if (pm->current_index < 0 ||
      (size_t)pm->current_index >= pm->media_list.count) {
    return false;
  }

  DcfMediaEntry *entry = &pm->media_list.entries[pm->current_index];
  entry->is_protected = dcf_storage_is_protected(entry->filepath);

  parse_jpeg_metadata(pm, entry, &pm->current_meta);

  SDL_Surface *surf = IMG_Load(entry->filepath);
  if (!surf) {
    fprintf(stderr, "[エラー] 画像の読み込みに失敗しました (%s): %s\n",
            entry->filepath, IMG_GetError());
    return false;
  }

  if (pm->current_meta.width <= 0 || pm->current_meta.height <= 0) {
    pm->current_meta.width = surf->w;
    pm->current_meta.height = surf->h;
  }

  pm->current_texture = create_safe_texture_from_surface(
      renderer, surf, 0, 0, &pm->current_tex_w, &pm->current_tex_h);
  SDL_FreeSurface(surf);

  return pm->current_texture != NULL;
}

static int get_grid_cells_per_page(PlayViewMode view_mode) {
  if (view_mode == PLAY_VIEW_GRID_4X4) {
    return 16;
  }
  return 9; /* PLAY_VIEW_GRID_3X3 */
}

static int get_grid_columns(PlayViewMode view_mode) {
  if (view_mode == PLAY_VIEW_GRID_4X4) {
    return 4;
  }
  return 3; /* PLAY_VIEW_GRID_3X3 */
}

static void ensure_grid_page_loaded(PlaybackManager *pm,
                                    SDL_Renderer *renderer) {
  if (!pm || !renderer || pm->media_list.count == 0 || pm->current_index < 0) {
    return;
  }

  int per_page = get_grid_cells_per_page(pm->view_mode);
  int page = pm->current_index / per_page;

  if (pm->cached_grid_page == page && pm->cached_grid_mode == pm->view_mode) {
    return;
  }

  free_grid_textures(pm);
  pm->cached_grid_page = page;
  pm->cached_grid_mode = pm->view_mode;

  int start_idx = page * per_page;
  for (int i = 0; i < per_page; ++i) {
    int media_idx = start_idx + i;
    if ((size_t)media_idx >= pm->media_list.count) {
      break;
    }
    const char *path = pm->media_list.entries[media_idx].filepath;
    SDL_Surface *surf = IMG_Load(path);
    if (!surf) {
      continue;
    }
    pm->grid_textures[i] = create_safe_texture_from_surface(
        renderer, surf, 320, 200, &pm->grid_tex_w[i], &pm->grid_tex_h[i]);
    SDL_FreeSurface(surf);
  }
}

/* --------------------------------------------------------------------------
 * Public Lifecycle & State Transition Functions
 * -------------------------------------------------------------------------- */

void playback_manager_init(PlaybackManager *pm, DcfStorage *dcf) {
  if (!pm) {
    return;
  }
  memset(pm, 0, sizeof(*pm));
  pm->mode = APP_MODE_LIVE;
  pm->view_mode = PLAY_VIEW_SINGLE;
  pm->dcf = dcf;
  pm->save_queue = NULL;
  pm->quick_review_duration_ms = PLAYBACK_DEFAULT_QUICK_REVIEW_MS;
  pm->current_index = -1;
  pm->show_detail_info = true;
  pm->cached_grid_page = -1;
  reset_zoom_and_pan(pm);
}

void playback_manager_set_save_queue(PlaybackManager *pm,
                                     AsyncSaveQueue *save_queue) {
  if (!pm) {
    return;
  }
  pm->save_queue = save_queue;
}

void playback_manager_destroy(PlaybackManager *pm) {
  if (!pm) {
    return;
  }
  free_current_texture(pm);
  free_grid_textures(pm);
  dcf_storage_free_media_list(&pm->media_list);
}

AppMode playback_manager_get_mode(const PlaybackManager *pm) {
  return pm ? pm->mode : APP_MODE_LIVE;
}

void playback_manager_start_quick_review(
    PlaybackManager *pm, SDL_Renderer *renderer, const char *filepath,
    const uint8_t *raw_data, int width, int height, int stride, bool is_bgr,
    const CameraExposureStatus *exp_status) {
  if (!pm || !renderer || !filepath) {
    return;
  }

  record_capture_metadata(pm, filepath, width, height, exp_status);

  free_current_texture(pm);
  free_grid_textures(pm);

  snprintf(pm->quick_review_filepath, sizeof(pm->quick_review_filepath), "%s",
           filepath);
  pm->quick_review_protected = dcf_storage_is_protected(filepath);
  pm->quick_review_start_ms = SDL_GetTicks();

  memset(&pm->current_meta, 0, sizeof(pm->current_meta));
  pm->current_meta.width = width;
  pm->current_meta.height = height;
  if (exp_status) {
    pm->current_meta.exposure_us = exp_status->shutter_us_setting > 0
                                       ? exp_status->shutter_us_setting
                                       : exp_status->actual_exposure_us;
    pm->current_meta.iso_value =
        exp_status->iso_setting > 0
            ? exp_status->iso_setting
            : (int)(exp_status->actual_analogue_gain * 100.0f + 0.5f);
    pm->current_meta.ev_bias = exp_status->ev_compensation;
  }

  if (raw_data && width > 0 && height > 0) {
    int eff_stride = stride > 0 ? stride : (width * 3);
    Uint32 fmt = is_bgr ? SDL_PIXELFORMAT_BGR24 : SDL_PIXELFORMAT_RGB24;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
        (void *)raw_data, width, height, 24, eff_stride, fmt);
    if (surf) {
      pm->current_texture = create_safe_texture_from_surface(
          renderer, surf, 0, 0, &pm->current_tex_w, &pm->current_tex_h);
      SDL_FreeSurface(surf);
    }
  }

  /* Fallback to loading from disk if raw buffer was not provided */
  if (!pm->current_texture) {
    SDL_Surface *surf = IMG_Load(filepath);
    if (surf) {
      pm->current_texture = create_safe_texture_from_surface(
          renderer, surf, 0, 0, &pm->current_tex_w, &pm->current_tex_h);
      SDL_FreeSurface(surf);
    }
  }

  pm->mode = APP_MODE_QUICK_REVIEW;
}

void playback_manager_enter_playback(PlaybackManager *pm,
                                     SDL_Renderer *renderer,
                                     const char *focus_filepath) {
  if (!pm || !renderer) {
    return;
  }

  /* Ensure any in-flight background JPEG writes complete before scanning DCIM
   */
  if (pm->save_queue) {
    async_save_queue_wait_idle(pm->save_queue);
  }

  free_current_texture(pm);
  free_grid_textures(pm);
  dcf_storage_free_media_list(&pm->media_list);

  dcf_storage_scan_media(pm->dcf, &pm->media_list);

  pm->mode = APP_MODE_PLAYBACK;
  pm->view_mode = PLAY_VIEW_SINGLE;
  pm->delete_dialog_open = false;
  reset_zoom_and_pan(pm);

  if (pm->media_list.count == 0) {
    pm->current_index = -1;
    return;
  }

  /* Select focus_filepath if specified, otherwise select the latest image */
  pm->current_index = (int)pm->media_list.count - 1;
  if (focus_filepath && focus_filepath[0] != '\0') {
    for (size_t i = 0; i < pm->media_list.count; ++i) {
      if (strcmp(pm->media_list.entries[i].filepath, focus_filepath) == 0) {
        pm->current_index = (int)i;
        break;
      }
    }
  }

  load_current_photo(pm, renderer);
}

void playback_manager_return_to_live(PlaybackManager *pm) {
  if (!pm) {
    return;
  }
  free_current_texture(pm);
  free_grid_textures(pm);
  dcf_storage_free_media_list(&pm->media_list);
  pm->mode = APP_MODE_LIVE;
  pm->view_mode = PLAY_VIEW_SINGLE;
  pm->delete_dialog_open = false;
  reset_zoom_and_pan(pm);
}

/* --------------------------------------------------------------------------
 * Media Actions (Protect & Delete)
 * -------------------------------------------------------------------------- */

static void toggle_current_protection(PlaybackManager *pm) {
  if (!pm || pm->current_index < 0 ||
      (size_t)pm->current_index >= pm->media_list.count) {
    return;
  }

  DcfMediaEntry *entry = &pm->media_list.entries[pm->current_index];
  bool new_state = !entry->is_protected;
  if (dcf_storage_set_protected(entry->filepath, new_state)) {
    entry->is_protected = new_state;
    if (new_state) {
      set_toast(pm, "[PROTECTED] 画像を保護しました (誤削除防止 ON)", 2200);
      printf("[-] プロテクト ON: %s\n", entry->filepath);
    } else {
      set_toast(pm, "[UNPROTECTED] 画像の保護を解除しました", 2200);
      printf("[-] プロテクト OFF: %s\n", entry->filepath);
    }
  } else {
    set_toast(pm, "[エラー] プロテクト状態の変更に失敗しました", 2500);
  }
}

static void execute_delete_single(PlaybackManager *pm, SDL_Renderer *renderer) {
  if (!pm || pm->current_index < 0 ||
      (size_t)pm->current_index >= pm->media_list.count) {
    return;
  }

  DcfMediaEntry *entry = &pm->media_list.entries[pm->current_index];
  if (entry->is_protected || dcf_storage_is_protected(entry->filepath)) {
    entry->is_protected = true;
    set_toast(pm, "[警告] 保護された画像は削除できません ([L]キーで解除可能)",
              2800);
    return;
  }

  char deleted_name[256];
  snprintf(deleted_name, sizeof(deleted_name), "%s", entry->filename);

  if (!dcf_storage_delete_file(pm->dcf, entry->filepath)) {
    set_toast(pm, "[エラー] ファイルの削除に失敗しました", 2500);
    return;
  }

  printf("[-] 削除しました: %s\n", entry->filepath);

  int prev_idx = pm->current_index;
  free_current_texture(pm);
  free_grid_textures(pm);
  dcf_storage_free_media_list(&pm->media_list);
  dcf_storage_scan_media(pm->dcf, &pm->media_list);

  if (pm->media_list.count == 0) {
    pm->current_index = -1;
  } else if ((size_t)prev_idx >= pm->media_list.count) {
    pm->current_index = (int)pm->media_list.count - 1;
    load_current_photo(pm, renderer);
  } else {
    pm->current_index = prev_idx;
    load_current_photo(pm, renderer);
  }

  char msg[320];
  snprintf(msg, sizeof(msg), "削除しました: %s", deleted_name);
  set_toast(pm, msg, 2200);
}

static void execute_delete_all_unprotected(PlaybackManager *pm,
                                           SDL_Renderer *renderer) {
  if (!pm) {
    return;
  }

  int deleted = dcf_storage_delete_all_unprotected(pm->dcf);
  if (deleted < 0) {
    set_toast(pm, "[エラー] 全削除処理に失敗しました", 2500);
    return;
  }

  printf("[-] 未保護の画像を一括削除しました (%d 枚)\n", deleted);

  free_current_texture(pm);
  free_grid_textures(pm);
  dcf_storage_free_media_list(&pm->media_list);
  dcf_storage_scan_media(pm->dcf, &pm->media_list);

  if (pm->media_list.count == 0) {
    pm->current_index = -1;
  } else {
    pm->current_index = (int)pm->media_list.count - 1;
    load_current_photo(pm, renderer);
  }

  char msg[128];
  if (deleted == 0) {
    snprintf(msg, sizeof(msg),
             "削除対象がありません (全画像が保護されています)");
  } else {
    snprintf(msg, sizeof(msg), "未保護の画像を %d 枚削除しました (残り %zu 枚)",
             deleted, pm->media_list.count);
  }
  set_toast(pm, msg, 2800);
}

/* --------------------------------------------------------------------------
 * Keyboard Input & Per-Frame Update
 * -------------------------------------------------------------------------- */

bool playback_manager_handle_key(PlaybackManager *pm, SDL_Renderer *renderer,
                                 const SDL_KeyboardEvent *key_event) {
  if (!pm || !renderer || !key_event || key_event->type != SDL_KEYDOWN) {
    return false;
  }

  SDL_Keycode sym = key_event->keysym.sym;

  /* 1. Handle Quick Review mode keys */
  if (pm->mode == APP_MODE_QUICK_REVIEW) {
    switch (sym) {
    case SDLK_DELETE:
    case SDLK_BACKSPACE:
    case SDLK_d:
      if (pm->quick_review_protected) {
        set_toast(pm, "[警告] 保護された画像は削除できません", 2200);
      } else if (async_save_queue_cancel_or_delete(pm->save_queue, pm->dcf,
                                                   pm->quick_review_filepath)) {
        printf("[-] クイックレビュー: 直前の撮影画像を破棄しました (%s)\n",
               pm->quick_review_filepath);
        set_toast(pm, "直前の撮影画像を削除しました", 2200);
        playback_manager_return_to_live(pm);
      }
      return true;

    case SDLK_l: {
      bool next_prot = !pm->quick_review_protected;
      if (async_save_queue_set_protected(
              pm->save_queue, pm->quick_review_filepath, next_prot)) {
        pm->quick_review_protected = next_prot;
        /* Extend review timer slightly so user sees confirmation */
        pm->quick_review_start_ms = SDL_GetTicks();
        set_toast(pm,
                  next_prot ? "[PROTECTED] 直前の撮影画像を保護しました"
                            : "画像の保護を解除しました",
                  2000);
      }
      return true;
    }

    case SDLK_p:
      playback_manager_enter_playback(pm, renderer, pm->quick_review_filepath);
      return true;

    case SDLK_LEFT:
    case SDLK_RIGHT:
      playback_manager_enter_playback(pm, renderer, pm->quick_review_filepath);
      if (pm->media_list.count > 1) {
        int dir = (sym == SDLK_LEFT) ? -1 : 1;
        pm->current_index =
            (pm->current_index + dir + (int)pm->media_list.count) %
            (int)pm->media_list.count;
        load_current_photo(pm, renderer);
      }
      return true;

    case SDLK_z:
    case SDLK_PLUS:
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
      playback_manager_enter_playback(pm, renderer, pm->quick_review_filepath);
      pm->zoom_step_index = 1; /* 2.0x */
      return true;

    case SDLK_SPACE:
    case SDLK_ESCAPE:
    case SDLK_RETURN:
      playback_manager_return_to_live(pm);
      return true;

    default:
      return false;
    }
  }

  if (pm->mode != APP_MODE_PLAYBACK) {
    return false;
  }

  /* 2. Handle Delete Confirmation Modal Dialog if open */
  if (pm->delete_dialog_open) {
    switch (sym) {
    case SDLK_UP:
    case SDLK_LEFT:
      pm->delete_dialog_selection = (pm->delete_dialog_selection + 2) % 3;
      return true;

    case SDLK_DOWN:
    case SDLK_RIGHT:
    case SDLK_TAB:
      pm->delete_dialog_selection = (pm->delete_dialog_selection + 1) % 3;
      return true;

    case SDLK_1:
    case SDLK_y:
      pm->delete_dialog_open = false;
      execute_delete_single(pm, renderer);
      return true;

    case SDLK_2:
      pm->delete_dialog_open = false;
      execute_delete_all_unprotected(pm, renderer);
      return true;

    case SDLK_3:
    case SDLK_n:
    case SDLK_ESCAPE:
      pm->delete_dialog_open = false;
      return true;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
      pm->delete_dialog_open = false;
      if (pm->delete_dialog_selection == 0) {
        execute_delete_single(pm, renderer);
      } else if (pm->delete_dialog_selection == 1) {
        execute_delete_all_unprotected(pm, renderer);
      }
      return true;

    default:
      return true;
    }
  }

  /* 3. Mode exit keys (P or Space returns to Live Viewfinder) */
  if (sym == SDLK_p || sym == SDLK_SPACE) {
    playback_manager_return_to_live(pm);
    return true;
  }

  /* If DCIM has no images, Escape also returns to Live Viewfinder */
  if (pm->media_list.count == 0) {
    if (sym == SDLK_ESCAPE) {
      playback_manager_return_to_live(pm);
    }
    return true;
  }

  /* 4. Common Playback keys (Protect, Delete, Info, Grid toggle) */
  if (sym == SDLK_l) {
    toggle_current_protection(pm);
    return true;
  }

  if (sym == SDLK_DELETE || sym == SDLK_BACKSPACE || sym == SDLK_d) {
    pm->delete_dialog_open = true;
    pm->delete_dialog_selection = 0;
    return true;
  }

  if (sym == SDLK_i) {
    pm->show_detail_info = !pm->show_detail_info;
    return true;
  }

  if (sym == SDLK_g) {
    reset_zoom_and_pan(pm);
    if (pm->view_mode == PLAY_VIEW_SINGLE) {
      pm->view_mode = PLAY_VIEW_GRID_3X3;
    } else if (pm->view_mode == PLAY_VIEW_GRID_3X3) {
      pm->view_mode = PLAY_VIEW_GRID_4X4;
    } else {
      pm->view_mode = PLAY_VIEW_SINGLE;
      load_current_photo(pm, renderer);
    }
    return true;
  }

  /* 5. Grid Index View navigation */
  if (pm->view_mode != PLAY_VIEW_SINGLE) {
    int count = (int)pm->media_list.count;
    int cols = get_grid_columns(pm->view_mode);
    int per_page = get_grid_cells_per_page(pm->view_mode);

    switch (sym) {
    case SDLK_LEFT:
      pm->current_index = (pm->current_index - 1 + count) % count;
      return true;

    case SDLK_RIGHT:
      pm->current_index = (pm->current_index + 1) % count;
      return true;

    case SDLK_UP:
      if (pm->current_index - cols >= 0) {
        pm->current_index -= cols;
      } else {
        pm->current_index = 0;
      }
      return true;

    case SDLK_DOWN:
      if (pm->current_index + cols < count) {
        pm->current_index += cols;
      } else {
        pm->current_index = count - 1;
      }
      return true;

    case SDLK_PAGEUP:
      pm->current_index -= per_page;
      if (pm->current_index < 0) {
        pm->current_index = 0;
      }
      return true;

    case SDLK_PAGEDOWN:
      pm->current_index += per_page;
      if (pm->current_index >= count) {
        pm->current_index = count - 1;
      }
      return true;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_z:
    case SDLK_PLUS:
    case SDLK_EQUALS:
    case SDLK_KP_PLUS:
    case SDLK_ESCAPE:
      pm->view_mode = PLAY_VIEW_SINGLE;
      load_current_photo(pm, renderer);
      return true;

    case SDLK_MINUS:
    case SDLK_KP_MINUS:
      if (pm->view_mode == PLAY_VIEW_GRID_3X3) {
        pm->view_mode = PLAY_VIEW_GRID_4X4;
      }
      return true;

    default:
      return true;
    }
  }

  /* 6. Single Photo View: Zoom & Pan or Previous/Next Photo */
  int count = (int)pm->media_list.count;
  float pan_step = 0.08f / ZOOM_FACTORS[pm->zoom_step_index];

  switch (sym) {
  case SDLK_z:
    pm->zoom_step_index = (pm->zoom_step_index + 1) % ZOOM_STEP_COUNT;
    clamp_pan_center(pm);
    return true;

  case SDLK_PLUS:
  case SDLK_EQUALS:
  case SDLK_KP_PLUS:
    if (pm->zoom_step_index < ZOOM_STEP_COUNT - 1) {
      pm->zoom_step_index++;
      clamp_pan_center(pm);
    }
    return true;

  case SDLK_MINUS:
  case SDLK_KP_MINUS:
    if (pm->zoom_step_index > 0) {
      pm->zoom_step_index--;
      clamp_pan_center(pm);
    } else {
      /* Zooming out from 1.0x switches to 3x3 Thumbnail Grid */
      pm->view_mode = PLAY_VIEW_GRID_3X3;
    }
    return true;

  case SDLK_0:
    reset_zoom_and_pan(pm);
    return true;

  case SDLK_ESCAPE:
    if (pm->zoom_step_index > 0) {
      reset_zoom_and_pan(pm);
    } else {
      playback_manager_return_to_live(pm);
    }
    return true;

  case SDLK_LEFT:
    if (pm->zoom_step_index > 0) {
      pm->pan_center_x -= pan_step;
      clamp_pan_center(pm);
    } else if (count > 0) {
      pm->current_index = (pm->current_index - 1 + count) % count;
      load_current_photo(pm, renderer);
    }
    return true;

  case SDLK_RIGHT:
    if (pm->zoom_step_index > 0) {
      pm->pan_center_x += pan_step;
      clamp_pan_center(pm);
    } else if (count > 0) {
      pm->current_index = (pm->current_index + 1) % count;
      load_current_photo(pm, renderer);
    }
    return true;

  case SDLK_UP:
    if (pm->zoom_step_index > 0) {
      pm->pan_center_y -= pan_step;
      clamp_pan_center(pm);
    }
    return true;

  case SDLK_DOWN:
    if (pm->zoom_step_index > 0) {
      pm->pan_center_y += pan_step;
      clamp_pan_center(pm);
    }
    return true;

  default:
    return false;
  }
}

void playback_manager_update(PlaybackManager *pm, SDL_Renderer *renderer) {
  (void)renderer;
  if (!pm) {
    return;
  }

  if (pm->mode == APP_MODE_QUICK_REVIEW) {
    uint32_t elapsed = SDL_GetTicks() - pm->quick_review_start_ms;
    if (elapsed >= pm->quick_review_duration_ms) {
      playback_manager_return_to_live(pm);
    }
  }
}

/* --------------------------------------------------------------------------
 * Rendering Helpers
 * -------------------------------------------------------------------------- */

static void render_zoom_pip_map(SDL_Renderer *renderer, int win_w, int win_h,
                                const PlaybackManager *pm) {
  if (!renderer || !pm || pm->zoom_step_index <= 0) {
    return;
  }

  float zoom = ZOOM_FACTORS[pm->zoom_step_index];
  int map_w = 140;
  int map_h = 84;
  if (pm->current_meta.width > 0 && pm->current_meta.height > 0) {
    map_h = (int)((float)map_w * (float)pm->current_meta.height /
                  (float)pm->current_meta.width);
    if (map_h < 50) {
      map_h = 50;
    } else if (map_h > 120) {
      map_h = 120;
    }
  }

  SDL_Rect outer = {win_w - map_w - 16, win_h - map_h - 64, map_w, map_h};
  SDL_Color bg = {0, 0, 0, 180};
  SDL_Color border = {200, 200, 200, 180};
  text_renderer_draw_panel(renderer, &outer, bg, border);

  /* Compute normalized visible window inside [0..1] */
  float span = 1.0f / zoom;
  float x0 = pm->pan_center_x - span * 0.5f;
  float y0 = pm->pan_center_y - span * 0.5f;
  if (x0 < 0.0f) {
    x0 = 0.0f;
  }
  if (y0 < 0.0f) {
    y0 = 0.0f;
  }

  SDL_Rect inner = {outer.x + (int)(x0 * (float)outer.w),
                    outer.y + (int)(y0 * (float)outer.h),
                    (int)(span * (float)outer.w), (int)(span * (float)outer.h)};
  if (inner.w < 6) {
    inner.w = 6;
  }
  if (inner.h < 6) {
    inner.h = 6;
  }

  SDL_SetRenderDrawColor(renderer, 255, 215, 0, 240);
  SDL_RenderDrawRect(renderer, &inner);

  char zoom_label[32];
  snprintf(zoom_label, sizeof(zoom_label), "ZOOM %.0fx", zoom);
  SDL_Color yellow = {255, 225, 60, 255};
  text_renderer_draw_text(renderer, zoom_label, outer.x + 6, outer.y + 4,
                          yellow, NULL, NULL);
}

static void render_single_view(PlaybackManager *pm, SDL_Renderer *renderer,
                               int win_w, int win_h) {
  SDL_Rect screen_bounds = {0, 0, win_w, win_h};
  SDL_Rect viewport_rect;
  calculate_aspect_fit_rect(pm->current_tex_w, pm->current_tex_h,
                            &screen_bounds, &viewport_rect);

  if (pm->current_texture && pm->current_tex_w > 0 && pm->current_tex_h > 0) {
    float zoom = ZOOM_FACTORS[pm->zoom_step_index];
    int src_w = (int)((float)pm->current_tex_w / zoom);
    int src_h = (int)((float)pm->current_tex_h / zoom);
    if (src_w < 1) {
      src_w = 1;
    }
    if (src_h < 1) {
      src_h = 1;
    }

    int src_x = (int)(pm->pan_center_x * (float)pm->current_tex_w) - src_w / 2;
    int src_y = (int)(pm->pan_center_y * (float)pm->current_tex_h) - src_h / 2;
    if (src_x < 0) {
      src_x = 0;
    } else if (src_x + src_w > pm->current_tex_w) {
      src_x = pm->current_tex_w - src_w;
    }
    if (src_y < 0) {
      src_y = 0;
    } else if (src_y + src_h > pm->current_tex_h) {
      src_y = pm->current_tex_h - src_h;
    }

    SDL_Rect src_rect = {src_x, src_y, src_w, src_h};
    SDL_RenderCopy(renderer, pm->current_texture, &src_rect, &viewport_rect);
  }

  const DcfMediaEntry *entry = &pm->media_list.entries[pm->current_index];

  /* Top guide & status bar */
  const char *top_guide =
      pm->zoom_step_index > 0
          ? "[PLAY ZOOM] [Arrows]:Pan  [Z/+/-]:Zoom  [0/Esc]:1x  [P/Space]:Live"
          : "[PLAY] [Left/Rgt]:Prev/Next  [Z/+]:Zoom  [G/-]:Grid  [L]:Lock  "
            "[Del]:Delete  [I]:Info  [P]:Live";
  SDL_Color white = {255, 255, 255, 255};
  SDL_Color panel_bg = {0, 0, 0, 185};
  SDL_Color panel_border = {255, 255, 255, 70};
  text_renderer_draw_badge(renderer, top_guide, 12, 12, white, panel_bg,
                           panel_border, NULL);

  /* Protection Lock badge at top-right */
  if (entry->is_protected) {
    SDL_Color lock_fg = {255, 230, 80, 255};
    SDL_Color lock_bg = {90, 60, 0, 210};
    SDL_Color lock_bd = {255, 200, 40, 220};
    int tw = 0;
    text_renderer_measure_text("[LOCKED / PROTECTED]", &tw, NULL);
    text_renderer_draw_badge(renderer, "[LOCKED / PROTECTED]", win_w - tw - 38,
                             12, lock_fg, lock_bg, lock_bd, NULL);
  }

  /* Detailed Exif / Metadata Panel (top-left under guide when enabled) */
  if (pm->show_detail_info) {
    char line1[640];
    char line2[256];
    char line3[256];

    double mib = (double)entry->file_size / (1024.0 * 1024.0);
    double mp =
        ((double)pm->current_meta.width * (double)pm->current_meta.height) /
        1000000.0;

    snprintf(line1, sizeof(line1), "[%d / %zu]  %s   (%s)%s%s",
             pm->current_index + 1, pm->media_list.count, entry->display_name,
             entry->is_protected ? "Protected" : "Normal",
             pm->current_meta.camera_model[0] != '\0' ? "   |   Model: " : "",
             pm->current_meta.camera_model[0] != '\0'
                 ? pm->current_meta.camera_model
                 : "");

    snprintf(
        line2, sizeof(line2),
        "Resolution: %d x %d (%.1f MP)   |   Size: %.2f MiB   |   Date: %s",
        pm->current_meta.width, pm->current_meta.height, mp, mib,
        pm->current_meta.datetime_str[0] != '\0' ? pm->current_meta.datetime_str
                                                 : "---");

    char ss_buf[32];
    format_exposure_time(pm->current_meta.exposure_us, ss_buf, sizeof(ss_buf));
    char iso_buf[32];
    if (pm->current_meta.iso_value > 0) {
      snprintf(iso_buf, sizeof(iso_buf), "%d", pm->current_meta.iso_value);
    } else {
      snprintf(iso_buf, sizeof(iso_buf), "---");
    }

    char optics_buf[64] = "";
    if (pm->current_meta.f_number > 0.0f &&
        pm->current_meta.focal_length_mm > 0.0f) {
      snprintf(optics_buf, sizeof(optics_buf), "   |   F%.1f  %.2fmm",
               pm->current_meta.f_number, pm->current_meta.focal_length_mm);
    }
    const char *wb_label = pm->current_meta.wb_mode == 1 ? "Manual" : "Auto";
    if (pm->current_meta.light_source == 1) {
      wb_label = "Daylight";
    } else if (pm->current_meta.light_source == 2) {
      wb_label = "Fluoresc";
    } else if (pm->current_meta.light_source == 3) {
      wb_label = "Tungsten";
    } else if (pm->current_meta.light_source == 10) {
      wb_label = "Cloudy";
    } else if (pm->current_meta.light_source == 11) {
      wb_label = "Shade";
    }

    const char *met_label = "Matrix";
    if (pm->current_meta.metering_mode == 2) {
      met_label = "Center";
    } else if (pm->current_meta.metering_mode == 3) {
      met_label = "Spot";
    }

    snprintf(line3, sizeof(line3),
             "Exif: SS %s  |  ISO %s  |  EV %+.1f%s  |  WB %s  |  Met %s  |  "
             "Zoom %.0fx",
             ss_buf, iso_buf, pm->current_meta.ev_bias, optics_buf, wb_label,
             met_label, ZOOM_FACTORS[pm->zoom_step_index]);

    int w1 = 0, w2 = 0, w3 = 0, lh = 20;
    text_renderer_measure_text(line1, &w1, &lh);
    text_renderer_measure_text(line2, &w2, NULL);
    text_renderer_measure_text(line3, &w3, NULL);
    int max_w = w1 > w2 ? w1 : w2;
    if (w3 > max_w) {
      max_w = w3;
    }

    int pad_x = 14;
    int pad_y = 8;
    int line_gap = 4;
    int box_h = lh * 3 + line_gap * 2 + pad_y * 2;
    SDL_Rect info_box = {12, win_h - box_h - 12, max_w + pad_x * 2, box_h};
    text_renderer_draw_panel(renderer, &info_box, panel_bg, panel_border);

    SDL_Color cyan = {130, 220, 255, 255};
    SDL_Color light_gray = {225, 225, 225, 255};
    text_renderer_draw_text(renderer, line1, info_box.x + pad_x,
                            info_box.y + pad_y, cyan, NULL, NULL);
    text_renderer_draw_text(renderer, line2, info_box.x + pad_x,
                            info_box.y + pad_y + lh + line_gap, white, NULL,
                            NULL);
    text_renderer_draw_text(renderer, line3, info_box.x + pad_x,
                            info_box.y + pad_y + (lh + line_gap) * 2,
                            light_gray, NULL, NULL);
  } else {
    char compact[640];
    snprintf(compact, sizeof(compact), "[%d/%zu] %s  (%.0fx)",
             pm->current_index + 1, pm->media_list.count, entry->display_name,
             ZOOM_FACTORS[pm->zoom_step_index]);
    text_renderer_draw_badge(renderer, compact, 12, win_h - 44, white, panel_bg,
                             panel_border, NULL);
  }

  /* Render PiP navigation map when zoomed in */
  render_zoom_pip_map(renderer, win_w, win_h, pm);
}

static void render_grid_view(PlaybackManager *pm, SDL_Renderer *renderer,
                             int win_w, int win_h) {
  ensure_grid_page_loaded(pm, renderer);

  int cols = get_grid_columns(pm->view_mode);
  int rows = cols;
  int per_page = get_grid_cells_per_page(pm->view_mode);
  int total_pages = ((int)pm->media_list.count + per_page - 1) / per_page;
  int cur_page = pm->current_index / per_page;
  int start_idx = cur_page * per_page;

  /* Top guide bar */
  char header[160];
  snprintf(
      header, sizeof(header),
      "[INDEX %dx%d] Page %d/%d (%zu photos)   [Arrows]:Move  [Enter/Z]:Open  "
      "[G]:Grid Size  [L]:Lock  [Del]:Delete  [P]:Live",
      cols, rows, cur_page + 1, total_pages, pm->media_list.count);
  SDL_Color white = {255, 255, 255, 255};
  SDL_Color panel_bg = {0, 0, 0, 190};
  SDL_Color panel_border = {255, 255, 255, 70};
  text_renderer_draw_badge(renderer, header, 12, 12, white, panel_bg,
                           panel_border, NULL);

  int margin_x = 20;
  int top_y = 54;
  int bottom_y = win_h - 46;
  int avail_w = win_w - margin_x * 2;
  int avail_h = bottom_y - top_y;
  int gap = 10;

  int cell_w = (avail_w - gap * (cols - 1)) / cols;
  int cell_h = (avail_h - gap * (rows - 1)) / rows;
  if (cell_w < 32 || cell_h < 32) {
    return;
  }

  for (int i = 0; i < per_page; ++i) {
    int media_idx = start_idx + i;
    if ((size_t)media_idx >= pm->media_list.count) {
      break;
    }

    int r = i / cols;
    int c = i % cols;
    SDL_Rect cell_rect = {margin_x + c * (cell_w + gap),
                          top_y + r * (cell_h + gap), cell_w, cell_h};

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 28, 28, 34, 255);
    SDL_RenderFillRect(renderer, &cell_rect);

    if (pm->grid_textures[i]) {
      SDL_Rect inner_bounds = {cell_rect.x + 4, cell_rect.y + 4,
                               cell_rect.w - 8, cell_rect.h - 8};
      SDL_Rect fit_rect;
      calculate_aspect_fit_rect(pm->grid_tex_w[i], pm->grid_tex_h[i],
                                &inner_bounds, &fit_rect);
      SDL_RenderCopy(renderer, pm->grid_textures[i], NULL, &fit_rect);
    }

    const DcfMediaEntry *entry = &pm->media_list.entries[media_idx];

    /* Label strip at bottom of cell */
    SDL_Rect strip = {cell_rect.x, cell_rect.y + cell_rect.h - 24, cell_rect.w,
                      24};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    SDL_RenderFillRect(renderer, &strip);

    char cell_label[300];
    snprintf(cell_label, sizeof(cell_label), "#%d %s", media_idx + 1,
             entry->filename);
    text_renderer_draw_text(renderer, cell_label, cell_rect.x + 6,
                            cell_rect.y + cell_rect.h - 22, white, NULL, NULL);

    /* Protected badge on thumbnail */
    if (entry->is_protected) {
      SDL_Color lock_fg = {255, 225, 60, 255};
      SDL_Color lock_bg = {80, 50, 0, 210};
      SDL_Color lock_bd = {255, 200, 40, 220};
      text_renderer_draw_badge(renderer, "LOCK", cell_rect.x + 6,
                               cell_rect.y + 6, lock_fg, lock_bg, lock_bd,
                               NULL);
    }

    /* Highlight border for selected thumbnail */
    if (media_idx == pm->current_index) {
      SDL_SetRenderDrawColor(renderer, 0, 220, 255, 255);
      for (int b = 0; b < 3; ++b) {
        SDL_Rect sel_rect = {cell_rect.x - b, cell_rect.y - b,
                             cell_rect.w + b * 2, cell_rect.h + b * 2};
        SDL_RenderDrawRect(renderer, &sel_rect);
      }
    } else {
      SDL_SetRenderDrawColor(renderer, 90, 90, 100, 200);
      SDL_RenderDrawRect(renderer, &cell_rect);
    }
  }

  /* Bottom selected file summary */
  if (pm->current_index >= 0 &&
      (size_t)pm->current_index < pm->media_list.count) {
    const DcfMediaEntry *sel = &pm->media_list.entries[pm->current_index];
    char footer[640];
    snprintf(footer, sizeof(footer), "Selected [%d/%zu]: %s   (%.2f MiB)%s",
             pm->current_index + 1, pm->media_list.count, sel->display_name,
             (double)sel->file_size / (1024.0 * 1024.0),
             sel->is_protected ? "   [PROTECTED]" : "");
    text_renderer_draw_badge(renderer, footer, 12, win_h - 38, white, panel_bg,
                             panel_border, NULL);
  }
}

static void render_delete_dialog(const PlaybackManager *pm,
                                 SDL_Renderer *renderer, int win_w, int win_h) {
  if (!pm || !pm->delete_dialog_open || pm->current_index < 0 ||
      (size_t)pm->current_index >= pm->media_list.count) {
    return;
  }

  /* Dim full screen behind modal */
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
  SDL_Rect full = {0, 0, win_w, win_h};
  SDL_RenderFillRect(renderer, &full);

  int dlg_w = 500;
  int dlg_h = 230;
  SDL_Rect dlg = {(win_w - dlg_w) / 2, (win_h - dlg_h) / 2, dlg_w, dlg_h};
  SDL_Color bg = {22, 24, 30, 245};
  SDL_Color border = {255, 110, 110, 230};
  text_renderer_draw_panel(renderer, &dlg, bg, border);

  const DcfMediaEntry *entry = &pm->media_list.entries[pm->current_index];
  char title[640];
  snprintf(title, sizeof(title), "Delete Photo Confirmation (%s)",
           entry->display_name);

  SDL_Color title_col = {255, 150, 150, 255};
  text_renderer_draw_text(renderer, title, dlg.x + 20, dlg.y + 18, title_col,
                          NULL, NULL);

  const char *options[3] = {
      "[1] Delete Current Photo (1枚削除)",
      "[2] Delete All Unprotected Photos (未保護の全写真を削除)",
      "[3] Cancel (キャンセル)"};

  for (int i = 0; i < 3; ++i) {
    SDL_Rect row = {dlg.x + 20, dlg.y + 62 + i * 42, dlg.w - 40, 34};
    bool selected = (pm->delete_dialog_selection == i);
    SDL_Color row_bg =
        selected ? (SDL_Color){180, 45, 45, 230} : (SDL_Color){45, 48, 58, 220};
    SDL_Color row_bd = selected ? (SDL_Color){255, 220, 120, 255}
                                : (SDL_Color){90, 95, 110, 180};
    text_renderer_draw_panel(renderer, &row, row_bg, row_bd);

    SDL_Color txt_col = {255, 255, 255, 255};
    text_renderer_draw_text(renderer, options[i], row.x + 12, row.y + 7,
                            txt_col, NULL, NULL);
  }

  SDL_Color hint_col = {190, 195, 205, 255};
  text_renderer_draw_text(renderer,
                          "[Up/Down]:Select   [Enter]:Confirm   [Esc]:Cancel",
                          dlg.x + 20, dlg.y + dlg.h - 28, hint_col, NULL, NULL);
}

void playback_manager_render(PlaybackManager *pm, SDL_Renderer *renderer,
                             int win_w, int win_h) {
  if (!pm || !renderer) {
    return;
  }

  SDL_SetRenderDrawColor(renderer, 10, 10, 12, 255);
  SDL_RenderClear(renderer);

  if (pm->mode == APP_MODE_QUICK_REVIEW) {
    SDL_Rect screen_bounds = {0, 0, win_w, win_h};
    SDL_Rect viewport_rect;
    calculate_aspect_fit_rect(pm->current_tex_w, pm->current_tex_h,
                              &screen_bounds, &viewport_rect);
    if (pm->current_texture) {
      SDL_RenderCopy(renderer, pm->current_texture, NULL, &viewport_rect);
    }

    uint32_t elapsed = SDL_GetTicks() - pm->quick_review_start_ms;
    uint32_t remain_ms = elapsed < pm->quick_review_duration_ms
                             ? (pm->quick_review_duration_ms - elapsed)
                             : 0;

    char banner[640];
    snprintf(banner, sizeof(banner), "[QUICK REVIEW %.1fs] Saved: %s (%dx%d)",
             (double)remain_ms / 1000.0, pm->quick_review_filepath,
             pm->current_meta.width, pm->current_meta.height);

    SDL_Color green = {120, 255, 150, 255};
    SDL_Color bg = {0, 0, 0, 190};
    SDL_Color border = {120, 255, 150, 160};
    text_renderer_draw_badge(renderer, banner, 12, 12, green, bg, border, NULL);

    if (pm->quick_review_protected) {
      SDL_Color lock_fg = {255, 230, 80, 255};
      SDL_Color lock_bg = {90, 60, 0, 210};
      SDL_Color lock_bd = {255, 200, 40, 220};
      int tw = 0;
      text_renderer_measure_text("[PROTECTED]", &tw, NULL);
      text_renderer_draw_badge(renderer, "[PROTECTED]", win_w - tw - 38, 12,
                               lock_fg, lock_bg, lock_bd, NULL);
    }

    const char *bottom_guide =
        "[Del / BS]: 即削除 (Discard)   |   [L]: 保護 (Protect)   |   "
        "[P]: 再生モードへ   |   [Space]: 撮影へ戻る";
    SDL_Color white = {255, 255, 255, 255};
    SDL_Color bot_border = {255, 255, 255, 80};
    text_renderer_draw_badge(renderer, bottom_guide, 12, win_h - 44, white, bg,
                             bot_border, NULL);

    playback_manager_render_toast(pm, renderer, win_w, win_h);
    return;
  }

  /* APP_MODE_PLAYBACK */
  if (pm->media_list.count == 0 || pm->current_index < 0) {
    SDL_Color white = {255, 255, 255, 255};
    SDL_Color bg = {30, 32, 40, 220};
    SDL_Color bd = {120, 130, 150, 180};
    const char *empty_msg = "保存された写真がありません (No Images in DCIM)  — "
                            " [P / Space]: 撮影モードへ戻る";
    int tw = 0, th = 0;
    text_renderer_measure_text(empty_msg, &tw, &th);
    text_renderer_draw_badge(renderer, empty_msg, (win_w - tw - 24) / 2,
                             (win_h - th - 12) / 2, white, bg, bd, NULL);
    playback_manager_render_toast(pm, renderer, win_w, win_h);
    return;
  }

  if (pm->view_mode == PLAY_VIEW_SINGLE) {
    render_single_view(pm, renderer, win_w, win_h);
  } else {
    render_grid_view(pm, renderer, win_w, win_h);
  }

  if (pm->delete_dialog_open) {
    render_delete_dialog(pm, renderer, win_w, win_h);
  }

  playback_manager_render_toast(pm, renderer, win_w, win_h);
}

void playback_manager_render_toast(PlaybackManager *pm, SDL_Renderer *renderer,
                                   int win_w, int win_h) {
  (void)win_h;
  if (!pm || !renderer || pm->toast_message[0] == '\0') {
    return;
  }

  uint32_t now = SDL_GetTicks();
  if (now >= pm->toast_expire_ms) {
    pm->toast_message[0] = '\0';
    return;
  }

  int tw = 0;
  int th = 0;
  text_renderer_measure_text(pm->toast_message, &tw, &th);
  int x = (win_w - (tw + 28)) / 2;
  if (x < 12) {
    x = 12;
  }

  SDL_Color fg = {255, 245, 190, 255};
  SDL_Color bg = {35, 30, 10, 230};
  SDL_Color bd = {255, 210, 70, 230};
  text_renderer_draw_badge(renderer, pm->toast_message, x, 52, fg, bg, bd,
                           NULL);
}
