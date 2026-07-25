/**
 * Host stand-in for the target furi_hal_resources.h.
 *
 * The real header describes every GPIO on the board. The UI layer only needs
 * the button enumeration, so that is all the simulator declares — keeping the
 * pin table out avoids pulling furi_hal_gpio and the STM32 LL headers in.
 *
 * Kept in sync manually with targets/f21/furi_hal/furi_hal_resources.h.
 */
#pragma once

#include <furi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    InputKeyUp,
    InputKeyDown,
    InputKeyRight,
    InputKeyLeft,
    InputKeyOk,
    InputKeyBack,
    InputKeyStart,
    InputKeyBusy,
    InputKeyCustom,
    InputKeyOff,
    InputKeyApps,
    InputKeySettings,
    InputKeyMAX, /**< Special value, don't use it */
} InputKey;

#ifdef __cplusplus
}
#endif
