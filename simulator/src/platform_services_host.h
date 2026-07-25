#pragma once

/** Register the inert stand-ins for the radios, audio, updater and LED
 * services. Call after furi_init() and before any app starts. */
void platform_services_host_init(void);
