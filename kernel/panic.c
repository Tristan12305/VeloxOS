
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


__attribute__((noreturn))
void panic(const char *msg) {
        __asm__ volatile("cli");
        //clearScreen();

        printk("KERNEL PANIC: %s\n", msg);
        for (;;) __asm__ volatile ("hlt");
}