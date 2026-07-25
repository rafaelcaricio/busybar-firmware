/**
 * Scenes: small widget trees built against the real GUI API.
 *
 * This is the part you edit while iterating on a design. Everything a scene
 * calls here — gui_get_layer, label_alloc, widget_set_align — is the same API
 * an application uses on the device, so what the window shows is what the
 * panel will show.
 *
 * Naming an application instead of a scene hands it to the desktop service
 * rather than starting a bare thread, which is what the device does for an app
 * started from anywhere other than the mode selector. The request outlives the
 * selector's own startup default, so --scene decides what is on screen at boot
 * without pinning it: flicking a mode still switches away.
 */
#include "scene_host.h"

#include <furi.h>

#include <stdio.h>
#include <string.h>

#include <applications.h>

#include <desktop/desktop.h>
#include <gui/gui.h>
#include <gui/modules/label.h>

#define TAG "Scene"

static void scene_demo(Gui* gui) {
    GuiLayer* layer = gui_get_layer(gui, GuiLayerIdMain);

    Widget* front_root = gui_layer_get_root_widget(layer, GuiDisplayIdFront);
    Label* front_label = label_alloc(front_root);
    label_set_text(front_label, "BUSY");
    label_set_text_font_size(front_label, LabelFontSizeNormal);
    label_set_text_color(front_label, (Color){.r = 255, .g = 80, .b = 0, .a = 255});
    widget_set_align(label_get_base(front_label), AlignCenter);

    Widget* back_root = gui_layer_get_root_widget(layer, GuiDisplayIdBack);

    Label* back_label = label_alloc(back_root);
    label_set_text(back_label, "Simulator");
    label_set_text_font_size(back_label, LabelFontSizeNormal);
    label_set_text_color(back_label, (Color){.r = 255, .g = 255, .b = 255, .a = 255});
    widget_set_align(label_get_base(back_label), AlignCenter);
}

/** The applications the simulator links, as generated from their manifests.
 *
 * Which apps those are is decided in CMakeLists.txt (SIM_APP_DIRS); this file
 * no longer names any of them. An app is runnable once every record it opens
 * exists — see services_host.c for what the simulator provides.
 */
static const FlipperInternalApplication* scene_host_find_app(const char* appid) {
    const struct {
        const FlipperInternalApplication* apps;
        size_t count;
    } tables[] = {
        {FLIPPER_APPS, FLIPPER_APPS_COUNT},
        {FLIPPER_SYSTEM_APPS, FLIPPER_SYSTEM_APPS_COUNT},
        {FLIPPER_SETTINGS_APPS, FLIPPER_SETTINGS_APPS_COUNT},
        {FLIPPER_DEBUG_APPS, FLIPPER_DEBUG_APPS_COUNT},
    };

    for(size_t table = 0; table < COUNT_OF(tables); table++) {
        for(size_t i = 0; i < tables[table].count; i++) {
            const FlipperInternalApplication* app = &tables[table].apps[i];
            if(strcmp(appid, app->appid) == 0) return app;
        }
    }

    return NULL;
}

void scene_host_list_apps(void) {
    const struct {
        const char* kind;
        const FlipperInternalApplication* apps;
        size_t count;
    } tables[] = {
        {"app", FLIPPER_APPS, FLIPPER_APPS_COUNT},
        {"system", FLIPPER_SYSTEM_APPS, FLIPPER_SYSTEM_APPS_COUNT},
        {"settings", FLIPPER_SETTINGS_APPS, FLIPPER_SETTINGS_APPS_COUNT},
        {"debug", FLIPPER_DEBUG_APPS, FLIPPER_DEBUG_APPS_COUNT},
    };

    printf("%-22s %-10s %s\n", "SCENE", "TYPE", "NAME");
    printf("%-22s %-10s %s\n", "demo", "scene", "built-in widget demo");

    for(size_t table = 0; table < COUNT_OF(tables); table++) {
        for(size_t i = 0; i < tables[table].count; i++) {
            const FlipperInternalApplication* app = &tables[table].apps[i];
            printf("%-22s %-10s %s\n", app->appid, tables[table].kind, app->name);
        }
    }
}

static int32_t scene_host_srv(void* context) {
    const char* name = context;

    Gui* gui = furi_record_open(RECORD_GUI);

    gui_lock(gui);
    scene_demo(gui);
    gui_unlock(gui);

    FURI_LOG_I(TAG, "scene \"%s\" built", name);
    return 0;
}

bool scene_host_is_demo(const char* name) {
    return name && strcmp(name, "demo") == 0;
}

/* Runs as a task because opening the desktop record blocks until the desktop
 * service has created it, and callers set this up before the scheduler is
 * running -- where a block is a deadlock, not a wait. */
static int32_t scene_host_request_app(void* context) {
    const FlipperInternalApplication* app = context;

    Desktop* desktop = furi_record_open(RECORD_DESKTOP);

    FURI_LOG_I(TAG, "starting app \"%s\" (%s)", app->appid, app->name);
    if(!desktop_replace_current_app(desktop, app->appid, NULL)) {
        FURI_LOG_E(TAG, "desktop refused to start \"%s\"", app->appid);
    }

    furi_record_close(RECORD_DESKTOP);
    return 0;
}

void scene_host_start(const char* name) {
    if(scene_host_is_demo(name)) {
        FuriThread* thread = furi_thread_alloc_ex("scene", 8 * 1024, scene_host_srv, (void*)name);
        furi_thread_start(thread);
        return;
    }

    const FlipperInternalApplication* app = scene_host_find_app(name);
    if(!app) {
        FURI_LOG_E(TAG, "unknown scene or app \"%s\"; leaving the mode selector in charge", name);
        return;
    }

    FuriThread* thread =
        furi_thread_alloc_ex("scene", 8 * 1024, scene_host_request_app, (void*)app);
    furi_thread_start(thread);
}
