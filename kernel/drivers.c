#include <include/drivers.h>

#include <arch/x86_64/irq.h>
#include <arch/x86_64/cpu/ioapic.h>

bool drivers_irq_register(uint8_t vector, drivers_irq_handler_t handler) {
    return irq_register_handler(vector, handler);
}

void drivers_irq_unregister(uint8_t vector) {
    irq_unregister_handler(vector);
}

void drivers_irq_free(uint8_t vector) {
    drivers_irq_unregister(vector);
}

bool drivers_route_isa_irq(uint8_t irq, uint8_t vector, bool masked) {
    return x86_ioapic_route_isa_irq(irq, vector, masked);
}

bool drivers_request_isa_irq(uint8_t irq,
                             uint8_t vector,
                             drivers_irq_handler_t handler,
                             bool masked) {
    if (!drivers_irq_register(vector, handler)) {
        return false;
    }

    if (!drivers_route_isa_irq(irq, vector, masked)) {
        drivers_irq_unregister(vector);
        return false;
    }

    return true;
}
