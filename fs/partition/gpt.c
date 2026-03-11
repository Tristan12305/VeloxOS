#include <arch/x86_64/drivers/block/virtio/vblk.h>
#include "gpt.h"
#include <include/printk.h>

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
typedef struct{
    uint16_t partGUID; //partition type GUID, if 0 then its unused
    uint16_t uniqueGUID; 
    uint8_t lbaStart;
    uint8_t lbaEnd;
    uint8_t p_attributes; //set by firmware, if bit is 0, it shouldnt be touched. 2 means the partition is needed to boot an os.
    char partname[72];
} __attribute__((packed)) gpt_entries_t;


//LBA1 sits at sector one. for now we dont add any abstraction layer for block drivers, and instead call virtIO directly.
//we dont have NVMe or xHCI for usb. 


void read_lba(){
    uint64_t sector[256];

    if(!virtio_blk_read(1, 1, sector)){
        printk("[GPT-Part] Could not read LBA1\n"); 
    }
    
    gpt_header_t *hdr = (gpt_header_t *)sector;

    if(__builtin_memcmp(hdr->signature, "EFI PART", 8) != 0){
        printk("[GPT-Part] Either not a GPT partition. or something went wrong when reading sector 1\n");
    }

    printk("[gpt] revision         : 0x%08x\n", hdr->revision);
    printk("[gpt] header_size      : %u bytes\n", hdr->header_size);
    printk("[gpt] current_lba      : %llu\n", hdr->current_lba);
    printk("[gpt] backup_lba       : %llu\n", hdr->backup_lba);
    printk("[gpt] first_usable_lba : %llu\n", hdr->first_usable_lba);
    printk("[gpt] last_usable_lba  : %llu\n", hdr->last_usable_lba);
    printk("[gpt] partition_entries_lba  : %llu\n", hdr->partition_entries_lba);
    printk("[gpt] num_partition_entries  : %u\n",   hdr->num_partition_entries);
    printk("[gpt] partition_entry_size   : %u\n",   hdr->partition_entry_size);
    printk("[gpt] partition_array_crc32  : 0x%08x\n", hdr->partition_array_crc32);

}
/*
void parse_gpt_entries(){

}

*/