/**
 * Force-included ahead of every translation unit in the simulator.
 *
 * The firmware is built against newlib, whose <sys/cdefs.h> provides
 * _ATTRIBUTE and whose headers transitively drag in the string functions
 * mlib assumes. Apple libc does neither, so both are supplied here rather
 * than by touching the vendored sources.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#ifndef _ATTRIBUTE
#define _ATTRIBUTE(x) __attribute__(x)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** newlib extension used by furi's log formatting; absent from Apple libc. */
char* itoa(int value, char* str, int base);

/** GNU extension used by the log storage service; absent from Apple libc. */
void* memrchr(const void* buffer, int character, size_t length);

#ifdef __cplusplus
}
#endif
