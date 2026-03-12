#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  type_guid[16];   // tells you what kind of partition (ESP, Linux data, etc.)
    uint8_t  unique_guid[16]; // unique identity, useful for persistent mounting
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t num_sectors;     // end - start + 1, convenient to have precomputed
    uint64_t attributes;
    char     name[72];        // UTF-16 converted to ASCII/UTF-8 for ease of use
} gpt_partition_t;
//simple GPT partition driver.
//reads LBA1 and LBA2 (gpt header and partition entry array.)
//retrievs information 

bool gpt_init(void);                                      // reads LBA1 + LBA2-33, populates table
int  gpt_get_partition_count(void);                       // number of valid (non-empty) partitions
bool gpt_get_partition(int index, gpt_partition_t *out);  // get descriptor by index
bool gpt_read_partition_sectors(int index, uint64_t relative_lba, uint32_t count, void *buf); // read within a partition

