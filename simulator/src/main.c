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
#include "light_sensor_host.h"
#include "platform_services_host.h"
#include "scene_host.h"
#include "services_host.h"
#include "sim_control.h"
#include "sim_window.h"
#include "storage_host.h"
#include "web_api_host.h"

#include <furi.h>
#include <furi_hal_resources.h>

#include <brightness_control/brightness_control.h>
#include <storage/storage.h>

#include <FreeRTOS.h>
#include <task.h>

#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define FRAME_DELAY_US (16000)

extern int32_t gui_srv(void* arg);
/* The display themes open the font registry record, so it has to exist before
 * gui_srv reaches lv_theme_front_alloc(). */
extern int font_registry_startup(void* arg);
/* The real busy timer service: the busy app opens its record, and it needs
 * only the RTC and records the simulator already provides. */
extern int32_t busy_timer_srv(void* arg);
extern int brightness_control_srv(void* arg);
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
    const char* listen_address;
    bool announce;
    const char* control_path;
    const char* state_dir;
    bool headless;
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
    .listen_address = "127.0.0.1",
    /* Discovery is opt-in because the API is loopback-only by default. */
    .announce = false,
    /* Off unless asked for: nothing but the tools speaks it. */
    .control_path = NULL,
    /* NULL creates a fresh temporary writable overlay. */
    .state_dir = NULL,
    .headless = false,
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

/** Give the UI a moment to settle, capture, and shut the window down.
 *
 * Runs as a FreeRTOS task because the PNG encoder allocates from furi's heap,
 * which is only safe to touch from a scheduled context.
 */
static int32_t simulator_capture_thread(void* context) {
    UNUSED(context);

    SimWindowStatus status;
    sim_window_status(&status);
    const uint64_t target = status.frames + (uint64_t)config.exit_after_frames;
    const uint64_t requested_timeout = (uint64_t)config.exit_after_frames * 100;
    const uint32_t timeout =
        requested_timeout > UINT32_MAX ? UINT32_MAX :
        requested_timeout < 5000      ? 5000 :
                                        (uint32_t)requested_timeout;

    if(!sim_window_wait_for_frame(target, timeout)) {
        FURI_LOG_E(
            "Sim",
            "timed out waiting for rendered frame %llu",
            (unsigned long long)target);
        sim_window_request_quit();
        return -1;
    }

    if(config.keys) simulator_replay_keys(config.keys);

    if(config.screenshot_dir && !sim_window_screenshot(config.screenshot_dir, 0)) {
        FURI_LOG_E("Sim", "exit screenshot failed");
    }
    sim_window_request_quit();

    return 0;
}

/** Where a capture asked for while the simulator runs is written. */
static const char* simulator_screenshot_dir(void) {
    return config.screenshot_dir ? config.screenshot_dir : ".";
}

static volatile sig_atomic_t simulator_screenshot_signalled;
static volatile sig_atomic_t simulator_terminate_signalled;

static void simulator_screenshot_signal(int signal) {
    UNUSED(signal);
    simulator_screenshot_signalled = 1;
}

static void simulator_terminate_signal(int signal) {
    UNUSED(signal);
    simulator_terminate_signalled = 1;
}

/** Let SIGUSR2 ask for a screenshot, so a script driving the HTTP API can take
 * one without the window being touched.
 *
 * A handler rather than a sigwait thread: the port unblocks every signal on a
 * task's thread the first time it runs (see prvSetupSignalsAndSchedulerPolicy),
 * so a signal aimed at the process lands on whichever task is running and a
 * thread waiting for it never sees it — SIGUSR2 would just kill the simulator.
 * All the handler does is set a flag, which is safe wherever it runs.
 *
 * SIGUSR2 because the port has already taken SIGUSR1 to resume tasks.
 */
static void simulator_install_screenshot_signal(void) {
    struct sigaction action = {
        .sa_handler = simulator_screenshot_signal,
        /* Tasks spend their time in syscalls; do not hand them an EINTR. */
        .sa_flags = SA_RESTART,
    };
    sigemptyset(&action.sa_mask);

    if(sigaction(SIGUSR2, &action, NULL) != 0) {
        fprintf(stderr, "[sim] no SIGUSR2 screenshot trigger: %s\n", strerror(errno));
    }
}

static void simulator_install_terminate_signals(void) {
    struct sigaction action = {
        .sa_handler = simulator_terminate_signal,
        .sa_flags = SA_RESTART,
    };
    sigemptyset(&action.sa_mask);

    if(sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) {
        fprintf(stderr, "[sim] no graceful termination handler: %s\n", strerror(errno));
    }
}

/** Serve screenshot requests for as long as the simulator is up.
 *
 * F12 and SIGUSR2 both only raise a flag: the encoder allocates from furi's
 * heap, so the capture itself has to happen in a task. Captures are numbered
 * from one so a session's worth of them accumulates rather than overwrites,
 * which is what driving the simulator over the HTTP API wants.
 */
static int32_t simulator_screenshot_thread(void* context) {
    UNUSED(context);

    while(true) {
        bool requested = sim_window_take_screenshot_request();

        if(simulator_screenshot_signalled) {
            simulator_screenshot_signalled = 0;
            requested = true;
        }

        if(requested) {
            unsigned sequence;
            uint64_t frame;
            sim_window_screenshot_next(simulator_screenshot_dir(), &sequence, &frame);
        }

        furi_delay_ms(50);
    }

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

/** Initialize simulator services from the first scheduled task.
 *
 * The POSIX FreeRTOS port gives every task a pthread. Creating all service
 * threads before vTaskStartScheduler() made their startup race the scheduler's
 * transition to running; occasionally one reached a blocking queue operation
 * while FreeRTOS still reported itself suspended. The device also performs
 * service startup from its init task, then turns that same task into the thread
 * reaper. Mirroring that ordering removes the host-only race.
 */
static int32_t simulator_init_thread(void* arg) {
    UNUSED(arg);

    furi_record_create("power", &power_instance);
    {
        /* Default to the mirrored asset tree CMake builds; the env var lets
         * you point at a different checkout of the resources. */
        const char* assets = getenv("BUSYBAR_SIM_ASSETS");
        if(!assets) assets = BUSYBAR_SIM_DEFAULT_ASSETS;
        const char* state = config.state_dir;
        if(!state) state = getenv("BUSYBAR_SIM_STATE");
        if(!storage_host_init(assets, state)) {
            sim_window_request_quit();
            return -1;
        }
    }
    display_host_init();
    input_host_init();
    font_registry_startup(NULL);
    services_host_init();
    platform_services_host_init();
    light_sensor_host_init();
    simulator_mark_setup_complete();

    FuriThread* brightness =
        furi_thread_alloc_ex("brightness", 8 * 1024, brightness_control_srv, NULL);
    furi_thread_start(brightness);

    /* Opening the record waits until brightness_control_alloc() has subscribed
     * to the light sensor. Publishing afterward guarantees that the normal
     * firmware event path, rather than a simulator display hook, raises a
     * fresh auto-brightness setting to 100%. */
    furi_record_open(RECORD_BRIGHTNESS_CONTROL);
    light_sensor_host_publish_max();
    furi_record_close(RECORD_BRIGHTNESS_CONTROL);

    FuriThread* gui = furi_thread_alloc_ex("gui", 16 * 1024, gui_srv, NULL);
    furi_thread_start(gui);

    FuriThread* busy_timer = furi_thread_alloc_ex("busy_timer", 8 * 1024, busy_timer_srv, NULL);
    furi_thread_start(busy_timer);

    /* Before the canvas and the HTTP API: both open the loader record to weigh
     * the foreground app's priority against an incoming draw. */
    FuriThread* loader = furi_thread_alloc_ex("loader", 8 * 1024, loader_srv, NULL);
    furi_thread_start(loader);

    if(config.api_port > 0) {
        if(!web_api_host_init((uint16_t)config.api_port, config.listen_address)) {
            sim_window_request_quit();
            return -1;
        }

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

    FuriThread* screenshot =
        furi_thread_alloc_ex("screenshot", 8 * 1024, simulator_screenshot_thread, NULL);
    furi_thread_start(screenshot);

    if(config.control_path) sim_control_init(config.control_path);

    if(config.exit_after_frames) {
        FuriThread* capture =
            furi_thread_alloc_ex("capture", 8 * 1024, simulator_capture_thread, NULL);
        furi_thread_start(capture);
    }

    /* furi hands a finished task to this reaper rather than deleting it on its
     * own stack. It also delivers FuriThreadStateStopped, which is how the
     * loader learns an app exited. This never returns. */
    furi_thread_set_current_priority(FuriThreadPriorityHighest);
    furi_background();

    return 0;
}

static void* simulator_scheduler_thread(void* arg) {
    UNUSED(arg);

    furi_init();

    /* Nothing registers a log sink on the host — the firmware's goes to the
     * debug UART — so furi's logging would otherwise be silent. */
    furi_log_add_handler((FuriLogHandler){.callback = simulator_log_callback, .context = NULL});
    furi_log_set_level(config.log_level);

    log_storage_on_system_start();

    FuriThread* init =
        furi_thread_alloc_ex("sim_init", 32 * 1024, simulator_init_thread, NULL);
    /* Finish establishing all services before any child task is allowed to
     * run, then lower into furi_background() at the end of init. */
    furi_thread_set_priority(init, FuriThreadPriorityHighest);
    furi_thread_start(init);

    vTaskStartScheduler();

    return NULL;
}

static void simulator_block_signals(void) {
    /* The port resumes tasks with SIGUSR1 and ticks them with SIGALRM. A
     * thread that is not a FreeRTOS task must handle neither: the tick handler
     * calls vTaskSwitchContext() and then suspends the thread it ran on, so a
     * tick caught by the SDL or AppKit threads parks them on a task's condvar
     * and deadlocks the scheduler. Threads inherit this mask, which is why it
     * is installed before anything else starts one. Termination signals stay
     * through so the SDL main loop can shut down gracefully. */
    sigset_t signals;
    sigfillset(&signals);
    sigdelset(&signals, SIGINT);
    sigdelset(&signals, SIGTERM);
    pthread_sigmask(SIG_SETMASK, &signals, NULL);
}

static void simulator_print_usage(const char* argv0) {
    printf(
        "Usage: %s [options]\n"
        "\n"
        "  -s, --scale N     pixels per front LED (default: fills the screen)\n"
        "      --scene NAME  app to boot into, or \"demo\" for the widget demo.\n"
        "                    By default the mode selector decides, as on the device.\n"
        "      --frames N    present N rendered frames after startup, then exit\n"
        "      --keys LIST   replay buttons before exiting, e.g. \"down,ok\"\n"
        "      --list-apps   list every app that can be given to --scene\n"
        "      --screenshot DIR  where captures go. Written on exit with --frames,\n"
        "                    and any time F12 or SIGUSR2 arrives (default: .)\n"
        "      --api-port N  serve the device HTTP API on N (default 8042, 0 disables)\n"
        "      --listen ADDR bind the API to ADDR (default: 127.0.0.1)\n"
        "      --network     bind to all IPv4 interfaces and enable mDNS\n"
        "      --mdns        announce the simulator on the local network\n"
        "      --no-mdns     disable mDNS (accepted for compatibility; the default)\n"
        "      --state-dir DIR  writable state overlay (default: fresh temporary dir)\n"
        "      --headless    use SDL's software-only dummy video driver\n"
        "      --control PATH  serve the tools' control socket, which is what\n"
        "                    records the window; see simctl.py record\n"
        "  -h, --help        this message\n"
        "\n"
        "Environment:\n"
        "  BUSYBAR_SIM_ASSETS  immutable asset directory served as LVGL drive 'C'\n"
        "  BUSYBAR_SIM_STATE   writable state overlay (overridden by --state-dir)\n"
        "\n"
        "Keys: arrows = dial, Enter = Ok, Space/s = Start, Esc = Back\n"
        "Mode selector: b = Busy, c = Custom, o = Off, a = Apps, ',' = Settings\n",
        argv0);
}

static bool simulator_parse_number(
    const char* option,
    const char* value,
    int minimum,
    int maximum,
    int* output) {
    char* end = NULL;
    errno = 0;
    const long parsed = strtol(value, &end, 10);

    if(errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
        fprintf(
            stderr,
            "%s expects a whole number from %d through %d, got %s\n",
            option,
            minimum,
            maximum,
            value);
        return false;
    }

    *output = (int)parsed;
    return true;
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
        {"listen", required_argument, NULL, 'i'},
        {"network", no_argument, NULL, 'N'},
        {"mdns", no_argument, NULL, 'd'},
        {"no-mdns", no_argument, NULL, 'm'},
        {"state-dir", required_argument, NULL, 't'},
        {"headless", no_argument, NULL, 'H'},
        {"control", required_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while((opt = getopt_long(argc, argv, "s:hv", options, NULL)) != -1) {
        switch(opt) {
        case 's':
            if(!simulator_parse_number("--scale", optarg, 1, 1000, &config.scale)) return 1;
            break;
        case 'n':
            config.scene = optarg;
            break;
        case 'p':
            config.screenshot_dir = optarg;
            break;
        case 'f':
            if(!simulator_parse_number(
                   "--frames",
                   optarg,
                   1,
                   1000000,
                   &config.exit_after_frames))
                return 1;
            break;
        case 'k':
            config.keys = optarg;
            break;
        case 'v':
            config.log_level = FuriLogLevelTrace;
            break;
        case 'a':
            if(!simulator_parse_number("--api-port", optarg, 0, 65535, &config.api_port))
                return 1;
            break;
        case 'i':
            config.listen_address = optarg;
            break;
        case 'N':
            config.listen_address = "0.0.0.0";
            config.announce = true;
            break;
        case 'd':
            config.announce = true;
            break;
        case 'm':
            config.announce = false;
            break;
        case 't':
            config.state_dir = optarg;
            break;
        case 'H':
            config.headless = true;
            break;
        case 'c':
            config.control_path = optarg;
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

    if(optind != argc) {
        fprintf(stderr, "unexpected positional argument: %s\n", argv[optind]);
        simulator_print_usage(argv[0]);
        return 1;
    }

    /* Before SDL_Init, so that every thread SDL and AppKit create inherits the
     * mask. A thread that has SIGALRM unblocked can be handed the FreeRTOS
     * tick, and the tick handler switches context — on a thread the scheduler
     * knows nothing about. */
    simulator_block_signals();

    if(config.headless && setenv("SDL_VIDEODRIVER", "dummy", 1) != 0) {
        fprintf(stderr, "failed to enable SDL's dummy video driver: %s\n", strerror(errno));
        return 1;
    }

    if(!sim_window_init(config.scale)) {
        return 1;
    }

    sim_window_request_selftest();

    simulator_install_screenshot_signal();
    simulator_install_terminate_signals();

    pthread_t scheduler;
    if(pthread_create(&scheduler, NULL, simulator_scheduler_thread, NULL) != 0) {
        fprintf(stderr, "failed to start the scheduler thread\n");
        sim_window_deinit();
        return 1;
    }

    while(sim_window_pump()) {
        if(simulator_terminate_signalled) sim_window_request_quit();
        usleep(FRAME_DELAY_US);
    }

    sim_window_deinit();

    /* The scheduler thread owns FreeRTOS state that has no clean teardown
     * path here; the process is going away regardless. */
    _exit(0);
}
