#include "sim_recorder.h"
#include "sim_window.h"

#include <furi.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "SimRecorder"

/* Three buffers is enough to cover the writer being busy with one frame while
 * the SDL thread fills the next: at any sane frame rate the downscale finishes
 * long inside a frame period, and a deeper pool would only hide a problem
 * worth reporting as a drop. */
#define SIM_RECORDER_POOL (3)

/* How long the writer sleeps with an empty queue. Short enough that stopping
 * is not felt, long enough not to spin. */
#define SIM_RECORDER_IDLE_MS (8)

/* How long stop() waits for the SDL thread to finish the frame it is in. */
#define SIM_RECORDER_SETTLE_MS (500)

static struct {
    pthread_mutex_t lock;

    bool running;
    bool writer_stop;

    int fps;
    int divisor;
    /* What the SDL thread hands over... */
    int source_width;
    int source_height;
    /* ...and what lands in the stream. */
    int width;
    int height;

    uint8_t* pool[SIM_RECORDER_POOL];
    int free_list[SIM_RECORDER_POOL];
    int free_count;
    int queue[SIM_RECORDER_POOL];
    int queue_head;
    int queue_count;
    /* Handed to the SDL thread and not in either list until the frame lands. */
    int filling;

    uint64_t period_ms;
    uint64_t next_due_ms;
    /* When frames were taken, rather than when they were asked for. */
    uint64_t first_frame_ms;
    uint64_t last_frame_ms;

    unsigned frames;
    unsigned dropped;

    uint8_t* scaled;
    FILE* stream;
    FuriThread* writer;
} recorder = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .filling = -1,
};

/** Average each divisor x divisor block down to one pixel.
 *
 * A box filter rather than a sample: the front panel is a grid of lit dots on
 * black, and dropping pixels rather than mixing them turns it into moire.
 */
static void sim_recorder_downscale(const uint8_t* source, uint8_t* target) {
    const int divisor = recorder.divisor;
    const size_t stride = (size_t)recorder.source_width * 3;
    const unsigned area = (unsigned)divisor * (unsigned)divisor;

    for(int y = 0; y < recorder.height; y++) {
        for(int x = 0; x < recorder.width; x++) {
            unsigned red = 0, green = 0, blue = 0;

            for(int block_y = 0; block_y < divisor; block_y++) {
                const uint8_t* row =
                    source + (size_t)(y * divisor + block_y) * stride + (size_t)(x * divisor) * 3;

                for(int block_x = 0; block_x < divisor; block_x++) {
                    red += row[block_x * 3 + 0];
                    green += row[block_x * 3 + 1];
                    blue += row[block_x * 3 + 2];
                }
            }

            uint8_t* pixel = target + ((size_t)y * recorder.width + x) * 3;
            pixel[0] = (uint8_t)(red / area);
            pixel[1] = (uint8_t)(green / area);
            pixel[2] = (uint8_t)(blue / area);
        }
    }
}

/** Take the oldest queued frame, or -1 when there is none. */
static int sim_recorder_dequeue(bool* stopping) {
    int index = -1;

    pthread_mutex_lock(&recorder.lock);
    if(recorder.queue_count > 0) {
        index = recorder.queue[recorder.queue_head];
        recorder.queue_head = (recorder.queue_head + 1) % SIM_RECORDER_POOL;
        recorder.queue_count--;
    }
    *stopping = recorder.writer_stop;
    pthread_mutex_unlock(&recorder.lock);

    return index;
}

static void sim_recorder_release(int index, bool written) {
    pthread_mutex_lock(&recorder.lock);
    recorder.free_list[recorder.free_count++] = index;
    if(written) recorder.frames++;
    pthread_mutex_unlock(&recorder.lock);
}

/** Scale and write queued frames until stop() has said there are no more.
 *
 * Polls rather than waiting on a condition variable: the queue is filled from
 * the SDL thread under a plain pthread mutex, and a task parked on a pthread
 * primitive is outside what the FreeRTOS port knows how to suspend.
 */
static int32_t sim_recorder_writer(void* context) {
    UNUSED(context);

    while(true) {
        bool stopping = false;
        const int index = sim_recorder_dequeue(&stopping);

        if(index < 0) {
            if(stopping) break;
            furi_delay_ms(SIM_RECORDER_IDLE_MS);
            continue;
        }

        const uint8_t* frame = recorder.pool[index];
        size_t size = (size_t)recorder.source_width * recorder.source_height * 3;

        if(recorder.divisor > 1) {
            sim_recorder_downscale(frame, recorder.scaled);
            frame = recorder.scaled;
            size = (size_t)recorder.width * recorder.height * 3;
        }

        const bool written = fwrite(frame, 1, size, recorder.stream) == size;
        if(!written) FURI_LOG_E(TAG, "short write; the stream is truncated");

        sim_recorder_release(index, written);
    }

    return 0;
}

static void sim_recorder_free_buffers(void) {
    for(int i = 0; i < SIM_RECORDER_POOL; i++) {
        free(recorder.pool[i]);
        recorder.pool[i] = NULL;
    }

    free(recorder.scaled);
    recorder.scaled = NULL;
}

bool sim_recorder_start(const char* path, int fps, int divisor, char* error, size_t error_size) {
    if(fps <= 0 || fps > 60) {
        snprintf(error, error_size, "fps out of range: %d", fps);
        return false;
    }

    if(divisor < 1 || divisor > 8) {
        snprintf(error, error_size, "divisor out of range: %d", divisor);
        return false;
    }

    pthread_mutex_lock(&recorder.lock);
    const bool busy = recorder.running;
    pthread_mutex_unlock(&recorder.lock);

    if(busy) {
        snprintf(error, error_size, "already recording");
        return false;
    }

    int width = 0, height = 0;
    sim_window_canvas_size(&width, &height);

    /* Only whole blocks are averaged, so a canvas that does not divide evenly
     * loses at most divisor-1 pixels off the right and bottom edges. */
    const int out_width = width / divisor;
    const int out_height = height / divisor;

    if(out_width <= 0 || out_height <= 0) {
        snprintf(
            error, error_size, "canvas is %dx%d, too small for divisor %d", width, height, divisor);
        return false;
    }

    FILE* stream = fopen(path, "wb");
    if(!stream) {
        snprintf(error, error_size, "cannot open %s", path);
        return false;
    }

    recorder.source_width = width;
    recorder.source_height = height;
    recorder.width = out_width;
    recorder.height = out_height;
    recorder.divisor = divisor;
    recorder.fps = fps;
    recorder.period_ms = 1000 / (uint64_t)fps;
    recorder.next_due_ms = 0;
    recorder.first_frame_ms = 0;
    recorder.last_frame_ms = 0;
    recorder.frames = 0;
    recorder.dropped = 0;
    recorder.stream = stream;
    recorder.writer_stop = false;
    recorder.filling = -1;
    recorder.free_count = 0;
    recorder.queue_head = 0;
    recorder.queue_count = 0;

    const size_t frame_size = (size_t)width * height * 3;

    for(int i = 0; i < SIM_RECORDER_POOL; i++) {
        recorder.pool[i] = malloc(frame_size);
        if(!recorder.pool[i]) {
            sim_recorder_free_buffers();
            fclose(stream);
            recorder.stream = NULL;
            snprintf(error, error_size, "out of memory for %zu byte frames", frame_size);
            return false;
        }
        recorder.free_list[recorder.free_count++] = i;
    }

    if(divisor > 1) {
        recorder.scaled = malloc((size_t)out_width * out_height * 3);
        if(!recorder.scaled) {
            sim_recorder_free_buffers();
            fclose(stream);
            recorder.stream = NULL;
            snprintf(error, error_size, "out of memory for the scaled frame");
            return false;
        }
    }

    recorder.writer = furi_thread_alloc_ex("sim_recorder", 8 * 1024, sim_recorder_writer, NULL);

    pthread_mutex_lock(&recorder.lock);
    recorder.running = true;
    pthread_mutex_unlock(&recorder.lock);

    furi_thread_start(recorder.writer);

    FURI_LOG_I(TAG, "recording %dx%d at %d fps into %s", out_width, out_height, fps, path);

    return true;
}

bool sim_recorder_stop(SimRecorderStatus* status, char* error, size_t error_size) {
    pthread_mutex_lock(&recorder.lock);
    const bool running = recorder.running;
    recorder.running = false;
    pthread_mutex_unlock(&recorder.lock);

    if(!running) {
        snprintf(error, error_size, "not recording");
        return false;
    }

    /* The SDL thread may be part-way through reading a frame back into a pool
     * buffer; the pool cannot be freed under it. */
    for(int waited = 0; waited < SIM_RECORDER_SETTLE_MS; waited += 5) {
        pthread_mutex_lock(&recorder.lock);
        const bool filling = recorder.filling >= 0;
        pthread_mutex_unlock(&recorder.lock);

        if(!filling) break;
        furi_delay_ms(5);
    }

    pthread_mutex_lock(&recorder.lock);
    recorder.writer_stop = true;
    pthread_mutex_unlock(&recorder.lock);

    furi_thread_join(recorder.writer);
    furi_thread_free(recorder.writer);
    recorder.writer = NULL;

    fclose(recorder.stream);
    recorder.stream = NULL;

    sim_recorder_free_buffers();

    sim_recorder_status(status);

    FURI_LOG_I(
        TAG,
        "recorded %u frames (%u dropped) at %dx%d over %u ms",
        status->frames,
        status->dropped,
        status->width,
        status->height,
        status->elapsed_ms);

    return true;
}

void sim_recorder_status(SimRecorderStatus* status) {
    pthread_mutex_lock(&recorder.lock);
    status->running = recorder.running;
    status->frames = recorder.frames;
    status->dropped = recorder.dropped;
    status->width = recorder.width;
    status->height = recorder.height;
    status->fps = recorder.fps;
    status->elapsed_ms = (unsigned)(recorder.last_frame_ms - recorder.first_frame_ms);
    pthread_mutex_unlock(&recorder.lock);
}

uint8_t* sim_recorder_frame_due(int canvas_width, int canvas_height, uint64_t now_ms) {
    uint8_t* frame = NULL;

    pthread_mutex_lock(&recorder.lock);

    if(!recorder.running || recorder.filling >= 0) goto out;

    /* The window can be resized while recording; the stream cannot change
     * geometry part-way through, so those frames are lost on purpose. */
    if(canvas_width != recorder.source_width || canvas_height != recorder.source_height) {
        recorder.dropped++;
        goto out;
    }

    if(recorder.next_due_ms == 0) recorder.next_due_ms = now_ms;
    if(now_ms < recorder.next_due_ms) goto out;

    /* Schedule the next frame from the due time, not from now, so the stream
     * keeps the rate it claims. A late frame that would put the next one in
     * the past resets the phase rather than firing a burst to catch up. */
    recorder.next_due_ms += recorder.period_ms;
    if(recorder.next_due_ms <= now_ms) recorder.next_due_ms = now_ms + recorder.period_ms;

    if(recorder.free_count == 0) {
        recorder.dropped++;
        goto out;
    }

    recorder.filling = recorder.free_list[--recorder.free_count];
    frame = recorder.pool[recorder.filling];

    if(recorder.first_frame_ms == 0) recorder.first_frame_ms = now_ms;
    recorder.last_frame_ms = now_ms;

out:
    pthread_mutex_unlock(&recorder.lock);
    return frame;
}

void sim_recorder_frame_ready(void) {
    pthread_mutex_lock(&recorder.lock);

    const int index = recorder.filling;
    recorder.filling = -1;

    if(index >= 0) {
        if(recorder.running) {
            const int tail = (recorder.queue_head + recorder.queue_count) % SIM_RECORDER_POOL;
            recorder.queue[tail] = index;
            recorder.queue_count++;
        } else {
            /* Stopped while the frame was being read back: hand the buffer
             * back so stop() can free the pool. */
            recorder.free_list[recorder.free_count++] = index;
        }
    }

    pthread_mutex_unlock(&recorder.lock);
}
