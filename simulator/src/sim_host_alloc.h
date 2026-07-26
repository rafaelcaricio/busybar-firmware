/**
 * Host-only allocation for simulator tooling.
 *
 * furi overrides malloc() with the simulated firmware heap. Screenshots and
 * recordings are workstation tooling, so their multi-megabyte buffers must
 * not change firmware heap pressure or fail because an app legitimately uses
 * that constrained heap. mmap() also avoids trying to call through the malloc
 * symbol that furi interposes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void* sim_host_alloc(size_t size);
void* sim_host_realloc(void* pointer, size_t size);
void sim_host_free(void* pointer);

/** Select host allocation for lodepng calls on only the current POSIX thread. */
void sim_host_allocator_enter(void);
void sim_host_allocator_leave(void);
bool sim_host_allocator_active(void);
