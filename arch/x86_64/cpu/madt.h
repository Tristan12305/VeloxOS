#pragma once

#include "acpi.h"
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// MADT table layout
// ---------------------------------------------------------------------------

typedef struct __attribute__((packed)) {
    acpi_sdt_header_t header;
    uint32_t          local_apic_address;  // default LAPIC MMIO base
    uint32_t          flags;               // bit 0: legacy 8259 PIC present
} madt_t;

// Every record inside the MADT starts with these two bytes.
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} madt_record_header_t;

// Entry types we care about.
#define MADT_TYPE_LAPIC       0
#define MADT_TYPE_IOAPIC      1
#define MADT_TYPE_ISO         2   // Interrupt Source Override
#define MADT_TYPE_LAPIC_NMI   4

// Type 0 — Processor Local APIC
typedef struct __attribute__((packed)) {
    madt_record_header_t header;   // type = 0, length = 8
    uint8_t              acpi_processor_uid;
    uint8_t              apic_id;
    uint32_t             flags;    // bit 0: enabled; bit 1: online-capable
} madt_lapic_t;

#define MADT_LAPIC_ENABLED        (1u << 0)
#define MADT_LAPIC_ONLINE_CAPABLE (1u << 1)

// Type 1 — I/O APIC
typedef struct __attribute__((packed)) {
    madt_record_header_t header;   // type = 1, length = 12
    uint8_t              ioapic_id;
    uint8_t              reserved;
    uint32_t             ioapic_address;   // MMIO base (physical)
    uint32_t             gsi_base;         // global system interrupt base
} madt_ioapic_t;

// Type 2 — Interrupt Source Override

typedef struct __attribute__((packed)) {
    madt_record_header_t header;   // type = 2, length = 10
    uint8_t              bus;      // 0 = ISA
    uint8_t              irq;      // source IRQ (ISA bus number)
    uint32_t             gsi;      // target global system interrupt
    uint16_t             flags;    // polarity [1:0], trigger mode [3:2]
} madt_iso_t;


#define ISO_POLARITY(f)     ((f) & 0x3)
#define ISO_TRIGGER(f)      (((f) >> 2) & 0x3)
// 0 = bus default, 1 = active-high, 3 = active-low
// 0 = bus default, 1 = edge,        3 = level



#define MADT_MAX_LAPICS   256
#define MADT_MAX_IOAPICS  8
#define MADT_MAX_ISOS     24

typedef struct {
    uint8_t  apic_id;
    uint8_t  acpi_uid;
    uint32_t flags;
} madt_lapic_info_t;

typedef struct {
    uint8_t  id;
    uint32_t address;   // physical MMIO base
    uint32_t gsi_base;
} madt_ioapic_info_t;

typedef struct {
    uint8_t  irq;      // ISA IRQ source
    uint32_t gsi;      // mapped global system interrupt
    uint8_t  polarity; // ISO_POLARITY(flags)
    uint8_t  trigger;  // ISO_TRIGGER(flags)
} madt_iso_info_t;

extern madt_lapic_info_t  g_lapics[MADT_MAX_LAPICS];
extern size_t        g_lapic_count;

extern madt_ioapic_info_t g_ioapics[MADT_MAX_IOAPICS];
extern size_t        g_ioapic_count;

extern madt_iso_info_t    g_isos[MADT_MAX_ISOS];
extern size_t        g_iso_count;

// Physical MMIO address from the MADT header (may be overridden by a
// type-5 LAPIC address override record, but that's rare on modern hw).
extern uint32_t g_lapic_address;

// Parses the MADT and populates the globals above.
// Must be called after acpi_init().
void madt_init(void);
