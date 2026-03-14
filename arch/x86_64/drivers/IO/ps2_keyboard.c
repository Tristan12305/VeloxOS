#include "ps2_keyboard.h"

#include "arch/x86_64/irq.h"
#include "arch/x86_64/cpu/ioapic.h"
#include "include/printk.h"

#include <stdint.h>

#define PS2_KBD_VECTOR 0x21U

static inline uint8_t inb_u8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}


static void ps2_keyboard_irq(interrupt_frame *frame) {
    (void)frame;
    uint8_t scancode = inb_u8(0x60);
    static bool e0_prefix = false;

    if (scancode == 0xE0) {
        e0_prefix = true;
        return;
    }

    bool release = (scancode & 0x80U) != 0;
    uint8_t code = scancode & 0x7FU;
    
    printk("[kbd] %s scancode=0x", release ? "break" : "make");
    printk_hex8(code);
    if (e0_prefix) {
        printk(" (e0)");
    }
    printk("\n");

    e0_prefix = false;
}

bool ps2_keyboard_init(void) {
    if (!irq_register_handler(PS2_KBD_VECTOR, ps2_keyboard_irq)) {
        return false;
    }
    if (!x86_ioapic_route_isa_irq(1, PS2_KBD_VECTOR, false)) {
        return false;
    }

    printk("[kbd] IRQ1 routed to vector 0x");
    printk_hex8(PS2_KBD_VECTOR);
    printk("\n");
    return true;
}
