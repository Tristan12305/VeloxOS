#pragma once
#include <stdint.h>

typedef struct {
    volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT { .lock = 0 }

/* should only be used inside ISRs */
extern void     spin_lock(spinlock_t *lock);
extern void     spin_unlock(spinlock_t *lock);

/* Use in normal kernel context — saves/restores interrupt state */
extern uint64_t spin_lock_irqsave(spinlock_t *lock);
extern void     spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags);