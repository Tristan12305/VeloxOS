#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <boot/boot.h>

#define PAGING_FLAG_WRITABLE      (1ULL << 0)
#define PAGING_FLAG_USER          (1ULL << 1)
#define PAGING_FLAG_EXEC          (1ULL << 2)
#define PAGING_FLAG_GLOBAL        (1ULL << 3)
#define PAGING_FLAG_NO_CACHE      (1ULL << 4)
#define PAGING_FLAG_WRITE_THROUGH (1ULL << 5)

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

void paging_set_nx_enabled(bool enabled);
bool paging_translate(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t* out_phys_addr);
bool paging_map_page(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags);
bool paging_map_range(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t size, uint64_t flags);
bool paging_unmap_page(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t* out_phys_addr);
bool paging_unmap_range(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t size);
bool paging_protect_page(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t flags);
bool paging_protect_range(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t size, uint64_t flags);
void paging_invlpg(uint64_t virt_addr);
