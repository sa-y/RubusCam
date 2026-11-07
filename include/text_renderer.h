#ifndef TEXT_RENDERER_H
#define TEXT_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "camera_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the text renderer and load a TrueType font.
 * If font_path is NULL, automatically searches common system font paths.
 * Returns true on success, false if initialization or font loading fails.
 */
bool text_renderer_init(const char *font_path, int font_size);

/*
 * Release the loaded font and free any cached overlay textures.
 */
void text_renderer_destroy(void);

/*
 * Draw the overlay guide bar matching the pycamera GUI using GPU acceleration.
 * Renders a semi-transparent dark background bar and white guide text.
 */
void text_renderer_draw_guide(SDL_Renderer *renderer, bool af_supported);

/*
 * Draw the exposure status HUD bar at the bottom-left of the screen.
 * Displays current shutter speed, ISO sensitivity, EV compensation, JPEG
 * quality preset label, and background save queue indicator.
 * Uses texture caching to avoid re-rasterizing unchanged text strings.
 */
void text_renderer_draw_exposure_hud(SDL_Renderer *renderer, int win_w,
                                     int win_h,
                                     const CameraExposureStatus *status,
                                     const char *quality_label,
                                     int pending_saves);

/*
 * Measure the pixel width and height of a UTF-8 string using the loaded font.
 * Returns true on success, false if no font is loaded.
 */
bool text_renderer_measure_text(const char *text, int *out_w, int *out_h);

/*
 * Render a UTF-8 string at (x, y) with the specified color.
 * Optionally outputs the rendered width and height.
 */
void text_renderer_draw_text(SDL_Renderer *renderer, const char *text, int x,
                             int y, SDL_Color color, int *out_w, int *out_h);

/*
 * Draw a semi-transparent bordered rectangular panel at rect.
 */
void text_renderer_draw_panel(SDL_Renderer *renderer, const SDL_Rect *rect,
                              SDL_Color bg_color, SDL_Color border_color);

/*
 * Draw a single-line pill/badge with background panel and border at (x, y).
 * Outputs the total bounding box in out_box if non-NULL.
 */
void text_renderer_draw_badge(SDL_Renderer *renderer, const char *text, int x,
                              int y, SDL_Color text_color, SDL_Color bg_color,
                              SDL_Color border_color, SDL_Rect *out_box);

/*
 * Visual style presets for interactive touch/mouse UI buttons.
 */
typedef enum UiButtonStyle {
  UI_BTN_STYLE_DEFAULT = 0, /* Semi-transparent dark button */
  UI_BTN_STYLE_ACTIVE = 1,  /* Cyan/blue active or selected state */
  UI_BTN_STYLE_ACCENT = 2,  /* Gold/amber accent (e.g. Lock / AF) */
  UI_BTN_STYLE_DANGER = 3,  /* Red button (e.g. Shutter / Delete) */
  UI_BTN_STYLE_SUCCESS = 4  /* Green confirmation/success button */
} UiButtonStyle;

/*
 * Draw an interactive touch/mouse button at (x, y) with minimum dimensions
 * (min_w, min_h), centered label text, and visual feedback for hover/pressed.
 * Outputs the hit-test bounding box in out_box if non-NULL.
 */
void text_renderer_draw_button(SDL_Renderer *renderer, const char *text, int x,
                               int y, int min_w, int min_h, UiButtonStyle style,
                               bool hovered, bool pressed, SDL_Rect *out_box);

/*
 * Hit-test helper: returns true if point (px, py) is inside rect.
 */
static inline bool ui_rect_contains(const SDL_Rect *rect, int px, int py) {
  return rect && rect->w > 0 && rect->h > 0 && px >= rect->x &&
         px < (rect->x + rect->w) && py >= rect->y && py < (rect->y + rect->h);
}

#ifdef __cplusplus
}
#endif

#endif /* TEXT_RENDERER_H */
