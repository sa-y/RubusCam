#include "live_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text_renderer.h"

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

typedef struct GridPreset {
  LiveGridMode mode;
  const char *name;
  const char *hud_label;
  const char *popup_label;
} GridPreset;

static const GridPreset GRID_PRESETS[] = {
    {LIVE_GRID_OFF, "Off (非表示)", "Off", "Off"},
    {LIVE_GRID_THIRDS, "Rule of Thirds (三分割グリッド)", "3x3",
     "3x3 (Thirds)"},
    {LIVE_GRID_GOLDEN, "Golden Ratio (黄金比グリッド)", "Golden",
     "Phi (Golden)"},
    {LIVE_GRID_CROSSHAIR, "Center Crosshair (十字センターマーク)", "Cross",
     "Cross (+)"},
    {LIVE_GRID_THIRDS_CROSS, "3x3 + Crosshair (三分割 + 十字マーク)",
     "3x3+Cross", "3x3 + Cross"},
};
static const int GRID_PRESET_COUNT =
    (int)(sizeof(GRID_PRESETS) / sizeof(GRID_PRESETS[0]));

typedef struct AspectPreset {
  LiveAspectMask mask;
  int aspect_w;
  int aspect_h;
  const char *name;
  const char *hud_label;
  const char *popup_label;
} AspectPreset;

static const AspectPreset ASPECT_PRESETS[] = {
    {LIVE_ASPECT_NATIVE, 0, 0, "Native (センサー全域)", "Native",
     "Native (Full)"},
    {LIVE_ASPECT_16_9, 16, 9, "16:9 (ワイド)", "16:9", "16:9"},
    {LIVE_ASPECT_4_3, 4, 3, "4:3 (スタンダード)", "4:3", "4:3"},
    {LIVE_ASPECT_3_2, 3, 2, "3:2 (35mmフィルム比)", "3:2", "3:2"},
    {LIVE_ASPECT_1_1, 1, 1, "1:1 (スクエア)", "1:1", "1:1"},
};
static const int ASPECT_PRESET_COUNT =
    (int)(sizeof(ASPECT_PRESETS) / sizeof(ASPECT_PRESETS[0]));

typedef struct DrivePreset {
  LiveDriveMode mode;
  const char *name;
  const char *hud_label;
  const char *popup_label;
} DrivePreset;

static const DrivePreset DRIVE_PRESETS[] = {
    {LIVE_DRIVE_SINGLE, "Single (単写)", "Single", "Single"},
    {LIVE_DRIVE_TIMER_2S, "Self-Timer 2s (2秒セルフタイマー)", "Timer 2s",
     "Timer 2s"},
    {LIVE_DRIVE_TIMER_10S, "Self-Timer 10s (10秒セルフタイマー)", "Timer 10s",
     "Timer 10s"},
    {LIVE_DRIVE_BURST, "Burst (連写モード)", "Burst", "Burst"},
    {LIVE_DRIVE_INTERVAL, "Interval (インターバル撮影)", "Interval",
     "Interval"},
    {LIVE_DRIVE_AEB, "AEB (3枚オートブラケティング)", "AEB(3f)", "AEB (3f)"},
};
static const int DRIVE_PRESET_COUNT =
    (int)(sizeof(DRIVE_PRESETS) / sizeof(DRIVE_PRESETS[0]));

typedef struct IntervalSecPreset {
  int seconds;
  const char *label;
} IntervalSecPreset;

static const IntervalSecPreset INTERVAL_SEC_PRESETS[] = {
    {1, "1s"},     {2, "2s"},     {3, "3s"},     {5, "5s"},
    {10, "10s"},   {30, "30s"},   {60, "1m"},    {300, "5m"},
    {600, "10m"},  {1800, "30m"}, {3600, "60m"},
};
static const int INTERVAL_SEC_PRESET_COUNT =
    (int)(sizeof(INTERVAL_SEC_PRESETS) / sizeof(INTERVAL_SEC_PRESETS[0]));

typedef struct IntervalShotPreset {
  int count; /* 0 = Unlimited */
  const char *label;
} IntervalShotPreset;

static const IntervalShotPreset INTERVAL_SHOT_PRESETS[] = {
    {5, "5f"},     {10, "10f"},   {30, "30f"},     {60, "60f"},
    {120, "120f"}, {300, "300f"}, {0, "Inf"},
};
static const int INTERVAL_SHOT_PRESET_COUNT =
    (int)(sizeof(INTERVAL_SHOT_PRESETS) / sizeof(INTERVAL_SHOT_PRESETS[0]));

typedef struct AebStepPreset {
  int ev_steps; /* in 1/3 EV increments */
  const char *label;
  const char *desc;
} AebStepPreset;

static const AebStepPreset AEB_STEP_PRESETS[] = {
    {1, "+/-0.3 EV", "±0.3 EV (1/3 EV)"},
    {2, "+/-0.7 EV", "±0.7 EV (2/3 EV)"},
    {3, "+/-1.0 EV", "±1.0 EV"},
    {6, "+/-2.0 EV", "±2.0 EV"},
};
static const int AEB_STEP_PRESET_COUNT =
    (int)(sizeof(AEB_STEP_PRESETS) / sizeof(AEB_STEP_PRESETS[0]));

static void live_ui_play_beep(LiveUiController *ui, float freq_hz,
                              int duration_ms, float volume) {
  if (!ui || ui->audio_dev == 0 || duration_ms <= 0 || freq_hz <= 20.0f) {
    return;
  }
  if (duration_ms > 500) {
    duration_ms = 500;
  }
  const int sample_rate = 44100;
  int total_samples = (sample_rate * duration_ms) / 1000;
  if (total_samples <= 0) {
    return;
  }
  int16_t *buf = (int16_t *)malloc((size_t)total_samples * sizeof(int16_t));
  if (!buf) {
    return;
  }
  int fade_samples = (sample_rate * 5) / 1000; /* 5ms fade in/out */
  if (fade_samples * 2 > total_samples) {
    fade_samples = total_samples / 2;
  }
  const float two_pi = 6.283185307f;
  float amp = volume * 28000.0f;
  for (int i = 0; i < total_samples; ++i) {
    float env = 1.0f;
    if (fade_samples > 0) {
      if (i < fade_samples) {
        env = (float)i / (float)fade_samples;
      } else if (i >= total_samples - fade_samples) {
        env = (float)(total_samples - 1 - i) / (float)fade_samples;
      }
    }
    float s = sinf(two_pi * freq_hz * ((float)i / (float)sample_rate));
    buf[i] = (int16_t)(s * amp * env);
  }
  SDL_QueueAudio((SDL_AudioDeviceID)ui->audio_dev, buf,
                 (Uint32)((size_t)total_samples * sizeof(int16_t)));
  free(buf);
}

static bool live_ui_cancel_active_drive(LiveUiController *ui,
                                        CameraStream *camera) {
  if (!ui) {
    return false;
  }
  bool cancelled = false;
  if (ui->timer_active) {
    ui->timer_active = false;
    if (ui->audio_dev != 0) {
      SDL_ClearQueuedAudio((SDL_AudioDeviceID)ui->audio_dev);
    }
    live_ui_play_beep(ui, 440.0f, 80, 0.20f);
    printf("[-] セルフタイマーをキャンセルしました。\n");
    cancelled = true;
  }
  if (ui->burst_active || ui->burst_held_by_key || ui->burst_held_by_pointer) {
    ui->burst_active = false;
    ui->burst_held_by_key = false;
    ui->burst_keyup_pending = false;
    ui->burst_held_by_pointer = false;
    if (camera) {
      camera_stream_set_burst_mode(camera, false);
    }
    printf("[-] 連写終了 (計 %d 枚撮影)。\n", ui->burst_shot_count);
    cancelled = true;
  }
  if (ui->interval_active) {
    ui->interval_active = false;
    ui->interval_waiting_capture = false;
    live_ui_play_beep(ui, 440.0f, 80, 0.20f);
    printf("[-] インターバル撮影を停止しました (計 %d 枚撮影)。\n",
           ui->interval_shots_taken);
    cancelled = true;
  }
  if (ui->aeb_active) {
    ui->aeb_active = false;
    ui->aeb_waiting_settle = false;
    ui->aeb_waiting_capture = false;
    ui->ev_step_index = ui->aeb_base_ev_step;
    if (camera) {
      camera_stream_set_ev(camera, (float)ui->ev_step_index / 3.0f);
    }
    live_ui_play_beep(ui, 440.0f, 80, 0.20f);
    printf("[-] オートブラケティング (AEB) を中断し、基準EV (%+.1f) "
           "を復元しました。\n",
           (float)ui->ev_step_index / 3.0f);
    cancelled = true;
  }
  return cancelled;
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
  printf("[-] JPEG保存画質: %s\n",
         JPEG_QUALITY_PRESETS[ui->quality_preset_index].name);
}

static void live_ui_set_grid_index(LiveUiController *ui, int idx) {
  ui->grid_mode_index =
      (idx % GRID_PRESET_COUNT + GRID_PRESET_COUNT) % GRID_PRESET_COUNT;
  printf("[-] 構図ガイドグリッド: %s\n",
         GRID_PRESETS[ui->grid_mode_index].name);
}

static void live_ui_set_aspect_index(LiveUiController *ui, int idx) {
  ui->aspect_mask_index =
      (idx % ASPECT_PRESET_COUNT + ASPECT_PRESET_COUNT) % ASPECT_PRESET_COUNT;
  printf("[-] アスペクト比マスク (撮影クロップ連動): %s\n",
         ASPECT_PRESETS[ui->aspect_mask_index].name);
}

static void live_ui_set_drive_index(LiveUiController *ui, CameraStream *camera,
                                    int idx) {
  live_ui_cancel_active_drive(ui, camera);
  ui->drive_mode_index =
      (idx % DRIVE_PRESET_COUNT + DRIVE_PRESET_COUNT) % DRIVE_PRESET_COUNT;
  printf("[-] ドライブモード: %s\n", DRIVE_PRESETS[ui->drive_mode_index].name);
}

static void live_ui_set_interval_sec_index(LiveUiController *ui, int idx) {
  ui->interval_sec_index =
      (idx % INTERVAL_SEC_PRESET_COUNT + INTERVAL_SEC_PRESET_COUNT) %
      INTERVAL_SEC_PRESET_COUNT;
  printf("[-] インターバル撮影間隔: %s (%d秒)\n",
         INTERVAL_SEC_PRESETS[ui->interval_sec_index].label,
         INTERVAL_SEC_PRESETS[ui->interval_sec_index].seconds);
}

static void live_ui_set_interval_shots_index(LiveUiController *ui, int idx) {
  ui->interval_shots_index =
      (idx % INTERVAL_SHOT_PRESET_COUNT + INTERVAL_SHOT_PRESET_COUNT) %
      INTERVAL_SHOT_PRESET_COUNT;
  int cnt = INTERVAL_SHOT_PRESETS[ui->interval_shots_index].count;
  if (cnt == 0) {
    printf("[-] インターバル総撮影枚数: 無制限 (Inf)\n");
  } else {
    printf("[-] インターバル総撮影枚数: %d枚\n", cnt);
  }
}

static void live_ui_set_aeb_step_index(LiveUiController *ui, int idx) {
  ui->aeb_step_index = (idx % AEB_STEP_PRESET_COUNT + AEB_STEP_PRESET_COUNT) %
                       AEB_STEP_PRESET_COUNT;
  printf("[-] オートブラケティング (AEB) ステップ幅: %s\n",
         AEB_STEP_PRESETS[ui->aeb_step_index].desc);
}

static void live_ui_reset_all(LiveUiController *ui, CameraStream *camera) {
  live_ui_cancel_active_drive(ui, camera);
  ui->ev_step_index = 0;
  ui->iso_preset_index = 0;
  ui->shutter_preset_index = 0;
  ui->wb_preset_index = 0;
  ui->wb_kelvin_value = 5500;
  ui->awb_locked = false;
  ui->metering_preset_index = 0;
  ui->quality_preset_index = 1;
  ui->grid_mode_index = 0;
  ui->aspect_mask_index = 0;
  ui->drive_mode_index = 0;
  ui->interval_sec_index = 3;   /* 5s */
  ui->interval_shots_index = 2; /* 30 shots */
  ui->aeb_step_index = 2;       /* +/-1.0 EV */
  ui->active_popup = LIVE_POPUP_NONE;
  ui->af_box_visible = false;
  camera_stream_set_ev(camera, 0.0f);
  camera_stream_set_iso(camera, 0);
  camera_stream_set_shutter_speed(camera, 0);
  camera_stream_set_wb_mode(camera, CAMERA_WB_MODE_AUTO);
  camera_stream_set_awb_lock(camera, false);
  camera_stream_set_metering_mode(camera, CAMERA_METERING_MATRIX);
  if (camera_stream_is_af_supported(camera)) {
    camera_stream_trigger_af(camera);
  }
  printf("[-] 露出・WB・測光・画質・構図・ドライブ設定をリセットしました "
         "(EV: 0.0, ISO: Auto, SS: Auto, WB: Auto, Met: Matrix, JPG: %s, "
         "Grid: Off, Aspect: Native, Drive: Single)\n",
         JPEG_QUALITY_PRESETS[ui->quality_preset_index].name);
}

static void render_aspect_mask(SDL_Renderer *renderer, LiveUiController *ui) {
  if (!renderer || !ui) {
    return;
  }

  ui->active_frame_rect = ui->viewport_rect;
  if (ui->viewport_rect.w <= 0 || ui->viewport_rect.h <= 0 ||
      ui->aspect_mask_index <= 0 ||
      ui->aspect_mask_index >= ASPECT_PRESET_COUNT) {
    return;
  }

  int aspect_w = ASPECT_PRESETS[ui->aspect_mask_index].aspect_w;
  int aspect_h = ASPECT_PRESETS[ui->aspect_mask_index].aspect_h;
  if (aspect_w <= 0 || aspect_h <= 0) {
    return;
  }

  ui_calc_aspect_fit_rect(aspect_w, aspect_h, &ui->viewport_rect,
                          &ui->active_frame_rect);

  const SDL_Rect *vp = &ui->viewport_rect;
  const SDL_Rect *af = &ui->active_frame_rect;

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 175);

  /* Top letterbox bar */
  if (af->y > vp->y) {
    SDL_Rect top_bar = {vp->x, vp->y, vp->w, af->y - vp->y};
    SDL_RenderFillRect(renderer, &top_bar);
  }
  /* Bottom letterbox bar */
  int af_bottom = af->y + af->h;
  int vp_bottom = vp->y + vp->h;
  if (af_bottom < vp_bottom) {
    SDL_Rect bot_bar = {vp->x, af_bottom, vp->w, vp_bottom - af_bottom};
    SDL_RenderFillRect(renderer, &bot_bar);
  }
  /* Left pillarbox bar */
  if (af->x > vp->x) {
    SDL_Rect left_bar = {vp->x, af->y, af->x - vp->x, af->h};
    SDL_RenderFillRect(renderer, &left_bar);
  }
  /* Right pillarbox bar */
  int af_right = af->x + af->w;
  int vp_right = vp->x + vp->w;
  if (af_right < vp_right) {
    SDL_Rect right_bar = {af_right, af->y, vp_right - af_right, af->h};
    SDL_RenderFillRect(renderer, &right_bar);
  }

  /* Subtle frame border around active aspect area */
  SDL_SetRenderDrawColor(renderer, 200, 220, 240, 150);
  SDL_RenderDrawRect(renderer, af);
}

static void draw_guide_line(SDL_Renderer *renderer, int x1, int y1, int x2,
                            int y2, SDL_Color fg) {
  int sx_off = (x1 == x2) ? 1 : 0;
  int sy_off = (y1 == y2) ? 1 : ((x1 != x2) ? 1 : 0);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 145);
  SDL_RenderDrawLine(renderer, x1 + sx_off, y1 + sy_off, x2 + sx_off,
                     y2 + sy_off);
  SDL_SetRenderDrawColor(renderer, fg.r, fg.g, fg.b, fg.a);
  SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
}

static void render_composition_grid(SDL_Renderer *renderer,
                                    const LiveUiController *ui) {
  if (!renderer || !ui || ui->grid_mode_index <= 0 ||
      ui->grid_mode_index >= GRID_PRESET_COUNT) {
    return;
  }

  const SDL_Rect *frame = (ui->active_frame_rect.w > 0 &&
                           ui->active_frame_rect.h > 0)
                              ? &ui->active_frame_rect
                              : &ui->viewport_rect;
  if (frame->w <= 8 || frame->h <= 8) {
    return;
  }

  LiveGridMode mode = GRID_PRESETS[ui->grid_mode_index].mode;
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  if (mode == LIVE_GRID_THIRDS || mode == LIVE_GRID_THIRDS_CROSS) {
    SDL_Color fg = {255, 255, 255, 175};
    int x1 = frame->x + frame->w / 3;
    int x2 = frame->x + (frame->w * 2) / 3;
    int y1 = frame->y + frame->h / 3;
    int y2 = frame->y + (frame->h * 2) / 3;

    draw_guide_line(renderer, x1, frame->y, x1, frame->y + frame->h - 1, fg);
    draw_guide_line(renderer, x2, frame->y, x2, frame->y + frame->h - 1, fg);
    draw_guide_line(renderer, frame->x, y1, frame->x + frame->w - 1, y1, fg);
    draw_guide_line(renderer, frame->x, y2, frame->x + frame->w - 1, y2, fg);
  } else if (mode == LIVE_GRID_GOLDEN) {
    /* Golden ratio conjugate: 1 / (1 + phi) ≈ 0.381966f, phi / (1 + phi) ≈
     * 0.618034f */
    SDL_Color fg = {255, 225, 130, 185};
    int x1 = frame->x + (int)((float)frame->w * 0.381966f + 0.5f);
    int x2 = frame->x + (int)((float)frame->w * 0.618034f + 0.5f);
    int y1 = frame->y + (int)((float)frame->h * 0.381966f + 0.5f);
    int y2 = frame->y + (int)((float)frame->h * 0.618034f + 0.5f);

    draw_guide_line(renderer, x1, frame->y, x1, frame->y + frame->h - 1, fg);
    draw_guide_line(renderer, x2, frame->y, x2, frame->y + frame->h - 1, fg);
    draw_guide_line(renderer, frame->x, y1, frame->x + frame->w - 1, y1, fg);
    draw_guide_line(renderer, frame->x, y2, frame->x + frame->w - 1, y2, fg);
  }

  if (mode == LIVE_GRID_CROSSHAIR || mode == LIVE_GRID_THIRDS_CROSS) {
    SDL_Color fg = {255, 255, 255, 215};
    int cx = frame->x + frame->w / 2;
    int cy = frame->y + frame->h / 2;
    int arm = 24;
    int gap = 6;

    draw_guide_line(renderer, cx - arm, cy, cx - gap, cy, fg);
    draw_guide_line(renderer, cx + gap, cy, cx + arm, cy, fg);
    draw_guide_line(renderer, cx, cy - arm, cx, cy - gap, fg);
    draw_guide_line(renderer, cx, cy + gap, cx, cy + arm, fg);

    /* Center point tick */
    draw_guide_line(renderer, cx - 1, cy, cx + 1, cy, fg);
    draw_guide_line(renderer, cx, cy - 1, cx, cy + 1, fg);
  }
}

static bool live_ui_trigger_single_capture(LiveUiController *ui,
                                           CameraStream *camera,
                                           AsyncSaveQueue *save_queue) {
  if (camera_stream_is_still_pending(camera)) {
    printf("[-] 撮影処理中です...\n");
    return false;
  }
  if (async_save_queue_get_pending_count(save_queue) >=
      ASYNC_SAVE_QUEUE_CAPACITY) {
    fprintf(stderr,
            "[警告] 保存キューが満杯です。書き込み完了をお待ちください。\n");
    return false;
  }
  if (camera_stream_request_still(camera)) {
    live_ui_play_beep(ui, 1320.0f, 55, 0.25f);
    return true;
  }
  return false;
}

static void live_ui_start_burst(LiveUiController *ui, CameraStream *camera,
                                AsyncSaveQueue *save_queue) {
  if (!ui || !camera || ui->burst_active) {
    return;
  }
  if (async_save_queue_get_pending_count(save_queue) >=
      ASYNC_SAVE_QUEUE_CAPACITY) {
    fprintf(stderr,
            "[警告] 保存キューが満杯です。書き込み完了をお待ちください。\n");
    return;
  }
  ui->active_popup = LIVE_POPUP_NONE;
  ui->burst_active = true;
  ui->burst_shot_count = 0;
  camera_stream_set_burst_mode(camera, true);
  printf("[-] 連写モード開始 (シャッター長押し中)...\n");
}

static void live_ui_stop_burst(LiveUiController *ui, CameraStream *camera) {
  if (!ui || !ui->burst_active) {
    return;
  }
  ui->burst_active = false;
  if (camera) {
    camera_stream_set_burst_mode(camera, false);
  }
  printf("[-] 連写モード終了 (計 %d 枚撮影)。\n", ui->burst_shot_count);
}

static void live_ui_request_shutter(LiveUiController *ui, CameraStream *camera,
                                    AsyncSaveQueue *save_queue) {
  if (!ui || !camera) {
    return;
  }

  /* If a timer, interval, or AEB sequence is currently active, pressing
   * shutter cancels/stops it. */
  if (ui->timer_active || ui->interval_active || ui->aeb_active) {
    live_ui_cancel_active_drive(ui, camera);
    return;
  }

  LiveDriveMode dmode = DRIVE_PRESETS[ui->drive_mode_index].mode;
  switch (dmode) {
  case LIVE_DRIVE_TIMER_2S:
  case LIVE_DRIVE_TIMER_10S: {
    uint32_t dur_ms = (dmode == LIVE_DRIVE_TIMER_2S) ? 2000U : 10000U;
    ui->timer_active = true;
    ui->timer_duration_ms = dur_ms;
    ui->timer_start_ms = SDL_GetTicks();
    ui->timer_last_beep_ms = ui->timer_start_ms;
    live_ui_play_beep(ui, 880.0f, 70, 0.25f);
    printf("[-] セルフタイマー開始 (%u秒)... "
           "[Space/Esc/CANCELで中断]\n",
           dur_ms / 1000U);
    break;
  }

  case LIVE_DRIVE_BURST:
    /* If triggered as a single tap without hold state, take 1 shot */
    if (!ui->burst_active) {
      live_ui_trigger_single_capture(ui, camera, save_queue);
    }
    break;

  case LIVE_DRIVE_INTERVAL: {
    int sec = INTERVAL_SEC_PRESETS[ui->interval_sec_index].seconds;
    int target = INTERVAL_SHOT_PRESETS[ui->interval_shots_index].count;
    ui->interval_active = true;
    ui->interval_shots_taken = 0;
    ui->interval_target_shots = target;
    ui->interval_step_ms = (uint32_t)sec * 1000U;
    ui->interval_next_shot_ms = SDL_GetTicks();
    ui->interval_waiting_capture = false;
    live_ui_play_beep(ui, 987.77f, 65, 0.25f);
    if (target > 0) {
      printf("[-] インターバル撮影を開始しました (間隔: %d秒, 総枚数: %d枚)\n",
             sec, target);
    } else {
      printf("[-] インターバル撮影を開始しました (間隔: %d秒, 枚数: 無制限)\n",
             sec);
    }
    break;
  }

  case LIVE_DRIVE_AEB: {
    if (camera_stream_is_still_pending(camera) ||
        async_save_queue_get_pending_count(save_queue) >=
            ASYNC_SAVE_QUEUE_CAPACITY - 2) {
      fprintf(stderr,
              "[警告] AEB撮影を開始できません (カメラまたは保存キューが処理中"
              "です)。\n");
      return;
    }
    int step = AEB_STEP_PRESETS[ui->aeb_step_index].ev_steps;
    int base = ui->ev_step_index;
    int under = base - step;
    if (under < -9) {
      under = -9;
    }
    int over = base + step;
    if (over > 9) {
      over = 9;
    }
    ui->aeb_base_ev_step = base;
    ui->aeb_target_ev_steps[0] = base;
    ui->aeb_target_ev_steps[1] = under;
    ui->aeb_target_ev_steps[2] = over;
    ui->aeb_shot_index = 0;
    ui->aeb_active = true;

    printf("[-] オートブラケティング (AEB) 3枚連続撮影を開始します "
           "(1枚目: %+.1f EV, 2枚目: %+.1f EV, 3枚目: %+.1f EV)...\n",
           (float)base / 3.0f, (float)under / 3.0f, (float)over / 3.0f);

    if (camera_stream_request_still(camera)) {
      ui->aeb_waiting_settle = false;
      ui->aeb_waiting_capture = true;
      live_ui_play_beep(ui, 1200.0f, 45, 0.24f);
    } else {
      ui->aeb_waiting_settle = true;
      ui->aeb_waiting_capture = false;
      ui->aeb_settle_start_seq = camera_stream_get_frame_sequence(camera);
      ui->aeb_settle_start_ms = SDL_GetTicks();
    }
    break;
  }

  case LIVE_DRIVE_SINGLE:
  default:
    live_ui_trigger_single_capture(ui, camera, save_queue);
    break;
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

static void render_drive_status_overlay(SDL_Renderer *renderer,
                                        const LiveUiController *ui) {
  if (!renderer || !ui) {
    return;
  }

  const SDL_Rect *frame = (ui->active_frame_rect.w > 0 &&
                           ui->active_frame_rect.h > 0)
                              ? &ui->active_frame_rect
                              : &ui->viewport_rect;
  if (frame->w <= 0 || frame->h <= 0) {
    return;
  }

  uint32_t now = SDL_GetTicks();

  /* 1. Self-Timer Countdown Center Overlay */
  if (ui->timer_active) {
    uint32_t elapsed = now - ui->timer_start_ms;
    uint32_t rem_ms =
        (elapsed < ui->timer_duration_ms) ? (ui->timer_duration_ms - elapsed) : 0;
    uint32_t rem_sec = (rem_ms + 999U) / 1000U;
    if (rem_sec < 1U) {
      rem_sec = 1U;
    }

    int box_w = 240;
    int box_h = 112;
    int cx = frame->x + frame->w / 2;
    int cy = frame->y + frame->h / 2;
    SDL_Rect panel = {cx - box_w / 2, cy - box_h / 2, box_w, box_h};

    bool fast_blink = (rem_ms <= 2000U) && (((now / 125U) & 1U) != 0U);
    SDL_Color bg = {12, 16, 24, 225};
    SDL_Color bd = fast_blink ? (SDL_Color){255, 75, 75, 255}
                              : (SDL_Color){255, 210, 60, 240};
    text_renderer_draw_panel(renderer, &panel, bg, bd);

    /* Simulated tally LED indicator dot */
    SDL_Rect led_rect = {panel.x + 16, panel.y + 14, 12, 12};
    bool led_on = (rem_ms <= 2000U) ? (((now / 125U) & 1U) == 0U)
                                    : (((now / 500U) & 1U) == 0U);
    SDL_SetRenderDrawColor(renderer, led_on ? 255 : 90, led_on ? 50 : 30,
                           led_on ? 50 : 30, 255);
    SDL_RenderFillRect(renderer, &led_rect);

    char hdr[48];
    snprintf(hdr, sizeof(hdr), "SELF-TIMER (%us)", ui->timer_duration_ms / 1000U);
    SDL_Color hdr_col = {255, 225, 120, 255};
    text_renderer_draw_text(renderer, hdr, panel.x + 36, panel.y + 10, hdr_col,
                            NULL, NULL);

    char count_str[64];
    snprintf(count_str, sizeof(count_str), ">>>  %u s  <<<", rem_sec);
    int tw = 0;
    int th = 0;
    text_renderer_measure_text(count_str, &tw, &th);
    SDL_Color num_col = fast_blink ? (SDL_Color){255, 110, 110, 255}
                                   : (SDL_Color){255, 255, 255, 255};
    text_renderer_draw_text(renderer, count_str, panel.x + (box_w - tw) / 2,
                            panel.y + 42, num_col, NULL, NULL);

    /* Progress bar inside countdown panel */
    int bar_margin = 18;
    int bar_w = box_w - bar_margin * 2;
    int bar_h = 12;
    SDL_Rect bar_bg = {panel.x + bar_margin, panel.y + box_h - 26, bar_w,
                       bar_h};
    SDL_SetRenderDrawColor(renderer, 40, 48, 62, 230);
    SDL_RenderFillRect(renderer, &bar_bg);

    float ratio = (ui->timer_duration_ms > 0)
                      ? ((float)rem_ms / (float)ui->timer_duration_ms)
                      : 0.0f;
    if (ratio < 0.0f) {
      ratio = 0.0f;
    } else if (ratio > 1.0f) {
      ratio = 1.0f;
    }
    int fill_w = (int)((float)bar_w * ratio + 0.5f);
    if (fill_w > 0) {
      SDL_Rect bar_fg = {bar_bg.x, bar_bg.y, fill_w, bar_h};
      SDL_SetRenderDrawColor(renderer, bd.r, bd.g, bd.b, 240);
      SDL_RenderFillRect(renderer, &bar_fg);
    }
    SDL_SetRenderDrawColor(renderer, 180, 195, 215, 180);
    SDL_RenderDrawRect(renderer, &bar_bg);
  }

  /* 2. Active Burst / Interval / AEB Banner at Top-Center of Frame */
  char status_msg[96];
  status_msg[0] = '\0';
  SDL_Color fg = {255, 255, 255, 255};
  SDL_Color bg = {18, 24, 34, 230};
  SDL_Color bd = {90, 220, 255, 245};

  if (ui->burst_active) {
    snprintf(status_msg, sizeof(status_msg), "BURST SHOOTING: %d shots",
             ui->burst_shot_count);
    fg = (SDL_Color){255, 240, 240, 255};
    bg = (SDL_Color){150, 24, 32, 235};
    bd = (SDL_Color){255, 120, 120, 255};
  } else if (ui->interval_active) {
    uint32_t next_s = 0;
    if (!ui->interval_waiting_capture &&
        !SDL_TICKS_PASSED(now, ui->interval_next_shot_ms)) {
      next_s = (ui->interval_next_shot_ms - now + 999U) / 1000U;
    }
    if (ui->interval_target_shots > 0) {
      snprintf(status_msg, sizeof(status_msg),
               "INTERVAL: %d / %d shots (Next: %us)", ui->interval_shots_taken,
               ui->interval_target_shots, next_s);
    } else {
      snprintf(status_msg, sizeof(status_msg),
               "INTERVAL: %d / Inf shots (Next: %us)", ui->interval_shots_taken,
               next_s);
    }
    fg = (SDL_Color){230, 255, 235, 255};
    bg = (SDL_Color){18, 95, 48, 235};
    bd = (SDL_Color){100, 245, 140, 250};
  } else if (ui->aeb_active) {
    float cur_ev = (float)ui->aeb_target_ev_steps[ui->aeb_shot_index] / 3.0f;
    snprintf(status_msg, sizeof(status_msg), "AEB BRACKETING: %d / 3 (%+.1f EV)",
             ui->aeb_shot_index + 1, cur_ev);
    fg = (SDL_Color){255, 240, 130, 255};
    bg = (SDL_Color){95, 65, 10, 235};
    bd = (SDL_Color){255, 205, 55, 250};
  }

  if (status_msg[0] != '\0') {
    int tw = 0;
    int th = 0;
    text_renderer_measure_text(status_msg, &tw, &th);
    int bx = frame->x + (frame->w - (tw + 24)) / 2;
    int by = frame->y + 52;
    if (by < 54) {
      by = 54;
    }
    text_renderer_draw_badge(renderer, status_msg, bx, by, fg, bg, bd, NULL);
  }
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
  ui->popup_sub_minus = (SDL_Rect){0, 0, 0, 0};
  ui->popup_sub_plus = (SDL_Rect){0, 0, 0, 0};
  ui->popup_sub_reset = (SDL_Rect){0, 0, 0, 0};
  ui->popup_sub_count = 0;
  ui->popup_opt_count = 0;

  if (ui->active_popup == LIVE_POPUP_NONE) {
    return;
  }

  /* Determine right edge of the left vertical settings column across all 11
   * buttons */
  const SDL_Rect *left_btns[11] = {
      &ui->hud_btn_ss,     &ui->hud_btn_iso,   &ui->hud_btn_ev,
      &ui->hud_btn_wb,     &ui->hud_btn_temp,  &ui->hud_btn_lock,
      &ui->hud_btn_met,    &ui->hud_btn_qual,  &ui->hud_btn_grid,
      &ui->hud_btn_aspect, &ui->hud_btn_drive,
  };
  int left_col_right = 152;
  for (int i = 0; i < 11; ++i) {
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
  case LIVE_POPUP_GRID:
    title = "Composition Guide Grid [G]";
    anchor_y = ui->hud_btn_grid.y;
    break;
  case LIVE_POPUP_ASPECT:
    title = "Aspect Ratio (Mask & Crop) [A]";
    anchor_y = ui->hud_btn_aspect.y;
    break;
  case LIVE_POPUP_DRIVE:
    title = "Drive Mode [D]";
    anchor_y = ui->hud_btn_drive.y > 0 ? ui->hud_btn_drive.y : 56;
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
  } else if (ui->active_popup == LIVE_POPUP_GRID) {
    ui->popup_opt_count = GRID_PRESET_COUNT;
    for (int i = 0; i < GRID_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%s",
               GRID_PRESETS[i].popup_label);
      it->min_w = 104;
      it->style = (ui->grid_mode_index == i) ? UI_BTN_STYLE_ACTIVE
                                             : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }
  } else if (ui->active_popup == LIVE_POPUP_ASPECT) {
    ui->popup_opt_count = ASPECT_PRESET_COUNT;
    for (int i = 0; i < ASPECT_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%s",
               ASPECT_PRESETS[i].popup_label);
      it->min_w = 96;
      it->style = (ui->aspect_mask_index == i) ? UI_BTN_STYLE_ACTIVE
                                               : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }
  } else if (ui->active_popup == LIVE_POPUP_DRIVE) {
    ui->popup_opt_count = DRIVE_PRESET_COUNT;
    for (int i = 0; i < DRIVE_PRESET_COUNT; ++i) {
      PopupBtnItem *it = &items[item_count++];
      snprintf(it->label, sizeof(it->label), "%s",
               DRIVE_PRESETS[i].popup_label);
      it->min_w = 92;
      it->style = (ui->drive_mode_index == i) ? UI_BTN_STYLE_ACTIVE
                                              : UI_BTN_STYLE_DEFAULT;
      it->target_rect = &ui->popup_opt_rects[i];
      it->force_new_row = false;
    }

    LiveDriveMode dmode = DRIVE_PRESETS[ui->drive_mode_index].mode;
    if (dmode == LIVE_DRIVE_INTERVAL) {
      PopupBtnItem *it_im = &items[item_count++];
      snprintf(it_im->label, sizeof(it_im->label), "Intvl -");
      it_im->min_w = 84;
      it_im->style = UI_BTN_STYLE_DEFAULT;
      it_im->target_rect = &ui->popup_btn_minus;
      it_im->force_new_row = true;

      PopupBtnItem *it_ir = &items[item_count++];
      snprintf(it_ir->label, sizeof(it_ir->label), "Interval: %s",
               INTERVAL_SEC_PRESETS[ui->interval_sec_index].label);
      it_ir->min_w = 148;
      it_ir->style = UI_BTN_STYLE_ACCENT;
      it_ir->target_rect = &ui->popup_btn_reset;
      it_ir->force_new_row = false;

      PopupBtnItem *it_ip = &items[item_count++];
      snprintf(it_ip->label, sizeof(it_ip->label), "Intvl +");
      it_ip->min_w = 84;
      it_ip->style = UI_BTN_STYLE_DEFAULT;
      it_ip->target_rect = &ui->popup_btn_plus;
      it_ip->force_new_row = false;

      PopupBtnItem *it_sm = &items[item_count++];
      snprintf(it_sm->label, sizeof(it_sm->label), "Shots -");
      it_sm->min_w = 84;
      it_sm->style = UI_BTN_STYLE_DEFAULT;
      it_sm->target_rect = &ui->popup_sub_minus;
      it_sm->force_new_row = true;

      PopupBtnItem *it_sr = &items[item_count++];
      int scnt = INTERVAL_SHOT_PRESETS[ui->interval_shots_index].count;
      if (scnt == 0) {
        snprintf(it_sr->label, sizeof(it_sr->label), "Shots: Inf (Unlimited)");
      } else {
        snprintf(it_sr->label, sizeof(it_sr->label), "Shots: %d frames", scnt);
      }
      it_sr->min_w = 148;
      it_sr->style = UI_BTN_STYLE_ACCENT;
      it_sr->target_rect = &ui->popup_sub_reset;
      it_sr->force_new_row = false;

      PopupBtnItem *it_sp = &items[item_count++];
      snprintf(it_sp->label, sizeof(it_sp->label), "Shots +");
      it_sp->min_w = 84;
      it_sp->style = UI_BTN_STYLE_DEFAULT;
      it_sp->target_rect = &ui->popup_sub_plus;
      it_sp->force_new_row = false;
    } else if (dmode == LIVE_DRIVE_AEB) {
      ui->popup_sub_count = AEB_STEP_PRESET_COUNT;
      for (int i = 0; i < AEB_STEP_PRESET_COUNT; ++i) {
        PopupBtnItem *it = &items[item_count++];
        snprintf(it->label, sizeof(it->label), "Step %s",
                 AEB_STEP_PRESETS[i].label);
        it->min_w = 112;
        it->style = (ui->aeb_step_index == i) ? UI_BTN_STYLE_ACCENT
                                              : UI_BTN_STYLE_DEFAULT;
        it->target_rect = &ui->popup_sub_rects[i];
        it->force_new_row = (i == 0);
      }
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

static void toggle_popup(LiveUiController *ui, LivePopupType type) {
  if (ui->active_popup == type) {
    ui->active_popup = LIVE_POPUP_NONE;
  } else {
    ui->active_popup = type;
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
  } else if (ui_rect_contains(&ui->hud_btn_grid, px, py) ||
             ui->active_popup == LIVE_POPUP_GRID) {
    live_ui_set_grid_index(ui, ui->grid_mode_index + dir);
  } else if (ui_rect_contains(&ui->hud_btn_aspect, px, py) ||
             ui->active_popup == LIVE_POPUP_ASPECT) {
    live_ui_set_aspect_index(ui, ui->aspect_mask_index + dir);
  } else if (ui_rect_contains(&ui->hud_btn_drive, px, py) ||
             ui_rect_contains(&ui->top_btn_drive, px, py) ||
             ui->active_popup == LIVE_POPUP_DRIVE) {
    live_ui_set_drive_index(ui, camera, ui->drive_mode_index + dir);
  } else {
    /* Default wheel action on live viewfinder: adjust EV compensation */
    live_ui_step_ev(ui, camera, dir);
  }
}

void live_ui_init(LiveUiController *ui) {
  if (!ui) {
    return;
  }
  memset(ui, 0, sizeof(*ui));
  ui->wb_kelvin_value = 5500;
  ui->quality_preset_index = 1;  /* Default: Fine (90%) */
  ui->interval_sec_index = 3;    /* Default: 5s */
  ui->interval_shots_index = 2;  /* Default: 30 shots */
  ui->aeb_step_index = 2;        /* Default: +/-1.0 EV */
  ui->pointer_x = -1;
  ui->pointer_y = -1;

  /* Initialize SDL2 Audio for synthesized self-timer & shutter beep tones
   * (graceful silent fallback if no audio output device is available) */
  if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
      return;
    }
  }
  SDL_AudioSpec want;
  SDL_AudioSpec have;
  SDL_zero(want);
  SDL_zero(have);
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 1024;
  want.callback = NULL;
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (dev > 0) {
    SDL_PauseAudioDevice(dev, 0);
    ui->audio_dev = (uint32_t)dev;
  }
}

void live_ui_destroy(LiveUiController *ui) {
  if (!ui) {
    return;
  }
  if (ui->audio_dev != 0) {
    SDL_CloseAudioDevice((SDL_AudioDeviceID)ui->audio_dev);
    ui->audio_dev = 0;
  }
}

void live_ui_update(LiveUiController *ui, CameraStream *camera,
                    AsyncSaveQueue *save_queue) {
  if (!ui || !camera) {
    return;
  }

  uint32_t now = SDL_GetTicks();

  /* 1. Self-timer countdown & synthesized sine-wave beep */
  if (ui->timer_active) {
    uint32_t elapsed = now - ui->timer_start_ms;
    if (elapsed >= ui->timer_duration_ms) {
      ui->timer_active = false;
      live_ui_trigger_single_capture(ui, camera, save_queue);
    } else {
      uint32_t rem_ms = ui->timer_duration_ms - elapsed;
      uint32_t beep_interval = (rem_ms <= 2000U) ? 250U : 1000U;
      if (SDL_TICKS_PASSED(now, ui->timer_last_beep_ms + beep_interval)) {
        ui->timer_last_beep_ms = now;
        if (rem_ms <= 2000U) {
          live_ui_play_beep(ui, 1046.5f, 45, 0.25f);
        } else {
          live_ui_play_beep(ui, 880.0f, 65, 0.22f);
        }
      }
    }
  }

  /* 2. Continuous Burst Mode management */
  if (ui->burst_active) {
    if (ui->burst_held_by_key && ui->burst_keyup_pending &&
        SDL_TICKS_PASSED(now, ui->burst_keyup_ms + 120U)) {
      ui->burst_held_by_key = false;
      ui->burst_keyup_pending = false;
    }
    if (!ui->burst_held_by_key && !ui->burst_held_by_pointer) {
      live_ui_stop_burst(ui, camera);
    } else {
      int pending = async_save_queue_get_pending_count(save_queue);
      if (pending >= ASYNC_SAVE_QUEUE_CAPACITY - 1) {
        /* Temporarily throttle burst stream when save queue is full */
        if (camera_stream_is_burst_mode(camera)) {
          camera_stream_set_burst_mode(camera, false);
        }
      } else {
        if (!camera_stream_is_burst_mode(camera)) {
          camera_stream_set_burst_mode(camera, true);
        }
      }
    }
  }

  /* 3. Interval shooting scheduler */
  if (ui->interval_active && !ui->interval_waiting_capture) {
    if (SDL_TICKS_PASSED(now, ui->interval_next_shot_ms)) {
      if (!camera_stream_is_still_pending(camera) &&
          async_save_queue_get_pending_count(save_queue) <
              ASYNC_SAVE_QUEUE_CAPACITY) {
        if (camera_stream_request_still(camera)) {
          ui->interval_waiting_capture = true;
          ui->interval_next_shot_ms = now + ui->interval_step_ms;
          live_ui_play_beep(ui, 1320.0f, 45, 0.22f);
        }
      }
    }
  }

  /* 4. Auto Exposure Bracketing (AEB) exposure settle & trigger */
  if (ui->aeb_active && ui->aeb_waiting_settle && !ui->aeb_waiting_capture) {
    uint64_t cur_seq = camera_stream_get_frame_sequence(camera);
    bool frames_settled = (cur_seq >= ui->aeb_settle_start_seq + 4ULL);
    bool time_settled = SDL_TICKS_PASSED(now, ui->aeb_settle_start_ms + 150U);
    if ((frames_settled || time_settled) &&
        !camera_stream_is_still_pending(camera) &&
        async_save_queue_get_pending_count(save_queue) <
            ASYNC_SAVE_QUEUE_CAPACITY) {
      if (camera_stream_request_still(camera)) {
        ui->aeb_waiting_settle = false;
        ui->aeb_waiting_capture = true;
        live_ui_play_beep(ui, 1200.0f, 45, 0.24f);
      }
    }
  }
}

bool live_ui_on_still_captured(LiveUiController *ui, CameraStream *camera,
                               CameraCapturedStill *still) {
  if (!ui) {
    return true;
  }

  /* 1. Burst Mode: count shot, click sound, suppress Quick Review while held */
  if (ui->burst_active ||
      DRIVE_PRESETS[ui->drive_mode_index].mode == LIVE_DRIVE_BURST) {
    ui->burst_shot_count++;
    live_ui_play_beep(ui, 1480.0f, 22, 0.16f);
    printf("[-] 連写キャプチャ: %d 枚目\n", ui->burst_shot_count);
    if (ui->burst_active) {
      return false;
    }
    return true;
  }

  /* 2. Interval Shooting: advance counter, suppress Quick Review until final
   * shot */
  if (ui->interval_active) {
    ui->interval_waiting_capture = false;
    ui->interval_shots_taken++;
    if (ui->interval_target_shots > 0) {
      printf("[-] インターバル撮影: %d / %d 枚目完了\n",
             ui->interval_shots_taken, ui->interval_target_shots);
      if (ui->interval_shots_taken >= ui->interval_target_shots) {
        ui->interval_active = false;
        live_ui_play_beep(ui, 1567.98f, 110, 0.28f);
        printf("[-] インターバル撮影が完了しました (計 %d 枚)。\n",
               ui->interval_shots_taken);
        return true;
      }
    } else {
      printf("[-] インターバル撮影: %d 枚目完了 (無制限モード)\n",
             ui->interval_shots_taken);
    }
    return false;
  }

  /* 3. Auto Exposure Bracketing (AEB): advance across 3 bracketed shots */
  if (ui->aeb_active) {
    ui->aeb_waiting_capture = false;
    int idx = ui->aeb_shot_index;
    if (idx < 0 || idx > 2) {
      idx = 0;
    }
    float shot_ev = (float)ui->aeb_target_ev_steps[idx] / 3.0f;
    if (still) {
      still->exp_status.ev_compensation = shot_ev;
    }
    printf("[-] AEB撮影: %d / 3 枚目完了 (%+.1f EV)\n", idx + 1, shot_ev);

    if (idx < 2) {
      ui->aeb_shot_index = idx + 1;
      float next_ev = (float)ui->aeb_target_ev_steps[ui->aeb_shot_index] / 3.0f;
      if (camera) {
        camera_stream_set_ev(camera, next_ev);
        ui->aeb_settle_start_seq = camera_stream_get_frame_sequence(camera);
      }
      ui->aeb_settle_start_ms = SDL_GetTicks();
      ui->aeb_waiting_settle = true;
      return false;
    } else {
      /* 3rd shot complete: restore base EV */
      ui->aeb_active = false;
      ui->aeb_waiting_settle = false;
      ui->ev_step_index = ui->aeb_base_ev_step;
      if (camera) {
        camera_stream_set_ev(camera, (float)ui->ev_step_index / 3.0f);
      }
      live_ui_play_beep(ui, 1567.98f, 100, 0.26f);
      printf("[-] AEB 3枚連続撮影が完了しました (基準EV %+.1f に復帰)。\n",
             (float)ui->ev_step_index / 3.0f);
      return true;
    }
  }

  return true;
}

void live_ui_print_help(const CameraStream *camera) {
  printf("=========================================\n");
  printf(" [Space]        : 写真を撮影 (連写時は長押し / タイマー・インターバル時は開始/停止)\n");
  printf(" [D / Shift+D]  : ドライブモード切替 (単写 / 2秒・10秒タイマー / 連写 / インターバル / AEB)\n");
  printf(" [P]            : 再生・ギャラリーモード切替 "
         "(ズーム/グリッド/削除/保護)\n");
  if (camera && camera_stream_is_af_supported(camera)) {
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
  printf(" [G / Shift+G]  : 構図ガイドグリッド切替 (Off / 三分割 / 黄金比 / "
         "十字 / 三分割+十字)\n");
  printf(" [A / Shift+A]  : アスペクト比マスク&クロップ切替 (Native / 16:9 / "
         "4:3 / 3:2 / 1:1)\n");
  printf(" [R]            : 露出・WB・測光・画質・構図・ドライブ設定をリセット\n");
  printf(" [Q]            : アプリを終了\n");
  printf("=========================================\n");
}

int live_ui_get_jpeg_quality(const LiveUiController *ui) {
  if (!ui || ui->quality_preset_index < 0 ||
      ui->quality_preset_index >= JPEG_QUALITY_PRESET_COUNT) {
    return 90;
  }
  return JPEG_QUALITY_PRESETS[ui->quality_preset_index].quality;
}

void live_ui_apply_aspect_crop(const LiveUiController *ui,
                               CameraCapturedStill *still) {
  if (!ui || !still || !still->data || still->width <= 0 ||
      still->height <= 0) {
    return;
  }
  if (ui->aspect_mask_index <= 0 ||
      ui->aspect_mask_index >= ASPECT_PRESET_COUNT) {
    return;
  }

  int aspect_w = ASPECT_PRESETS[ui->aspect_mask_index].aspect_w;
  int aspect_h = ASPECT_PRESETS[ui->aspect_mask_index].aspect_h;
  if (aspect_w <= 0 || aspect_h <= 0) {
    return;
  }

  SDL_Rect full_bounds = {0, 0, still->width, still->height};
  SDL_Rect crop_rect = {0, 0, still->width, still->height};
  ui_calc_aspect_fit_rect(aspect_w, aspect_h, &full_bounds, &crop_rect);

  /* Align cropped dimensions to even pixels for clean JPEG chroma subsampling */
  int crop_w = crop_rect.w & ~1;
  int crop_h = crop_rect.h & ~1;
  if (crop_w < 2) {
    crop_w = crop_rect.w;
  }
  if (crop_h < 2) {
    crop_h = crop_rect.h;
  }
  int crop_x = (still->width - crop_w) / 2;
  int crop_y = (still->height - crop_h) / 2;

  if (crop_w <= 0 || crop_h <= 0 ||
      (crop_w == still->width && crop_h == still->height)) {
    return;
  }

  int src_stride = still->stride > 0 ? still->stride : (still->width * 3);
  int dst_stride = crop_w * 3;

  for (int y = 0; y < crop_h; ++y) {
    const uint8_t *src_row = still->data +
                             (size_t)(crop_y + y) * (size_t)src_stride +
                             (size_t)crop_x * 3U;
    uint8_t *dst_row = still->data + (size_t)y * (size_t)dst_stride;
    if (dst_row != src_row) {
      memmove(dst_row, src_row, (size_t)dst_stride);
    }
  }

  still->width = crop_w;
  still->height = crop_h;
  still->stride = dst_stride;
  still->size = (size_t)dst_stride * (size_t)crop_h;
  printf("[-] アスペクト比クロップ適用 (%s): %dx%d\n",
         ASPECT_PRESETS[ui->aspect_mask_index].hud_label, crop_w, crop_h);
}

bool live_ui_is_quit_requested(const LiveUiController *ui) {
  return ui && ui->quit_requested;
}

void live_ui_render(LiveUiController *ui, SDL_Renderer *renderer,
                    CameraStream *camera, const CameraExposureStatus *status,
                    int pending_saves, int win_w, int win_h) {
  if (!ui || !renderer || !camera || !status) {
    return;
  }

  bool af_supported = camera_stream_is_af_supported(camera);

  /* 1. Aspect ratio mask, composition guide grid, Touch AF reticle, and Drive
   * status overlays */
  render_aspect_mask(renderer, ui);
  render_composition_grid(renderer, ui);
  render_af_reticle(renderer, ui, camera);
  render_drive_status_overlay(renderer, ui);

  /* 2. Top action bar (Top-left: Settings + Drive [+ Reset], Top-right: Quit) */
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

  char top_drv_str[40];
  snprintf(top_drv_str, sizeof(top_drv_str), "Drive:%s [D]",
           DRIVE_PRESETS[ui->drive_mode_index].hud_label);
  text_renderer_draw_button(
      renderer, top_drv_str, tx, ty, 128, th,
      ui->active_popup == LIVE_POPUP_DRIVE
          ? UI_BTN_STYLE_ACTIVE
          : (ui->drive_mode_index != 0 ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
      live_btn_hovered(ui, &ui->top_btn_drive),
      live_btn_pressed(ui, &ui->top_btn_drive), &r);
  ui->top_btn_drive = r;
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

  const char *sh_label = "SHOT";
  UiButtonStyle sh_style = UI_BTN_STYLE_DANGER;
  if (ui->timer_active) {
    sh_label = "CANCEL";
    sh_style = UI_BTN_STYLE_ACCENT;
  } else if (ui->interval_active || ui->aeb_active) {
    sh_label = "STOP";
    sh_style = UI_BTN_STYLE_ACCENT;
  } else if (ui->burst_active) {
    sh_label = "BURST";
    sh_style = UI_BTN_STYLE_ACTIVE;
  } else if (still_busy) {
    sh_label = "WAIT...";
    sh_style = UI_BTN_STYLE_ACCENT;
  }

  text_renderer_draw_button(renderer, sh_label, sh_x, sh_y, sh_w, sh_h,
                            sh_style, live_btn_hovered(ui, &ui->btn_shutter),
                            live_btn_pressed(ui, &ui->btn_shutter),
                            &ui->btn_shutter);

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
    ui->hud_btn_grid = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_aspect = (SDL_Rect){0, 0, 0, 0};
    ui->hud_btn_drive = (SDL_Rect){0, 0, 0, 0};
    if (ui->active_popup != LIVE_POPUP_DRIVE) {
      ui->active_popup = LIVE_POPUP_NONE;
    }
    render_live_popup(renderer, ui, win_w, win_h);
    return;
  }

  int bx = 12;
  int by = 56;
  int avail_h = win_h - by - 10;
  int bgap = avail_h >= 460 ? 5 : 3;
  int bh = (avail_h - 10 * bgap) / 11;
  if (bh > 38) {
    bh = 38;
  }
  if (bh < 26) {
    bh = 26;
  }
  int bw = 156;

  char ss_str[48];
  if (status->shutter_us_setting > 0) {
    char m_ss[24];
    ui_format_shutter_speed(status->shutter_us_setting, m_ss, sizeof(m_ss));
    snprintf(ss_str, sizeof(ss_str), "SS:%s[M]", m_ss);
  } else {
    char a_ss[24];
    ui_format_shutter_speed(status->actual_exposure_us, a_ss, sizeof(a_ss));
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
  by += r.h + bgap;

  char grid_str[32];
  snprintf(grid_str, sizeof(grid_str), "Grid:%s",
           GRID_PRESETS[ui->grid_mode_index].hud_label);
  text_renderer_draw_button(renderer, grid_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_GRID
                                ? UI_BTN_STYLE_ACTIVE
                                : (ui->grid_mode_index != 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_grid),
                            live_btn_pressed(ui, &ui->hud_btn_grid), &r);
  ui->hud_btn_grid = r;
  by += r.h + bgap;

  char asp_str[32];
  snprintf(asp_str, sizeof(asp_str), "Aspect:%s",
           ASPECT_PRESETS[ui->aspect_mask_index].hud_label);
  text_renderer_draw_button(renderer, asp_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_ASPECT
                                ? UI_BTN_STYLE_ACTIVE
                                : (ui->aspect_mask_index != 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_aspect),
                            live_btn_pressed(ui, &ui->hud_btn_aspect), &r);
  ui->hud_btn_aspect = r;
  by += r.h + bgap;

  char drv_str[32];
  snprintf(drv_str, sizeof(drv_str), "Drive:%s",
           DRIVE_PRESETS[ui->drive_mode_index].hud_label);
  text_renderer_draw_button(renderer, drv_str, bx, by, bw, bh,
                            ui->active_popup == LIVE_POPUP_DRIVE
                                ? UI_BTN_STYLE_ACTIVE
                                : (ui->drive_mode_index != 0
                                       ? UI_BTN_STYLE_ACCENT
                                       : UI_BTN_STYLE_DEFAULT),
                            live_btn_hovered(ui, &ui->hud_btn_drive),
                            live_btn_pressed(ui, &ui->hud_btn_drive), &r);
  ui->hud_btn_drive = r;

  /* 5. Active parameter popup selector next to the left parameter column */
  render_live_popup(renderer, ui, win_w, win_h);
}

bool live_ui_handle_key(LiveUiController *ui, CameraStream *camera,
                        PlaybackManager *playback_mgr,
                        AsyncSaveQueue *save_queue, SDL_Renderer *renderer,
                        const SDL_KeyboardEvent *key_event) {
  if (!ui || !camera || !key_event) {
    return false;
  }

  bool shift_held = (key_event->keysym.mod & KMOD_SHIFT) != 0;

  switch (key_event->keysym.sym) {
  case SDLK_ESCAPE:
    if (live_ui_cancel_active_drive(ui, camera)) {
      return true;
    }
    if (ui->active_popup != LIVE_POPUP_NONE) {
      ui->active_popup = LIVE_POPUP_NONE;
      return true;
    }
    return false;

  case SDLK_p:
    live_ui_cancel_active_drive(ui, camera);
    ui->active_popup = LIVE_POPUP_NONE;
    if (playback_mgr && renderer) {
      playback_manager_enter_playback(playback_mgr, renderer, NULL);
    }
    return true;

  case SDLK_f:
    live_ui_trigger_af_at(ui, camera, -1.0f, -1.0f);
    return true;

  case SDLK_UP:
  case SDLK_RIGHTBRACKET:
    live_ui_step_ev(ui, camera, 1);
    return true;

  case SDLK_DOWN:
  case SDLK_LEFTBRACKET:
    live_ui_step_ev(ui, camera, -1);
    return true;

  case SDLK_i:
    live_ui_set_iso_index(ui, camera,
                          ui->iso_preset_index + (shift_held ? -1 : 1));
    return true;

  case SDLK_s:
  case SDLK_RIGHT: {
    int step = (key_event->keysym.sym == SDLK_s && shift_held) ? -1 : 1;
    live_ui_set_shutter_index(ui, camera, ui->shutter_preset_index + step);
    return true;
  }

  case SDLK_LEFT:
    live_ui_set_shutter_index(ui, camera, ui->shutter_preset_index - 1);
    return true;

  case SDLK_w:
    live_ui_set_wb_index(ui, camera,
                         ui->wb_preset_index + (shift_held ? -1 : 1));
    return true;

  case SDLK_k:
    live_ui_set_kelvin(ui, camera,
                       ui->wb_kelvin_value + (shift_held ? -500 : 500));
    return true;

  case SDLK_l:
    live_ui_toggle_awb_lock(ui, camera);
    return true;

  case SDLK_m:
    live_ui_set_metering_index(
        ui, camera, ui->metering_preset_index + (shift_held ? -1 : 1));
    return true;

  case SDLK_j:
    live_ui_set_quality_index(ui,
                              ui->quality_preset_index + (shift_held ? -1 : 1));
    return true;

  case SDLK_g:
    live_ui_set_grid_index(ui, ui->grid_mode_index + (shift_held ? -1 : 1));
    return true;

  case SDLK_a:
    live_ui_set_aspect_index(ui,
                             ui->aspect_mask_index + (shift_held ? -1 : 1));
    return true;

  case SDLK_d:
    live_ui_set_drive_index(
        ui, camera, ui->drive_mode_index + (shift_held ? -1 : 1));
    return true;

  case SDLK_r:
    live_ui_reset_all(ui, camera);
    return true;

  case SDLK_SPACE:
    ui->active_popup = LIVE_POPUP_NONE;
    if (DRIVE_PRESETS[ui->drive_mode_index].mode == LIVE_DRIVE_BURST) {
      ui->burst_held_by_key = true;
      ui->burst_keyup_pending = false;
      if (!ui->burst_active) {
        live_ui_start_burst(ui, camera, save_queue);
      }
      return true;
    }
    if (key_event->repeat != 0) {
      return true;
    }
    live_ui_request_shutter(ui, camera, save_queue);
    return true;

  default:
    return false;
  }
}

bool live_ui_handle_key_up(LiveUiController *ui, CameraStream *camera,
                           const SDL_KeyboardEvent *key_event) {
  (void)camera;
  if (!ui || !key_event) {
    return false;
  }
  if (key_event->keysym.sym == SDLK_SPACE) {
    if (ui->burst_held_by_key) {
      ui->burst_keyup_pending = true;
      ui->burst_keyup_ms = SDL_GetTicks();
      return true;
    }
  }
  return false;
}

void live_ui_handle_pointer_down(LiveUiController *ui, CameraStream *camera,
                                 AsyncSaveQueue *save_queue, int x, int y,
                                 uint8_t button) {
  if (!ui) {
    return;
  }
  if (button == SDL_BUTTON_LEFT) {
    ui->pointer_x = x;
    ui->pointer_y = y;
    ui->pointer_down = true;
    ui->down_x = x;
    ui->down_y = y;
    if (camera &&
        DRIVE_PRESETS[ui->drive_mode_index].mode == LIVE_DRIVE_BURST &&
        ui_rect_contains(&ui->btn_shutter, x, y)) {
      ui->burst_held_by_pointer = true;
      live_ui_start_burst(ui, camera, save_queue);
    }
  } else if (button == SDL_BUTTON_RIGHT && camera) {
    /* Right click steps backward on hovered Live HUD button */
    handle_live_wheel_or_step(ui, camera, -1, x, y);
  }
}

void live_ui_handle_pointer_move(LiveUiController *ui, int x, int y) {
  if (!ui) {
    return;
  }
  ui->pointer_x = x;
  ui->pointer_y = y;
}

void live_ui_handle_pointer_up(LiveUiController *ui, CameraStream *camera,
                               PlaybackManager *playback_mgr,
                               AsyncSaveQueue *save_queue,
                               SDL_Renderer *renderer, int x, int y) {
  if (!ui || !camera) {
    return;
  }

  int sx = ui->down_x;
  int sy = ui->down_y;
  ui->pointer_x = x;
  ui->pointer_y = y;
  ui->pointer_down = false;

  if (ui->burst_held_by_pointer) {
    ui->burst_held_by_pointer = false;
    if (!ui->burst_held_by_key) {
      live_ui_stop_burst(ui, camera);
    }
    return;
  }

  /* Top-left Settings toggle button is always interactive */
  if (ui_rect_contains(&ui->top_btn_settings, x, y) &&
      ui_rect_contains(&ui->top_btn_settings, sx, sy)) {
    ui->settings_visible = !ui->settings_visible;
    if (!ui->settings_visible && ui->active_popup != LIVE_POPUP_DRIVE) {
      ui->active_popup = LIVE_POPUP_NONE;
    }
    return;
  }

  /* Top bar Drive button opens/toggles the Drive Mode popup */
  if (ui_rect_contains(&ui->top_btn_drive, x, y) &&
      ui_rect_contains(&ui->top_btn_drive, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_DRIVE);
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
    } else if (ui->active_popup == LIVE_POPUP_DRIVE) {
      for (int i = 0; i < ui->popup_opt_count; ++i) {
        if (ui_rect_contains(&ui->popup_opt_rects[i], x, y) &&
            ui_rect_contains(&ui->popup_opt_rects[i], sx, sy)) {
          live_ui_set_drive_index(ui, camera, i);
          return;
        }
      }
      LiveDriveMode dmode = DRIVE_PRESETS[ui->drive_mode_index].mode;
      if (dmode == LIVE_DRIVE_INTERVAL) {
        if (ui_rect_contains(&ui->popup_btn_minus, x, y) &&
            ui_rect_contains(&ui->popup_btn_minus, sx, sy)) {
          live_ui_set_interval_sec_index(ui, ui->interval_sec_index - 1);
          return;
        }
        if (ui_rect_contains(&ui->popup_btn_plus, x, y) &&
            ui_rect_contains(&ui->popup_btn_plus, sx, sy)) {
          live_ui_set_interval_sec_index(ui, ui->interval_sec_index + 1);
          return;
        }
        if (ui_rect_contains(&ui->popup_btn_reset, x, y) &&
            ui_rect_contains(&ui->popup_btn_reset, sx, sy)) {
          live_ui_set_interval_sec_index(ui, 3); /* Default 5s */
          return;
        }
        if (ui_rect_contains(&ui->popup_sub_minus, x, y) &&
            ui_rect_contains(&ui->popup_sub_minus, sx, sy)) {
          live_ui_set_interval_shots_index(ui, ui->interval_shots_index - 1);
          return;
        }
        if (ui_rect_contains(&ui->popup_sub_plus, x, y) &&
            ui_rect_contains(&ui->popup_sub_plus, sx, sy)) {
          live_ui_set_interval_shots_index(ui, ui->interval_shots_index + 1);
          return;
        }
        if (ui_rect_contains(&ui->popup_sub_reset, x, y) &&
            ui_rect_contains(&ui->popup_sub_reset, sx, sy)) {
          live_ui_set_interval_shots_index(ui, 2); /* Default 30 shots */
          return;
        }
      } else if (dmode == LIVE_DRIVE_AEB) {
        for (int i = 0; i < ui->popup_sub_count; ++i) {
          if (ui_rect_contains(&ui->popup_sub_rects[i], x, y) &&
              ui_rect_contains(&ui->popup_sub_rects[i], sx, sy)) {
            live_ui_set_aeb_step_index(ui, i);
            return;
          }
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
          } else if (ui->active_popup == LIVE_POPUP_GRID) {
            live_ui_set_grid_index(ui, i);
          } else if (ui->active_popup == LIVE_POPUP_ASPECT) {
            live_ui_set_aspect_index(ui, i);
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
    live_ui_cancel_active_drive(ui, camera);
    ui->active_popup = LIVE_POPUP_NONE;
    if (playback_mgr && renderer) {
      playback_manager_enter_playback(playback_mgr, renderer, NULL);
    }
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
    live_ui_cancel_active_drive(ui, camera);
    ui->quit_requested = true;
    return;
  }

  /* 3. Right-edge Shutter Button */
  if (ui_rect_contains(&ui->btn_shutter, x, y) &&
      ui_rect_contains(&ui->btn_shutter, sx, sy)) {
    ui->active_popup = LIVE_POPUP_NONE;
    live_ui_request_shutter(ui, camera, save_queue);
    return;
  }

  /* 4. Left-edge HUD parameter buttons */
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
  if (ui_rect_contains(&ui->hud_btn_grid, x, y) &&
      ui_rect_contains(&ui->hud_btn_grid, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_GRID);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_aspect, x, y) &&
      ui_rect_contains(&ui->hud_btn_aspect, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_ASPECT);
    return;
  }
  if (ui_rect_contains(&ui->hud_btn_drive, x, y) &&
      ui_rect_contains(&ui->hud_btn_drive, sx, sy)) {
    toggle_popup(ui, LIVE_POPUP_DRIVE);
    return;
  }

  /* 5. If popup was open and user tapped outside, close popup */
  if (ui->active_popup != LIVE_POPUP_NONE) {
    ui->active_popup = LIVE_POPUP_NONE;
    return;
  }

  /* 6. Tapping inside the active camera preview frame triggers Touch AF */
  const SDL_Rect *af_bounds =
      (ui->active_frame_rect.w > 0 && ui->active_frame_rect.h > 0)
          ? &ui->active_frame_rect
          : &ui->viewport_rect;
  if (ui_rect_contains(af_bounds, x, y) && abs(x - sx) < 24 &&
      abs(y - sy) < 24 && ui->viewport_rect.w > 0 && ui->viewport_rect.h > 0) {
    float nx = (float)(x - ui->viewport_rect.x) / (float)ui->viewport_rect.w;
    float ny = (float)(y - ui->viewport_rect.y) / (float)ui->viewport_rect.h;
    live_ui_trigger_af_at(ui, camera, nx, ny);
  }
}

void live_ui_handle_wheel(LiveUiController *ui, CameraStream *camera,
                          int wheel_y, int pointer_x, int pointer_y) {
  if (!ui || !camera || wheel_y == 0) {
    return;
  }
  handle_live_wheel_or_step(ui, camera, wheel_y > 0 ? 1 : -1, pointer_x,
                            pointer_y);
}
