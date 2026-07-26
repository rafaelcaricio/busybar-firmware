/**
 * Host implementation of the storage service.
 *
 * font_registry and anim_file read their assets through this API. On the
 * device it reaches the internal flash and the SD card; here reads are layered
 * over an immutable generated asset root and writes go to a separate state
 * root. Pointing the simulator at a resource tree is enough to load real fonts
 * and animations without letting an app mutate that tree.
 *
 * The HTTP API reaches the same service to upload assets and manage files, so
 * the mutating calls are implemented too. Anything still missing returns a
 * failure rather than pretending to have succeeded.
 */
#include "storage_host.h"

#include <furi.h>

#include <storage/storage.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define TAG "StorageHost"

struct File {
    FILE* stream;
    DIR* state_dir;
    DIR* assets_dir;
    bool reading_assets;
    /* Kept so directory entries can be stat'd: readdir gives a name, and the
     * API reports a size for each one. */
    char state_dir_path[PATH_MAX];
    char assets_dir_path[PATH_MAX];
};

/* The service record must be non-NULL for furi_record_open(). */
static int storage_host_instance;
static char storage_host_assets[PATH_MAX];
static char storage_host_state[PATH_MAX];

static bool storage_host_path_is_safe(const char* path) {
    if(!path) return false;

    const char* cursor = path;
    while(*cursor) {
        while(*cursor == '/') cursor++;
        if(!*cursor) break;

        const char* end = strchr(cursor, '/');
        const size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if((length == 1 && cursor[0] == '.') ||
           (length == 2 && cursor[0] == '.' && cursor[1] == '.')) {
            return false;
        }
        cursor += length;
    }

    return true;
}

static bool storage_host_is_within(const char* root, const char* path) {
    const size_t root_length = strlen(root);
    return strncmp(root, path, root_length) == 0 &&
           (path[root_length] == '\0' || path[root_length] == '/');
}

static bool storage_host_ensure_directory(const char* path) {
    if(mkdir(path, 0755) == 0) return true;
    if(errno != EEXIST) return false;

    struct stat info;
    /* macOS commonly exposes its temporary root through /var -> /private/var.
     * Existing ancestors may therefore be directory symlinks; realpath() in
     * storage_host_canonical_root() resolves and validates the final root. */
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static bool storage_host_mkdir_path(char* path) {
    for(char* cursor = path + 1; *cursor; cursor++) {
        if(*cursor != '/') continue;
        *cursor = '\0';
        if(!storage_host_ensure_directory(path)) {
            *cursor = '/';
            return false;
        }
        *cursor = '/';
    }
    return storage_host_ensure_directory(path);
}

static bool storage_host_canonical_root(
    const char* path,
    bool create,
    char* out,
    size_t out_size) {
    if(!path || !*path) return false;

    char candidate[PATH_MAX];
    if(snprintf(candidate, sizeof(candidate), "%s", path) >= (int)sizeof(candidate)) {
        return false;
    }
    if(create && !storage_host_mkdir_path(candidate)) return false;

    char canonical[PATH_MAX];
    if(!realpath(candidate, canonical)) return false;

    struct stat info;
    if(stat(canonical, &info) != 0 || !S_ISDIR(info.st_mode)) return false;

    return snprintf(out, out_size, "%s", canonical) < (int)out_size;
}

static bool storage_host_make_temporary_state(char* out, size_t out_size) {
    const char* temporary_root = getenv("TMPDIR");
    if(!temporary_root || !*temporary_root) temporary_root = "/tmp";

    char pattern[PATH_MAX];
    if(snprintf(
           pattern, sizeof(pattern), "%s/busybar-sim-state.XXXXXX", temporary_root) >=
       (int)sizeof(pattern)) {
        return false;
    }
    if(!mkdtemp(pattern)) return false;
    return storage_host_canonical_root(pattern, false, out, out_size);
}

static bool storage_host_state_candidate(
    const char* path,
    char* out,
    size_t out_size,
    bool allow_leaf_symlink) {
    if(!storage_host_path_is_safe(path)) return false;
    const char* relative = path[0] == '/' ? path + 1 : path;
    if(snprintf(out, out_size, "%s/%s", storage_host_state, relative) >= (int)out_size) {
        return false;
    }

    /* A writable overlay supplied by a developer may already contain
     * symlinks. Reject them component by component so no operation can escape
     * the canonical state root. The leaf may be allowed for unlink(), which
     * removes the link itself without following it. */
    const size_t root_length = strlen(storage_host_state);
    for(char* cursor = out + root_length + 1;; cursor++) {
        if(*cursor != '/' && *cursor != '\0') continue;

        const bool leaf = *cursor == '\0';
        const char saved = *cursor;
        *cursor = '\0';

        struct stat info;
        const bool exists = lstat(out, &info) == 0;
        const bool unsafe = exists && S_ISLNK(info.st_mode) && !(leaf && allow_leaf_symlink);

        *cursor = saved;
        if(unsafe) return false;
        if(leaf) break;
    }

    return true;
}

static bool storage_host_assets_candidate(const char* path, char* out, size_t out_size) {
    if(!storage_host_path_is_safe(path)) return false;
    const char* relative = path[0] == '/' ? path + 1 : path;

    char candidate[PATH_MAX];
    if(snprintf(candidate, sizeof(candidate), "%s/%s", storage_host_assets, relative) >=
       (int)sizeof(candidate)) {
        return false;
    }

    char canonical[PATH_MAX];
    if(!realpath(candidate, canonical) ||
       !storage_host_is_within(storage_host_assets, canonical)) {
        return false;
    }

    return snprintf(out, out_size, "%s", canonical) < (int)out_size;
}

bool storage_host_init(const char* assets_root, const char* state_root) {
    if(!storage_host_canonical_root(
           assets_root ? assets_root : ".", false, storage_host_assets, sizeof(storage_host_assets))) {
        FURI_LOG_E(TAG, "invalid assets root: %s", assets_root ? assets_root : "(null)");
        return false;
    }

    const bool state_ok =
        state_root ?
            storage_host_canonical_root(
                state_root, true, storage_host_state, sizeof(storage_host_state)) :
            storage_host_make_temporary_state(storage_host_state, sizeof(storage_host_state));
    if(!state_ok) {
        FURI_LOG_E(TAG, "invalid state root: %s", state_root ? state_root : "(temporary)");
        return false;
    }

    if(storage_host_is_within(storage_host_assets, storage_host_state) ||
       storage_host_is_within(storage_host_state, storage_host_assets)) {
        FURI_LOG_E(TAG, "assets and state roots must not overlap");
        return false;
    }

    /* /ext is a mounted filesystem root on the device. The immutable asset
     * layer makes it visible for reads, but write-side mkdir/open calls resolve
     * only into the overlay. Materialize the mount point before firmware
     * services start so their ordinary one-level mkdir calls behave exactly as
     * they do against a mounted SD card. */
    char external_root[PATH_MAX];
    if(!storage_host_state_candidate(
           STORAGE_EXT_PATH_PREFIX, external_root, sizeof(external_root), false) ||
       !storage_host_mkdir_path(external_root)) {
        FURI_LOG_E(TAG, "could not initialize writable %s mount", STORAGE_EXT_PATH_PREFIX);
        return false;
    }

    FURI_LOG_I(TAG, "assets: %s", storage_host_assets);
    FURI_LOG_I(TAG, "state: %s", storage_host_state);

    furi_record_create(RECORD_STORAGE, &storage_host_instance);
    return true;
}

const char* storage_host_assets_root(void) {
    return storage_host_assets;
}

const char* storage_host_state_root(void) {
    return storage_host_state;
}

bool storage_host_resolve_path(const char* path, char* out, size_t out_size) {
    char state_path[PATH_MAX];
    if(storage_host_state_candidate(path, state_path, sizeof(state_path), false)) {
        struct stat info;
        if(lstat(state_path, &info) == 0) {
            return snprintf(out, out_size, "%s", state_path) < (int)out_size;
        }
    }

    return storage_host_assets_candidate(path, out, out_size);
}

bool storage_host_resolve_write_path(const char* path, char* out, size_t out_size) {
    return storage_host_state_candidate(path, out, out_size, false);
}

static bool storage_host_ensure_parent(char* path) {
    char* separator = strrchr(path, '/');
    if(!separator) return false;
    *separator = '\0';
    const bool result = storage_host_mkdir_path(path);
    *separator = '/';
    return result;
}

static bool storage_host_copy_for_write(const char* device_path, char* state_path) {
    char assets_path[PATH_MAX];
    if(!storage_host_assets_candidate(device_path, assets_path, sizeof(assets_path))) return false;
    if(!storage_host_ensure_parent(state_path)) return false;

    FILE* source = fopen(assets_path, "rb");
    FILE* target = source ? fopen(state_path, "wb") : NULL;
    if(!source || !target) {
        if(source) fclose(source);
        if(target) fclose(target);
        return false;
    }

    bool ok = true;
    uint8_t buffer[8192];
    size_t count;
    while((count = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if(fwrite(buffer, 1, count, target) != count) {
            ok = false;
            break;
        }
    }
    if(ferror(source)) ok = false;
    if(fclose(source) != 0) ok = false;
    if(fclose(target) != 0) ok = false;
    if(!ok) unlink(state_path);
    return ok;
}

File* storage_file_alloc(Storage* storage) {
    UNUSED(storage);
    return calloc(1, sizeof(File));
}

void storage_file_free(File* file) {
    if(!file) return;

    if(file->stream) fclose(file->stream);
    if(file->state_dir) closedir(file->state_dir);
    if(file->assets_dir) closedir(file->assets_dir);
    free(file);
}

bool storage_file_open(
    File* file,
    const char* path,
    FS_AccessMode access_mode,
    FS_OpenMode open_mode) {
    furi_check(file);

    char resolved[PATH_MAX];
    if(!(access_mode & FSAM_WRITE)) {
        if(!storage_host_resolve_path(path, resolved, sizeof(resolved))) return false;
        file->stream = fopen(resolved, "rb");
    } else {
        if(!storage_host_resolve_write_path(path, resolved, sizeof(resolved))) return false;

        struct stat state_info;
        const bool state_exists = lstat(resolved, &state_info) == 0;
        char assets_path[PATH_MAX];
        const bool assets_exist =
            storage_host_assets_candidate(path, assets_path, sizeof(assets_path));

        if((open_mode & FSOM_CREATE_NEW) && (state_exists || assets_exist)) return false;

        const bool preserve_existing =
            open_mode & (FSOM_OPEN_EXISTING | FSOM_OPEN_ALWAYS | FSOM_OPEN_APPEND);
        if(!state_exists && assets_exist && preserve_existing &&
           !storage_host_copy_for_write(path, resolved)) {
            return false;
        }

        int flags = (access_mode & FSAM_READ) ? O_RDWR : O_WRONLY;
        if(open_mode & FSOM_OPEN_ALWAYS) flags |= O_CREAT;
        if(open_mode & FSOM_OPEN_APPEND) flags |= O_CREAT | O_APPEND;
        if(open_mode & FSOM_CREATE_NEW) flags |= O_CREAT | O_EXCL;
        if(open_mode & FSOM_CREATE_ALWAYS) flags |= O_CREAT | O_TRUNC;

        const int descriptor = open(resolved, flags, 0644);
        if(descriptor >= 0) {
            const char* mode = (access_mode & FSAM_READ) ? "r+b" : "wb";
            if(open_mode & FSOM_OPEN_APPEND) mode = (access_mode & FSAM_READ) ? "a+b" : "ab";
            file->stream = fdopen(descriptor, mode);
            if(!file->stream) close(descriptor);
        }
    }

    if(!file->stream) {
        FURI_LOG_W(TAG, "open failed: %s", resolved);
        return false;
    }

    return true;
}

bool storage_file_close(File* file) {
    if(!file || !file->stream) return false;

    const bool ok = fclose(file->stream) == 0;
    file->stream = NULL;
    return ok;
}

bool storage_file_is_open(File* file) {
    return file && file->stream;
}

size_t storage_file_read(File* file, void* buff, size_t bytes_to_read) {
    if(!file || !file->stream) return 0;
    return fread(buff, 1, bytes_to_read, file->stream);
}

size_t storage_file_write(File* file, const void* buff, size_t bytes_to_write) {
    if(!file || !file->stream) return 0;
    return fwrite(buff, 1, bytes_to_write, file->stream);
}

bool storage_file_seek(File* file, uint32_t offset, bool from_start) {
    if(!file || !file->stream) return false;
    return fseek(file->stream, (long)offset, from_start ? SEEK_SET : SEEK_CUR) == 0;
}

uint64_t storage_file_tell(File* file) {
    if(!file || !file->stream) return 0;

    const long pos = ftell(file->stream);
    return pos < 0 ? 0 : (uint64_t)pos;
}

uint64_t storage_file_size(File* file) {
    if(!file || !file->stream) return 0;

    const long current = ftell(file->stream);
    if(current < 0) return 0;

    if(fseek(file->stream, 0, SEEK_END) != 0) return 0;
    const long size = ftell(file->stream);
    fseek(file->stream, current, SEEK_SET);

    return size < 0 ? 0 : (uint64_t)size;
}

bool storage_file_eof(File* file) {
    if(!file || !file->stream) return true;
    return feof(file->stream) != 0;
}

FS_Error storage_common_stat(Storage* storage, const char* path, FileInfo* fileinfo) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_resolve_path(path, resolved, sizeof(resolved))) return FSE_NOT_EXIST;

    struct stat info;
    if(lstat(resolved, &info) != 0) return FSE_NOT_EXIST;

    if(fileinfo) {
        fileinfo->flags = S_ISDIR(info.st_mode) ? FSF_DIRECTORY : 0;
        fileinfo->size = (uint64_t)info.st_size;
    }

    return FSE_OK;
}

void storage_common_resolve_path_and_ensure_app_directory(Storage* storage, FuriString* path) {
    UNUSED(storage);
    furi_check(path);

    /* APP_DATA_PATH already contains the app namespace in this build. Preserve
     * it and create the matching directory in the writable overlay. */
    char resolved[PATH_MAX];
    if(!storage_host_resolve_write_path(
           furi_string_get_cstr(path), resolved, sizeof(resolved))) {
        return;
    }

    char* last_separator = strrchr(resolved, '/');
    if(last_separator) {
        *last_separator = '\0';
        if(!storage_host_mkdir_path(resolved)) {
            FURI_LOG_W(TAG, "could not create app data directory: %s", resolved);
        }
    }
}

bool storage_file_exists(Storage* storage, const char* path) {
    FileInfo info;
    return storage_common_stat(storage, path, &info) == FSE_OK &&
           !(info.flags & FSF_DIRECTORY);
}

bool storage_dir_exists(Storage* storage, const char* path) {
    FileInfo info;
    return storage_common_stat(storage, path, &info) == FSE_OK && (info.flags & FSF_DIRECTORY);
}

bool storage_dir_open(File* file, const char* path) {
    furi_check(file);

    if(storage_host_state_candidate(
           path, file->state_dir_path, sizeof(file->state_dir_path), false)) {
        file->state_dir = opendir(file->state_dir_path);
    }
    if(storage_host_assets_candidate(
           path, file->assets_dir_path, sizeof(file->assets_dir_path))) {
        file->assets_dir = opendir(file->assets_dir_path);
    }

    return file->state_dir || file->assets_dir;
}

bool storage_dir_close(File* file) {
    if(!file || (!file->state_dir && !file->assets_dir)) return false;

    bool ok = true;
    if(file->state_dir && closedir(file->state_dir) != 0) ok = false;
    if(file->assets_dir && closedir(file->assets_dir) != 0) ok = false;
    file->state_dir = NULL;
    file->assets_dir = NULL;
    file->reading_assets = false;
    return ok;
}

bool storage_dir_read(File* file, FileInfo* fileinfo, char* name, uint16_t name_length) {
    if(!file || (!file->state_dir && !file->assets_dir)) return false;

    const struct dirent* entry;
    const char* source_dir_path;
    while(true) {
        DIR* source_dir = file->reading_assets ? file->assets_dir : file->state_dir;
        source_dir_path =
            file->reading_assets ? file->assets_dir_path : file->state_dir_path;

        if(!source_dir) {
            if(file->reading_assets) return false;
            file->reading_assets = true;
            continue;
        }

        entry = readdir(source_dir);
        if(!entry) {
            if(file->reading_assets) return false;
            file->reading_assets = true;
            continue;
        }

        /* The device's FatFS enumeration has no "." or ".." entries. */
        if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        /* An overlay entry shadows an immutable asset with the same name. */
        if(file->reading_assets && file->state_dir_path[0]) {
            char state_entry[PATH_MAX];
            if(snprintf(
                   state_entry,
                   sizeof(state_entry),
                   "%s/%s",
                   file->state_dir_path,
                   entry->d_name) < (int)sizeof(state_entry)) {
                struct stat state_info;
                if(lstat(state_entry, &state_info) == 0) continue;
            }
        }
        break;
    }

    if(name && name_length) {
        strncpy(name, entry->d_name, name_length - 1);
        name[name_length - 1] = '\0';
    }

    if(fileinfo) {
        fileinfo->flags = entry->d_type == DT_DIR ? FSF_DIRECTORY : 0;
        fileinfo->size = 0;

        char entry_path[PATH_MAX];
        const int path_length =
            snprintf(entry_path, sizeof(entry_path), "%s/%s", source_dir_path, entry->d_name);
        if(path_length >= 0 && path_length < (int)sizeof(entry_path)) {
            struct stat info;
            if(lstat(entry_path, &info) == 0) {
                fileinfo->flags = S_ISDIR(info.st_mode) ? FSF_DIRECTORY : 0;
                fileinfo->size = (uint64_t)info.st_size;
            }
        }
    }

    return true;
}

/* ---------------------------------------------------------------------------
 * Write paths
 *
 * The read side above is all the UI needs. The HTTP API also uploads assets,
 * writes files and creates directories, so the mutating half of the service is
 * implemented here on top of the same path resolution.
 * ------------------------------------------------------------------------ */

static FS_Error storage_host_errno_to_fs_error(int error) {
    switch(error) {
    case 0:
        return FSE_OK;
    case ENOENT:
        return FSE_NOT_EXIST;
    case EEXIST:
        return FSE_EXIST;
    case EACCES:
    case EPERM:
    case EROFS:
        return FSE_DENIED;
    case EINVAL:
    case ENAMETOOLONG:
        return FSE_INVALID_NAME;
    default:
        return FSE_INTERNAL;
    }
}

bool storage_file_sync(File* file) {
    if(!file || !file->stream) return false;
    return fflush(file->stream) == 0;
}

bool storage_file_truncate(File* file) {
    if(!file || !file->stream) return false;

    const long at = ftell(file->stream);
    if(at < 0) return false;
    if(fflush(file->stream) != 0) return false;

    return ftruncate(fileno(file->stream), at) == 0;
}

FS_Error storage_common_timestamp(Storage* storage, const char* path, uint32_t* timestamp) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_resolve_path(path, resolved, sizeof(resolved))) return FSE_NOT_EXIST;

    struct stat info;
    if(lstat(resolved, &info) != 0) return storage_host_errno_to_fs_error(errno);

    if(timestamp) *timestamp = (uint32_t)info.st_mtime;
    return FSE_OK;
}

FS_Error storage_common_remove(Storage* storage, const char* path) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_state_candidate(path, resolved, sizeof(resolved), true)) {
        return FSE_INVALID_NAME;
    }

    struct stat info;
    if(lstat(resolved, &info) != 0) {
        char assets_path[PATH_MAX];
        if(storage_host_assets_candidate(path, assets_path, sizeof(assets_path))) {
            return FSE_DENIED;
        }
        return storage_host_errno_to_fs_error(errno);
    }

    const int result = S_ISDIR(info.st_mode) ? rmdir(resolved) : unlink(resolved);
    return result == 0 ? FSE_OK : storage_host_errno_to_fs_error(errno);
}

FS_Error storage_common_rename(Storage* storage, const char* old_path, const char* new_path) {
    UNUSED(storage);

    char resolved_old[PATH_MAX];
    char resolved_new[PATH_MAX];
    if(!storage_host_resolve_write_path(old_path, resolved_old, sizeof(resolved_old)) ||
       !storage_host_resolve_write_path(new_path, resolved_new, sizeof(resolved_new))) {
        return FSE_INVALID_NAME;
    }

    struct stat info;
    if(lstat(resolved_old, &info) != 0) {
        char assets_path[PATH_MAX];
        return storage_host_assets_candidate(old_path, assets_path, sizeof(assets_path)) ?
                   FSE_DENIED :
                   FSE_NOT_EXIST;
    }

    return rename(resolved_old, resolved_new) == 0 ? FSE_OK :
                                                     storage_host_errno_to_fs_error(errno);
}

FS_Error storage_common_mkdir(Storage* storage, const char* path) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_resolve_write_path(path, resolved, sizeof(resolved))) {
        return FSE_INVALID_NAME;
    }

    return mkdir(resolved, 0755) == 0 ? FSE_OK : storage_host_errno_to_fs_error(errno);
}

FS_Error storage_common_fs_info(
    Storage* storage,
    const char* fs_path,
    uint64_t* total_space,
    uint64_t* free_space,
    bool* is_read_only) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_resolve_write_path(fs_path, resolved, sizeof(resolved))) {
        return FSE_INVALID_NAME;
    }

    struct statvfs info;
    if(statvfs(resolved, &info) != 0) {
        /* The requested overlay directory may not exist yet; the state root
         * is on the same filesystem and has the values callers need. */
        if(statvfs(storage_host_state, &info) != 0) return storage_host_errno_to_fs_error(errno);
    }

    if(total_space) *total_space = (uint64_t)info.f_blocks * info.f_frsize;
    if(free_space) *free_space = (uint64_t)info.f_bavail * info.f_frsize;
    if(is_read_only) *is_read_only = (info.f_flag & ST_RDONLY) != 0;

    return FSE_OK;
}

bool storage_simply_remove_recursive(Storage* storage, const char* path) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_state_candidate(path, resolved, sizeof(resolved), true)) return false;

    struct stat info;
    if(lstat(resolved, &info) != 0) {
        char assets_path[PATH_MAX];
        return !storage_host_assets_candidate(path, assets_path, sizeof(assets_path));
    }

    if(S_ISDIR(info.st_mode)) {
        DIR* dir = opendir(resolved);
        if(!dir) return false;

        bool ok = true;
        const struct dirent* entry;
        while((entry = readdir(dir))) {
            if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            char child[PATH_MAX];
            if(snprintf(child, sizeof(child), "%s/%s", resolved, entry->d_name) >=
               (int)sizeof(child)) {
                ok = false;
                break;
            }

            struct stat child_info;
            if(lstat(child, &child_info) != 0) {
                ok = false;
                break;
            }
            if(S_ISDIR(child_info.st_mode)) {
                /* Translate back to a device path only after containment has
                 * already been established by state_candidate(). */
                const char* relative = child + strlen(storage_host_state);
                if(!storage_simply_remove_recursive(storage, relative)) {
                    ok = false;
                    break;
                }
            } else if(unlink(child) != 0) {
                ok = false;
                break;
            }
        }
        closedir(dir);
        if(!ok) return false;
        return rmdir(resolved) == 0;
    }

    return unlink(resolved) == 0;
}

bool storage_simply_remove(Storage* storage, const char* path) {
    const FS_Error error = storage_common_remove(storage, path);
    return error == FSE_OK || error == FSE_NOT_EXIST;
}

bool storage_simply_mkpath(Storage* storage, const char* path) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_resolve_write_path(path, resolved, sizeof(resolved))) return false;
    return storage_host_mkdir_path(resolved);
}

size_t storage_simply_read_entire_file(
    Storage* storage,
    const char* path,
    void* buffer,
    size_t buf_sz) {
    UNUSED(storage);
    if(buf_sz == 0) return 0;

    char resolved[PATH_MAX];
    if(!storage_host_resolve_path(path, resolved, sizeof(resolved))) return 0;

    FILE* stream = fopen(resolved, "rb");
    if(!stream) return 0;

    const size_t read = fread(buffer, 1, buf_sz - 1, stream);
    fclose(stream);

    return read;
}

bool storage_simply_write_entire_file(
    Storage* storage,
    const char* path,
    const void* buffer,
    size_t length) {
    UNUSED(storage);

    char resolved[PATH_MAX];
    if(!storage_host_resolve_write_path(path, resolved, sizeof(resolved))) return false;
    if(!storage_host_ensure_parent(resolved)) return false;

    FILE* stream = fopen(resolved, "wb");
    if(!stream) return false;

    const size_t written = fwrite(buffer, 1, length, stream);
    return fclose(stream) == 0 && written == length;
}
