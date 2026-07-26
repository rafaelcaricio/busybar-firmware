#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Register the host stand-ins for radios, audio, updater, power and LEDs.
 * Call after furi_init() and before any app starts. */
void platform_services_host_init(void);

/** Change the simulated battery and publish the same power events as hardware.
 * Charging is forced off when USB is disconnected. */
void platform_services_host_set_power(uint8_t charge, bool usb_connected, bool charging);

void platform_services_host_get_power(
    uint8_t* charge,
    bool* usb_connected,
    bool* charging);
