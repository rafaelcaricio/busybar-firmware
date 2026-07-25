/**
 * BUSY Bar UI simulator.
 *
 * Runs the firmware's real GUI service — LVGL, the widget layer and the
 * themes, unmodified — on the host, with the two display drivers pointed at
 * an SDL window instead of the panels.
 *
 * Thread layout:
 *
 *   main thread      SDL only: window, event pump, present. Signals blocked
 *                    so the FreeRTOS tick never lands here.
 *   scheduler thread furi_init() plus vTaskStartScheduler(); becomes the
 *                    FreeRTOS "main" thread that the port sigwaits on.
 *   task threads     Created by the port. gui_srv, input_host and the scene.
 */
#include "discovery_host.h"
#include "display_host.h"
#include "input_host.h"
#include "platform_services_host.h"
#include "scene_host.h"
#include "services_host.h"
#include "sim_window.h"
#include "storage_host.h"
#include "web_api_host.h"

#include <furi.h>
#include <furi_hal_resources.h>

#include <storage/storage.h>

#include <FreeRTOS.h>
#include <task.h>

#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_DELAY_US (16000)

extern int32_t gui_srv(void* arg);
/* The display themes open the font registry record, so it has to exist before
 * gui_srv reaches lv_theme_front_alloc(). */
extern int font_registry_startup(void* arg);
/* The real busy timer service: the busy app opens its record, and it needs
 * only the RTC and records the simulator already provides. */
extern int32_t busy_timer_srv(void* arg);
/* The two services behind the mode selector: the loader owns app lifetime and
 * the desktop maps selector positions onto apps. */
extern int32_t loader_srv(void* arg);
extern int32_t desktop_srv(void* arg);
/* The firmware's own HTTP API, and the canvas service its /api/display routes
 * draw through. */
extern int32_t web_srv_start(void* arg);
extern int32_t canvas_service_start(void* arg);
/* Watches every other service and encodes what changed as protobuf; this is
 * what /api/status/ws streams. */
extern int32_t state_publisher_srv(void* arg);
/* Captures the log into the ring buffer /api/log_dump snapshots. */
extern void log_storage_on_system_start(void);

/* The device serves on port 80. That needs root here, and would stop two
 * simulators from running side by side, so the API moves to a high port. */
#define API_PORT_DEFAULT (8042)

static struct {
    int scale;
    const char* scene;
    const char* screenshot_dir;
    int exit_after_frames;
    FuriLogLevel log_level;
    const char* keys;
    int api_port;
    bool announce;
} config = {
    /* 0 means "as large as the screen allows"; see sim_window_init(). */
    .scale = 0,
    /* No scene by default: the mode selector decides what runs, as on the
     * device. --scene overrides the boot app without pinning it. */
    .scene = NULL,
    .screenshot_dir = NULL,
    .exit_after_frames = 0,
    .log_level = FuriLogLevelInfo,
    .keys = NULL,
    .api_port = API_PORT_DEFAULT,
    .announce = true,
};

/** Button names accepted by --keys. */
static const struct {
    const char* name;
    InputKey key;
} simulator_key_names[] = {
    /* Screen-relative, like the arrow keys; see SIM_KEY_SCROLL_UP. */
    {"up", SIM_KEY_SCROLL_UP},  {"down", SIM_KEY_SCROLL_DOWN},
    {"ok", InputKeyOk},         {"back", InputKeyBack},
    {"start", InputKeyStart},   {"busy", InputKeyBusy},
    {"custom", InputKeyCustom}, {"off", InputKeyOff},
    {"apps", InputKeyApps},     {"settings", InputKeySettings},
};

/** Replay a comma-separated button script, e.g. "down,ok".
 *
 * Each press is held briefly and followed by a settle delay, so the UI gets
 * the same press/release/short sequence a real button produces and has time
 * to animate between steps. */
static void simulator_replay_keys(const char* script) {
    const char* cursor = script;

    while(*cursor) {
        const char* separator = strchr(cursor, ',');
        const size_t length = separator ? (size_t)(separator - cursor) : strlen(cursor);

        bool matched = false;
        for(size_t i = 0; i < COUNT_OF(simulator_key_names); i++) {
            const char* name = simulator_key_names[i].name;
            if(strlen(name) != length || strncmp(name, cursor, length) != 0) continue;

            FURI_LOG_I("Sim", "key %s", name);
            sim_window_inject_key(simulator_key_names[i].key, true);
            furi_delay_ms(60);
            sim_window_inject_key(simulator_key_names[i].key, false);
            furi_delay_ms(400);

            matched = true;
            break;
        }

        if(!matched) FURI_LOG_E("Sim", "unknown key in --keys: %.*s", (int)length, cursor);

        cursor += length;
        if(*cursor == ',') cursor++;
    }
}

static void simulator_log_callback(const uint8_t* data, size_t size, void* context) {
    UNUSED(context);
    fwrite(data, 1, size, stderr);
}

/* gui.c opens the power record before anything else; it never calls into it. */
static int power_instance;

/* Shared with the LVGL filesystem driver, which serves the same tree. */
const char* simulator_assets_root = NULL;

/** Give the UI a moment to settle, capture, and shut the window down.
 *
 * Runs as a FreeRTOS task because the PNG encoder allocates from furi's heap,
 * which is only safe to touch from a scheduled context.
 */
static int32_t simulator_capture_thread(void* context) {
    UNUSED(context);

    furi_delay_ms((uint32_t)config.exit_after_frames * (FRAME_DELAY_US / 1000));

    if(config.keys) simulator_replay_keys(config.keys);

    if(config.screenshot_dir) sim_window_screenshot(config.screenshot_dir);
    sim_window_request_quit();

    return 0;
}

/* furi hands a finished task to a reaper rather than deleting it where it ran:
 * the FreeRTOS task cannot free its own stack while it is standing on it. The
 * reaper is also what delivers FuriThreadStateStopped, which is how the loader
 * learns an app has exited, so without it an app can start but never end.
 *
 * The device runs this on its init task once startup is done (see
 * targets/f21/src/main.c); here it gets a task of its own.
 */
static int32_t simulator_reaper_thread(void* context) {
    UNUSED(context);

    furi_background();

    return 0;
}

/* The desktop's startup app plays the out-of-the-box animation and waits for a
 * button unless this flag says setup is already done. Those animations live on
 * the recovery partition, which the simulator has no counterpart for, so
 * without the flag it would sit on a blank screen until something was pressed.
 * The simulator stands in for a device that has been unboxed. */
static void simulator_mark_setup_complete(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);

    if(!storage_file_exists(storage, APP_DATA_PATH("done.txt"))) {
        storage_simply_mkpath(storage, STORAGE_APP_DATA_PATH_PREFIX);
        storage_simply_write_entire_file(storage, APP_DATA_PATH("done.txt"), "", 0);
    }

    furi_record_close(RECORD_STORAGE);
}

static void* simulator_scheduler_thread(void* arg) {
    UNUSED(arg);

    furi_init();

    /* Nothing registers a log sink on the host — the firmware's goes to the
     * debug UART — so furi's logging would otherwise be silent. */
    furi_log_add_handler((FuriLogHandler){.callback = simulator_log_callback, .context = NULL});
    furi_log_set_level(config.log_level);

    log_storage_on_system_start();

    furi_record_create("power", &power_instance);
    {
        /* Default to the mirrored asset tree CMake builds; the env var lets
         * you point at a different checkout of the resources. */
        const char* assets = getenv("BUSYBAR_SIM_ASSETS");
        if(!assets) assets = BUSYBAR_SIM_DEFAULT_ASSETS;
        storage_host_init(assets);
        simulator_assets_root = assets;
    }
    display_host_init();
    input_host_init();
    font_registry_startup(NULL);
    services_host_init();
    platform_services_host_init();
    simulator_mark_setup_complete();

    FuriThread* reaper = furi_thread_alloc_ex("reaper", 8 * 1024, simulator_reaper_thread, NULL);
    furi_thread_set_priority(reaper, FuriThreadPriorityHighest);
    furi_thread_start(reaper);

    FuriThread* gui = furi_thread_alloc_ex("gui", 16 * 1024, gui_srv, NULL);
    furi_thread_start(gui);

    FuriThread* busy_timer = furi_thread_alloc_ex("busy_timer", 8 * 1024, busy_timer_srv, NULL);
    furi_thread_start(busy_timer);

    /* Before the canvas and the HTTP API: both open the loader record to weigh
     * the foreground app's priority against an incoming draw. */
    FuriThread* loader = furi_thread_alloc_ex("loader", 8 * 1024, loader_srv, NULL);
    furi_thread_start(loader);

    if(config.api_port > 0) {
        web_api_host_init((uint16_t)config.api_port);

        /* Before web_srv, which announces itself as it starts listening. */
        discovery_host_init((uint16_t)config.api_port, config.announce);

        /* Before the server: the display routes open the canvas record and
         * /api/status/ws opens the state publisher's. */
        FuriThread* canvas =
            furi_thread_alloc_ex("canvas", 8 * 1024, canvas_service_start, NULL);
        furi_thread_start(canvas);

        FuriThread* state_publisher =
            furi_thread_alloc_ex("state_publisher", 16 * 1024, state_publisher_srv, NULL);
        furi_thread_start(state_publisher);

        FuriThread* web = furi_thread_alloc_ex("web_srv", 32 * 1024, web_srv_start, NULL);
        furi_thread_start(web);
    }

    /* The demo scene draws straight onto the main layer, so it and the desktop
     * cannot share the screen; anything else boots the device properly and
     * lets the mode selector work. */
    if(scene_host_is_demo(config.scene)) {
        scene_host_start(config.scene);

    } else {
        FuriThread* desktop = furi_thread_alloc_ex("desktop", 8 * 1024, desktop_srv, NULL);
        furi_thread_start(desktop);

        if(config.scene) scene_host_start(config.scene);
    }

    if(config.exit_after_frames) {
        FuriThread* capture =
            furi_thread_alloc_ex("capture", 8 * 1024, simulator_capture_thread, NULL);
        furi_thread_start(capture);
    }

    vTaskStartScheduler();

    return NULL;
}

static void simulator_block_signals(void) {
    /* The port resumes tasks with SIGUSR1 and ticks them with SIGALRM. A
     * thread that is not a FreeRTOS task must handle neither: the tick handler
     * calls vTaskSwitchContext() and then suspends the thread it ran on, so a
     * tick caught by the SDL or AppKit threads parks them on a task's condvar
     * and deadlocks the scheduler. Threads inherit this mask, which is why it
     * is installed before anything else starts one. SIGINT stays through so
     * Ctrl-C still works. */
    sigset_t signals;
    sigfillset(&signals);
    sigdelset(&signals, SIGINT);
    pthread_sigmask(SIG_SETMASK, &signals, NULL);
}

static void simulator_print_usage(const char* argv0) {
    printf(
        "Usage: %s [options]\n"
        "\n"
        "  -s, --scale N     pixels per front LED (default: fills the screen)\n"
        "      --scene NAME  app to boot into, or \"demo\" for the widget demo.\n"
        "                    By default the mode selector decides, as on the device.\n"
        "      --frames N    run N frames then exit\n"
        "      --keys LIST   replay buttons before exiting, e.g. \"down,ok\"\n"
        "      --list-apps   list every app that can be given to --scene\n"
        "      --screenshot DIR  write front.png/back.png on exit (with --frames)\n"
        "      --api-port N  serve the device HTTP API on N (default 8042, 0 disables)\n"
        "      --no-mdns     do not announce the simulator on the local network\n"
        "  -h, --help        this message\n"
        "\n"
        "Environment:\n"
        "  BUSYBAR_SIM_ASSETS  directory served as LVGL drive 'C'\n"
        "\n"
        "Keys: arrows = dial, Enter/Space = Ok, Esc = Back, s = Start\n"
        "Mode selector: b = Busy, c = Custom, o = Off, a = Apps, ',' = Settings\n",
        argv0);
}

int main(int argc, char** argv) {
    static const struct option options[] = {
        {"scale", required_argument, NULL, 's'},
        {"scene", required_argument, NULL, 'n'},
        {"screenshot", required_argument, NULL, 'p'},
        {"frames", required_argument, NULL, 'f'},
        {"verbose", no_argument, NULL, 'v'},
        {"keys", required_argument, NULL, 'k'},
        {"list-apps", no_argument, NULL, 'l'},
        {"api-port", required_argument, NULL, 'a'},
        {"no-mdns", no_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while((opt = getopt_long(argc, argv, "s:hv", options, NULL)) != -1) {
        switch(opt) {
        case 's':
            config.scale = atoi(optarg);
            break;
        case 'n':
            config.scene = optarg;
            break;
        case 'p':
            config.screenshot_dir = optarg;
            break;
        case 'f':
            config.exit_after_frames = atoi(optarg);
            break;
        case 'k':
            config.keys = optarg;
            break;
        case 'v':
            config.log_level = FuriLogLevelTrace;
            break;
        case 'a':
            config.api_port = atoi(optarg);
            break;
        case 'm':
            config.announce = false;
            break;
        case 'l':
            scene_host_list_apps();
            return 0;
        case 'h':
            simulator_print_usage(argv[0]);
            return 0;
        default:
            simulator_print_usage(argv[0]);
            return 1;
        }
    }

    /* Before SDL_Init, so that every thread SDL and AppKit create inherits the
     * mask. A thread that has SIGALRM unblocked can be handed the FreeRTOS
     * tick, and the tick handler switches context — on a thread the scheduler
     * knows nothing about. */
    simulator_block_signals();

    if(!sim_window_init(config.scale)) {
        return 1;
    }

    sim_window_request_selftest();

    pthread_t scheduler;
    if(pthread_create(&scheduler, NULL, simulator_scheduler_thread, NULL) != 0) {
        fprintf(stderr, "failed to start the scheduler thread\n");
        sim_window_deinit();
        return 1;
    }

    while(sim_window_pump()) {
        usleep(FRAME_DELAY_US);
    }

    sim_window_deinit();

    /* The scheduler thread owns FreeRTOS state that has no clean teardown
     * path here; the process is going away regardless. */
    _exit(0);
}
