#include "isr.h"
#include "cpu/apic.h"
#include <include/printk.h>
#include <kernel/panic.h>

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

void isr_handler(interrupt_frame* frame) {
    if (frame->vector < 32) {
        printk("[exc] vec=%llu err=0x%llx rip=0x%llx cs=0x%llx rflags=0x%llx\n",
               (unsigned long long)frame->vector,
               (unsigned long long)frame->error_code,
               (unsigned long long)frame->rip,
               (unsigned long long)frame->cs,
               (unsigned long long)frame->rflags);

        if (interrupt_from_user(frame)) {
            const interrupt_frame_ring3* user_frame = (const interrupt_frame_ring3*)frame;
            (void)user_frame;
        } else {
            const interrupt_frame_ring0* kernel_frame = (const interrupt_frame_ring0*)frame;
            (void)kernel_frame;
        }

        panic(exception_messages[frame->vector]);
    }

    /* Hardware IRQ / software interrupt vectors. */
    if (x86_lapic_ready()) {
        x86_lapic_handle_irq((uint8_t)frame->vector);
    }
}
