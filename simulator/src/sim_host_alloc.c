#include "sim_host_alloc.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

typedef union {
    struct {
        size_t mapping_size;
        size_t payload_size;
    } fields;
    max_align_t alignment;
} SimHostAllocationHeader;

static _Thread_local unsigned sim_host_allocator_depth;

void* sim_host_alloc(size_t size) {
    if(size == 0) size = 1;
    if(size > SIZE_MAX - sizeof(SimHostAllocationHeader)) return NULL;

    const size_t mapping_size = sizeof(SimHostAllocationHeader) + size;
    SimHostAllocationHeader* header = mmap(
        NULL,
        mapping_size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    if(header == MAP_FAILED) return NULL;

    header->fields.mapping_size = mapping_size;
    header->fields.payload_size = size;
    return header + 1;
}

void* sim_host_realloc(void* pointer, size_t size) {
    if(!pointer) return sim_host_alloc(size);
    if(size == 0) {
        sim_host_free(pointer);
        return NULL;
    }

    SimHostAllocationHeader* header = (SimHostAllocationHeader*)pointer - 1;
    void* replacement = sim_host_alloc(size);
    if(!replacement) return NULL;

    const size_t copy_size =
        header->fields.payload_size < size ? header->fields.payload_size : size;
    memcpy(replacement, pointer, copy_size);
    sim_host_free(pointer);
    return replacement;
}

void sim_host_free(void* pointer) {
    if(!pointer) return;

    SimHostAllocationHeader* header = (SimHostAllocationHeader*)pointer - 1;
    munmap(header, header->fields.mapping_size);
}

void sim_host_allocator_enter(void) {
    sim_host_allocator_depth++;
}

void sim_host_allocator_leave(void) {
    if(sim_host_allocator_depth) sim_host_allocator_depth--;
}

bool sim_host_allocator_active(void) {
    return sim_host_allocator_depth != 0;
}
