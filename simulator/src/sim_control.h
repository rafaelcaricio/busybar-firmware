/**
 * A control channel for the tools that drive the simulator.
 *
 * The HTTP API on --api-port is the firmware's own: it takes button presses
 * and reports device state, and it has no business growing endpoints for
 * screenshots or recordings, which exist only here. Those go over a unix
 * socket instead — one line in, one line out, so the tool side needs nothing
 * but the standard library, same as tools/simctl.py already assumes.
 *
 * Requests, with the reply each gives:
 *
 *   ping                                 ok
 *   status                               ok FRAMES FRONT_UPDATES BACK_UPDATES WIDTH HEIGHT QUITTING HEAP_FREE HEAP_MIN
 *   power                                ok CHARGE USB CHARGING
 *   power CHARGE USB CHARGING            ok CHARGE USB CHARGING
 *   wait frame TARGET TIMEOUT_MS         ok CURRENT_FRAME
 *   screenshot DIRECTORY                 ok SEQUENCE CAPTURED_FRAME
 *   quit                                 ok
 *   record start FPS DIVISOR PATH        ok WIDTH HEIGHT FPS
 *   record stop                          ok FRAMES DROPPED WIDTH HEIGHT FPS ELAPSED_MS
 *   record status                        ok RUNNING FRAMES DROPPED WIDTH HEIGHT FPS ELAPSED_MS
 *
 * Anything that fails answers "error MESSAGE". PATH runs to the end of the
 * line, so it may contain spaces. ELAPSED_MS spans the first frame to the
 * last: the rate a recording came out at is what it has to be played back at,
 * and it is not always the rate that was asked for.
 */
#pragma once

#include <stdbool.h>

/** Serve @p path until the process exits. Task context, once, at startup. */
bool sim_control_init(const char* path);
