#include "fat32_vfs.h"

#include <include/kmalloc.h>
#include <lib/string.h>

typedef struct {
    fat32_vol_t *vol;
    uint32_t first_cluster;
    uint32_t size;
    uint8_t attributes;
    fat32_dirent_t *dir_cache;
    uint32_t dir_cache_count;
    uint32_t dir_cache_cap;
    bool dir_cache_loaded;
} fat32_vfs_inode_t;

static char fat32_ci_tolower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + 32);
    }
    return c;
}

static bool fat32_name_equal_ci(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }

    while (*a && *b) {
        if (fat32_ci_tolower(*a) != fat32_ci_tolower(*b)) {
            return false;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static vfs_node_t *fat32_find_child_ci(vfs_node_t *parent, const char *name)
{
    if (!parent || !name) {
        return NULL;
    }

    for (vfs_node_t *child = parent->first_child; child; child = child->next_sibling) {
        if (child->name && fat32_name_equal_ci(child->name, name)) {
            return child;
        }
    }

    return NULL;
}

static vfs_node_t *fat32_vfs_make_node(fat32_vol_t *vol, const fat32_dirent_t *ent);

static int fat32_vfs_open(vfs_file_t *file, uint32_t flags)
{
    (void)file;
    (void)flags;
    return VFS_OK;
}

static vfs_ssize_t fat32_vfs_read(vfs_file_t *file, void *buf, size_t len)
{
    if (!file || !file->node || !buf) {
        return VFS_ERR_INVAL;
    }

    fat32_vfs_inode_t *inode = (fat32_vfs_inode_t *)file->node->data;
    if (!inode) {
        return VFS_ERR_IO;
    }

    return (vfs_ssize_t)fat32_read_file_at(inode->vol,
                                           inode->first_cluster,
                                           inode->size,
                                           file->pos,
                                           buf, len);
}

static vfs_ssize_t fat32_vfs_write(vfs_file_t *file, const void *buf, size_t len)
{
    (void)file;
    (void)buf;
    (void)len;
    return VFS_ERR_NOTSUP;
}

static int fat32_vfs_load_dir_cache(fat32_vfs_inode_t *inode)
{
    if (!inode) {
        return VFS_ERR_INVAL;
    }

    if (inode->dir_cache_loaded) {
        return VFS_OK;
    }

    uint32_t cap = 64;
    while (cap > 0) {
        fat32_dirent_t *entries = kmalloc(sizeof(*entries) * cap);
        if (!entries) {
            return VFS_ERR_NOMEM;
        }

        uint32_t count = 0;
        if (!fat32_read_dir(inode->vol, inode->first_cluster,
                            entries, cap, &count)) {
            kfree(entries);
            return VFS_ERR_IO;
        }

        if (count < cap) {
            inode->dir_cache = entries;
            inode->dir_cache_count = count;
            inode->dir_cache_cap = cap;
            inode->dir_cache_loaded = true;
            return VFS_OK;
        }

        kfree(entries);
        cap *= 2;
    }

    return VFS_ERR_NOMEM;
}

static int fat32_vfs_readdir(vfs_node_t *dir, size_t index, vfs_dirent_t *out)
{
    if (!dir || !out) {
        return VFS_ERR_INVAL;
    }

    fat32_vfs_inode_t *inode = (fat32_vfs_inode_t *)dir->data;
    if (!inode) {
        return VFS_ERR_IO;
    }

    if (!inode->dir_cache_loaded) {
        int cache_rc = fat32_vfs_load_dir_cache(inode);
        if (cache_rc == VFS_ERR_NOMEM) {
            // Fallback to on-demand lookup if we cannot cache.
        } else if (cache_rc < 0) {
            return cache_rc;
        }
    }

    fat32_dirent_t ent;
    if (inode->dir_cache_loaded) {
        if (index >= inode->dir_cache_count) {
            return 0;
        }
        ent = inode->dir_cache[index];
    } else {
        int rc = fat32_readdir_index(inode->vol, inode->first_cluster, index, &ent);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return 0;
        }
    }

    size_t name_len = 0;
    while (name_len < VFS_NAME_MAX && ent.name[name_len]) {
        name_len++;
    }
    if (ent.name[name_len] != '\0') {
        return VFS_ERR_NAMETOOLONG;
    }

    memcpy(out->name, ent.name, name_len);
    out->name[name_len] = '\0';
    out->type = (ent.attributes & FAT_ATTR_DIRECTORY) ? VFS_NODE_DIR : VFS_NODE_FILE;

    return 1;
}

static vfs_node_t *fat32_vfs_lookup(vfs_node_t *dir, const char *name)
{
    if (!dir || !name) {
        return NULL;
    }

    vfs_node_t *cached = fat32_find_child_ci(dir, name);
    if (cached) {
        return cached;
    }

    fat32_vfs_inode_t *inode = (fat32_vfs_inode_t *)dir->data;
    if (!inode) {
        return NULL;
    }

    fat32_dirent_t ent;
    if (inode->dir_cache_loaded && inode->dir_cache) {
        bool found = false;
        for (uint32_t i = 0; i < inode->dir_cache_count; i++) {
            if (fat32_name_equal_ci(inode->dir_cache[i].name, name)) {
                ent = inode->dir_cache[i];
                found = true;
                break;
            }
        }
        if (!found) {
            return NULL;
        }
    } else {
        if (fat32_lookup(inode->vol, inode->first_cluster, name, &ent) < 0) {
            return NULL;
        }
    }

    vfs_node_t *child = fat32_vfs_make_node(inode->vol, &ent);
    if (!child) {
        return NULL;
    }

    if (vfs_add_child(dir, child) < 0) {
        vfs_node_destroy(child);
        return NULL;
    }

    return child;
}

static int fat32_vfs_stat(vfs_node_t *node, vfs_stat_t *out_stat)
{
    if (!node || !out_stat) {
        return VFS_ERR_INVAL;
    }

    vfs_stat_t *st = out_stat;
    fat32_vfs_inode_t *inode = (fat32_vfs_inode_t *)node->data;
    if (!inode) {
        return VFS_ERR_IO;
    }

    if (node->parent && node->parent->data) {
        fat32_vfs_inode_t *parent = (fat32_vfs_inode_t *)node->parent->data;
        fat32_dirent_t ent;
        if (fat32_lookup(parent->vol, parent->first_cluster, node->name, &ent) == FAT32_OK) {
            inode->size = ent.file_size;
            node->size = ent.file_size;
            inode->attributes = ent.attributes;
        }
    }

    st->type = node->type;
    st->size = node->size;
    return VFS_OK;
}

static const vfs_node_ops_t fat32_dir_ops = {
    .open = fat32_vfs_open,
    .read = NULL,
    .write = NULL,
    .readdir = fat32_vfs_readdir,
    .lookup = fat32_vfs_lookup,
    .stat = fat32_vfs_stat,
};

static const vfs_node_ops_t fat32_file_ops = {
    .open = fat32_vfs_open,
    .read = fat32_vfs_read,
    .write = fat32_vfs_write,
    .readdir = NULL,
    .lookup = NULL,
    .stat = fat32_vfs_stat,
};

static vfs_node_t *fat32_vfs_make_node(fat32_vol_t *vol, const fat32_dirent_t *ent)
{
    if (!vol || !ent) {
        return NULL;
    }

    uint32_t type = (ent->attributes & FAT_ATTR_DIRECTORY) ? VFS_NODE_DIR : VFS_NODE_FILE;
    const vfs_node_ops_t *ops = (type & VFS_NODE_DIR) ? &fat32_dir_ops : &fat32_file_ops;

    vfs_node_t *node = vfs_node_create(ent->name, type, ops, NULL);
    if (!node) {
        return NULL;
    }

    fat32_vfs_inode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) {
        vfs_node_destroy(node);
        return NULL;
    }

    inode->vol = vol;
    inode->first_cluster = ent->first_cluster;
    inode->size = ent->file_size;
    inode->attributes = ent->attributes;
    inode->dir_cache = NULL;
    inode->dir_cache_count = 0;
    inode->dir_cache_cap = 0;
    inode->dir_cache_loaded = false;

    node->data = inode;
    node->size = ent->file_size;

    return node;
}

int fat32_vfs_mount(fat32_vol_t *vol, const char *path)
{
    if (!vol || !path) {
        return VFS_ERR_INVAL;
    }

    vfs_node_t *root = vfs_node_create("", VFS_NODE_DIR, &fat32_dir_ops, NULL);
    if (!root) {
        return VFS_ERR_NOMEM;
    }

    fat32_vfs_inode_t *inode = kmalloc(sizeof(*inode));
    if (!inode) {
        vfs_node_destroy(root);
        return VFS_ERR_NOMEM;
    }

    inode->vol = vol;
    inode->first_cluster = vol->root_cluster;
    inode->size = 0;
    inode->attributes = FAT_ATTR_DIRECTORY;
    inode->dir_cache = NULL;
    inode->dir_cache_count = 0;
    inode->dir_cache_cap = 0;
    inode->dir_cache_loaded = false;

    root->data = inode;

    int rc = vfs_mount(root, path);
    if (rc < 0) {
        kfree(inode);
        vfs_node_destroy(root);
        return rc;
    }

    return VFS_OK;
}

static void fat32_vfs_free_tree(vfs_node_t *node)
{
    if (!node) {
        return;
    }

    vfs_node_t *child = node->first_child;
    while (child) {
        vfs_node_t *next = child->next_sibling;
        fat32_vfs_free_tree(child);
        child = next;
    }

    fat32_vfs_inode_t *inode = (fat32_vfs_inode_t *)node->data;
    if (inode) {
        if (inode->dir_cache) {
            kfree(inode->dir_cache);
        }
        kfree(inode);
    }

    vfs_node_destroy(node);
}

int fat32_vfs_unmount(const char *path)
{
    vfs_node_t *root = NULL;
    int rc = vfs_unmount_detach(path, &root);
    if (rc < 0) {
        return rc;
    }

    fat32_vfs_free_tree(root);
    return VFS_OK;
}
