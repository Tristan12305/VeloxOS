#include "acpi.h"
#include <boot/boot.h>
#include <include/printk.h>
#include <stddef.h>
#include <kernel/panic.h>



static acpi_xsdt_t *xsdt = NULL;
static size_t        xsdt_entry_count = 0;

static int acpi_sig4_equal(const char a[4], const char b[4]) {
    return (a[0] == b[0]) &&
           (a[1] == b[1]) &&
           (a[2] == b[2]) &&
           (a[3] == b[3]);
}


// Validate an SDT checksum: byte-sum of the entire table must be 0.
static int sdt_checksum_valid(const acpi_sdt_header_t *hdr) {
    const uint8_t *p   = (const uint8_t *)hdr;
    uint8_t        sum = 0;
    for (uint32_t i = 0; i < hdr->length; i++)
        sum += p[i];
    return sum == 0;
}



void acpi_init(void) {
    if (g_rsdp_address == 0) {
        panic("acpi: limine did not provide an RSDP response");
    }

    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)(uintptr_t)g_rsdp_address;

    if (rsdp->revision < 2 || rsdp->xsdt_address == 0) {
        // RSDT fallback is intentionally not implemented; Limine always
        // provides an XSDT on x86-64 UEFI systems.
        printk("acpi: no XSDT available (RSDP revision %u)\n", rsdp->revision);
        panic("acpi: no XSDT available");
    }

    xsdt = (acpi_xsdt_t *)(uintptr_t)rsdp->xsdt_address;

    if (!acpi_sig4_equal(xsdt->header.signature, "XSDT")) {
        panic("acpi: XSDT signature mismatch");
        xsdt = NULL;
        return;
    }

    if (!sdt_checksum_valid(&xsdt->header)) {
        printk("acpi: XSDT checksum invalid\n");
        xsdt = NULL;
        return;
    }

    xsdt_entry_count =
        (xsdt->header.length - sizeof(acpi_sdt_header_t)) / sizeof(uint64_t);

    printk("acpi: XSDT at %p, %zu entries\n", (void *)xsdt, xsdt_entry_count);
}

acpi_sdt_header_t *acpi_find_table(const char sig[4]) {
    if (!xsdt) return NULL;

    for (size_t i = 0; i < xsdt_entry_count; i++) {
        acpi_sdt_header_t *hdr =
            (acpi_sdt_header_t *)(uintptr_t)xsdt->entries[i];

        if (!acpi_sig4_equal(hdr->signature, sig))
            continue;

        if (!sdt_checksum_valid(hdr)) {
            printk("acpi: table '%.4s' has invalid checksum, skipping\n", sig);
            continue;
        }

        return hdr;
    }

    return NULL;
}
