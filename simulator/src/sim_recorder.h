/**
 * The window as raw video, for the times a still will not do.
 *
 * A screenshot answers "what is on the panel"; a wipe between scenes, a
 * scrolling label or the busy timer counting down are only visible in motion.
 * The recorder takes the same read-back the screenshot writer uses and repeats
 * it at a fixed rate into a stream of raw RGB24 frames.
 *
 * Nothing here encodes. The frames leave the simulator raw and ffmpeg makes
 * the GIF (see tools/simctl.py record), which quantises a panel of discrete
 * LEDs far better than a hand-rolled palette would, and scales it for free.
 *
 * Threading: sim_recorder_frame_due() and sim_recorder_frame_ready() are the
 * SDL thread's half, and neither allocates nor blocks — the buffers they fill
 * are taken from a pool allocated when recording starts. Everything else runs
 * in a task, because malloc() on this port is furi's heap.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool running;
    /** Frames written to the stream. */
    unsigned frames;
    /** Frames the SDL thread had to skip: the writer was still behind, or the
     * window changed size and the stream's geometry no longer matched. */
    unsigned dropped;
    /** Geometry of the stream, after the divisor. */
    int width;
    int height;
    /** The rate that was asked for. */
    int fps;
    /** Wall clock between the first frame and the last.
     *
     * Reading a whole window back off the GPU is not free, and it happens on
     * the thread that presents, so a capture can cost more than the frame
     * period it was asked for and the stream comes out slower than @c fps.
     * That is what the frames are actually spaced by, and encoding them at
     * anything else plays the recording back at the wrong speed.
     */
    unsigned elapsed_ms;
} SimRecorderStatus;

/** Open @p path and start recording. Task context only.
 *
 * @p divisor shrinks each frame by an integer factor, averaged over the block
 * rather than sampled, which is what a grid of LEDs needs. @p error takes a
 * message when this returns false.
 */
bool sim_recorder_start(const char* path, int fps, int divisor, char* error, size_t error_size);

/** Drain the queue, close the stream and report what was written. Task only. */
bool sim_recorder_stop(SimRecorderStatus* status, char* error, size_t error_size);

/** Frames and geometry so far, running or not. Safe from any thread. */
void sim_recorder_status(SimRecorderStatus* status);

/** A buffer to read the finished frame into, or NULL when none is due.
 *
 * SDL thread only, and always paired with sim_recorder_frame_ready(): the
 * buffer is held out of the pool until the frame lands.
 */
uint8_t* sim_recorder_frame_due(int canvas_width, int canvas_height, uint64_t now_ms);

/** Hand the filled buffer to the writer. SDL thread only. */
void sim_recorder_frame_ready(void);
