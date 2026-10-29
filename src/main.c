#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "camera_stream.h"
#include "dcf_storage.h"
#include "text_renderer.h"

static volatile sig_atomic_t g_running = 1;

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
    out_rect->w = (int)(win_h * src_aspect);
    out_rect->x = (win_w - out_rect->w) / 2;
    out_rect->y = 0;
  } else {
    // Window is taller than camera: add black bars on top/bottom (letterbox)
    out_rect->w = win_w;
    out_rect->h = (int)(win_w / src_aspect);
    out_rect->x = 0;
    out_rect->y = (win_h - out_rect->h) / 2;
  }
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

  // 1. Initialize SDL2 & SDL2_ttf
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
    fprintf(stderr, "[エラー] SDL2 の初期化に失敗しました: %s\n",
            SDL_GetError());
    return 1;
  }

  if (TTF_Init() < 0) {
    fprintf(stderr, "[エラー] SDL2_ttf の初期化に失敗しました: %s\n",
            TTF_GetError());
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
    SDL_Quit();
    return 1;
  }

  // Initialize text renderer using SDL2_ttf
  text_renderer_init(NULL, 16);

  // Hide mouse cursor (convenient for console environments)
  SDL_ShowCursor(SDL_DISABLE);

  printf("[-] SDL2 / SDL2_ttf を初期化しました。\n");

  // 2. Initialize CameraStream (libcamera via C wrapper)
  CameraStream *camera = camera_stream_create();
  if (!camera) {
    fprintf(stderr,
            "[エラー] CameraStream オブジェクトの作成に失敗しました。\n");
    text_renderer_destroy();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
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
    SDL_Quit();
    return 1;
  }

  printf("=========================================\n");
  printf(" [Space] : 写真を撮影して保存\n");
  if (camera_stream_is_af_supported(camera)) {
    printf(" [F]     : オートフォーカス再調整 (ワンショットAF)\n");
  }
  printf(" [Q]     : アプリを終了\n");
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
    SDL_Quit();
    return 1;
  }

  const uint32_t target_frame_time_ms = 1000 / 30; // ~30 FPS

  // 3. Main event & GPU rendering loop
  while (g_running) {
    uint32_t frame_start = SDL_GetTicks();

    // Process SDL events
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        g_running = 0;
      } else if (event.type == SDL_KEYDOWN) {
        switch (event.key.keysym.sym) {
        case SDLK_q:
          // Quit on 'Q' key
          g_running = 0;
          break;

        case SDLK_f:
          // Refocus on 'F' key (if AF supported)
          if (camera_stream_is_af_supported(camera)) {
            camera_stream_trigger_af(camera);
          }
          break;

        case SDLK_SPACE: {
          // Capture high-resolution still image on 'Space' key using DCF file
          // naming
          char filepath[512];
          if (dcf_storage_next_filepath(&dcf_storage, filepath,
                                        sizeof(filepath))) {
            camera_stream_capture_still(camera, filepath);
          }
          break;
        }

        default:
          break;
        }
      }
    }

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
    int cur_win_w = screen_width;
    int cur_win_h = screen_height;
    SDL_GetWindowSize(window, &cur_win_w, &cur_win_h);

    SDL_Rect viewport_rect;
    calculate_viewport_rect(cam_width, cam_height, cur_win_w, cur_win_h,
                            &viewport_rect);

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_RenderCopy(renderer, camera_texture, NULL, &viewport_rect);

    // 6. Draw overlay guide text via SDL2_ttf
    text_renderer_draw_guide(renderer, camera_stream_is_af_supported(camera));

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
  SDL_Quit();
  printf("[-] アプリケーションを安全に終了しました。\n");

  return 0;
}
