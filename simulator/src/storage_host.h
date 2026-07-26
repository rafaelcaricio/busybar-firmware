#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Create the storage service record.
 *
 * Reads use @p assets_root as an immutable base and prefer a matching file in
 * @p state_root. All writes go to state_root. When state_root is NULL, a fresh
 * temporary directory is created for this process.
 */
bool storage_host_init(const char* assets_root, const char* state_root);

/** Resolve a device path for reading, preferring the writable overlay. */
bool storage_host_resolve_path(const char* path, char* out, size_t out_size);

/** Resolve a device path inside the writable overlay. */
bool storage_host_resolve_write_path(const char* path, char* out, size_t out_size);

/** Canonical roots, primarily for diagnostics and the LVGL host driver. */
const char* storage_host_assets_root(void);
const char* storage_host_state_root(void);
