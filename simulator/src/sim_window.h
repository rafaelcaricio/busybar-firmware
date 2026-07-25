/**
 * The simulator's SDL window.
 *
 * Threading contract: every SDL call happens on the main thread. The display
 * services run inside FreeRTOS tasks and only ever hand over pixels, which
 * are copied under a plain pthread mutex — no furi primitive is touched from
 * the main thread and no SDL handle is touched from a task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Create the window. Main thread only, before the scheduler starts. */
bool sim_window_init(int scale);

void sim_window_deinit(void);

/** Publish a front display frame (RGB888, FRONT_DISPLAY_W x FRONT_DISPLAY_H). */
void sim_window_submit_front(const uint8_t* pixels);

/** Publish a back display frame (L8, BACK_DISPLAY_W x BACK_DISPLAY_H). */
void sim_window_submit_back(const uint8_t* pixels);

void sim_window_set_front_blanked(bool blanked);

/** Drain SDL events and repaint. Main thread only. Returns false to quit. */
bool sim_window_pump(void);

/** Pop one pending key transition. Returns false when the queue is empty.
 *
 * Fed by sim_window_pump() on the main thread, drained by the input service
 * task, so the queue is the handoff point between the two worlds.
 */
bool sim_window_poll_key(uint8_t* key, bool* pressed);

/** Inject a key transition as if it came from the keyboard.
 *
 * Used by --keys to drive scripted walkthroughs; safe from a task. */
void sim_window_inject_key(uint8_t key, bool pressed);

/** Run the control hit-test check on the next frame. */
void sim_window_request_selftest(void);

/** Ask for a screenshot. Safe from any thread, including a signal handler's
 * companion, since the capture itself has to happen in a task. */
void sim_window_request_screenshot(void);

/** Consume a pending screenshot request. Call from a task. */
bool sim_window_take_screenshot_request(void);

/** Ask the main loop to stop; safe to call from a FreeRTOS task. */
void sim_window_request_quit(void);

/** Write both displays and the whole window into @p directory.
 *
 * @p sequence 0 writes front.png, back.png and window.png; anything else
 * appends it, so repeated captures of a running simulator do not overwrite
 * each other.
 *
 * Encodes through lodepng, which allocates from furi's heap, so this must be
 * called from a FreeRTOS task rather than the SDL thread. */
void sim_window_screenshot(const char* directory, unsigned sequence);
