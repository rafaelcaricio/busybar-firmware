/**
 * Host replacement for furi core/check.c.
 *
 * The on-target version dumps Cortex-M registers, inspects CoreDebug->DHCSR
 * to detect an attached DAP, and falls back on furi_hal_power_reset(). None
 * of that exists here, so a crash prints the message plus a native backtrace
 * and aborts, which is what a debugger on the host wants anyway.
 */
#include "check.h"
#include "log.h"
#include "common_defines.h"

#include <execinfo.h>
#include <stdio.h>
#include <stdlib.h>

#include <FreeRTOS.h>
#include <task.h>

_Thread_local const void* __furi_check_message_host = NULL;

static const char* furi_check_message_resolve(void) {
    const void* message = __furi_check_message_host;

    if(message == NULL) {
        return "Fatal Error";
    } else if(message == (void*)__FURI_ASSERT_MESSAGE_FLAG) {
        return "furi_assert failed";
    } else if(message == (void*)__FURI_CHECK_MESSAGE_FLAG) {
        return "furi_check failed";
    }

    return (const char*)message;
}

static void furi_check_print_backtrace(void) {
    void* frames[64];
    int count = backtrace(frames, 64);
    fprintf(stderr, "\r\n\tbacktrace:\r\n");
    backtrace_symbols_fd(frames, count, fileno(stderr));
}

static const char* furi_check_thread_name(void) {
    const char* name =
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) ? pcTaskGetName(NULL) : NULL;
    return name ? name : "main";
}

FURI_WEAK bool furi_crash_handler(bool debug) {
    UNUSED(debug);
    return false;
}

FURI_NORETURN void __furi_crash_implementation(void) {
    fprintf(
        stderr,
        "\r\n\033[0;31m[CRASH][%s] %s\033[0m\r\n",
        furi_check_thread_name(),
        furi_check_message_resolve());
    furi_check_print_backtrace();
    fflush(stderr);
    abort();
}

FURI_NORETURN void __furi_halt_implementation(void) {
    fprintf(
        stderr,
        "\r\n\033[0;31m[HALT][%s] %s\033[0m\r\n",
        furi_check_thread_name(),
        furi_check_message_resolve());
    fflush(stderr);
    exit(0);
}

void vAssertCalled(const char* file, unsigned long line) {
    fprintf(stderr, "\r\n\033[0;31m[FreeRTOS assert] %s:%lu\033[0m\r\n", file, line);
    furi_check_print_backtrace();
    fflush(stderr);
    abort();
}
