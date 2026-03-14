
#include "percpu.h"
#include "apic.h"

#include <include/printk.h>
#include <cpuid.h>
#include <mm/paging.h>
#include <mm/pmm.h>

#define IA32_APIC_BASE_MSR       0x1BU
#define IA32_X2APIC_ID_MSR       0x802U   // x2APIC ID register

#define APIC_BASE_BSP_BIT            (1ULL << 8)
#define APIC_BASE_X2APIC_ENABLE_BIT  (1ULL << 10)
#define APIC_BASE_GLOBAL_ENABLE_BIT  (1ULL << 11)
#define APIC_BASE_ADDR_MASK          0x000FFFFFFFFFF000ULL

#define LAPIC_REG_ID            0x020U
#define LAPIC_REG_VERSION       0x030U
#define LAPIC_REG_TPR           0x080U
#define LAPIC_REG_EOI           0x0B0U
#define LAPIC_REG_SVR           0x0F0U
#define LAPIC_REG_ESR           0x280U
#define LAPIC_REG_ICR_LOW       0x300U   // xAPIC only; x2APIC uses MSR 0x830
#define LAPIC_REG_ICR_HIGH      0x310U   // xAPIC only
#define LAPIC_REG_LVT_TIMER     0x320U
#define LAPIC_REG_LVT_THERM     0x330U
#define LAPIC_REG_LVT_PERF      0x340U
#define LAPIC_REG_LVT_LINT0     0x350U
#define LAPIC_REG_LVT_LINT1     0x360U
#define LAPIC_REG_LVT_ERROR     0x370U
#define LAPIC_REG_TIMER_INITCNT 0x380U
#define LAPIC_REG_TIMER_CURRCNT 0x390U
#define LAPIC_REG_TIMER_DIVCONF 0x3E0U
#define LAPIC_REG_LVT_CMCI      0x2F0U

#define LAPIC_SVR_SOFT_ENABLE    (1U << 8)
#define LAPIC_LVT_MASKED         (1U << 16)
#define LAPIC_TIMER_PERIODIC     (1U << 17)
#define LAPIC_TIMER_DIVIDE_BY_16 0x3U

#define LAPIC_SPURIOUS_VECTOR          0xFFU
#define LAPIC_TIMER_VECTOR_DEFAULT     0x20U
#define LAPIC_TIMER_INIT_COUNT_DEFAULT 1000000U

#define LAPIC_ICR_STATUS_PENDING (1U << 12)

#define PIC1_DATA_PORT 0x21U
#define PIC2_DATA_PORT 0xA1U



static inline uint64_t rdmsr_u64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_u64(uint32_t msr, uint64_t value) {
    __asm__ volatile ("wrmsr"
                      :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                      : "memory");
}

static inline void outb_u8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

// Read a LAPIC register. Callers pass the lapic_info_t they already have —
// this avoids any cpu_self() call inside the register access path.
static uint32_t lapic_read_reg(const lapic_info_t *l, uint32_t reg) {
    if (l->x2apic_enabled) {
        return (uint32_t)rdmsr_u64(0x800U + (reg >> 4));
    }
    const volatile uint32_t *mmio = (const volatile uint32_t *)(uintptr_t)l->apic_base_virt;
    return mmio[reg >> 2];
}

// Write a LAPIC register. xAPIC path does a read-back after write to enforce
// ordering (the LAPIC MMIO write is not guaranteed to complete until the bus
// cycles are flushed; reading a non-ICR register forces that flush).
static void lapic_write_reg(lapic_info_t *l, uint32_t reg, uint32_t value) {
    if (l->x2apic_enabled) {
        wrmsr_u64(0x800U + (reg >> 4), value);
        return;
    }
    volatile uint32_t *mmio = (volatile uint32_t *)(uintptr_t)l->apic_base_virt;
    mmio[reg >> 2] = value;
    (void)mmio[LAPIC_REG_ID >> 2];   // read-back to flush posted write
}

static void lapic_mask_lvt(lapic_info_t *l, uint32_t reg) {
    lapic_write_reg(l, reg, lapic_read_reg(l, reg) | LAPIC_LVT_MASKED);
}

static void pic_disable_legacy_8259(void) {
    outb_u8(PIC1_DATA_PORT, 0xFFU);
    outb_u8(PIC2_DATA_PORT, 0xFFU);
}

// Wait for a pending IPI to be accepted by the bus (xAPIC only).
// x2APIC has no delivery-status bit; the MSR write is synchronous.
static void lapic_wait_icr_idle(lapic_info_t *l) {
    if (l->x2apic_enabled) return;
    while (lapic_read_reg(l, LAPIC_REG_ICR_LOW) & LAPIC_ICR_STATUS_PENDING) {
        __asm__ volatile ("pause");
    }
}


static bool lapic_configure(cpu_t *cpu) {
    lapic_info_t *l = &cpu->lapic;

    // Clear Task Priority Register — accept all interrupts.
    lapic_write_reg(l, LAPIC_REG_TPR, 0);

    // Mask every LVT entry we have.
    if (l->max_lvt_entries >= 1U) lapic_mask_lvt(l, LAPIC_REG_LVT_TIMER);
    if (l->max_lvt_entries >= 2U) lapic_mask_lvt(l, LAPIC_REG_LVT_THERM);
    if (l->max_lvt_entries >= 3U) lapic_mask_lvt(l, LAPIC_REG_LVT_PERF);
    if (l->max_lvt_entries >= 4U) lapic_mask_lvt(l, LAPIC_REG_LVT_LINT0);
    if (l->max_lvt_entries >= 5U) lapic_mask_lvt(l, LAPIC_REG_LVT_LINT1);
    if (l->max_lvt_entries >= 6U) lapic_mask_lvt(l, LAPIC_REG_LVT_ERROR);
    if (l->max_lvt_entries >= 7U) lapic_mask_lvt(l, LAPIC_REG_LVT_CMCI);

    // Clear ESR before and after (Intel SDM recommendation — must be written
    // twice to latch any pending errors, then clear them).
    if (!l->x2apic_enabled) {
        lapic_write_reg(l, LAPIC_REG_ESR, 0);
        lapic_write_reg(l, LAPIC_REG_ESR, 0);
    }

    // Enable the LAPIC via the Spurious Vector Register.
    uint32_t svr = lapic_read_reg(l, LAPIC_REG_SVR);
    svr  = (svr & ~0xFFU) | l->spurious_vector;
    svr |= LAPIC_SVR_SOFT_ENABLE;
    lapic_write_reg(l, LAPIC_REG_SVR, svr);

    // Issue a blanket EOI to clear any stale pending interrupt.
    lapic_write_reg(l, LAPIC_REG_EOI, 0);

    // Start the per-CPU periodic timer.
    cpu->lapic_timer_ticks      = 0;
    cpu->lapic_timer_first_tick = false;

    lapic_write_reg(l, LAPIC_REG_TIMER_DIVCONF, LAPIC_TIMER_DIVIDE_BY_16);

    uint32_t lvt = LAPIC_TIMER_VECTOR_DEFAULT | LAPIC_TIMER_PERIODIC;
    lapic_write_reg(l, LAPIC_REG_LVT_TIMER, lvt);
    lapic_write_reg(l, LAPIC_REG_TIMER_INITCNT, LAPIC_TIMER_INIT_COUNT_DEFAULT);

    l->timer_vector        = LAPIC_TIMER_VECTOR_DEFAULT;
    l->timer_initial_count = LAPIC_TIMER_INIT_COUNT_DEFAULT;
    l->timer_periodic      = true;

    l->initialized = true;
    return true;
}


bool x86_lapic_bsp_init(void) {
    if (!paging_ready()) {
        printk("[lapic] paging not ready\n");
        return false;
    }

    cpu_t *cpu = &g_cpus[0];   // BSP is always slot 0
    lapic_info_t *l = &cpu->lapic;

    // The full cpu_info (vendor, features, topology) is populated separately
    // via cpuid_get_info(&cpu->cpu_info) — see percpu init in smp.c / main.c.
    // Here we just need the two bits that gate LAPIC bring-up.
    uint32_t eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);

    bool has_apic   = ((edx >> 9)  & 1U) != 0;
    bool has_msr    = ((edx >> 5)  & 1U) != 0;
    bool has_x2apic = ((ecx >> 21) & 1U) != 0;

    if (!has_msr || !has_apic) {
        printk("[lapic] CPU lacks APIC or MSR support\n");
        return false;
    }

    uint64_t apic_base = rdmsr_u64(IA32_APIC_BASE_MSR);
    apic_base |= APIC_BASE_GLOBAL_ENABLE_BIT;
    wrmsr_u64(IA32_APIC_BASE_MSR, apic_base);
    apic_base = rdmsr_u64(IA32_APIC_BASE_MSR);   // re-read after write

    // If x2APIC was already enabled by firmware, nudge it back to xAPIC mode.
    // We use xAPIC by default for simplicity; x2APIC opt-in comes later.
    if ((apic_base & APIC_BASE_X2APIC_ENABLE_BIT) && has_x2apic) {
        wrmsr_u64(IA32_APIC_BASE_MSR, apic_base & ~APIC_BASE_X2APIC_ENABLE_BIT);
        apic_base = rdmsr_u64(IA32_APIC_BASE_MSR);
    }

    l->initialized      = false;
    l->bsp              = (apic_base & APIC_BASE_BSP_BIT) != 0;
    l->x2apic_supported = has_x2apic;
    l->x2apic_enabled   = (apic_base & APIC_BASE_X2APIC_ENABLE_BIT) != 0;
    l->apic_base_msr_raw = apic_base;
    l->apic_base_phys   = apic_base & APIC_BASE_ADDR_MASK;
    l->apic_base_virt   = l->x2apic_enabled ? 0
                                             : pmm_phys_to_virt(l->apic_base_phys);
    l->spurious_vector  = LAPIC_SPURIOUS_VECTOR;

    // Read hardware APIC ID and version.
    uint32_t id_reg  = lapic_read_reg(l, LAPIC_REG_ID);
    uint32_t ver_reg = lapic_read_reg(l, LAPIC_REG_VERSION);
    l->apic_id        = l->x2apic_enabled ? id_reg : (id_reg >> 24);
    l->apic_version   = ver_reg & 0xFFU;
    l->max_lvt_entries = ((ver_reg >> 16) & 0xFFU) + 1U;

    cpu->apic_id = l->apic_id;
    cpu->is_bsp  = l->bsp;
    g_cpu_count  = 1;

    pic_disable_legacy_8259();
    printk("[lapic] legacy PIC masked\n");

    if (!lapic_configure(cpu)) {
        printk("[lapic] BSP configure failed\n");
        return false;
    }

    printk("[lapic] BSP ready: mode=%s id=%u ver=0x%x lvt=%u base_phys=0x%llx timer_vec=%u\n",
           l->x2apic_enabled ? "x2apic" : "xapic",
           l->apic_id,
           l->apic_version,
           l->max_lvt_entries,
           (unsigned long long)l->apic_base_phys,
           (unsigned)l->timer_vector);

    return true;
}

bool x86_lapic_ap_init(cpu_t *cpu) {
    lapic_info_t *l = &cpu->lapic;

    // Inherit x2APIC mode from BSP — all CPUs in the system use the same mode.
    const lapic_info_t *bsp_lapic = &g_cpus[0].lapic;

    l->initialized      = false;
    l->bsp              = false;
    l->x2apic_supported = bsp_lapic->x2apic_supported;
    l->x2apic_enabled   = bsp_lapic->x2apic_enabled;
    l->spurious_vector  = LAPIC_SPURIOUS_VECTOR;

    // Read this AP's own APIC base MSR.
    uint64_t apic_base   = rdmsr_u64(IA32_APIC_BASE_MSR);
    apic_base           |= APIC_BASE_GLOBAL_ENABLE_BIT;
    wrmsr_u64(IA32_APIC_BASE_MSR, apic_base);
    apic_base            = rdmsr_u64(IA32_APIC_BASE_MSR);

    l->apic_base_msr_raw = apic_base;
    l->apic_base_phys    = apic_base & APIC_BASE_ADDR_MASK;
    l->apic_base_virt    = l->x2apic_enabled ? 0
                                              : pmm_phys_to_virt(l->apic_base_phys);

    uint32_t id_reg  = lapic_read_reg(l, LAPIC_REG_ID);
    uint32_t ver_reg = lapic_read_reg(l, LAPIC_REG_VERSION);
    l->apic_id        = l->x2apic_enabled ? id_reg : (id_reg >> 24);
    l->apic_version   = ver_reg & 0xFFU;
    l->max_lvt_entries = ((ver_reg >> 16) & 0xFFU) + 1U;

    // Verify the hardware ID matches what the BSP put in cpu->apic_id from MADT.
    if (l->apic_id != cpu->apic_id) {
        printk("[lapic] AP apic_id mismatch: expected %u got %u\n",
               cpu->apic_id, l->apic_id);
        return false;
    }

    if (!lapic_configure(cpu)) {
        printk("[lapic] AP %u configure failed\n", cpu->cpu_id);
        return false;
    }

    printk("[lapic] AP %u ready: id=%u base_phys=0x%llx\n",
           cpu->cpu_id, l->apic_id, (unsigned long long)l->apic_base_phys);

    return true;
}


bool x86_lapic_ready(void) {
    return cpu_self()->lapic.initialized;
}

const lapic_info_t *x86_lapic_get_info(void) {
    return &cpu_self()->lapic;
}

// Read the LAPIC ID directly from hardware — no cpu_t dependency.
// This is intentionally self-contained because cpu_self() calls it.
uint32_t x86_lapic_id(void) {
    uint64_t base = rdmsr_u64(IA32_APIC_BASE_MSR);
    if (base & APIC_BASE_X2APIC_ENABLE_BIT) {
        return (uint32_t)rdmsr_u64(IA32_X2APIC_ID_MSR);
    }
    uint64_t virt = pmm_phys_to_virt(base & APIC_BASE_ADDR_MASK);
    const volatile uint32_t *mmio = (const volatile uint32_t *)(uintptr_t)virt;
    return mmio[LAPIC_REG_ID >> 2] >> 24;
}

void x86_lapic_eoi(void) {
    cpu_t *cpu = cpu_self();
    if (!cpu->lapic.initialized) return;
    lapic_write_reg(&cpu->lapic, LAPIC_REG_EOI, 0);
}

void x86_lapic_handle_irq(uint8_t vector) {
    cpu_t *cpu = cpu_self();
    if (!cpu->lapic.initialized) return;

    if (vector == cpu->lapic.timer_vector && cpu->lapic.timer_vector != 0) {
        cpu->lapic_timer_ticks++;
        if (!cpu->lapic_timer_first_tick) {
            cpu->lapic_timer_first_tick = true;
            printk("[lapic] CPU %u timer tick started (vector=%u)\n",
                   cpu->cpu_id, (unsigned)vector);
        }
    }

    lapic_write_reg(&cpu->lapic, LAPIC_REG_EOI, 0);
}



bool x86_lapic_timer_start(uint8_t vector, uint32_t initial_count, bool periodic) {
    cpu_t *cpu = cpu_self();
    lapic_info_t *l = &cpu->lapic;

    if (!l->initialized)    return false;
    if (vector < 32U)       return false;
    if (initial_count == 0) return false;

    lapic_write_reg(l, LAPIC_REG_TIMER_DIVCONF, LAPIC_TIMER_DIVIDE_BY_16);

    uint32_t lvt = vector;
    if (periodic) lvt |= LAPIC_TIMER_PERIODIC;
    lapic_write_reg(l, LAPIC_REG_LVT_TIMER, lvt);
    lapic_write_reg(l, LAPIC_REG_TIMER_INITCNT, initial_count);

    l->timer_vector        = vector;
    l->timer_initial_count = initial_count;
    l->timer_periodic      = periodic;
    return true;
}

void x86_lapic_timer_stop(void) {
    cpu_t *cpu = cpu_self();
    if (!cpu->lapic.initialized) return;

    uint32_t lvt = lapic_read_reg(&cpu->lapic, LAPIC_REG_LVT_TIMER);
    lapic_write_reg(&cpu->lapic, LAPIC_REG_LVT_TIMER, lvt | LAPIC_LVT_MASKED);
    lapic_write_reg(&cpu->lapic, LAPIC_REG_TIMER_INITCNT, 0);
}

uint64_t x86_lapic_timer_ticks(void) {
    return cpu_self()->lapic_timer_ticks;
}



void x86_lapic_send_ipi(uint32_t target_apic_id, uint32_t icr_low) {
    cpu_t *cpu = cpu_self();
    lapic_info_t *l = &cpu->lapic;

    if (l->x2apic_enabled) {
        // x2APIC: single 64-bit MSR write; high 32 bits = destination.
        uint64_t icr = ((uint64_t)target_apic_id << 32) | icr_low;
        wrmsr_u64(0x830U, icr);
    } else {
        // xAPIC: write destination (high) before command (low) — ordering matters.
        lapic_wait_icr_idle(l);
        lapic_write_reg(l, LAPIC_REG_ICR_HIGH, target_apic_id << 24);
        lapic_write_reg(l, LAPIC_REG_ICR_LOW,  icr_low);
        lapic_wait_icr_idle(l);
    }
}

void x86_lapic_send_init(uint32_t target_apic_id) {
    x86_lapic_send_ipi(target_apic_id,
                       LAPIC_ICR_DELIVERY_INIT | LAPIC_ICR_LEVEL_ASSERT);
}

void x86_lapic_send_sipi(uint32_t target_apic_id, uint8_t trampoline_page) {
    x86_lapic_send_ipi(target_apic_id,
                       LAPIC_ICR_DELIVERY_SIPI | LAPIC_ICR_LEVEL_ASSERT
                       | trampoline_page);
}



const char *x86_cpu_vendor_name(cpu_vendor_t vendor) {
    switch (vendor) {
        case CPU_VENDOR_INTEL:  return "Intel";
        case CPU_VENDOR_AMD:    return "AMD";
        case CPU_VENDOR_KVM:    return "KVM";
        case CPU_VENDOR_HYPERV: return "Hyper-V";
        case CPU_VENDOR_VMWARE: return "VMware";
        default:                return "Unknown";
    }
}