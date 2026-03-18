#pragma once

#include <stdint.h>

/* CPU frame when interrupt/exception happens in CPL0 (kernel). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags;
} interrupt_frame_ring0;

/* CPU frame when interrupt/exception happens in CPL3 (user). */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame_ring3;

/* Common prefix passed from assembly; cast to ring3 when cs.DPL == 3. */
typedef interrupt_frame_ring0 interrupt_frame;

_Static_assert(sizeof(interrupt_frame_ring0) == 20 * sizeof(uint64_t), "ring0 frame size mismatch");
_Static_assert(sizeof(interrupt_frame_ring3) == 22 * sizeof(uint64_t), "ring3 frame size mismatch");

static inline int interrupt_from_user(const interrupt_frame* frame) {
    return (frame->cs & 0x3u) == 0x3u;
}

interrupt_frame *isr_handler(interrupt_frame* frame);
