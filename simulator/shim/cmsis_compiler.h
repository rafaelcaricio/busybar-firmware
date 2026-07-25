/**
 * Host stand-in for the CMSIS compiler intrinsics header.
 *
 * furi reaches for ARM intrinsics in exactly three places
 * (core/common_defines.h, core/critical.c, core/check.c). On the host every
 * thread is an ordinary pthread, so there is no exception context and no
 * maskable interrupt state: IPSR and PRIMASK read as zero, which makes
 * FURI_IS_ISR() constantly false and routes every critical section through
 * the FreeRTOS scheduler primitives.
 */
#pragma once

#include <stdint.h>

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif

#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))
#endif

#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

#ifndef __PACKED
#define __PACKED __attribute__((packed))
#endif

#ifndef __USED
#define __USED __attribute__((used))
#endif

#ifndef __NO_RETURN
#define __NO_RETURN __attribute__((__noreturn__))
#endif

#ifndef __ASM
#define __ASM __asm
#endif

#ifndef __INLINE
#define __INLINE inline
#endif

__STATIC_INLINE uint32_t __get_IPSR(void) {
    return 0U;
}

__STATIC_INLINE uint32_t __get_PRIMASK(void) {
    return 0U;
}

__STATIC_INLINE void __set_PRIMASK(uint32_t priMask) {
    (void)priMask;
}

__STATIC_INLINE void __disable_irq(void) {
}

__STATIC_INLINE void __enable_irq(void) {
}

__STATIC_INLINE void __NOP(void) {
}

__STATIC_INLINE void __DSB(void) {
    __sync_synchronize();
}

__STATIC_INLINE void __ISB(void) {
    __sync_synchronize();
}

__STATIC_INLINE void __DMB(void) {
    __sync_synchronize();
}

__STATIC_INLINE void __WFI(void) {
}

__STATIC_INLINE uint8_t __CLZ(uint32_t value) {
    return value == 0U ? 32U : (uint8_t)__builtin_clz(value);
}
