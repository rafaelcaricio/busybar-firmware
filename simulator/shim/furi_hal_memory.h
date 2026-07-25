/**
 * Host stand-in for the target furi_hal_memory API.
 *
 * The STM32U5 splits RAM into regions and furi allocates its heap out of one
 * of them. The simulator hands furi a single static buffer to play the same
 * role, so furi's own allocator runs unmodified and the process heap stays
 * out of the picture — furi overrides malloc(), so anything that forwards
 * back to libc would recurse.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of the static buffer furi's allocator manages in the simulator. */
#define FURI_HAL_MEMORY_HEAP_SIZE (32 * 1024 * 1024)

typedef enum {
    FuriHalMemoryRegionIdHeap,
    FuriHalMemoryRegionIdPool,

    FuriHalMemoryRegionIdMax,
} FuriHalMemoryRegionId;

typedef enum {
    FuriHalMemoryHeapTrackModeNone,
    FuriHalMemoryHeapTrackModeMain,
    FuriHalMemoryHeapTrackModeTree,
    FuriHalMemoryHeapTrackModeAll,
} FuriHalMemoryHeapTrackMode;

typedef struct {
    void* start;
    size_t size_bytes;
} FuriHalMemoryRegion;

void* furi_hal_memory_alloc(size_t size);

size_t furi_hal_memory_get_free(void);

size_t furi_hal_memory_max_pool_block(void);

uint32_t furi_hal_memory_get_region_count(void);

const FuriHalMemoryRegion* furi_hal_memory_get_region(uint32_t index);

FuriHalMemoryHeapTrackMode furi_hal_memory_get_heap_track_mode(void);

#ifdef __cplusplus
}
#endif
