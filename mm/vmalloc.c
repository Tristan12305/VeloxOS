#include "vmalloc.h"

#include "pmm.h"
#include "paging.h"

#include <boot/boot.h>
#include <include/printk.h>
#include <kernel/panic.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/*3. vfree requires the caller to pass the original size.
This is the fundamental interface difference from free(). Every caller has to track the size of their vmalloc allocation separately. 
It's not a bug, but it makes vmalloc harder to compose — which is why kmalloc stores its size in a header. 
For vmalloc, you could store a size_t in a header page (or use a separate lookup table keyed on the start address) 
to eventually make vfree(ptr) work without a size argument.
4. No guard pages between vmalloc regions.
Adjacent vmalloc allocations sit directly next to each other in virtual memory. 
A buffer overrun in one vmalloc region will silently walk into the next allocation's data with no fault. 
Inserting one unmapped guard page between allocations would turn those overruns into immediate page faults. Cheap to add, high diagnostic value.*/

#define VMALLOC_DEFAULT_SIZE (256ULL * 1024ULL * 1024ULL)
#define VMALLOC_MAX_RANGES 128

extern uint64_t g_hhdm_offset;
extern char __kernel_virtual_end;

typedef struct {
    uint64_t start;
    uint64_t size;
} vmalloc_range_t;

static vmalloc_range_t g_vmalloc_free[VMALLOC_MAX_RANGES];
static size_t g_vmalloc_free_count;
static uint64_t g_vmalloc_base;
static uint64_t g_vmalloc_end;
static bool g_vmalloc_initialized;

static inline uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static uint64_t range_end(uint64_t start, uint64_t size) {
    uint64_t end = start + size;
    if (end < start) {
        return UINT64_MAX;
    }
    return end;
}

static void vmalloc_free_range(uint64_t start, uint64_t size) {
    if (size == 0) {
        return;
    }

    if (g_vmalloc_free_count >= VMALLOC_MAX_RANGES) {
        panic("vmalloc: free list overflow");
    }

    size_t insert_at = 0;
    while (insert_at < g_vmalloc_free_count && g_vmalloc_free[insert_at].start < start) {
        insert_at++;
    }

    for (size_t i = g_vmalloc_free_count; i > insert_at; i--) {
        g_vmalloc_free[i] = g_vmalloc_free[i - 1];
    }

    g_vmalloc_free[insert_at].start = start;
    g_vmalloc_free[insert_at].size = size;
    g_vmalloc_free_count++;

    for (size_t i = 0; i + 1 < g_vmalloc_free_count; ) {
        uint64_t a_start = g_vmalloc_free[i].start;
        uint64_t a_end = range_end(a_start, g_vmalloc_free[i].size);
        uint64_t b_start = g_vmalloc_free[i + 1].start;
        uint64_t b_end = range_end(b_start, g_vmalloc_free[i + 1].size);

        if (a_end >= b_start) {
            uint64_t new_end = (a_end > b_end) ? a_end : b_end;
            g_vmalloc_free[i].size = new_end - a_start;
            for (size_t j = i + 1; j + 1 < g_vmalloc_free_count; j++) {
                g_vmalloc_free[j] = g_vmalloc_free[j + 1];
            }
            g_vmalloc_free_count--;
            continue;
        }

        i++;
    }
}

static bool vmalloc_init_once(void) {
    if (g_vmalloc_initialized) {
        return true;
    }

    if (!pmm_ready() || !paging_ready()) {
        return false;
    }

    const pmm_info_t* pmm = pmm_get_info();
    if (!pmm || pmm->highest_physical_address == 0 || pmm->highest_physical_address == UINT64_MAX) {
        return false;
    }

    uint64_t kernel_end = align_up((uint64_t)(uintptr_t)&__kernel_virtual_end, PMM_PAGE_SIZE);
    uint64_t hhdm_span = align_up(pmm->highest_physical_address, PMM_PAGE_SIZE);
    uint64_t hhdm_end = g_hhdm_offset + hhdm_span;
    if (hhdm_end < g_hhdm_offset) {
        hhdm_end = UINT64_MAX;
    }

    uint64_t base = kernel_end;
    if (base >= g_hhdm_offset && base < hhdm_end) {
        base = hhdm_end;
    }

    base = align_up(base, PMM_PAGE_SIZE);

    uint64_t size = VMALLOC_DEFAULT_SIZE;
    uint64_t end = range_end(base, size);

    if (base < g_hhdm_offset && end > g_hhdm_offset) {
        end = g_hhdm_offset;
        if (end <= base) {
            return false;
        }
        size = end - base;
    }

    if (size < PMM_PAGE_SIZE) {
        return false;
    }

    g_vmalloc_base = base;
    g_vmalloc_end = base + size;
    g_vmalloc_free[0].start = base;
    g_vmalloc_free[0].size = size;
    g_vmalloc_free_count = 1;
    g_vmalloc_initialized = true;

    printk("[vmalloc] region [0x%llx, 0x%llx) size=%llu MiB\n",
           (unsigned long long)g_vmalloc_base,
           (unsigned long long)g_vmalloc_end,
           (unsigned long long)(size / (1024ULL * 1024ULL)));

    return true;
}

void vmalloc_init(void) {
    if (!vmalloc_init_once()) {
        printk("[vmalloc] init failed\n");
    }
}

void* vmalloc(size_t size, uint64_t flags) {
    if (size == 0) {
        return (void*)0;
    }

    if (!vmalloc_init_once()) {
        return (void*)0;
    }

    uint64_t aligned = align_up((uint64_t)size, PMM_PAGE_SIZE);
    if (aligned < (uint64_t)size) {
        return (void*)0;
    }

    for (size_t i = 0; i < g_vmalloc_free_count; i++) {
        if (g_vmalloc_free[i].size < aligned) {
            continue;
        }

        uint64_t addr = g_vmalloc_free[i].start;
        g_vmalloc_free[i].start += aligned;
        g_vmalloc_free[i].size -= aligned;
        if (g_vmalloc_free[i].size == 0) {
            for (size_t j = i; j + 1 < g_vmalloc_free_count; j++) {
                g_vmalloc_free[j] = g_vmalloc_free[j + 1];
            }
            g_vmalloc_free_count--;
        }

        const paging_info_t* paging = paging_get_info();
        if (!paging || paging->root_pml4_phys == 0) {
            vmalloc_free_range(addr, aligned);
            return (void*)0;
        }

        uint64_t root = paging->root_pml4_phys;
        uint64_t mapped = 0;

        for (uint64_t offset = 0; offset < aligned; offset += PMM_PAGE_SIZE) {
            uint64_t phys = PMM_INVALID_PHYS_ADDR;
            if (!pmm_try_alloc_page_phys(&phys)) {
                break;
            }

            if (!paging_map_page(root, addr + offset, phys, flags)) {
                pmm_free_page_phys(phys);
                break;
            }

            mapped += PMM_PAGE_SIZE;
        }

        if (mapped == aligned) {
            return (void*)(uintptr_t)addr;
        }

        for (uint64_t offset = 0; offset < mapped; offset += PMM_PAGE_SIZE) {
            uint64_t phys = 0;
            if (paging_unmap_page(root, addr + offset, &phys)) {
                pmm_free_page_phys(phys);
            }
        }

        vmalloc_free_range(addr, aligned);
        return (void*)0;
    }

    return (void*)0;
}

void vfree(void* addr, size_t size) {
    if (!addr || size == 0) {
        return;
    }

    if (!g_vmalloc_initialized) {
        return;
    }

    uint64_t start = (uint64_t)(uintptr_t)addr;
    if ((start & (PMM_PAGE_SIZE - 1)) != 0) {
        printk("[vmalloc] vfree: unaligned address 0x%llx\n", (unsigned long long)start);
        return;
    }

    uint64_t aligned = align_up((uint64_t)size, PMM_PAGE_SIZE);
    if (aligned < (uint64_t)size) {
        return;
    }

    if (start < g_vmalloc_base || start >= g_vmalloc_end || start > (g_vmalloc_end - aligned)) {
        printk("[vmalloc] vfree: address out of range 0x%llx\n", (unsigned long long)start);
        return;
    }

    const paging_info_t* paging = paging_get_info();
    if (!paging || paging->root_pml4_phys == 0) {
        return;
    }

    uint64_t root = paging->root_pml4_phys;
    for (uint64_t offset = 0; offset < aligned; offset += PMM_PAGE_SIZE) {
        uint64_t phys = 0;
        if (paging_unmap_page(root, start + offset, &phys)) {
            pmm_free_page_phys(phys);
        }
    }

    vmalloc_free_range(start, aligned);
}
