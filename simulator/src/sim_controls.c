#include "sim_controls.h"
#include "input_host.h"

#include <furi_hal_resources.h>

#include <string.h>

/* -- 5x7 bitmap font ----------------------------------------------------- *
 *
 * SDL2 draws no text on its own and pulling in SDL_ttf for a handful of
 * button captions is not worth the dependency. Columns are bytes, bit 0 is
 * the top row.
 */

#define FONT_W    (5)
#define FONT_H    (7)
#define FONT_GAP  (1)

static const uint8_t font_digits[10][FONT_W] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
};

static const uint8_t font_letters[26][FONT_W] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x01, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

static const uint8_t font_slash[FONT_W] = {0x20, 0x10, 0x08, 0x04, 0x02};
static const uint8_t font_blank[FONT_W] = {0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t* font_glyph(char c) {
    if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

    if(c >= 'A' && c <= 'Z') return font_letters[c - 'A'];
    if(c >= '0' && c <= '9') return font_digits[c - '0'];
    if(c == '/') return font_slash;

    return font_blank;
}

static int text_width(const char* text, int scale) {
    const int length = (int)strlen(text);
    return length ? (length * (FONT_W + FONT_GAP) - FONT_GAP) * scale : 0;
}

static void draw_text(SDL_Renderer* renderer, int x, int y, int scale, const char* text) {
    for(const char* c = text; *c; c++) {
        const uint8_t* glyph = font_glyph(*c);

        for(int column = 0; column < FONT_W; column++) {
            for(int row = 0; row < FONT_H; row++) {
                if(!(glyph[column] & (1u << row))) continue;

                const SDL_Rect pixel = {
                    .x = x + column * scale,
                    .y = y + row * scale,
                    .w = scale,
                    .h = scale,
                };
                SDL_RenderFillRect(renderer, &pixel);
            }
        }

        x += (FONT_W + FONT_GAP) * scale;
    }
}

static void draw_text_centred(
    SDL_Renderer* renderer,
    const SDL_Rect* area,
    int scale,
    const char* text) {
    draw_text(
        renderer,
        area->x + (area->w - text_width(text, scale)) / 2,
        area->y + (area->h - FONT_H * scale) / 2,
        scale,
        text);
}

static void fill_circle(SDL_Renderer* renderer, int cx, int cy, int radius) {
    for(int dy = -radius; dy <= radius; dy++) {
        const int span = (int)SDL_sqrt((double)(radius * radius - dy * dy));
        SDL_RenderDrawLine(renderer, cx - span, cy + dy, cx + span, cy + dy);
    }
}

/* -- controls ------------------------------------------------------------ */

typedef enum {
    ControlShapeRect,
    ControlShapeCircle,
} ControlShape;

typedef struct {
    const char* label;
    uint8_t key;
    ControlShape shape;
    SDL_Rect rect;
} Control;

/* Order matches the device left to right: mode lever, Start/Pause pad, Back,
 * then the dial split into scroll-up / press / scroll-down. */
static Control controls[] = {
    {"BUSY", InputKeyBusy, ControlShapeRect, {0}},
    {"CUSTOM", InputKeyCustom, ControlShapeRect, {0}},
    {"OFF", InputKeyOff, ControlShapeRect, {0}},
    {"APPS", InputKeyApps, ControlShapeRect, {0}},
    {"SETTINGS", InputKeySettings, ControlShapeRect, {0}},
    {"START/PAUSE", InputKeyStart, ControlShapeRect, {0}},
    {"BACK", InputKeyBack, ControlShapeCircle, {0}},
    {"UP", SIM_KEY_SCROLL_UP, ControlShapeRect, {0}},
    {"OK", InputKeyOk, ControlShapeCircle, {0}},
    {"DOWN", SIM_KEY_SCROLL_DOWN, ControlShapeRect, {0}},
};

#define CONTROL_COUNT     ((int)(sizeof(controls) / sizeof(controls[0])))
#define CONTROL_MODE_FIRST (0)
#define CONTROL_MODE_COUNT (5)
#define CONTROL_START      (5)
#define CONTROL_BACK       (6)
#define CONTROL_UP         (7)
#define CONTROL_OK         (8)
#define CONTROL_DOWN       (9)

static int control_scale = 2;

static int scale_for_width(int width) {
    const int scale = width / 700;
    return scale > 0 ? scale : 1;
}

int sim_controls_preferred_height(int width) {
    /* Five stacked mode positions, each a comfortable touch target, plus the
     * padding above and below. */
    return scale_for_width(width) * 96;
}

void sim_controls_layout(const SDL_Rect* deck) {
    const int scale = scale_for_width(deck->w);
    control_scale = scale;

    const int pad = 6 * scale;

    /* Split the deck the way the top surface is arranged. */
    const int mode_w = (deck->w * 38) / 100;
    const int start_w = (deck->w * 30) / 100;
    const int back_w = (deck->w * 12) / 100;
    const int dial_w = deck->w - mode_w - start_w - back_w - 3 * pad;

    /* Mode lever: five stacked positions, as printed on the case. */
    const int mode_h = (deck->h - 2 * pad) / CONTROL_MODE_COUNT;
    for(int i = 0; i < CONTROL_MODE_COUNT; i++) {
        controls[CONTROL_MODE_FIRST + i].rect = (SDL_Rect){
            .x = deck->x + pad,
            .y = deck->y + pad + i * mode_h,
            .w = mode_w - pad,
            .h = mode_h - scale,
        };
    }

    controls[CONTROL_START].rect = (SDL_Rect){
        .x = deck->x + mode_w + pad,
        .y = deck->y + pad,
        .w = start_w - pad,
        .h = deck->h - 2 * pad,
    };

    const int deck_inner_h = deck->h - 2 * pad;
    int back_size = back_w - 2 * pad;
    if(back_size > deck_inner_h) back_size = deck_inner_h;
    controls[CONTROL_BACK].rect = (SDL_Rect){
        .x = deck->x + mode_w + start_w + pad,
        .y = deck->y + (deck->h - back_size) / 2,
        .w = back_size,
        .h = back_size,
    };

    /* Dial: an outer ring split top and bottom for scrolling, pressed in the
     * middle for OK — the same gestures the physical encoder offers. */
    const int dial_x = deck->x + mode_w + start_w + back_w + pad;
    const int dial_size = dial_w < deck_inner_h ? dial_w : deck_inner_h;
    const int dial_y = deck->y + (deck->h - dial_size) / 2;

    controls[CONTROL_UP].rect = (SDL_Rect){
        .x = dial_x,
        .y = dial_y,
        .w = dial_size,
        .h = dial_size / 3,
    };
    controls[CONTROL_OK].rect = (SDL_Rect){
        .x = dial_x,
        .y = dial_y + dial_size / 3,
        .w = dial_size,
        .h = dial_size / 3,
    };
    controls[CONTROL_DOWN].rect = (SDL_Rect){
        .x = dial_x,
        .y = dial_y + 2 * (dial_size / 3),
        .w = dial_size,
        .h = dial_size / 3,
    };
}

bool sim_controls_hit(int x, int y, uint8_t* key) {
    const SDL_Point point = {x, y};

    for(int i = 0; i < CONTROL_COUNT; i++) {
        if(!SDL_PointInRect(&point, &controls[i].rect)) continue;

        *key = controls[i].key;
        return true;
    }

    return false;
}

static void set_colour(SDL_Renderer* renderer, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
}

static void draw_button(
    SDL_Renderer* renderer,
    const Control* control,
    bool pressed,
    bool accent) {
    const SDL_Rect* rect = &control->rect;

    if(control->shape == ControlShapeCircle) {
        const int cx = rect->x + rect->w / 2;
        const int cy = rect->y + rect->h / 2;
        const int radius = (rect->w < rect->h ? rect->w : rect->h) / 2;

        if(accent) {
            /* The Back button is orange on the hardware. */
            if(pressed) {
                set_colour(renderer, 255, 150, 80);
            } else {
                set_colour(renderer, 214, 88, 34);
            }
        } else {
            set_colour(renderer, pressed ? 235 : 200, pressed ? 235 : 200, pressed ? 240 : 205);
        }
        fill_circle(renderer, cx, cy, radius);

        set_colour(renderer, 20, 20, 24);
        draw_text_centred(renderer, rect, control_scale, control->label);
        return;
    }

    if(pressed) {
        set_colour(renderer, 236, 236, 240);
    } else if(accent) {
        set_colour(renderer, 198, 60, 42);
    } else {
        set_colour(renderer, 58, 58, 66);
    }
    SDL_RenderFillRect(renderer, rect);

    set_colour(renderer, 96, 96, 106);
    SDL_RenderDrawRect(renderer, rect);

    if(pressed) {
        set_colour(renderer, 20, 20, 24);
    } else {
        set_colour(renderer, 226, 226, 232);
    }
    draw_text_centred(renderer, rect, control_scale, control->label);
}

bool sim_controls_selftest(void) {
    bool ok = true;

    for(int i = 0; i < CONTROL_COUNT; i++) {
        const Control* control = &controls[i];
        const int cx = control->rect.x + control->rect.w / 2;
        const int cy = control->rect.y + control->rect.h / 2;

        uint8_t key = InputKeyMAX;
        const bool hit = sim_controls_hit(cx, cy, &key);

        if(!hit || key != control->key) {
            SDL_Log("control selftest FAIL: %s at (%d,%d) hit=%d key=%u expected=%u",
                    control->label, cx, cy, hit, key, control->key);
            ok = false;
        }
    }

    SDL_Log("control selftest: %s (%d controls)", ok ? "pass" : "FAIL", CONTROL_COUNT);
    return ok;
}

void sim_controls_render(SDL_Renderer* renderer, const bool* pressed) {
    const InputKey selected_mode = input_host_get_switch_key();

    for(int i = 0; i < CONTROL_COUNT; i++) {
        const Control* control = &controls[i];
        const bool is_mode = i < CONTROL_MODE_FIRST + CONTROL_MODE_COUNT;

        /* A lever stays where it was put, so the selected position is drawn
         * held down rather than only for as long as the click lasts. */
        const bool held = pressed[control->key] || (is_mode && control->key == selected_mode);

        /* BUSY is printed in red on the case; Back is the orange stud. */
        const bool accent = (i == CONTROL_MODE_FIRST) || (i == CONTROL_BACK);

        draw_button(renderer, control, held, accent);
    }
}
