#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t keycode;   // Set 1 scancode without the release bit
    bool    e0_prefix;
    bool    pressed;   // true = down, false = up
} kbd_event_t;

typedef void (*kbd_event_handler_t)(kbd_event_t event, void *ctx);

typedef struct kbd_listener {
    kbd_event_handler_t   handler;
    void                 *ctx;
    int                   priority; // higher runs first
    struct kbd_listener  *next;
} kbd_listener_t;

static inline void kbd_listener_init(kbd_listener_t *listener,
                                     kbd_event_handler_t handler,
                                     void *ctx,
                                     int priority) {
    if (!listener) {
        return;
    }
    listener->handler  = handler;
    listener->ctx      = ctx;
    listener->priority = priority;
    listener->next     = NULL;
}

bool ps2_keyboard_init(void);

// Callbacks run in IRQ context with the keyboard lock held.
// Keep them short and do not call register/unregister from a callback.
bool kbd_register_listener(kbd_listener_t *listener);
void kbd_unregister_listener(kbd_listener_t *listener);
