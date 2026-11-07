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

// Cached texture for exposure HUD overlay
static SDL_Texture *g_hud_texture = NULL;
static int g_hud_w = 0;
static int g_hud_h = 0;
static char g_last_hud_text[384] = {0};

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

  if (g_hud_texture) {
    SDL_DestroyTexture(g_hud_texture);
    g_hud_texture = NULL;
    g_hud_w = 0;
    g_hud_h = 0;
    g_last_hud_text[0] = '\0';
  }

  clear_text_cache();

  if (g_font) {
    TTF_CloseFont(g_font);
    g_font = NULL;
  }
}

static void format_shutter_speed(int32_t exposure_us, char *buf,
                                 size_t buf_size) {
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

void text_renderer_draw_guide(SDL_Renderer *renderer, bool af_supported) {
  if (!renderer) {
    return;
  }

  SDL_Texture **tex_slot =
      af_supported ? &g_guide_texture_af : &g_guide_texture_no_af;
  int *w_slot = af_supported ? &g_guide_w_af : &g_guide_w_no_af;
  int *h_slot = af_supported ? &g_guide_h_af : &g_guide_h_no_af;

  const char *guide_text =
      af_supported
          ? "[SPACE]:Shot [P]:Play [F]:AF [Up/Dn]:EV [I]:ISO [S]:SS [W]:WB "
            "[K]:Temp [L]:Lock [M]:Met [J]:Qual [R]:Reset [Q]:Quit"
          : "[SPACE]:Shot [P]:Play [Up/Dn]:EV [I]:ISO [S]:SS [W]:WB [K]:Temp "
            "[L]:Lock [M]:Met [J]:Qual [R]:Reset [Q]:Quit";

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

void text_renderer_draw_exposure_hud(SDL_Renderer *renderer, int win_w,
                                     int win_h,
                                     const CameraExposureStatus *status,
                                     const char *quality_label,
                                     int pending_saves) {
  (void)win_w;
  if (!renderer || !status) {
    return;
  }

  char ss_str[64];
  if (status->shutter_us_setting > 0) {
    char manual_ss[32];
    format_shutter_speed(status->shutter_us_setting, manual_ss,
                         sizeof(manual_ss));
    snprintf(ss_str, sizeof(ss_str), "SS: %s [M]", manual_ss);
  } else {
    char auto_ss[32];
    format_shutter_speed(status->actual_exposure_us, auto_ss, sizeof(auto_ss));
    snprintf(ss_str, sizeof(ss_str), "SS: Auto (%s)", auto_ss);
  }

  char iso_str[64];
  if (status->iso_setting > 0) {
    snprintf(iso_str, sizeof(iso_str), "ISO: %d [M]", status->iso_setting);
  } else {
    int actual_iso = (int)(status->actual_analogue_gain * 100.0f + 0.5f);
    if (actual_iso < 100) {
      actual_iso = 100;
    }
    // Quantize display to 10s so tiny sensor gain jitter doesn't invalidate
    // texture cache every frame
    actual_iso = ((actual_iso + 5) / 10) * 10;
    snprintf(iso_str, sizeof(iso_str), "ISO: Auto (%d)", actual_iso);
  }

  const char *q_str = (quality_label && quality_label[0] != '\0')
                          ? quality_label
                          : "Fine (90%)";

  char wb_str[64];
  int quant_k = status->actual_colour_temp_k > 0
                    ? ((status->actual_colour_temp_k + 50) / 100) * 100
                    : 5500;
  if (status->awb_locked) {
    snprintf(wb_str, sizeof(wb_str), "WB: LOCK (%dK)", quant_k);
  } else {
    switch (status->wb_mode) {
    case CAMERA_WB_MODE_DAYLIGHT:
      snprintf(wb_str, sizeof(wb_str), "WB: Daylight");
      break;
    case CAMERA_WB_MODE_CLOUDY:
      snprintf(wb_str, sizeof(wb_str), "WB: Cloudy");
      break;
    case CAMERA_WB_MODE_SHADE:
      snprintf(wb_str, sizeof(wb_str), "WB: Shade");
      break;
    case CAMERA_WB_MODE_TUNGSTEN:
      snprintf(wb_str, sizeof(wb_str), "WB: Tungsten");
      break;
    case CAMERA_WB_MODE_FLUORESCENT:
      snprintf(wb_str, sizeof(wb_str), "WB: Fluoresc");
      break;
    case CAMERA_WB_MODE_KELVIN: {
      int k_val =
          status->wb_kelvin_setting > 0 ? status->wb_kelvin_setting : 5500;
      snprintf(wb_str, sizeof(wb_str), "WB: %dK [M]", k_val);
      break;
    }
    case CAMERA_WB_MODE_AUTO:
    default:
      snprintf(wb_str, sizeof(wb_str), "WB: AWB (%dK)", quant_k);
      break;
    }
  }

  const char *met_str = "Met: Matrix";
  switch (status->metering_mode) {
  case CAMERA_METERING_CENTRE_WEIGHTED:
    met_str = "Met: Center";
    break;
  case CAMERA_METERING_SPOT:
    met_str = "Met: Spot";
    break;
  case CAMERA_METERING_MATRIX:
  default:
    met_str = "Met: Matrix";
    break;
  }

  char save_str[48] = "";
  if (pending_saves > 0) {
    snprintf(save_str, sizeof(save_str), "  |  [Saving: %d]", pending_saves);
  }

  char hud_text[384];
  snprintf(hud_text, sizeof(hud_text),
           "%s  |  %s  |  EV: %+.1f  |  %s  |  %s  |  JPG: %s%s", ss_str,
           iso_str, status->ev_compensation, wb_str, met_str, q_str, save_str);

  // Re-rasterize only when text string changes
  if (strcmp(hud_text, g_last_hud_text) != 0 && g_font) {
    if (g_hud_texture) {
      SDL_DestroyTexture(g_hud_texture);
      g_hud_texture = NULL;
    }
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font, hud_text, white);
    if (surf) {
      g_hud_texture = SDL_CreateTextureFromSurface(renderer, surf);
      g_hud_w = surf->w;
      g_hud_h = surf->h;
      SDL_FreeSurface(surf);
      snprintf(g_last_hud_text, sizeof(g_last_hud_text), "%s", hud_text);
    }
  }

  int content_w = g_hud_w > 0 ? g_hud_w : 360;
  int content_h = g_hud_h > 0 ? g_hud_h : 20;
  int pad_x = 12;
  int pad_y = 6;

  SDL_Rect bg_rect;
  bg_rect.x = 12;
  bg_rect.y = win_h - (content_h + pad_y * 2) - 12;
  bg_rect.w = content_w + pad_x * 2;
  bg_rect.h = content_h + pad_y * 2;

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
  SDL_RenderFillRect(renderer, &bg_rect);

  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
  SDL_RenderDrawRect(renderer, &bg_rect);

  if (g_hud_texture) {
    SDL_Rect text_rect;
    text_rect.x = bg_rect.x + pad_x;
    text_rect.y = bg_rect.y + pad_y;
    text_rect.w = g_hud_w;
    text_rect.h = g_hud_h;
    SDL_RenderCopy(renderer, g_hud_texture, NULL, &text_rect);
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
