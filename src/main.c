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
#include "live_ui.h"
#include "playback_manager.h"
#include "text_renderer.h"

static volatile sig_atomic_t g_running = 1;

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
                            int width, int height, int stride, bool is_bgr,
                            int jpeg_quality, const ExifMetadata *exif) {
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

  const int quality =
      (jpeg_quality > 0 && jpeg_quality <= 100) ? jpeg_quality : 90;
  int ret = IMG_SaveJPG(surface, filename, quality);
  if (ret != 0) {
    fprintf(stderr, "[エラー] IMG_SaveJPG による保存に失敗しました: %s\n",
            IMG_GetError());
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "%s: %d: IMG_SaveJPG による保存に失敗しました: %s", __FILE__,
                 __LINE__, SDL_GetError());
  } else if (exif) {
    exif_inject_into_jpeg_file(filename, exif);
  }

  SDL_FreeSurface(surface);
  return ret == 0;
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

  live_ui_print_help(camera);

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
  live_ui_init(&live_ui);

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
        } else {
          live_ui_handle_key(&live_ui, camera, &playback_mgr, &save_queue,
                             renderer, &event.key);
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

        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          if (event.button.button == SDL_BUTTON_LEFT) {
            playback_manager_handle_pointer_down(&playback_mgr, renderer, mx,
                                                 my);
          }
        } else {
          live_ui_handle_pointer_down(&live_ui, camera, mx, my,
                                      event.button.button);
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
        live_ui_handle_pointer_move(&live_ui, mx, my);
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
            live_ui_handle_pointer_up(&live_ui, camera, &playback_mgr,
                                      &save_queue, renderer, mx, my);
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
          live_ui_handle_wheel(&live_ui, camera, wy, mx, my);
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
          live_ui_handle_pointer_down(&live_ui, camera, tx, ty,
                                      SDL_BUTTON_LEFT);
        }
      } else if (event.type == SDL_FINGERMOTION) {
        int tx = (int)(event.tfinger.x * (float)cur_win_w);
        int ty = (int)(event.tfinger.y * (float)cur_win_h);
        live_ui_handle_pointer_move(&live_ui, tx, ty);
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_pointer_move(&playback_mgr, renderer, tx, ty);
        }
      } else if (event.type == SDL_FINGERUP) {
        int tx = (int)(event.tfinger.x * (float)cur_win_w);
        int ty = (int)(event.tfinger.y * (float)cur_win_h);
        if (playback_manager_get_mode(&playback_mgr) != APP_MODE_LIVE) {
          playback_manager_handle_pointer_up(&playback_mgr, renderer, tx, ty);
        } else {
          live_ui_handle_pointer_up(&live_ui, camera, &playback_mgr,
                                    &save_queue, renderer, tx, ty);
        }
        /* Clear hover state after touch release so touch buttons don't stay
         * hovered */
        live_ui.pointer_x = -1;
        live_ui.pointer_y = -1;
        playback_mgr.pointer_x = -1;
        playback_mgr.pointer_y = -1;
      }

      if (live_ui_is_quit_requested(&live_ui)) {
        g_running = 0;
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
        int jpeg_q = live_ui_get_jpeg_quality(&live_ui);
        if (!async_save_queue_push(&save_queue, filepath, captured_still.data,
                                   captured_still.size, captured_still.width,
                                   captured_still.height, captured_still.stride,
                                   captured_still.is_bgr, jpeg_q, &exif)) {
          // Fallback to synchronous save if background queue was unavailable
          save_still_jpeg(filepath, captured_still.data, captured_still.width,
                          captured_still.height, captured_still.stride,
                          captured_still.is_bgr, jpeg_q, &exif);
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
      live_ui_render(&live_ui, renderer, camera, &exp_status, pending_saves,
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
