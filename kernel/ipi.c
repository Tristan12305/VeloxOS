#include "ipi.h"

#include <arch/x86_64/irq.h>
#include <arch/x86_64/cpu/apic.h>
#include <arch/x86_64/cpu/percpu.h>
#include <mm/paging.h>
#include <include/printk.h>

static volatile int g_ipi_ready;

static void ipi_send_vector_apic(uint32_t apic_id, uint8_t vector) {
    uint32_t icr_low = LAPIC_ICR_DELIVERY_FIXED | (uint32_t)vector;
    x86_lapic_send_ipi(apic_id, icr_low);
}

static void ipi_send_vector_cpu(uint32_t cpu_id, uint8_t vector) {
    if (!x86_lapic_ready()) {
        return;
    }
    if (cpu_id >= g_cpu_count) {
        return;
    }
    cpu_t *cpu = &g_cpus[cpu_id];
    if (!cpu->online) {
        return;
    }
    ipi_send_vector_apic(cpu->apic_id, vector);
}

static void ipi_broadcast_vector(uint8_t vector, int include_self) {
    if (!x86_lapic_ready()) {
        return;
    }
    cpu_t *self = cpu_self();
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        cpu_t *cpu = &g_cpus[i];
        if (!cpu->online) {
            continue;
        }
        if (!include_self && cpu->apic_id == self->apic_id) {
            continue;
        }
        ipi_send_vector_apic(cpu->apic_id, vector);
    }
}

static void ipi_tlb_handler(interrupt_frame *frame) {
    (void)frame;
    paging_tlb_flush_all();
}

__attribute__((noreturn))
static void ipi_halt_handler(interrupt_frame *frame) {
    (void)frame;
    if (x86_lapic_ready()) {
        x86_lapic_eoi();
    }
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void ipi_init(void) {
    if (g_ipi_ready) {
        return;
    }

    if (!irq_register_handler(IPI_VECTOR_TLB, ipi_tlb_handler)) {
        printk("[ipi] failed to register TLB IPI handler\n");
    }
    if (!irq_register_handler(IPI_VECTOR_HALT, ipi_halt_handler)) {
        printk("[ipi] failed to register HALT IPI handler\n");
    }

    g_ipi_ready = 1;
}

int ipi_ready(void) {
    return g_ipi_ready != 0;
}

void ipi_send_resched(uint32_t cpu_id) {
    ipi_send_vector_cpu(cpu_id, IPI_VECTOR_RESCHED);
}

void ipi_broadcast_resched(void) {
    ipi_broadcast_vector(IPI_VECTOR_RESCHED, 0);
}

void ipi_send_tlb_shootdown(uint32_t cpu_id) {
    ipi_send_vector_cpu(cpu_id, IPI_VECTOR_TLB);
}

void ipi_broadcast_tlb_shootdown(void) {
    ipi_broadcast_vector(IPI_VECTOR_TLB, 0);
}

void ipi_tlb_shootdown(void) {
    if (!g_ipi_ready || g_cpu_count <= 1) {
        return;
    }
    ipi_broadcast_vector(IPI_VECTOR_TLB, 0);
}

void ipi_send_halt(uint32_t cpu_id) {
    ipi_send_vector_cpu(cpu_id, IPI_VECTOR_HALT);
}

void ipi_halt_others(void) {
    if (!g_ipi_ready || g_cpu_count <= 1) {
        return;
    }
    ipi_broadcast_vector(IPI_VECTOR_HALT, 0);
}
