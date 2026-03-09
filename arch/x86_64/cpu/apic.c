#include "apic.h"

#include <include/printk.h>

#include <cpuid.h>
#include <mm/paging.h>
#include <mm/pmm.h>

#define IA32_APIC_BASE_MSR 0x1BU

#define PIC1_DATA_PORT 0x21U
#define PIC2_DATA_PORT 0xA1U

#define APIC_BASE_BSP_BIT            (1ULL << 8)
#define APIC_BASE_X2APIC_ENABLE_BIT  (1ULL << 10)
#define APIC_BASE_GLOBAL_ENABLE_BIT  (1ULL << 11)
#define APIC_BASE_ADDR_MASK          0x000FFFFFFFFFF000ULL

#define LAPIC_REG_ID        0x020U
#define LAPIC_REG_VERSION   0x030U
#define LAPIC_REG_TPR       0x080U
#define LAPIC_REG_EOI       0x0B0U
#define LAPIC_REG_SVR       0x0F0U
#define LAPIC_REG_ESR       0x280U
#define LAPIC_REG_LVT_CMCI  0x2F0U
#define LAPIC_REG_LVT_TIMER 0x320U
#define LAPIC_REG_LVT_THERM 0x330U
#define LAPIC_REG_LVT_PERF  0x340U
#define LAPIC_REG_LVT_LINT0 0x350U
#define LAPIC_REG_LVT_LINT1 0x360U
#define LAPIC_REG_LVT_ERROR 0x370U
#define LAPIC_REG_TIMER_INITCNT 0x380U
#define LAPIC_REG_TIMER_CURRCNT 0x390U
#define LAPIC_REG_TIMER_DIVCONF 0x3E0U

#define LAPIC_SVR_SOFT_ENABLE (1U << 8)
#define LAPIC_LVT_MASKED      (1U << 16)
#define LAPIC_TIMER_PERIODIC  (1U << 17)

#define LAPIC_TIMER_DIVIDE_BY_16 0x3U

#define LAPIC_SPURIOUS_VECTOR 0xFFU
#define LAPIC_TIMER_VECTOR_DEFAULT 0x20U
#define LAPIC_TIMER_INIT_COUNT_DEFAULT 1000000U

static lapic_info_t g_lapic_info;
static bool g_lapic_ready;
static volatile uint64_t g_lapic_timer_ticks;
static bool g_lapic_timer_first_tick_printed;

static void lapic_info_reset(void) {
    g_lapic_info.initialized = false;
    g_lapic_info.vendor = CPU_VENDOR_UNKNOWN;
    g_lapic_info.bsp = false;
    g_lapic_info.x2apic_supported = false;
    g_lapic_info.x2apic_enabled = false;
    g_lapic_info.apic_base_msr_raw = 0;
    g_lapic_info.apic_base_phys = 0;
    g_lapic_info.apic_base_virt = 0;
    g_lapic_info.apic_id = 0;
    g_lapic_info.apic_version = 0;
    g_lapic_info.max_lvt_entries = 0;
    g_lapic_info.spurious_vector = 0;
    g_lapic_info.timer_vector = 0;
    g_lapic_info.timer_initial_count = 0;
    g_lapic_info.timer_periodic = false;
}

static inline void outb_u8(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static void pic_disable_legacy_8259(void) {
    outb_u8(PIC1_DATA_PORT, 0xFFU);
    outb_u8(PIC2_DATA_PORT, 0xFFU);
}

static inline uint64_t rdmsr_u64(uint32_t msr) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr_u64(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

static cpu_vendor_t detect_vendor(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __cpuid(0, eax, ebx, ecx, edx);

    if (ebx == 0x756E6547U && edx == 0x49656E69U && ecx == 0x6C65746EU) {
        return CPU_VENDOR_INTEL;
    }
    if (ebx == 0x68747541U && edx == 0x69746E65U && ecx == 0x444D4163U) {
        return CPU_VENDOR_AMD;
    }

    return CPU_VENDOR_UNKNOWN;
}

const char* x86_cpu_vendor_name(cpu_vendor_t vendor) {
    switch (vendor) {
        case CPU_VENDOR_INTEL:
            return "Intel";
        case CPU_VENDOR_AMD:
            return "AMD";
        case CPU_VENDOR_KVM:
            return "KVM";
        case CPU_VENDOR_HYPERV:
            return "Hyper-V";
        case CPU_VENDOR_VMWARE:
            return "VMware";
        default:
            return "Unknown";
    }
}

static uint32_t lapic_read_reg(uint32_t reg) {
    if (g_lapic_info.x2apic_enabled) {
        return (uint32_t)rdmsr_u64(0x800U + (reg >> 4));
    }

    volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)g_lapic_info.apic_base_virt;
    return mmio[reg >> 2];
}

static void lapic_write_reg(uint32_t reg, uint32_t value) {
    if (g_lapic_info.x2apic_enabled) {
        wrmsr_u64(0x800U + (reg >> 4), value);
        return;
    }

    volatile uint32_t* mmio = (volatile uint32_t*)(uintptr_t)g_lapic_info.apic_base_virt;
    mmio[reg >> 2] = value;
    (void)mmio[LAPIC_REG_ID >> 2];
}

static void lapic_mask_lvt(uint32_t reg) {
    uint32_t value = lapic_read_reg(reg);
    value |= LAPIC_LVT_MASKED;
    lapic_write_reg(reg, value);
}

uint32_t x86_lapic_id(void) {
    if (!g_lapic_ready) {
        return 0;
    }
    return g_lapic_info.apic_id;
}

void x86_lapic_eoi(void) {
    if (!g_lapic_ready) {
        return;
    }
    lapic_write_reg(LAPIC_REG_EOI, 0);
}

bool x86_lapic_timer_start(uint8_t vector, uint32_t initial_count, bool periodic) {
    if (!g_lapic_ready) {
        return false;
    }
    if (vector < 32U || initial_count == 0U) {
        return false;
    }

    lapic_write_reg(LAPIC_REG_TIMER_DIVCONF, LAPIC_TIMER_DIVIDE_BY_16);

    uint32_t lvt = vector;
    if (periodic) {
        lvt |= LAPIC_TIMER_PERIODIC;
    }
    lapic_write_reg(LAPIC_REG_LVT_TIMER, lvt);
    lapic_write_reg(LAPIC_REG_TIMER_INITCNT, initial_count);

    g_lapic_info.timer_vector = vector;
    g_lapic_info.timer_initial_count = initial_count;
    g_lapic_info.timer_periodic = periodic;
    return true;
}

void x86_lapic_timer_stop(void) {
    if (!g_lapic_ready) {
        return;
    }

    uint32_t lvt = lapic_read_reg(LAPIC_REG_LVT_TIMER);
    lvt |= LAPIC_LVT_MASKED;
    lapic_write_reg(LAPIC_REG_LVT_TIMER, lvt);
    lapic_write_reg(LAPIC_REG_TIMER_INITCNT, 0);
}

uint64_t x86_lapic_timer_ticks(void) {
    return g_lapic_timer_ticks;
}

void x86_lapic_handle_irq(uint8_t vector) {
    if (!g_lapic_ready) {
        return;
    }

    if (vector == g_lapic_info.timer_vector && g_lapic_info.timer_vector != 0) {
        g_lapic_timer_ticks++;
        if (!g_lapic_timer_first_tick_printed) {
            g_lapic_timer_first_tick_printed = true;
            printk("[lapic] timer tick started (vector=%u)\n", (unsigned)vector);
        }
    }

    x86_lapic_eoi();
}

bool x86_lapic_init(void) {
    if (g_lapic_ready) {
        return true;
    }
    if (!paging_ready()) {
        printk("[lapic] paging is not ready\n");
        return false;
    }

    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    __cpuid(1, eax, ebx, ecx, edx);

    bool has_apic = ((edx >> 9) & 1U) != 0;
    bool has_msr = ((edx >> 5) & 1U) != 0;
    bool has_x2apic = ((ecx >> 21) & 1U) != 0;

    if (!has_msr || !has_apic) {
        printk("[lapic] unsupported CPU features (msr=%u apic=%u)\n",
               has_msr ? 1U : 0U,
               has_apic ? 1U : 0U);
        return false;
    }

    uint64_t apic_base = rdmsr_u64(IA32_APIC_BASE_MSR);
    apic_base |= APIC_BASE_GLOBAL_ENABLE_BIT;
    wrmsr_u64(IA32_APIC_BASE_MSR, apic_base);
    apic_base = rdmsr_u64(IA32_APIC_BASE_MSR);

    if ((apic_base & APIC_BASE_X2APIC_ENABLE_BIT) != 0 && has_x2apic) {
        uint64_t try_xapic = apic_base & ~APIC_BASE_X2APIC_ENABLE_BIT;
        wrmsr_u64(IA32_APIC_BASE_MSR, try_xapic);
        apic_base = rdmsr_u64(IA32_APIC_BASE_MSR);
    }

    lapic_info_reset();
    g_lapic_info.vendor = detect_vendor();
    g_lapic_info.x2apic_supported = has_x2apic;
    g_lapic_info.x2apic_enabled = (apic_base & APIC_BASE_X2APIC_ENABLE_BIT) != 0;
    g_lapic_info.bsp = (apic_base & APIC_BASE_BSP_BIT) != 0;
    g_lapic_info.apic_base_msr_raw = apic_base;
    g_lapic_info.apic_base_phys = apic_base & APIC_BASE_ADDR_MASK;
    g_lapic_info.apic_base_virt = g_lapic_info.x2apic_enabled
        ? 0
        : pmm_phys_to_virt(g_lapic_info.apic_base_phys);
    g_lapic_info.spurious_vector = LAPIC_SPURIOUS_VECTOR;
    g_lapic_timer_ticks = 0;
    g_lapic_timer_first_tick_printed = false;

    pic_disable_legacy_8259();
    printk("[lapic] legacy PIC masked\n");

    uint32_t id_reg = lapic_read_reg(LAPIC_REG_ID);
    uint32_t ver_reg = lapic_read_reg(LAPIC_REG_VERSION);

    g_lapic_info.apic_id = g_lapic_info.x2apic_enabled ? id_reg : (id_reg >> 24);
    g_lapic_info.apic_version = ver_reg & 0xFFU;
    g_lapic_info.max_lvt_entries = ((ver_reg >> 16) & 0xFFU) + 1U;

    lapic_write_reg(LAPIC_REG_TPR, 0);

    if (g_lapic_info.max_lvt_entries >= 1U) lapic_mask_lvt(LAPIC_REG_LVT_TIMER);
    if (g_lapic_info.max_lvt_entries >= 2U) lapic_mask_lvt(LAPIC_REG_LVT_THERM);
    if (g_lapic_info.max_lvt_entries >= 3U) lapic_mask_lvt(LAPIC_REG_LVT_PERF);
    if (g_lapic_info.max_lvt_entries >= 4U) lapic_mask_lvt(LAPIC_REG_LVT_LINT0);
    if (g_lapic_info.max_lvt_entries >= 5U) lapic_mask_lvt(LAPIC_REG_LVT_LINT1);
    if (g_lapic_info.max_lvt_entries >= 6U) lapic_mask_lvt(LAPIC_REG_LVT_ERROR);
    if (g_lapic_info.max_lvt_entries >= 7U) lapic_mask_lvt(LAPIC_REG_LVT_CMCI);

    if (!g_lapic_info.x2apic_enabled) {
        lapic_write_reg(LAPIC_REG_ESR, 0);
        lapic_write_reg(LAPIC_REG_ESR, 0);
    }

    uint32_t svr = lapic_read_reg(LAPIC_REG_SVR);
    svr &= ~0xFFU;
    svr |= g_lapic_info.spurious_vector;
    svr |= LAPIC_SVR_SOFT_ENABLE;
    lapic_write_reg(LAPIC_REG_SVR, svr);

    x86_lapic_eoi();

    g_lapic_info.initialized = true;
    g_lapic_ready = true;

    if (!x86_lapic_timer_start(LAPIC_TIMER_VECTOR_DEFAULT,
                               LAPIC_TIMER_INIT_COUNT_DEFAULT,
                               true)) {
        printk("[lapic] timer setup failed\n");
        g_lapic_info.initialized = false;
        g_lapic_ready = false;
        return false;
    }

    printk("[lapic] ready: vendor=%s mode=%s id=%u ver=0x%x lvt=%u base_phys=0x%llx timer_vec=%u\n",
           x86_cpu_vendor_name(g_lapic_info.vendor),
           g_lapic_info.x2apic_enabled ? "x2apic" : "xapic",
           g_lapic_info.apic_id,
           g_lapic_info.apic_version,
           g_lapic_info.max_lvt_entries,
           (unsigned long long)g_lapic_info.apic_base_phys,
           (unsigned)g_lapic_info.timer_vector);

    return true;
}

bool x86_lapic_ready(void) {
    return g_lapic_ready;
}

const lapic_info_t* x86_lapic_get_info(void) {
    return &g_lapic_info;
}
