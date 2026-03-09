
#include "panic.h"
#include "include/printk.h"
#include <stdint.h>


__attribute__((noreturn))
void earlyPanic(void){
        for(;;){
                __asm__ __volatile__ ("cli; hlt");
        }
        __builtin_unreachable();
}

// Each stack frame looks like this in memory (x86-64 SysV ABI):
// [rbp+0] = saved rbp of caller
// [rbp+8] = return address (rip of caller)
/*
typedef struct {
    uint64_t rbp;
    uint64_t rip;
} __attribute__((packed)) stack_frame_t;

void print_stack_trace(uint64_t rbp) {
    printk("Stack trace:\n");

    for (int i = 0; i < 16; i++) {
        if (rbp == 0 || rbp < 0xffff800000000000)
            break;

        stack_frame_t *frame = (stack_frame_t *)rbp;
        printk("  [%d] rip = 0x%016llx\n", i, frame->rip);

        if (frame->rip == 0)
            break;

        rbp = frame->rbp;
    }
}

__attribute__((noreturn))
void kpanic(registers_t *regs, const char *fmt, ...) {
    // 1. Disable interrupts
    __asm__ volatile ("cli");

    // 2. Print reason
    printk("\nKERNEL PANIC\n");
    // ... va_args handling for fmt ...

    // 3. Print registers (if regs != NULL)
    if (regs) {
        printk("rip=%016llx  rsp=%016llx  rbp=%016llx\n",
               regs->rip, regs->rsp, regs->rbp);
        printk("rax=%016llx  rbx=%016llx  rcx=%016llx\n",
               regs->rax, regs->rbx, regs->rcx);
        
        printk("cr2=%016llx\n", read_cr2());
    }

    // 4. Stack trace
    uint64_t rbp = regs ? regs->rbp : ({uint64_t r; __asm__("mov %%rbp,%0":"=r"(r)); r;});
    print_stack_trace(rbp);

    for (;;) __asm__ volatile ("hlt");
}
*/

__attribute__((noreturn))
void panic(const char *msg) {
        __asm__ volatile("cli");
        //clearScreen();

        printk("KERNEL PANIC: %s\n", msg);
        for (;;) __asm__ volatile ("hlt");
}