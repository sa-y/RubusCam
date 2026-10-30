#ifndef PLAYBACK_MANAGER_H
#define PLAYBACK_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>

#include "async_save_queue.h"
#include "camera_stream.h"
#include "dcf_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Default Quick Review display duration in milliseconds (2.0 seconds).
 */
#define PLAYBACK_DEFAULT_QUICK_REVIEW_MS 2000U

/*
 * Maximum number of thumbnails on a single grid page (4x4 = 16).
 */
#define PLAYBACK_MAX_GRID_CELLS 16

/*
 * Maximum number of in-session capture metadata records cached for display.
 */
#define PLAYBACK_META_CACHE_SIZE 64

/*
 * Top-level application operating mode.
 */
typedef enum {
  APP_MODE_LIVE = 0,         /* Live camera viewfinder & shooting mode */
  APP_MODE_QUICK_REVIEW = 1, /* Auto review immediately after capture */
  APP_MODE_PLAYBACK = 2      /* Standalone gallery / playback mode */
} AppMode;

/*
 * Sub-view mode inside APP_MODE_PLAYBACK.
 */
typedef enum {
  PLAY_VIEW_SINGLE = 0,   /* Single photo view (supports 1x/2x/4x/8x zoom & pan) */
  PLAY_VIEW_GRID_3X3 = 1, /* 3x3 thumbnail index view (9 photos per page) */
  PLAY_VIEW_GRID_4X4 = 2  /* 4x4 thumbnail index view (16 photos per page) */
} PlayViewMode;

/*
 * Parsed Exif / image metadata for the currently displayed photo.
 */
typedef struct {
  int width;
  int height;
  bool has_exif;
  char camera_model[64];
  char datetime_str[32];
  int32_t exposure_us;
  int iso_value;
  float ev_bias;
  float f_number;
  float focal_length_mm;
  uint16_t wb_mode;
} PhotoMetadata;

/*
 * In-session record of capture exposure parameters (used when Exif APP1 is
 * not embedded in the JPEG file).
 */
typedef struct {
  char filepath[512];
  int width;
  int height;
  CameraExposureStatus exp_status;
} CaptureMetaRecord;

/*
 * Playback, Quick Review, and Media Management controller state.
 */
typedef struct {
  AppMode mode;
  PlayViewMode view_mode;
  DcfStorage *dcf;
  AsyncSaveQueue *save_queue;

  /* Quick Review state */
  uint32_t quick_review_start_ms;
  uint32_t quick_review_duration_ms;
  char quick_review_filepath[512];
  bool quick_review_protected;

  /* Media list & current selection */
  DcfMediaList media_list;
  int current_index; /* 0 .. media_list.count - 1, or -1 if empty */

  /* Active full/display texture for single view & quick review */
  SDL_Texture *current_texture;
  int current_tex_w;
  int current_tex_h;
  PhotoMetadata current_meta;
  bool show_detail_info;

  /* Zoom & pan state in PLAY_VIEW_SINGLE */
  int zoom_step_index; /* 0=1.0x, 1=2.0x, 2=4.0x, 3=8.0x */
  float pan_center_x;  /* Normalized center X (0.0 .. 1.0, default 0.5) */
  float pan_center_y;  /* Normalized center Y (0.0 .. 1.0, default 0.5) */

  /* Thumbnail grid cache for current page */
  int cached_grid_page;
  PlayViewMode cached_grid_mode;
  SDL_Texture *grid_textures[PLAYBACK_MAX_GRID_CELLS];
  int grid_tex_w[PLAYBACK_MAX_GRID_CELLS];
  int grid_tex_h[PLAYBACK_MAX_GRID_CELLS];

  /* Delete confirmation modal dialog state */
  bool delete_dialog_open;
  int delete_dialog_selection; /* 0=Delete Single, 1=Delete All Unprotected, 2=Cancel */

  /* Toast notification banner (rendered in both Live and Playback modes) */
  char toast_message[320];
  uint32_t toast_expire_ms;

  /* Ring buffer of recent capture exposure metadata */
  CaptureMetaRecord meta_cache[PLAYBACK_META_CACHE_SIZE];
  size_t meta_cache_next;
} PlaybackManager;

/*
 * Initialize the playback manager and associate it with DCF storage.
 */
void playback_manager_init(PlaybackManager *pm, DcfStorage *dcf);

/*
 * Associate an asynchronous background save queue with the playback manager
 * so Quick Review discard/protect and Playback gallery entry synchronize with
 * in-flight background writes.
 */
void playback_manager_set_save_queue(PlaybackManager *pm,
                                     AsyncSaveQueue *save_queue);

/*
 * Release all textures and media list memory owned by the playback manager.
 */
void playback_manager_destroy(PlaybackManager *pm);

/*
 * Query current application mode (APP_MODE_LIVE, APP_MODE_QUICK_REVIEW, or
 * APP_MODE_PLAYBACK).
 */
AppMode playback_manager_get_mode(const PlaybackManager *pm);

/*
 * Trigger Quick Review immediately after capturing a still image.
 * Creates the display texture directly from the in-memory RGB/BGR frame buffer
 * to avoid re-reading the JPEG from disk.
 */
void playback_manager_start_quick_review(
    PlaybackManager *pm, SDL_Renderer *renderer, const char *filepath,
    const uint8_t *raw_data, int width, int height, int stride, bool is_bgr,
    const CameraExposureStatus *exp_status);

/*
 * Enter Playback (Gallery) mode, scanning DCIM storage.
 * If focus_filepath is non-NULL and exists in the scanned list, selects it;
 * otherwise selects the newest photo.
 */
void playback_manager_enter_playback(PlaybackManager *pm,
                                     SDL_Renderer *renderer,
                                     const char *focus_filepath);

/*
 * Return to live camera viewfinder mode (APP_MODE_LIVE) and free playback
 * textures.
 */
void playback_manager_return_to_live(PlaybackManager *pm);

/*
 * Process keyboard input when in APP_MODE_QUICK_REVIEW or APP_MODE_PLAYBACK.
 * Returns true if the event was consumed by the playback manager.
 */
bool playback_manager_handle_key(PlaybackManager *pm, SDL_Renderer *renderer,
                                 const SDL_KeyboardEvent *key_event);

/*
 * Per-frame update (handles Quick Review timer expiration and continuous arrow
 * key panning while zoomed in).
 */
void playback_manager_update(PlaybackManager *pm, SDL_Renderer *renderer);

/*
 * Render Quick Review or Playback mode (single view, zoom/pan with PiP map,
 * 3x3/4x4 thumbnail grid, metadata HUD, delete confirmation dialog).
 */
void playback_manager_render(PlaybackManager *pm, SDL_Renderer *renderer,
                             int win_w, int win_h);

/*
 * Render active toast notification overlay (can be called in APP_MODE_LIVE as
 * well so post-delete messages remain visible when returning to live view).
 */
void playback_manager_render_toast(PlaybackManager *pm, SDL_Renderer *renderer,
                                   int win_w, int win_h);

#ifdef __cplusplus
}
#endif

#endif /* PLAYBACK_MANAGER_H */

