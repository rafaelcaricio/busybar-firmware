/**
 * Host stand-in for the target furi_hal umbrella header.
 *
 * Only the pieces furi core actually reaches for are present; anything
 * peripheral-facing (radio, flash, power, displays) is deliberately absent so
 * that a service pulled into the simulator fails loudly at compile time
 * instead of silently linking against a stub.
 */
#pragma once

/* furi core sources include <furi_hal.h> and rely on it dragging in furi
 * itself, the way the on-target umbrella header does. */
#include <furi.h>

#include "furi_hal_cpu.h"
#include "furi_hal_interrupt.h"
#include "furi_hal_memory.h"
#include "furi_hal_power.h"
#include "furi_hal_rtc.h"
