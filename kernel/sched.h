#pragma once

#include <stdint.h>
#include <arch/x86_64/isr.h>

#define SCHED_TICK_HZ 100U
#define SCHED_TICK_VECTOR 0x20U

typedef void (*sched_entry_t)(void *arg);

void sched_init(void);
void sched_ap_init(void);
int sched_spawn(sched_entry_t entry, void *arg);
interrupt_frame *sched_on_tick(interrupt_frame *frame);
int sched_is_ready(void);
