#include <kernel/init.h>
#include <arch/x86_64/cpu/acpi.h>
#include <arch/x86_64/cpu/madt.h>
#include <arch/x86_64/drivers/block/pci.h>

void arch_acpi_init(void) {
    acpi_init();
    madt_init();
    pci_enumerate();
}