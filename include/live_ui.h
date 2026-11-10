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
  LIVE_POPUP_QUAL = 7
} LivePopupType;

/*
 * Live View UI, exposure/WB/metering/quality controls, and touch/mouse state.
 */
typedef struct LiveUiController {
  /* Exposure, WB, Metering & JPEG quality state indices */
  int ev_step_index; /* -9 (-3.0 EV) to +9 (+3.0 EV) in 1/3 EV increments */
  int iso_preset_index;
  int shutter_preset_index;
  int wb_preset_index;
  int wb_kelvin_value;
  bool awb_locked;
  int metering_preset_index;
  int quality_preset_index;

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
  SDL_Rect popup_opt_rects[16];
  int popup_opt_count;

  /* Live View interactive button hitboxes */
  SDL_Rect top_btn_settings;
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

  SDL_Rect viewport_rect;
} LiveUiController;

/*
 * Initialize LiveUiController with default shooting parameters and UI state.
 */
void live_ui_init(LiveUiController *ui);

/*
 * Print keyboard shortcut help banner to stdout.
 */
void live_ui_print_help(const CameraStream *camera);

/*
 * Return the currently selected JPEG encoding quality (1..100).
 */
int live_ui_get_jpeg_quality(const LiveUiController *ui);

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

/*
 * Handle mouse/touch pointer press, move, release, and mouse wheel events
 * when in APP_MODE_LIVE.
 */
void live_ui_handle_pointer_down(LiveUiController *ui, CameraStream *camera,
                                 int x, int y, uint8_t button);
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

