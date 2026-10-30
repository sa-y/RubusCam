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
 * Displays current shutter speed, ISO sensitivity, and EV compensation.
 * Uses texture caching to avoid re-rasterizing unchanged text strings.
 */
void text_renderer_draw_exposure_hud(SDL_Renderer *renderer, int win_w,
                                     int win_h,
                                     const CameraExposureStatus *status);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_RENDERER_H */
