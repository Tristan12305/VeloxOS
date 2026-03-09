#pragma once
#include <stdint.h>

#define X86_GDT_CODE_SEGMENT 0x08
#define X86_GDT_DATA_SEGMENT 0x10

void x86_initGDT();
