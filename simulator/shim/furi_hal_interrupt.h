/**
 * Host stand-in for the target furi_hal_interrupt API.
 *
 * Nothing on the host runs in exception context, so the ISR accounting that
 * feeds furi's thread list is always zero.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* furi_hal_interrupt_get_name(uint8_t exception_number);

uint32_t furi_hal_interrupt_get_time_in_isr_total(void);

#ifdef __cplusplus
}
#endif
