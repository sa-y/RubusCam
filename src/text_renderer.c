#include "text_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TTF_Font *g_font = NULL;
static int g_font_size = 18;

// Cached textures for guide overlays to avoid per-frame text surface
// rasterization
static SDL_Texture *g_guide_texture_af = NULL;
static int g_guide_w_af = 0;
static int g_guide_h_af = 0;

static SDL_Texture *g_guide_texture_no_af = NULL;
static int g_guide_w_no_af = 0;
static int g_guide_h_no_af = 0;

static const char *DEFAULT_FONT_PATHS[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    NULL};

bool text_renderer_init(const char *font_path, int font_size) {
  if (!TTF_WasInit()) {
    if (TTF_Init() < 0) {
      fprintf(stderr, "[エラー] TTF_Init() に失敗しました: %s\n",
              TTF_GetError());
      return false;
    }
  }

  if (font_size > 0) {
    g_font_size = font_size;
  }

  // 1. Try explicit user path if specified
  if (font_path && font_path[0] != '\0') {
    g_font = TTF_OpenFont(font_path, g_font_size);
    if (g_font) {
      printf("[-] TrueType フォントを読み込みました: %s (サイズ: %d)\n",
             font_path, g_font_size);
      return true;
    }
    fprintf(stderr, "[警告] 指定フォント '%s' の読み込みに失敗しました: %s\n",
            font_path, TTF_GetError());
  }

  // 2. Search known system and local font paths
  for (int i = 0; DEFAULT_FONT_PATHS[i] != NULL; ++i) {
    g_font = TTF_OpenFont(DEFAULT_FONT_PATHS[i], g_font_size);
    if (g_font) {
      printf("[-] システム TrueType フォントを自動検出・読み込みました: %s "
             "(サイズ: %d)\n",
             DEFAULT_FONT_PATHS[i], g_font_size);
      return true;
    }
  }

  fprintf(stderr,
          "[エラー] 利用可能な TrueType フォントが見つかりませんでした。\n");
  fprintf(stderr, "        'sudo apt install fonts-dejavu-core' "
                  "などでフォントをインストールしてください。\n");
  return false;
}

void text_renderer_destroy(void) {
  if (g_guide_texture_af) {
    SDL_DestroyTexture(g_guide_texture_af);
    g_guide_texture_af = NULL;
    g_guide_w_af = 0;
    g_guide_h_af = 0;
  }

  if (g_guide_texture_no_af) {
    SDL_DestroyTexture(g_guide_texture_no_af);
    g_guide_texture_no_af = NULL;
    g_guide_w_no_af = 0;
    g_guide_h_no_af = 0;
  }

  if (g_font) {
    TTF_CloseFont(g_font);
    g_font = NULL;
  }
}

void text_renderer_draw_guide(SDL_Renderer *renderer, bool af_supported) {
  if (!renderer) {
    return;
  }

  SDL_Texture **tex_slot =
      af_supported ? &g_guide_texture_af : &g_guide_texture_no_af;
  int *w_slot = af_supported ? &g_guide_w_af : &g_guide_w_no_af;
  int *h_slot = af_supported ? &g_guide_h_af : &g_guide_h_no_af;

  const char *guide_text = af_supported
                               ? "[SPACE]: Capture   [F]: Focus   [Q]: Quit"
                               : "[SPACE]: Capture   [Q]: Quit";

  // Lazily create and cache guide text texture on GPU
  if (!*tex_slot && g_font) {
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font, guide_text, white);
    if (surf) {
      *tex_slot = SDL_CreateTextureFromSurface(renderer, surf);
      *w_slot = surf->w;
      *h_slot = surf->h;
      SDL_FreeSurface(surf);
    }
  }

  int content_w = *w_slot > 0 ? *w_slot : 340;
  int content_h = *h_slot > 0 ? *h_slot : 20;

  int pad_x = 12;
  int pad_y = 6;

  SDL_Rect bg_rect;
  bg_rect.x = 12;
  bg_rect.y = 12;
  bg_rect.w = content_w + pad_x * 2;
  bg_rect.h = content_h + pad_y * 2;

  // 1. Draw semi-transparent dark background bar
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
  SDL_RenderFillRect(renderer, &bg_rect);

  // 2. Draw border outline
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
  SDL_RenderDrawRect(renderer, &bg_rect);

  // 3. Render guide text texture
  if (*tex_slot) {
    SDL_Rect text_rect;
    text_rect.x = bg_rect.x + pad_x;
    text_rect.y = bg_rect.y + pad_y;
    text_rect.w = *w_slot;
    text_rect.h = *h_slot;
    SDL_RenderCopy(renderer, *tex_slot, NULL, &text_rect);
  }
}
