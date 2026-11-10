#include "text_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TTF_Font *g_font = NULL;
static int g_font_size = 18;

// LRU cache for general text & UI button textures to avoid per-frame
// TTF_RenderUTF8_Blended + SDL_CreateTextureFromSurface overhead
#define TEXT_CACHE_CAPACITY 96

typedef struct TextCacheEntry {
  bool in_use;
  char text[192];
  SDL_Color color;
  SDL_Texture *texture;
  int w;
  int h;
  uint32_t last_used_tick;
} TextCacheEntry;

static TextCacheEntry g_text_cache[TEXT_CACHE_CAPACITY];
static uint32_t g_text_cache_clock = 0;

static void clear_text_cache(void) {
  for (int i = 0; i < TEXT_CACHE_CAPACITY; ++i) {
    if (g_text_cache[i].texture) {
      SDL_DestroyTexture(g_text_cache[i].texture);
      g_text_cache[i].texture = NULL;
    }
    g_text_cache[i].in_use = false;
  }
}

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
  clear_text_cache();

  if (g_font) {
    TTF_CloseFont(g_font);
    g_font = NULL;
  }
}

void ui_format_shutter_speed(int32_t exposure_us, char *buf, size_t buf_size) {
  if (!buf || buf_size == 0) {
    return;
  }
  if (exposure_us <= 0) {
    snprintf(buf, buf_size, "---");
    return;
  }
  if (exposure_us >= 1000000) {
    double secs = (double)exposure_us / 1000000.0;
    snprintf(buf, buf_size, "%.1fs", secs);
  } else {
    int denom = (int)(1000000.0 / (double)exposure_us + 0.5);
    if (denom <= 1) {
      snprintf(buf, buf_size, "1.0s");
    } else {
      snprintf(buf, buf_size, "1/%ds", denom);
    }
  }
}

void ui_calc_aspect_fit_rect(int src_w, int src_h, const SDL_Rect *bounds,
                             SDL_Rect *out_rect) {
  if (!out_rect) {
    return;
  }
  if (!bounds) {
    *out_rect = (SDL_Rect){0, 0, 0, 0};
    return;
  }
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

bool text_renderer_measure_text(const char *text, int *out_w, int *out_h) {
  if (!g_font || !text || text[0] == '\0') {
    if (out_w) {
      *out_w = 0;
    }
    if (out_h) {
      *out_h = g_font ? TTF_FontHeight(g_font) : 18;
    }
    return false;
  }

  int w = 0;
  int h = 0;
  if (TTF_SizeUTF8(g_font, text, &w, &h) != 0) {
    return false;
  }
  if (out_w) {
    *out_w = w;
  }
  if (out_h) {
    *out_h = h;
  }
  return true;
}

void text_renderer_draw_text(SDL_Renderer *renderer, const char *text, int x,
                             int y, SDL_Color color, int *out_w, int *out_h) {
  if (out_w) {
    *out_w = 0;
  }
  if (out_h) {
    *out_h = g_font ? TTF_FontHeight(g_font) : 18;
  }
  if (!renderer || !g_font || !text || text[0] == '\0') {
    return;
  }

  size_t text_len = strlen(text);
  if (text_len < sizeof(g_text_cache[0].text)) {
    g_text_cache_clock++;

    /* 1. Lookup in LRU texture cache */
    for (int i = 0; i < TEXT_CACHE_CAPACITY; ++i) {
      TextCacheEntry *e = &g_text_cache[i];
      if (e->in_use && e->texture && e->color.r == color.r &&
          e->color.g == color.g && e->color.b == color.b &&
          e->color.a == color.a && strcmp(e->text, text) == 0) {
        e->last_used_tick = g_text_cache_clock;
        SDL_Rect dst = {x, y, e->w, e->h};
        SDL_RenderCopy(renderer, e->texture, NULL, &dst);
        if (out_w) {
          *out_w = e->w;
        }
        if (out_h) {
          *out_h = e->h;
        }
        return;
      }
    }

    /* 2. Rasterize and insert into free or LRU slot */
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font, text, color);
    if (!surf) {
      return;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    int w = surf->w;
    int h = surf->h;
    SDL_FreeSurface(surf);
    if (!tex) {
      return;
    }

    int target_slot = -1;
    uint32_t oldest_tick = UINT32_MAX;
    for (int i = 0; i < TEXT_CACHE_CAPACITY; ++i) {
      if (!g_text_cache[i].in_use) {
        target_slot = i;
        break;
      }
      if (g_text_cache[i].last_used_tick <= oldest_tick) {
        oldest_tick = g_text_cache[i].last_used_tick;
        target_slot = i;
      }
    }

    if (target_slot >= 0) {
      TextCacheEntry *e = &g_text_cache[target_slot];
      if (e->texture) {
        SDL_DestroyTexture(e->texture);
      }
      e->in_use = true;
      snprintf(e->text, sizeof(e->text), "%s", text);
      e->color = color;
      e->texture = tex;
      e->w = w;
      e->h = h;
      e->last_used_tick = g_text_cache_clock;
    }

    SDL_Rect dst = {x, y, w, h};
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    if (target_slot < 0) {
      SDL_DestroyTexture(tex);
    }
    if (out_w) {
      *out_w = w;
    }
    if (out_h) {
      *out_h = h;
    }
    return;
  }

  /* Fallback for strings longer than cache entry buffer */
  SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font, text, color);
  if (!surf) {
    return;
  }

  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
  if (tex) {
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_RenderCopy(renderer, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
  }

  if (out_w) {
    *out_w = surf->w;
  }
  if (out_h) {
    *out_h = surf->h;
  }
  SDL_FreeSurface(surf);
}

void text_renderer_draw_panel(SDL_Renderer *renderer, const SDL_Rect *rect,
                              SDL_Color bg_color, SDL_Color border_color) {
  if (!renderer || !rect) {
    return;
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, bg_color.r, bg_color.g, bg_color.b,
                         bg_color.a);
  SDL_RenderFillRect(renderer, rect);

  if (border_color.a > 0) {
    SDL_SetRenderDrawColor(renderer, border_color.r, border_color.g,
                           border_color.b, border_color.a);
    SDL_RenderDrawRect(renderer, rect);
  }
}

void text_renderer_draw_badge(SDL_Renderer *renderer, const char *text, int x,
                              int y, SDL_Color text_color, SDL_Color bg_color,
                              SDL_Color border_color, SDL_Rect *out_box) {
  if (!renderer || !text) {
    return;
  }

  int tw = 0;
  int th = 0;
  if (!text_renderer_measure_text(text, &tw, &th)) {
    tw = (int)strlen(text) * 9;
    th = 18;
  }

  const int pad_x = 12;
  const int pad_y = 6;
  SDL_Rect box = {x, y, tw + pad_x * 2, th + pad_y * 2};
  text_renderer_draw_panel(renderer, &box, bg_color, border_color);
  text_renderer_draw_text(renderer, text, x + pad_x, y + pad_y, text_color,
                          NULL, NULL);

  if (out_box) {
    *out_box = box;
  }
}

void text_renderer_draw_button(SDL_Renderer *renderer, const char *text, int x,
                               int y, int min_w, int min_h, UiButtonStyle style,
                               bool hovered, bool pressed, SDL_Rect *out_box) {
  if (!renderer || !text) {
    if (out_box) {
      SDL_Rect empty = {0, 0, 0, 0};
      *out_box = empty;
    }
    return;
  }

  int tw = 0;
  int th = 0;
  if (!text_renderer_measure_text(text, &tw, &th)) {
    tw = (int)strlen(text) * 9;
    th = 18;
  }

  const int pad_x = 12;
  const int pad_y = 7;
  int w = tw + pad_x * 2;
  int h = th + pad_y * 2;
  if (min_w > w) {
    w = min_w;
  }
  if (min_h > h) {
    h = min_h;
  }

  SDL_Color fg = {245, 248, 255, 255};
  SDL_Color bg = {18, 22, 30, 205};
  SDL_Color bd = {150, 165, 190, 140};

  switch (style) {
  case UI_BTN_STYLE_ACTIVE:
    fg = (SDL_Color){255, 255, 255, 255};
    bg = (SDL_Color){16, 105, 155, 235};
    bd = (SDL_Color){90, 220, 255, 255};
    break;
  case UI_BTN_STYLE_ACCENT:
    fg = (SDL_Color){255, 235, 110, 255};
    bg = (SDL_Color){95, 65, 8, 225};
    bd = (SDL_Color){255, 205, 55, 240};
    break;
  case UI_BTN_STYLE_DANGER:
    fg = (SDL_Color){255, 245, 245, 255};
    bg = (SDL_Color){170, 32, 38, 230};
    bd = (SDL_Color){255, 130, 130, 245};
    break;
  case UI_BTN_STYLE_SUCCESS:
    fg = (SDL_Color){230, 255, 235, 255};
    bg = (SDL_Color){22, 115, 55, 230};
    bd = (SDL_Color){110, 240, 145, 240};
    break;
  case UI_BTN_STYLE_DEFAULT:
  default:
    break;
  }

  if (pressed) {
    bg.r = (Uint8)(bg.r + (255 - bg.r) / 3);
    bg.g = (Uint8)(bg.g + (255 - bg.g) / 3);
    bg.b = (Uint8)(bg.b + (255 - bg.b) / 3);
    bg.a = 250;
    bd = (SDL_Color){255, 255, 255, 255};
  } else if (hovered) {
    bg.r = (Uint8)(bg.r + (255 - bg.r) / 6);
    bg.g = (Uint8)(bg.g + (255 - bg.g) / 6);
    bg.b = (Uint8)(bg.b + (255 - bg.b) / 6);
    bg.a = 235;
    bd.a = 235;
  }

  SDL_Rect box = {x, y, w, h};
  text_renderer_draw_panel(renderer, &box, bg, bd);

  /* Draw a second inner border when active or hovered for crisp visibility */
  if (style == UI_BTN_STYLE_ACTIVE || style == UI_BTN_STYLE_DANGER || hovered ||
      pressed) {
    SDL_Rect inner = {x + 1, y + 1, w - 2, h - 2};
    SDL_Color inner_bd = {bd.r, bd.g, bd.b, (Uint8)(bd.a / 2)};
    SDL_SetRenderDrawColor(renderer, inner_bd.r, inner_bd.g, inner_bd.b,
                           inner_bd.a);
    SDL_RenderDrawRect(renderer, &inner);
  }

  int tx = x + (w - tw) / 2;
  int ty = y + (h - th) / 2;
  text_renderer_draw_text(renderer, text, tx, ty, fg, NULL, NULL);

  if (out_box) {
    *out_box = box;
  }
}
