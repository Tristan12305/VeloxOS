#include <arch/x86_64/cpu/percpu.h>
#include <arch/x86_64/cpu/apic.h>

// The actual storage for the per-CPU array and count.
// Every other file that includes percpu.h sees these as extern.
cpu_t    g_cpus[MAX_CPUS];
uint32_t g_cpu_count = 0;

cpu_t *cpu_self(void) {
    uint32_t my_apic_id = x86_lapic_id();
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (g_cpus[i].apic_id == my_apic_id) {
            return &g_cpus[i];
        }
    }
    // Fallback: called before g_cpu_count is set (early BSP init).
    // g_cpus[0] is always the BSP, so this is safe.
    return &g_cpus[0];
}