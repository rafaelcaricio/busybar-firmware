/**
 * Minimal host stand-ins for the non-UI services that applications open.
 *
 * These exist so a real application can be launched in the simulator without
 * dragging in wifi, networking, Matter or the updater. Each one implements
 * only the calls the UI layer actually makes, and does the least surprising
 * thing: the clock reports workstation time and autoupdates are a no-op.
 *
 * The loader and the desktop are deliberately absent here: both are compiled
 * from applications/services, because between them they own app lifetime and
 * the mode selector, and a stub of either turns the simulator into a
 * single-app viewer.
 *
 * The real time service (applications/services/time) is deliberately not
 * compiled in — it opens the network and wifi records.
 */
#include "services_host.h"
#include "sim_window.h"

#include <furi.h>

#include <utz/utz.h>
#include <furi_hal.h>

#include <time/time.h>
#include <updater/updater.h>
#include <low_power/low_power.h>
#include <power/power_service/power.h>

#define TAG "ServicesHost"

/* None of these carry state; the records only have to be non-NULL. */
static int time_instance;
static int updater_instance;
static int low_power_instance;

/* The state stream reports the clock settings and re-reports them whenever
 * they change. Nothing changes them here, but a subscriber still has to be
 * handed a real object holding the same values time_get_settings() reports. */
static FuriState* time_settings_state;

static void services_host_time_settings_init(void) {
    time_settings_state = furi_state_alloc(sizeof(TimeSettings));

    TimeSettings settings;
    time_get_settings((Time*)&time_instance, &settings);
    furi_state_set(time_settings_state, &settings);
}

void services_host_init(void) {
    services_host_time_settings_init();

    furi_record_create(RECORD_TIME, &time_instance);
    furi_record_create(RECORD_UPDATER, &updater_instance);
    furi_record_create(RECORD_LOW_POWER, &low_power_instance);
}

/* -- time --------------------------------------------------------------- */

void time_get_settings(const Time* instance, TimeSettings* settings) {
    UNUSED(instance);
    furi_check(settings);

    memset(settings, 0, sizeof(*settings));
    settings->time_format = TimeSettingTimeFormat24h;
    /* The timezone carries the abbreviation format string that the settings
     * menu passes straight to snprintf(); a zeroed one is a NULL format. */
    settings->timezone = utz_zone_default;
}

FuriState* time_get_settings_state(Time* instance) {
    UNUSED(instance);
    return time_settings_state;
}

time_t time_get_timestamp_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (time_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

LocalTime time_get_local_time(Time* instance) {
    UNUSED(instance);

    /* furi_hal_rtc already reports workstation local time in the simulator,
     * so the offset here is zero rather than a real timezone shift. */
    const LocalTime local = {
        .dt = furi_hal_rtc_get_datetime().dt,
        .offset = {0},
    };

    return local;
}

/* -- updater ------------------------------------------------------------ */

void updater_pause_autoupdates(Updater* instance) {
    UNUSED(instance);
}

void updater_resume_autoupdates(Updater* instance) {
    UNUSED(instance);
}

/* -- low power ---------------------------------------------------------- */

void low_power_lock(LowPower* instance) {
    UNUSED(instance);
}

void low_power_unlock(LowPower* instance) {
    UNUSED(instance);
}

/* -- power -------------------------------------------------------------- */

bool power_off(Power* power) {
    UNUSED(power);
    FURI_LOG_I(TAG, "power off requested; stopping the simulator");
    sim_window_request_quit();
    return true;
}
