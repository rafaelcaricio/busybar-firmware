/**
 * Host implementations of the furi_hal surface that furi core links against.
 *
 * Everything here is either inert (interrupt accounting, memory regions) or
 * forwarded to the workstation equivalent (monotonic clock, wall clock).
 */
#include <furi.h>
#include <furi_hal.h>

#include <FreeRTOS.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

/* -- memory ------------------------------------------------------------- */

void* furi_hal_memory_alloc(size_t size) {
    UNUSED(size);
    /* No secondary pool on the host; callers fall back to malloc(). */
    return NULL;
}

size_t furi_hal_memory_get_free(void) {
    return 0;
}

size_t furi_hal_memory_max_pool_block(void) {
    return 0;
}

/** The simulator's stand-in for the linker-provided heap section.
 *
 * furi's allocator carves everything out of this, exactly as it does out of
 * SRAM on the device. The default is deliberately finite enough to expose
 * leaks while leaving room for host-only screenshots and recorder buffers;
 * CMake exposes BUSYBAR_SIM_HEAP_MB for stress runs.
 */
static uint8_t furi_hal_memory_heap[FURI_HAL_MEMORY_HEAP_SIZE];

static const FuriHalMemoryRegion furi_hal_memory_regions[] = {
    {
        .start = furi_hal_memory_heap,
        .size_bytes = sizeof(furi_hal_memory_heap),
    },
};

uint32_t furi_hal_memory_get_region_count(void) {
    return COUNT_OF(furi_hal_memory_regions);
}

const FuriHalMemoryRegion* furi_hal_memory_get_region(uint32_t index) {
    furi_check(index < COUNT_OF(furi_hal_memory_regions));
    return &furi_hal_memory_regions[index];
}

FuriHalMemoryHeapTrackMode furi_hal_memory_get_heap_track_mode(void) {
    return FuriHalMemoryHeapTrackModeNone;
}

/* -- power -------------------------------------------------------------- */

/* The loader takes an insomnia hold for the duration of any app whose manifest
 * asks to keep the device awake. Nothing here ever sleeps, so the hold has
 * nothing to prevent. */
void furi_hal_power_insomnia_enter(void) {
}

void furi_hal_power_insomnia_exit(void) {
}

/* -- interrupts --------------------------------------------------------- */

const char* furi_hal_interrupt_get_name(uint8_t exception_number) {
    UNUSED(exception_number);
    return NULL;
}

uint32_t furi_hal_interrupt_get_time_in_isr_total(void) {
    return 0;
}

/* -- cpu ---------------------------------------------------------------- */

uint32_t furi_hal_cpu_get_cycles_per_us(void) {
    return FURI_HAL_CPU_CYCLES_PER_US;
}

uint32_t furi_hal_cpu_get_cycle_count(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* One "cycle" is a nanosecond, matching FURI_HAL_CPU_CYCLES_PER_US. */
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec);
}

/* -- rtc ---------------------------------------------------------------- */

static int64_t furi_hal_rtc_offset_ms = 0;

static int64_t furi_hal_rtc_host_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

DateTimeMs furi_hal_rtc_get_datetime(void) {
    const int64_t now_ms = furi_hal_rtc_host_now_ms() + furi_hal_rtc_offset_ms;
    const time_t utc = (time_t)(now_ms / 1000);

    DateTimeMs result;
    /* Reuse the datetime library rather than mapping struct tm by hand: it
     * owns the dayofweek convention the widgets expect. */
    result.dt = datetime_timestamp_to_datetime(utc);
    result.millis = (uint16_t)(now_ms % 1000);

    return result;
}

void furi_hal_rtc_set_datetime(const DateTimeMs* datetime) {
    furi_check(datetime);
    const time_t utc = datetime_datetime_to_timestamp(&datetime->dt);
    furi_hal_rtc_offset_ms =
        ((int64_t)utc * 1000 + datetime->millis) - furi_hal_rtc_host_now_ms();
}

time_t furi_hal_rtc_get_timestamp(void) {
    return (time_t)((furi_hal_rtc_host_now_ms() + furi_hal_rtc_offset_ms) / 1000);
}

time_t furi_hal_rtc_get_timestamp_ms(void) {
    return (time_t)(furi_hal_rtc_host_now_ms() + furi_hal_rtc_offset_ms);
}

/* -- heap --------------------------------------------------------------- */

/* The allocator itself is furi's own memmgr_heap.c, running over the static
 * region above; it supplies pvPortMalloc and the xPortGet*HeapSize family. */

void vApplicationMallocFailedHook(void) {
    extern size_t xPortGetFreeHeapSize(void);
    extern size_t xPortGetMinimumEverFreeHeapSize(void);
    fprintf(
        stderr,
        "[sim] heap allocation failed: %zu bytes free, %zu minimum ever free\n",
        xPortGetFreeHeapSize(),
        xPortGetMinimumEverFreeHeapSize());
    furi_crash("Out of memory");
}

/* -- libc gap ----------------------------------------------------------- */

char* itoa(int value, char* str, int base) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

    if(base < 2 || base > 36) {
        str[0] = '\0';
        return str;
    }

    char* out = str;
    /* Only base 10 carries a sign, matching newlib. */
    const bool negative = value < 0 && base == 10;
    /* Accumulate in unsigned so INT_MIN does not overflow on negation. */
    unsigned int magnitude = negative ? (unsigned int)-(long)value : (unsigned int)value;

    do {
        *out++ = digits[magnitude % (unsigned int)base];
        magnitude /= (unsigned int)base;
    } while(magnitude);

    if(negative) *out++ = '-';
    *out = '\0';

    for(char *head = str, *tail = out - 1; head < tail; ++head, --tail) {
        const char swap = *head;
        *head = *tail;
        *tail = swap;
    }

    return str;
}

void* memrchr(const void* buffer, int character, size_t length) {
    const unsigned char* cursor = (const unsigned char*)buffer + length;

    while(cursor-- > (const unsigned char*)buffer) {
        if(*cursor == (unsigned char)character) return (void*)cursor;
    }

    return NULL;
}

/* -- entropy ------------------------------------------------------------- */

/* The device has a hardware RNG. arc4random is the host's equivalent: seeded
 * by the kernel, and never failing the way /dev/urandom reads can. */

void furi_hal_random_fill_buf(uint8_t* buf, uint32_t len) {
    arc4random_buf(buf, len);
}

uint32_t furi_hal_random_get(void) {
    return arc4random();
}
