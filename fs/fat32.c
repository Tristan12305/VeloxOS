#include "fat32.h"
#include "partition/gpt.h"
#include <include/kmalloc.h>
#include <include/printk.h>
#include <lib/string.h>


// Convert a cluster number to a partition-relative LBA.
static uint64_t cluster_to_lba(const fat32_vol_t *vol, uint32_t cluster)
{
    return vol->data_start_lba
         + (uint64_t)(cluster - 2) * vol->bpb.sectors_per_cluster;
}

// Read `count` sectors at `relative_lba` (partition-relative) into `buf`.
static bool read_sectors(fat32_vol_t *vol,
                         uint64_t relative_lba, uint32_t count, void *buf)
{
    return gpt_read_partition_sectors(vol->gpt_partition_index,
                                      relative_lba, count, buf);
}


bool fat32_mount(int gpt_index, fat32_vol_t *vol)
{
    memset(vol, 0, sizeof(*vol));
    vol->gpt_partition_index = gpt_index;

    // --- read the VBR (partition-relative LBA 0) ---
    uint8_t sector[512];
    if (!gpt_read_partition_sectors(gpt_index, 0, 1, sector))
        return false;

    memcpy(&vol->bpb, sector, sizeof(fat32_bpb_t));
    fat32_bpb_t *b = &vol->bpb;


    // MBR/VBR boot signature at bytes 510-511
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        printk("[FAT32] partition %d: missing 0x55AA boot signature\n", gpt_index);
        return false;
    }

    // FAT32 specific: these fields must be zero
    if (b->root_entry_count != 0 || b->fat_size_16 != 0 || b->total_sectors_16 != 0) {
        printk("[FAT32] partition %d: not FAT32 (FAT12/16 fields non-zero)\n", gpt_index);
        return false;
    }

    if (b->fat_size_32 == 0) {
        printk("[FAT32] partition %d: fat_size_32 is 0\n", gpt_index);
        return false;
    }

    if (b->fs_version != 0x0000) {
        printk("[FAT32] partition %d: unsupported FAT32 version 0x%04x\n",
               gpt_index, b->fs_version);
        return false;
    }

    // Bytes-per-sector must be a power-of-2 in [512, 4096].
    uint16_t bps = b->bytes_per_sector;
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) != 0) {
        printk("[FAT32] partition %d: bogus bytes_per_sector %u\n", gpt_index, bps);
        return false;
    }

    // sectors_per_cluster must be a non-zero power of 2
    uint8_t spc = b->sectors_per_cluster;
    if (spc == 0 || (spc & (spc - 1)) != 0) {
        printk("[FAT32] partition %d: bogus sectors_per_cluster %u\n", gpt_index, spc);
        return false;
    }


    vol->bytes_per_cluster = (uint32_t)bps * spc;

    // FAT #1 starts immediately after the reserved sectors
    vol->fat_start_lba  = b->reserved_sectors;

    // data region starts after all FATs
    vol->data_start_lba = b->reserved_sectors
                        + (uint64_t)b->num_fats * b->fat_size_32;

    vol->root_cluster   = b->root_cluster;

    // total data clusters = (total_sectors - data_start) / sectors_per_cluster
    uint32_t total_sectors = b->total_sectors_32
                           ? b->total_sectors_32
                           : b->total_sectors_16;
    uint64_t data_sectors  = total_sectors - vol->data_start_lba;
    vol->cluster_count     = (uint32_t)(data_sectors / spc);

    printk("[FAT32] mounted partition %d\n", gpt_index);
    printk("[FAT32]   bytes/sector=%u  sectors/cluster=%u  bytes/cluster=%u\n",
           bps, spc, vol->bytes_per_cluster);
    printk("[FAT32]   FAT start LBA=%llu  data start LBA=%llu\n",
           vol->fat_start_lba, vol->data_start_lba);
    printk("[FAT32]   root cluster=%u  cluster count=%u\n",
           vol->root_cluster, vol->cluster_count);

    return true;
}


uint32_t fat32_read_fat_entry(fat32_vol_t *vol, uint32_t cluster)
{
    // Each FAT entry is 4 bytes.  Figure out which sector of the FAT
    // contains this entry, and the byte offset within that sector.
    uint32_t bps         = vol->bpb.bytes_per_sector;
    uint32_t entries_per_sector = bps / 4;

    uint64_t fat_sector  = vol->fat_start_lba + (cluster / entries_per_sector);
    uint32_t byte_offset = (cluster % entries_per_sector) * 4;

    // We only need one sector, so a single 512-byte (or bps-byte) stack buffer
    // is fine.  Guard against exotic sector sizes > 512 if you ever care.
    uint8_t sector[512];
    if (bps > sizeof(sector)) {
        printk("[FAT32] fat_entry: sector size %u exceeds read buffer\n", bps);
        return FAT32_BAD;
    }

    if (!read_sectors(vol, fat_sector, 1, sector)) {
        printk("[FAT32] fat_entry: I/O error reading FAT sector %llu\n", fat_sector);
        return FAT32_BAD;
    }

    uint32_t entry;
    memcpy(&entry, sector + byte_offset, 4);
    return entry & 0x0FFFFFFF;  // only 28 bits are defined
}


bool fat32_read_cluster(fat32_vol_t *vol, uint32_t cluster, void *buf)
{
    // Cluster 0 and 1 are reserved and never appear in a chain.
    if (cluster < 2 || cluster > vol->cluster_count + 1) {
        printk("[FAT32] read_cluster: cluster %u out of range\n", cluster);
        return false;
    }

    uint64_t lba = cluster_to_lba(vol, cluster);
    return read_sectors(vol, lba, vol->bpb.sectors_per_cluster, buf);
}


// Maximum LFN sequence number is 20 (20 * 13 = 260 chars, enough for 255+NUL).
#define LFN_MAX_SLOTS 20

// LFN accumulator: filled slot by slot as we scan backwards through the
// entries on disk, then flushed into the dirent name when we hit the 8.3 entry.
typedef struct {
    uint16_t chars[LFN_MAX_SLOTS * 13]; // UTF-16LE codepoints, in order
    int      total;                     // number of valid codepoints
    uint8_t  expected_seq;              // next sequence number we expect
    uint8_t  checksum;                  // 8.3 checksum the LFN entries claim
    bool     valid;                     // false if sequence broke or checksum mismatch
} lfn_state_t;

// Compute the standard FAT LFN checksum over an 8.3 name (11 bytes, space-padded).
static uint8_t lfn_checksum(const char name11[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name11[i];
    return sum;
}

// Append 13 UTF-16LE code units from an LFN entry into the accumulator.
// LFN entries arrive in *reverse* order on disk (highest seq first), so we
// prepend each block at the front of the buffer.
static void lfn_push(lfn_state_t *lfn, const fat32_lfn_entry_t *e)
{
    // Each LFN entry contributes 13 chars.  Shift existing chars right to make
    // room at the front.
    int n = 13;
    if (lfn->total + n > LFN_MAX_SLOTS * 13) {
        lfn->valid = false;
        return;
    }
    // Shift existing content right by 13 positions
    for (int i = lfn->total - 1; i >= 0; i--)
        lfn->chars[i + n] = lfn->chars[i];

    // Write the 13 UTF-16LE code units at the front
    lfn->chars[0]  = e->name1[0]; lfn->chars[1]  = e->name1[1];
    lfn->chars[2]  = e->name1[2]; lfn->chars[3]  = e->name1[3];
    lfn->chars[4]  = e->name1[4];
    lfn->chars[5]  = e->name2[0]; lfn->chars[6]  = e->name2[1];
    lfn->chars[7]  = e->name2[2]; lfn->chars[8]  = e->name2[3];
    lfn->chars[9]  = e->name2[4]; lfn->chars[10] = e->name2[5];
    lfn->chars[11] = e->name3[0]; lfn->chars[12] = e->name3[1];

    lfn->total += n;
}

// Naively encode a UTF-16LE string to UTF-8 (BMP only — enough for filenames).
// Stops at a NUL code unit or `src_len` code units.  `dst` must have room for
// at least src_len*3 + 1 bytes.  Returns the number of bytes written (excl NUL).
static int utf16le_to_utf8(const uint16_t *src, int src_len, char *dst)
{
    int out = 0;
    for (int i = 0; i < src_len; i++) {
        uint16_t cp = src[i];
        if (cp == 0x0000) break;            // NUL terminator

        if (cp < 0x0080) {
            dst[out++] = (char)cp;
        } else if (cp < 0x0800) {
            dst[out++] = (char)(0xC0 | (cp >> 6));
            dst[out++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[out++] = (char)(0xE0 | (cp >> 12));
            dst[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[out++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[out] = '\0';
    return out;
}

// Build the name from an 8.3 entry as a fallback when no valid LFN exists.
// Produces "NAME    .EXT" → "NAME.EXT", all lower-case (Windows NT convention).
static void build_83_name(const fat32_raw_entry_t *e, char *out)
{
    // Copy base name, strip trailing spaces
    int base_len = 8;
    while (base_len > 0 && e->name[base_len - 1] == ' ') base_len--;

    int i = 0;
    for (int j = 0; j < base_len; j++) {
        char c = e->name[j];
        // lower-case ASCII letters (Windows NT style)
        if (c >= 'A' && c <= 'Z') c += 32;
        out[i++] = c;
    }

    // Append extension if it isn't all spaces
    int ext_len = 3;
    while (ext_len > 0 && e->ext[ext_len - 1] == ' ') ext_len--;
    if (ext_len > 0) {
        out[i++] = '.';
        for (int j = 0; j < ext_len; j++) {
            char c = e->ext[j];
            if (c >= 'A' && c <= 'Z') c += 32;
            out[i++] = c;
        }
    }
    out[i] = '\0';
}

// Process one cluster-worth of raw directory bytes.
// Updates `lfn`, writes complete dirents into `out[]`.
// Returns false on a hard error.  `done` is set to true if we hit a
// terminal 0x00 entry (no more entries exist anywhere in this directory).
static bool process_dir_cluster(fat32_vol_t       *vol,
                                 const uint8_t     *cluster_buf,
                                 lfn_state_t       *lfn,
                                 fat32_dirent_t    *out,
                                 uint32_t           max_entries,
                                 uint32_t          *count,
                                 bool              *done)
{
    uint32_t entries_per_cluster = vol->bytes_per_cluster / 32;

    for (uint32_t i = 0; i < entries_per_cluster; i++) {
        const fat32_raw_entry_t *raw =
            (const fat32_raw_entry_t *)(cluster_buf + i * 32);

        uint8_t first = (uint8_t)raw->name[0];

        // 0x00 = this slot and all after it are free — directory is done
        if (first == 0x00) {
            *done = true;
            return true;
        }

        // 0xE5 = deleted entry, skip
        if (first == 0xE5) {
            lfn->valid = false;
            lfn->total = 0;
            continue;
        }

        // LFN fragment
        if (raw->attributes == FAT_ATTR_LFN) {
            const fat32_lfn_entry_t *le = (const fat32_lfn_entry_t *)raw;
            uint8_t seq = le->sequence;

            if (seq & 0x40) {
                // First LFN entry we've seen for this file (last in the chain,
                // highest sequence number).  Initialise the accumulator.
                lfn->total        = 0;
                lfn->expected_seq = seq & ~0x40;  // strip the "last" bit
                lfn->checksum     = le->checksum;
                lfn->valid        = true;
            }

            // Validate sequence continuity
            if (!lfn->valid || (seq & ~0x40) != lfn->expected_seq) {
                lfn->valid = false;
            }

            if (lfn->valid) {
                lfn_push(lfn, le);
                lfn->expected_seq--;
            }
            continue;
        }

        // Volume label — skip and reset LFN state
        if (raw->attributes & FAT_ATTR_VOLUME_ID) {
            lfn->valid = false;
            lfn->total = 0;
            continue;
        }

        // Regular 8.3 entry (file or directory).
        // Stop if the caller's buffer is full.
        if (*count >= max_entries) {
            lfn->valid = false;
            lfn->total = 0;
            continue;
        }

        fat32_dirent_t *d = &out[(*count)++];
        memset(d, 0, sizeof(*d));

        bool used_lfn = false;
        if (lfn->valid && lfn->total > 0 && lfn->expected_seq == 0) {
            // Verify checksum before trusting the LFN
            char name11[11];
            memcpy(name11, raw->name, 8);
            memcpy(name11 + 8, raw->ext, 3);
            if (lfn_checksum(name11) == lfn->checksum) {
                utf16le_to_utf8(lfn->chars, lfn->total, d->name);
                used_lfn = true;
            }
        }
        if (!used_lfn)
            build_83_name(raw, d->name);

        // --- attributes, cluster, size ---
        d->attributes    = raw->attributes;
        d->first_cluster = ((uint32_t)raw->cluster_high << 16) | raw->cluster_low;
        d->file_size     = raw->file_size;

        // --- timestamps ---
        d->created.raw_date  = raw->create_date;
        d->created.raw_time  = raw->create_time;
        d->modified.raw_date = raw->write_date;
        d->modified.raw_time = raw->write_time;
        d->accessed.raw_date = raw->access_date;
        d->accessed.raw_time = 0;   // FAT stores access date only

        // Reset LFN accumulator for the next entry
        lfn->valid = false;
        lfn->total = 0;
    }

    return true;
}

bool fat32_read_dir(fat32_vol_t *vol, uint32_t dir_cluster,
                    fat32_dirent_t *out, uint32_t max_entries,
                    uint32_t *count_out)
{
    *count_out = 0;

    // Allocate cluster buffer on the heap (clusters can be up to 32 KiB).
    // If your kernel doesn't have kmalloc yet, you can make this a static
    // buffer — just not stack-allocated for large cluster sizes.
    uint8_t *cluster_buf = kmalloc(vol->bytes_per_cluster);
    if (!cluster_buf) {
        printk("[FAT32] read_dir: out of memory\n");
        return false;
    }

    lfn_state_t lfn = { .valid = false, .total = 0 };
    uint32_t    cluster = dir_cluster;
    bool        done    = false;

    while (!done && cluster >= 2 && cluster < FAT32_BAD) {
        if (!fat32_read_cluster(vol, cluster, cluster_buf)) {
            printk("[FAT32] read_dir: I/O error on cluster %u\n", cluster);
            kfree(cluster_buf);
            return false;
        }

        if (!process_dir_cluster(vol, cluster_buf, &lfn,
                                  out, max_entries, count_out, &done)) {
            kfree(cluster_buf);
            return false;
        }

        if (done) break;

        // Walk the FAT chain to the next cluster
        uint32_t next = fat32_read_fat_entry(vol, cluster);
        if (next == FAT32_BAD) {
            printk("[FAT32] read_dir: FAT I/O error at cluster %u\n", cluster);
            kfree(cluster_buf);
            return false;
        }
        cluster = next;   // FAT32_EOC (>= 0x0FFFFFF8) will exit the loop
    }

    kfree(cluster_buf);
    return true;
}

static char fat32_tolower(char c)
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
        if (fat32_tolower(*a) != fat32_tolower(*b)) {
            return false;
        }
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static const char *fat32_skip_slashes(const char *p)
{
    while (p && *p == '/') {
        p++;
    }
    return p;
}

static int fat32_copy_segment(const char *p, char *out, size_t out_cap,
                              const char **next)
{
    size_t len = 0;

    if (!p || !out || out_cap == 0) {
        return FAT32_ERR_INVAL;
    }

    while (p[len] && p[len] != '/') {
        if (len + 1 >= out_cap) {
            return FAT32_ERR_NAMETOOLONG;
        }
        out[len] = p[len];
        len++;
    }

    out[len] = '\0';
    if (next) {
        *next = p + len;
    }

    return (int)len;
}

typedef int (*fat32_dir_iter_cb)(const fat32_dirent_t *ent, void *ctx);

typedef struct {
    const char *name;
    fat32_dirent_t *out;
    bool found;
} fat32_lookup_ctx_t;

typedef struct {
    size_t target;
    size_t current;
    fat32_dirent_t *out;
    bool found;
} fat32_readdir_ctx_t;

static int fat32_lookup_cb(const fat32_dirent_t *ent, void *ctx_ptr)
{
    fat32_lookup_ctx_t *ctx = (fat32_lookup_ctx_t *)ctx_ptr;
    if (fat32_name_equal_ci(ent->name, ctx->name)) {
        *ctx->out = *ent;
        ctx->found = true;
        return 1;
    }
    return 0;
}

static int fat32_readdir_cb(const fat32_dirent_t *ent, void *ctx_ptr)
{
    fat32_readdir_ctx_t *ctx = (fat32_readdir_ctx_t *)ctx_ptr;
    if (ctx->current == ctx->target) {
        *ctx->out = *ent;
        ctx->found = true;
        return 1;
    }
    ctx->current++;
    return 0;
}

static int fat32_iter_dir(fat32_vol_t *vol, uint32_t dir_cluster,
                          fat32_dir_iter_cb cb, void *ctx)
{
    if (!vol || !cb) {
        return FAT32_ERR_INVAL;
    }

    if (dir_cluster < 2) {
        return FAT32_ERR_INVAL;
    }

    uint8_t *cluster_buf = kmalloc(vol->bytes_per_cluster);
    if (!cluster_buf) {
        printk("[FAT32] iter_dir: out of memory\n");
        return FAT32_ERR_NOMEM;
    }

    lfn_state_t lfn = { .valid = false, .total = 0 };
    uint32_t cluster = dir_cluster;
    bool done = false;
    int rc = FAT32_OK;

    while (!done && cluster >= 2 && cluster < FAT32_BAD) {
        if (!fat32_read_cluster(vol, cluster, cluster_buf)) {
            printk("[FAT32] iter_dir: I/O error on cluster %u\n", cluster);
            rc = FAT32_ERR_IO;
            break;
        }

        uint32_t entries_per_cluster = vol->bytes_per_cluster / 32;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            const fat32_raw_entry_t *raw =
                (const fat32_raw_entry_t *)(cluster_buf + i * 32);

            uint8_t first = (uint8_t)raw->name[0];

            if (first == 0x00) {
                done = true;
                break;
            }

            if (first == 0xE5) {
                lfn.valid = false;
                lfn.total = 0;
                continue;
            }

            if (raw->attributes == FAT_ATTR_LFN) {
                const fat32_lfn_entry_t *le = (const fat32_lfn_entry_t *)raw;
                uint8_t seq = le->sequence;

                if (seq & 0x40) {
                    lfn.total        = 0;
                    lfn.expected_seq = seq & ~0x40;
                    lfn.checksum     = le->checksum;
                    lfn.valid        = true;
                }

                if (!lfn.valid || (seq & ~0x40) != lfn.expected_seq) {
                    lfn.valid = false;
                }

                if (lfn.valid) {
                    lfn_push(&lfn, le);
                    lfn.expected_seq--;
                }
                continue;
            }

            if (raw->attributes & FAT_ATTR_VOLUME_ID) {
                lfn.valid = false;
                lfn.total = 0;
                continue;
            }

            fat32_dirent_t d;
            memset(&d, 0, sizeof(d));

            bool used_lfn = false;
            if (lfn.valid && lfn.total > 0 && lfn.expected_seq == 0) {
                char name11[11];
                memcpy(name11, raw->name, 8);
                memcpy(name11 + 8, raw->ext, 3);
                if (lfn_checksum(name11) == lfn.checksum) {
                    utf16le_to_utf8(lfn.chars, lfn.total, d.name);
                    used_lfn = true;
                }
            }
            if (!used_lfn) {
                build_83_name(raw, d.name);
            }

            d.attributes    = raw->attributes;
            d.first_cluster = ((uint32_t)raw->cluster_high << 16) | raw->cluster_low;
            d.file_size     = raw->file_size;

            d.created.raw_date  = raw->create_date;
            d.created.raw_time  = raw->create_time;
            d.modified.raw_date = raw->write_date;
            d.modified.raw_time = raw->write_time;
            d.accessed.raw_date = raw->access_date;
            d.accessed.raw_time = 0;

            lfn.valid = false;
            lfn.total = 0;

            int cb_rc = cb(&d, ctx);
            if (cb_rc < 0) {
                rc = cb_rc;
                done = true;
                break;
            }
            if (cb_rc > 0) {
                done = true;
                break;
            }
        }

        if (done) {
            break;
        }

        uint32_t next = fat32_read_fat_entry(vol, cluster);
        if (next == FAT32_BAD) {
            printk("[FAT32] iter_dir: FAT I/O error at cluster %u\n", cluster);
            rc = FAT32_ERR_IO;
            break;
        }
        if (next >= FAT32_EOC) {
            break;
        }
        cluster = next;
    }

    kfree(cluster_buf);
    return rc;
}

int fat32_lookup(fat32_vol_t *vol, uint32_t dir_cluster,
                 const char *name, fat32_dirent_t *out)
{
    if (!vol || !name || !out) {
        return FAT32_ERR_INVAL;
    }

    fat32_lookup_ctx_t ctx = {
        .name = name,
        .out = out,
        .found = false,
    };

    int rc = fat32_iter_dir(vol, dir_cluster, fat32_lookup_cb, &ctx);
    if (rc < 0) {
        return rc;
    }
    return ctx.found ? FAT32_OK : FAT32_ERR_NOENT;
}

int fat32_readdir_index(fat32_vol_t *vol, uint32_t dir_cluster,
                        size_t index, fat32_dirent_t *out)
{
    if (!vol || !out) {
        return FAT32_ERR_INVAL;
    }

    fat32_readdir_ctx_t ctx = {
        .target = index,
        .current = 0,
        .out = out,
        .found = false,
    };

    int rc = fat32_iter_dir(vol, dir_cluster, fat32_readdir_cb, &ctx);
    if (rc < 0) {
        return rc;
    }
    return ctx.found ? 1 : 0;
}

static void fat32_make_root_dirent(const fat32_vol_t *vol, fat32_dirent_t *out)
{
    memset(out, 0, sizeof(*out));
    out->name[0] = '/';
    out->name[1] = '\0';
    out->attributes = FAT_ATTR_DIRECTORY;
    out->first_cluster = vol->root_cluster;
    out->file_size = 0;
}

int fat32_lookup_path(fat32_vol_t *vol, const char *path, fat32_dirent_t *out)
{
    if (!vol || !out) {
        return FAT32_ERR_INVAL;
    }

    if (!path || path[0] == '\0') {
        fat32_make_root_dirent(vol, out);
        return FAT32_OK;
    }

    const char *p = fat32_skip_slashes(path);
    if (!p || *p == '\0') {
        fat32_make_root_dirent(vol, out);
        return FAT32_OK;
    }

    uint32_t dir_cluster = vol->root_cluster;
    fat32_dirent_t current;

    while (p && *p) {
        char name[FAT32_NAME_MAX + 1];
        int seg_len = fat32_copy_segment(p, name, sizeof(name), &p);
        if (seg_len < 0) {
            return seg_len;
        }

        int rc = fat32_lookup(vol, dir_cluster, name, &current);
        if (rc < 0) {
            return rc;
        }

        p = fat32_skip_slashes(p);
        if (p && *p) {
            if (!(current.attributes & FAT_ATTR_DIRECTORY)) {
                return FAT32_ERR_NOTDIR;
            }
            if (current.first_cluster < 2) {
                return FAT32_ERR_IO;
            }
            dir_cluster = current.first_cluster;
        }
    }

    *out = current;
    return FAT32_OK;
}

int fat32_stat_path(fat32_vol_t *vol, const char *path, fat32_dirent_t *out)
{
    return fat32_lookup_path(vol, path, out);
}

fat32_ssize_t fat32_read_file_at(fat32_vol_t *vol,
                                 uint32_t first_cluster,
                                 uint32_t file_size,
                                 uint64_t offset,
                                 void *buf, size_t len)
{
    if (!vol || !buf) {
        return FAT32_ERR_INVAL;
    }

    if (offset >= file_size || len == 0) {
        return 0;
    }

    if (file_size == 0) {
        return 0;
    }

    if (first_cluster < 2) {
        return FAT32_ERR_IO;
    }

    uint64_t remaining = (uint64_t)file_size - offset;
    if (len > remaining) {
        len = (size_t)remaining;
    }

    uint8_t *cluster_buf = kmalloc(vol->bytes_per_cluster);
    if (!cluster_buf) {
        printk("[FAT32] read_file: out of memory\n");
        return FAT32_ERR_NOMEM;
    }

    uint64_t cluster_size = vol->bytes_per_cluster;
    uint32_t cluster = first_cluster;
    uint64_t skip = offset;

    while (skip >= cluster_size) {
        uint32_t next = fat32_read_fat_entry(vol, cluster);
        if (next == FAT32_BAD) {
            printk("[FAT32] read_file: FAT I/O error at cluster %u\n", cluster);
            kfree(cluster_buf);
            return FAT32_ERR_IO;
        }
        if (next >= FAT32_EOC) {
            kfree(cluster_buf);
            return 0;
        }
        cluster = next;
        skip -= cluster_size;
    }

    size_t total_read = 0;
    size_t to_read = len;
    uint64_t intra = skip;

    while (to_read > 0 && cluster >= 2 && cluster < FAT32_EOC) {
        if (!fat32_read_cluster(vol, cluster, cluster_buf)) {
            printk("[FAT32] read_file: I/O error on cluster %u\n", cluster);
            kfree(cluster_buf);
            return FAT32_ERR_IO;
        }

        size_t avail = (size_t)cluster_size - (size_t)intra;
        size_t chunk = (to_read < avail) ? to_read : avail;

        memcpy((uint8_t *)buf + total_read, cluster_buf + intra, chunk);
        total_read += chunk;
        to_read -= chunk;
        intra = 0;

        if (to_read == 0) {
            break;
        }

        uint32_t next = fat32_read_fat_entry(vol, cluster);
        if (next == FAT32_BAD) {
            printk("[FAT32] read_file: FAT I/O error at cluster %u\n", cluster);
            kfree(cluster_buf);
            return FAT32_ERR_IO;
        }
        if (next >= FAT32_EOC) {
            break;
        }
        cluster = next;
    }

    kfree(cluster_buf);
    return (fat32_ssize_t)total_read;
}

int fat32_read_dir_path(fat32_vol_t *vol, const char *path,
                        fat32_dirent_t *out, uint32_t max_entries,
                        uint32_t *count_out)
{
    if (!vol || !out || !count_out) {
        return FAT32_ERR_INVAL;
    }

    fat32_dirent_t dirent;
    int rc = fat32_lookup_path(vol, path, &dirent);
    if (rc < 0) {
        return rc;
    }

    if (!(dirent.attributes & FAT_ATTR_DIRECTORY)) {
        return FAT32_ERR_NOTDIR;
    }

    uint32_t dir_cluster = dirent.first_cluster;
    if (dir_cluster < 2) {
        dir_cluster = vol->root_cluster;
    }

    if (!fat32_read_dir(vol, dir_cluster, out, max_entries, count_out)) {
        return FAT32_ERR_IO;
    }

    return FAT32_OK;
}
