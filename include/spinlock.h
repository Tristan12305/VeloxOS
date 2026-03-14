#pragma once
#include <stddef.h>
#include <stdint.h>


typedef struct{ 
    volatile uint32_t lock;
} spinlock_t;


extern void spin_lock(spinlock_t *);
extern void spin_unlock(spinlock_t *);