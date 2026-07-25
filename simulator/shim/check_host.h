/**
 * Host replacement for furi core/check.h.
 *
 * The on-target header smuggles the crash message through r12 with inline
 * asm so the debugger sees pristine registers. That asm is Cortex-M specific
 * and the macros are expanded at every furi_check() call site, so the whole
 * firmware would fail to compile for an arm64 host. Here the message travels
 * in a thread-local instead; the public macro contract is unchanged.
 *
 * Kept in sync manually with fbt_layers/core_libs/lib/furi/core/check.h.
 */
#pragma once

#include <m-core.h>
#include "common_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

#define __FURI_ASSERT_MESSAGE_FLAG (0x01)
#define __FURI_CHECK_MESSAGE_FLAG  (0x02)

extern _Thread_local const void* __furi_check_message_host;

/** Crash system */
FURI_NORETURN void __furi_crash_implementation(void);

/** Halt system */
FURI_NORETURN void __furi_halt_implementation(void);

#define __furi_crash(message)                                    \
    do {                                                         \
        __furi_check_message_host = (const void*)(message);      \
        __furi_crash_implementation();                           \
    } while(0)

#define furi_crash(...) M_APPLY(__furi_crash, M_IF_EMPTY(__VA_ARGS__)((NULL), (__VA_ARGS__)))

#define __furi_halt(message)                                     \
    do {                                                         \
        __furi_check_message_host = (const void*)(message);      \
        __furi_halt_implementation();                            \
    } while(0)

#define furi_halt(...) M_APPLY(__furi_halt, M_IF_EMPTY(__VA_ARGS__)((NULL), (__VA_ARGS__)))

#define __furi_check(__e, __m) \
    do {                       \
        if(!(__e)) {           \
            __furi_crash(__m); \
        }                      \
    } while(0)

#define furi_check(...) \
    M_APPLY(__furi_check, M_DEFAULT_ARGS(2, (__FURI_CHECK_MESSAGE_FLAG), __VA_ARGS__))

#ifdef FURI_DEBUG
#define __furi_assert(__e, __m) \
    do {                        \
        if(!(__e)) {            \
            __furi_crash(__m);  \
        }                       \
    } while(0)
#else
#define __furi_assert(__e, __m) \
    do {                        \
        ((void)(__e));          \
        ((void)(__m));          \
    } while(0)
#endif

#define furi_assert(...) \
    M_APPLY(__furi_assert, M_DEFAULT_ARGS(2, (__FURI_ASSERT_MESSAGE_FLAG), __VA_ARGS__))

#define furi_break(__e)        \
    do {                       \
        if(!(__e)) {           \
            __builtin_trap();  \
        }                      \
    } while(0)

bool furi_crash_handler(bool debug);

#ifdef __cplusplus
}
#endif
