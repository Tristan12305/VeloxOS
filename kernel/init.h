#pragma once

void arch_early_init(void);

void mem_init(void);

void arch_cpu_init(void);

void arch_acpi_init(void);

void smp_init(void);

void arch_irq_init(void);

void idle(void) __attribute__((noreturn));