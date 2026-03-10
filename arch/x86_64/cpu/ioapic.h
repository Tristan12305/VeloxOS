#pragma once

#include <stdbool.h>
#include <stdint.h>

bool x86_ioapic_init(void);
bool x86_ioapic_ready(void);

// Route a legacy ISA IRQ (0-15) to the given vector.
bool x86_ioapic_route_isa_irq(uint8_t irq, uint8_t vector, bool masked);

// Mask/unmask a GSI if needed later.
bool x86_ioapic_mask_gsi(uint32_t gsi, bool masked);
