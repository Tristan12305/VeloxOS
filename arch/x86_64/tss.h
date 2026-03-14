#pragma once
#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3];       // rsp[0] = rsp0 (ring-0 stack pointer)
    uint64_t reserved1;
    uint64_t ist[7];       // ist[0]=IST1 .. ist[6]=IST7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;   // set to sizeof(tss64_t) to disable I/O bitmap
} tss64_t;

_Static_assert(sizeof(tss64_t) == 104, "tss64_t must be 104 bytes");