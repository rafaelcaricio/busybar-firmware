/**
 * LED matrix rendering for the front display.
 *
 * Four passes, cheap enough to run every frame at any window size:
 *
 *   substrate   the dark diffuser the LEDs sit behind.
 *   dark dots   every LED, lit or not, in the colour an unlit one reflects.
 *               This is what makes the panel read as hardware while it is off.
 *   lit LEDs    the framebuffer, carved into rounded squares by a mask.
 *   haze        the framebuffer again, smoothly magnified and added over the
 *               top twice, so light spills into the gaps and past the edges.
 *
 * The mask is one texture covering the whole panel rather than one draw per
 * LED: at 72x16 that is the difference between four draw calls a frame and
 * over a thousand. It is rebuilt only when the panel is resized.
 */
#include "sim_led_panel.h"

#include <math.h>
#include <stdlib.h>

/* Fractions of the LED pitch. The device's LEDs very nearly touch, so the gap
 * is thin; widen it and the panel starts to look like a scoreboard. */
#define LED_GAP_RATIO    (0.16f)
#define LED_CORNER_RATIO (0.30f)
#define LED_MIN_PITCH    (4.0f)

/* An unlit LED is not black: it is a grey lens over a dark backing. */
#define LED_OFF_R (18)
#define LED_OFF_G (18)
#define LED_OFF_B (22)

#define SUBSTRATE_R (7)
#define SUBSTRATE_G (7)
#define SUBSTRATE_B (9)

/* Spread in LED pitches, alpha out of 255. The tight pass fills the gaps
 * between neighbours, the wide one is the bloom around the whole glyph. */
#define HAZE_NEAR_SPREAD (0.45f)
#define HAZE_NEAR_ALPHA  (88)
#define HAZE_FAR_SPREAD  (1.25f)
#define HAZE_FAR_ALPHA   (26)

/* The matrix does not reach the edge of the device: it sits behind a dark
 * rounded window with a little clearance around it. Both in LED pitches. */
#define GLASS_MARGIN_RATIO (0.9f)
#define GLASS_CORNER_RATIO (1.6f)
#define GLASS_RIM_R        (46)
#define GLASS_RIM_G        (46)
#define GLASS_RIM_B        (52)

struct SimLedPanel {
    SDL_Renderer* renderer;
    int columns;
    int rows;

    SDL_Texture* mask;
    SDL_Texture* lit;
    SDL_Texture* glass;
    SDL_Rect glass_area;
    int width;
    int height;
};

/** Signed distance to a rounded rectangle centred on the origin. */
static float sim_led_panel_distance(
    float offset_x,
    float offset_y,
    float half_w,
    float half_h,
    float corner) {
    float dx = fabsf(offset_x) - (half_w - corner);
    float dy = fabsf(offset_y) - (half_h - corner);
    if(dx < 0.0f) dx = 0.0f;
    if(dy < 0.0f) dy = 0.0f;

    return sqrtf(dx * dx + dy * dy) - corner;
}

/** A distance turned into coverage, antialiased over the last pixel. */
static float sim_led_panel_coverage(float distance) {
    const float coverage = 0.5f - distance;

    if(coverage <= 0.0f) return 0.0f;
    if(coverage >= 1.0f) return 1.0f;
    return coverage;
}

static float sim_led_panel_led_coverage(float offset_x, float offset_y, float pitch) {
    /* In a small enough window there is no room to draw a dot: the gap and the
     * rounding would eat the whole LED, so fill the cell instead. */
    if(pitch < LED_MIN_PITCH) return 1.0f;

    const float half = pitch * 0.5f - pitch * LED_GAP_RATIO;

    return sim_led_panel_coverage(
        sim_led_panel_distance(offset_x, offset_y, half, half, half * LED_CORNER_RATIO));
}

/** One LED per cell, white on black.
 *
 * Both channels carry the coverage: the alpha draws the unlit dots, and the
 * colour multiplies the lit ones down to the same shape.
 */
static SDL_Texture*
    sim_led_panel_build_mask(SDL_Renderer* renderer, int width, int height, int columns, int rows) {
    uint32_t* pixels = malloc((size_t)width * height * sizeof(uint32_t));
    if(!pixels) return NULL;

    const float pitch_x = (float)width / (float)columns;
    const float pitch_y = (float)height / (float)rows;

    for(int y = 0; y < height; y++) {
        const int row = (int)((float)y / pitch_y);
        const float offset_y = (float)y + 0.5f - ((float)row + 0.5f) * pitch_y;

        for(int x = 0; x < width; x++) {
            const int column = (int)((float)x / pitch_x);
            const float offset_x = (float)x + 0.5f - ((float)column + 0.5f) * pitch_x;

            const float coverage = sim_led_panel_led_coverage(
                offset_x, offset_y, pitch_x < pitch_y ? pitch_x : pitch_y);
            const uint32_t level = (uint32_t)(coverage * 255.0f + 0.5f);

            pixels[(size_t)y * width + x] =
                (level << 24) | (level << 16) | (level << 8) | level;
        }
    }

    SDL_Texture* mask = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, width, height);

    if(mask) {
        SDL_UpdateTexture(mask, NULL, pixels, width * (int)sizeof(uint32_t));
        SDL_SetTextureScaleMode(mask, SDL_ScaleModeNearest);
    }

    free(pixels);
    return mask;
}

/** The dark window the matrix sits behind, with a rim where it meets the case. */
static SDL_Texture* sim_led_panel_build_glass(
    SDL_Renderer* renderer,
    int width,
    int height,
    float corner) {
    uint32_t* pixels = malloc((size_t)width * height * sizeof(uint32_t));
    if(!pixels) return NULL;

    const float half_w = (float)width * 0.5f;
    const float half_h = (float)height * 0.5f;

    for(int y = 0; y < height; y++) {
        const float offset_y = (float)y + 0.5f - half_h;

        for(int x = 0; x < width; x++) {
            const float offset_x = (float)x + 0.5f - half_w;
            const float distance =
                sim_led_panel_distance(offset_x, offset_y, half_w, half_h, corner);

            const uint32_t alpha = (uint32_t)(sim_led_panel_coverage(distance) * 255.0f + 0.5f);
            /* The rim is the last two pixels of the window, where the case
             * catches the light. */
            const float rim = distance > -2.0f ? (distance + 2.0f) * 0.5f : 0.0f;

            const uint32_t r = (uint32_t)(SUBSTRATE_R + (GLASS_RIM_R - SUBSTRATE_R) * rim);
            const uint32_t g = (uint32_t)(SUBSTRATE_G + (GLASS_RIM_G - SUBSTRATE_G) * rim);
            const uint32_t b = (uint32_t)(SUBSTRATE_B + (GLASS_RIM_B - SUBSTRATE_B) * rim);

            pixels[(size_t)y * width + x] = (r << 24) | (g << 16) | (b << 8) | alpha;
        }
    }

    SDL_Texture* glass = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, width, height);

    if(glass) {
        SDL_UpdateTexture(glass, NULL, pixels, width * (int)sizeof(uint32_t));
        SDL_SetTextureBlendMode(glass, SDL_BLENDMODE_BLEND);
    }

    free(pixels);
    return glass;
}

static bool sim_led_panel_resize(SimLedPanel* instance, int width, int height) {
    if(instance->mask && instance->width == width && instance->height == height) return true;

    if(instance->mask) SDL_DestroyTexture(instance->mask);
    if(instance->lit) SDL_DestroyTexture(instance->lit);
    if(instance->glass) SDL_DestroyTexture(instance->glass);

    instance->mask = sim_led_panel_build_mask(
        instance->renderer, width, height, instance->columns, instance->rows);
    instance->lit = SDL_CreateTexture(
        instance->renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        width,
        height);

    const float pitch = (float)width / (float)instance->columns;
    const int margin = (int)(pitch * GLASS_MARGIN_RATIO + 0.5f);

    instance->glass = sim_led_panel_build_glass(
        instance->renderer, width + 2 * margin, height + 2 * margin, pitch * GLASS_CORNER_RATIO);
    instance->glass_area = (SDL_Rect){
        .x = -margin,
        .y = -margin,
        .w = width + 2 * margin,
        .h = height + 2 * margin,
    };

    instance->width = width;
    instance->height = height;

    return instance->mask && instance->lit && instance->glass;
}

/** The panel grown by @p spread LED pitches on every side. */
static SDL_Rect sim_led_panel_spread(const SDL_Rect* area, float pitch, float spread) {
    const int margin = (int)(pitch * spread + 0.5f);

    return (SDL_Rect){
        .x = area->x - margin,
        .y = area->y - margin,
        .w = area->w + 2 * margin,
        .h = area->h + 2 * margin,
    };
}

SimLedPanel* sim_led_panel_alloc(SDL_Renderer* renderer, int columns, int rows) {
    SimLedPanel* instance = calloc(1, sizeof(SimLedPanel));
    if(!instance) return NULL;

    instance->renderer = renderer;
    instance->columns = columns;
    instance->rows = rows;

    return instance;
}

void sim_led_panel_free(SimLedPanel* instance) {
    if(!instance) return;

    if(instance->mask) SDL_DestroyTexture(instance->mask);
    if(instance->lit) SDL_DestroyTexture(instance->lit);
    if(instance->glass) SDL_DestroyTexture(instance->glass);
    free(instance);
}

void sim_led_panel_render(SimLedPanel* instance, SDL_Texture* frame, const SDL_Rect* area) {
    SDL_Renderer* renderer = instance->renderer;

    if(!sim_led_panel_resize(instance, area->w, area->h)) {
        SDL_RenderCopy(renderer, frame, NULL, area);
        return;
    }

    const SDL_Rect glass_area = {
        .x = area->x + instance->glass_area.x,
        .y = area->y + instance->glass_area.y,
        .w = instance->glass_area.w,
        .h = instance->glass_area.h,
    };
    SDL_RenderCopy(renderer, instance->glass, NULL, &glass_area);

    SDL_SetTextureBlendMode(instance->mask, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(instance->mask, LED_OFF_R, LED_OFF_G, LED_OFF_B);
    SDL_RenderCopy(renderer, instance->mask, NULL, area);

    /* Carve the frame into LEDs off-screen: multiplying the mask straight onto
     * the window would take the dark dots down with it. */
    SDL_SetRenderTarget(renderer, instance->lit);
    SDL_SetTextureBlendMode(frame, SDL_BLENDMODE_NONE);
    SDL_RenderCopy(renderer, frame, NULL, NULL);

    SDL_SetTextureColorMod(instance->mask, 255, 255, 255);
    SDL_SetTextureBlendMode(instance->mask, SDL_BLENDMODE_MOD);
    SDL_RenderCopy(renderer, instance->mask, NULL, NULL);
    SDL_SetRenderTarget(renderer, NULL);

    SDL_SetTextureBlendMode(instance->lit, SDL_BLENDMODE_ADD);
    SDL_RenderCopy(renderer, instance->lit, NULL, area);

    /* Magnifying 72 columns with linear filtering is the blur: neighbouring
     * LEDs bleed into each other exactly as far as they are apart. */
    const float pitch = (float)area->w / (float)instance->columns;
    const SDL_Rect near_area = sim_led_panel_spread(area, pitch, HAZE_NEAR_SPREAD);
    const SDL_Rect far_area = sim_led_panel_spread(area, pitch, HAZE_FAR_SPREAD);

    SDL_SetTextureScaleMode(frame, SDL_ScaleModeLinear);
    SDL_SetTextureBlendMode(frame, SDL_BLENDMODE_ADD);

    /* Held to the window the light comes out of. Without this the wide pass
     * ends on a rectangle out in the background, which reads as a lit box
     * rather than as glow. */
    SDL_RenderSetClipRect(renderer, &glass_area);
    SDL_SetTextureAlphaMod(frame, HAZE_NEAR_ALPHA);
    SDL_RenderCopy(renderer, frame, NULL, &near_area);
    SDL_SetTextureAlphaMod(frame, HAZE_FAR_ALPHA);
    SDL_RenderCopy(renderer, frame, NULL, &far_area);
    SDL_RenderSetClipRect(renderer, NULL);

    SDL_SetTextureAlphaMod(frame, 255);
    SDL_SetTextureBlendMode(frame, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(frame, SDL_ScaleModeNearest);
}
