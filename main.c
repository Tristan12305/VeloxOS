

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/cpu/apic.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "kernel/panic.h"
#include "kernel/helpers.h"
#include "boot/boot.h"
#include "include/printk.h"

__attribute__((noreturn))
void kmain(void){
        cli();
        limine_init();
        printk_init();
        pmm_init(&g_memory_map);
        paging_init(&g_memory_map);
        x86_initGDT();
        idt_init();
        if (!x86_lapic_init()) {
                panic("LAPIC init failed");
        }
        const pmm_info_t* pmm = pmm_get_info();
        const paging_info_t* paging = paging_get_info();
        const lapic_info_t* lapic = x86_lapic_get_info();
        printk("PMM ready: total=%llu free=%llu used=%llu bitmap=%llu bytes\n",
               pmm->total_pages, pmm->free_pages, pmm->used_pages, pmm->bitmap_bytes);
        printk("Paging ready: pml4=0x%llx tables=%llu reclaimed=%llu\n",
               (unsigned long long)paging->root_pml4_phys,
               (unsigned long long)paging->table_pages_allocated,
               (unsigned long long)paging->reclaimed_bootloader_pages);
        printk("LAPIC ready: mode=%s id=%u base=0x%llx\n",
               lapic->x2apic_enabled ? "x2apic" : "xapic",
               lapic->apic_id,
               (unsigned long long)lapic->apic_base_phys);
        printk("Hello world");
        printk("Interrupts enabled. Waiting for IRQs...\n");
        sti();
        for (;;) {
                hlt();
        }
}
