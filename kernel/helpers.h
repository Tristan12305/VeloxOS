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


