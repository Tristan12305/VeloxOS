

#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/irq.h>
#include <arch/x86_64/cpu/apic.h>
#include <arch/x86_64/cpu/ioapic.h>
#include <arch/x86_64/drivers/IO/ps2_keyboard.h>
#include <mm/paging.h>
#include <mm/pmm.h>
#include <kernel/panic.h>
#include <kernel/helpers.h>
#include <boot/boot.h>
#include <include/printk.h>
#include <arch/x86_64/cpu/acpi.h>
#include <arch/x86_64/cpu/madt.h>
#include <mm/vmalloc.h>
#include <arch/x86_64/drivers/block/pci.h>
#include <arch/x86_64/drivers/block/virtio/vblk.h>
#include <fs/partition/gpt.h>
#include <include/libk.h>
#include <arch/x86_64/cpu/percpu.h>
#include <arch/x86_64/cpu/vendor.h>
#include <kernel/init.h>
#include <kernel/sched.h>

__attribute__((noreturn))
void kmain(void){

        arch_early_init();
        mem_init();
        arch_cpu_init();
        arch_acpi_init();
        smp_init();
        sched_init();
        arch_irq_init();

        


        virtio_blk_init();
        gpt_init();


        idle();
        
}




/*

__attribute__((noreturn))
void kmain(void){
        cli();
        limine_init();
        printk_init();
        pmm_init(&g_memory_map);
        paging_init(&g_memory_map);
        x86_initGDT();
        idt_init();
        acpi_init();
        madt_init();
        pci_enumerate();
        vmalloc_init();
        cpuid_get_info(&g_cpus[0].cpu_info);
        if (!x86_lapic_bsp_init()) {
                panic("LAPIC init failed");
        }
        if (!x86_ioapic_init()) {
                panic("IOAPIC init failed");
        }
        if (!ps2_keyboard_init()) {
                panic("PS/2 keyboard init failed");
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
        printk("Interrupts enabled. Waiting for IRQs...\n");
        kmalloc_init();
        virtio_blk_init();
        gpt_init();

        int *buffer = kmalloc(256);
        if (!buffer){
                printk("failed allocation with kmalloc\n");
        }
        else{
                *buffer = 123;
                printk("%i\n", buffer);
                kfree(buffer);
        }

        sti();

        for (;;) {
                hlt();
        }
}
*/
