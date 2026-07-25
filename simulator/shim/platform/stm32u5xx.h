/**
 * Stand-in for ST's CMSIS device header.
 *
 * The simulator runs on the host, so there are no memory-mapped peripherals to
 * describe. This exists because a few HAL headers the UI includes for their
 * type definitions — furi_hal_flash_otp.h, reached from furi_hal_version.h —
 * include it unconditionally even though the declarations the UI uses need
 * nothing from it.
 *
 * Deliberately empty: anything that genuinely needs a peripheral definition
 * should fail to compile here rather than link against a fabricated one.
 */
#pragma once
