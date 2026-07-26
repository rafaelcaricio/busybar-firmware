/**
 * Host implementations of the front and back display services.
 *
 * On the device these drive an LED matrix and a greyscale panel over SPI. Here
 * they forward the frame the GUI just flushed straight into the SDL window.
 * The service records exist so gui.c's furi_record_open() calls resolve
 * unchanged.
 */
#include "sim_window.h"

#include <furi.h>

#include <front_display/front_display.h>
#include <back_display/back_display.h>

/* The services carry no state of their own here, but the records must hand
 * back a non-NULL instance because furi_record_open() asserts on it. */
static int front_display_instance;
static int back_display_instance;

void display_host_init(void) {
    furi_record_create(RECORD_FRONT_DISPLAY, &front_display_instance);
    furi_record_create(RECORD_BACK_DISPLAY, &back_display_instance);
}

/* -- front -------------------------------------------------------------- */

void front_display_draw(FrontDisplaySrv* instance, const uint8_t* buf) {
    UNUSED(instance);
    furi_check(buf);
    sim_window_submit_front(buf);
}

void front_display_set_brightness(FrontDisplaySrv* instance, FrontDisplayBrightness brightness) {
    UNUSED(instance);
    sim_window_set_front_brightness(brightness.val);
}

void front_display_set_blanked(FrontDisplaySrv* instance, bool is_blanked) {
    UNUSED(instance);
    sim_window_set_front_blanked(is_blanked);
}

void front_display_sleep_mode(FrontDisplaySrv* instance, bool sleep) {
    UNUSED(instance);
    sim_window_set_front_sleeping(sleep);
}

/* -- back --------------------------------------------------------------- */

void back_display_draw(BackDisplaySrv* instance, const uint8_t* data) {
    UNUSED(instance);
    furi_check(data);
    sim_window_submit_back(data);
}

void back_display_sleep_mode(BackDisplaySrv* instance, bool sleep) {
    UNUSED(instance);
    sim_window_change_back_sleep(sleep);
}

void back_display_set_contrast(BackDisplaySrv* instance, BackDisplayContrast contrast) {
    UNUSED(instance);
    sim_window_set_back_contrast(contrast.val);
}

size_t back_display_get_width(void) {
    return BACK_DISPLAY_W;
}

size_t back_display_get_height(void) {
    return BACK_DISPLAY_H;
}
