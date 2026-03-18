#include "ps2_keyboard.h"

#include "arch/x86_64/irq.h"
#include "arch/x86_64/cpu/ioapic.h"
#include "include/printk.h"
#include <include/spinlock.h>

#include <stdint.h>

#define PS2_KBD_VECTOR 0x21U

static spinlock_t     g_kbd_lock = SPINLOCK_INIT;
static kbd_listener_t *g_kbd_list = NULL;
static bool           g_e0_prefix = false;

static inline uint8_t inb_u8(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void ps2_keyboard_irq(interrupt_frame *frame) {
    (void)frame;

    spin_lock(&g_kbd_lock);
    uint8_t code = inb_u8(0x60);

    if (code == 0xE0) {
        g_e0_prefix = true;
        spin_unlock(&g_kbd_lock);
        return;
    }

    kbd_event_t event = {0};
    event.e0_prefix = g_e0_prefix;
    event.pressed   = (code & 0x80U) == 0;
    event.keycode   = code & 0x7FU;

    g_e0_prefix = false;

    for (kbd_listener_t *listener = g_kbd_list; listener; listener = listener->next) {
        if (listener->handler) {
            listener->handler(event, listener->ctx);
        }
    }
    spin_unlock(&g_kbd_lock);

}

bool ps2_keyboard_init(void) {
    if (!irq_register_handler(PS2_KBD_VECTOR, ps2_keyboard_irq)) {
        return false;
    }
    if (!x86_ioapic_route_isa_irq(1, PS2_KBD_VECTOR, false)) {
        return false;
    }

    safe_printk("[kbd] IRQ1 routed to vector 0x");
    printk_hex8(PS2_KBD_VECTOR);
    printk("\n");
    return true;
}

bool kbd_register_listener(kbd_listener_t *listener) {
    if (!listener || !listener->handler) {
        return false;
    }

    uint64_t flags = spin_lock_irqsave(&g_kbd_lock);
    kbd_listener_t **pp = &g_kbd_list;
    while (*pp) {
        if (*pp == listener) {
            spin_unlock_irqrestore(&g_kbd_lock, flags);
            return false;
        }
        if ((*pp)->priority < listener->priority) {
            break;
        }
        pp = &(*pp)->next;
    }

    listener->next = *pp;
    *pp = listener;
    spin_unlock_irqrestore(&g_kbd_lock, flags);
    return true;
}

void kbd_unregister_listener(kbd_listener_t *listener) {
    if (!listener) {
        return;
    }

    uint64_t flags = spin_lock_irqsave(&g_kbd_lock);
    kbd_listener_t **pp = &g_kbd_list;
    while (*pp) {
        if (*pp == listener) {
            *pp = listener->next;
            listener->next = NULL;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock_irqrestore(&g_kbd_lock, flags);
}
