
#include "panic.h"
#include "include/printk.h"
#include "kernel/ipi.h"
#include <stdint.h>


__attribute__((noreturn))
void earlyPanic(void){
        for(;;){
                __asm__ volatile ("cli; hlt");
        }
        __builtin_unreachable();
}


__attribute__((noreturn))
void panic(const char *msg) {
        __asm__ volatile("cli");
        ipi_halt_others();
        //clearScreen();

        printk("KERNEL PANIC: %s\n", msg);
        for (;;) __asm__ volatile ("hlt");
}
