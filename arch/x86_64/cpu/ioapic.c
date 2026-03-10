#include "ioapic.h"

#include "apic.h"
#include "madt.h"

#include <include/printk.h>
#include <mm/paging.h>
#include <mm/pmm.h>

#include <stddef.h>

#define IOAPIC_REG_ID   0x00U
#define IOAPIC_REG_VER  0x01U

#define IOAPIC_REDIR_BASE 0x10U

#define IOAPIC_REDIR_MASKED   (1ULL << 16)
#define IOAPIC_REDIR_POLARITY (1ULL << 13)
#define IOAPIC_REDIR_TRIGGER  (1ULL << 15)

typedef struct {
    uint32_t id;
    uint32_t gsi_base;
    uint32_t max_redirs;
    uint64_t phys_base;
    volatile uint32_t *regsel;
    volatile uint32_t *window;
} ioapic_desc_t;

static ioapic_desc_t g_ioapic_desc[MADT_MAX_IOAPICS];
static size_t g_ioapic_desc_count;
static bool g_ioapic_ready;

static inline uint32_t ioapic_read(ioapic_desc_t *io, uint8_t reg) {
    *io->regsel = reg;
    return *io->window;
}

static inline void ioapic_write(ioapic_desc_t *io, uint8_t reg, uint32_t value) {
    *io->regsel = reg;
    *io->window = value;
}

static void ioapic_write_redir(ioapic_desc_t *io, uint32_t index, uint64_t entry) {
    uint8_t lo = (uint8_t)(IOAPIC_REDIR_BASE + (index * 2));
    ioapic_write(io, lo, (uint32_t)(entry & 0xFFFFFFFFU));
    ioapic_write(io, (uint8_t)(lo + 1), (uint32_t)(entry >> 32));
}

static uint64_t ioapic_read_redir(ioapic_desc_t *io, uint32_t index) {
    uint8_t lo = (uint8_t)(IOAPIC_REDIR_BASE + (index * 2));
    uint64_t low = ioapic_read(io, lo);
    uint64_t high = ioapic_read(io, (uint8_t)(lo + 1));
    return low | (high << 32);
}

static ioapic_desc_t *ioapic_for_gsi(uint32_t gsi, uint32_t *out_index) {
    for (size_t i = 0; i < g_ioapic_desc_count; i++) {
        ioapic_desc_t *io = &g_ioapic_desc[i];
        uint32_t max_gsi = io->gsi_base + io->max_redirs - 1;
        if (gsi >= io->gsi_base && gsi <= max_gsi) {
            if (out_index) {
                *out_index = gsi - io->gsi_base;
            }
            return io;
        }
    }
    return NULL;
}

static const madt_iso_info_t *find_iso_for_irq(uint8_t irq) {
    for (size_t i = 0; i < g_iso_count; i++) {
        if (g_isos[i].irq == irq) {
            return &g_isos[i];
        }
    }
    return NULL;
}

bool x86_ioapic_init(void) {
    if (g_ioapic_ready) {
        return true;
    }
    if (!paging_ready()) {
        printk("[ioapic] paging is not ready\n");
        return false;
    }
    if (g_ioapic_count == 0) {
        printk("[ioapic] no IOAPICs found in MADT\n");
        return false;
    }

    g_ioapic_desc_count = 0;

    for (size_t i = 0; i < g_ioapic_count && i < MADT_MAX_IOAPICS; i++) {
        ioapic_desc_t *io = &g_ioapic_desc[g_ioapic_desc_count++];
        io->id = g_ioapics[i].id;
        io->gsi_base = g_ioapics[i].gsi_base;
        io->phys_base = g_ioapics[i].address;

        uint64_t virt = pmm_phys_to_virt(io->phys_base);
        io->regsel = (volatile uint32_t *)(uintptr_t)virt;
        io->window = (volatile uint32_t *)(uintptr_t)(virt + 0x10);

        uint32_t ver = ioapic_read(io, IOAPIC_REG_VER);
        io->max_redirs = ((ver >> 16) & 0xFFU) + 1U;

        printk("[ioapic] id=%u base=0x%08llx gsi_base=%u redirs=%u\n",
               io->id,
               (unsigned long long)io->phys_base,
               io->gsi_base,
               io->max_redirs);

        // Mask all redirection entries by default.
        for (uint32_t r = 0; r < io->max_redirs; r++) {
            uint64_t entry = IOAPIC_REDIR_MASKED | 0x20U; // vector placeholder
            ioapic_write_redir(io, r, entry);
        }
    }

    g_ioapic_ready = true;
    return true;
}

bool x86_ioapic_ready(void) {
    return g_ioapic_ready;
}

bool x86_ioapic_route_isa_irq(uint8_t irq, uint8_t vector, bool masked) {
    if (!g_ioapic_ready) {
        return false;
    }
    if (vector < 32U) {
        return false;
    }

    uint32_t gsi = irq;
    uint8_t polarity = 0; // conforming
    uint8_t trigger = 0;  // conforming

    const madt_iso_info_t *iso = find_iso_for_irq(irq);
    if (iso) {
        gsi = iso->gsi;
        polarity = iso->polarity;
        trigger = iso->trigger;
    }

    uint32_t index = 0;
    ioapic_desc_t *io = ioapic_for_gsi(gsi, &index);
    if (!io) {
        printk("[ioapic] no IOAPIC for GSI %u\n", gsi);
        return false;
    }

    uint64_t entry = vector;

    // Polarity
    if (polarity == 3) { // active low
        entry |= IOAPIC_REDIR_POLARITY;
    }

    // Trigger mode
    if (trigger == 3) { // level
        entry |= IOAPIC_REDIR_TRIGGER;
    }

    if (masked) {
        entry |= IOAPIC_REDIR_MASKED;
    }

    uint32_t dest = x86_lapic_id();
    entry |= ((uint64_t)dest) << 56;

    ioapic_write_redir(io, index, entry);

    printk("[ioapic] IRQ%u -> GSI%u vector=0x%02x dest=%u\n",
           (unsigned)irq, gsi, vector, dest);

    return true;
}

bool x86_ioapic_mask_gsi(uint32_t gsi, bool masked) {
    if (!g_ioapic_ready) {
        return false;
    }

    uint32_t index = 0;
    ioapic_desc_t *io = ioapic_for_gsi(gsi, &index);
    if (!io) {
        return false;
    }

    uint64_t entry = ioapic_read_redir(io, index);
    if (masked) {
        entry |= IOAPIC_REDIR_MASKED;
    } else {
        entry &= ~IOAPIC_REDIR_MASKED;
    }
    ioapic_write_redir(io, index, entry);
    return true;
}
