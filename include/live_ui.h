#ifndef LIVE_UI_H
#define LIVE_UI_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>

#include "async_save_queue.h"
#include "camera_stream.h"
#include "playback_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Active popup selector type in Live View mode.
 */
typedef enum LivePopupType {
  LIVE_POPUP_NONE = 0,
  LIVE_POPUP_SS = 1,
  LIVE_POPUP_ISO = 2,
  LIVE_POPUP_EV = 3,
  LIVE_POPUP_WB = 4,
  LIVE_POPUP_TEMP = 5,
  LIVE_POPUP_MET = 6,
  LIVE_POPUP_QUAL = 7,
  LIVE_POPUP_GRID = 8,
  LIVE_POPUP_ASPECT = 9,
  LIVE_POPUP_DRIVE = 10
} LivePopupType;

/*
 * Drive & special shooting modes (Phase 4).
 */
typedef enum LiveDriveMode {
  LIVE_DRIVE_SINGLE = 0,    /* Single shot (default) */
  LIVE_DRIVE_TIMER_2S = 1,  /* 2-second self-timer */
  LIVE_DRIVE_TIMER_10S = 2, /* 10-second self-timer */
  LIVE_DRIVE_BURST = 3,     /* Continuous burst shooting while held */
  LIVE_DRIVE_INTERVAL = 4,  /* Intervalometer (auto time-lapse stills) */
  LIVE_DRIVE_AEB = 5        /* Auto Exposure Bracketing (3 shots: 0, -, +) */
} LiveDriveMode;

/*
 * Composition guide grid modes for the Live View viewfinder.
 */
typedef enum LiveGridMode {
  LIVE_GRID_OFF = 0,          /* Grid hidden (default) */
  LIVE_GRID_THIRDS = 1,       /* Rule of Thirds (3x3) grid */
  LIVE_GRID_GOLDEN = 2,       /* Golden Ratio (Phi ~ 1:0.618:1) grid */
  LIVE_GRID_CROSSHAIR = 3,    /* Center crosshair reticle */
  LIVE_GRID_THIRDS_CROSS = 4  /* Rule of Thirds + Center crosshair */
} LiveGridMode;

/*
 * Aspect ratio mask & crop presets for Live View and still capture.
 */
typedef enum LiveAspectMask {
  LIVE_ASPECT_NATIVE = 0, /* Full sensor native aspect ratio (no mask/crop) */
  LIVE_ASPECT_16_9 = 1,   /* 16:9 widescreen */
  LIVE_ASPECT_4_3 = 2,    /* 4:3 standard */
  LIVE_ASPECT_3_2 = 3,    /* 3:2 35mm classic */
  LIVE_ASPECT_1_1 = 4     /* 1:1 square */
} LiveAspectMask;

/*
 * Live View UI, exposure/WB/metering/quality controls, and touch/mouse state.
 */
typedef struct LiveUiController {
  /* Exposure, WB, Metering, JPEG quality, Grid & Aspect state indices */
  int ev_step_index; /* -9 (-3.0 EV) to +9 (+3.0 EV) in 1/3 EV increments */
  int iso_preset_index;
  int shutter_preset_index;
  int wb_preset_index;
  int wb_kelvin_value;
  bool awb_locked;
  int metering_preset_index;
  int quality_preset_index;
  int grid_mode_index;
  int aspect_mask_index;
  int drive_mode_index;
  int interval_sec_index;
  int interval_shots_index;
  int aeb_step_index;

  /* Self-timer runtime state */
  bool timer_active;
  uint32_t timer_start_ms;
  uint32_t timer_duration_ms;
  uint32_t timer_last_beep_ms;

  /* Burst shooting runtime state */
  bool burst_active;
  bool burst_held_by_key;
  bool burst_held_by_pointer;
  int burst_shot_count;

  /* Interval shooting runtime state */
  bool interval_active;
  int interval_shots_taken;
  int interval_target_shots;
  uint32_t interval_step_ms;
  uint32_t interval_next_shot_ms;
  bool interval_waiting_capture;

  /* Auto Exposure Bracketing (AEB) runtime state */
  bool aeb_active;
  int aeb_shot_index;
  int aeb_base_ev_step;
  int aeb_target_ev_steps[3];
  bool aeb_waiting_settle;
  bool aeb_waiting_capture;
  uint64_t aeb_settle_start_seq;
  uint32_t aeb_settle_start_ms;

  /* SDL2 Audio device ID for synthesized self-timer & shutter beep tones */
  uint32_t audio_dev;

  /* Application quit requested via UI Quit button */
  bool quit_requested;

  /* Pointer & cursor state */
  int pointer_x;
  int pointer_y;
  bool pointer_down;
  int down_x;
  int down_y;
  bool cursor_visible;
  uint32_t last_mouse_move_ms;

  /* Touch AF target reticle state */
  bool af_box_visible;
  float af_box_norm_x;
  float af_box_norm_y;
  uint32_t af_box_start_ms;

  /* Active popup selector state */
  bool settings_visible;
  LivePopupType active_popup;
  SDL_Rect popup_panel_rect;
  SDL_Rect popup_btn_close;
  SDL_Rect popup_btn_minus;
  SDL_Rect popup_btn_plus;
  SDL_Rect popup_btn_reset;
  SDL_Rect popup_sub_minus;
  SDL_Rect popup_sub_plus;
  SDL_Rect popup_sub_reset;
  SDL_Rect popup_sub_rects[8];
  int popup_sub_count;
  SDL_Rect popup_opt_rects[16];
  int popup_opt_count;

  /* Live View interactive button hitboxes */
  SDL_Rect top_btn_settings;
  SDL_Rect top_btn_drive;
  SDL_Rect top_btn_play;
  SDL_Rect top_btn_af;
  SDL_Rect top_btn_reset;
  SDL_Rect top_btn_quit;
  SDL_Rect btn_shutter;

  SDL_Rect hud_btn_ss;
  SDL_Rect hud_btn_iso;
  SDL_Rect hud_btn_ev;
  SDL_Rect hud_btn_wb;
  SDL_Rect hud_btn_temp;
  SDL_Rect hud_btn_lock;
  SDL_Rect hud_btn_met;
  SDL_Rect hud_btn_qual;
  SDL_Rect hud_btn_grid;
  SDL_Rect hud_btn_aspect;
  SDL_Rect hud_btn_drive;

  SDL_Rect viewport_rect;
  SDL_Rect active_frame_rect;
} LiveUiController;

/*
 * Initialize LiveUiController with default shooting parameters and UI state.
 */
void live_ui_init(LiveUiController *ui);

/*
 * Release resources (such as SDL audio device) owned by LiveUiController.
 */
void live_ui_destroy(LiveUiController *ui);

/*
 * Per-frame update for drive modes (self-timer countdown & beep, continuous
 * burst management, intervalometer scheduling, and AEB exposure sequencing).
 */
void live_ui_update(LiveUiController *ui, CameraStream *camera,
                    AsyncSaveQueue *save_queue);

/*
 * Notify LiveUiController that a still capture has completed.
 * Updates AEB / Interval / Burst state and returns true if Quick Review should
 * be displayed for this captured frame, or false if Quick Review should be
 * suppressed so continuous shooting can proceed.
 */
bool live_ui_on_still_captured(LiveUiController *ui, CameraStream *camera,
                               CameraCapturedStill *still);

/*
 * Print keyboard shortcut help banner to stdout.
 */
void live_ui_print_help(const CameraStream *camera);

/*
 * Return the currently selected JPEG encoding quality (1..100).
 */
int live_ui_get_jpeg_quality(const LiveUiController *ui);

/*
 * Crop a captured still frame in-place to match the currently selected aspect
 * ratio mask (no-op when LIVE_ASPECT_NATIVE is active).
 */
void live_ui_apply_aspect_crop(const LiveUiController *ui,
                               CameraCapturedStill *still);

/*
 * Return true if the user clicked the Quit button in the Live View UI.
 */
bool live_ui_is_quit_requested(const LiveUiController *ui);

/*
 * Render Live View overlays: Touch AF reticle, top action bar, right shutter &
 * AF/Play controls, left parameter column, and active parameter popup.
 */
void live_ui_render(LiveUiController *ui, SDL_Renderer *renderer,
                    CameraStream *camera, const CameraExposureStatus *status,
                    int pending_saves, int win_w, int win_h);

/*
 * Handle keyboard shortcuts when in APP_MODE_LIVE.
 * Returns true if the key event was handled.
 */
bool live_ui_handle_key(LiveUiController *ui, CameraStream *camera,
                        PlaybackManager *playback_mgr,
                        AsyncSaveQueue *save_queue, SDL_Renderer *renderer,
                        const SDL_KeyboardEvent *key_event);
bool live_ui_handle_key_up(LiveUiController *ui, CameraStream *camera,
                           const SDL_KeyboardEvent *key_event);

/*
 * Handle mouse/touch pointer press, move, release, and mouse wheel events
 * when in APP_MODE_LIVE.
 */
void live_ui_handle_pointer_down(LiveUiController *ui, CameraStream *camera,
                                 AsyncSaveQueue *save_queue, int x, int y,
                                 uint8_t button);
void live_ui_handle_pointer_move(LiveUiController *ui, int x, int y);
void live_ui_handle_pointer_up(LiveUiController *ui, CameraStream *camera,
                               PlaybackManager *playback_mgr,
                               AsyncSaveQueue *save_queue,
                               SDL_Renderer *renderer, int x, int y);
void live_ui_handle_wheel(LiveUiController *ui, CameraStream *camera,
                          int wheel_y, int pointer_x, int pointer_y);

#ifdef __cplusplus
}
#endif

#endif /* LIVE_UI_H */
