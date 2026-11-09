#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "async_save_queue.h"
#include "camera_stream.h"
#include "dcf_storage.h"
#include "exif_writer.h"
#include "playback_manager.h"
#include "text_renderer.h"

static volatile sig_atomic_t g_running = 1;
static PlaybackManager *g_active_playback = NULL;
static SDL_Renderer *g_active_renderer = NULL;
static CameraStream *g_active_camera = NULL;
static int g_active_jpeg_quality = 90;

static const int ISO_PRESETS[] = {0, 100, 200, 400, 800, 1600, 3200};
static const int ISO_PRESET_COUNT =
    (int)(sizeof(ISO_PRESETS) / sizeof(ISO_PRESETS[0]));

typedef struct ShutterPreset {
  int32_t exposure_us;
  const char *label;
} ShutterPreset;

static const ShutterPreset SHUTTER_PRESETS[] = {
    {0, "Auto"},       {500, "1/2000s"},  {1000, "1/1000s"}, {2000, "1/500s"},
    {4000, "1/250s"},  {8000, "1/125s"},  {16667, "1/60s"},  {33333, "1/30s"},
    {66667, "1/15s"},  {125000, "1/8s"},  {250000, "1/4s"},  {500000, "1/2s"},
    {1000000, "1.0s"}, {2000000, "2.0s"},
};
static const int SHUTTER_PRESET_COUNT =
    (int)(sizeof(SHUTTER_PRESETS) / sizeof(SHUTTER_PRESETS[0]));

typedef struct JpegQualityPreset {
  int quality;
  const char *name;
  const char *hud_label;
} JpegQualityPreset;

static const JpegQualityPreset JPEG_QUALITY_PRESETS[] = {
    {98, "SuperFine (98%)", "SFine (98%)"},
    {90, "Fine (90%)", "Fine (90%)"},
    {80, "Normal (80%)", "Norm (80%)"},
};
static const int JPEG_QUALITY_PRESET_COUNT =
    (int)(sizeof(JPEG_QUALITY_PRESETS) / sizeof(JPEG_QUALITY_PRESETS[0]));

typedef struct WbPreset {
  CameraWbMode mode;
  const char *name;
  const char *ui_label;
} WbPreset;

static const WbPreset WB_PRESETS[] = {
    {CAMERA_WB_MODE_AUTO, "Auto (AWB)", "AWB"},
    {CAMERA_WB_MODE_DAYLIGHT, "Daylight (晴天 ~5500K)", "Daylight"},
    {CAMERA_WB_MODE_CLOUDY, "Cloudy (曇天 ~6500K)", "Cloudy"},
    {CAMERA_WB_MODE_SHADE, "Shade (日陰 ~7500K)", "Shade"},
    {CAMERA_WB_MODE_TUNGSTEN, "Tungsten (白熱灯 ~2800K)", "Tungsten"},
    {CAMERA_WB_MODE_FLUORESCENT, "Fluorescent (蛍光灯 ~4000K)", "Fluoresc"},
    {CAMERA_WB_MODE_KELVIN, "Manual K (色温度指定)", "Manual K"},
};
static const int WB_PRESET_COUNT =
    (int)(sizeof(WB_PRESETS) / sizeof(WB_PRESETS[0]));

typedef struct MeteringPreset {
  CameraMeteringMode mode;
  const char *name;
  const char *ui_label;
} MeteringPreset;

static const MeteringPreset METERING_PRESETS[] = {
    {CAMERA_METERING_MATRIX, "Matrix (評価測光)", "Matrix"},
    {CAMERA_METERING_CENTRE_WEIGHTED, "CenterWeighted (中央重点測光)",
     "Center"},
    {CAMERA_METERING_SPOT, "Spot (スポット測光)", "Spot"},
};
static const int METERING_PRESET_COUNT =
    (int)(sizeof(METERING_PRESETS) / sizeof(METERING_PRESETS[0]));

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

static void signal_handler(int signum) {
  (void)signum;
  g_running = 0;
}

static void calculate_viewport_rect(int src_w, int src_h, int win_w, int win_h,
                                    SDL_Rect *out_rect) {
  if (src_w <= 0 || src_h <= 0 || win_w <= 0 || win_h <= 0) {
    out_rect->x = 0;
    out_rect->y = 0;
    out_rect->w = win_w;
    out_rect->h = win_h;
    return;
  }

  float src_aspect = (float)src_w / (float)src_h;
  float win_aspect = (float)win_w / (float)win_h;

  if (win_aspect > src_aspect) {
    // Window is wider than camera: add black bars on left/right (pillarbox)
    out_rect->h = win_h;
    out_rect->w = (int)((float)win_h * src_aspect + 0.5f);
    out_rect->x = (win_w - out_rect->w) / 2;
    out_rect->y = 0;
  } else {
    // Window is taller than camera: add black bars on top/bottom (letterbox)
    out_rect->w = win_w;
    out_rect->h = (int)((float)win_w / src_aspect + 0.5f);
    out_rect->x = 0;
    out_rect->y = (win_h - out_rect->h) / 2;
  }
}

static bool save_still_jpeg(const char *filename, const uint8_t *src_data,
                            int width, int height, int stride, bool is_bgr) {
  if (!filename || !src_data || width <= 0 || height <= 0) {
    return false;
  }

  int effective_stride = stride > 0 ? stride : (width * 3);
  Uint32 format = is_bgr ? SDL_PIXELFORMAT_BGR24 : SDL_PIXELFORMAT_RGB24;

  SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
      (void *)src_data, width, height, 24, effective_stride, format);
  if (!surface) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "%s: %d: JPEG保存用サーフェスの作成に失敗しました : %s",
                 __FILE__, __LINE__, SDL_GetError());
    return false;
  }

  const int jpeg_quality =
      g_active_jpeg_quality > 0 ? g_active_jpeg_quality : 90;
  int ret = IMG_SaveJPG(surface, filename, jpeg_quality);
  if (ret != 0) {
    fprintf(stderr, "[エラー] IMG_SaveJPG による保存に失敗しました: %s\n",
            IMG_GetError());
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "%s: %d: IMG_SaveJPG による保存に失敗しました: %s", __FILE__,
                 __LINE__, SDL_GetError());
  } else {
    CameraExposureStatus exp_status;
    memset(&exp_status, 0, sizeof(exp_status));
    if (g_active_camera) {
      camera_stream_get_exposure_status(g_active_camera, &exp_status);
    }
    ExifMetadata exif;
    exif_metadata_init(
        &exif,
        g_active_camera ? camera_stream_get_model_name(g_active_camera) : NULL,
        width, height, &exp_status);
    exif_inject_into_jpeg_file(filename, &exif);

    if (g_active_playback && g_active_renderer) {
      playback_manager_start_quick_review(g_active_playback, g_active_renderer,
                                          filename, src_data, width, height,
                                          stride, is_bgr, &exp_status);
    }
  }

  SDL_FreeSurface(surface);
  return ret == 0;
}

static void format_ss_short(int32_t exposure_us, char *buf, size_t buf_size) {
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

static bool live_btn_hovered(const LiveUiController *ui, const SDL_Rect *r) {
  return ui && ui_rect_contains(r, ui->pointer_x, ui->pointer_y);
}

static bool live_btn_pressed(const LiveUiController *ui, const SDL_Rect *r) {
  return ui && ui->pointer_down &&
         ui_rect_contains(r, ui->down_x, ui->down_y) &&
         ui_rect_contains(r, ui->pointer_x, ui->pointer_y);
}

static void live_ui_step_ev(LiveUiController *ui, CameraStream *camera,
                            int delta) {
  int next = ui->ev_step_index + delta;
  if (next < -9) {
    next = -9;
  } else if (next > 9) {
    next = 9;
  }
  ui->ev_step_index = next;
  float ev = (float)ui->ev_step_index / 3.0f;
  camera_stream_set_ev(camera, ev);
  printf("[-] 露出補正 (EV): %+.2f EV\n", ev);
}

static void live_ui_set_iso_index(LiveUiController *ui, CameraStream *camera,
                                  int idx) {
  ui->iso_preset_index =
      (idx % ISO_PRESET_COUNT + ISO_PRESET_COUNT) % ISO_PRESET_COUNT;
  int iso = ISO_PRESETS[ui->iso_preset_index];
  camera_stream_set_iso(camera, iso);
  if (iso == 0) {
    printf("[-] ISO感度: Auto\n");
  } else {
    printf("[-] ISO感度: %d (手動)\n", iso);
  }
}

static void live_ui_set_shutter_index(LiveUiController *ui,
                                      CameraStream *camera, int idx) {
  ui->shutter_preset_index =
      (idx % SHUTTER_PRESET_COUNT + SHUTTER_PRESET_COUNT) %
      SHUTTER_PRESET_COUNT;
  int32_t shutter_us = SHUTTER_PRESETS[ui->shutter_preset_index].exposure_us;
  camera_stream_set_shutter_speed(camera, shutter_us);
  printf("[-] シャッタースピード: %s\n",
         SHUTTER_PRESETS[ui->shutter_preset_index].label);
}

static void live_ui_set_wb_index(LiveUiController *ui, CameraStream *camera,
                                 int idx) {
  ui->wb_preset_index =
      (idx % WB_PRESET_COUNT + WB_PRESET_COUNT) % WB_PRESET_COUNT;
  ui->awb_locked = false;
  CameraWbMode wb_mode = WB_PRESETS[ui->wb_preset_index].mode;
  if (wb_mode == CAMERA_WB_MODE_KELVIN) {
    camera_stream_set_wb_kelvin(camera, ui->wb_kelvin_value);
    printf("[-] ホワイトバランス: %s (%dK)\n",
           WB_PRESETS[ui->wb_preset_index].name, ui->wb_kelvin_value);
  } else {
    camera_stream_set_wb_mode(camera, wb_mode);
    printf("[-] ホワイトバランス: %s\n", WB_PRESETS[ui->wb_preset_index].name);
  }
}

static void live_ui_set_kelvin(LiveUiController *ui, CameraStream *camera,
                               int kelvin) {
  if (kelvin < 2500) {
    kelvin = 2500;
  } else if (kelvin > 10000) {
    kelvin = 10000;
  }
  ui->wb_kelvin_value = kelvin;
  ui->wb_preset_index = WB_PRESET_COUNT - 1; /* CAMERA_WB_MODE_KELVIN */
  ui->awb_locked = false;
  camera_stream_set_wb_kelvin(camera, ui->wb_kelvin_value);
  printf("[-] 色温度手動指定 (WB Kelvin): %dK\n", ui->wb_kelvin_value);
}

static void live_ui_toggle_awb_lock(LiveUiController *ui,
                                    CameraStream *camera) {
  ui->awb_locked = !ui->awb_locked;
  camera_stream_set_awb_lock(camera, ui->awb_locked);
  printf("[-] AWBロック: %s\n",
         ui->awb_locked ? "ON (現在の色合いで固定)" : "OFF (解除)");
}

static void live_ui_set_metering_index(LiveUiController *ui,
                                       CameraStream *camera, int idx) {
  ui->metering_preset_index =
      (idx % METERING_PRESET_COUNT + METERING_PRESET_COUNT) %
      METERING_PRESET_COUNT;
  CameraMeteringMode met_mode =
      METERING_PRESETS[ui->metering_preset_index].mode;
  camera_stream_set_metering_mode(camera, met_mode);
  printf("[-] 測光モード: %s\n",
         METERING_PRESETS[ui->metering_preset_index].name);
}

static void live_ui_set_quality_index(LiveUiController *ui, int idx) {
  ui->quality_preset_index =
      (idx % JPEG_QUALITY_PRESET_COUNT + JPEG_QUALITY_PRESET_COUNT) %
      JPEG_QUALITY_PRESET_COUNT;
  g_active_jpeg_quality =
      JPEG_QUALITY_PRESETS[ui->quality_preset_index].quality;
  printf("[-] JPEG保存画質: %s\n",
         JPEG_QUALITY_PRESETS[ui->quality_preset_index].name);
}

static void live_ui_reset_all(LiveUiController *ui, CameraStream *camera) {
  ui->ev_step_index = 0;
  ui->iso_preset_index = 0;
  ui->shutter_preset_index = 0;
  ui->wb_preset_index = 0;
  ui->wb_kelvin_value = 5500;
  ui->awb_locked = false;
  ui->metering_preset_index = 0;
  ui->quality_preset_index = 1;
  ui->active_popup = LIVE_POPUP_NONE;
  ui->af_box_visible = false;
  g_active_jpeg_quality =
      JPEG_QUALITY_PRESETS[ui->quality_preset_index].quality;
  camera_stream_set_ev(camera, 0.0f);
  camera_stream_set_iso(camera, 0);
  camera_stream_set_shutter_speed(camera, 0);
  camera_stream_set_wb_mode(camera, CAMERA_WB_MODE_AUTO);
  camera_stream_set_awb_lock(camera, false);
  camera_stream_set_metering_mode(camera, CAMERA_METERING_MATRIX);
  if (camera_stream_is_af_supported(camera)) {
    camera_stream_trigger_af(camera);
  }
  printf("[-] 露出・WB・測光・画質設定をリセットしました (EV: 0.0, ISO: Auto, "
         "SS: Auto, WB: Auto, Met: Matrix, JPG: %s)\n",
         JPEG_QUALITY_PRESETS[ui->quality_preset_index].name);
}

static void live_ui_request_shutter(CameraStream *camera,
                                    AsyncSaveQueue *save_queue) {
  if (camera_stream_is_still_pending(camera)) {
    printf("[-] 撮影処理中です...\n");
  } else if (async_save_queue_get_pending_count(save_queue) >=
             ASYNC_SAVE_QUEUE_CAPACITY) {
    fprintf(stderr,
            "[警告] 保存キューが満杯です。書き込み完了をお待ちください。\n");
  } else {
    camera_stream_request_still(camera);
  }
}

static void live_ui_trigger_af_at(LiveUiController *ui, CameraStream *camera,
                                  float norm_x, float norm_y) {
  if (!camera_stream_is_af_supported(camera)) {
    return;
  }
  if (norm_x < 0.0f || norm_y < 0.0f) {
    ui->af_box_norm_x = 0.5f;
    ui->af_box_norm_y = 0.5f;
    camera_stream_trigger_af(camera);
  } else {
    ui->af_box_norm_x = norm_x;
    ui->af_box_norm_y = norm_y;
    camera_stream_trigger_touch_af(camera, norm_x, norm_y);
  }
  ui->af_box_visible = true;
  ui->af_box_start_ms = SDL_GetTicks();
}

static void render_af_reticle(SDL_Renderer *renderer, LiveUiController *ui,
                              CameraStream *camera) {
  if (!ui || !ui->af_box_visible || !camera_stream_is_af_supported(camera)) {
    return;
  }

  uint32_t now = SDL_GetTicks();
  CameraAfStatus af_st = camera_stream_get_af_status(camera);
  uint32_t timeout_ms = (af_st == CAMERA_AF_STATUS_SCANNING) ? 2800U : 1600U;
  if (SDL_TICKS_PASSED(now, ui->af_box_start_ms + timeout_ms)) {
    ui->af_box_visible = false;
    return;
  }

  int box_w = 76;
  int box_h = 76;
  int cx = ui->viewport_rect.x +
           (int)(ui->af_box_norm_x * (float)ui->viewport_rect.w);
  int cy = ui->viewport_rect.y +
           (int)(ui->af_box_norm_y * (float)ui->viewport_rect.h);
  SDL_Rect box = {cx - box_w / 2, cy - box_h / 2, box_w, box_h};

  SDL_Color col = {255, 220, 40, 240}; /* Yellow: Scanning */
  if (af_st == CAMERA_AF_STATUS_FOCUSED) {
    col = (SDL_Color){60, 250, 100, 245}; /* Green: Focused */
  } else if (af_st == CAMERA_AF_STATUS_FAILED) {
    col = (SDL_Color){255, 70, 70, 245}; /* Red: Failed */
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, col.r, col.g, col.b, col.a);
  for (int i = 0; i < 2; ++i) {
    SDL_Rect r = {box.x - i, box.y - i, box.w + i * 2, box.h + i * 2};
    SDL_RenderDrawRect(renderer, &r);
  }

  /* Center crosshair ticks */
  SDL_RenderDrawLine(renderer, cx - 8, cy, cx + 8, cy);
  SDL_RenderDrawLine(renderer, cx, cy - 8, cx, cy + 8);
}

typedef struct {
  char label[48];
  int min_w;
  UiButtonStyle style;
  SDL_Rect *target_rect;
  bool force_new_row;
  int rel_x;
  int rel_y;
} PopupBtnItem;

static int calc_button_width(const char *text, int min_w) {
  int tw = 0;
  int th = 0;
  if (!text_renderer_measure_text(text, &tw, &th)) {
    tw = (int)strlen(text) * 9;
  }
  int w = tw + 24; /* matches pad_x = 12 * 2 in text_renderer_draw_button */
  return (min_w > w) ? min_w : w;
}

static void render_live_popup(SDL_Renderer *renderer, LiveUiController *ui,
                              int win_w, int win_h) {
  ui->popup_panel_rect = (SDL_Rect){0, 0, 0, 0};
  ui->popup_btn_close = (SDL_Rect){0, 0, 0, 0};
  ui->popup_btn_minus = (SDL_Rect){0, 0, 0, 0};
  ui->popup_btn_plus = (SDL_Rect){0, 0, 0, 0};
  ui->popup_btn_reset = (SDL_Rect){0, 0, 0, 0};
  ui->popup_opt_count = 0;

  if (ui->active_popup == LIVE_POPUP_NONE) {
    return;
  }

  /* Determine right edge of the left vertical settings column across all 8
   * buttons */
  const SDL_Rect *left_btns[8] = {
      &ui->hud_btn_ss,   &ui->hud_btn_iso,  &ui->hud_btn_ev,  &ui->hud_btn_wb,
      &ui->hud_btn_temp, &ui->hud_btn_lock, &ui->hud_btn_met, &ui->hud_btn_qual,
  };
  int left_col_right = 152;
  for (int i = 0; i < 8; ++i) {
    int r_edge = left_btns[i]->x + left_btns[i]->w;
    if (r_edge > left_col_right) {
      left_col_right = r_edge;
    }
  }

  int panel_x = left_col_right + 10;
  int right_limit =
      (ui->btn_shutter.x > 0 ? ui->btn_shutter.x : (win_w - 110)) - 10;
  int max_panel_w = right_limit - panel_x;
  if (max_panel_w > 740) {
    max_panel_w = 740;
  }
  if (max_panel_w < 300) {
    max_panel_w = 300;
  }

  const char *title = "";
  int anchor_y = 56;
  switch (ui->active_popup) {
  case LIVE_POPUP_SS:
    title = "Shutter Speed [S / Left / Right]";
    anchor_y = ui->hud_btn_ss.y;
    break;
  case LIVE_POPUP_ISO:
    title = "ISO Sensitivity [I]";
    anchor_y = ui->hud_btn_iso.y;
    break;
  case LIVE_POPUP_EV:
    title = "Exposure Compensation (EV) [Up / Down]";
    anchor_y = ui->hud_btn_ev.y;
    break;
  case LIVE_POPUP_WB:
    title = "White Balance Mode [W]";
    anchor_y = ui->hud_btn_wb.y;
    break;
  case LIVE_POPUP_TEMP:
    title = "Color Temperature (Kelvin) [K]";
    anchor_y = ui->hud_btn_temp.y;
    break;
  case LIVE_POPUP_MET:
    title = "AE Metering Mode [M]";
    anchor_y = ui->hud_btn_met.y;
    break;
  case LIVE_POPUP_QUAL:
    title = "JPEG Quality [J]";
    anchor_y = ui->hud_btn_qual.y;
    break;
  default:
    break;
  }

  /* Build list of buttons for the active popup */
  PopupBtnItem items[20];
  int item_count = 0;

  if (ui->active_popup == LIVE_POPUP_ISO) {
    ui->popup_opt_count = ISO_PRESET_COUNT;
    for (int i = 0; i < ISO_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      if (ISO_PRESETS[i] == 0) {
        snprintf(it->label, sizeof(it->label), "Auto");
      } else {
        snprintf(it->label, sizeof(it->label), "ISO %d", ISO_PRESETS[i]);
      }
      it->min_w = 86;
      it->style = (ui->iso_preset_index == i) ? UI_BTN_STYLE_ACTIVE
                                              : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }
  } else if (ui->active_popup == LIVE_POPUP_WB) {
    ui->popup_opt_count = WB_PRESET_COUNT;
    for (int i = 0; i < WB_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%s", WB_PRESETS[i].ui_label);
      it->min_w = 90;
      it->style = (ui->wb_preset_index == i && !ui->awb_locked)
                      ? UI_BTN_STYLE_ACTIVE
                      : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }
  } else if (ui->active_popup == LIVE_POPUP_MET) {
    ui->popup_opt_count = METERING_PRESET_COUNT;
    for (int i = 0; i < METERING_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%s",
               METERING_PRESETS[i].ui_label);
      it->min_w = 116;
      it->style = (ui->metering_preset_index == i) ? UI_BTN_STYLE_ACTIVE
                                                   : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }
  } else if (ui->active_popup == LIVE_POPUP_QUAL) {
    ui->popup_opt_count = JPEG_QUALITY_PRESET_COUNT;
    for (int i = 0; i < JPEG_QUALITY_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%s",
               JPEG_QUALITY_PRESETS[i].name);
      it->min_w = 136;
      it->style = (ui->quality_preset_index == i) ? UI_BTN_STYLE_ACTIVE
                                                  : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }
  } else if (ui->active_popup == LIVE_POPUP_EV) {
    PopupBtnItem *it_minus = &items[item_count++];
    snprintf(it_minus->label, sizeof(it_minus->label), "- 1/3 EV");
    it_minus->min_w = 96;
    it_minus->style = UI_BTN_STYLE_DEFAULT;
    it_minus->target_rect = &ui->popup_btn_minus;
    it_minus->force_new_row = false;

    PopupBtnItem *it_reset = &items[item_count++];
    snprintf(it_reset->label, sizeof(it_reset->label), "Reset 0.0 (Now: %+.1f)",
             (float)ui->ev_step_index / 3.0f);
    it_reset->min_w = 172;
    it_reset->style =
        ui->ev_step_index == 0 ? UI_BTN_STYLE_ACTIVE : UI_BTN_STYLE_DEFAULT;
    it_reset->target_rect = &ui->popup_btn_reset;
    it_reset->force_new_row = false;

    PopupBtnItem *it_plus = &items[item_count++];
    snprintf(it_plus->label, sizeof(it_plus->label), "+ 1/3 EV");
    it_plus->min_w = 96;
    it_plus->style = UI_BTN_STYLE_DEFAULT;
    it_plus->target_rect = &ui->popup_btn_plus;
    it_plus->force_new_row = false;

    static const int EV_QUICK_STEPS[5] = {-6, -3, 0, 3, 6};
    ui->popup_opt_count = 5;
    for (int i = 0; i < 5; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%+d.0 EV", EV_QUICK_STEPS[i] / 3);
      it->min_w = 76;
      it->style = (ui->ev_step_index == EV_QUICK_STEPS[i])
                      ? UI_BTN_STYLE_ACTIVE
                      : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = (i == 0);
    }
  } else if (ui->active_popup == LIVE_POPUP_TEMP) {
    PopupBtnItem *it_minus = &items[item_count++];
    snprintf(it_minus->label, sizeof(it_minus->label), "- 500K");
    it_minus->min_w = 92;
    it_minus->style = UI_BTN_STYLE_DEFAULT;
    it_minus->target_rect = &ui->popup_btn_minus;
    it_minus->force_new_row = false;

    PopupBtnItem *it_reset = &items[item_count++];
    snprintf(it_reset->label, sizeof(it_reset->label), "5500K (Now: %dK)",
             ui->wb_kelvin_value);
    it_reset->min_w = 168;
    it_reset->style = ui->wb_kelvin_value == 5500 ? UI_BTN_STYLE_ACTIVE
                                                  : UI_BTN_STYLE_DEFAULT;
    it_reset->target_rect = &ui->popup_btn_reset;
    it_reset->force_new_row = false;

    PopupBtnItem *it_plus = &items[item_count++];
    snprintf(it_plus->label, sizeof(it_plus->label), "+ 500K");
    it_plus->min_w = 92;
    it_plus->style = UI_BTN_STYLE_DEFAULT;
    it_plus->target_rect = &ui->popup_btn_plus;
    it_plus->force_new_row = false;

    static const int K_QUICK[5] = {2800, 4000, 5500, 6500, 8000};
    ui->popup_opt_count = 5;
    for (int i = 0; i < 5; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%dK", K_QUICK[i]);
      it->min_w = 76;
      it->style =
          (WB_PRESETS[ui->wb_preset_index].mode == CAMERA_WB_MODE_KELVIN &&
           ui->wb_kelvin_value == K_QUICK[i])
              ? UI_BTN_STYLE_ACTIVE
              : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = (i == 0);
    }
  } else if (ui->active_popup == LIVE_POPUP_SS) {
    ui->popup_opt_count = SHUTTER_PRESET_COUNT;
    for (int i = 0; i < SHUTTER_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%s", SHUTTER_PRESETS[i].label);
      it->min_w = 88;
      it->style = (ui->shutter_preset_index == i) ? UI_BTN_STYLE_ACTIVE
                                                  : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }
  }

  /* Pass 1: Compute wrapped relative button positions within max_panel_w */
  const int pad_x = 12;
  const int header_h = 44;
  const int gap_x = 6;
  const int gap_y = 6;
  const int btn_h = 38;

  int cur_x = pad_x;
  int cur_y = header_h;
  int max_right_x = pad_x;

  for (int i = 0; i < item_count; ++i) {
    int bw = calc_button_width(items[i].label, items[i].min_w);
    if (items[i].force_new_row ||
        (cur_x > pad_x && cur_x + bw > max_panel_w - pad_x)) {
      cur_x = pad_x;
      cur_y += btn_h + gap_y;
    }
    items[i].rel_x = cur_x;
    items[i].rel_y = cur_y;
    if (cur_x + bw > max_right_x) {
      max_right_x = cur_x + bw;
    }
    cur_x += bw + gap_x;
  }

  int title_w = 0;
  int title_h = 0;
  if (!text_renderer_measure_text(title, &title_w, &title_h)) {
    title_w = (int)strlen(title) * 9;
  }
  int close_w = calc_button_width("Close [X]", 86);
  int min_header_w = pad_x + title_w + 16 + close_w + pad_x;

  int panel_w = max_right_x + pad_x;
  if (panel_w < min_header_w) {
    panel_w = min_header_w;
  }
  if (panel_w > max_panel_w) {
    panel_w = max_panel_w;
  }

  int panel_h = cur_y + btn_h + 12;
  int panel_y = anchor_y;
  if (panel_y + panel_h > win_h - 10) {
    panel_y = win_h - panel_h - 10;
  }
  if (panel_y < 54) {
    panel_y = 54;
  }

  SDL_Rect panel = {panel_x, panel_y, panel_w, panel_h};
  ui->popup_panel_rect = panel;

  SDL_Color bg = {14, 18, 26, 235};
  SDL_Color bd = {100, 210, 255, 220};
  text_renderer_draw_panel(renderer, &panel, bg, bd);

  SDL_Color cyan = {130, 225, 255, 255};
  text_renderer_draw_text(renderer, title, panel.x + pad_x, panel.y + 10, cyan,
                          NULL, NULL);

  text_renderer_draw_button(
      renderer, "Close [X]", panel.x + panel.w - close_w - pad_x, panel.y + 6,
      86, 32, UI_BTN_STYLE_DEFAULT, live_btn_hovered(ui, &ui->popup_btn_close),
      live_btn_pressed(ui, &ui->popup_btn_close), &ui->popup_btn_close);

  /* Pass 2: Draw all buttons inside the fitted panel */
  for (int i = 0; i < item_count; ++i) {
    SDL_Rect r;
    text_renderer_draw_button(
        renderer, items[i].label, panel.x + items[i].rel_x,
        panel.y + items[i].rel_y, items[i].min_w, btn_h, items[i].style,
        live_btn_hovered(ui, items[i].target_rect),
        live_btn_pressed(ui, items[i].target_rect), &r);
    if (items[i].target_rect) {
      *(items[i].target_rect) = r;
    }
  }
}

static void render_live_ui(SDL_Renderer *renderer, LiveUiController *ui,
                           CameraStream *camera,
                           const CameraExposureStatus *status,
                           int pending_saves, int win_w, int win_h) {
  bool af_supported = camera_stream_is_af_supported(camera);

  /* 1. Touch AF reticle inside viewfinder */
  render_af_reticle(renderer, ui, camera);

  /* 2. Top action bar (Top-left: Settings [+ Reset], Top-right: Quit) */
  int tx = 12;
  int ty = 10;
  int tgap = 8;
  int th = 38;
  SDL_Rect r;

  text_renderer_draw_button(renderer, "Settings", tx, ty, 104, th,
                            ui->settings_visible ? UI_BTN_STYLE_ACTIVE
                                                 : UI_BTN_STYLE_DEFAULT,
                            live_btn_hovered(ui, &ui->top_btn_settings),
                            live_btn_pressed(ui, &ui->top_btn_settings), &r);
  ui->top_btn_settings = r;
  tx += r.w + tgap;

  if (ui->settings_visible) {
    text_renderer_draw_button(renderer, "Reset [R]", tx, ty, 96, th,
                              UI_BTN_STYLE_DEFAULT,
                              live_btn_hovered(ui, &ui->top_btn_reset),
                              live_btn_pressed(ui, &ui->top_btn_reset), &r);
    ui->top_btn_reset = r;
    tx += r.w + tgap;
  } else {
    ui->top_btn_reset = (SDL_Rect){0, 0, 0, 0};
  }

  int right_col_w = 98;
  int right_col_x = win_w - right_col_w - 12;

  text_renderer_draw_button(
      renderer, "Quit [Q]", right_col_x, ty, right_col_w, th,
      UI_BTN_STYLE_DANGER, live_btn_hovered(ui, &ui->top_btn_quit),
      live_btn_pressed(ui, &ui->top_btn_quit), &ui->top_btn_quit);

  if (pending_saves > 0) {
    char save_lbl[48];
    snprintf(save_lbl, sizeof(save_lbl), "Saving: %d", pending_saves);
    SDL_Color fg = {255, 235, 110, 255};
    SDL_Color bg = {70, 50, 0, 215};
    SDL_Color bd = {255, 200, 50, 230};
    text_renderer_draw_badge(renderer, save_lbl, tx + 6, ty + 3, fg, bg, bd,
                             NULL);
  }

  /* 3. Right-edge large floating Shutter Button + Focus Button + Bottom-Right
   * Play Button */
  bool still_busy = camera_stream_is_still_pending(camera);
  int sh_w = right_col_w;
  int sh_h = 84;
  int af_h = 52;
  int af_gap = 10;
  int right_group_h = af_supported ? (sh_h + af_gap + af_h) : sh_h;
  int sh_x = right_col_x;
  int sh_y = (win_h - right_group_h) / 2;

  text_renderer_draw_button(
      renderer, still_busy ? "WAIT..." : "SHOT", sh_x, sh_y, sh_w, sh_h,
      still_busy ? UI_BTN_STYLE_ACCENT : UI_BTN_STYLE_DANGER,
      live_btn_hovered(ui, &ui->btn_shutter),
      live_btn_pressed(ui, &ui->btn_shutter), &ui->btn_shutter);

  if (af_supported) {
    CameraAfStatus af_st = camera_stream_get_af_status(camera);
    const char *af_lbl = "AF [F]";
    UiButtonStyle af_style = UI_BTN_STYLE_DEFAULT;
    if (af_st == CAMERA_AF_STATUS_SCANNING) {
      af_lbl = "AF... [F]";
      af_style = UI_BTN_STYLE_ACCENT;
    } else if (af_st == CAMERA_AF_STATUS_FOCUSED) {
      af_lbl = "AF OK [F]";
      af_style = UI_BTN_STYLE_SUCCESS;
    }
    int af_y = sh_y + sh_h + af_gap;
    text_renderer_draw_button(renderer, af_lbl, sh_x, af_y, sh_w, af_h,
                              af_style, live_btn_hovered(ui, &ui->top_btn_af),
                              live_btn_pressed(ui, &ui->top_btn_af),
                              &ui->top_btn_af);
  } else {
    ui->top_btn_af = (SDL_Rect){0, 0, 0, 0};
  }

  text_renderer_draw_button(
      renderer, "Play [P]", right_col_x, win_h - th - 10, right_col_w, th,
      UI_BTN_STYLE_ACTIVE, live_btn_hovered(ui, &ui->top_btn_play),
      live_btn_pressed(ui, &ui->top_btn_play), &ui->top_btn_play);

  /* 4. Left-edge vertical shooting parameter column (shown only when
   * settings_visible is true) */
  if (!ui->settings_visible) {
    ui->hud_btn_ss = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_iso = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_ev = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_wb = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_temp = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_lock = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_met = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_qual = (SDL_Rect){0, 0, 0, 0};
    ui->active_popup = LIVE_POPUP_NONE;
    render_live_popup(renderer, ui, win_w, win_h);
    return;
  }

  int bx = 12;
  int by = 56;
  int bgap = 6;
  int avail_h = win_h - by - 10;
  int bh = (avail_h - 7 * bgap) / 8;
  if (bh > 40) {
    bh = 40;
  }
  if (bh < 32) {
    bh = 32;
  }
  int bw = 156;

  char ss_str[48];
  if (status->shutter_us_setting > 0) {
    char m_ss[24];
    format_ss_short(status->shutter_us_setting, m_ss, sizeof(m_ss));
    snprintf(ss_str, sizeof(ss_str), "SS:%s[M]", m_ss);
  } else {
    char a_ss[24];
    format_ss_short(status->actual_exposure_us, a_ss, sizeof(a_ss));
    snprintf(ss_str, sizeof(ss_str), "SS:Auto(%s)", a_ss);
  }
  int w_ss = calc_button_width(ss_str, bw);
  if (w_ss > bw) {
    bw = w_ss;
  }
  text_renderer_draw_button(renderer, ss_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_SS
                                ? UI_BTN_STYLE_ACTIVE
                                : (status->shutter_us_setting > 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_ss),
                            live_btn_pressed(ui, &ui->hud_btn_ss), &r);
  ui->hud_btn_ss = r;
  by += r.h + bgap;

  char iso_str[48];
  if (status->iso_setting > 0) {
    snprintf(iso_str, sizeof(iso_str), "ISO:%d[M]", status->iso_setting);
  } else {
    int actual_iso = (int)(status->actual_analogue_gain * 100.0f + 0.5f);
    if (actual_iso < 100) {
      actual_iso = 100;
    }
    actual_iso = ((actual_iso + 5) / 10) * 10;
    snprintf(iso_str, sizeof(iso_str), "ISO:Auto(%d)", actual_iso);
  }
  text_renderer_draw_button(renderer, iso_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_ISO
                                ? UI_BTN_STYLE_ACTIVE
                                : (status->iso_setting > 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_iso),
                            live_btn_pressed(ui, &ui->hud_btn_iso), &r);
  ui->hud_btn_iso = r;
  by += r.h + bgap;

  char ev_str[32];
  snprintf(ev_str, sizeof(ev_str), "EV:%+.1f", status->ev_compensation);
  text_renderer_draw_button(renderer, ev_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_EV
                                ? UI_BTN_STYLE_ACTIVE
                                : (ui->ev_step_index != 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_ev),
                            live_btn_pressed(ui, &ui->hud_btn_ev), &r);
  ui->hud_btn_ev = r;
  by += r.h + bgap;

  char wb_str[48];
  snprintf(wb_str, sizeof(wb_str), "WB:%s",
           WB_PRESETS[ui->wb_preset_index].ui_label);
  text_renderer_draw_button(renderer, wb_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_WB
                                ? UI_BTN_STYLE_ACTIVE
                                : (ui->wb_preset_index != 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_wb),
                            live_btn_pressed(ui, &ui->hud_btn_wb), &r);
  ui->hud_btn_wb = r;
  by += r.h + bgap;

  int disp_k = (WB_PRESETS[ui->wb_preset_index].mode == CAMERA_WB_MODE_KELVIN)
                   ? ui->wb_kelvin_value
                   : (status->actual_colour_temp_k > 0
                          ? ((status->actual_colour_temp_k + 50) / 100) * 100
                          : 5500);
  char k_str[32];
  snprintf(k_str, sizeof(k_str), "K:%dK", disp_k);
  text_renderer_draw_button(
      renderer, k_str, bx, by, bw, bh,
      ui->active_popup == LIVE_POPUP_TEMP
          ? UI_BTN_STYLE_ACTIVE
          : (WB_PRESETS[ui->wb_preset_index].mode == CAMERA_WB_MODE_KELVIN
                 ? UI_BTN_STYLE_ACCENT
                 : UI_BTN_STYLE_DEFAULT),
      live_btn_hovered(ui, &ui->hud_btn_temp),
      live_btn_pressed(ui, &ui->hud_btn_temp), &r);
  ui->hud_btn_temp = r;
  by += r.h + bgap;

  text_renderer_draw_button(
      renderer, ui->awb_locked ? "AWB:LOCK" : "AWB:Free", bx, by, bw, bh,
      ui->awb_locked ? UI_BTN_STYLE_ACCENT : UI_BTN_STYLE_DEFAULT,
      live_btn_hovered(ui, &ui->hud_btn_lock),
      live_btn_pressed(ui, &ui->hud_btn_lock), &r);
  ui->hud_btn_lock = r;
  by += r.h + bgap;

  char met_str[32];
  snprintf(met_str, sizeof(met_str), "Met:%s",
           METERING_PRESETS[ui->metering_preset_index].ui_label);
  text_renderer_draw_button(renderer, met_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_MET
                                ? UI_BTN_STYLE_ACTIVE
                                : (ui->metering_preset_index != 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_met),
                            live_btn_pressed(ui, &ui->hud_btn_met), &r);
  ui->hud_btn_met = r;
  by += r.h + bgap;

  char q_str[32];
  snprintf(q_str, sizeof(q_str), "JPG:%s",
           JPEG_QUALITY_PRESETS[ui->quality_preset_index].hud_label);
  text_renderer_draw_button(renderer, q_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_QUAL
                                ? UI_BTN_STYLE_ACTIVE
                                : UI_BTN_STYLE_DEFAULT,
                            live_btn_hovered(ui, &ui->hud_btn_qual),
                            live_btn_pressed(ui, &ui->hud_btn_qual), &r);
  ui->hud_btn_qual = r;

  /* 5. Active parameter popup selector next to the left parameter column */
  render_live_popup(renderer, ui, win_w, win_h);
}

static void toggle_popup(LiveUiController *ui, LivePopupType type) {
  if (ui->active_popup == type) {
    ui->active_popup = LIVE_POPUP_NONE;
  } else {
    ui->active_popup = type;
  }
}

static void handle_live_pointer_up(LiveUiController *ui, CameraStream *camera,
                                   PlaybackManager *playback_mgr,
                                   AsyncSaveQueue *save_queue,
                                   SDL_Renderer *renderer, int x, int y) {
  int sx = ui->down_x;
  int sy = ui->down_y;
  ui->pointer_x = x;
  ui->pointer_y = y;
  ui->pointer_down = false;

  /* Top-left Settings toggle button is always interactive */
  if (ui_rect_contains(&ui->top_btn_settings, x, y) &&
      ui_rect_contains(&ui->top_btn_settings, sx, sy)) {
    ui->settings_visible = !ui->settings_visible;
    if (!ui->settings_visible) {
      ui->active_popup = LIVE_POPUP_NONE;
    }
    return;
  }

  /* 1. Check active popup buttons first if open */
  if (ui->active_popup != LIVE_POPUP_NONE) {
    if (ui_rect_contains(&ui->popup_btn_close, x, y) &&
        ui_rect_contains(&ui->popup_btn_close, sx, sy)) {
      ui->active_popup = LIVE_POPUP_NONE;
      return;
    }

    if (ui->active_popup == LIVE_POPUP_EV) {
      if (ui_rect_contains(&ui->popup_btn_minus, x, y) &&
          ui_rect_contains(&ui->popup_btn_minus, sx, sy)) {
        live_ui_step_ev(ui, camera, -1);
        return;
      }
      if (ui_rect_contains(&ui->popup_btn_plus, x, y) &&
          ui_rect_contains(&ui->popup_btn_plus, sx, sy)) {
        live_ui_step_ev(ui, camera, 1);
        return;
      }
      if (ui_rect_contains(&ui->popup_btn_reset, x, y) &&
          ui_rect_contains(&ui->popup_btn_reset, sx, sy)) {
        ui->ev_step_index = 0;
        camera_stream_set_ev(camera, 0.0f);
        printf("[-] 露出補正 (EV): +0.00 EV\n");
        return;
      }
      static const int EV_QUICK_STEPS[5] = {-6, -3, 0, 3, 6};
      for (int i = 0; i < ui->popup_opt_count; ++i) {
        if (ui_rect_contains(&ui->popup_opt_rects[i], x, y) &&
            ui_rect_contains(&ui->popup_opt_rects[i], sx, sy)) {
          ui->ev_step_index = EV_QUICK_STEPS[i];
          camera_stream_set_ev(camera, (float)ui->ev_step_index / 3.0f);
          printf("[-] 露出補正 (EV): %+.2f EV\n",
                 (float)ui->ev_step_index / 3.0f);
          return;
        }
      }
    } else if (ui->active_popup == LIVE_POPUP_TEMP) {
      if (ui_rect_contains(&ui->popup_btn_minus, x, y) &&
          ui_rect_contains(&ui->popup_btn_minus, sx, sy)) {
        live_ui_set_kelvin(ui, camera, ui->wb_kelvin_value - 500);
        return;
      }
      if (ui_rect_contains(&ui->popup_btn_plus, x, y) &&
          ui_rect_contains(&ui->popup_btn_plus, sx, sy)) {
        live_ui_set_kelvin(ui, camera, ui->wb_kelvin_value + 500);
        return;
      }
      if (ui_rect_contains(&ui->popup_btn_reset, x, y) &&
          ui_rect_contains(&ui->popup_btn_reset, sx, sy)) {
        live_ui_set_kelvin(ui, camera, 5500);
        return;
      }
      static const int K_QUICK[5] = {2800, 4000, 5500, 6500, 8000};
      for (int i = 0; i < ui->popup_opt_count; ++i) {
        if (ui_rect_contains(&ui->popup_opt_rects[i], x, y) &&
            ui_rect_contains(&ui->popup_opt_rects[i], sx, sy)) {
          live_ui_set_kelvin(ui, camera, K_QUICK[i]);
          return;
        }
      }
    } else {
      for (int i = 0; i < ui->popup_opt_count; ++i) {
        if (ui_rect_contains(&ui->popup_opt_rects[i], x, y) &&
            ui_rect_contains(&ui->popup_opt_rects[i], sx, sy)) {
          if (ui->active_popup == LIVE_POPUP_SS) {
            live_ui_set_shutter_index(ui, camera, i);
          } else if (ui->active_popup == LIVE_POPUP_ISO) {
            live_ui_set_iso_index(ui, camera, i);
          } else if (ui->active_popup == LIVE_POPUP_WB) {
            live_ui_set_wb_index(ui, camera, i);
          } else if (ui->active_popup == LIVE_POPUP_MET) {
            live_ui_set_metering_index(ui, camera, i);
          } else if (ui->active_popup == LIVE_POPUP_QUAL) {
            live_ui_set_quality_index(ui, i);
          }
          return;
        }
      }
    }

    /* Absorb clicks inside the popup panel background */
    if (ui_rect_contains(&ui->popup_panel_rect, x, y)) {
      return;
    }
  }

  /* 2. Top bar buttons */
  if (ui_rect_contains(&ui->top_btn_play, x, y) &&
      ui_rect_contains(&ui->top_btn_play, sx, sy)) {
    ui->active_popup = LIVE_POPUP_NONE;
    playback_manager_enter_playback(playback_mgr, renderer, NULL);
    return;
  }

  if (ui_rect_contains(&ui->top_btn_af, x, y) &&
      ui_rect_contains(&ui->top_btn_af, sx, sy)) {
    live_ui_trigger_af_at(ui, camera, -1.0f, -1.0f);
    return;
  }

  if (ui_rect_contains(&ui->top_btn_reset, x, y) &&
      ui_rect_contains(&ui->top_btn_reset, sx, sy)) {
    live_ui_reset_all(ui, camera);
    return;
  }

  if (ui_rect_contains(&ui->top_btn_quit, x, y) &&
      ui_rect_contains(&ui->top_btn_quit, sx, sy)) {
    g_running = 0;
    return;
  }

  /* 3. Right-edge Shutter Button */
  if (ui_rect_contains(&ui->btn_shutter, x, y) &&
      ui_rect_contains(&ui->btn_shutter, sx, sy)) {
    ui->active_popup = LIVE_POPUP_NONE;
    live_ui_request_shutter(camera, save_queue);
    return;
  }

  /* 4. Bottom HUD parameter buttons */
  if (ui_rect_contains(&ui->hud_btn_ss, x, y) &&
      ui_rect_contains(&ui->hud_btn_ss, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_SS);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_iso, x, y) &&
      ui_rect_contains(&ui->hud_btn_iso, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_ISO);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_ev, x, y) &&
      ui_rect_contains(&ui->hud_btn_ev, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_EV);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_wb, x, y) &&
      ui_rect_contains(&ui->hud_btn_wb, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_WB);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_temp, x, y) &&
      ui_rect_contains(&ui->hud_btn_temp, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_TEMP);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_lock, x, y) &&
      ui_rect_contains(&ui->hud_btn_lock, sx, sy)) {
    ui->active_popup = LIVE_POPUP_NONE;
    live_ui_toggle_awb_lock(ui, camera);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_met, x, y) &&
      ui_rect_contains(&ui->hud_btn_met, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_MET);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_qual, x, y) &&
      ui_rect_contains(&ui->hud_btn_qual, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_QUAL);
    return;
  }

  /* 5. If popup was open and user tapped outside, close popup */
  if (ui->active_popup != LIVE_POPUP_NONE) {
    ui->active_popup = LIVE_POPUP_NONE;
    return;
  }

  /* 6. Tapping inside the camera preview viewport triggers Touch AF */
  if (ui_rect_contains(&ui->viewport_rect, x, y) && abs(x - sx) < 24 &&
      abs(y - sy) < 24 && ui->viewport_rect.w > 0 && ui->viewport_rect.h > 0) {
    float nx = (float)(x - ui->viewport_rect.x) / (float)ui->viewport_rect.w;
    float ny = (float)(y - ui->viewport_rect.y) / (float)ui->viewport_rect.h;
    live_ui_trigger_af_at(ui, camera, nx, ny);
  }
}

static void handle_live_wheel_or_step(LiveUiController *ui,
                                      CameraStream *camera, int dir, int px,
                                      int py) {
  if (dir == 0) {
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_ss, px, py) ||
      ui->active_popup == LIVE_POPUP_SS) {
    live_ui_set_shutter_index(ui, camera, ui->shutter_preset_index + dir);
  } else if (ui_rect_contains(&ui->hud_btn_iso, px, py) ||
             ui->active_popup == LIVE_POPUP_ISO) {
    live_ui_set_iso_index(ui, camera, ui->iso_preset_index + dir);
  } else if (ui_rect_contains(&ui->hud_btn_ev, px, py) ||
             ui->active_popup == LIVE_POPUP_EV) {
    live_ui_step_ev(ui, camera, dir);
  } else if (ui_rect_contains(&ui->hud_btn_wb, px, py) ||
             ui->active_popup == LIVE_POPUP_WB) {
    live_ui_set_wb_index(ui, camera, ui->wb_preset_index + dir);
  } else if (ui_rect_contains(&ui->hud_btn_temp, px, py) ||
             ui->active_popup == LIVE_POPUP_TEMP) {
    live_ui_set_kelvin(ui, camera, ui->wb_kelvin_value + dir * 500);
  } else if (ui_rect_contains(&ui->hud_btn_lock, px, py)) {
    live_ui_toggle_awb_lock(ui, camera);
  } else if (ui_rect_contains(&ui->hud_btn_met, px, py) ||
             ui->active_popup == LIVE_POPUP_MET) {
    live_ui_set_metering_index(ui, camera, ui->metering_preset_index + dir);
  } else if (ui_rect_contains(&ui->hud_btn_qual, px, py) ||
             ui->active_popup == LIVE_POPUP_QUAL) {
    live_ui_set_quality_index(ui, ui->quality_preset_index + dir);
  } else {
    /* Default wheel action on live viewfinder: adjust EV compensation */
    live_ui_step_ev(ui, camera, dir);
  }
}

int main(int argc, char *argv[]) {

  SDL_LogPriority log_priority = SDL_LOG_PRIORITY_INFO;
  DcfDirMode dcf_mode = DCF_DIR_MODE_STANDARD;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--date-dir") == 0) {
      dcf_mode = DCF_DIR_MODE_DATE;
    } else if (strcmp(argv[i], "--debug") == 0) {
      log_priority = SDL_LOG_PRIORITY_DEBUG;
    } else if (strcmp(argv[i], "--verbose") == 0) {
      log_priority = SDL_LOG_PRIORITY_VERBOSE;
    }
  }

  SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, log_priority);

  // 1. Initialize SDL2, SDL2_image & SDL2_ttf
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %d: SDL_Init() fail : %s",
                 __FILE__, __LINE__, SDL_GetError());
    exit(1);
  }
  atexit(SDL_Quit);

  if ((IMG_Init(IMG_INIT_JPG) & IMG_INIT_JPG) != IMG_INIT_JPG) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %d: IMG_Init() fail : %s",
                 __FILE__, __LINE__, IMG_GetError());
    exit(2);
  }
  atexit(IMG_Quit);

  if (TTF_Init() < 0) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s: %d: TTF_Init() fail : %s",
                 __FILE__, __LINE__, TTF_GetError());
    exit(3);
  }
  atexit(TTF_Quit);

  // Initialize DCF storage manager (DCIM/100_PICA/IMG_0001.JPG or
  // DCIM/YYYYMMDD/IMG_0001.JPG)
  DcfStorage dcf_storage;
  dcf_storage_init(&dcf_storage, "DCIM", dcf_mode);
  uint64_t free_bytes = 0;
  if (dcf_storage_get_free_space(&dcf_storage, &free_bytes)) {
    printf("[-] 保存先ストレージ空き容量: %.1f MiB (DCFモード: %s)\n",
           (double)free_bytes / (1024.0 * 1024.0),
           dcf_mode == DCF_DIR_MODE_STANDARD ? "DCIM/NNN_PICA"
                                             : "DCIM/YYYYMMDD");
  }

  const int screen_width = 800;
  const int screen_height = 480;

  SDL_Window *window = SDL_CreateWindow(
      "RubusCam", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screen_width,
      screen_height,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "%s: %d: SDL_CreateWindow() fail : %s", __FILE__, __LINE__,
                 SDL_GetError());
    exit(4);
  }

  int cur_win_w = screen_width;
  int cur_win_h = screen_height;
  SDL_GetWindowSize(window, &cur_win_w, &cur_win_h);
  SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s: %d: Window Size : (%d, %d)",
              __FILE__, __LINE__, cur_win_w, cur_win_h);

  // Attempt hardware accelerated renderer with VSync, fallback to default if
  // failed
  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "%s: %d: SDL_CreateRenderer() fail : %s", __FILE__, __LINE__,
                SDL_GetError());
    SDL_LogWarn(
        SDL_LOG_CATEGORY_APPLICATION,
        "%s: %d: "
        "ハードウェアアクセラレータ作成失敗、デフォルトレンダラーを使用します",
        __FILE__, __LINE__);
    renderer = SDL_CreateRenderer(window, -1, 0);
  }

  if (!renderer) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "%s: %d: レンダラーの作成に失敗しました : %s", __FILE__,
                 __LINE__, SDL_GetError());
    SDL_DestroyWindow(window);
    exit(5);
  }

  // Initialize text renderer using SDL2_ttf
  text_renderer_init(NULL, 16);

  // Hide mouse cursor (convenient for console environments)
  SDL_ShowCursor(SDL_DISABLE);

  SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
              "%s: %d: SDL2 / SDL2_image /SDL2_ttf を初期化しました。",
              __FILE__, __LINE__);

  // 2. Initialize CameraStream (libcamera via C wrapper)
  CameraStream *camera = camera_stream_create();
  if (!camera) {
    fprintf(stderr,
            "[エラー] CameraStream オブジェクトの作成に失敗しました。\n");
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    exit(6);
  }

  if (!camera_stream_init(camera, cur_win_w, cur_win_h)) {
    fprintf(stderr, "[エラー] カメラの初期化に失敗しました。\n");
    camera_stream_destroy(camera);
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    exit(7);
  }

  if (!camera_stream_start(camera)) {
    fprintf(stderr, "[エラー] カメラの起動に失敗しました。\n");
    camera_stream_destroy(camera);
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    exit(8);
  }

  printf("=========================================\n");
  printf(" [Space]        : 写真を撮影して非同期保存 "
         "(直後にクイックレビュー表示)\n");
  printf(" [P]            : 再生・ギャラリーモード切替 "
         "(ズーム/グリッド/削除/保護)\n");
  if (camera_stream_is_af_supported(camera)) {
    printf(" [F]            : オートフォーカス再調整 (ワンショットAF)\n");
  }
  printf(" [Up/Down, [/]] : 露出補正 (EV -3.0 ~ +3.0, 1/3ステップ)\n");
  printf(" [I / Shift+I]  : ISO感度切替 (Auto / 100 ~ 3200)\n");
  printf(" [S / Left/Rgt] : シャッター速度切替 (Auto / 1/2000s ~ 2.0s)\n");
  printf(" [W / Shift+W]  : ホワイトバランス切替 (Auto / 晴天 / 曇天 / 日陰 / "
         "白熱灯 / 蛍光灯 / 色温度K)\n");
  printf(" [K / Shift+K]  : 色温度 (K値) 手動調整 (2500K ~ 10000K, "
         "500Kステップ)\n");
  printf(" [L]            : AWBロック切替 "
         "(現在の色合いでホワイトバランスを固定/解除)\n");
  printf(" [M / Shift+M]  : 測光モード切替 (評価測光 Matrix / 中央重点 Center "
         "/ スポット Spot)\n");
  printf(" [J / Shift+J]  : JPEG保存画質切替 (SuperFine 98%% / Fine 90%% / "
         "Normal 80%%)\n");
  printf(" [R]            : 露出・WB・測光・画質設定をデフォルトにリセット\n");
  printf(" [Q]            : アプリを終了\n");
  printf("=========================================\n");

  size_t frame_size = camera_stream_get_frame_size(camera);
  uint8_t *frame_buffer = (uint8_t *)malloc(frame_size);
  if (!frame_buffer) {
    fprintf(stderr, "[エラー] フレームバッファのメモリ確保に失敗しました。\n");
    camera_stream_stop(camera);
    camera_stream_destroy(camera);
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    exit(9);
  }

  uint64_t frame_seq = 0;

  int cam_width = camera_stream_get_width(camera);
  int cam_height = camera_stream_get_height(camera);

  // Setup GPU streaming texture for camera frames
  Uint32 sdl_format = camera_stream_is_bgr(camera) ? SDL_PIXELFORMAT_BGR24
                                                   : SDL_PIXELFORMAT_RGB24;
  SDL_Texture *camera_texture = SDL_CreateTexture(
      renderer, sdl_format, SDL_TEXTUREACCESS_STREAMING, cam_width, cam_height);
  if (!camera_texture) {
    fprintf(stderr,
            "[エラー] カメラストリーミングテクスチャの作成に失敗しました: %s\n",
            SDL_GetError());
    free(frame_buffer);
    camera_stream_stop(camera);
    camera_stream_destroy(camera);
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }

  const uint32_t target_frame_time_ms = 1000 / 30; // ~30 FPS

  // Live View UI & Exposure/WB/Metering/Quality controller state
  LiveUiController live_ui;
  memset(&live_ui, 0, sizeof(live_ui));
  live_ui.wb_kelvin_value = 5500;
  live_ui.quality_preset_index = 1; // Default: Fine (90%)
  live_ui.pointer_x = -1;
  live_ui.pointer_y = -1;
  g_active_jpeg_quality =
      JPEG_QUALITY_PRESETS[live_ui.quality_preset_index].quality;

  // Initialize background async JPEG + Exif save queue
  AsyncSaveQueue save_queue;
  if (!async_save_queue_init(&save_queue)) {
    fprintf(stderr, "[警告] 非同期保存キューの初期化に失敗しました "
                    "(同期保存で動作します)。\n");
  }

  // Initialize Playback & Media Gallery manager
  PlaybackManager playback_mgr;
  playback_manager_init(&playback_mgr, &dcf_storage);
  playback_manager_set_save_queue(&playback_mgr, &save_queue);
  g_active_playback = &playback_mgr;
  g_active_renderer = renderer;
  g_active_camera = camera;

  // Set up signal handler for clean interrupt (Ctrl+C)
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // 3. Main event & GPU rendering loop
  while (g_running) {
    uint32_t frame_start = SDL_GetTicks();
    SDL_GetWindowSize(window, &cur_win_w, &cur_win_h);

    // Process SDL events (Keyboard, Mouse, Mouse Wheel, and Touch Finger)
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        g_running = 0;
      } else if (event.type == SDL_KEYDOWN) {
        if (event.key.keysym.sym == SDLK_q) {
          g_running = 0;
          continue;
        }

        // Delegate key events to Playback / Quick Review manager when active
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_key(&playback_mgr, renderer, &event.key);
          continue;
        }

        bool shift_held = (event.key.keysym.mod & KMOD_SHIFT) != 0;

        switch (event.key.keysym.sym) {
        case SDLK_ESCAPE:
          if (live_ui.active_popup != LIVE_POPUP_NONE) {
            live_ui.active_popup = LIVE_POPUP_NONE;
          }
          break;

        case SDLK_p:
          live_ui.active_popup = LIVE_POPUP_NONE;
          playback_manager_enter_playback(&playback_mgr, renderer, NULL);
          break;

        case SDLK_f:
          live_ui_trigger_af_at(&live_ui, camera, -1.0f, -1.0f);
          break;

        case SDLK_UP:
        case SDLK_RIGHTBRACKET:
          live_ui_step_ev(&live_ui, camera, 1);
          break;

        case SDLK_DOWN:
        case SDLK_LEFTBRACKET:
          live_ui_step_ev(&live_ui, camera, -1);
          break;

        case SDLK_i:
          live_ui_set_iso_index(&live_ui, camera,
                                live_ui.iso_preset_index +
                                    (shift_held ? -1 : 1));
          break;

        case SDLK_s:
        case SDLK_RIGHT: {
          int step = (event.key.keysym.sym == SDLK_s && shift_held) ? -1 : 1;
          live_ui_set_shutter_index(&live_ui, camera,
                                    live_ui.shutter_preset_index + step);
          break;
        }

        case SDLK_LEFT:
          live_ui_set_shutter_index(&live_ui, camera,
                                    live_ui.shutter_preset_index - 1);
          break;

        case SDLK_w:
          live_ui_set_wb_index(&live_ui, camera,
                               live_ui.wb_preset_index + (shift_held ? -1 : 1));
          break;

        case SDLK_k:
          live_ui_set_kelvin(&live_ui, camera,
                             live_ui.wb_kelvin_value +
                                 (shift_held ? -500 : 500));
          break;

        case SDLK_l:
          live_ui_toggle_awb_lock(&live_ui, camera);
          break;

        case SDLK_m:
          live_ui_set_metering_index(&live_ui, camera,
                                     live_ui.metering_preset_index +
                                         (shift_held ? -1 : 1));
          break;

        case SDLK_j:
          live_ui_set_quality_index(&live_ui, live_ui.quality_preset_index +
                                                  (shift_held ? -1 : 1));
          break;

        case SDLK_r:
          live_ui_reset_all(&live_ui, camera);
          break;

        case SDLK_SPACE:
          live_ui.active_popup = LIVE_POPUP_NONE;
          live_ui_request_shutter(camera, &save_queue);
          break;

        default:
          break;
        }
      } else if (event.type == SDL_MOUSEBUTTONDOWN) {
        if (event.button.which == SDL_TOUCH_MOUSEID) {
          continue; /* Handled via SDL_FINGERDOWN to avoid duplicate events */
        }
        if (!live_ui.cursor_visible) {
          SDL_ShowCursor(SDL_ENABLE);
          live_ui.cursor_visible = true;
        }
        live_ui.last_mouse_move_ms = frame_start;
        int mx = event.button.x;
        int my = event.button.y;

        if (event.button.button == SDL_BUTTON_LEFT) {
          if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
            playback_manager_handle_pointer_down(&playback_mgr, renderer, mx,
                                                 my);
          } else {
            live_ui.pointer_x = mx;
            live_ui.pointer_y = my;
            live_ui.pointer_down = true;
            live_ui.down_x = mx;
            live_ui.down_y = my;
          }
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
          /* Right click steps backward on hovered Live HUD button or exits zoom
           */
          if (playback_manager_get_mode(&playback_mgr) == APP_MODE_LIVE) {
            handle_live_wheel_or_step(&live_ui, camera, -1, mx, my);
          }
        }
      } else if (event.type == SDL_MOUSEMOTION) {
        if (event.motion.which == SDL_TOUCH_MOUSEID) {
          continue;
        }
        if (!live_ui.cursor_visible) {
          SDL_ShowCursor(SDL_ENABLE);
          live_ui.cursor_visible = true;
        }
        live_ui.last_mouse_move_ms = frame_start;
        int mx = event.motion.x;
        int my = event.motion.y;
        live_ui.pointer_x = mx;
        live_ui.pointer_y = my;
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_pointer_move(&playback_mgr, renderer, mx, my);
        }
      } else if (event.type == SDL_MOUSEBUTTONUP) {
        if (event.button.which == SDL_TOUCH_MOUSEID) {
          continue;
        }
        live_ui.last_mouse_move_ms = frame_start;
        if (event.button.button == SDL_BUTTON_LEFT) {
          int mx = event.button.x;
          int my = event.button.y;
          if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
            playback_manager_handle_pointer_up(&playback_mgr, renderer, mx, my);
          } else {
            handle_live_pointer_up(&live_ui, camera, &playback_mgr, &save_queue,
                                   renderer, mx, my);
          }
        }
      } else if (event.type == SDL_MOUSEWHEEL) {
        if (event.wheel.which == SDL_TOUCH_MOUSEID || event.wheel.y == 0) {
          continue;
        }
        if (!live_ui.cursor_visible) {
          SDL_ShowCursor(SDL_ENABLE);
          live_ui.cursor_visible = true;
        }
        live_ui.last_mouse_move_ms = frame_start;
        int wy = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
          wy = -wy;
        }
        int mx = live_ui.pointer_x >= 0 ? live_ui.pointer_x : (cur_win_w / 2);
        int my = live_ui.pointer_y >= 0 ? live_ui.pointer_y : (cur_win_h / 2);
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_wheel(&playback_mgr, renderer, wy, mx, my);
        } else {
          handle_live_wheel_or_step(&live_ui, camera, wy > 0 ? 1 : -1, mx, my);
        }
      } else if (event.type == SDL_FINGERDOWN) {
        if (live_ui.cursor_visible) {
          SDL_ShowCursor(SDL_DISABLE);
          live_ui.cursor_visible = false;
        }
        int tx = (int)(event.tfinger.x * (float)cur_win_w);
        int ty = (int)(event.tfinger.y * (float)cur_win_h);
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_pointer_down(&playback_mgr, renderer, tx, ty);
        } else {
          live_ui.pointer_x = tx;
          live_ui.pointer_y = ty;
          live_ui.pointer_down = true;
          live_ui.down_x = tx;
          live_ui.down_y = ty;
        }
      } else if (event.type == SDL_FINGERMOTION) {
        int tx = (int)(event.tfinger.x * (float)cur_win_w);
        int ty = (int)(event.tfinger.y * (float)cur_win_h);
        live_ui.pointer_x = tx;
        live_ui.pointer_y = ty;
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_pointer_move(&playback_mgr, renderer, tx, ty);
        }
      } else if (event.type == SDL_FINGERUP) {
        int tx = (int)(event.tfinger.x * (float)cur_win_w);
        int ty = (int)(event.tfinger.y * (float)cur_win_h);
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_pointer_up(&playback_mgr, renderer, tx, ty);
        } else {
          handle_live_pointer_up(&live_ui, camera, &playback_mgr, &save_queue,
                                 renderer, tx, ty);
        }
        /* Clear hover state after touch release so touch buttons don't stay
         * hovered */
        live_ui.pointer_x = -1;
        live_ui.pointer_y = -1;
        playback_mgr.pointer_x = -1;
        playback_mgr.pointer_y = -1;
      }
    }

    /* Auto-hide mouse cursor after 3 seconds of inactivity */
    if (live_ui.cursor_visible && !live_ui.pointer_down &&
        !playback_mgr.pointer_down &&
        SDL_TICKS_PASSED(frame_start, live_ui.last_mouse_move_ms + 3000U)) {
      SDL_ShowCursor(SDL_DISABLE);
      live_ui.cursor_visible = false;
    }

    // Poll completed asynchronous still capture and dispatch to Quick Review +
    // background save queue
    CameraCapturedStill captured_still;
    if (camera_stream_poll_captured_still(camera, &captured_still)) {
      char filepath[512];
      if (dcf_storage_next_filepath(&dcf_storage, filepath, sizeof(filepath))) {
        // 1. Immediately start Quick Review from in-memory raw buffer (zero
        // disk wait)
        playback_manager_start_quick_review(
            &playback_mgr, renderer, filepath, captured_still.data,
            captured_still.width, captured_still.height, captured_still.stride,
            captured_still.is_bgr, &captured_still.exp_status);

        // 2. Prepare Exif 2.3 metadata
        ExifMetadata exif;
        exif_metadata_init(&exif, camera_stream_get_model_name(camera),
                           captured_still.width, captured_still.height,
                           &captured_still.exp_status);

        // 3. Enqueue for background JPEG encoding & SD card write
        int jpeg_q = JPEG_QUALITY_PRESETS[live_ui.quality_preset_index].quality;
        if (!async_save_queue_push(&save_queue, filepath, captured_still.data,
                                   captured_still.size, captured_still.width,
                                   captured_still.height, captured_still.stride,
                                   captured_still.is_bgr, jpeg_q, &exif)) {
          // Fallback to synchronous save if background queue was unavailable
          save_still_jpeg(filepath, captured_still.data, captured_still.width,
                          captured_still.height, captured_still.stride,
                          captured_still.is_bgr);
          free(captured_still.data);
        }
      } else {
        free(captured_still.data);
      }
    }

    // Update playback/quick-review timers
    playback_manager_update(&playback_mgr, renderer);

    if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
      playback_manager_render(&playback_mgr, renderer, cur_win_w, cur_win_h);
    } else {
      // 4. Retrieve latest camera frame and update GPU texture
      bool has_frame = camera_stream_get_latest_frame(camera, frame_buffer,
                                                      frame_size, &frame_seq);
      if (has_frame) {
        int stride = camera_stream_get_stride(camera);
        if (stride == 0) {
          stride = cam_width * 3;
        }
        SDL_UpdateTexture(camera_texture, NULL, frame_buffer, stride);
      }

      // 5. Render camera frame with aspect-ratio preserving viewport
      calculate_viewport_rect(cam_width, cam_height, cur_win_w, cur_win_h,
                              &live_ui.viewport_rect);

      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
      SDL_RenderClear(renderer);

      SDL_RenderCopy(renderer, camera_texture, NULL, &live_ui.viewport_rect);

      // 6. Draw interactive touch/mouse Live UI, HUD
      // buttons, and popups
      CameraExposureStatus exp_status;
      camera_stream_get_exposure_status(camera, &exp_status);
      int pending_saves = async_save_queue_get_pending_count(&save_queue) +
                          (camera_stream_is_still_pending(camera) ? 1 : 0);
      render_live_ui(renderer, &live_ui, camera, &exp_status, pending_saves,
                     cur_win_w, cur_win_h);

      playback_manager_render_toast(&playback_mgr, renderer, cur_win_w,
                                    cur_win_h);
    }

    // 7. Present frame to physical display
    SDL_RenderPresent(renderer);

    // 8. Maintain ~30 FPS frame rate
    uint32_t elapsed = SDL_GetTicks() - frame_start;
    if (elapsed < target_frame_time_ms) {
      SDL_Delay(target_frame_time_ms - elapsed);
    }
  }

  // 9. Cleanup resources
  SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s: %d: 終了処理を実行中...",
              __FILE__, __LINE__);
  if (async_save_queue_get_pending_count(&save_queue) > 0) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "%s: %d: バックグラウンド保存キューの完了を待機中...", __FILE__,
                __LINE__);
    async_save_queue_wait_idle(&save_queue);
  }
  async_save_queue_destroy(&save_queue);
  g_active_playback = NULL;
  g_active_renderer = NULL;
  g_active_camera = NULL;
  playback_manager_destroy(&playback_mgr);
  free(frame_buffer);
  camera_stream_stop(camera);
  camera_stream_destroy(camera);

  text_renderer_destroy();

  if (camera_texture) {
    SDL_DestroyTexture(camera_texture);
  }
  if (renderer) {
    SDL_DestroyRenderer(renderer);
  }
  if (window) {
    SDL_DestroyWindow(window);
  }

  SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
              "%s: %d: アプリケーションを終了します。", __FILE__, __LINE__);

  exit(0);
  return 0;
}
