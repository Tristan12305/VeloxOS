#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define VFS_NAME_MAX 255

#define VFS_NODE_FILE    0x0001
#define VFS_NODE_DIR     0x0002
#define VFS_NODE_CHAR    0x0004
#define VFS_NODE_BLOCK   0x0008
#define VFS_NODE_SYMLINK 0x0010

#define VFS_NODE_NAME_OWNED 0x80000000u

#define VFS_O_RDONLY 0x0001
#define VFS_O_WRONLY 0x0002
#define VFS_O_RDWR   0x0003
#define VFS_O_CREAT  0x0100
#define VFS_O_TRUNC  0x0200
#define VFS_O_APPEND 0x0400

#define VFS_OK               0
#define VFS_ERR_NOENT       -2
#define VFS_ERR_IO          -5
#define VFS_ERR_NOMEM       -12
#define VFS_ERR_EXIST       -17
#define VFS_ERR_NOTDIR      -20
#define VFS_ERR_INVAL       -22
#define VFS_ERR_NAMETOOLONG -36
#define VFS_ERR_NOTSUP      -95

typedef int64_t vfs_ssize_t;

typedef struct vfs_node vfs_node_t;
typedef struct vfs_file vfs_file_t;
typedef struct vfs_stat vfs_stat_t;

typedef struct {
    char     name[VFS_NAME_MAX + 1];
    uint32_t type;
} vfs_dirent_t;

typedef struct {
    int (*open)(vfs_file_t *file, uint32_t flags);
    int (*close)(vfs_file_t *file);
    vfs_ssize_t (*read)(vfs_file_t *file, void *buf, size_t len);
    vfs_ssize_t (*write)(vfs_file_t *file, const void *buf, size_t len);
    int (*readdir)(vfs_node_t *dir, size_t index, vfs_dirent_t *out);
    vfs_node_t *(*lookup)(vfs_node_t *dir, const char *name);
    int (*ioctl)(vfs_file_t *file, uint32_t request, void *arg);
    int (*stat)(vfs_node_t *node, vfs_stat_t *out);
} vfs_node_ops_t;

struct vfs_node {
    const char *name;
    uint16_t name_len;
    uint32_t type;
    uint32_t flags;
    uint64_t size;

    const vfs_node_ops_t *ops;
    void *data;

    vfs_node_t *parent;
    vfs_node_t *first_child;
    vfs_node_t *next_sibling;
};

struct vfs_file {
    vfs_node_t *node;
    uint64_t pos;
    uint32_t flags;
    void *private_data;
};

typedef struct vfs_mount {
    char *path;
    uint16_t path_len;
    vfs_node_t *root;
    struct vfs_mount *next;
} vfs_mount_t;

struct vfs_stat {
    uint32_t type;
    uint64_t size;
};

void vfs_init(void);

int vfs_mount(vfs_node_t *root, const char *path);
int vfs_unmount(const char *path);
int vfs_unmount_detach(const char *path, vfs_node_t **out_root);

int vfs_resolve_path(const char *path, vfs_node_t **out_node);
vfs_node_t *vfs_resolve(const char *path);

int vfs_open(const char *path, uint32_t flags, vfs_file_t **out_file);
int vfs_close(vfs_file_t *file);

vfs_ssize_t vfs_read(vfs_file_t *file, void *buf, size_t len);
vfs_ssize_t vfs_write(vfs_file_t *file, const void *buf, size_t len);

int vfs_readdir(vfs_file_t *file, size_t index, vfs_dirent_t *out);

int vfs_stat(const char *path, vfs_stat_t *out);
int vfs_fstat(vfs_file_t *file, vfs_stat_t *out);

vfs_node_t *vfs_node_create(const char *name, uint32_t type,
                            const vfs_node_ops_t *ops, void *data);
void vfs_node_destroy(vfs_node_t *node);

int vfs_add_child(vfs_node_t *parent, vfs_node_t *child);
vfs_node_t *vfs_find_child(vfs_node_t *parent, const char *name);
