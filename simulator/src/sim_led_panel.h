#pragma once

#include <SDL.h>

/** Draws a framebuffer the way the front panel's LED matrix shows it.
 *
 * The front display is not a screen: it is a grid of discrete LEDs behind a
 * dark diffuser, so an unlit panel still shows its dots and a lit one hazes
 * into the gaps between them. Scaling the framebuffer up gives flat blocks
 * instead, which reads as a much brighter and much flatter display than the
 * device has.
 */
typedef struct SimLedPanel SimLedPanel;

SimLedPanel* sim_led_panel_alloc(SDL_Renderer* renderer, int columns, int rows);

void sim_led_panel_free(SimLedPanel* instance);

/** Draw @p frame into @p area as an LED matrix.
 *
 * @p frame is left with the scale mode, blend mode and modulation it arrived
 * with, so the caller can keep drawing it plainly elsewhere.
 */
void sim_led_panel_render(SimLedPanel* instance, SDL_Texture* frame, const SDL_Rect* area);
