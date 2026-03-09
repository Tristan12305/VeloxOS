bits 64

; isr_stubs.asm
; ISR stubs for x86-64 IDT

section .text

; Macro for exceptions WITHOUT an error code (CPU doesn't push one)
; We push a dummy 0 to keep the stack frame uniform
%macro isr_no_err 1
isr_stub_%+%1:
    push    0               ; dummy error code
    push    %1              ; interrupt vector number
    jmp     isr_common_stub
%endmacro

; Macro for exceptions WITH an error code (CPU pushes it automatically)
%macro isr_err 1
isr_stub_%+%1:
    push    %1              ; interrupt vector number (error code already on stack)
    jmp     isr_common_stub
%endmacro

; x86-64 exception vectors 0-31
; These specific ones push an error code automatically:
;   8 (Double Fault), 10-14 (TSS/Segment/Stack/GP/Page), 17, 21, 29, 30
isr_no_err  0   ; Divide by Zero
isr_no_err  1   ; Debug
isr_no_err  2   ; NMI
isr_no_err  3   ; Breakpoint
isr_no_err  4   ; Overflow
isr_no_err  5   ; Bound Range Exceeded
isr_no_err  6   ; Invalid Opcode
isr_no_err  7   ; Device Not Available
isr_err     8   ; Double Fault
isr_no_err  9   ; Coprocessor Segment Overrun (legacy)
isr_err     10  ; Invalid TSS
isr_err     11  ; Segment Not Present
isr_err     12  ; Stack-Segment Fault
isr_err     13  ; General Protection Fault
isr_err     14  ; Page Fault
isr_no_err  15  ; Reserved
isr_no_err  16  ; x87 FPU Error
isr_err     17  ; Alignment Check
isr_no_err  18  ; Machine Check
isr_no_err  19  ; SIMD FPU Exception
isr_no_err  20  ; Virtualization Exception
isr_err     21  ; Control Protection Exception
isr_no_err  22  ; Reserved
isr_no_err  23  ; Reserved
isr_no_err  24  ; Reserved
isr_no_err  25  ; Reserved
isr_no_err  26  ; Reserved
isr_no_err  27  ; Reserved
isr_no_err  28  ; Hypervisor Injection
isr_err     29  ; VMM Communication
isr_err     30  ; Security Exception
isr_no_err  31  ; Reserved

; External IRQ / software interrupt vectors 32-255 (no CPU error code)
%assign i 32
%rep 224
isr_no_err i
%assign i i+1
%endrep

	; Common stub: save all registers, call C handler, restore, iretq
	extern isr_handler     ; your C handler
	isr_common_stub:
    ; At this point the stack looks like (top = rsp):
    ;   [vector]        <- we pushed this
    ;   [error code]    <- either real or dummy 0
    ;   [rip]           <- pushed by CPU
    ;   [cs]
    ;   [rflags]
    ;   [rsp]           <- if privilege change occurred
    ;   [ss]

    ; Save general-purpose registers
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
	    push    r13
	    push    r14
	    push    r15

	    ; C code expects DF=0 and ABI-aligned stack.
	    cld
	    mov     rdi, rsp        ; pass pointer to software frame
	    mov     rax, rsp        ; keep original frame pointer for restore
	    and     rsp, -16
	    sub     rsp, 8
	    push    rax
	    call    isr_handler
	    pop     rax
	    mov     rsp, rax

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax

    add     rsp, 16         ; pop vector number + error code
    iretq                   ; return from interrupt (64-bit)

; Export the table of stub pointers
section .data
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
    %assign i i+1
%endrep
