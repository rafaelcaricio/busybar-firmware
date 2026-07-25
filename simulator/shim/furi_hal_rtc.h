/**
 * Host stand-in for the target furi_hal_rtc API.
 *
 * Backed by the workstation clock, so clock faces and countdowns in the
 * simulator show real local time. A set_datetime call is applied as an offset
 * rather than touching the system clock.
 */
#pragma once

#include <datetime/datetime.h>

#ifdef __cplusplus
extern "C" {
#endif

DateTimeMs furi_hal_rtc_get_datetime(void);

void furi_hal_rtc_set_datetime(const DateTimeMs* datetime);

time_t furi_hal_rtc_get_timestamp(void);

time_t furi_hal_rtc_get_timestamp_ms(void);

#ifdef __cplusplus
}
#endif
