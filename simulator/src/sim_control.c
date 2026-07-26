#include "sim_control.h"
#include "platform_services_host.h"
#include "sim_recorder.h"
#include "sim_window.h"

#include <furi.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TAG "SimControl"

#define SIM_CONTROL_LINE    (1024)
/* A client that has connected but sent nothing is a bug in the tool, not a
 * reason to stop answering the next one. */
#define SIM_CONTROL_READ_MS (2000)
#define SIM_CONTROL_IDLE_MS (20)

static struct {
    int listener;
    char path[108];
    FuriThread* thread;
} control;

/** Read one newline-terminated request. False if the client gave up on us. */
static bool sim_control_read_line(int client, char* line, size_t size) {
    size_t used = 0;

    for(int waited = 0; waited < SIM_CONTROL_READ_MS; waited += SIM_CONTROL_IDLE_MS) {
        const ssize_t got = recv(client, line + used, size - used - 1, 0);

        if(got > 0) {
            used += (size_t)got;
            line[used] = '\0';

            char* end = strchr(line, '\n');
            if(end) {
                *end = '\0';
                return true;
            }

            if(used + 1 >= size) return false;
            continue;
        }

        if(got == 0) return false;
        if(errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;

        furi_delay_ms(SIM_CONTROL_IDLE_MS);
    }

    return false;
}

static void sim_control_reply(int client, const char* reply) {
    const size_t length = strlen(reply);

    send(client, reply, length, 0);
    send(client, "\n", 1, 0);
}

/** "record start FPS DIVISOR PATH", where PATH is the rest of the line. */
static void sim_control_record_start(int client, char* arguments) {
    int fps = 0, divisor = 0, consumed = 0;

    if(sscanf(arguments, "%d %d %n", &fps, &divisor, &consumed) < 2 || consumed <= 0 ||
       arguments[consumed] == '\0') {
        sim_control_reply(client, "error usage: record start FPS DIVISOR PATH");
        return;
    }

    char error[256] = "";
    if(!sim_recorder_start(arguments + consumed, fps, divisor, error, sizeof(error))) {
        char reply[320];
        snprintf(reply, sizeof(reply), "error %s", error);
        sim_control_reply(client, reply);
        return;
    }

    SimRecorderStatus status;
    sim_recorder_status(&status);

    char reply[128];
    snprintf(reply, sizeof(reply), "ok %d %d %d", status.width, status.height, status.fps);
    sim_control_reply(client, reply);
}

static void sim_control_record_stop(int client) {
    SimRecorderStatus status;
    char error[256] = "";

    if(!sim_recorder_stop(&status, error, sizeof(error))) {
        char reply[320];
        snprintf(reply, sizeof(reply), "error %s", error);
        sim_control_reply(client, reply);
        return;
    }

    char reply[128];
    snprintf(
        reply,
        sizeof(reply),
        "ok %u %u %d %d %d %u",
        status.frames,
        status.dropped,
        status.width,
        status.height,
        status.fps,
        status.elapsed_ms);
    sim_control_reply(client, reply);
}

static void sim_control_record_status(int client) {
    SimRecorderStatus status;
    sim_recorder_status(&status);

    char reply[128];
    snprintf(
        reply,
        sizeof(reply),
        "ok %d %u %u %d %d %d %u",
        status.running ? 1 : 0,
        status.frames,
        status.dropped,
        status.width,
        status.height,
        status.fps,
        status.elapsed_ms);
    sim_control_reply(client, reply);
}

static void sim_control_status(int client) {
    extern size_t xPortGetFreeHeapSize(void);
    extern size_t xPortGetMinimumEverFreeHeapSize(void);

    SimWindowStatus status;
    sim_window_status(&status);

    char reply[256];
    snprintf(
        reply,
        sizeof(reply),
        "ok %llu %llu %llu %d %d %d %zu %zu",
        (unsigned long long)status.frames,
        (unsigned long long)status.front_updates,
        (unsigned long long)status.back_updates,
        status.canvas_width,
        status.canvas_height,
        status.quitting ? 1 : 0,
        xPortGetFreeHeapSize(),
        xPortGetMinimumEverFreeHeapSize());
    sim_control_reply(client, reply);
}

static void sim_control_power_status(int client) {
    uint8_t charge = 0;
    bool usb = false;
    bool charging = false;
    platform_services_host_get_power(&charge, &usb, &charging);

    char reply[64];
    snprintf(
        reply,
        sizeof(reply),
        "ok %u %d %d",
        charge,
        usb ? 1 : 0,
        charging ? 1 : 0);
    sim_control_reply(client, reply);
}

static void sim_control_power_set(int client, const char* arguments) {
    unsigned charge = 0, usb = 0, charging = 0;
    char extra = '\0';
    if(sscanf(arguments, "%u %u %u %c", &charge, &usb, &charging, &extra) != 3 ||
       charge > 100 || usb > 1 || charging > 1 || (!usb && charging)) {
        sim_control_reply(
            client,
            "error usage: power CHARGE USB CHARGING (charge 0..100; flags 0 or 1)");
        return;
    }

    platform_services_host_set_power((uint8_t)charge, usb != 0, charging != 0);
    sim_control_power_status(client);
}

static void sim_control_wait_frame(int client, const char* arguments) {
    unsigned long long target = 0;
    unsigned timeout_ms = 0;
    char extra = '\0';

    if(sscanf(arguments, "%llu %u %c", &target, &timeout_ms, &extra) != 2 ||
       timeout_ms == 0 || timeout_ms > 600000) {
        sim_control_reply(client, "error usage: wait frame TARGET TIMEOUT_MS (1..600000)");
        return;
    }

    if(!sim_window_wait_for_frame((uint64_t)target, timeout_ms)) {
        SimWindowStatus status;
        sim_window_status(&status);

        char reply[160];
        snprintf(
            reply,
            sizeof(reply),
            "error timed out at frame %llu waiting for %llu",
            (unsigned long long)status.frames,
            target);
        sim_control_reply(client, reply);
        return;
    }

    SimWindowStatus status;
    sim_window_status(&status);
    char reply[96];
    snprintf(reply, sizeof(reply), "ok %llu", (unsigned long long)status.frames);
    sim_control_reply(client, reply);
}

/** "screenshot DIRECTORY", where DIRECTORY is the rest of the line. */
static void sim_control_screenshot(int client, const char* directory) {
    if(!*directory) {
        sim_control_reply(client, "error usage: screenshot DIRECTORY");
        return;
    }

    unsigned sequence = 0;
    uint64_t frame = 0;
    if(!sim_window_screenshot_next(directory, &sequence, &frame)) {
        sim_control_reply(client, "error screenshot failed; see simulator log");
        return;
    }

    char reply[96];
    snprintf(
        reply,
        sizeof(reply),
        "ok %u %llu",
        sequence,
        (unsigned long long)frame);
    sim_control_reply(client, reply);
}

static void sim_control_dispatch(int client, char* line) {
    if(strcmp(line, "ping") == 0) {
        sim_control_reply(client, "ok");

    } else if(strcmp(line, "status") == 0) {
        sim_control_status(client);

    } else if(strcmp(line, "power") == 0) {
        sim_control_power_status(client);

    } else if(strncmp(line, "power ", 6) == 0) {
        sim_control_power_set(client, line + 6);

    } else if(strncmp(line, "wait frame ", 11) == 0) {
        sim_control_wait_frame(client, line + 11);

    } else if(strncmp(line, "screenshot ", 11) == 0) {
        sim_control_screenshot(client, line + 11);

    } else if(strcmp(line, "quit") == 0) {
        sim_control_reply(client, "ok");
        sim_window_request_quit();

    } else if(strncmp(line, "record start ", 13) == 0) {
        sim_control_record_start(client, line + 13);

    } else if(strcmp(line, "record stop") == 0) {
        sim_control_record_stop(client);

    } else if(strcmp(line, "record status") == 0) {
        sim_control_record_status(client);

    } else {
        sim_control_reply(client, "error unknown request");
    }
}

/** Take one client at a time: the tools are serial and a request is a line.
 *
 * The listener is non-blocking and the task sleeps between polls rather than
 * parking in accept(), so it stays somewhere the FreeRTOS port can suspend it.
 */
static int32_t sim_control_thread(void* context) {
    UNUSED(context);

    while(true) {
        const int client = accept(control.listener, NULL, NULL);

        if(client < 0) {
            if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                furi_delay_ms(SIM_CONTROL_IDLE_MS);
                continue;
            }

            FURI_LOG_E(TAG, "accept failed: %s", strerror(errno));
            break;
        }

        const int flags = fcntl(client, F_GETFL, 0);
        fcntl(client, F_SETFL, flags | O_NONBLOCK);

        char line[SIM_CONTROL_LINE];
        if(sim_control_read_line(client, line, sizeof(line))) {
            sim_control_dispatch(client, line);
        }

        close(client);
    }

    return 0;
}

bool sim_control_init(const char* path) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};

    if(strlen(path) >= sizeof(address.sun_path)) {
        FURI_LOG_E(TAG, "socket path is longer than %zu bytes", sizeof(address.sun_path) - 1);
        return false;
    }

    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    strncpy(control.path, path, sizeof(control.path) - 1);

    /* A socket left behind by a simulator that was killed rather than stopped
     * would make bind() fail; nothing else owns this name. */
    unlink(path);

    control.listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if(control.listener < 0) {
        FURI_LOG_E(TAG, "socket failed: %s", strerror(errno));
        return false;
    }

    if(bind(control.listener, (struct sockaddr*)&address, sizeof(address)) != 0) {
        FURI_LOG_E(TAG, "cannot bind %s: %s", path, strerror(errno));
        close(control.listener);
        control.listener = -1;
        return false;
    }

    if(listen(control.listener, 4) != 0) {
        FURI_LOG_E(TAG, "cannot listen on %s: %s", path, strerror(errno));
        close(control.listener);
        control.listener = -1;
        return false;
    }

    const int flags = fcntl(control.listener, F_GETFL, 0);
    fcntl(control.listener, F_SETFL, flags | O_NONBLOCK);

    control.thread = furi_thread_alloc_ex("sim_control", 8 * 1024, sim_control_thread, NULL);
    furi_thread_start(control.thread);

    FURI_LOG_I(TAG, "control socket on %s", path);

    return true;
}
