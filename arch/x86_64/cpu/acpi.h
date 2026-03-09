#pragma once

#include <stdint.h>

// All ACPI tables begin with this header.
typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

// RSDP (Root System Description Pointer) — v2 superset of v1.
typedef struct __attribute__((packed)) {
    char     signature[8];   // "RSD PTR "
    uint8_t  checksum;       // covers first 20 bytes
    char     oem_id[6];
    uint8_t  revision;       // 0 = v1, 2 = v2+
    uint32_t rsdt_address;   // physical, 32-bit

    // v2 fields (revision >= 2)
    uint32_t length;
    uint64_t xsdt_address;   // physical, 64-bit
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

// XSDT — header followed by n × 64-bit table physical addresses.
typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint64_t          entries[];
} acpi_xsdt_t;

void acpi_init(void);


// `sig` (e.g. "APIC" for the MADT), or NULL if not found.
acpi_sdt_header_t *acpi_find_table(const char sig[4]);