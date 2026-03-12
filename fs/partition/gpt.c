#include <arch/x86_64/drivers/block/virtio/vblk.h>
#include "gpt.h"
#include <include/printk.h>
#include <lib/string.h>


typedef struct {
    char signature[8];
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;

    uint64_t current_lba;
    uint64_t backup_lba;

    uint64_t first_usable_lba;
    uint64_t last_usable_lba;

    uint8_t disk_guid[16];

    uint64_t partition_entries_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} __attribute__((packed)) gpt_header_t;


//the LBA2 contains partition entries
typedef struct {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint16_t name[36];  // UTF-16LE
} __attribute__((packed)) gpt_entry_t;



//LBA1 sits at sector one. for now we dont add any abstraction layer for block drivers, and instead call ATA directly.
//we dont have NVMe or xHCI for usb. 

static gpt_header_t    gpt_hdr;
static gpt_partition_t partitions[128];
static int             partition_count = 0;

static void guid_to_utf8(const uint16_t *utf16, char *out, int max_chars) {
    for (int i = 0; i < max_chars - 1 && utf16[i]; i++)
        out[i] = (utf16[i] < 0x80) ? (char)utf16[i] : '?';
    out[max_chars - 1] = '\0';
}

bool gpt_init(void) {
    // read LBA1
    uint8_t sector[512];
    if (!virtio_blk_read(1, 1, sector)) return false;
    memcpy(&gpt_hdr, sector, sizeof(gpt_header_t));
    if (memcmp(gpt_hdr.signature, "EFI PART", 8) != 0) return false;

    // read LBA2-33
    uint8_t buf[512 * 32];
    if (!virtio_blk_read(2, 32, buf)) return false;

    uint32_t count     = gpt_hdr.num_partition_entries;
    uint32_t esize     = gpt_hdr.partition_entry_size;
    partition_count    = 0;

    for (uint32_t i = 0; i < count; i++) {
        gpt_entry_t *e = (gpt_entry_t *)(buf + i * esize);

        // skip empty entries
        bool empty = true;
        for (int b = 0; b < 16; b++)
            if (e->type_guid[b]) { empty = false; break; }
        if (empty) continue;

        gpt_partition_t *p = &partitions[partition_count++];
        memcpy(p->type_guid,   e->type_guid,   16);
        memcpy(p->unique_guid, e->unique_guid, 16);
        p->start_lba  = e->start_lba;
        p->end_lba    = e->end_lba;
        p->num_sectors = e->end_lba - e->start_lba + 1;
        p->attributes = e->attributes;
        guid_to_utf8(e->name, p->name, sizeof(p->name));
    }

    printk("[GPT] found %d partition(s)\n", partition_count);
    return true;
}

int gpt_get_partition_count(void) {
    return partition_count;
}

bool gpt_get_partition(int index, gpt_partition_t *out) {
    if (index < 0 || index >= partition_count) return false;
    memcpy(out, &partitions[index], sizeof(gpt_partition_t));
    return true;
}

bool gpt_read_partition_sectors(int index, uint64_t relative_lba, uint32_t count, void *buf) {
    if (index < 0 || index >= partition_count) return false;
    gpt_partition_t *p = &partitions[index];

    // bounds checking
    if (relative_lba + count > p->num_sectors) return false;

    uint64_t absolute_lba = p->start_lba + relative_lba;
    return virtio_blk_read(absolute_lba, count, buf);
}