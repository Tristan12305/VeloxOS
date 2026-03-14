#include "pmm.h"

/*1. O(n) linear scan — already deferred, but note the real cost. 
Every kmalloc call that crosses a pool boundary will eventually bottom out here. With 4 GiB of RAM, that's ~1 million bits to scan worst-case. 
A simple fix later is a 64-bit word-level scan (__builtin_ctzll on inverted bitmap words),
 which cuts the constant by 64× without changing the allocator structure.*/

/*2. pmm_alloc_page() returns a virtual address silently. 
The function name suggests it returns a page, but it actually returns the HHDM virtual address of that page. 
Meanwhile pmm_alloc_page_phys() returns the physical address. 
This asymmetry is a trap — the naming convention should be enforced more clearly, especially as you add more callers.*/


#include <kernel/panic.h>

extern uint64_t g_hhdm_offset;

extern char __kernel_physical_start;
extern char __kernel_physical_end;

static uint8_t* g_bitmap;
static pmm_info_t g_pmm_info;
static uint64_t g_next_search;
static bool g_initialized;

static inline uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static inline uint64_t align_down(uint64_t value, uint64_t align) {
    return value & ~(align - 1);
}

static inline bool bit_test(uint64_t bit) {
    return (g_bitmap[bit >> 3] & (1u << (bit & 7u))) != 0;
}

static inline void bit_set(uint64_t bit) {
    g_bitmap[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

static inline void bit_clear(uint64_t bit) {
    g_bitmap[bit >> 3] &= (uint8_t)~(1u << (bit & 7u));
}

static inline uint64_t phys_to_page(uint64_t phys_addr) {
    return phys_addr / PMM_PAGE_SIZE;
}

static inline void mark_page_used(uint64_t page) {
    if (page >= g_pmm_info.total_pages) {
        return;
    }

    if (!bit_test(page)) {
        bit_set(page);
        g_pmm_info.free_pages--;
        g_pmm_info.used_pages++;
    }
}

static inline void mark_page_free(uint64_t page) {
    if (page >= g_pmm_info.total_pages) {
        return;
    }

    if (bit_test(page)) {
        bit_clear(page);
        g_pmm_info.free_pages++;
        g_pmm_info.used_pages--;
    }
}

static void mark_range_free(uint64_t start_phys, uint64_t end_phys) {
    if (end_phys <= start_phys) {
        return;
    }

    /* Free only fully covered pages: [ceil(start), floor(end)). */
    uint64_t page_start = phys_to_page(align_up(start_phys, PMM_PAGE_SIZE));
    uint64_t page_end = phys_to_page(align_down(end_phys, PMM_PAGE_SIZE));

    if (page_end > g_pmm_info.total_pages) {
        page_end = g_pmm_info.total_pages;
    }

    for (uint64_t page = page_start; page < page_end; page++) {
        mark_page_free(page);
    }
}

static void mark_range_used(uint64_t start_phys, uint64_t end_phys) {
    if (end_phys <= start_phys) {
        return;
    }

    /* Reserve any touched page: [floor(start), ceil(end)). */
    uint64_t page_start = phys_to_page(align_down(start_phys, PMM_PAGE_SIZE));
    uint64_t page_end = phys_to_page(align_up(end_phys, PMM_PAGE_SIZE));

    if (page_end > g_pmm_info.total_pages) {
        page_end = g_pmm_info.total_pages;
    }

    for (uint64_t page = page_start; page < page_end; page++) {
        mark_page_used(page);
    }
}

static uint64_t max_physical_end(const BootMemoryMap* memory_map) {
    uint64_t max_end = 0;

    for (size_t i = 0; i < memory_map->count; i++) {
        const MemoryMapEntry* entry = &memory_map->entries[i];
        uint64_t end = entry->base + entry->length;

        if (end < entry->base) {
            end = UINT64_MAX;
        }

        if (end > max_end) {
            max_end = end;
        }
    }

    return max_end;
}

static bool find_bitmap_region(const BootMemoryMap* memory_map,
                               uint64_t bitmap_size,
                               uint64_t* out_phys_base) {
    const uint64_t bitmap_span_bytes = align_up(bitmap_size, PMM_PAGE_SIZE);

    for (size_t i = 0; i < memory_map->count; i++) {
        const MemoryMapEntry* entry = &memory_map->entries[i];
        if (entry->type != MEMMAP_USABLE) {
            continue;
        }

        uint64_t region_start = align_up(entry->base, PMM_PAGE_SIZE);
        uint64_t region_raw_end = entry->base + entry->length;
        if (region_raw_end < entry->base) {
            region_raw_end = UINT64_MAX;
        }
        uint64_t region_end = align_down(region_raw_end, PMM_PAGE_SIZE);

        if (region_end < region_start) {
            continue;
        }

        if ((region_end - region_start) >= bitmap_span_bytes) {
            *out_phys_base = region_start;
            return true;
        }
    }

    return false;
}

void pmm_init(const BootMemoryMap* memory_map) {
    if (!memory_map || !memory_map->entries || memory_map->count == 0) {
        panic("PMM init failed: invalid memory map");
    }

    uint64_t highest_phys = max_physical_end(memory_map);
    uint64_t total_pages = align_up(highest_phys, PMM_PAGE_SIZE) / PMM_PAGE_SIZE;

    if (total_pages == 0) {
        panic("PMM init failed: no physical memory pages detected");
    }

    uint64_t bitmap_bytes = align_up(total_pages, 8) / 8;
    uint64_t bitmap_phys = 0;

    if (!find_bitmap_region(memory_map, bitmap_bytes, &bitmap_phys)) {
        panic("PMM init failed: no space for bitmap");
    }

    g_bitmap = (uint8_t*)(uintptr_t)pmm_phys_to_virt(bitmap_phys);
    g_pmm_info.total_pages = total_pages;
    g_pmm_info.free_pages = 0;
    g_pmm_info.used_pages = total_pages;
    g_pmm_info.bitmap_bytes = bitmap_bytes;
    g_pmm_info.bitmap_phys_base = bitmap_phys;
    g_pmm_info.highest_physical_address = highest_phys;
    g_next_search = 0;

    for (uint64_t i = 0; i < bitmap_bytes; i++) {
        g_bitmap[i] = 0xFF;
    }

    for (size_t i = 0; i < memory_map->count; i++) {
        const MemoryMapEntry* entry = &memory_map->entries[i];
        if (entry->type == MEMMAP_USABLE) {
            uint64_t end = entry->base + entry->length;
            if (end < entry->base) {
                end = UINT64_MAX;
            }
            mark_range_free(entry->base, end);
        }
    }

    /* Keep bootloader-reclaimable regions reserved until a later reclaim step. */
    mark_range_used(bitmap_phys, bitmap_phys + align_up(bitmap_bytes, PMM_PAGE_SIZE));

    uint64_t kernel_phys_start = (uint64_t)(uintptr_t)&__kernel_physical_start;
    uint64_t kernel_phys_end = (uint64_t)(uintptr_t)&__kernel_physical_end;
    mark_range_used(kernel_phys_start, kernel_phys_end);

    mark_page_used(0);

    if ((g_pmm_info.free_pages + g_pmm_info.used_pages) != g_pmm_info.total_pages) {
        panic("PMM init failed: page accounting mismatch");
    }

    g_next_search = (g_pmm_info.total_pages > 1) ? 1 : 0;
    g_initialized = true;
}

bool pmm_ready(void) {
    return g_initialized;
}

uint64_t pmm_alloc_page_phys(void) {
    uint64_t phys_addr = PMM_INVALID_PHYS_ADDR;
    if (!pmm_try_alloc_page_phys(&phys_addr)) {
        return PMM_INVALID_PHYS_ADDR;
    }

    return phys_addr;
}

bool pmm_try_alloc_page_phys(uint64_t* out_phys_addr) {
    if (!out_phys_addr) {
        return false;
    }

    *out_phys_addr = PMM_INVALID_PHYS_ADDR;

    if (!g_initialized || g_pmm_info.free_pages == 0 || g_pmm_info.total_pages == 0) {
        return false;
    }

    uint64_t start = g_next_search;
    if (start >= g_pmm_info.total_pages) {
        start = 0;
    }

    for (uint64_t scanned = 0; scanned < g_pmm_info.total_pages; scanned++) {
        uint64_t page = start + scanned;
        if (page >= g_pmm_info.total_pages) {
            page -= g_pmm_info.total_pages;
        }

        if (!bit_test(page)) {
            mark_page_used(page);
            g_next_search = page + 1;
            if (g_next_search >= g_pmm_info.total_pages) {
                g_next_search = 0;
            }
            *out_phys_addr = page * PMM_PAGE_SIZE;
            return true;
        }
    }

    return false;
}

void* pmm_alloc_page(void) {
    uint64_t phys_addr = PMM_INVALID_PHYS_ADDR;
    if (!pmm_try_alloc_page_phys(&phys_addr)) {
        return (void*)0;
    }

    return (void*)(uintptr_t)pmm_phys_to_virt(phys_addr);
}

void pmm_free_page_phys(uint64_t phys_addr) {
    if (!g_initialized) {
        return;
    }
    if ((phys_addr & (PMM_PAGE_SIZE - 1)) != 0) {
        return;
    }

    uint64_t page = phys_to_page(phys_addr);
    if (page >= g_pmm_info.total_pages) {
        return;
    }

    if (bit_test(page)) {
        mark_page_free(page);
        if (page < g_next_search) {
            g_next_search = page;
        }
    }
}

void pmm_free_page(void* virt_addr) {
    if (!virt_addr) {
        return;
    }

    pmm_free_page_phys(pmm_virt_to_phys((uint64_t)(uintptr_t)virt_addr));
}

uint64_t pmm_phys_to_virt(uint64_t phys_addr) {
    return phys_addr + g_hhdm_offset;
}

uint64_t pmm_virt_to_phys(uint64_t virt_addr) {
    return virt_addr - g_hhdm_offset;
}

const pmm_info_t* pmm_get_info(void) {
    return &g_pmm_info;
}
