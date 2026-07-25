/**
 * Host stand-in for the target furi_hal_cpu API.
 *
 * Only furi_delay_us() uses this, as a cycle-counted busy wait. Backing it
 * with the monotonic clock keeps the delay honest without a DWT unit.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FURI_HAL_CPU_CYCLES_PER_US (1000U)

uint32_t furi_hal_cpu_get_cycle_count(void);

uint32_t furi_hal_cpu_get_cycles_per_us(void);

#ifdef __cplusplus
}
#endif
