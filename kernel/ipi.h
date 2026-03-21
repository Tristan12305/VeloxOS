#pragma once

#include <stdint.h>

// Dedicated IPI vectors (upper IDT range).
#define IPI_VECTOR_TLB      0xF0U
#define IPI_VECTOR_RESCHED  0xF1U
#define IPI_VECTOR_HALT     0xF2U

void ipi_init(void);
int  ipi_ready(void);

void ipi_send_resched(uint32_t cpu_id);
void ipi_broadcast_resched(void);

void ipi_send_tlb_shootdown(uint32_t cpu_id);
void ipi_broadcast_tlb_shootdown(void);
void ipi_tlb_shootdown(void);

void ipi_send_halt(uint32_t cpu_id);
void ipi_halt_others(void);
