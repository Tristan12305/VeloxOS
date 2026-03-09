bits 64

global gdt_install

section .data

align 16
tss64:
    ; You can zero-initialize it for now.
    ; Later you'll set rsp0 and IST pointers.
    times 104 db 0

align 8
gdt_start:
    dq 0                                ; null

; kernel code: base=0 limit=0, access=0x9A, flags=0xA0 (L=1, G=1)
gdt_kcode:
    dw 0x0000
    dw 0x0000
    db 0x00
    db 10011010b                        ; 0x9A
    db 10100000b                        ; 0xA0 (L=1, G=1, D=0)
    db 0x00

; kernel data: access=0x92, flags=0x00
gdt_kdata:
    dw 0x0000
    dw 0x0000
    db 0x00
    db 10010010b                        ; 0x92
    db 00000000b
    db 0x00

; 64-bit TSS descriptor (16 bytes, two entries)
; Base fields are patched at runtime before lgdt.
gdt_tss:
    dw (104 - 1)                        ; limit 0..15
    dw 0                                ; base 0..15
    db 0                                ; base 16..23
    db 10001001b                        ; access 0x89 (Present, type=Available TSS)
    db (((104 - 1) >> 16) & 0x0F)       ; limit 16..19 (flags=0 for TSS)
    db 0                                ; base 24..31
    dd 0                                ; base 32..63
    dd 0                                ; reserved

gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dq gdt_start

; Selectors (byte offsets into GDT)
KCODE_SEL equ (gdt_kcode - gdt_start)
KDATA_SEL equ (gdt_kdata - gdt_start)
KTSS_SEL  equ (gdt_tss   - gdt_start)


section .text
gdt_install:
    cli                                 ; optional early on (recommended)

    ; Build the 64-bit base fields in the TSS descriptor.
    lea rax, [rel tss64]
    mov word [rel gdt_tss + 2], ax
    shr rax, 16
    mov byte [rel gdt_tss + 4], al
    shr rax, 8
    mov byte [rel gdt_tss + 7], al
    shr rax, 8
    mov dword [rel gdt_tss + 8], eax

    lgdt [rel gdt_ptr]

    ; Reload data segments
    mov ax, KDATA_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; Reload CS via far return
    push qword KCODE_SEL
    lea  rax, [rel .flush_cs]
    push rax
    retfq

.flush_cs:
    ; Load task register with TSS selector
    mov ax, KTSS_SEL
    ltr ax

    ;sti
    ret
