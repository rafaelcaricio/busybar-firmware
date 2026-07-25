/**
 * The device renders the window draws its virtual displays onto.
 *
 * Each side is one image with three rectangles measured in that image's own
 * pixels: the case, the black display face, and the display itself. The window
 * asks for a case placed at a given display-face width, and gets back where
 * the display has to go inside it — so the front matrix and the back screen
 * land exactly where they sit on the hardware, at any window size.
 *
 * The face rather than the case is what the layout scales by: the two images
 * were rendered from slightly different angles, so their cases differ in width
 * by 3%, while the black face is the same physical part on both.
 */
#pragma once

#include <SDL.h>

#include <stdbool.h>

typedef enum {
    SimBackgroundSideFront,
    SimBackgroundSideBack,
} SimBackgroundSide;

typedef struct SimBackground SimBackground;

/** Load one side. Returns NULL if the generated frame is missing. */
SimBackground* sim_background_alloc(SDL_Renderer* renderer, SimBackgroundSide side);

void sim_background_free(SimBackground* instance);

/** Case width as a multiple of the display face's, for the width budget. */
float sim_background_width_ratio(const SimBackground* instance);

/** Case height as a multiple of the display face's width, for the height budget. */
float sim_background_height_ratio(const SimBackground* instance);

/** Display width as a fraction of the display face's, to size the window from
 * a wanted magnification. */
float sim_background_display_ratio(const SimBackground* instance);

/** Place the case so its display face is @p face_width wide and centred on
 * @p face_centre_x, with the case's top edge at @p top. */
SDL_Rect sim_background_case_rect(
    const SimBackground* instance,
    int face_width,
    int face_centre_x,
    int top);

/** Where the virtual display goes inside @p case_rect. */
SDL_Rect sim_background_display_rect(const SimBackground* instance, const SDL_Rect* case_rect);

/** The window the display sits behind: how far its light may spill. */
SDL_Rect sim_background_window_rect(const SimBackground* instance, const SDL_Rect* case_rect);

void sim_background_render(SimBackground* instance, const SDL_Rect* case_rect);
