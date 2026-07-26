#include "sim_window.h"
#include "input_host.h"
#include "sim_background.h"
#include "sim_controls.h"
#include "sim_led_panel.h"
#include "sim_recorder.h"

#include <front_display/front_display.h>
#include <back_display/back_display.h>
#include <furi_hal_resources.h>

#include <SDL.h>
#include <libs/lodepng/lodepng.h>

#include <furi.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARGIN         (20)
#define GAP            (28)
#define KEY_QUEUE_SIZE (64)

/* Magnification to open at when none is asked for, in pixels per front LED.
 * The matrix is only 72x16, so below about a dozen pixels an LED the panel
 * stops being readable. Trimmed at startup if the screen cannot take it. */
#define SIM_WINDOW_DEFAULT_SCALE (18)

typedef struct {
    uint8_t key;
    bool pressed;
} KeyEvent;

/** The two device renders and the rectangles a frame is drawn into. */
typedef struct {
    SDL_Rect front_case;
    SDL_Rect back_case;
    SDL_Rect front_display;
    SDL_Rect back_display;
    SDL_Rect front_window;
    SDL_Rect deck;
} SimLayout;

static struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* front_texture;
    SDL_Texture* back_texture;
    /* The front panel is an LED matrix and is drawn as one; the back panel is
     * a greyscale display and is drawn as it is. */
    SimLedPanel* front_leds;
    /* The device itself, front above and back below, with the two displays
     * landing where they sit on the hardware. */
    SimBackground* front_background;
    SimBackground* back_background;
    /* Magnification each panel ended up at, for the screenshot writer. */
    int front_scale;
    int back_scale;

    pthread_mutex_t lock;
    uint8_t front_pixels[FRONT_DISPLAY_W * FRONT_DISPLAY_H * 3];
    uint8_t back_pixels[BACK_DISPLAY_W * BACK_DISPLAY_H];
    bool front_dirty;
    bool back_dirty;
    bool front_blanked;

    bool quit_requested;

    /* Held state per InputKey, so the deck can light up the pressed control
     * whether it came from the keyboard or the mouse. */
    bool key_held[InputKeyMAX];
    uint8_t mouse_key;
    bool mouse_down;
    bool selftest_pending;
    bool screenshot_pending;

    /* Whole-window capture. The pixels can only be read on the SDL thread, but
     * the PNG encoder allocates from furi's heap and so has to run in a task;
     * these fields are the handshake between the two. */
    uint8_t* window_capture;
    int window_capture_w;
    int window_capture_h;
    bool window_capture_pending;
    bool window_capture_done;
    int canvas_w;
    int canvas_h;

    KeyEvent keys[KEY_QUEUE_SIZE];
    size_t key_head;
    size_t key_tail;
} sim;

/*
 * The FreeRTOS POSIX port preempts its task pthreads with SIGALRM. A task must
 * not be switched out while it owns a host pthread mutex: the next FreeRTOS
 * task can block on that mutex while the scheduler believes it is the running
 * task, leaving the owner parked indefinitely.
 *
 * This is not theoretical here. The macOS hang report caught the input task at
 * the return boundary of pthread_mutex_unlock(), suspended in the tick handler,
 * while the SDL and GUI threads both waited on this lock. Keep the tick masked
 * through the complete host mutex transaction. The previous mask is per
 * pthread so callers that already disabled the tick stay disabled afterwards.
 */
static _Thread_local sigset_t sim_window_previous_signal_mask;

static void sim_window_lock(void) {
    sigset_t tick_signal;
    sigemptyset(&tick_signal);
    sigaddset(&tick_signal, SIGALRM);

    int error =
        pthread_sigmask(SIG_BLOCK, &tick_signal, &sim_window_previous_signal_mask);
    if(error != 0) {
        fprintf(stderr, "[sim] pthread_sigmask(SIG_BLOCK) failed: %s\n", strerror(error));
        abort();
    }

    error = pthread_mutex_lock(&sim.lock);
    if(error != 0) {
        (void)pthread_sigmask(SIG_SETMASK, &sim_window_previous_signal_mask, NULL);
        fprintf(stderr, "[sim] pthread_mutex_lock failed: %s\n", strerror(error));
        abort();
    }
}

static void sim_window_unlock(void) {
    const int unlock_error = pthread_mutex_unlock(&sim.lock);
    const int mask_error =
        pthread_sigmask(SIG_SETMASK, &sim_window_previous_signal_mask, NULL);

    if(unlock_error != 0) {
        fprintf(stderr, "[sim] pthread_mutex_unlock failed: %s\n", strerror(unlock_error));
        abort();
    }
    if(mask_error != 0) {
        fprintf(stderr, "[sim] pthread_sigmask(SIG_SETMASK) failed: %s\n", strerror(mask_error));
        abort();
    }
}

/** Physical buttons the device has, mapped onto a keyboard. */
static uint8_t sim_window_map_key(SDL_Keycode code) {
    switch(code) {
    /* Swapped on purpose: see SIM_KEY_SCROLL_UP in input_host.h. */
    case SDLK_UP:
        return SIM_KEY_SCROLL_UP;
    case SDLK_DOWN:
        return SIM_KEY_SCROLL_DOWN;
    /* The device has no left or right button — see input_host_key_exists() —
     * so these are the dial too. Pickers that draw < > chevrons invite them. */
    case SDLK_LEFT:
        return SIM_KEY_SCROLL_UP;
    case SDLK_RIGHT:
        return SIM_KEY_SCROLL_DOWN;
    case SDLK_RETURN:
        return InputKeyOk;
    /* The wide pad on top of the device. */
    case SDLK_SPACE:
        return InputKeyStart;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE:
        return InputKeyBack;
    case SDLK_s:
        return InputKeyStart;
    case SDLK_b:
        return InputKeyBusy;
    case SDLK_c:
        return InputKeyCustom;
    case SDLK_o:
        return InputKeyOff;
    case SDLK_a:
        return InputKeyApps;
    case SDLK_COMMA:
        return InputKeySettings;
    default:
        return InputKeyMAX;
    }
}

static void sim_window_push_key(uint8_t key, bool pressed);

/** Record a button transition: queue it for the input service and remember
 * the held state so the deck can render the control as pressed. */
static void sim_window_set_key(uint8_t key, bool pressed) {
    if(key < InputKeyMAX) sim.key_held[key] = pressed;
    sim_window_push_key(key, pressed);
}

static void sim_window_push_key(uint8_t key, bool pressed) {
    const size_t next = (sim.key_head + 1) % KEY_QUEUE_SIZE;
    if(next == sim.key_tail) return; /* Full: drop, the UI is not keeping up. */

    sim.keys[sim.key_head] = (KeyEvent){.key = key, .pressed = pressed};
    sim.key_head = next;
}

/** Width the two cases are drawn at when the front matrix gets @p scale pixels
 * per LED. Both are scaled by their display face, not their bounding box: see
 * sim_background.h. */
static int sim_window_face_width_for(int scale) {
    return (int)((float)(scale * FRONT_DISPLAY_W) /
                 sim_background_display_ratio(sim.front_background));
}

/** Widest the cases get at @p face_width, which is not the face width: the
 * back render's side tabs stand a few percent proud of its panel. */
static int sim_window_case_width_for(int face_width) {
    const float front = sim_background_width_ratio(sim.front_background);
    const float back = sim_background_width_ratio(sim.back_background);

    return (int)((float)face_width * (front > back ? front : back) + 0.5f);
}

/** Height of the stacked cases at @p face_width, the gap between them included. */
static int sim_window_stack_height_for(int face_width) {
    const float ratio = sim_background_height_ratio(sim.front_background) +
                        sim_background_height_ratio(sim.back_background);

    return (int)((float)face_width * ratio + 0.5f) + GAP;
}

/** How tall the window has to be to draw the device at @p face_width. */
static int sim_window_height_for(int face_width, int width) {
    return sim_window_stack_height_for(face_width) + sim_controls_preferred_height(width) +
           3 * MARGIN;
}

/** Size the window so the front matrix gets @p scale pixels per LED, or as
 * close as the screen allows.
 *
 * The layout is recomputed from the actual canvas every frame, so this only
 * decides where the window starts; it can be resized freely afterwards.
 */
static void sim_window_resize_to_fit(int scale) {
    int face_width = sim_window_face_width_for(scale);
    int width = sim_window_case_width_for(face_width) + 2 * MARGIN;
    int height = sim_window_height_for(face_width, width);

    /* Shrink to fit rather than opening off the bottom of the screen. */
    SDL_Rect usable;
    if(SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        while(scale > 1 && (width > usable.w || height > usable.h)) {
            scale--;
            face_width = sim_window_face_width_for(scale);
            width = sim_window_case_width_for(face_width) + 2 * MARGIN;
            height = sim_window_height_for(face_width, width);
        }
    }

    SDL_SetWindowSize(sim.window, width, height);
    SDL_SetWindowPosition(sim.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

bool sim_window_init(int scale) {
    /* Replaced by the first layout pass; only matters if a screenshot beats
     * it, and the writer cannot magnify by zero. */
    sim.front_scale = 1;
    sim.back_scale = 1;
    pthread_mutex_init(&sim.lock, NULL);

    /* Must be set before the renderer exists; per-texture scale modes are
     * otherwise overridden and the magnified panels come out interpolated. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    if(SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    /* Opened hidden at a placeholder size: how big it wants to be depends on
     * the device renders, and those need a renderer, which needs a window.
     * sim_window_resize_to_fit() below settles it before anything is shown. */
    sim.window = SDL_CreateWindow(
        "BUSY Bar simulator",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        960,
        720,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if(!sim.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    sim.renderer = SDL_CreateRenderer(sim.window, -1, SDL_RENDERER_ACCELERATED);
    if(!sim.renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    /* LVGL's RGB888 is byte-order blue, green, red — see the static_asserts
     * at the top of gui.c — so the texture must be BGR, not RGB. */
    sim.front_texture = SDL_CreateTexture(
        sim.renderer,
        SDL_PIXELFORMAT_BGR24,
        SDL_TEXTUREACCESS_STREAMING,
        FRONT_DISPLAY_W,
        FRONT_DISPLAY_H);
    sim.back_texture = SDL_CreateTexture(
        sim.renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        BACK_DISPLAY_W,
        BACK_DISPLAY_H);

    if(!sim.front_texture || !sim.back_texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }

    /* The panels are physically discrete pixels; keep them crisp when scaled. */
    SDL_SetTextureScaleMode(sim.front_texture, SDL_ScaleModeNearest);
    SDL_SetTextureScaleMode(sim.back_texture, SDL_ScaleModeNearest);

    sim.front_leds = sim_led_panel_alloc(sim.renderer, FRONT_DISPLAY_W, FRONT_DISPLAY_H);
    if(!sim.front_leds) {
        fprintf(stderr, "sim_led_panel_alloc failed\n");
        return false;
    }

    sim.front_background = sim_background_alloc(sim.renderer, SimBackgroundSideFront);
    sim.back_background = sim_background_alloc(sim.renderer, SimBackgroundSideBack);
    if(!sim.front_background || !sim.back_background) {
        fprintf(stderr, "[sim] device renders are missing; re-run cmake to regenerate them\n");
        return false;
    }

    /* The render brings its own window around the matrix, so the panel must
     * not draw a second one. The spill rectangle is set per frame with the
     * layout. */
    sim_led_panel_set_glass(sim.front_leds, false, NULL);

    sim_window_resize_to_fit(scale > 0 ? scale : SIM_WINDOW_DEFAULT_SCALE);
    SDL_ShowWindow(sim.window);

    return true;
}

void sim_window_deinit(void) {
    sim_background_free(sim.front_background);
    sim_background_free(sim.back_background);
    sim_led_panel_free(sim.front_leds);
    if(sim.front_texture) SDL_DestroyTexture(sim.front_texture);
    if(sim.back_texture) SDL_DestroyTexture(sim.back_texture);
    if(sim.renderer) SDL_DestroyRenderer(sim.renderer);
    if(sim.window) SDL_DestroyWindow(sim.window);
    SDL_Quit();
}

void sim_window_submit_front(const uint8_t* pixels) {
    sim_window_lock();
    memcpy(sim.front_pixels, pixels, sizeof(sim.front_pixels));
    sim.front_dirty = true;
    sim_window_unlock();
}

void sim_window_submit_back(const uint8_t* pixels) {
    sim_window_lock();
    memcpy(sim.back_pixels, pixels, sizeof(sim.back_pixels));
    sim.back_dirty = true;
    sim_window_unlock();
}

void sim_window_set_front_blanked(bool blanked) {
    sim_window_lock();
    sim.front_blanked = blanked;
    sim.front_dirty = true;
    sim_window_unlock();
}

bool sim_window_poll_key(uint8_t* key, bool* pressed) {
    bool available = false;

    sim_window_lock();
    if(sim.key_tail != sim.key_head) {
        const KeyEvent event = sim.keys[sim.key_tail];
        sim.key_tail = (sim.key_tail + 1) % KEY_QUEUE_SIZE;
        *key = event.key;
        *pressed = event.pressed;
        available = true;
    }
    sim_window_unlock();

    return available;
}

static void sim_window_upload(void) {
    uint8_t front_rgb[sizeof(sim.front_pixels)];
    uint8_t back_rgb[BACK_DISPLAY_W * BACK_DISPLAY_H * 3];
    bool front_dirty, back_dirty;

    sim_window_lock();
    front_dirty = sim.front_dirty;
    back_dirty = sim.back_dirty;

    if(front_dirty) {
        if(sim.front_blanked) {
            memset(front_rgb, 0, sizeof(front_rgb));
        } else {
            memcpy(front_rgb, sim.front_pixels, sizeof(front_rgb));
        }
        sim.front_dirty = false;
    }

    if(back_dirty) {
        /* The back panel is 8bpp greyscale; expand to RGB for the texture. */
        for(size_t i = 0; i < sizeof(sim.back_pixels); i++) {
            const uint8_t level = sim.back_pixels[i];
            back_rgb[i * 3 + 0] = level;
            back_rgb[i * 3 + 1] = level;
            back_rgb[i * 3 + 2] = level;
        }
        sim.back_dirty = false;
    }
    sim_window_unlock();

    if(front_dirty) {
        SDL_UpdateTexture(sim.front_texture, NULL, front_rgb, FRONT_DISPLAY_W * 3);
    }
    if(back_dirty) {
        SDL_UpdateTexture(sim.back_texture, NULL, back_rgb, BACK_DISPLAY_W * 3);
    }
}

/** Place the device in the current canvas: front above, back below.
 *
 * Both cases are drawn at one display-face width, so the device is the same
 * size top and bottom, and each display is then placed by its render rather
 * than by a scale factor of its own. The panels end up at whatever
 * magnification that works out to — usually fractional, which is what the
 * hardware looks like: neither panel's pixels are square multiples of the
 * screen's.
 */
static void sim_window_layout(SimLayout* layout) {
    int canvas_w = 0, canvas_h = 0;
    SDL_GetRendererOutputSize(sim.renderer, &canvas_w, &canvas_h);

    sim_window_lock();
    sim.canvas_w = canvas_w;
    sim.canvas_h = canvas_h;
    sim_window_unlock();

    const int deck_h = sim_controls_preferred_height(canvas_w);

    layout->deck = (SDL_Rect){
        .x = MARGIN,
        .y = canvas_h - deck_h - MARGIN,
        .w = canvas_w - 2 * MARGIN,
        .h = deck_h,
    };

    const int available_w = canvas_w - 2 * MARGIN;
    const int available_h = layout->deck.y - 2 * MARGIN;

    /* The face width the canvas can carry, whichever constraint bites first. */
    const float width_ratio_front = sim_background_width_ratio(sim.front_background);
    const float width_ratio_back = sim_background_width_ratio(sim.back_background);
    const float width_ratio =
        width_ratio_front > width_ratio_back ? width_ratio_front : width_ratio_back;
    const float height_ratio = sim_background_height_ratio(sim.front_background) +
                               sim_background_height_ratio(sim.back_background);

    const int from_width = (int)((float)available_w / width_ratio);
    const int from_height = (int)((float)(available_h - GAP) / height_ratio);

    int face_width = from_width < from_height ? from_width : from_height;
    if(face_width < 1) face_width = 1;

    const int centre_x = canvas_w / 2;
    layout->front_case = sim_background_case_rect(sim.front_background, face_width, centre_x, 0);
    layout->back_case = sim_background_case_rect(sim.back_background, face_width, centre_x, 0);

    const int block_h = layout->front_case.h + GAP + layout->back_case.h;
    layout->front_case.y = MARGIN + (available_h - block_h) / 2;
    layout->back_case.y = layout->front_case.y + layout->front_case.h + GAP;

    layout->front_display =
        sim_background_display_rect(sim.front_background, &layout->front_case);
    layout->back_display = sim_background_display_rect(sim.back_background, &layout->back_case);
    layout->front_window = sim_background_window_rect(sim.front_background, &layout->front_case);

    /* Only the screenshot writer reads these, and it needs whole pixels. */
    sim.front_scale = layout->front_display.w / FRONT_DISPLAY_W;
    sim.back_scale = layout->back_display.w / BACK_DISPLAY_W;
    if(sim.front_scale < 1) sim.front_scale = 1;
    if(sim.back_scale < 1) sim.back_scale = 1;
}

bool sim_window_pump(void) {
    SDL_Event event;

    sim_window_lock();
    const bool quit = sim.quit_requested;
    sim_window_unlock();
    if(quit) return false;

    while(SDL_PollEvent(&event)) {
        if(event.type == SDL_QUIT) return false;

        if(event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            if(event.key.repeat) continue;

            if(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_q &&
               (event.key.keysym.mod & KMOD_GUI)) {
                return false;
            }

            /* Not a device button, so it is handled here rather than mapped. */
            if(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F12) {
                sim_window_request_screenshot();
                continue;
            }

            const uint8_t key = sim_window_map_key(event.key.keysym.sym);
            if(key != InputKeyMAX) {
                sim_window_lock();
                sim_window_set_key(key, event.type == SDL_KEYDOWN);
                sim_window_unlock();
            }

        } else if(event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
            uint8_t key;
            /* Mouse coordinates are in window space; the renderer may be
             * scaled on a HiDPI display, so map them onto the canvas. */
            int window_w = 0, window_h = 0, canvas_w = 0, canvas_h = 0;
            SDL_GetWindowSize(sim.window, &window_w, &window_h);
            SDL_GetRendererOutputSize(sim.renderer, &canvas_w, &canvas_h);

            const int x = window_w ? event.button.x * canvas_w / window_w : event.button.x;
            const int y = window_h ? event.button.y * canvas_h / window_h : event.button.y;

            if(sim_controls_hit(x, y, &key)) {
                sim_window_lock();
                sim.mouse_key = key;
                sim.mouse_down = true;
                sim_window_set_key(key, true);
                sim_window_unlock();
            }

        } else if(event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            sim_window_lock();
            if(sim.mouse_down) {
                sim_window_set_key(sim.mouse_key, false);
                sim.mouse_down = false;
            }
            sim_window_unlock();

        } else if(event.type == SDL_MOUSEWHEEL) {
            /* The dial is an encoder; a wheel notch is one scroll step. */
            const uint8_t key = event.wheel.y > 0 ? SIM_KEY_SCROLL_UP : SIM_KEY_SCROLL_DOWN;
            if(event.wheel.y != 0) {
                sim_window_lock();
                sim_window_push_key(key, true);
                sim_window_push_key(key, false);
                sim_window_unlock();
            }
        }
    }

    sim_window_upload();

    SimLayout layout;
    sim_window_layout(&layout);
    sim_controls_layout(&layout.deck);

    if(sim.selftest_pending) {
        sim.selftest_pending = false;
        sim_controls_selftest();
    }

    /* The renders sit on black, so the window has to as well. */
    SDL_SetRenderDrawColor(sim.renderer, 0, 0, 0, 255);
    SDL_RenderClear(sim.renderer);

    sim_background_render(sim.front_background, &layout.front_case);
    sim_background_render(sim.back_background, &layout.back_case);

    sim_led_panel_set_glass(sim.front_leds, false, &layout.front_window);
    sim_led_panel_render(sim.front_leds, sim.front_texture, &layout.front_display);
    SDL_RenderCopy(sim.renderer, sim.back_texture, NULL, &layout.back_display);

    sim_window_lock();
    bool held[InputKeyMAX];
    memcpy(held, sim.key_held, sizeof(held));
    sim_window_unlock();

    sim_controls_render(sim.renderer, held);
    /* Read back before presenting: after the swap the target is undefined. */
    sim_window_lock();
    if(sim.window_capture_pending && sim.window_capture) {
        const SDL_Rect area = {0, 0, sim.window_capture_w, sim.window_capture_h};
        SDL_RenderReadPixels(
            sim.renderer,
            &area,
            SDL_PIXELFORMAT_RGB24,
            sim.window_capture,
            sim.window_capture_w * 3);
        sim.window_capture_pending = false;
        sim.window_capture_done = true;
    }
    sim_window_unlock();

    /* A recording reads the same finished frame back, into buffers the
     * recorder allocated when it started: nothing here allocates, and a frame
     * the writer has no room for is dropped rather than stalling the loop. */
    uint8_t* recording = sim_recorder_frame_due(sim.canvas_w, sim.canvas_h, SDL_GetTicks64());
    if(recording) {
        const SDL_Rect area = {0, 0, sim.canvas_w, sim.canvas_h};
        SDL_RenderReadPixels(
            sim.renderer, &area, SDL_PIXELFORMAT_RGB24, recording, sim.canvas_w * 3);
        sim_recorder_frame_ready();
    }

    SDL_RenderPresent(sim.renderer);

    return true;
}

/** Blow a framebuffer up by the window's scale so the PNG is legible.
 *
 * Nearest-neighbour on purpose: these are 72x16 and 160x80 panels and the
 * point of a screenshot is to inspect individual pixels.
 */
static void sim_window_write_png(
    const char* path,
    const uint8_t* rgb,
    size_t width,
    size_t height,
    int scale) {
    uint8_t* scaled = NULL;

    if(scale > 1) {
        scaled = malloc(width * height * scale * scale * 3);
        if(!scaled) return;

        for(size_t y = 0; y < height * (size_t)scale; y++) {
            for(size_t x = 0; x < width * (size_t)scale; x++) {
                const size_t src = ((y / scale) * width + (x / scale)) * 3;
                const size_t dst = (y * width * scale + x) * 3;
                scaled[dst + 0] = rgb[src + 0];
                scaled[dst + 1] = rgb[src + 1];
                scaled[dst + 2] = rgb[src + 2];
            }
        }
    }

    /* Tuned for capture rate rather than file size. A whole-window shot is
     * four megapixels; at lodepng's defaults — every scanline filtered five
     * ways and a 2048-byte match window — that is five seconds an image, and
     * the point of on-demand capture is to take them in a loop. Filtering up
     * and a short window costs about a third more bytes on disk. */
    LodePNGState state;
    lodepng_state_init(&state);
    state.info_raw.colortype = LCT_RGB;
    state.info_raw.bitdepth = 8;
    state.info_png.color.colortype = LCT_RGB;
    state.info_png.color.bitdepth = 8;
    state.encoder.auto_convert = 0;
    state.encoder.filter_strategy = LFS_TWO;
    state.encoder.zlibsettings.windowsize = 256;

    /* LVGL's lodepng fork routes *_file() through lv_fs, so encode to memory
     * and write the bytes here instead. */
    uint8_t* png = NULL;
    size_t png_size = 0;
    const unsigned error = lodepng_encode(
        &png,
        &png_size,
        scaled ? scaled : rgb,
        (unsigned)(width * scale),
        (unsigned)(height * scale),
        &state);

    lodepng_state_cleanup(&state);

    if(error) {
        fprintf(stderr, "[sim] png encode failed for %s: %u\n", path, error);
    } else {
        FILE* file = fopen(path, "wb");
        if(file) {
            fwrite(png, 1, png_size, file);
            fclose(file);
        } else {
            fprintf(stderr, "[sim] could not open %s for writing\n", path);
        }
    }

    free(png);
    free(scaled);
}

void sim_window_request_selftest(void) {
    sim.selftest_pending = true;
}

void sim_window_request_screenshot(void) {
    sim_window_lock();
    sim.screenshot_pending = true;
    sim_window_unlock();
}

bool sim_window_take_screenshot_request(void) {
    sim_window_lock();
    const bool pending = sim.screenshot_pending;
    sim.screenshot_pending = false;
    sim_window_unlock();

    return pending;
}

void sim_window_inject_key(uint8_t key, bool pressed) {
    sim_window_lock();
    sim_window_set_key(key, pressed);
    sim_window_unlock();
}

void sim_window_request_quit(void) {
    sim_window_lock();
    sim.quit_requested = true;
    sim_window_unlock();
}

void sim_window_canvas_size(int* width, int* height) {
    sim_window_lock();
    *width = sim.canvas_w;
    *height = sim.canvas_h;
    sim_window_unlock();
}

/** Build "<directory>/<name>.png", numbered when @p sequence is not zero. */
static void sim_window_capture_path(
    char* path,
    size_t size,
    const char* directory,
    const char* name,
    unsigned sequence) {
    if(sequence) {
        snprintf(path, size, "%s/%s-%03u.png", directory, name, sequence);
    } else {
        snprintf(path, size, "%s/%s.png", directory, name);
    }
}

/** Ask the SDL thread for the composited window and write it out.
 *
 * Unlike the per-panel images this shows the device and the layout around the
 * panels, which is the only way to check the window presentation. */
static void sim_window_capture_window(const char* directory, unsigned sequence) {
    sim_window_lock();
    const int width = sim.canvas_w;
    const int height = sim.canvas_h;
    sim_window_unlock();

    if(width <= 0 || height <= 0) return;

    uint8_t* pixels = malloc((size_t)width * height * 3);
    if(!pixels) return;

    sim_window_lock();
    sim.window_capture = pixels;
    sim.window_capture_w = width;
    sim.window_capture_h = height;
    sim.window_capture_done = false;
    sim.window_capture_pending = true;
    sim_window_unlock();

    /* The SDL thread fills this on its next present. */
    bool done = false;
    for(int attempt = 0; attempt < 100 && !done; attempt++) {
        furi_delay_ms(20);
        sim_window_lock();
        done = sim.window_capture_done;
        sim_window_unlock();
    }

    if(done) {
        char path[1024];
        sim_window_capture_path(path, sizeof(path), directory, "window", sequence);
        sim_window_write_png(path, pixels, (size_t)width, (size_t)height, 1);
    } else {
        fprintf(stderr, "[sim] window capture timed out\n");
    }

    sim_window_lock();
    sim.window_capture = NULL;
    sim.window_capture_pending = false;
    sim_window_unlock();

    free(pixels);
}

void sim_window_screenshot(const char* directory, unsigned sequence) {
    uint8_t front_rgb[sizeof(sim.front_pixels)];
    uint8_t back_rgb[BACK_DISPLAY_W * BACK_DISPLAY_H * 3];
    int front_scale;
    int back_scale;

    sim_window_lock();
    /* Swap to RGB for the encoder; the panel buffer is BGR. */
    for(size_t i = 0; i < sizeof(front_rgb); i += 3) {
        front_rgb[i + 0] = sim.front_pixels[i + 2];
        front_rgb[i + 1] = sim.front_pixels[i + 1];
        front_rgb[i + 2] = sim.front_pixels[i + 0];
    }
    for(size_t i = 0; i < sizeof(sim.back_pixels); i++) {
        const uint8_t level = sim.back_pixels[i];
        back_rgb[i * 3 + 0] = level;
        back_rgb[i * 3 + 1] = level;
        back_rgb[i * 3 + 2] = level;
    }
    front_scale = sim.front_scale;
    back_scale = sim.back_scale;
    sim_window_unlock();

    char path[1024];

    sim_window_capture_path(path, sizeof(path), directory, "front", sequence);
    sim_window_write_png(path, front_rgb, FRONT_DISPLAY_W, FRONT_DISPLAY_H, front_scale);

    sim_window_capture_path(path, sizeof(path), directory, "back", sequence);
    sim_window_write_png(path, back_rgb, BACK_DISPLAY_W, BACK_DISPLAY_H, back_scale);

    sim_window_capture_window(directory, sequence);

    sim_window_capture_path(path, sizeof(path), directory, "window", sequence);
    fprintf(stderr, "[sim] wrote %s and its two panels\n", path);
}
