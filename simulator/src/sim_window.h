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

typedef struct {
    uint64_t frames;
    uint64_t front_updates;
    uint64_t back_updates;
    int canvas_width;
    int canvas_height;
    bool quitting;
} SimWindowStatus;

/** Create the window. Main thread only, before the scheduler starts. */
bool sim_window_init(int scale);

void sim_window_deinit(void);

/** Publish a front display frame (RGB888, FRONT_DISPLAY_W x FRONT_DISPLAY_H). */
void sim_window_submit_front(const uint8_t* pixels);

/** Publish a back display frame (L8, BACK_DISPLAY_W x BACK_DISPLAY_H). */
void sim_window_submit_back(const uint8_t* pixels);

void sim_window_set_front_blanked(bool blanked);

void sim_window_set_front_sleeping(bool sleeping);

void sim_window_set_front_brightness(uint8_t brightness);

/** Mirror the back display service's reference-counted sleep contract. */
void sim_window_change_back_sleep(bool sleeping);

void sim_window_set_back_contrast(uint8_t contrast);

/** Drain SDL events and repaint. Main thread only. Returns false to quit. */
bool sim_window_pump(void);

/** Snapshot render progress for tools and deterministic tests. */
void sim_window_status(SimWindowStatus* status);

/** Wait until @p target has been presented, using rendered frames rather than
 * a wall-clock guess. Must be called from a FreeRTOS task. */
bool sim_window_wait_for_frame(uint64_t target, uint32_t timeout_ms);

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

/** Size of the render target a whole-window capture comes back at.
 *
 * Not the window size: on a HiDPI display the renderer is larger than the
 * window it is presented in, and a capture is in the renderer's pixels.
 */
void sim_window_canvas_size(int* width, int* height);

/** Write both displays and the whole window into @p directory.
 *
 * @p sequence 0 writes front.png, back.png, their fixed-size raw variants,
 * and window.png; anything else appends it, so repeated captures of a running
 * simulator do not overwrite each other.
 *
 * Encodes through lodepng, which allocates from furi's heap, so this must be
 * called from a FreeRTOS task rather than the SDL thread. Returns false if any
 * image could not be captured or written. */
bool sim_window_screenshot(const char* directory, unsigned sequence);

/** Take the next numbered capture, serialized with F12/SIGUSR2 captures.
 * Returns the chosen sequence and the exact rendered frame when requested. */
bool sim_window_screenshot_next(
    const char* directory,
    unsigned* sequence,
    uint64_t* captured_frame);
