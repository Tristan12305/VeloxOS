#include "gdt.h"
#include <stdint.h>


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

#define GDT_ENTRY(base, limit, access, flags) { \
    GDT_LOWER_LIMIT(limit),                     \
    GDT_LOWER_BASE(base),                       \
    GDT_MIDDLE_BASE(base),                      \
    (access),                                   \
    GDT_FLAGS_LIMIT_HIGH(limit, flags),         \
    GDT_HIGHER_BASE(base)                       \
}

gdtEntry GDT[] = {
    // Null descriptor
    GDT_ENTRY(0, 0, 0, 0),

    // Kernel code segment (64-bit)
    // FLAG_64BIT sets the L bit.
    GDT_ENTRY(0, 0, ACCESS_PRESENT | RING_ZERO | CODE_SEGMENT | CODE_READABLE,
              FLAG_64BIT | GRANULARITY_4K),

    // Kernel data segment
    // In long mode, data segments mostly don't matter but ss must be valid.
    GDT_ENTRY(0, 0, ACCESS_PRESENT | RING_ZERO | DATA_SEGMENT | DATA_WRITEABLE,
              GRANULARITY_4K),
};

gDescriptor gdtDescriptor = { sizeof(GDT) - 1, GDT };


extern void x86_loadGDT(gDescriptor* descriptor, uint16_t codeSegment, uint16_t dataSegment);

void x86_initGDT() {
    x86_loadGDT(&gdtDescriptor, X86_GDT_CODE_SEGMENT, X86_GDT_DATA_SEGMENT);
    
}
