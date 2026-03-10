#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <arch/x86_64/irq.h>


#ifndef VXKERNEL
#if defined(VXKERNEL_DYNAMIC)
#define VXKERNEL __attribute__((visibility("default")))
#else
#define VXKERNEL
#endif
#endif

typedef irq_handler_t drivers_irq_handler_t;

VXKERNEL bool drivers_irq_register(uint8_t vector, drivers_irq_handler_t handler);
VXKERNEL void drivers_irq_unregister(uint8_t vector);
VXKERNEL void drivers_irq_free(uint8_t vector);

/* Route a legacy ISA IRQ (0-15) to an interrupt vector via the IOAPIC. */
VXKERNEL bool drivers_route_isa_irq(uint8_t irq, uint8_t vector, bool masked);

/* Convenience: register handler and route ISA IRQ in one call. */
VXKERNEL bool drivers_request_isa_irq(uint8_t irq,
                                     uint8_t vector,
                                     drivers_irq_handler_t handler,
                                     bool masked);
