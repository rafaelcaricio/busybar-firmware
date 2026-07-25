/**
 * The control deck: an on-screen stand-in for the device's top surface.
 *
 * The hardware has a five-position mode lever (BUSY, CUSTOM, OFF, APPS,
 * SETTINGS), a wide Start/Pause pad, a small Back button and a dial that
 * scrolls and presses for OK. Each of those maps to one InputKey, so the deck
 * is really just a clickable picture of the button set.
 *
 * Rendering and hit-testing happen on the SDL thread; the caller turns a hit
 * into the same key events the keyboard produces.
 */
#pragma once

#include <SDL.h>

#include <stdbool.h>
#include <stdint.h>

/** Lay the controls out inside @p deck. Call before render or hit-test. */
void sim_controls_layout(const SDL_Rect* deck);

/** Draw the deck. @p pressed is indexed by InputKey. */
void sim_controls_render(SDL_Renderer* renderer, const bool* pressed);

/** Find the control under a point. Returns false if none. */
bool sim_controls_hit(int x, int y, uint8_t* key);

/** Check every control's centre hit-resolves to its own key. */
bool sim_controls_selftest(void);

/** Height the deck wants for a given window width. */
int sim_controls_preferred_height(int width);
