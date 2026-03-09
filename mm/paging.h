#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <boot/boot.h>

typedef struct {
    bool initialized;
    uint64_t root_pml4_phys;
    uint64_t table_pages_allocated;
    uint64_t mapped_kernel_pages;
    uint64_t mapped_hhdm_2m_pages;
    uint64_t mapped_hhdm_4k_pages;
    uint64_t mapped_passthrough_pages;
    uint64_t reclaimed_bootloader_pages;
} paging_info_t;

void paging_init(const BootMemoryMap* memory_map);
bool paging_ready(void);
const paging_info_t* paging_get_info(void);
