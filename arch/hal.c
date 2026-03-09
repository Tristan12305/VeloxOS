#include "hal.h"
#include "x86_64/gdt.h"



//testing
#define X86_64

#ifdef X86_64

void initHAL(){
        x86_initGDT();
}

#endif


