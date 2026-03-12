#pragma once


void sti(){
        __asm__ volatile("sti");
}


void cli(){
        __asm__ volatile("cli");
}


void hlt(){
        __asm__ volatile("hlt");
}

void ThrowException(){
        volatile int a = 1;
        volatile int b = 0;
        volatile int c = a / b;
        (void)c;
}
