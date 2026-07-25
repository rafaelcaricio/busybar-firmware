#include "sim_window.h"
#include "input_host.h"
#include "sim_controls.h"
#include "sim_led_panel.h"

#include <front_display/front_display.h>
#include <back_display/back_display.h>
#include <furi_hal_resources.h>

#include <SDL.h>
#include <libs/lodepng/lodepng.h>

#include <furi.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARGIN         (20)
#define GAP            (28)
#define BEZEL          (4)
#define KEY_QUEUE_SIZE (64)

/* Magnification to open at when none is asked for. The panels are 72x16 and
 * 160x80: at anything less than this the LEDs are too small to read and the
 * window is a postage stamp on a desktop display. Trimmed at startup if the
 * screen cannot take it. */
#define SIM_WINDOW_DEFAULT_SCALE (10)

typedef struct {
    uint8_t key;
    bool pressed;
} KeyEvent;

static struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* front_texture;
    SDL_Texture* back_texture;
    /* The front panel is an LED matrix and is drawn as one; the back panel is
     * a greyscale display and is drawn as it is. */
    SimLedPanel* front_leds;
    /* Separate factors: the front bar is magnified harder than the back panel. */
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

/** How tall the window has to be to give both panels @p scale. */
static int sim_window_height_for(int scale, int width) {
    return (BACK_DISPLAY_H + FRONT_DISPLAY_H) * scale + 3 * MARGIN + 2 * BEZEL + GAP +
           sim_controls_preferred_height(width) + MARGIN;
}

bool sim_window_init(int scale) {
    int initial = scale > 0 ? scale : SIM_WINDOW_DEFAULT_SCALE;
    sim.front_scale = initial;
    sim.back_scale = initial;
    pthread_mutex_init(&sim.lock, NULL);

    /* Must be set before the renderer exists; per-texture scale modes are
     * otherwise overridden and the magnified panels come out interpolated. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    if(SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    /* Open at the requested scale, but the layout is recomputed from the
     * actual canvas every frame, so the window can be resized freely.
     *
     * The height has to carry the control deck as well as the two panels, or
     * the layout shrinks them to fit and the window opens smaller than asked.
     */
    int width = BACK_DISPLAY_W * initial + 2 * MARGIN;
    int height = sim_window_height_for(initial, width);

    /* Shrink to fit rather than opening off the bottom of the screen. */
    SDL_Rect usable;
    if(SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        while(initial > 1 && (width > usable.w || height > usable.h)) {
            initial--;
            width = BACK_DISPLAY_W * initial + 2 * MARGIN;
            height = sim_window_height_for(initial, width);
        }
    }

    sim.window = SDL_CreateWindow(
        "BUSY Bar simulator",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
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

    return true;
}

void sim_window_deinit(void) {
    sim_led_panel_free(sim.front_leds);
    if(sim.front_texture) SDL_DestroyTexture(sim.front_texture);
    if(sim.back_texture) SDL_DestroyTexture(sim.back_texture);
    if(sim.renderer) SDL_DestroyRenderer(sim.renderer);
    if(sim.window) SDL_DestroyWindow(sim.window);
    SDL_Quit();
}

void sim_window_submit_front(const uint8_t* pixels) {
    pthread_mutex_lock(&sim.lock);
    memcpy(sim.front_pixels, pixels, sizeof(sim.front_pixels));
    sim.front_dirty = true;
    pthread_mutex_unlock(&sim.lock);
}

void sim_window_submit_back(const uint8_t* pixels) {
    pthread_mutex_lock(&sim.lock);
    memcpy(sim.back_pixels, pixels, sizeof(sim.back_pixels));
    sim.back_dirty = true;
    pthread_mutex_unlock(&sim.lock);
}

void sim_window_set_front_blanked(bool blanked) {
    pthread_mutex_lock(&sim.lock);
    sim.front_blanked = blanked;
    sim.front_dirty = true;
    pthread_mutex_unlock(&sim.lock);
}

bool sim_window_poll_key(uint8_t* key, bool* pressed) {
    bool available = false;

    pthread_mutex_lock(&sim.lock);
    if(sim.key_tail != sim.key_head) {
        const KeyEvent event = sim.keys[sim.key_tail];
        sim.key_tail = (sim.key_tail + 1) % KEY_QUEUE_SIZE;
        *key = event.key;
        *pressed = event.pressed;
        available = true;
    }
    pthread_mutex_unlock(&sim.lock);

    return available;
}

static void sim_window_upload(void) {
    uint8_t front_rgb[sizeof(sim.front_pixels)];
    uint8_t back_rgb[BACK_DISPLAY_W * BACK_DISPLAY_H * 3];
    bool front_dirty, back_dirty;

    pthread_mutex_lock(&sim.lock);
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
    pthread_mutex_unlock(&sim.lock);

    if(front_dirty) {
        SDL_UpdateTexture(sim.front_texture, NULL, front_rgb, FRONT_DISPLAY_W * 3);
    }
    if(back_dirty) {
        SDL_UpdateTexture(sim.back_texture, NULL, back_rgb, BACK_DISPLAY_W * 3);
    }
}

/** Place both panels in the current canvas.
 *
 * The front LED bar is the device's headline display, so it gets the full
 * width and the back panel takes what is left underneath. That means two
 * different scale factors rather than one shared one — their on-screen sizes
 * no longer reflect their physical sizes, which is the point: the front panel
 * is only 16 rows tall and needs the magnification to be readable.
 *
 * Both scales stay integers. A fractional one makes some source pixels a row
 * wider than their neighbours, which reads as distortion in exactly the
 * layouts this tool exists to check.
 */
static void sim_window_layout(SDL_Rect* front, SDL_Rect* back, SDL_Rect* deck) {
    int canvas_w = 0, canvas_h = 0;
    SDL_GetRendererOutputSize(sim.renderer, &canvas_w, &canvas_h);

    pthread_mutex_lock(&sim.lock);
    sim.canvas_w = canvas_w;
    sim.canvas_h = canvas_h;
    pthread_mutex_unlock(&sim.lock);

    const int deck_h = sim_controls_preferred_height(canvas_w);

    *deck = (SDL_Rect){
        .x = MARGIN,
        .y = canvas_h - deck_h - MARGIN,
        .w = canvas_w - 2 * MARGIN,
        .h = deck_h,
    };

    const int available_w = canvas_w - 2 * (BEZEL + MARGIN);
    const int available_h =
        canvas_h - deck_h - MARGIN - 2 * (BEZEL + MARGIN) - GAP - 2 * BEZEL;

    int front_scale = available_w / FRONT_DISPLAY_W;
    if(front_scale < 1) front_scale = 1;

    /* Leave the back panel at least a third of the height to work with. */
    int front_h = FRONT_DISPLAY_H * front_scale;
    while(front_scale > 1 && front_h > (available_h * 2) / 3) {
        front_scale--;
        front_h = FRONT_DISPLAY_H * front_scale;
    }

    const int remaining_h = available_h - front_h;
    int back_scale = available_w / BACK_DISPLAY_W;
    if(remaining_h / BACK_DISPLAY_H < back_scale) back_scale = remaining_h / BACK_DISPLAY_H;
    if(back_scale < 1) back_scale = 1;

    sim.front_scale = front_scale;
    sim.back_scale = back_scale;

    const int front_w = FRONT_DISPLAY_W * front_scale;
    const int back_w = BACK_DISPLAY_W * back_scale;
    const int back_h = BACK_DISPLAY_H * back_scale;

    const int block_h = front_h + back_h + GAP + 2 * BEZEL;
    const int panel_area_h = canvas_h - deck_h - MARGIN;
    const int top = (panel_area_h - block_h) / 2 + BEZEL;

    *front = (SDL_Rect){
        .x = (canvas_w - front_w) / 2,
        .y = top,
        .w = front_w,
        .h = front_h,
    };
    *back = (SDL_Rect){
        .x = (canvas_w - back_w) / 2,
        .y = top + front_h + GAP + 2 * BEZEL,
        .w = back_w,
        .h = back_h,
    };
}

/** A thin surround so each panel reads as a separate physical display. */
static void sim_window_draw_bezel(const SDL_Rect* panel) {
    for(int inset = 1; inset <= BEZEL; inset++) {
        /* Fade the frame outwards, brightest against the panel edge. */
        const uint8_t level = (uint8_t)(72 - (inset - 1) * (48 / BEZEL));
        SDL_SetRenderDrawColor(sim.renderer, level, level, level + 6, 255);

        const SDL_Rect edge = {
            .x = panel->x - inset,
            .y = panel->y - inset,
            .w = panel->w + 2 * inset,
            .h = panel->h + 2 * inset,
        };
        SDL_RenderDrawRect(sim.renderer, &edge);
    }
}

bool sim_window_pump(void) {
    SDL_Event event;

    pthread_mutex_lock(&sim.lock);
    const bool quit = sim.quit_requested;
    pthread_mutex_unlock(&sim.lock);
    if(quit) return false;

    while(SDL_PollEvent(&event)) {
        if(event.type == SDL_QUIT) return false;

        if(event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            if(event.key.repeat) continue;

            if(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_q &&
               (event.key.keysym.mod & KMOD_GUI)) {
                return false;
            }

            const uint8_t key = sim_window_map_key(event.key.keysym.sym);
            if(key != InputKeyMAX) {
                pthread_mutex_lock(&sim.lock);
                sim_window_set_key(key, event.type == SDL_KEYDOWN);
                pthread_mutex_unlock(&sim.lock);
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
                pthread_mutex_lock(&sim.lock);
                sim.mouse_key = key;
                sim.mouse_down = true;
                sim_window_set_key(key, true);
                pthread_mutex_unlock(&sim.lock);
            }

        } else if(event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) {
            pthread_mutex_lock(&sim.lock);
            if(sim.mouse_down) {
                sim_window_set_key(sim.mouse_key, false);
                sim.mouse_down = false;
            }
            pthread_mutex_unlock(&sim.lock);

        } else if(event.type == SDL_MOUSEWHEEL) {
            /* The dial is an encoder; a wheel notch is one scroll step. */
            const uint8_t key = event.wheel.y > 0 ? SIM_KEY_SCROLL_UP : SIM_KEY_SCROLL_DOWN;
            if(event.wheel.y != 0) {
                pthread_mutex_lock(&sim.lock);
                sim_window_push_key(key, true);
                sim_window_push_key(key, false);
                pthread_mutex_unlock(&sim.lock);
            }
        }
    }

    sim_window_upload();

    SDL_Rect front_rect, back_rect, deck_rect;
    sim_window_layout(&front_rect, &back_rect, &deck_rect);
    sim_controls_layout(&deck_rect);

    if(sim.selftest_pending) {
        sim.selftest_pending = false;
        sim_controls_selftest();
    }

    SDL_SetRenderDrawColor(sim.renderer, 18, 18, 22, 255);
    SDL_RenderClear(sim.renderer);

    /* Only the back panel gets a drawn bezel: the front one brings its own,
     * the rounded window the LEDs sit behind. */
    sim_window_draw_bezel(&back_rect);

    sim_led_panel_render(sim.front_leds, sim.front_texture, &front_rect);
    SDL_RenderCopy(sim.renderer, sim.back_texture, NULL, &back_rect);

    pthread_mutex_lock(&sim.lock);
    bool held[InputKeyMAX];
    memcpy(held, sim.key_held, sizeof(held));
    pthread_mutex_unlock(&sim.lock);

    sim_controls_render(sim.renderer, held);
    /* Read back before presenting: after the swap the target is undefined. */
    pthread_mutex_lock(&sim.lock);
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
    pthread_mutex_unlock(&sim.lock);

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
    uint8_t* scaled = malloc(width * height * scale * scale * 3);
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

    /* LVGL's lodepng fork routes *_file() through lv_fs, so encode to memory
     * and write the bytes here instead. */
    uint8_t* png = NULL;
    size_t png_size = 0;
    const unsigned error = lodepng_encode24(
        &png, &png_size, scaled, (unsigned)(width * scale), (unsigned)(height * scale));

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

void sim_window_inject_key(uint8_t key, bool pressed) {
    pthread_mutex_lock(&sim.lock);
    sim_window_set_key(key, pressed);
    pthread_mutex_unlock(&sim.lock);
}

void sim_window_request_quit(void) {
    pthread_mutex_lock(&sim.lock);
    sim.quit_requested = true;
    pthread_mutex_unlock(&sim.lock);
}

/** Ask the SDL thread for the composited window and write it out.
 *
 * Unlike the per-panel images this shows the layout itself — bezels, relative
 * sizes, spacing — which is the only way to check the window presentation. */
static void sim_window_capture_window(const char* directory) {
    pthread_mutex_lock(&sim.lock);
    const int width = sim.canvas_w;
    const int height = sim.canvas_h;
    pthread_mutex_unlock(&sim.lock);

    if(width <= 0 || height <= 0) return;

    uint8_t* pixels = malloc((size_t)width * height * 3);
    if(!pixels) return;

    pthread_mutex_lock(&sim.lock);
    sim.window_capture = pixels;
    sim.window_capture_w = width;
    sim.window_capture_h = height;
    sim.window_capture_done = false;
    sim.window_capture_pending = true;
    pthread_mutex_unlock(&sim.lock);

    /* The SDL thread fills this on its next present. */
    bool done = false;
    for(int attempt = 0; attempt < 100 && !done; attempt++) {
        furi_delay_ms(20);
        pthread_mutex_lock(&sim.lock);
        done = sim.window_capture_done;
        pthread_mutex_unlock(&sim.lock);
    }

    if(done) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/window.png", directory);
        sim_window_write_png(path, pixels, (size_t)width, (size_t)height, 1);
    } else {
        fprintf(stderr, "[sim] window capture timed out\n");
    }

    pthread_mutex_lock(&sim.lock);
    sim.window_capture = NULL;
    sim.window_capture_pending = false;
    pthread_mutex_unlock(&sim.lock);

    free(pixels);
}

void sim_window_screenshot(const char* directory) {
    uint8_t front_rgb[sizeof(sim.front_pixels)];
    uint8_t back_rgb[BACK_DISPLAY_W * BACK_DISPLAY_H * 3];

    pthread_mutex_lock(&sim.lock);
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
    pthread_mutex_unlock(&sim.lock);

    char path[1024];

    snprintf(path, sizeof(path), "%s/front.png", directory);
    sim_window_write_png(path, front_rgb, FRONT_DISPLAY_W, FRONT_DISPLAY_H, sim.front_scale);

    snprintf(path, sizeof(path), "%s/back.png", directory);
    sim_window_write_png(path, back_rgb, BACK_DISPLAY_W, BACK_DISPLAY_H, sim.back_scale);

    sim_window_capture_window(directory);

    fprintf(stderr, "[sim] wrote screenshots to %s\n", directory);
}
