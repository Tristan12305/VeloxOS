#include "gdt.h"
#include <stdint.h>
#include <string.h>
#include <arch/x86_64/cpu/percpu.h>


typedef struct {
    uint16_t lowLimit;
    uint16_t lowBase;
    uint8_t  middleBase;
    uint8_t  access;
    uint8_t  limitFlagsHigh;
    uint8_t  highBase;
} __attribute__((packed)) gdtEntry;

typedef struct {
    uint16_t  limit;
    gdtEntry* address;
} __attribute__((packed)) gDescriptor;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid1;
    uint8_t  access;
    uint8_t  limit_high;
    uint8_t  base_mid2;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed)) tssDescriptor;

typedef enum {
    CODE_READABLE        = 0x02,
    DATA_WRITEABLE       = 0x02,
    DATA_SEGMENT         = 0x10,
    CODE_SEGMENT         = 0x18,
    RING_ZERO            = 0x00,
    RING_THREE           = 0x60,  // bits 5-6 set for DPL=3
    ACCESS_PRESENT       = 0x80
} gdtAccess;

typedef enum {
    FLAG_64BIT       = 0x20,   // L bit — marks 64-bit code segment
    FLAG_32BIT       = 0x40,   // D/B bit — shouldnt be set alongside FLAG_64BIT
    GRANULARITY_4K   = 0x80
} gdtFlags;


#define GDT_LOWER_LIMIT(limit)          ((limit) & 0xFFFF)
#define GDT_LOWER_BASE(base)            ((base) & 0xFFFF)
#define GDT_MIDDLE_BASE(base)           (((base) >> 16) & 0xFF)
#define GDT_FLAGS_LIMIT_HIGH(limit, flags) ((((limit) >> 16) & 0x0F) | ((flags) & 0xF0))
#define GDT_HIGHER_BASE(base)           (((base) >> 24) & 0xFF)

#define GDT_ENTRY(base, limit, access, flags) ((gdtEntry){ \
    GDT_LOWER_LIMIT(limit),                     \
    GDT_LOWER_BASE(base),                       \
    GDT_MIDDLE_BASE(base),                      \
    (access),                                   \
    GDT_FLAGS_LIMIT_HIGH(limit, flags),         \
    GDT_HIGHER_BASE(base)                       \
})


extern void x86_loadGDT(gDescriptor* descriptor, uint16_t codeSegment, uint16_t dataSegment);

static inline void x86_loadTSS(uint16_t selector) {
    __asm__ volatile("ltr %0" :: "m"(selector));
}

static inline uintptr_t read_rsp(void) {
    uintptr_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    return rsp;
}

static void gdt_set_tss(uint8_t *gdt_base, tss64_t *tss) {
    tssDescriptor *tss_desc = (tssDescriptor *)(gdt_base + (3 * 8));
    uint64_t base = (uint64_t)(uintptr_t)tss;
    uint32_t limit = (uint32_t)(sizeof(tss64_t) - 1);

    tss_desc->limit_low  = (uint16_t)(limit & 0xFFFF);
    tss_desc->base_low   = (uint16_t)(base & 0xFFFF);
    tss_desc->base_mid1  = (uint8_t)((base >> 16) & 0xFF);
    tss_desc->access     = 0x89; // present, ring 0, available 64-bit TSS
    tss_desc->limit_high = (uint8_t)((limit >> 16) & 0x0F);
    tss_desc->base_mid2  = (uint8_t)((base >> 24) & 0xFF);
    tss_desc->base_high  = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    tss_desc->reserved   = 0;
}

void x86_gdt_init_cpu(struct cpu_t *cpu, uintptr_t rsp0) {
    if (!cpu) {
        return;
    }

    memset(cpu->gdt, 0, sizeof(cpu->gdt));

    gdtEntry *entries = (gdtEntry *)cpu->gdt;
    entries[0] = GDT_ENTRY(0, 0, 0, 0);
    entries[1] = GDT_ENTRY(0, 0,
                           ACCESS_PRESENT | RING_ZERO | CODE_SEGMENT | CODE_READABLE,
                           FLAG_64BIT | GRANULARITY_4K);
    entries[2] = GDT_ENTRY(0, 0,
                           ACCESS_PRESENT | RING_ZERO | DATA_SEGMENT | DATA_WRITEABLE,
                           GRANULARITY_4K);

    memset(&cpu->tss, 0, sizeof(cpu->tss));
    cpu->tss.rsp[0] = rsp0;
    cpu->tss.iomap_base = (uint16_t)sizeof(tss64_t);
    gdt_set_tss(cpu->gdt, &cpu->tss);

    gDescriptor gdtr = {
        .limit = (uint16_t)(CPU_GDT_SIZE - 1),
        .address = (gdtEntry *)cpu->gdt,
    };

    x86_loadGDT(&gdtr, X86_GDT_CODE_SEGMENT, X86_GDT_DATA_SEGMENT);
    x86_loadTSS(X86_GDT_TSS_SEGMENT);
}

void x86_initGDT() {
    cpu_t *cpu = &g_cpus[0];
    uintptr_t rsp0 = read_rsp();
    cpu->kernel_stack_top = rsp0;
    x86_gdt_init_cpu(cpu, rsp0);
}
