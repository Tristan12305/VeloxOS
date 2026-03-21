#include "isr.h"
#include "irq.h"
#include "cpu/apic.h"
#include <include/printk.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/ipi.h>

static const char* const exception_messages[] = {
    "Divide by Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD FPU Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection",
    "VMM Communication",
    "Security Exception",
    "Reserved"
};


interrupt_frame *isr_handler(interrupt_frame* frame) {
    if (frame->vector < 32) {
        printk("\n[EXCEPTION] %s (vector=%llu)\n",
               exception_messages[frame->vector],
               (unsigned long long)frame->vector);
        printk("  error_code : 0x%llx\n", (unsigned long long)frame->error_code);
        printk("  rip        : 0x%llx\n", (unsigned long long)frame->rip);
        printk("  cs         : 0x%llx\n", (unsigned long long)frame->cs);
        printk("  rflags     : 0x%llx\n", (unsigned long long)frame->rflags);
        //printk("  rsp        : 0x%llx\n", (unsigned long long)frame->rsp);  // only valid in ring0 frame if no priv change
        printk("  rax        : 0x%llx\n", (unsigned long long)frame->rax);
        printk("  rbx        : 0x%llx\n", (unsigned long long)frame->rbx);
        printk("  rcx        : 0x%llx\n", (unsigned long long)frame->rcx);
        printk("  rdx        : 0x%llx\n", (unsigned long long)frame->rdx);

        // CR2 holds the faulting virtual address for page faults
        if (frame->vector == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            printk("  cr2 (fault addr) : 0x%llx\n", (unsigned long long)cr2);
            printk("  page fault flags : %s%s%s%s%s\n",
                   (frame->error_code & (1<<0)) ? "PRESENT "   : "NOT-PRESENT ",
                   (frame->error_code & (1<<1)) ? "WRITE "     : "READ ",
                   (frame->error_code & (1<<2)) ? "USER "      : "KERNEL ",
                   (frame->error_code & (1<<3)) ? "RSVD-BIT "  : "",
                   (frame->error_code & (1<<4)) ? "INSTR-FETCH" : "");
        }

        panic(exception_messages[frame->vector]);
    }

    irq_dispatch(frame);

    if (frame->vector == SCHED_TICK_VECTOR) {
        frame = sched_on_tick(frame);
    }
    if (frame->vector == IPI_VECTOR_RESCHED && sched_is_ready()) {
        frame = sched_on_tick(frame);
    }

    if (x86_lapic_ready()) {
        x86_lapic_handle_irq((uint8_t)frame->vector);
    }

    return frame;
}
