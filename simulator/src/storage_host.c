/**
 * Host implementation of the storage service.
 *
 * font_registry and anim_file read their assets through this API. On the
 * device it reaches the internal flash and the SD card; here every path is
 * resolved underneath the assets root, so pointing the simulator at a
 * checkout of the resources tree is enough to load real fonts and animations.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define TAG "StorageHost"

struct File {
    FILE* stream;
    DIR* dir;
    /* Kept so directory entries can be stat'd: readdir gives a name, and the
     * API reports a size for each one. */
    char dir_path[1024];
};

/* The service is stateless beyond the root path, but the record must be
 * non-NULL for furi_record_open(). */
static int storage_host_instance;
static char storage_host_root[512];

void storage_host_init(const char* root) {
    snprintf(storage_host_root, sizeof(storage_host_root), "%s", root ? root : ".");
    FURI_LOG_I(TAG, "root: %s", storage_host_root);

    furi_record_create(RECORD_STORAGE, &storage_host_instance);
}

void storage_host_resolve_path(const char* path, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", storage_host_root, path[0] == '/' ? path + 1 : path);
}

File* storage_file_alloc(Storage* storage) {
    UNUSED(storage);
    return calloc(1, sizeof(File));
}

void storage_file_free(File* file) {
    if(!file) return;

    if(file->stream) fclose(file->stream);
    if(file->dir) closedir(file->dir);
    free(file);
}

bool storage_file_open(
    File* file,
    const char* path,
    FS_AccessMode access_mode,
    FS_OpenMode open_mode) {
    furi_check(file);

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    const char* mode = "rb";
    if(access_mode & FSAM_WRITE) {
        if(open_mode & (FSOM_CREATE_ALWAYS | FSOM_CREATE_NEW)) {
            mode = "w+b";
        } else if(open_mode & FSOM_OPEN_APPEND) {
            mode = "a+b";
        } else {
            mode = "r+b";
        }
    }

    file->stream = fopen(resolved, mode);
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

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    struct stat info;
    if(stat(resolved, &info) != 0) return FSE_NOT_EXIST;

    if(fileinfo) {
        fileinfo->flags = S_ISDIR(info.st_mode) ? FSF_DIRECTORY : 0;
        fileinfo->size = (uint64_t)info.st_size;
    }

    return FSE_OK;
}

void storage_common_resolve_path_and_ensure_app_directory(Storage* storage, FuriString* path) {
    UNUSED(storage);
    furi_check(path);

    /* On the device this rewrites an app-relative path to that app's data
     * directory and creates it. The simulator serves everything from one
     * tree, so the path is left alone and only the directory is created. */
    char resolved[1024];
    storage_host_resolve_path(furi_string_get_cstr(path), resolved, sizeof(resolved));

    char* last_separator = strrchr(resolved, '/');
    if(last_separator) {
        *last_separator = '\0';
        /* mkdir -p, one component at a time. */
        for(char* cursor = resolved + 1; *cursor; cursor++) {
            if(*cursor != '/') continue;
            *cursor = '\0';
            mkdir(resolved, 0755);
            *cursor = '/';
        }
        mkdir(resolved, 0755);
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

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    file->dir = opendir(resolved);
    if(!file->dir) return false;

    snprintf(file->dir_path, sizeof(file->dir_path), "%s", resolved);
    return true;
}

bool storage_dir_close(File* file) {
    if(!file || !file->dir) return false;

    const bool ok = closedir(file->dir) == 0;
    file->dir = NULL;
    return ok;
}

bool storage_dir_read(File* file, FileInfo* fileinfo, char* name, uint16_t name_length) {
    if(!file || !file->dir) return false;

    const struct dirent* entry;
    /* The device's FatFS enumeration has no "." or ".." entries; readdir does,
     * and a caller listing a directory over the HTTP API would see them. */
    do {
        entry = readdir(file->dir);
        if(!entry) return false;
    } while(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);

    if(name && name_length) {
        strncpy(name, entry->d_name, name_length - 1);
        name[name_length - 1] = '\0';
    }

    if(fileinfo) {
        fileinfo->flags = entry->d_type == DT_DIR ? FSF_DIRECTORY : 0;
        fileinfo->size = 0;

        char entry_path[1024];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", file->dir_path, entry->d_name);

        struct stat info;
        if(stat(entry_path, &info) == 0) {
            fileinfo->flags = S_ISDIR(info.st_mode) ? FSF_DIRECTORY : 0;
            fileinfo->size = (uint64_t)info.st_size;
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

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    struct stat info;
    if(stat(resolved, &info) != 0) return storage_host_errno_to_fs_error(errno);

    if(timestamp) *timestamp = (uint32_t)info.st_mtime;
    return FSE_OK;
}

FS_Error storage_common_remove(Storage* storage, const char* path) {
    UNUSED(storage);

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    struct stat info;
    if(stat(resolved, &info) != 0) return storage_host_errno_to_fs_error(errno);

    const int result = S_ISDIR(info.st_mode) ? rmdir(resolved) : unlink(resolved);
    return result == 0 ? FSE_OK : storage_host_errno_to_fs_error(errno);
}

FS_Error storage_common_rename(Storage* storage, const char* old_path, const char* new_path) {
    UNUSED(storage);

    char resolved_old[1024];
    char resolved_new[1024];
    storage_host_resolve_path(old_path, resolved_old, sizeof(resolved_old));
    storage_host_resolve_path(new_path, resolved_new, sizeof(resolved_new));

    return rename(resolved_old, resolved_new) == 0 ? FSE_OK :
                                                     storage_host_errno_to_fs_error(errno);
}

FS_Error storage_common_mkdir(Storage* storage, const char* path) {
    UNUSED(storage);

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    return mkdir(resolved, 0755) == 0 ? FSE_OK : storage_host_errno_to_fs_error(errno);
}

FS_Error storage_common_fs_info(
    Storage* storage,
    const char* fs_path,
    uint64_t* total_space,
    uint64_t* free_space,
    bool* is_read_only) {
    UNUSED(storage);

    char resolved[1024];
    storage_host_resolve_path(fs_path, resolved, sizeof(resolved));

    struct statvfs info;
    if(statvfs(resolved, &info) != 0) return storage_host_errno_to_fs_error(errno);

    if(total_space) *total_space = (uint64_t)info.f_blocks * info.f_frsize;
    if(free_space) *free_space = (uint64_t)info.f_bavail * info.f_frsize;
    if(is_read_only) *is_read_only = (info.f_flag & ST_RDONLY) != 0;

    return FSE_OK;
}

bool storage_simply_remove_recursive(Storage* storage, const char* path) {
    FileInfo info;
    if(storage_common_stat(storage, path, &info) != FSE_OK) return true;

    if(info.flags & FSF_DIRECTORY) {
        File* dir = storage_file_alloc(storage);
        FuriString* child = furi_string_alloc();
        char name[256];

        if(storage_dir_open(dir, path)) {
            /* Names are collected before anything is unlinked: the directory
             * stream is not required to stay well defined across removals. */
            FuriString* names = furi_string_alloc();
            while(storage_dir_read(dir, NULL, name, sizeof(name))) {
                furi_string_cat_printf(names, "%s\n", name);
            }
            storage_dir_close(dir);

            const char* cursor = furi_string_get_cstr(names);
            while(*cursor) {
                const char* end = strchr(cursor, '\n');
                if(!end) break;
                furi_string_printf(child, "%s/%.*s", path, (int)(end - cursor), cursor);
                storage_simply_remove_recursive(storage, furi_string_get_cstr(child));
                cursor = end + 1;
            }
            furi_string_free(names);
        }

        furi_string_free(child);
        storage_file_free(dir);
    }

    return storage_common_remove(storage, path) == FSE_OK;
}

bool storage_simply_remove(Storage* storage, const char* path) {
    const FS_Error error = storage_common_remove(storage, path);
    return error == FSE_OK || error == FSE_NOT_EXIST;
}

bool storage_simply_mkpath(Storage* storage, const char* path) {
    UNUSED(storage);

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    for(char* cursor = resolved + 1; *cursor; cursor++) {
        if(*cursor != '/') continue;
        *cursor = '\0';
        mkdir(resolved, 0755);
        *cursor = '/';
    }

    return mkdir(resolved, 0755) == 0 || errno == EEXIST;
}

size_t storage_simply_read_entire_file(
    Storage* storage,
    const char* path,
    void* buffer,
    size_t buf_sz) {
    UNUSED(storage);
    if(buf_sz == 0) return 0;

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

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

    char resolved[1024];
    storage_host_resolve_path(path, resolved, sizeof(resolved));

    FILE* stream = fopen(resolved, "wb");
    if(!stream) return false;

    const size_t written = fwrite(buffer, 1, length, stream);
    return fclose(stream) == 0 && written == length;
}
