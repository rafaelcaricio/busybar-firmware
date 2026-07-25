#pragma once

#include <furi_hal_resources.h>

/** Create the input pubsub record and start the key translation task. */
void input_host_init(void);

/** Which mode the selector is parked on, as an InputKey, or InputKeyMAX.
 *
 * The five mode keys are positions of a lever rather than buttons, so the deck
 * draws the selected one held down. Read from the SDL thread while the input
 * task writes it, which is why it is a single key and not a struct.
 */
InputKey input_host_get_switch_key(void);

/* Scroll direction, named after the screen rather than the dial.
 *
 * The device scrolls with a dial and the firmware names the two directions
 * after it: rotating one way is InputKeyUp, which the widget layer turns into
 * lv_group_focus_next() -- the item *below* the highlighted one. Verified in
 * the settings list: sending "up" moves the highlight from Bluetooth down to
 * Wi-Fi.
 *
 * The simulator's affordances are an arrow key, a scroll wheel and a pair of
 * on-screen pads, all of which point at the screen and are expected to move
 * the highlight the way they point. They use these two rather than the raw
 * InputKey names, so the one inversion lives here instead of in three places.
 */
#define SIM_KEY_SCROLL_UP   InputKeyDown
#define SIM_KEY_SCROLL_DOWN InputKeyUp
