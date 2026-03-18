#include "vfs.h"

#include <include/kmalloc.h>
#include <lib/string.h>

static vfs_mount_t *g_mounts = NULL;

static size_t vfs_strlen(const char *s)
{
    size_t n = 0;

    if (!s) {
        return 0;
    }

    while (s[n]) {
        n++;
    }

    return n;
}

static int vfs_strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }
    return (int)((unsigned char)*a) - (int)((unsigned char)*b);
}

static size_t vfs_trim_path_len(const char *path)
{
    size_t len = vfs_strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    return len;
}

static const char *vfs_skip_slashes(const char *p, const char *end)
{
    while (p < end && *p == '/') {
        p++;
    }
    return p;
}

static bool vfs_path_is_prefix(const char *path, size_t path_len,
                               const vfs_mount_t *mnt)
{
    if (!mnt || !mnt->path || mnt->path_len == 0) {
        return false;
    }

    if (mnt->path_len == 1) {
        return path_len >= 1 && path[0] == '/';
    }

    if (path_len < mnt->path_len) {
        return false;
    }

    if (memcmp(path, mnt->path, mnt->path_len) != 0) {
        return false;
    }

    if (path_len == mnt->path_len) {
        return true;
    }

    return path[mnt->path_len] == '/';
}

static vfs_mount_t *vfs_find_mount_for_path(const char *path, size_t path_len)
{
    vfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (vfs_mount_t *mnt = g_mounts; mnt; mnt = mnt->next) {
        if (!vfs_path_is_prefix(path, path_len, mnt)) {
            continue;
        }
        if (mnt->path_len >= best_len) {
            best = mnt;
            best_len = mnt->path_len;
        }
    }

    return best;
}

static vfs_node_t *vfs_lookup_child(vfs_node_t *dir, const char *name)
{
    if (!dir) {
        return NULL;
    }

    if (dir->ops && dir->ops->lookup) {
        return dir->ops->lookup(dir, name);
    }

    return vfs_find_child(dir, name);
}

void vfs_init(void)
{
    g_mounts = NULL;
}

int vfs_mount(vfs_node_t *root, const char *path)
{
    if (!root || !path || path[0] != '/') {
        return VFS_ERR_INVAL;
    }

    size_t path_len = vfs_trim_path_len(path);
    if (path_len == 0) {
        return VFS_ERR_INVAL;
    }

    for (vfs_mount_t *mnt = g_mounts; mnt; mnt = mnt->next) {
        if (mnt->path_len == path_len &&
            memcmp(mnt->path, path, path_len) == 0) {
            return VFS_ERR_EXIST;
        }
    }

    if (path_len > 1 && g_mounts) {
        vfs_node_t *mp = NULL;
        int rc = vfs_resolve_path(path, &mp);
        if (rc < 0) {
            return rc;
        }
        if (!(mp->type & VFS_NODE_DIR)) {
            return VFS_ERR_NOTDIR;
        }
    }

    vfs_mount_t *mnt = kmalloc(sizeof(*mnt));
    if (!mnt) {
        return VFS_ERR_NOMEM;
    }

    mnt->path = kmalloc(path_len + 1);
    if (!mnt->path) {
        kfree(mnt);
        return VFS_ERR_NOMEM;
    }

    memcpy(mnt->path, path, path_len);
    mnt->path[path_len] = '\0';
    mnt->path_len = (uint16_t)path_len;
    mnt->root = root;

    mnt->next = g_mounts;
    g_mounts = mnt;

    return VFS_OK;
}

int vfs_unmount(const char *path)
{
    return vfs_unmount_detach(path, NULL);
}

int vfs_unmount_detach(const char *path, vfs_node_t **out_root)
{
    if (!path || path[0] != '/') {
        return VFS_ERR_INVAL;
    }

    size_t path_len = vfs_trim_path_len(path);
    vfs_mount_t *prev = NULL;

    for (vfs_mount_t *mnt = g_mounts; mnt; mnt = mnt->next) {
        if (mnt->path_len == path_len &&
            memcmp(mnt->path, path, path_len) == 0) {
            if (prev) {
                prev->next = mnt->next;
            } else {
                g_mounts = mnt->next;
            }
            if (out_root) {
                *out_root = mnt->root;
            }
            kfree(mnt->path);
            kfree(mnt);
            return VFS_OK;
        }
        prev = mnt;
    }

    return VFS_ERR_NOENT;
}

int vfs_resolve_path(const char *path, vfs_node_t **out_node)
{
    if (!path || !out_node || path[0] != '/') {
        return VFS_ERR_INVAL;
    }

    size_t path_len = vfs_trim_path_len(path);
    if (path_len == 0) {
        return VFS_ERR_INVAL;
    }

    vfs_mount_t *mnt = vfs_find_mount_for_path(path, path_len);
    if (!mnt || !mnt->root) {
        return VFS_ERR_NOENT;
    }

    const char *end = path + path_len;
    const char *p = path + mnt->path_len;
    if (mnt->path_len > 1 && p < end && *p == '/') {
        p++;
    }

    p = vfs_skip_slashes(p, end);

    vfs_node_t *node = mnt->root;
    if (p >= end) {
        *out_node = node;
        return VFS_OK;
    }

    while (p < end) {
        const char *seg = p;
        while (p < end && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg);

        if (seg_len == 0) {
            p = vfs_skip_slashes(p, end);
            continue;
        }

        if (seg_len > VFS_NAME_MAX) {
            return VFS_ERR_NAMETOOLONG;
        }

        char name[VFS_NAME_MAX + 1];
        memcpy(name, seg, seg_len);
        name[seg_len] = '\0';

        node = vfs_lookup_child(node, name);
        if (!node) {
            return VFS_ERR_NOENT;
        }

        p = vfs_skip_slashes(p, end);
        if (p < end && !(node->type & VFS_NODE_DIR)) {
            return VFS_ERR_NOTDIR;
        }
    }

    *out_node = node;
    return VFS_OK;
}

vfs_node_t *vfs_resolve(const char *path)
{
    vfs_node_t *node = NULL;

    if (vfs_resolve_path(path, &node) < 0) {
        return NULL;
    }

    return node;
}

int vfs_open(const char *path, uint32_t flags, vfs_file_t **out_file)
{
    if (!out_file) {
        return VFS_ERR_INVAL;
    }

    vfs_node_t *node = NULL;
    int rc = vfs_resolve_path(path, &node);
    if (rc < 0) {
        return rc;
    }

    vfs_file_t *file = kmalloc(sizeof(*file));
    if (!file) {
        return VFS_ERR_NOMEM;
    }

    memset(file, 0, sizeof(*file));
    file->node = node;
    file->flags = flags;
    file->pos = 0;

    if (node->ops && node->ops->open) {
        rc = node->ops->open(file, flags);
        if (rc < 0) {
            kfree(file);
            return rc;
        }
    }

    *out_file = file;
    return VFS_OK;
}

int vfs_close(vfs_file_t *file)
{
    if (!file) {
        return VFS_ERR_INVAL;
    }

    int rc = VFS_OK;
    if (file->node && file->node->ops && file->node->ops->close) {
        rc = file->node->ops->close(file);
    }

    kfree(file);
    return rc;
}

vfs_ssize_t vfs_read(vfs_file_t *file, void *buf, size_t len)
{
    if (!file || !file->node || !buf) {
        return VFS_ERR_INVAL;
    }

    if (!file->node->ops || !file->node->ops->read) {
        return VFS_ERR_NOTSUP;
    }

    vfs_ssize_t ret = file->node->ops->read(file, buf, len);
    if (ret > 0) {
        file->pos += (uint64_t)ret;
    }

    return ret;
}

vfs_ssize_t vfs_write(vfs_file_t *file, const void *buf, size_t len)
{
    if (!file || !file->node || !buf) {
        return VFS_ERR_INVAL;
    }

    if (!file->node->ops || !file->node->ops->write) {
        return VFS_ERR_NOTSUP;
    }

    vfs_ssize_t ret = file->node->ops->write(file, buf, len);
    if (ret > 0) {
        file->pos += (uint64_t)ret;
    }

    return ret;
}

int vfs_readdir(vfs_file_t *file, size_t index, vfs_dirent_t *out)
{
    if (!file || !file->node || !out) {
        return VFS_ERR_INVAL;
    }

    vfs_node_t *dir = file->node;
    if (!(dir->type & VFS_NODE_DIR)) {
        return VFS_ERR_NOTDIR;
    }

    if (dir->ops && dir->ops->readdir) {
        return dir->ops->readdir(dir, index, out);
    }

    vfs_node_t *child = dir->first_child;
    size_t i = 0;
    while (child && i < index) {
        child = child->next_sibling;
        i++;
    }

    if (!child) {
        return 0;
    }

    size_t name_len = child->name_len;
    if (name_len > VFS_NAME_MAX) {
        return VFS_ERR_NAMETOOLONG;
    }

    memcpy(out->name, child->name, name_len);
    out->name[name_len] = '\0';
    out->type = child->type;

    return 1;
}

static int vfs_stat_node(vfs_node_t *node, vfs_stat_t *out)
{
    if (!node || !out) {
        return VFS_ERR_INVAL;
    }

    if (node->ops && node->ops->stat) {
        int rc = node->ops->stat(node, out);
        if (rc < 0) {
            return rc;
        }
    } else {
        out->type = node->type;
        out->size = node->size;
    }

    return VFS_OK;
}

int vfs_stat(const char *path, vfs_stat_t *out)
{
    if (!path || !out) {
        return VFS_ERR_INVAL;
    }

    vfs_node_t *node = NULL;
    int rc = vfs_resolve_path(path, &node);
    if (rc < 0) {
        return rc;
    }

    return vfs_stat_node(node, out);
}

int vfs_fstat(vfs_file_t *file, vfs_stat_t *out)
{
    if (!file || !out || !file->node) {
        return VFS_ERR_INVAL;
    }

    return vfs_stat_node(file->node, out);
}

vfs_node_t *vfs_node_create(const char *name, uint32_t type,
                            const vfs_node_ops_t *ops, void *data)
{
    vfs_node_t *node = kmalloc(sizeof(*node));
    if (!node) {
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    node->type = type;
    node->ops = ops;
    node->data = data;

    size_t name_len = vfs_strlen(name ? name : "");
    if (name_len > VFS_NAME_MAX) {
        kfree(node);
        return NULL;
    }

    char *name_copy = kmalloc(name_len + 1);
    if (!name_copy) {
        kfree(node);
        return NULL;
    }

    if (name_len) {
        memcpy(name_copy, name, name_len);
    }
    name_copy[name_len] = '\0';

    node->name = name_copy;
    node->name_len = (uint16_t)name_len;
    node->flags |= VFS_NODE_NAME_OWNED;

    return node;
}

void vfs_node_destroy(vfs_node_t *node)
{
    if (!node) {
        return;
    }

    if ((node->flags & VFS_NODE_NAME_OWNED) && node->name) {
        kfree((void *)node->name);
    }

    kfree(node);
}

int vfs_add_child(vfs_node_t *parent, vfs_node_t *child)
{
    if (!parent || !child) {
        return VFS_ERR_INVAL;
    }

    if (!(parent->type & VFS_NODE_DIR)) {
        return VFS_ERR_NOTDIR;
    }

    child->parent = parent;
    child->next_sibling = parent->first_child;
    parent->first_child = child;

    return VFS_OK;
}

vfs_node_t *vfs_find_child(vfs_node_t *parent, const char *name)
{
    if (!parent || !name) {
        return NULL;
    }

    for (vfs_node_t *child = parent->first_child; child; child = child->next_sibling) {
        if (child->name && vfs_strcmp(child->name, name) == 0) {
            return child;
        }
    }

    return NULL;
}
