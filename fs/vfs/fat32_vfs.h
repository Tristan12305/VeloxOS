#pragma once

#include <fs/fat32.h>
#include <fs/vfs/vfs.h>

// Mount a FAT32 volume into the VFS at `path`.
// Returns VFS_OK or a negative VFS_ERR_* code.
int fat32_vfs_mount(fat32_vol_t *vol, const char *path);
int fat32_vfs_unmount(const char *path);
