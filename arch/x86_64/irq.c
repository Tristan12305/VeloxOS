#include "irq.h"

#include <stddef.h>

static irq_handler_t g_irq_handlers[256];

bool irq_register_handler(uint8_t vector, irq_handler_t handler) {
    if (vector < 32U || handler == NULL) {
        return false;
    }
    g_irq_handlers[vector] = handler;
    return true;
}

void irq_unregister_handler(uint8_t vector) {
    if (vector < 32U) {
        return;
    }
    g_irq_handlers[vector] = NULL;
}

void irq_dispatch(interrupt_frame *frame) {
    if (!frame || frame->vector < 32U) {
        return;
    }

    irq_handler_t handler = g_irq_handlers[frame->vector];
    if (handler) {
        handler(frame);
    }
}
