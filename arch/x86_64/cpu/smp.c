#include <boot/boot.h>
#include <include/printk.h>
#include <mm/vmalloc.h>
#include <string.h>

#include <arch/x86_64/gdt.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/cpu/apic.h>
#include <arch/x86_64/cpu/percpu.h>
#include <arch/x86_64/cpu/vendor.h>
#include <kernel/sched.h>

#define SMP_STACK_PAGES 4
#define SMP_STACK_SIZE  (SMP_STACK_PAGES * 0x1000ULL)

static void smp_ap_entry_main(cpu_t *cpu, struct LIMINE_MP(info) *info);

__attribute__((noreturn))
static void smp_ap_entry(struct LIMINE_MP(info) *info) {
    cpu_t *cpu = (cpu_t *)(uintptr_t)info->extra_argument;
    if (!cpu || cpu->kernel_stack_top == 0) {
        for (;;)
            __asm__ volatile("hlt");
    }

    __asm__ volatile(
        "mov %0, %%rsp\n"
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "jmp *%3\n"
        :: "r"(cpu->kernel_stack_top),
           "r"(cpu),
           "r"(info),
           "r"(smp_ap_entry_main)
        : "rdi", "rsi", "memory");

    __builtin_unreachable();
}

__attribute__((noreturn))
static void smp_ap_entry_main(cpu_t *cpu, struct LIMINE_MP(info) *info) {
    cpu->is_bsp = false;
    cpu->online = false;
    if (info) {
        cpu->apic_id = info->lapic_id;
    }

    cpuid_get_info(&cpu->cpu_info);

    x86_gdt_init_cpu(cpu, cpu->kernel_stack_top);
    idt_load();

    if (!x86_lapic_ap_init(cpu)) {
        printk("[smp] AP %u LAPIC init failed\n", cpu->cpu_id);
        for (;;)
            __asm__ volatile("pause");
    }

    cpu->online = true;

    while (!sched_is_ready()) {
        __asm__ volatile("pause");
    }

    sched_ap_init();
    __asm__ volatile("sti");

    for (;;)
        __asm__ volatile("hlt");
}

void smp_init(void) {
    if (!g_mp_response || !g_mp_response->cpus || g_mp_response->cpu_count == 0) {
        printk("[smp] Limine MP response missing; running as UP\n");
        g_cpu_count = 1;
        g_cpus[0].cpu_id = 0;
        g_cpus[0].is_bsp = true;
        g_cpus[0].online = true;
        return;
    }

    uint64_t reported = g_mp_response->cpu_count;
    uint32_t bsp_lapic = g_mp_response->bsp_lapic_id;

    g_cpus[0].cpu_id = 0;
    g_cpus[0].is_bsp = true;
    g_cpus[0].online = true;

    if (g_cpus[0].apic_id != 0 && g_cpus[0].apic_id != bsp_lapic) {
        printk("[smp] BSP LAPIC mismatch: lapic=%u limine=%u\n",
               g_cpus[0].apic_id, bsp_lapic);
    }

    uint32_t next_cpu = 1;
    uint32_t started = 0;

    for (uint64_t i = 0; i < reported; i++) {
        struct LIMINE_MP(info) *info = g_mp_response->cpus[i];
        if (!info) {
            continue;
        }
        if (info->lapic_id == bsp_lapic) {
            continue;
        }
        if (next_cpu >= MAX_CPUS) {
            printk("[smp] CPU limit reached (%u)\n", MAX_CPUS);
            break;
        }

        cpu_t *cpu = &g_cpus[next_cpu];
        memset(cpu, 0, sizeof(*cpu));
        cpu->cpu_id = next_cpu;
        cpu->apic_id = info->lapic_id;
        cpu->is_bsp = false;
        cpu->online = false;

        void *stack = vmalloc(SMP_STACK_SIZE, VMALLOC_DEFAULT_FLAGS);
        if (!stack) {
            printk("[smp] AP %u stack alloc failed\n", next_cpu);
            continue;
        }

        cpu->kernel_stack_top = (uintptr_t)stack + SMP_STACK_SIZE;
        info->extra_argument = (uint64_t)(uintptr_t)cpu;
        info->goto_address = smp_ap_entry;

        started++;
        next_cpu++;
    }

    g_cpu_count = next_cpu;

    for (uint32_t i = 1; i < g_cpu_count; i++) {
        cpu_t *cpu = &g_cpus[i];
        for (uint64_t spin = 0; !cpu->online && spin < 100000000ULL; spin++) {
            __asm__ volatile("pause");
        }
        if (!cpu->online) {
            printk("[smp] AP %u failed to come online\n", cpu->cpu_id);
        }
    }

    printk("[smp] BSP=%u APs started=%u total=%u\n",
           bsp_lapic,
           started,
           g_cpu_count);
}
