#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <boot/boot.h>

#define PMM_PAGE_SIZE 4096ULL
#define PMM_INVALID_PHYS_ADDR UINT64_MAX

typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t bitmap_bytes;
    uint64_t bitmap_phys_base;
    uint64_t highest_physical_address;
} pmm_info_t;

void pmm_init(const BootMemoryMap* memory_map);
bool pmm_ready(void);

bool pmm_try_alloc_page_phys(uint64_t* out_phys_addr);
uint64_t pmm_alloc_page_phys(void);
void* pmm_alloc_page(void);

void pmm_free_page_phys(uint64_t phys_addr);
void pmm_free_page(void* virt_addr);

uint64_t pmm_phys_to_virt(uint64_t phys_addr);
uint64_t pmm_virt_to_phys(uint64_t virt_addr);

const pmm_info_t* pmm_get_info(void);

