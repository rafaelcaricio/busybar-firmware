/**
 * Host replacement for lvgl_addons/fs/lv_fs.c.
 *
 * The firmware version routes LVGL's file access through the storage service
 * and the device filesystem. Here it goes straight to stdio, rooted at a
 * directory on the workstation, so fonts and images load from a checkout of
 * the resources tree without a storage service in the build.
 *
 * The drive letter stays 'C' so asset paths in the UI code are unchanged.
 */
#include <lvgl_addons/fs/lv_fs.h>

#include <furi.h>
#include <lvgl.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char lv_fs_host_root[512];

static void lv_fs_host_resolve(const char* path, char* out, size_t out_size) {
    /* LVGL hands over the path with the drive prefix already stripped. */
    snprintf(out, out_size, "%s/%s", lv_fs_host_root, path[0] == '/' ? path + 1 : path);
}

static void* lv_fs_host_open(lv_fs_drv_t* drv, const char* path, lv_fs_mode_t mode) {
    UNUSED(drv);

    char resolved[1024];
    lv_fs_host_resolve(path, resolved, sizeof(resolved));

    FILE* file = fopen(resolved, mode == LV_FS_MODE_RD ? "rb" : "r+b");
    if(!file) {
        FURI_LOG_W("LvFsHost", "open failed: %s", resolved);
    }

    return file;
}

static lv_fs_res_t lv_fs_host_close(lv_fs_drv_t* drv, void* file_p) {
    UNUSED(drv);
    return fclose(file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_HW_ERR;
}

static lv_fs_res_t
    lv_fs_host_read(lv_fs_drv_t* drv, void* file_p, void* buf, uint32_t btr, uint32_t* br) {
    UNUSED(drv);
    *br = (uint32_t)fread(buf, 1, btr, file_p);
    return ferror(file_p) ? LV_FS_RES_HW_ERR : LV_FS_RES_OK;
}

static lv_fs_res_t
    lv_fs_host_write(lv_fs_drv_t* drv, void* file_p, const void* buf, uint32_t btw, uint32_t* bw) {
    UNUSED(drv);
    *bw = (uint32_t)fwrite(buf, 1, btw, file_p);
    return ferror(file_p) ? LV_FS_RES_HW_ERR : LV_FS_RES_OK;
}

static lv_fs_res_t lv_fs_host_seek(lv_fs_drv_t* drv, void* file_p, uint32_t pos, lv_fs_whence_t whence) {
    UNUSED(drv);

    int origin = SEEK_SET;
    if(whence == LV_FS_SEEK_CUR) origin = SEEK_CUR;
    if(whence == LV_FS_SEEK_END) origin = SEEK_END;

    return fseek(file_p, (long)pos, origin) == 0 ? LV_FS_RES_OK : LV_FS_RES_HW_ERR;
}

static lv_fs_res_t lv_fs_host_tell(lv_fs_drv_t* drv, void* file_p, uint32_t* pos_p) {
    UNUSED(drv);

    const long pos = ftell(file_p);
    if(pos < 0) return LV_FS_RES_HW_ERR;

    *pos_p = (uint32_t)pos;
    return LV_FS_RES_OK;
}

static void* lv_fs_host_dir_open(lv_fs_drv_t* drv, const char* path) {
    UNUSED(drv);

    char resolved[1024];
    lv_fs_host_resolve(path, resolved, sizeof(resolved));

    return opendir(resolved);
}

static lv_fs_res_t lv_fs_host_dir_read(lv_fs_drv_t* drv, void* rddir_p, char* fn, uint32_t fn_len) {
    UNUSED(drv);

    const struct dirent* entry = readdir(rddir_p);
    if(!entry) {
        fn[0] = '\0';
        return LV_FS_RES_OK;
    }

    strncpy(fn, entry->d_name, fn_len - 1);
    fn[fn_len - 1] = '\0';
    return LV_FS_RES_OK;
}

static lv_fs_res_t lv_fs_host_dir_close(lv_fs_drv_t* drv, void* rddir_p) {
    UNUSED(drv);
    return closedir(rddir_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_HW_ERR;
}

static lv_fs_drv_t lv_fs_host_driver = {
    .letter = 'C',
    .cache_size = 1024 * 5,
    .ready_cb = NULL,
    .open_cb = lv_fs_host_open,
    .close_cb = lv_fs_host_close,
    .read_cb = lv_fs_host_read,
    .write_cb = lv_fs_host_write,
    .seek_cb = lv_fs_host_seek,
    .tell_cb = lv_fs_host_tell,
    .dir_open_cb = lv_fs_host_dir_open,
    .dir_read_cb = lv_fs_host_dir_read,
    .dir_close_cb = lv_fs_host_dir_close,
};

void lv_storage_driver_init(void) {
    /* Set by main() before the GUI service starts, so both this driver and
     * the storage service resolve against the same tree. */
    extern const char* simulator_assets_root;
    snprintf(
        lv_fs_host_root,
        sizeof(lv_fs_host_root),
        "%s",
        simulator_assets_root ? simulator_assets_root : ".");

    FURI_LOG_I("LvFsHost", "assets root: %s", lv_fs_host_root);
    lv_fs_drv_register(&lv_fs_host_driver);
}
