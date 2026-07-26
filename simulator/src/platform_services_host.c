/**
 * Stubs for the platform services the simulator has no way to provide.
 *
 * Radios (Wi-Fi, BLE, Matter, MQTT), audio, the LED drivers, the OTA updater
 * and the version/OTP HAL all talk to hardware or to the network. None of that
 * exists on a workstation, so each record here is an inert object of the right
 * shape: the UI can open it, subscribe to it and read from it, and it always
 * reports the same disconnected, idle, factory state.
 *
 * The point is to let the screens be looked at. A settings app will draw its
 * "not connected" and "off" branches correctly and navigate normally; it will
 * never show a connected one, because nothing here ever changes state. Calls
 * that would act on hardware are accepted and logged rather than ignored
 * silently, so it is obvious from the log when a screen tried to do something
 * real.
 *
 * FuriPubSub and FuriState objects are genuine, not NULL: apps subscribe to
 * them during alloc and would crash on a NULL handle. They simply never
 * publish.
 */
#include "platform_services_host.h"

#include "audio_host.h"

#include <furi.h>

#include <string.h>

#include <audio/audio.h>
#include <ble/ble.h>
#include <device_name/device_name.h>
#include <matter/matter.h>
#include <mqtt/mqtt.h>
#include <power/power_service/power.h>
#include <sl_info/sl_info.h>
#include <status_lights/status_lights.h>
#include <sysctl/sysctl.h>
#include <time/time.h>
#include <wifi/wifi.h>

#include <updater/updater.h>

#include <furi_hal_nvm.h>
#include <furi_hal_version.h>

#include <toolbox/update_lib/factory_reset.h>

#include <version/version.h>
#include <version/version.inc.h>

#define TAG "PlatformHost"

/* One instance per record. The address is what matters — nothing dereferences
 * these — but each needs to be distinct so a record cannot be confused for
 * another. */
static struct {
    int audio;
    int ble;
    int device_name;
    int matter;
    int mqtt;
    int sl_info;
    int status_lights;
    int wifi;

    FuriPubSub* audio_events;
    FuriPubSub* ble_events;
    FuriPubSub* device_name_events;
    FuriPubSub* matter_events;
    FuriPubSub* mqtt_events;
    FuriPubSub* power_events;
    FuriMutex* power_mutex;

    FuriState* wifi_state;
    FuriState* updater_check_state;
    FuriState* updater_update_state;
    FuriState* updater_settings_state;
    FuriState* matter_switch_state;

    float volume;
    char device_name_value[DEVICE_NAME_MAX_SIZE];
    MqttConfig mqtt_config;
    PowerInfo power_info;
    bool usb_connected;
} platform;

void platform_services_host_init(void) {
    platform.audio_events = furi_pubsub_alloc();
    platform.ble_events = furi_pubsub_alloc();
    platform.device_name_events = furi_pubsub_alloc();
    platform.matter_events = furi_pubsub_alloc();
    platform.mqtt_events = furi_pubsub_alloc();
    platform.power_events = furi_pubsub_alloc();
    platform.power_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* Item sizes match the real services, so a subscriber that reads the
     * published item sees a correctly sized zeroed struct. */
    platform.wifi_state = furi_state_alloc(sizeof(WifiInfo));
    platform.updater_check_state = furi_state_alloc(sizeof(UpdaterCheckState));
    platform.updater_update_state = furi_state_alloc(sizeof(UpdaterUpdateState));
    platform.updater_settings_state = furi_state_alloc(sizeof(UpdaterSettings));
    platform.matter_switch_state = furi_state_alloc(sizeof(MatterSwitchState));

    const WifiInfo wifi_info = {0};
    /* Not {0}: UpdaterCheckResultAvailable is the zero value, which would
     * claim an update is waiting and send the firmware settings app straight
     * into its update dialog. */
    const UpdaterCheckState updater_check_state = {.result = UpdaterCheckResultNone};
    furi_state_set(platform.wifi_state, &wifi_info);
    furi_state_set(platform.updater_check_state, &updater_check_state);

    const UpdaterUpdateState updater_update_state = {
        .event = UpdaterUpdateEventActionDone,
        .action = UpdaterUpdateActionNone,
        .status = UpdaterStatusOk,
    };
    furi_state_set(platform.updater_update_state, &updater_update_state);

    /* Autoupdates are off here, so the settings a subscriber sees say so
     * rather than describing a channel this build never checks. */
    const UpdaterSettings updater_settings = {0};
    furi_state_set(platform.updater_settings_state, &updater_settings);

    const MatterSwitchState matter_switch_state = {0};
    furi_state_set(platform.matter_switch_state, &matter_switch_state);

    platform.volume = 0.5f;
    snprintf(platform.device_name_value, sizeof(platform.device_name_value), "BUSY Simulator");
    platform_services_host_set_power(100, true, true);

    furi_record_create(RECORD_AUDIO, &platform.audio);
    furi_record_create(RECORD_BLE, &platform.ble);
    furi_record_create(RECORD_DEVICE_NAME, &platform.device_name);
    furi_record_create(RECORD_MATTER, &platform.matter);
    furi_record_create(RECORD_MQTT, &platform.mqtt);
    furi_record_create(RECORD_SL_INFO, &platform.sl_info);
    furi_record_create(RECORD_STATUS_LIGHTS, &platform.status_lights);
    furi_record_create(RECORD_WIFI, &platform.wifi);
}

/* --- audio ------------------------------------------------------------- */

void audio_enable(Audio* instance) {
    UNUSED(instance);
}

/* Not a stop. These two bracket the amplifier, not the playback: the real
 * service keeps the amp on until the file finishes, and /api/audio/play calls
 * disable() the moment it has queued a sound. Stopping here cut every
 * API-played sound off before it was heard. */
void audio_disable(Audio* instance) {
    UNUSED(instance);
}

FuriPubSub* audio_get_pubsub(Audio* audio) {
    UNUSED(audio);
    return platform.audio_events;
}

float audio_get_volume(Audio* instance) {
    UNUSED(instance);
    return platform.volume;
}

void audio_set_volume(Audio* instance, float volume) {
    UNUSED(instance);
    platform.volume = volume;
    audio_host_set_volume(volume);
}

bool audio_play_file(Audio* instance, const char* file_name) {
    UNUSED(instance);
    /* Real output: src/audio_host.c queues the .snd on the workstation's
     * sound device. */
    return audio_host_play_file(file_name);
}

bool audio_stop(Audio* instance) {
    UNUSED(instance);
    audio_host_stop();
    return true;
}

/* --- ble --------------------------------------------------------------- */

FuriPubSub* ble_get_pubsub(Ble* ble) {
    UNUSED(ble);
    return platform.ble_events;
}

bool ble_get_state(Ble* ble, BleState* const output) {
    UNUSED(ble);
    if(output) memset(output, 0, sizeof(*output));
    return true;
}

bool ble_start(Ble* ble) {
    UNUSED(ble);
    /* Reports success: the pairing screen asserts on it, and a radio that
     * advertises to nobody is the closest honest analogue. */
    FURI_LOG_I(TAG, "ble_start: simulated, no radio");
    return true;
}

bool ble_stop(Ble* ble) {
    UNUSED(ble);
    return true;
}

bool ble_forget(Ble* ble) {
    UNUSED(ble);
    return true;
}

/* --- device name ------------------------------------------------------- */

void device_name_get(DeviceName* instance, FuriString* name) {
    UNUSED(instance);
    if(name) furi_string_set(name, platform.device_name_value);
}

/* Held in memory only: the device writes the name to its settings partition,
 * which has no equivalent here, so a rename lasts until the simulator exits.
 *
 * The rules mirror device_name_validate() in the real service, whose
 * translation unit also carries the service loop and its storage. */
DeviceNameError device_name_set(DeviceName* instance, const FuriString* name) {
    UNUSED(instance);
    furi_check(name);

    const char* value = furi_string_get_cstr(name);
    const size_t length = strnlen(value, DEVICE_NAME_MAX_SIZE);

    if(length == 0) return DeviceNameErrorEmpty;
    if(length > DEVICE_NAME_MAX_LENGTH) return DeviceNameErrorTooLong;

    static const char* const allowed_special_chars = " !()-_=+;:,.?'|@#$%^&*[]{}/\\\"<>";
    bool only_spaces = true;

    for(size_t i = 0; i < length; i++) {
        const char c = value[i];
        if(c != ' ') only_spaces = false;
        if(!isalnum((unsigned char)c) && !strchr(allowed_special_chars, c)) {
            return DeviceNameErrorIllegalChar;
        }
    }

    if(only_spaces) return DeviceNameErrorOnlySpaces;

    snprintf(platform.device_name_value, sizeof(platform.device_name_value), "%s", value);

    const DeviceNameEvent event = {
        .type = DeviceNameEventTypeNameChanged,
        .name_changed = {.name = platform.device_name_value},
    };
    furi_pubsub_publish(platform.device_name_events, (void*)&event);

    return DeviceNameErrorNone;
}

FuriPubSub* device_name_get_pubsub(DeviceName* instance) {
    UNUSED(instance);
    return platform.device_name_events;
}

/* --- matter ------------------------------------------------------------ */

FuriPubSub* matter_get_pubsub(Matter* instance) {
    UNUSED(instance);
    return platform.matter_events;
}

MatterStatus matter_enable_commissioning(Matter* instance, MatterCommissioningInfo* info) {
    UNUSED(instance);
    if(info) memset(info, 0, sizeof(*info));
    FURI_LOG_I(TAG, "matter_enable_commissioning: no Matter stack");
    return MatterStatusOk;
}

MatterStatus matter_get_commissioned_fabrics(Matter* instance, MatterCommissionedFabrics* fabrics) {
    UNUSED(instance);
    if(fabrics) memset(fabrics, 0, sizeof(*fabrics));
    return MatterStatusOk;
}

FuriState* matter_get_switch_state(Matter* instance) {
    UNUSED(instance);
    return platform.matter_switch_state;
}

MatterStatus matter_set_switch_state(Matter* instance, MatterSwitchState switch_state) {
    UNUSED(instance);
    furi_state_set(platform.matter_switch_state, &switch_state);
    return MatterStatusOk;
}

MatterStatus matter_set_switch_startup_mode(Matter* instance, MatterSwitchStartupMode mode) {
    UNUSED(instance);
    FURI_LOG_I(TAG, "matter_set_switch_startup_mode(%d): no Matter stack", mode);
    return MatterStatusOk;
}

MatterStatus matter_factory_reset(Matter* instance, MatterReboot reboot) {
    UNUSED(instance);
    UNUSED(reboot);
    FURI_LOG_I(TAG, "matter_factory_reset: nothing to reset");
    return MatterStatusOk;
}

/* --- mqtt -------------------------------------------------------------- */

/* The broker settings are the one part of MQTT that is real: /api/account
 * writes them and reads them back, which is what pairing a device does before
 * anything connects. Nothing acts on them here. */

void mqtt_get_config(Mqtt* instance, MqttConfig* config) {
    UNUSED(instance);
    if(config) *config = platform.mqtt_config;
}

bool mqtt_set_config(Mqtt* instance, const MqttConfig* config) {
    UNUSED(instance);
    if(!config) return false;

    platform.mqtt_config = *config;
    return true;
}

bool mqtt_publish(
    Mqtt* instance,
    MqttQos qos,
    const char* topic,
    const void* data,
    size_t data_size) {
    UNUSED(instance);
    UNUSED(qos);
    UNUSED(data);
    FURI_LOG_D(TAG, "mqtt_publish(%s, %zu bytes): no broker", topic, data_size);
    return false;
}

MqttSubscription* mqtt_subscribe(
    Mqtt* instance,
    MqttQos qos,
    const char* topic,
    MqttSubscriptionCallback callback,
    void* context) {
    UNUSED(instance);
    UNUSED(qos);
    UNUSED(callback);
    UNUSED(context);
    /* NULL is what the real service returns on failure, and subscribers check
     * it. Nothing would ever arrive on the topic anyway. */
    FURI_LOG_D(TAG, "mqtt_subscribe(%s): no broker", topic);
    return NULL;
}

const void* mqtt_message_get_data(const MqttMessage* message, size_t* data_size) {
    UNUSED(message);
    if(data_size) *data_size = 0;
    return NULL;
}

FuriPubSub* mqtt_get_pubsub(Mqtt* instance) {
    UNUSED(instance);
    return platform.mqtt_events;
}

MqttStatus mqtt_get_status(Mqtt* instance) {
    UNUSED(instance);
    return MqttStatusNotConnected;
}

void mqtt_get_session_info(Mqtt* instance, MqttSessionInfo* info) {
    UNUSED(instance);
    if(!info) return;

    /* The strings belong to the caller, which allocates the ones it wants
     * filled and leaves the rest NULL. Overwriting the pointers instead of
     * their contents would leak them and hand back dangling ones. */
    if(info->session_id) furi_string_reset(info->session_id);
    if(info->email) furi_string_reset(info->email);
    if(info->user_id) furi_string_reset(info->user_id);
    info->is_valid = false;
}

bool mqtt_request_link_pin(Mqtt* instance) {
    UNUSED(instance);
    FURI_LOG_I(TAG, "mqtt_request_link_pin: no broker");
    return false;
}

void mqtt_unlink(Mqtt* instance) {
    UNUSED(instance);
}

/* --- power ------------------------------------------------------------- */

void platform_services_host_set_power(uint8_t charge, bool usb_connected, bool charging) {
    if(charge > 100) charge = 100;
    if(!usb_connected) charging = false;

    furi_check(furi_mutex_acquire(platform.power_mutex, FuriWaitForever) == FuriStatusOk);
    const bool charge_changed = platform.power_info.charge != charge;
    const bool usb_changed = platform.usb_connected != usb_connected;
    const bool charging_changed = platform.power_info.is_charging != charging;

    platform.usb_connected = usb_connected;
    platform.power_info = (PowerInfo){
        .is_charging = charging,
        .is_full_charged = charge == 100,
        .charge_enabled = usb_connected,
        .charge = charge,
        .current_battery = charging ? 500 : (usb_connected ? 0 : -250),
        .current_usb = usb_connected ? 500 : 0,
        .voltage_battery = 3300.0f + (float)charge * 9.0f,
        .voltage_usb = usb_connected ? 5000.0f : 0.0f,
        .temperature_charger = 25.0f,
        .temperature_battery = 25.0f,
        .charge_ilim_usb = 1500,
        .charge_ilim_battery = 1500,
        .charge_level_limit = 100,
    };
    furi_check(furi_mutex_release(platform.power_mutex) == FuriStatusOk);

    if(charge_changed) {
        PowerEvent event = {.type = PowerEventChargeAmountUpdate};
        furi_pubsub_publish(platform.power_events, &event);
    }
    if(usb_changed) {
        PowerEvent event = {.type = PowerEventUsbConnectionStateUpdate};
        furi_pubsub_publish(platform.power_events, &event);
    }
    if(charging_changed) {
        PowerEvent event = {.type = PowerEventChargingStateUpdate};
        furi_pubsub_publish(platform.power_events, &event);
    }
}

void platform_services_host_get_power(
    uint8_t* charge,
    bool* usb_connected,
    bool* charging) {
    furi_check(furi_mutex_acquire(platform.power_mutex, FuriWaitForever) == FuriStatusOk);
    if(charge) *charge = platform.power_info.charge;
    if(usb_connected) *usb_connected = platform.usb_connected;
    if(charging) *charging = platform.power_info.is_charging;
    furi_check(furi_mutex_release(platform.power_mutex) == FuriStatusOk);
}

void power_get_info(Power* power, PowerInfo* info) {
    UNUSED(power);
    if(!info) return;

    furi_check(furi_mutex_acquire(platform.power_mutex, FuriWaitForever) == FuriStatusOk);
    *info = platform.power_info;
    furi_check(furi_mutex_release(platform.power_mutex) == FuriStatusOk);
}

FuriPubSub* power_get_pubsub(Power* power) {
    UNUSED(power);
    return platform.power_events;
}

float power_get_temperature_battery_celsius(float temperature_battery) {
    return temperature_battery;
}

bool power_is_usb_connected(Power* power) {
    UNUSED(power);
    bool connected;
    platform_services_host_get_power(NULL, &connected, NULL);
    return connected;
}

void power_reboot(Power* power, PowerRebootMode mode) {
    UNUSED(power);
    UNUSED(mode);
    FURI_LOG_I(TAG, "power_reboot: ignored");
}

/* --- silabs info / status lights / sysctl ------------------------------ */

SlInfoStatus sl_info_get_value(const SlInfo* instance, const char* key, const char** value) {
    UNUSED(instance);
    UNUSED(key);
    if(value) *value = "";
    return SlInfoStatusOk;
}

StatusLightsStatus
    status_lights_run_preset(StatusLights* instance, StatusLightsPreset preset, Color color) {
    UNUSED(instance);
    UNUSED(preset);
    UNUSED(color);
    return StatusLightsStatusOk;
}

void sysctl_set_debug_enabled(bool enabled) {
    FURI_LOG_I(TAG, "sysctl_set_debug_enabled(%d)", enabled);
}

/* --- updater ----------------------------------------------------------- */

UpdaterStatus updater_check_for_update(Updater* instance) {
    UNUSED(instance);
    FURI_LOG_I(TAG, "updater_check_for_update: no update server");
    return UpdaterStatusOk;
}

const char* updater_get_active_version(void) {
    return version_get_version(NULL);
}

UpdaterStatus updater_get_allowance_status(Updater* instance) {
    UNUSED(instance);
    uint8_t charge;
    bool usb_connected;
    platform_services_host_get_power(&charge, &usb_connected, NULL);
    return (charge >= 40 || usb_connected) ? UpdaterStatusOk : UpdaterStatusBatteryLow;
}

void updater_get_check_info(Updater* instance, UpdateCheckInfo* info) {
    UNUSED(instance);
    if(!info) return;

    /* Caller-owned strings, as in mqtt_get_session_info above. */
    if(info->version) furi_string_reset(info->version);
    if(info->url) furi_string_reset(info->url);
    if(info->id) furi_string_reset(info->id);
    if(info->sha256) furi_string_reset(info->sha256);
    if(info->changelog) furi_string_reset(info->changelog);
}

FuriState* updater_get_check_state(Updater* instance) {
    UNUSED(instance);
    return platform.updater_check_state;
}

FuriState* updater_get_settings_state(Updater* instance) {
    UNUSED(instance);
    return platform.updater_settings_state;
}

void updater_get_settings(const Updater* instance, UpdaterSettings* settings) {
    UNUSED(instance);
    if(settings) memset(settings, 0, sizeof(*settings));
}

bool updater_set_settings(Updater* instance, const UpdaterSettings* settings) {
    UNUSED(instance);
    UNUSED(settings);
    return false;
}

void updater_install_from_url(Updater* instance, const char* url, const char* sha256) {
    UNUSED(instance);
    UNUSED(sha256);
    FURI_LOG_I(TAG, "updater_install_from_url(%s): ignored", url);
}

UpdaterStatus updater_session_start(Updater* instance) {
    UNUSED(instance);
    return UpdaterStatusOk;
}

/* The install path is refused rather than faked. Unpacking a bundle would
 * scatter firmware images through the asset tree, and applying one would mean
 * rebooting into an image this process cannot run. */

void updater_abort_download(Updater* instance) {
    UNUSED(instance);
}

bool updater_get_active_security_flags(uint32_t* flags) {
    if(flags) *flags = 0;
    /* No signed or encrypted co-processor image to describe. */
    return false;
}

FuriState* updater_get_update_state(Updater* instance) {
    UNUSED(instance);
    return platform.updater_update_state;
}

UpdaterStatus updater_unpack(
    Updater* instance,
    const char* tar_path,
    const char* staging_path,
    FuriString* manifest_path,
    bool do_wait) {
    UNUSED(instance);
    UNUSED(tar_path);
    UNUSED(staging_path);
    UNUSED(manifest_path);
    UNUSED(do_wait);

    FURI_LOG_I(TAG, "updater_unpack: refused, no firmware to install");
    return UpdaterStatusUnpackArchiveOpenFailure;
}

UpdaterStatus
    updater_installation_prepare(Updater* instance, const char* manifest_path, bool do_wait) {
    UNUSED(instance);
    UNUSED(manifest_path);
    UNUSED(do_wait);

    return UpdaterStatusInstallationPrepareManifestNotFound;
}

void updater_installation_apply(Updater* instance, bool do_wait) {
    UNUSED(instance);
    UNUSED(do_wait);

    FURI_LOG_I(TAG, "updater_installation_apply: nothing prepared, not rebooting");
}

/* Mirrors the table in applications/system/updater/updater.c, which also holds
 * the service's message loop and so cannot be linked here. The assert below is
 * the same one that file makes: a new status without a string fails the build
 * rather than reaching an API response as garbage. */
static const char* const updater_status_strings[] = {
    [UpdaterStatusOk] = "Success",
    [UpdaterStatusBatteryLow] = "Battery level too low",
    [UpdaterStatusBusy] = "Operation already in progress",

    [UpdaterStatusDownloadFailure] = "Failed to download update bundle",
    [UpdaterStatusDownloadAbort] = "Download aborted",

    [UpdaterStatusShaMismatch] = "SHA256 checksum verification failed",

    [UpdaterStatusUnpackCreateStagingDirectoryFailure] = "Failed to create staging directory",
    [UpdaterStatusUnpackArchiveOpenFailure] = "Failed to open tar file",
    [UpdaterStatusUnpackArchiveUnpackFailure] = "Failed to unpack tar file",

    [UpdaterStatusInstallationPrepareManifestNotFound] = "Manifest not found",
    [UpdaterStatusInstallationPrepareManifestInvalid] = "Failed to validate manifest",
    [UpdaterStatusInstallationPrepareSecurityMismatch] = "Bundle security mismatch",
    [UpdaterStatusInstallationPrepareSessionConfigSetupFailure] = "Failed to save session config",
    [UpdaterStatusInstallationPreparePointerSetupFailure] = "Failed to write pointer file",

    [UpdaterStatusUnknownFailure] = "Unknown error",
};

static_assert(COUNT_OF(updater_status_strings) == UpdaterStatusesCount);

const char* updater_get_status_string(UpdaterStatus status) {
    return (status < UpdaterStatusesCount) ? updater_status_strings[status] :
                                             "Unknown error code";
}

void updater_session_stop(Updater* instance) {
    UNUSED(instance);
}

void factory_reset_perform(Updater* updater, bool shipping_mode) {
    UNUSED(updater);
    UNUSED(shipping_mode);
    FURI_LOG_I(TAG, "factory_reset_perform: nothing to reset");
}

/* --- wifi -------------------------------------------------------------- */

FuriState* wifi_get_state(Wifi* instance) {
    UNUSED(instance);
    return platform.wifi_state;
}

WifiStatus wifi_get_info(Wifi* instance, WifiInfo* info) {
    UNUSED(instance);
    if(info) memset(info, 0, sizeof(*info));
    return WifiStatusOk;
}

WifiStatus wifi_disconnect(Wifi* instance) {
    UNUSED(instance);
    return WifiStatusOk;
}

WifiStatus wifi_forget(Wifi* instance) {
    UNUSED(instance);
    return WifiStatusOk;
}

WifiStatus
    wifi_scan(Wifi* instance, WifiScanResult* results, uint8_t* result_count, uint8_t max_result_count) {
    UNUSED(instance);
    UNUSED(results);
    UNUSED(max_result_count);

    /* An empty scan, not a failure: there is no radio, and a caller that got an
     * error could not tell that apart from a broken one. */
    if(result_count) *result_count = 0;
    return WifiStatusOk;
}

WifiStatus
    wifi_connect(Wifi* instance, const WifiCredentials* credentials, const WifiIpConfig* ip_config) {
    UNUSED(instance);
    UNUSED(credentials);
    UNUSED(ip_config);

    FURI_LOG_I(TAG, "wifi_connect: no radio");
    return WifiStatusAccessPointNotFound;
}

/* --- version and OTP HAL ----------------------------------------------- */

/* lib/version/version.c cannot be compiled for the host: it static_asserts the
 * offsets of this struct against a 32-bit layout, because the bootloader reads
 * it out of flash. The layout is irrelevant here, so the struct -- opaque in
 * version.h -- is defined locally and filled from the same git data fbt uses.
 * CMakeLists.txt generates version.inc.h. */
struct Version {
    const char* git_hash;
    const char* git_branch;
    const char* build_date;
    const char* version;
};

static const Version platform_version = {
    .git_hash = GIT_COMMIT,
    .git_branch = GIT_BRANCH,
    .build_date = BUILD_DATE,
    .version = VERSION BUILD_DIRTY_SUFFIX,
};

const Version* version_get(void) {
    return &platform_version;
}

const char* version_get_githash(const Version* v) {
    return (v ? v : &platform_version)->git_hash;
}

const char* version_get_gitbranch(const Version* v) {
    return (v ? v : &platform_version)->git_branch;
}

const char* version_get_builddate(const Version* v) {
    return (v ? v : &platform_version)->build_date;
}

const char* version_get_version(const Version* v) {
    return (v ? v : &platform_version)->version;
}

uint8_t version_get_target(const Version* v) {
    UNUSED(v);
    return furi_hal_version_get_hw_target();
}

bool version_get_dirty_flag(const Version* v) {
    UNUSED(v);
    return BUILD_DIRTY;
}

/* The device reads these out of one-time-programmable flash. There is no OTP
 * here, so the values describe the simulator rather than a board. */
static const uint8_t platform_uid[8] = {'S', 'I', 'M', 'U', 'L', 'A', 'T', 'E'};
static const uint8_t platform_usb_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

const uint8_t* furi_hal_version_uid(void) {
    return platform_uid;
}

size_t furi_hal_version_uid_size(void) {
    return sizeof(platform_uid);
}

const uint8_t* furi_hal_version_get_usb_mac(void) {
    return platform_usb_mac;
}

const char* furi_hal_version_get_ic_id(void) {
    return "simulator";
}

const char* furi_hal_version_get_fcc_id(void) {
    return "";
}

uint8_t furi_hal_version_get_hw_version(void) {
    return 0;
}

uint8_t furi_hal_version_get_hw_target(void) {
    return 21;
}

uint8_t furi_hal_version_get_hw_body(void) {
    return 0;
}

uint8_t furi_hal_version_get_hw_connect(void) {
    return 0;
}

const char* furi_hal_version_get_name_ptr(void) {
    /* The device returns the name programmed into OTP, or NULL when the board
     * was never named. Nothing is programmed here. */
    return NULL;
}

bool furi_hal_version_is_otp_valid(FuriHalFlashOtpBlock block) {
    UNUSED(block);
    return false;
}

uint32_t furi_hal_version_get_hw_timestamp(void) {
    return 0;
}

bool furi_hal_nvm_is_flag_set(FuriHalNvmFlag flag) {
    /* Debug builds of the firmware set this; the simulator is always a debug
     * build, and the debug app list is worth having. */
    return flag == FuriHalNvmFlagDebug;
}
