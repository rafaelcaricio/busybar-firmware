/**
 * Host stand-in for the target furi_hal_power API.
 *
 * The real header covers charging, the fuel gauge, reset causes and the
 * low-power modes. Only the insomnia pair is declared here, because the loader
 * brackets every app that asks to stay awake with it. A workstation never
 * sleeps, so on the host these are counters that no one reads.
 *
 * Everything else stays absent on purpose: a service that reaches for the
 * battery or a reset cause should fail to compile rather than link against a
 * stub that invents an answer.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Keep the device awake. Must be paired with furi_hal_power_insomnia_exit(). */
void furi_hal_power_insomnia_enter(void);

/** Release one insomnia hold taken by furi_hal_power_insomnia_enter(). */
void furi_hal_power_insomnia_exit(void);

#ifdef __cplusplus
}
#endif
