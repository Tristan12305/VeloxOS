#include "idt.h"
#include <stdint.h>

#define IDT_MAX_DESCRIPTORS 256
#define GDT_OFFSET_KERNEL_CODE 0x08


typedef struct {
    uint16_t isr_low; // lower 16 bits of the ISR addr
    uint16_t kernel_cs; 
    uint8_t ist;
    uint8_t attributes;   // Type and attributes
	uint16_t isr_mid;      // The higher 16 bits of the lower 32 bits of the ISR's address
	uint32_t isr_high;     // The higher 32 bits of the ISR's address
	uint32_t reserved; // set to 0

}__attribute__((packed)) idtEntry;

typedef struct {
	uint16_t	limit;
	uint64_t	base;
} __attribute__((packed)) idtr_r;

__attribute__((aligned(0x10))) 
static idtEntry idt[256];

static idtr_r idtr;

static void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    idtEntry* descriptor = &idt[vector];

    descriptor->isr_low        = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs      = GDT_OFFSET_KERNEL_CODE;
    descriptor->ist            = 0;
    descriptor->attributes     = flags;
    descriptor->isr_mid        = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high       = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved       = 0;
}

extern void* isr_stub_table[];

void idt_init() {
    idtr.base = (uintptr_t)&idt[0];
    idtr.limit = (uint16_t)(sizeof(idt) - 1);

    for (uint16_t vector = 0; vector < IDT_MAX_DESCRIPTORS; vector++) {
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);
    }

    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

//for the other threads

void idt_load(void) {
    __asm__ volatile ("lidt %0" :: "m"(idtr));
}



