#include <kernel/init.h>
#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/cpu/percpu.h>
#include <arch/x86_64/cpu/apic.h>
#include <arch/x86_64/cpu/ioapic.h>
#include <boot/boot.h>
#include <include/printk.h>
#include <kernel/panic.h>
#include <kernel/ipi.h>
#include "drivers/IO/ps2_keyboard.h"


void arch_early_init(void) {
    __asm__ volatile ("cli");

    limine_init();
    printk_init();
    cpuid_get_info(&g_cpus[0].cpu_info);
}

void arch_cpu_init(void) {

    x86_initGDT();

    //cpu_setup_gdt_tss(&g_cpus[0], /* BSP stack top */ 0);
    idt_init();
    if (!x86_lapic_bsp_init())
        panic("LAPIC BSP init failed");
    ipi_init();
}

void arch_irq_init(void) {
    if (!x86_ioapic_init())
        panic("IOAPIC init failed");
    if (!ps2_keyboard_init())
        panic("PS/2 keyboard init failed");
    __asm__ volatile ("sti");
}

__attribute__((noreturn))
void idle(void) {
    for (;;)
        __asm__ volatile ("hlt");
}
