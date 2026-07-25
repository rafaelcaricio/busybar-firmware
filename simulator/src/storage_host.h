#pragma once

#include <stddef.h>

/** Create the storage service record, resolving all paths under @p root. */
void storage_host_init(const char* root);

/** Map a device path such as /ext/apps_assets/... onto the asset root. */
void storage_host_resolve_path(const char* path, char* out, size_t out_size);
