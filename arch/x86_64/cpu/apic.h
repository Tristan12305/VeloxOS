#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "vendor.h"

typedef struct {
    bool initialized;

    cpu_vendor_t vendor;

    bool bsp;
    bool x2apic_supported;
    bool x2apic_enabled;

    uint64_t apic_base_msr_raw;
    uint64_t apic_base_phys;
    uint64_t apic_base_virt;

    uint32_t apic_id;
    uint32_t apic_version;
    uint32_t max_lvt_entries;

    uint8_t spurious_vector;
    uint8_t timer_vector;
    uint32_t timer_initial_count;
    bool timer_periodic;
} lapic_info_t;

bool x86_lapic_init(void);
bool x86_lapic_ready(void);
const lapic_info_t* x86_lapic_get_info(void);

uint32_t x86_lapic_id(void);
void x86_lapic_eoi(void);
void x86_lapic_handle_irq(uint8_t vector);

const char* x86_cpu_vendor_name(cpu_vendor_t vendor);

bool x86_lapic_timer_start(uint8_t vector, uint32_t initial_count, bool periodic);
void x86_lapic_timer_stop(void);
uint64_t x86_lapic_timer_ticks(void);
