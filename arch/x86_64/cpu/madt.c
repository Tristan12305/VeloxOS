#include "madt.h"
#include <include/printk.h>
#include <string.h>

// Global state


madt_lapic_info_t  g_lapics[MADT_MAX_LAPICS];
size_t        g_lapic_count = 0;

madt_ioapic_info_t g_ioapics[MADT_MAX_IOAPICS];
size_t        g_ioapic_count = 0;

madt_iso_info_t    g_isos[MADT_MAX_ISOS];
size_t        g_iso_count = 0;

uint32_t      g_lapic_address = 0;


void madt_init(void) {
    madt_t *madt = (madt_t *)acpi_find_table("APIC");
    if (!madt) {
        printk("madt: table not found\n");
        return;
    }

    g_lapic_address = madt->local_apic_address;
    printk("madt: LAPIC base = 0x%08x, flags = 0x%08x\n",
           g_lapic_address, madt->flags);

    // Walk the variable-length record list that follows the fixed header.
    uint8_t *base = (uint8_t *)madt;
    uint8_t *ptr  = base + sizeof(madt_t);
    uint8_t *end  = base + madt->header.length;

    while (ptr < end) {
        madt_record_header_t *rec = (madt_record_header_t *)ptr;

        // a record must be at least 2 bytes long.
        if (rec->length < 2 || ptr + rec->length > end)
            break;

        switch (rec->type) {
        case MADT_TYPE_LAPIC: {
            if (rec->length < sizeof(madt_lapic_t)) break;
            madt_lapic_t *e = (madt_lapic_t *)rec;

            // Only count processors that are actually usable.
            if (!(e->flags & MADT_LAPIC_ENABLED) &&
                !(e->flags & MADT_LAPIC_ONLINE_CAPABLE))
                break;

            if (g_lapic_count >= MADT_MAX_LAPICS) {
                printk("madt: LAPIC table full, ignoring entry\n");
                break;
            }

            g_lapics[g_lapic_count++] = (madt_lapic_info_t){
                .apic_id  = e->apic_id,
                .acpi_uid = e->acpi_processor_uid,
                .flags    = e->flags,
            };

            printk("madt: LAPIC  apic_id=%u uid=%u flags=0x%x\n",
                   e->apic_id, e->acpi_processor_uid, e->flags);
            break;
        }

        case MADT_TYPE_IOAPIC: {
            if (rec->length < sizeof(madt_ioapic_t)) break;
            madt_ioapic_t *e = (madt_ioapic_t *)rec;

            if (g_ioapic_count >= MADT_MAX_IOAPICS) {
                printk("madt: I/O APIC table full, ignoring entry\n");
                break;
            }

            g_ioapics[g_ioapic_count++] = (madt_ioapic_info_t){
                .id       = e->ioapic_id,
                .address  = e->ioapic_address,
                .gsi_base = e->gsi_base,
            };

            printk("madt: IOAPIC id=%u base=0x%08x gsi_base=%u\n",
                   e->ioapic_id, e->ioapic_address, e->gsi_base);
            break;
        }

        case MADT_TYPE_ISO: {
            if (rec->length < sizeof(madt_iso_t)) break;
            madt_iso_t *e = (madt_iso_t *)rec;

            if (g_iso_count >= MADT_MAX_ISOS) {
                printk("madt: ISO table full, ignoring entry\n");
                break;
            }

            g_isos[g_iso_count++] = (madt_iso_info_t){
                .irq      = e->irq,
                .gsi      = e->gsi,
                .polarity = ISO_POLARITY(e->flags),
                .trigger  = ISO_TRIGGER(e->flags),
            };

            printk("madt: ISO    irq=%u -> gsi=%u polarity=%u trigger=%u\n",
                   e->irq, e->gsi,
                   ISO_POLARITY(e->flags), ISO_TRIGGER(e->flags));
            break;
        }

        default:
            break;
        }

        ptr += rec->length;
    }

    printk("madt: %zu LAPIC(s), %zu I/O APIC(s), %zu ISO(s)\n",
           g_lapic_count, g_ioapic_count, g_iso_count);
}
