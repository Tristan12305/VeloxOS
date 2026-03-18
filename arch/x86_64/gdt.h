#pragma once
#include <stdint.h>

#define X86_GDT_CODE_SEGMENT 0x08
#define X86_GDT_DATA_SEGMENT 0x10
#define X86_GDT_TSS_SEGMENT  0x18

struct cpu_t;

void x86_initGDT();
void x86_gdt_init_cpu(struct cpu_t *cpu, uintptr_t rsp0);
