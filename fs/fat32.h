#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    uint8_t  jump[3];
    char     oem_id[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;      // 0 on FAT32
    uint16_t total_sectors_16;      // 0 on FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;           // 0 on FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended BPB (offset 36)
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;            // must be 0x0000
    uint32_t root_cluster;          // first cluster of root dir, usually 2
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;        // 0x29 if volume_id/label/fs_type are valid
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];            // "FAT32   " — don't use to detect FAT32
} __attribute__((packed)) fat32_bpb_t;

// Computed once during mount, used by every subsequent operation.

typedef struct {
    int      gpt_partition_index;   // which GPT partition this volume lives on

    // raw BPB, kept around for callers that want to inspect it
    fat32_bpb_t bpb;

    // derived / cached fields
    uint32_t bytes_per_cluster;
    uint64_t fat_start_lba;         // relative to partition
    uint64_t data_start_lba;        // relative to partition
    uint32_t root_cluster;          // first cluster of the root directory
    uint32_t cluster_count;         // total data clusters on the volume
} fat32_vol_t;

// FAT entry sentinels (28-bit values after masking with 0x0FFFFFFF)
#define FAT32_EOC       0x0FFFFFF8  // end-of-chain (>= this value)
#define FAT32_BAD       0x0FFFFFF7  // bad cluster
#define FAT32_FREE      0x00000000  // unallocated


#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F    // all four low bits set = LFN entry



typedef struct {
    char     name[8];           // space-padded, 0xE5 = deleted, 0x00 = end
    char     ext[3];            // space-padded extension
    uint8_t  attributes;
    uint8_t  reserved_nt;       // Windows NT casing flags, ignore
    uint8_t  create_time_tenth; // tenths of a second on creation
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high;      // high 16 bits of first cluster
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_low;       // low 16 bits of first cluster
    uint32_t file_size;         // 0 for directories
} __attribute__((packed)) fat32_raw_entry_t;

// LFN entry (also 32 bytes, attributes == FAT_ATTR_LFN)
typedef struct {
    uint8_t  sequence;          // 1-based; bit 6 set on the last LFN entry
                                // (i.e. the one that comes first on disk)
    uint16_t name1[5];          // chars 1-5   (UTF-16LE)
    uint8_t  attributes;        // always 0x0F
    uint8_t  type;              // always 0x00
    uint8_t  checksum;          // checksum of the 8.3 name
    uint16_t name2[6];          // chars 6-11  (UTF-16LE)
    uint16_t cluster_zero;      // always 0x0000
    uint16_t name3[2];          // chars 12-13 (UTF-16LE)
} __attribute__((packed)) fat32_lfn_entry_t;

// FAT dates/times are packed into 16-bit fields.

typedef struct {
    uint16_t raw_date;  // bits 15-9 = year since 1980, 8-5 = month, 4-0 = day
    uint16_t raw_time;  // bits 15-11 = hour, 10-5 = minute, 4-0 = seconds/2
} fat32_timestamp_t;

// What fat32_read_dir returns: one of these per visible entry.

#define FAT32_NAME_MAX  256     // max UTF-8 bytes for a name (LFN = 255 UTF-16
                                // code units → up to 255*3 UTF-8 bytes, but
                                // in practice 255+NUL is enough)

typedef struct {
    char     name[FAT32_NAME_MAX]; // NUL-terminated UTF-8 name
    uint8_t  attributes;           // FAT_ATTR_* flags from the 8.3 entry
    uint32_t first_cluster;        // 0 for an empty file
    uint32_t file_size;            // bytes; 0 for directories
    fat32_timestamp_t created;
    fat32_timestamp_t modified;
    fat32_timestamp_t accessed;    // FAT only stores an access *date*, no time;
                                   // raw_time will be 0
} fat32_dirent_t;

// High-level FAT32 API error codes (negative, POSIX-ish).
#define FAT32_OK               0
#define FAT32_ERR_NOENT       -2
#define FAT32_ERR_IO          -5
#define FAT32_ERR_NOMEM       -12
#define FAT32_ERR_EXIST       -17
#define FAT32_ERR_NOTDIR      -20
#define FAT32_ERR_INVAL       -22
#define FAT32_ERR_NAMETOOLONG -36
#define FAT32_ERR_NOTSUP      -95

typedef int64_t fat32_ssize_t;

// Initialise a volume context by reading + validating the VBR on the given
// GPT partition.  Returns false if the partition doesn't look like FAT32.
bool fat32_mount(int gpt_index, fat32_vol_t *vol);

// Read the 28-bit FAT entry for `cluster`.
// Returns FAT32_FREE / FAT32_BAD / FAT32_EOC / next cluster number.
// Returns FAT32_BAD on I/O error.
uint32_t fat32_read_fat_entry(fat32_vol_t *vol, uint32_t cluster);

// Read all sectors of `cluster` into `buf`.
// buf must be at least vol->bytes_per_cluster bytes.
// Returns false on I/O error or out-of-range cluster.
bool fat32_read_cluster(fat32_vol_t *vol, uint32_t cluster, void *buf);

// Read every non-deleted, non-LFN-fragment entry in a directory.
//
// `dir_cluster` — first cluster of the directory (use vol->root_cluster for /).
// `out`         — caller-allocated array of fat32_dirent_t.
// `max_entries` — capacity of `out`.
// `count_out`   — set to the number of entries written on success.
//
// Returns false on I/O error.  If the directory contains more entries than
// `max_entries`, only the first `max_entries` are written and the function
// still returns true (check *count_out == max_entries as a hint to retry
// with a larger buffer).
bool fat32_read_dir(fat32_vol_t *vol, uint32_t dir_cluster,
                    fat32_dirent_t *out, uint32_t max_entries,
                    uint32_t *count_out);

// Resolve a path (absolute or relative to volume root) into a directory entry.
// For "/" or empty path, returns a synthetic entry for the root directory.
int fat32_lookup_path(fat32_vol_t *vol, const char *path, fat32_dirent_t *out);

// Lookup a single child by name inside a directory cluster.
int fat32_lookup(fat32_vol_t *vol, uint32_t dir_cluster,
                 const char *name, fat32_dirent_t *out);

// Read the Nth visible entry in a directory (0-based).
// Returns 1 if an entry was written, 0 if index is past end, <0 on error.
int fat32_readdir_index(fat32_vol_t *vol, uint32_t dir_cluster,
                        size_t index, fat32_dirent_t *out);

// Convenience wrapper around fat32_lookup_path for stat-like queries.
int fat32_stat_path(fat32_vol_t *vol, const char *path, fat32_dirent_t *out);

// Read file contents at an offset. Returns bytes read or <0 on error.
fat32_ssize_t fat32_read_file_at(fat32_vol_t *vol,
                                 uint32_t first_cluster,
                                 uint32_t file_size,
                                 uint64_t offset,
                                 void *buf, size_t len);

// Read directory entries by path.
int fat32_read_dir_path(fat32_vol_t *vol, const char *path,
                        fat32_dirent_t *out, uint32_t max_entries,
                        uint32_t *count_out);
