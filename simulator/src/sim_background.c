#include "sim_background.h"

#include <sim_background_generated.h>

#include <stdio.h>
#include <stdlib.h>

/* -- measured geometry ---------------------------------------------------- *
 *
 * All four rectangles are in the source image's pixels, taken off the renders
 * by thresholding, and hold for those images only — hence the static_asserts.
 *
 *   case     the device's bounding box plus two pixels of slack. Everything
 *            outside it is background, and blitting it would waste the window.
 *   face     the black front or back panel, rim included. The layout scales by
 *            this because it is the same physical part in both renders.
 *   window   the darker area the display shows through. On the front that is
 *            the rounded window around the matrix; on the back the screen has
 *            no surround, so it is the screen.
 *   display  where the framebuffer goes.
 *
 * The back render bakes a status bar into its screen area, which is why the
 * measurement is unambiguous there: the rectangle is 412x207, and 412/207 is
 * 1.99 against the panel's own 160/80.
 *
 * The front matrix is not drawn in its render, so its rectangle is derived
 * from the 1539x371 window instead. Requiring a square LED pitch fixes the
 * horizontal inset at 4.5x the vertical one, and the window's corners — a
 * squircle, not an arc, so its outline was traced rather than fitted — fix how
 * small the insets can get. The largest matrix that still clears that outline
 * by 8px insets 34px and 22px, for a pitch of 20.43px both ways.
 */

typedef struct {
    const char* path;
    int image_w;
    int image_h;
    SDL_Rect case_area;
    SDL_Rect face_area;
    SDL_Rect window_area;
    SDL_Rect display_area;
} SimBackgroundSource;

static const SimBackgroundSource sources[] = {
    [SimBackgroundSideFront] =
        {
            .path = SIM_BACKGROUND_FRONT_PATH,
            .image_w = SIM_BACKGROUND_FRONT_WIDTH,
            .image_h = SIM_BACKGROUND_FRONT_HEIGHT,
            .case_area = {44, 180, 1568, 559},
            .face_area = {46, 343, 1564, 394},
            .window_area = {58, 355, 1539, 371},
            .display_area = {92, 377, 1471, 327},
        },
    [SimBackgroundSideBack] =
        {
            .path = SIM_BACKGROUND_BACK_PATH,
            .image_w = SIM_BACKGROUND_BACK_WIDTH,
            .image_h = SIM_BACKGROUND_BACK_HEIGHT,
            .case_area = {45, 138, 1617, 542},
            .face_area = {77, 310, 1550, 368},
            .window_area = {649, 352, 412, 207},
            .display_area = {649, 352, 412, 207},
        },
};

static_assert(
    SIM_BACKGROUND_FRONT_WIDTH == 1672 && SIM_BACKGROUND_FRONT_HEIGHT == 941,
    "front.png changed size; the rectangles above were measured on 1672x941");
static_assert(
    SIM_BACKGROUND_BACK_WIDTH == 1672 && SIM_BACKGROUND_BACK_HEIGHT == 941,
    "back.png changed size; the rectangles above were measured on 1672x941");

struct SimBackground {
    SDL_Renderer* renderer;
    const SimBackgroundSource* source;
    SDL_Texture* texture;
};

/** Read the raw frame straight into the texture.
 *
 * A row at a time into locked texture memory: the frames are around 4.7MB
 * each, and every allocator in this process ends up at furi's heap, which has
 * not been started yet when the window is built.
 */
static bool sim_background_load(SDL_Texture* texture, const SimBackgroundSource* source) {
    FILE* file = fopen(source->path, "rb");
    if(!file) {
        fprintf(stderr, "[sim] could not open background %s\n", source->path);
        return false;
    }

    void* pixels = NULL;
    int pitch = 0;
    if(SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0) {
        fprintf(stderr, "[sim] SDL_LockTexture failed: %s\n", SDL_GetError());
        fclose(file);
        return false;
    }

    bool ok = true;
    for(int y = 0; y < source->image_h && ok; y++) {
        uint8_t* row = (uint8_t*)pixels + (size_t)y * pitch;
        ok = fread(row, 3, (size_t)source->image_w, file) == (size_t)source->image_w;
    }

    SDL_UnlockTexture(texture);
    fclose(file);

    if(!ok) fprintf(stderr, "[sim] background %s is short\n", source->path);

    return ok;
}

SimBackground* sim_background_alloc(SDL_Renderer* renderer, SimBackgroundSide side) {
    const SimBackgroundSource* source = &sources[side];

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        source->image_w,
        source->image_h);
    if(!texture) {
        fprintf(stderr, "[sim] background texture failed: %s\n", SDL_GetError());
        return NULL;
    }

    if(!sim_background_load(texture, source)) {
        SDL_DestroyTexture(texture);
        return NULL;
    }

    /* The case is drawn smaller than it was rendered at any usable window
     * size, so it is minified and wants filtering. */
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);

    SimBackground* instance = calloc(1, sizeof(SimBackground));
    if(!instance) {
        SDL_DestroyTexture(texture);
        return NULL;
    }

    instance->renderer = renderer;
    instance->source = source;
    instance->texture = texture;

    return instance;
}

void sim_background_free(SimBackground* instance) {
    if(!instance) return;

    if(instance->texture) SDL_DestroyTexture(instance->texture);
    free(instance);
}

float sim_background_width_ratio(const SimBackground* instance) {
    return (float)instance->source->case_area.w / (float)instance->source->face_area.w;
}

float sim_background_height_ratio(const SimBackground* instance) {
    return (float)instance->source->case_area.h / (float)instance->source->face_area.w;
}

float sim_background_display_ratio(const SimBackground* instance) {
    return (float)instance->source->display_area.w / (float)instance->source->face_area.w;
}

SDL_Rect sim_background_case_rect(
    const SimBackground* instance,
    int face_width,
    int face_centre_x,
    int top) {
    const SimBackgroundSource* source = instance->source;
    const float scale = (float)face_width / (float)source->face_area.w;

    /* Where the face's centre falls inside the case, so the two sides line up
     * on the panel rather than on their bounding boxes. */
    const float face_centre = (float)(source->face_area.x - source->case_area.x) +
                              (float)source->face_area.w * 0.5f;

    return (SDL_Rect){
        .x = face_centre_x - (int)(face_centre * scale + 0.5f),
        .y = top,
        .w = (int)((float)source->case_area.w * scale + 0.5f),
        .h = (int)((float)source->case_area.h * scale + 0.5f),
    };
}

/** An image-space rectangle mapped onto the case as it was drawn. */
static SDL_Rect sim_background_map(
    const SimBackgroundSource* source,
    const SDL_Rect* area,
    const SDL_Rect* case_rect) {
    const float scale = (float)case_rect->w / (float)source->case_area.w;

    return (SDL_Rect){
        .x = case_rect->x + (int)((float)(area->x - source->case_area.x) * scale + 0.5f),
        .y = case_rect->y + (int)((float)(area->y - source->case_area.y) * scale + 0.5f),
        .w = (int)((float)area->w * scale + 0.5f),
        .h = (int)((float)area->h * scale + 0.5f),
    };
}

SDL_Rect sim_background_display_rect(const SimBackground* instance, const SDL_Rect* case_rect) {
    return sim_background_map(instance->source, &instance->source->display_area, case_rect);
}

SDL_Rect sim_background_window_rect(const SimBackground* instance, const SDL_Rect* case_rect) {
    return sim_background_map(instance->source, &instance->source->window_area, case_rect);
}

void sim_background_render(SimBackground* instance, const SDL_Rect* case_rect) {
    SDL_RenderCopy(instance->renderer, instance->texture, &instance->source->case_area, case_rect);
}
