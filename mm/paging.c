#include "paging.h"

#include "pmm.h"

#include <include/printk.h>
#include <include/spinlock.h>
#include <kernel/panic.h>
#include <kernel/ipi.h>

#include <stddef.h>
#include <stdint.h>

extern uint64_t g_hhdm_offset;
extern BootFramebuffer g_framebuffer;

extern char __kernel_virtual_start;
extern char __kernel_virtual_end;
extern char __kernel_physical_start;

extern uint64_t paging_read_cr3(void);
extern void paging_load_cr3(uint64_t phys_addr);

static inline uint64_t paging_read_cr4(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void paging_write_cr4(uint64_t value) {
    __asm__ volatile("mov %0, %%cr4" :: "r"(value) : "memory");
}

#define PAGE_SIZE_4K  0x1000ULL
#define PAGE_SIZE_2M  0x200000ULL
#define PAGE_SIZE_1G  0x40000000ULL

#define PAGE_PRESENT  (1ULL << 0)
#define PAGE_WRITABLE (1ULL << 1)
#define PAGE_USER     (1ULL << 2)
#define PAGE_PWT      (1ULL << 3)
#define PAGE_PCD      (1ULL << 4)
#define PAGE_PS       (1ULL << 7)
#define PAGE_GLOBAL   (1ULL << 8)
#define PAGE_NX       (1ULL << 63)

#define ENTRY_ADDR_MASK_4K 0x000FFFFFFFFFF000ULL
#define ENTRY_ADDR_MASK_2M 0x000FFFFFFFE00000ULL
#define ENTRY_ADDR_MASK_1G 0x000FFFFFC0000000ULL

#define STACK_PROTECT_WINDOW_BYTES (64ULL * 1024ULL)

static paging_info_t g_paging_info;
static bool g_paging_initialized;
static bool g_nx_enabled;
static spinlock_t g_paging_lock = SPINLOCK_INIT;

static void map_page_4k(uint64_t root_pml4_phys,
                        uint64_t virt_addr,
                        uint64_t phys_addr,
                        uint64_t flags);
static bool translate_with_cr3(uint64_t root_cr3_phys, uint64_t virt_addr, uint64_t* out_phys_addr);

static uint64_t paging_flags_to_hw(uint64_t flags);
static bool walk_to_pte(uint64_t root_pml4_phys,
                        uint64_t virt_addr,
                        bool create,
                        bool user,
                        uint64_t** out_pte);
static bool paging_map_page_locked(uint64_t root_pml4_phys,
                                   uint64_t virt_addr,
                                   uint64_t phys_addr,
                                   uint64_t flags);
static bool paging_unmap_page_locked(uint64_t root_pml4_phys,
                                     uint64_t virt_addr,
                                     uint64_t* out_phys_addr);
static bool paging_protect_page_locked(uint64_t root_pml4_phys,
                                       uint64_t virt_addr,
                                       uint64_t flags);

static inline uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static inline uint64_t align_down(uint64_t value, uint64_t align) {
    return value & ~(align - 1);
}

static inline uint16_t pml4_index(uint64_t virt_addr) {
    return (uint16_t)((virt_addr >> 39) & 0x1FF);
}

static inline uint16_t pdpt_index(uint64_t virt_addr) {
    return (uint16_t)((virt_addr >> 30) & 0x1FF);
}

static inline uint16_t pd_index(uint64_t virt_addr) {
    return (uint16_t)((virt_addr >> 21) & 0x1FF);
}

static inline uint16_t pt_index(uint64_t virt_addr) {
    return (uint16_t)((virt_addr >> 12) & 0x1FF);
}

static inline uint64_t read_rsp(void) {
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static inline uint64_t* table_virt_from_phys(uint64_t phys_addr) {
    return (uint64_t*)(uintptr_t)pmm_phys_to_virt(phys_addr);
}

static uint64_t alloc_table_page(void) {
    uint64_t table_phys = PMM_INVALID_PHYS_ADDR;
    if (!pmm_try_alloc_page_phys(&table_phys) || table_phys == PMM_INVALID_PHYS_ADDR) {
        panic("Paging init failed: out of memory for page tables");
    }

    uint64_t* table = table_virt_from_phys(table_phys);
    for (size_t i = 0; i < 512; i++) {
        table[i] = 0;
    }

    g_paging_info.table_pages_allocated++;
    return table_phys;
}

static uint64_t get_or_create_table(uint64_t* parent_table, uint16_t index, bool user) {
    uint64_t entry = parent_table[index];

    if ((entry & PAGE_PRESENT) != 0) {
        if ((entry & PAGE_PS) != 0) {
            panic("Paging: huge-page entry in intermediate table walk");
        }
        /* Retrofit PAGE_USER if this table is being used for a user mapping
         * but was originally created as a kernel-only entry.  x86-64 requires
         * PAGE_USER on every intermediate level for CPL-3 access to reach the
         * PTE — even if the PTE itself has PAGE_USER set.                    */
        if (user && !(entry & PAGE_USER)) {
            parent_table[index] = entry | PAGE_USER;
        }
        return entry & ENTRY_ADDR_MASK_4K;
    }

    uint64_t child_phys = alloc_table_page();
    uint64_t new_entry = child_phys | PAGE_PRESENT | PAGE_WRITABLE;
    if (user) {
        new_entry |= PAGE_USER;
    }
    parent_table[index] = new_entry;
    return child_phys;
}

static uint64_t paging_flags_to_hw(uint64_t flags) {
    uint64_t hw = 0;
    if ((flags & PAGING_FLAG_WRITABLE) != 0) {
        hw |= PAGE_WRITABLE;
    }
    if ((flags & PAGING_FLAG_USER) != 0) {
        hw |= PAGE_USER;
    }
    if ((flags & PAGING_FLAG_GLOBAL) != 0) {
        hw |= PAGE_GLOBAL;
    }
    if ((flags & PAGING_FLAG_WRITE_THROUGH) != 0) {
        hw |= PAGE_PWT;
    }
    if ((flags & PAGING_FLAG_NO_CACHE) != 0) {
        hw |= PAGE_PCD;
    }
    if ((flags & PAGING_FLAG_EXEC) == 0 && g_nx_enabled) {
        hw |= PAGE_NX;
    }
    return hw;
}

static bool walk_to_pte(uint64_t root_pml4_phys,
                        uint64_t virt_addr,
                        bool create,
                        bool user,
                        uint64_t** out_pte) {
    if (!out_pte) {
        return false;
    }

    uint64_t* pml4 = table_virt_from_phys(root_pml4_phys & ENTRY_ADDR_MASK_4K);
    uint16_t l4 = pml4_index(virt_addr);

    if (!(pml4[l4] & PAGE_PRESENT) && !create) {
        return false;
    }
    uint64_t pdpt_phys = get_or_create_table(pml4, l4, user);

    uint64_t* pdpt = table_virt_from_phys(pdpt_phys);
    uint16_t l3 = pdpt_index(virt_addr);
    if (pdpt[l3] & PAGE_PS) {
        return false; /* 1 GiB page — can't walk to a 4 KiB PTE inside it */
    }
    if (!(pdpt[l3] & PAGE_PRESENT) && !create) {
        return false;
    }
    uint64_t pd_phys = get_or_create_table(pdpt, l3, user);

    uint64_t* pd = table_virt_from_phys(pd_phys);
    uint16_t l2 = pd_index(virt_addr);
    if (pd[l2] & PAGE_PS) {
        return false; /* 2 MiB page — can't walk to a 4 KiB PTE inside it */
    }
    if (!(pd[l2] & PAGE_PRESENT) && !create) {
        return false;
    }
    uint64_t pt_phys = get_or_create_table(pd, l2, user);

    uint64_t* pt = table_virt_from_phys(pt_phys);
    *out_pte = &pt[pt_index(virt_addr)];
    return true;
}

static bool ensure_page_mapping(uint64_t root_pml4_phys,
                                uint64_t virt_addr,
                                uint64_t phys_addr,
                                uint64_t flags) {
    uint64_t mapped_phys = 0;
    if (translate_with_cr3(root_pml4_phys, virt_addr, &mapped_phys)) {
        if (align_down(mapped_phys, PAGE_SIZE_4K) != align_down(phys_addr, PAGE_SIZE_4K)) {
            panic("Paging init failed: virtual address already mapped to different physical page");
        }
        return false;
    }

    map_page_4k(root_pml4_phys, virt_addr, phys_addr, flags);
    return true;
}

static void map_page_2m(uint64_t root_pml4_phys,
                        uint64_t virt_addr,
                        uint64_t phys_addr,
                        uint64_t flags) {
    if (((virt_addr | phys_addr) & (PAGE_SIZE_2M - 1)) != 0) {
        panic("Paging init failed: 2 MiB mapping not aligned");
    }

    uint64_t* pml4 = table_virt_from_phys(root_pml4_phys);
    uint64_t pdpt_phys = get_or_create_table(pml4, pml4_index(virt_addr), false);
    uint64_t* pdpt = table_virt_from_phys(pdpt_phys);
    uint64_t pd_phys = get_or_create_table(pdpt, pdpt_index(virt_addr), false);

    uint64_t* pd = table_virt_from_phys(pd_phys);

    pd[pd_index(virt_addr)] =
        (phys_addr & ENTRY_ADDR_MASK_2M) | flags | PAGE_PRESENT | PAGE_PS;
}

static void map_page_4k(uint64_t root_pml4_phys,
                        uint64_t virt_addr,
                        uint64_t phys_addr,
                        uint64_t flags) {
    if (((virt_addr | phys_addr) & (PAGE_SIZE_4K - 1)) != 0) {
        panic("Paging init failed: 4 KiB mapping not aligned");
    }

    uint64_t* pml4 = table_virt_from_phys(root_pml4_phys);
    uint64_t pdpt_phys = get_or_create_table(pml4, pml4_index(virt_addr), false);
    uint64_t* pdpt = table_virt_from_phys(pdpt_phys);
    uint64_t pd_phys = get_or_create_table(pdpt, pdpt_index(virt_addr), false);
    uint64_t* pd = table_virt_from_phys(pd_phys);

    uint64_t pd_entry = pd[pd_index(virt_addr)];
    if ((pd_entry & PAGE_PRESENT) != 0 && (pd_entry & PAGE_PS) != 0) {
        panic("Paging init failed: 4 KiB mapping conflicts with 2 MiB mapping");
    }

    uint64_t pt_phys = get_or_create_table(pd, pd_index(virt_addr), false);
    uint64_t* pt = table_virt_from_phys(pt_phys);

    pt[pt_index(virt_addr)] = (phys_addr & ENTRY_ADDR_MASK_4K) | flags | PAGE_PRESENT;
}

static bool translate_with_cr3(uint64_t root_cr3_phys, uint64_t virt_addr, uint64_t* out_phys_addr) {
    if (!out_phys_addr) {
        return false;
    }

    uint64_t* pml4 = table_virt_from_phys(root_cr3_phys & ENTRY_ADDR_MASK_4K);
    uint64_t pml4e = pml4[pml4_index(virt_addr)];
    if ((pml4e & PAGE_PRESENT) == 0) {
        return false;
    }

    uint64_t* pdpt = table_virt_from_phys(pml4e & ENTRY_ADDR_MASK_4K);
    uint64_t pdpte = pdpt[pdpt_index(virt_addr)];
    if ((pdpte & PAGE_PRESENT) == 0) {
        return false;
    }

    if ((pdpte & PAGE_PS) != 0) {
        *out_phys_addr = (pdpte & ENTRY_ADDR_MASK_1G) | (virt_addr & (PAGE_SIZE_1G - 1));
        return true;
    }

    uint64_t* pd = table_virt_from_phys(pdpte & ENTRY_ADDR_MASK_4K);
    uint64_t pde = pd[pd_index(virt_addr)];
    if ((pde & PAGE_PRESENT) == 0) {
        return false;
    }

    if ((pde & PAGE_PS) != 0) {
        *out_phys_addr = (pde & ENTRY_ADDR_MASK_2M) | (virt_addr & (PAGE_SIZE_2M - 1));
        return true;
    }

    uint64_t* pt = table_virt_from_phys(pde & ENTRY_ADDR_MASK_4K);
    uint64_t pte = pt[pt_index(virt_addr)];
    if ((pte & PAGE_PRESENT) == 0) {
        return false;
    }

    *out_phys_addr = (pte & ENTRY_ADDR_MASK_4K) | (virt_addr & (PAGE_SIZE_4K - 1));
    return true;
}

static uint64_t map_kernel_range_from_old_cr3(uint64_t new_pml4_phys,
                                              uint64_t old_cr3_phys) {
    uint64_t kernel_virt_start = (uint64_t)(uintptr_t)&__kernel_virtual_start;
    uint64_t kernel_virt_end = (uint64_t)(uintptr_t)&__kernel_virtual_end;

    if (kernel_virt_end <= kernel_virt_start) {
        panic("Paging init failed: invalid kernel mapping bounds");
    }

    uint64_t virt_size = kernel_virt_end - kernel_virt_start;
    uint64_t page_count = align_up(virt_size, PAGE_SIZE_4K) / PAGE_SIZE_4K;
    for (uint64_t page = 0; page < page_count; page++) {
        uint64_t virt = kernel_virt_start + (page * PAGE_SIZE_4K);
        uint64_t phys = 0;

        if (!translate_with_cr3(old_cr3_phys, virt, &phys)) {
            panic("Paging init failed: old CR3 missing kernel mapping");
        }

        map_page_4k(new_pml4_phys, virt, align_down(phys, PAGE_SIZE_4K), PAGE_WRITABLE | PAGE_GLOBAL);
    }

    return page_count;
}

static void map_hhdm_range(uint64_t new_pml4_phys, uint64_t hhdm_span_bytes) {
    bool use_2m = (g_hhdm_offset & (PAGE_SIZE_2M - 1)) == 0;
    uint64_t two_mib_limit = use_2m ? align_down(hhdm_span_bytes, PAGE_SIZE_2M) : 0;
    uint64_t phys = 0;

    while (phys < two_mib_limit) {
        uint64_t virt = g_hhdm_offset + phys;
        if (virt < g_hhdm_offset) {
            panic("Paging init failed: HHDM virtual overflow");
        }
        map_page_2m(new_pml4_phys, virt, phys, PAGE_WRITABLE | PAGE_GLOBAL);
        g_paging_info.mapped_hhdm_2m_pages++;
        phys += PAGE_SIZE_2M;
    }

    while (phys < hhdm_span_bytes) {
        uint64_t virt = g_hhdm_offset + phys;
        if (virt < g_hhdm_offset) {
            panic("Paging init failed: HHDM virtual overflow");
        }
        map_page_4k(new_pml4_phys, virt, phys, PAGE_WRITABLE | PAGE_GLOBAL);
        g_paging_info.mapped_hhdm_4k_pages++;
        phys += PAGE_SIZE_4K;
    }
}

static uint64_t map_range_from_old_cr3(uint64_t new_pml4_phys,
                                       uint64_t old_cr3_phys,
                                       uint64_t virt_start,
                                       uint64_t virt_end) {
    uint64_t page_start = align_down(virt_start, PAGE_SIZE_4K);
    uint64_t page_end = align_up(virt_end, PAGE_SIZE_4K);
    uint64_t mapped = 0;

    for (uint64_t virt = page_start; virt < page_end; virt += PAGE_SIZE_4K) {
        uint64_t phys = 0;
        if (!translate_with_cr3(old_cr3_phys, virt, &phys)) {
            panic("Paging init failed: old CR3 translation missing for required range");
        }

        if (ensure_page_mapping(new_pml4_phys,
                                virt,
                                align_down(phys, PAGE_SIZE_4K),
                                PAGE_WRITABLE | PAGE_GLOBAL)) {
            mapped++;
        }
    }

    return mapped;
}

static uint64_t reclaim_bootloader_reclaimable(const BootMemoryMap* memory_map,
                                               uint64_t skip_phys_start,
                                               uint64_t skip_phys_end) {
    if (!memory_map || !memory_map->entries || memory_map->count == 0) {
        return 0;
    }

    uint64_t reclaimed_pages = 0;

    for (size_t i = 0; i < memory_map->count; i++) {
        const MemoryMapEntry* entry = &memory_map->entries[i];
        if (entry->type != MEMMAP_BOOTLOADER_RECLAIMABLE) {
            continue;
        }

        uint64_t raw_end = entry->base + entry->length;
        if (raw_end < entry->base) {
            raw_end = UINT64_MAX;
        }

        uint64_t start = align_up(entry->base, PAGE_SIZE_4K);
        uint64_t end = align_down(raw_end, PAGE_SIZE_4K);

        for (uint64_t phys = start; phys < end; phys += PAGE_SIZE_4K) {
            if (phys >= skip_phys_start && phys < skip_phys_end) {
                continue;
            }
            pmm_free_page_phys(phys);
            reclaimed_pages++;
        }
    }

    return reclaimed_pages;
}

void paging_init(const BootMemoryMap* memory_map) {
    if (!memory_map || !memory_map->entries || memory_map->count == 0) {
        panic("Paging init failed: invalid memory map");
    }

    spin_lock(&g_paging_lock);
    if (g_paging_initialized) {
        spin_unlock(&g_paging_lock);
        return;
    }

    if (!pmm_ready()) {
        panic("Paging init failed: PMM is not ready");
    }

    const pmm_info_t* pmm = pmm_get_info();
    if (!pmm || pmm->highest_physical_address == 0 || pmm->highest_physical_address == UINT64_MAX) {
        panic("Paging init failed: invalid PMM info");
    }

    uint64_t kernel_virt_start = (uint64_t)(uintptr_t)&__kernel_virtual_start;
    uint64_t kernel_phys_linker = (uint64_t)(uintptr_t)&__kernel_physical_start;

    uint64_t hhdm_span_bytes = align_up(pmm->highest_physical_address, PAGE_SIZE_4K);
    uint64_t old_cr3_phys = paging_read_cr3() & ENTRY_ADDR_MASK_4K;
    uint64_t kernel_phys_old = 0;
    bool kernel_phys_ok = translate_with_cr3(old_cr3_phys, kernel_virt_start, &kernel_phys_old);
    if (!kernel_phys_ok) {
        panic("Paging init failed: unable to translate kernel start from old CR3");
    }

    printk("[paging] kernel map check: old_cr3(vstart)=0x%llx linker=0x%llx %s\n",
           (unsigned long long)align_down(kernel_phys_old, PAGE_SIZE_4K),
           (unsigned long long)align_down(kernel_phys_linker, PAGE_SIZE_4K),
           (align_down(kernel_phys_old, PAGE_SIZE_4K) == align_down(kernel_phys_linker, PAGE_SIZE_4K))
                ? "match"
                : "MISMATCH");

    printk("[paging] step 1/7: allocating new root page table\n");
    uint64_t new_pml4_phys = alloc_table_page();
    g_paging_info.root_pml4_phys = new_pml4_phys;
    printk("[paging] new PML4 at phys 0x%llx (current CR3=0x%llx)\n",
           (unsigned long long)new_pml4_phys,
           (unsigned long long)old_cr3_phys);

    printk("[paging] step 2/7: mapping kernel higher-half pages\n");
    g_paging_info.mapped_kernel_pages = map_kernel_range_from_old_cr3(new_pml4_phys, old_cr3_phys);
    printk("[paging] kernel pages mapped: %llu\n",
           (unsigned long long)g_paging_info.mapped_kernel_pages);

    printk("[paging] step 3/7: mapping HHDM span\n");
    if ((g_hhdm_offset & (PAGE_SIZE_2M - 1)) != 0) {
        printk("[paging] HHDM offset not 2MiB aligned; using 4KiB pages for HHDM\n");
    }
    map_hhdm_range(new_pml4_phys, hhdm_span_bytes);
    printk("[paging] HHDM mapped: 2MiB=%llu 4KiB=%llu\n",
           (unsigned long long)g_paging_info.mapped_hhdm_2m_pages,
           (unsigned long long)g_paging_info.mapped_hhdm_4k_pages);

    printk("[paging] step 4/7: preserving current stack + framebuffer mappings\n");
    uint64_t current_rsp = read_rsp();
    uint64_t stack_window_start = (current_rsp > STACK_PROTECT_WINDOW_BYTES)
        ? (current_rsp - STACK_PROTECT_WINDOW_BYTES)
        : 0;
    uint64_t stack_window_end = current_rsp + STACK_PROTECT_WINDOW_BYTES;
    if (stack_window_end < current_rsp) {
        stack_window_end = UINT64_MAX;
    }

    g_paging_info.mapped_passthrough_pages += map_range_from_old_cr3(
        new_pml4_phys,
        old_cr3_phys,
        stack_window_start,
        stack_window_end
    );

    if (g_framebuffer.address && g_framebuffer.pitch != 0 && g_framebuffer.height != 0) {
        if (g_framebuffer.pitch > (UINT64_MAX / g_framebuffer.height)) {
            panic("Paging init failed: framebuffer size overflow");
        }
        uint64_t fb_base = (uint64_t)(uintptr_t)g_framebuffer.address;
        uint64_t fb_size = g_framebuffer.pitch * g_framebuffer.height;
        uint64_t fb_end = fb_base + fb_size;
        if (fb_end < fb_base) {
            fb_end = UINT64_MAX;
        }

        g_paging_info.mapped_passthrough_pages += map_range_from_old_cr3(
            new_pml4_phys,
            old_cr3_phys,
            fb_base,
            fb_end
        );
    }

    printk("[paging] passthrough pages mapped: %llu\n",
           (unsigned long long)g_paging_info.mapped_passthrough_pages);

    uint64_t stack_phys = 0;
    bool have_stack_phys = translate_with_cr3(old_cr3_phys, current_rsp, &stack_phys);
    uint64_t reclaim_skip_start = 0;
    uint64_t reclaim_skip_end = 0;

    if (have_stack_phys) {
        reclaim_skip_start = (stack_phys > STACK_PROTECT_WINDOW_BYTES)
            ? align_down(stack_phys - STACK_PROTECT_WINDOW_BYTES, PAGE_SIZE_4K)
            : 0;
        reclaim_skip_end = align_up(stack_phys + STACK_PROTECT_WINDOW_BYTES, PAGE_SIZE_4K);
        if (reclaim_skip_end < reclaim_skip_start) {
            reclaim_skip_end = UINT64_MAX;
        }

        printk("[paging] stack-protect physical window: [0x%llx, 0x%llx)\n",
               (unsigned long long)reclaim_skip_start,
               (unsigned long long)reclaim_skip_end);
    } else {
        printk("[paging] warning: could not resolve current stack physical page\n");
    }

    printk("[paging] step 5/7: switching CR3 to kernel-owned page tables\n");
    paging_load_cr3(new_pml4_phys);
    printk("[paging] CR3 switch complete\n");

    printk("[paging] step 6/7: reclaiming bootloader-reclaimable memory\n");
    g_paging_info.reclaimed_bootloader_pages = reclaim_bootloader_reclaimable(
        memory_map,
        reclaim_skip_start,
        reclaim_skip_end
    );
    printk("[paging] bootloader pages reclaimed: %llu\n",
           (unsigned long long)g_paging_info.reclaimed_bootloader_pages);

    g_paging_info.initialized = true;
    g_paging_initialized = true;

    printk("[paging] step 7/7: done (tables=%llu)\n",
           (unsigned long long)g_paging_info.table_pages_allocated);

    spin_unlock(&g_paging_lock);
}

bool paging_ready(void) {
    return g_paging_initialized;
}

const paging_info_t* paging_get_info(void) {
    return &g_paging_info;
}

void paging_set_nx_enabled(bool enabled) {
    uint64_t flags = spin_lock_irqsave(&g_paging_lock);
    g_nx_enabled = enabled;
    spin_unlock_irqrestore(&g_paging_lock, flags);
}

bool paging_translate(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t* out_phys_addr) {
    uint64_t flags = spin_lock_irqsave(&g_paging_lock);
    bool ok = translate_with_cr3(root_pml4_phys, virt_addr, out_phys_addr);
    spin_unlock_irqrestore(&g_paging_lock, flags);
    return ok;
}

void paging_invlpg(uint64_t virt_addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(virt_addr) : "memory");
    if (ipi_ready()) {
        ipi_tlb_shootdown();
    }
}

void paging_tlb_flush_all(void) {
    uint64_t cr3 = paging_read_cr3();
    paging_load_cr3(cr3);
    uint64_t cr4 = paging_read_cr4();
    const uint64_t cr4_pge = 1ULL << 7;
    if (cr4 & cr4_pge) {
        paging_write_cr4(cr4 & ~cr4_pge);
        paging_write_cr4(cr4);
    }
}

static bool paging_map_page_locked(uint64_t root_pml4_phys,
                                   uint64_t virt_addr,
                                   uint64_t phys_addr,
                                   uint64_t flags) {
    if (((virt_addr | phys_addr) & (PAGE_SIZE_4K - 1)) != 0) {
        return false;
    }

    /* GLOBAL and USER are mutually exclusive: GLOBAL suppresses TLB flushes
     * on CR3 switches, so a GLOBAL user page would persist into other
     * address spaces and could be accessed by the wrong process.           */
    if ((flags & PAGING_FLAG_GLOBAL) && (flags & PAGING_FLAG_USER)) {
        printk("[paging] paging_map_page: GLOBAL+USER flags are mutually exclusive "
               "(virt=0x%llx)\n", (unsigned long long)virt_addr);
        return false;
    }

    /* Reject any remapping attempt — the caller must unmap first, or use
     * paging_protect_page() to change flags on an existing mapping.
     * Silently changing flags on a live mapping is a security/correctness
     * hazard that is much harder to debug than an explicit failure here.   */
    uint64_t existing_phys = 0;
    if (translate_with_cr3(root_pml4_phys, virt_addr, &existing_phys)) {
        printk("[paging] paging_map_page: virt=0x%llx already mapped to phys=0x%llx\n",
               (unsigned long long)virt_addr,
               (unsigned long long)(existing_phys & ENTRY_ADDR_MASK_4K));
        return false;
    }

    uint64_t* pte = NULL;
    bool user = (flags & PAGING_FLAG_USER) != 0;
    if (!walk_to_pte(root_pml4_phys, virt_addr, true, user, &pte)) {
        return false;
    }

    uint64_t hw_flags = paging_flags_to_hw(flags) | PAGE_PRESENT;
    *pte = (phys_addr & ENTRY_ADDR_MASK_4K) | hw_flags;
    paging_invlpg(virt_addr);
    return true;
}

bool paging_map_page(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t phys_addr, uint64_t flags) {
    uint64_t irq_flags = spin_lock_irqsave(&g_paging_lock);
    bool ok = paging_map_page_locked(root_pml4_phys, virt_addr, phys_addr, flags);
    spin_unlock_irqrestore(&g_paging_lock, irq_flags);
    return ok;
}

bool paging_map_range(uint64_t root_pml4_phys,
                      uint64_t virt_addr,
                      uint64_t phys_addr,
                      uint64_t size,
                      uint64_t flags) {
    if (size == 0) {
        return false;
    }

    if ((virt_addr > UINT64_MAX - size) || (phys_addr > UINT64_MAX - size)) {
        return false;
    }

    uint64_t aligned = align_up(size, PAGE_SIZE_4K);
    if (aligned < size) {
        return false;
    }

    uint64_t irq_flags = spin_lock_irqsave(&g_paging_lock);
    bool ok = true;
    for (uint64_t offset = 0; offset < aligned; offset += PAGE_SIZE_4K) {
        if (!paging_map_page_locked(root_pml4_phys,
                                    virt_addr + offset,
                                    phys_addr + offset,
                                    flags)) {
            ok = false;
            break;
        }
    }
    spin_unlock_irqrestore(&g_paging_lock, irq_flags);
    return ok;
}


/* Returns true if all 512 entries in the table at table_phys are zero. */
static bool is_table_empty(uint64_t table_phys) {
    const uint64_t* table = table_virt_from_phys(table_phys);
    for (size_t i = 0; i < 512; i++) {
        if (table[i] != 0) {
            return false;
        }
    }
    return true;
}

/* After a PTE has been zeroed, walk back up the four levels and free any
 * intermediate page-table pages that are now completely empty.
 * Stops at the PML4 itself — that is never freed here.                    */
static void free_empty_tables(uint64_t root_pml4_phys, uint64_t virt_addr) {
    uint64_t* pml4 = table_virt_from_phys(root_pml4_phys & ENTRY_ADDR_MASK_4K);
    uint16_t l4 = pml4_index(virt_addr);
    uint64_t pml4e = pml4[l4];
    if (!(pml4e & PAGE_PRESENT) || (pml4e & PAGE_PS)) {
        return;
    }

    uint64_t pdpt_phys = pml4e & ENTRY_ADDR_MASK_4K;
    uint64_t* pdpt = table_virt_from_phys(pdpt_phys);
    uint16_t l3 = pdpt_index(virt_addr);
    uint64_t pdpte = pdpt[l3];
    if (!(pdpte & PAGE_PRESENT) || (pdpte & PAGE_PS)) {
        return;
    }

    uint64_t pd_phys = pdpte & ENTRY_ADDR_MASK_4K;
    uint64_t* pd = table_virt_from_phys(pd_phys);
    uint16_t l2 = pd_index(virt_addr);
    uint64_t pde = pd[l2];
    if (!(pde & PAGE_PRESENT) || (pde & PAGE_PS)) {
        return;
    }

    uint64_t pt_phys = pde & ENTRY_ADDR_MASK_4K;

    /* Level 1 (PT): free if empty */
    if (is_table_empty(pt_phys)) {
        pmm_free_page_phys(pt_phys);
        g_paging_info.table_pages_allocated--;
        pd[l2] = 0;

        /* Level 2 (PD): free if now empty */
        if (is_table_empty(pd_phys)) {
            pmm_free_page_phys(pd_phys);
            g_paging_info.table_pages_allocated--;
            pdpt[l3] = 0;

            /* Level 3 (PDPT): free if now empty */
            if (is_table_empty(pdpt_phys)) {
                pmm_free_page_phys(pdpt_phys);
                g_paging_info.table_pages_allocated--;
                pml4[l4] = 0;
                /* PML4 itself is never freed here — it lives for the
                 * lifetime of the address space.                         */
            }
        }
    }
}


static bool paging_unmap_page_locked(uint64_t root_pml4_phys,
                                     uint64_t virt_addr,
                                     uint64_t* out_phys_addr) {
    uint64_t* pte = NULL;
    if (!walk_to_pte(root_pml4_phys, virt_addr, false, false, &pte)) {
        return false;
    }

    uint64_t entry = *pte;
    if ((entry & PAGE_PRESENT) == 0) {
        return false;
    }

    if (out_phys_addr) {
        /* Return the physical page base address only — never mix in virtual
         * offset bits, as pmm_free_page_phys() requires page-aligned input
         * and will silently drop non-aligned addresses.                    */
        *out_phys_addr = entry & ENTRY_ADDR_MASK_4K;
    }

    *pte = 0;
    paging_invlpg(virt_addr);

    /* Walk back up the table hierarchy and free any levels that are now
     * completely empty, returning those physical pages to the PMM.        */
    free_empty_tables(root_pml4_phys, virt_addr);

    return true;
}

bool paging_unmap_page(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t* out_phys_addr) {
    uint64_t irq_flags = spin_lock_irqsave(&g_paging_lock);
    bool ok = paging_unmap_page_locked(root_pml4_phys, virt_addr, out_phys_addr);
    spin_unlock_irqrestore(&g_paging_lock, irq_flags);
    return ok;
}

static bool paging_protect_page_locked(uint64_t root_pml4_phys,
                                       uint64_t virt_addr,
                                       uint64_t flags) {
    uint64_t* pte = NULL;
    if (!walk_to_pte(root_pml4_phys, virt_addr, false, false, &pte)) {
        return false;
    }

    uint64_t entry = *pte;
    if ((entry & PAGE_PRESENT) == 0) {
        return false;
    }

    uint64_t phys_addr = entry & ENTRY_ADDR_MASK_4K;
    uint64_t hw_flags = paging_flags_to_hw(flags) | PAGE_PRESENT;
    *pte = phys_addr | hw_flags;
    paging_invlpg(virt_addr);
    return true;
}

bool paging_protect_page(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t flags) {
    uint64_t irq_flags = spin_lock_irqsave(&g_paging_lock);
    bool ok = paging_protect_page_locked(root_pml4_phys, virt_addr, flags);
    spin_unlock_irqrestore(&g_paging_lock, irq_flags);
    return ok;
}

bool paging_protect_range(uint64_t root_pml4_phys, uint64_t virt_addr, uint64_t size, uint64_t flags) {
    if (size == 0) {
        return false;
    }

    if (virt_addr > UINT64_MAX - size) {
        return false;
    }

    uint64_t aligned = align_up(size, PAGE_SIZE_4K);
    if (aligned < size) {
        return false;
    }

    uint64_t irq_flags = spin_lock_irqsave(&g_paging_lock);
    bool ok = true;
    for (uint64_t offset = 0; offset < aligned; offset += PAGE_SIZE_4K) {
        if (!paging_protect_page_locked(root_pml4_phys, virt_addr + offset, flags)) {
            ok = false;
        }
    }
    spin_unlock_irqrestore(&g_paging_lock, irq_flags);

    return ok;
}
