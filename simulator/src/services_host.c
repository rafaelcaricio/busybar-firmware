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
#include <time/settings/settings_i.h>
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
static FuriMutex* time_settings_mutex;
static TimeSettings time_settings;

static void services_host_time_settings_init(void) {
    time_settings_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    time_settings_state = furi_state_alloc(sizeof(TimeSettings));

    if(!time_settings_load(&time_settings) && !time_settings_reset(&time_settings)) {
        FURI_LOG_W(TAG, "could not load or reset time settings; using UTC/24h");
        memset(&time_settings, 0, sizeof(time_settings));
        time_settings.time_format = TimeSettingTimeFormat24h;
        time_settings.timezone = utz_zone_default;
    }
    furi_state_set(time_settings_state, &time_settings);
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

    furi_check(furi_mutex_acquire(time_settings_mutex, FuriWaitForever) == FuriStatusOk);
    *settings = time_settings;
    furi_check(furi_mutex_release(time_settings_mutex) == FuriStatusOk);
}

bool time_set_settings(Time* instance, const TimeSettings* settings) {
    UNUSED(instance);
    furi_check(settings);

    if(!time_settings_save(settings)) return false;

    furi_check(furi_mutex_acquire(time_settings_mutex, FuriWaitForever) == FuriStatusOk);
    time_settings = *settings;
    furi_check(furi_mutex_release(time_settings_mutex) == FuriStatusOk);
    furi_state_set(time_settings_state, settings);
    return true;
}

FuriState* time_get_settings_state(Time* instance) {
    UNUSED(instance);
    return time_settings_state;
}

time_t time_get_timestamp_ms(void) {
    return furi_hal_rtc_get_timestamp_ms();
}

time_t time_get_timestamp(void) {
    return furi_hal_rtc_get_timestamp();
}

LocalTime time_get_local_time(Time* instance) {
    UNUSED(instance);

    TimeSettings settings;
    time_get_settings(instance, &settings);

    DateTimeMs utc = furi_hal_rtc_get_datetime();
    utz_offset_t offset;
    utz_get_current_offset(&settings.timezone, &utc.dt, &offset);

    return (LocalTime){
        .dt = utz_udatetime_add(&utc.dt, &offset),
        .offset = offset,
    };
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
