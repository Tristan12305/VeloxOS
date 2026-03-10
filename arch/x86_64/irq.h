#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "isr.h"

typedef void (*irq_handler_t)(interrupt_frame *frame);

bool irq_register_handler(uint8_t vector, irq_handler_t handler);
void irq_unregister_handler(uint8_t vector);
void irq_dispatch(interrupt_frame *frame);
