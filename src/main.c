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
    {0, "Auto"},         {500, "1/2000s"},   {1000, "1/1000s"},
    {2000, "1/500s"},    {4000, "1/250s"},   {8000, "1/125s"},
    {16667, "1/60s"},    {33333, "1/30s"},   {66667, "1/15s"},
    {125000, "1/8s"},    {250000, "1/4s"},   {500000, "1/2s"},
    {1000000, "1.0s"},   {2000000, "2.0s"},
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

static void signal_handler(int signum) {
  (void)signum;
  printf("\n[-] 割り込みを検知しました。\n");
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
    fprintf(stderr, "[エラー] JPEG保存用サーフェスの作成に失敗しました: %s\n",
            SDL_GetError());
    return false;
  }

  const int jpeg_quality = g_active_jpeg_quality > 0 ? g_active_jpeg_quality : 90;
  int ret = IMG_SaveJPG(surface, filename, jpeg_quality);
  if (ret != 0) {
    fprintf(stderr, "[エラー] IMG_SaveJPG による保存に失敗しました: %s\n",
            IMG_GetError());
  } else {
    CameraExposureStatus exp_status;
    memset(&exp_status, 0, sizeof(exp_status));
    if (g_active_camera) {
      camera_stream_get_exposure_status(g_active_camera, &exp_status);
    }
    ExifMetadata exif;
    exif_metadata_init(&exif,
                       g_active_camera
                           ? camera_stream_get_model_name(g_active_camera)
                           : NULL,
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

int main(int argc, char *argv[]) {
  DcfDirMode dcf_mode = DCF_DIR_MODE_STANDARD;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--date-dir") == 0) {
      dcf_mode = DCF_DIR_MODE_DATE;
    }
  }

  // Set up signal handler for clean interrupt (Ctrl+C)
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

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

  // 1. Initialize SDL2, SDL2_image & SDL2_ttf
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
    fprintf(stderr, "[エラー] SDL2 の初期化に失敗しました: %s\n",
            SDL_GetError());
    return 1;
  }

  if ((IMG_Init(IMG_INIT_JPG) & IMG_INIT_JPG) != IMG_INIT_JPG) {
    fprintf(stderr, "[エラー] SDL2_image (JPG) の初期化に失敗しました: %s\n",
            IMG_GetError());
    SDL_Quit();
    return 1;
  }

  if (TTF_Init() < 0) {
    fprintf(stderr, "[エラー] SDL2_ttf の初期化に失敗しました: %s\n",
            TTF_GetError());
    IMG_Quit();
    SDL_Quit();
    return 1;
  }

  const int screen_width = 1024;
  const int screen_height = 600;

  SDL_Window *window = SDL_CreateWindow(
      "Raspberry Pi V3 Pygame Camera (C Version - SDL2)",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, screen_width,
      screen_height,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    fprintf(stderr, "[エラー] ウィンドウの作成に失敗しました: %s\n",
            SDL_GetError());
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }

  // Attempt hardware accelerated renderer with VSync, fallback to default if
  // failed
  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) {
    printf("[警告] "
           "ハードウェアアクセラレータ作成失敗、デフォルトレンダラーを使用しま"
           "す: %s\n",
           SDL_GetError());
    renderer = SDL_CreateRenderer(window, -1, 0);
  }

  if (!renderer) {
    fprintf(stderr, "[エラー] レンダラーの作成に失敗しました: %s\n",
            SDL_GetError());
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }

  // Initialize text renderer using SDL2_ttf
  text_renderer_init(NULL, 16);

  // Hide mouse cursor (convenient for console environments)
  SDL_ShowCursor(SDL_DISABLE);

  printf("[-] SDL2 / SDL2_image / SDL2_ttf を初期化しました。\n");

  // 2. Initialize CameraStream (libcamera via C wrapper)
  CameraStream *camera = camera_stream_create();
  if (!camera) {
    fprintf(stderr,
            "[エラー] CameraStream オブジェクトの作成に失敗しました。\n");
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }

  if (!camera_stream_init(camera, screen_width, screen_height)) {
    fprintf(stderr, "[エラー] カメラの初期化に失敗しました。\n");
    camera_stream_destroy(camera);
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }

  if (!camera_stream_start(camera)) {
    fprintf(stderr, "[エラー] カメラの起動に失敗しました。\n");
    camera_stream_destroy(camera);
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
  }

  printf("=========================================\n");
  printf(" [Space]        : 写真を撮影して非同期保存 (直後にクイックレビュー表示)\n");
  printf(" [P]            : 再生・ギャラリーモード切替 (ズーム/グリッド/削除/保護)\n");
  if (camera_stream_is_af_supported(camera)) {
    printf(" [F]            : オートフォーカス再調整 (ワンショットAF)\n");
  }
  printf(" [Up/Down, [/]] : 露出補正 (EV -3.0 ~ +3.0, 1/3ステップ)\n");
  printf(" [I / Shift+I]  : ISO感度切替 (Auto / 100 ~ 3200)\n");
  printf(" [S / Left/Rgt] : シャッター速度切替 (Auto / 1/2000s ~ 2.0s)\n");
  printf(" [J / Shift+J]  : JPEG保存画質切替 (SuperFine 98%% / Fine 90%% / Normal 80%%)\n");
  printf(" [R]            : 露出・画質設定をデフォルトにリセット\n");
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
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 1;
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

  // Exposure & JPEG quality control state indices
  int ev_step_index = 0; // -9 (-3.0 EV) to +9 (+3.0 EV) in 1/3 EV increments
  int iso_preset_index = 0;
  int shutter_preset_index = 0;
  int quality_preset_index = 1; // Default: Fine (90%)
  g_active_jpeg_quality = JPEG_QUALITY_PRESETS[quality_preset_index].quality;

  // Initialize background async JPEG + Exif save queue
  AsyncSaveQueue save_queue;
  if (!async_save_queue_init(&save_queue)) {
    fprintf(stderr,
            "[警告] 非同期保存キューの初期化に失敗しました (同期保存で動作します)。\n");
  }

  // Initialize Playback & Media Gallery manager
  PlaybackManager playback_mgr;
  playback_manager_init(&playback_mgr, &dcf_storage);
  playback_manager_set_save_queue(&playback_mgr, &save_queue);
  g_active_playback = &playback_mgr;
  g_active_renderer = renderer;
  g_active_camera = camera;

  // 3. Main event & GPU rendering loop
  while (g_running) {
    uint32_t frame_start = SDL_GetTicks();

    // Process SDL events
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
        case SDLK_p:
          // Switch to Playback / Gallery mode on 'P' key
          playback_manager_enter_playback(&playback_mgr, renderer, NULL);
          break;

        case SDLK_f:
          // Refocus on 'F' key (if AF supported)
          if (camera_stream_is_af_supported(camera)) {
            camera_stream_trigger_af(camera);
          }
          break;

        case SDLK_UP:
        case SDLK_RIGHTBRACKET: {
          if (ev_step_index < 9) {
            ev_step_index++;
            float ev = (float)ev_step_index / 3.0f;
            camera_stream_set_ev(camera, ev);
            printf("[-] 露出補正 (EV): %+.2f EV\n", ev);
          }
          break;
        }

        case SDLK_DOWN:
        case SDLK_LEFTBRACKET: {
          if (ev_step_index > -9) {
            ev_step_index--;
            float ev = (float)ev_step_index / 3.0f;
            camera_stream_set_ev(camera, ev);
            printf("[-] 露出補正 (EV): %+.2f EV\n", ev);
          }
          break;
        }

        case SDLK_i: {
          if (shift_held) {
            iso_preset_index =
                (iso_preset_index - 1 + ISO_PRESET_COUNT) % ISO_PRESET_COUNT;
          } else {
            iso_preset_index = (iso_preset_index + 1) % ISO_PRESET_COUNT;
          }
          int iso = ISO_PRESETS[iso_preset_index];
          camera_stream_set_iso(camera, iso);
          if (iso == 0) {
            printf("[-] ISO感度: Auto\n");
          } else {
            printf("[-] ISO感度: %d (手動)\n", iso);
          }
          break;
        }

        case SDLK_s:
        case SDLK_RIGHT: {
          if (event.key.keysym.sym == SDLK_s && shift_held) {
            shutter_preset_index =
                (shutter_preset_index - 1 + SHUTTER_PRESET_COUNT) %
                SHUTTER_PRESET_COUNT;
          } else {
            shutter_preset_index =
                (shutter_preset_index + 1) % SHUTTER_PRESET_COUNT;
          }
          int32_t shutter_us = SHUTTER_PRESETS[shutter_preset_index].exposure_us;
          camera_stream_set_shutter_speed(camera, shutter_us);
          printf("[-] シャッタースピード: %s\n",
                 SHUTTER_PRESETS[shutter_preset_index].label);
          break;
        }

        case SDLK_LEFT: {
          shutter_preset_index =
              (shutter_preset_index - 1 + SHUTTER_PRESET_COUNT) %
              SHUTTER_PRESET_COUNT;
          int32_t shutter_us = SHUTTER_PRESETS[shutter_preset_index].exposure_us;
          camera_stream_set_shutter_speed(camera, shutter_us);
          printf("[-] シャッタースピード: %s\n",
                 SHUTTER_PRESETS[shutter_preset_index].label);
          break;
        }

        case SDLK_j: {
          if (shift_held) {
            quality_preset_index =
                (quality_preset_index - 1 + JPEG_QUALITY_PRESET_COUNT) %
                JPEG_QUALITY_PRESET_COUNT;
          } else {
            quality_preset_index =
                (quality_preset_index + 1) % JPEG_QUALITY_PRESET_COUNT;
          }
          g_active_jpeg_quality =
              JPEG_QUALITY_PRESETS[quality_preset_index].quality;
          printf("[-] JPEG保存画質: %s\n",
                 JPEG_QUALITY_PRESETS[quality_preset_index].name);
          break;
        }

        case SDLK_r: {
          ev_step_index = 0;
          iso_preset_index = 0;
          shutter_preset_index = 0;
          quality_preset_index = 1;
          g_active_jpeg_quality =
              JPEG_QUALITY_PRESETS[quality_preset_index].quality;
          camera_stream_set_ev(camera, 0.0f);
          camera_stream_set_iso(camera, 0);
          camera_stream_set_shutter_speed(camera, 0);
          printf("[-] 露出・画質設定をリセットしました (EV: 0.0, ISO: Auto, SS: "
                 "Auto, JPG: %s)\n",
                 JPEG_QUALITY_PRESETS[quality_preset_index].name);
          break;
        }

        case SDLK_SPACE: {
          // Request asynchronous high-resolution still capture on 'Space' key
          if (camera_stream_is_still_pending(camera)) {
            printf("[-] 撮影処理中です...\n");
          } else if (async_save_queue_get_pending_count(&save_queue) >=
                     ASYNC_SAVE_QUEUE_CAPACITY) {
            fprintf(stderr,
                    "[警告] 保存キューが満杯です。書き込み完了をお待ちください。\n");
          } else {
            camera_stream_request_still(camera);
          }
          break;
        }

        default:
          break;
        }
      }
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
        int jpeg_q = JPEG_QUALITY_PRESETS[quality_preset_index].quality;
        if (!async_save_queue_push(
                &save_queue, filepath, captured_still.data, captured_still.size,
                captured_still.width, captured_still.height,
                captured_still.stride, captured_still.is_bgr, jpeg_q, &exif)) {
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

    int cur_win_w = screen_width;
    int cur_win_h = screen_height;
    SDL_GetWindowSize(window, &cur_win_w, &cur_win_h);

    if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
      // Drain incoming camera frames in background so buffer sequence stays fresh
      camera_stream_get_latest_frame(camera, frame_buffer, frame_size,
                                     &frame_seq);
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
      SDL_Rect viewport_rect;
      calculate_viewport_rect(cam_width, cam_height, cur_win_w, cur_win_h,
                              &viewport_rect);

      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
      SDL_RenderClear(renderer);

      SDL_RenderCopy(renderer, camera_texture, NULL, &viewport_rect);

      // 6. Draw overlay guide text & exposure HUD via SDL2_ttf
      text_renderer_draw_guide(renderer, camera_stream_is_af_supported(camera));

      CameraExposureStatus exp_status;
      camera_stream_get_exposure_status(camera, &exp_status);
      int pending_saves = async_save_queue_get_pending_count(&save_queue) +
                          (camera_stream_is_still_pending(camera) ? 1 : 0);
      text_renderer_draw_exposure_hud(
          renderer, cur_win_w, cur_win_h, &exp_status,
          JPEG_QUALITY_PRESETS[quality_preset_index].hud_label, pending_saves);

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
  printf("[-] 終了処理を実行中...\n");
  if (async_save_queue_get_pending_count(&save_queue) > 0) {
    printf("[-] バックグラウンド保存キューの完了を待機中...\n");
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

  TTF_Quit();
  IMG_Quit();
  SDL_Quit();
  printf("[-] アプリケーションを安全に終了しました。\n");

  return 0;
}
