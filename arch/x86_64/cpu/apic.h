#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "vendor.h"

struct cpu_t;

typedef struct {
    bool initialized;
    bool bsp;
    bool x2apic_supported;
    bool x2apic_enabled;

    uint64_t apic_base_msr_raw;
    uint64_t apic_base_phys;
    uint64_t apic_base_virt;

    uint32_t apic_id;
    uint32_t apic_version;
    uint32_t max_lvt_entries;

    uint8_t  spurious_vector;
    uint8_t  timer_vector;
    uint32_t timer_initial_count;
    bool     timer_periodic;
} lapic_info_t;

bool x86_lapic_bsp_init(void);

bool x86_lapic_ap_init(struct cpu_t *cpu);

bool               x86_lapic_ready(void);
const lapic_info_t *x86_lapic_get_info(void);

uint32_t x86_lapic_id(void);

void x86_lapic_eoi(void);
void x86_lapic_handle_irq(uint8_t vector);

bool     x86_lapic_timer_start(uint8_t vector, uint32_t initial_count, bool periodic);
void     x86_lapic_timer_stop(void);
uint64_t x86_lapic_timer_ticks(void);
bool     x86_lapic_timer_calibrate(uint32_t target_hz, uint32_t *out_initial_count);

#define LAPIC_ICR_DELIVERY_FIXED  0x00000U
#define LAPIC_ICR_DELIVERY_INIT   0x00500U
#define LAPIC_ICR_DELIVERY_SIPI   0x00600U
#define LAPIC_ICR_LEVEL_ASSERT    (1U << 14)
#define LAPIC_ICR_TRIGGER_LEVEL   (1U << 15)

void x86_lapic_send_ipi(uint32_t target_apic_id, uint32_t icr_low);

void x86_lapic_send_init(uint32_t target_apic_id);
void x86_lapic_send_sipi(uint32_t target_apic_id, uint8_t trampoline_page);

const char *x86_cpu_vendor_name(cpu_vendor_t vendor);
