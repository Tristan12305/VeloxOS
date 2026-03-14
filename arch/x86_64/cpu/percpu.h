#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <arch/x86_64/cpu/vendor.h>
#include <arch/x86_64/cpu/apic.h>
#include <arch/x86_64/tss.h>

#define MAX_CPUS 256

// GDT layout per CPU:
//   [0]   null descriptor          (8 bytes)
//   [1]   kernel code segment      (8 bytes)
//   [2]   kernel data segment      (8 bytes)
//   [3-4] 64-bit TSS descriptor    (16 bytes — two 8-byte slots)

#define CPU_GDT_ENTRY_COUNT  5
#define CPU_GDT_SIZE         (CPU_GDT_ENTRY_COUNT * 8)   // 40 bytes

typedef struct cpu_t{

    uint32_t        cpu_id;           // logical index: BSP=0, APs=1..N-1
    uint32_t        apic_id;          // hardware LAPIC ID from MADT

    bool            is_bsp;
    volatile bool   online;           // set true by each CPU once it's running

    cpu_info_t      cpu_info;

 
    lapic_info_t    lapic;
    volatile uint64_t lapic_timer_ticks;
    bool            lapic_timer_first_tick;

    tss64_t         tss;
    uint8_t         gdt[CPU_GDT_SIZE];

    uintptr_t       kernel_stack_top; // set by SMP init before waking AP

} __attribute__((aligned(64))) cpu_t;

extern cpu_t     g_cpus[MAX_CPUS];
extern uint32_t  g_cpu_count;        // total (BSP + all APs that came online)

// Return a pointer to the calling CPU's cpu_t.
// Phase 1 (now): walks g_cpus[] matching LAPIC ID — safe before GS is set up.
// Phase 2 (later): single rdgsbase instruction once GS-base points at cpu_t.
cpu_t *cpu_self(void);