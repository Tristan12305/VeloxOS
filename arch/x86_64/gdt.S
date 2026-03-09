
global x86_loadGDT

x86_loadGDT:
    ; Load the GDT descriptor — rdi already holds the pointer
    lgdt [rdi]

    ; We need to do a far return to reload CS with the new code segment.
    ; In 64-bit mode you can't push a 64-bit segment selector with a direct
    ; far jump, so the trick is to manually create a return frame on the
    ; stack: [rip, cs] and then use retfq.

    ; Push the data segment value for use after the far return
    push rdx                    ; save data segment for later

    ; Build the far-return frame on the stack
    ; Stack needs: [new rip (8 bytes), new cs (8 bytes)] for retfq
    lea rax, [rel .reload_cs]   ; rip-relative address of .reload_cs
    push rsi                    ; push code segment selector (zero-extended)
    push rax                    ; push return address
    retfq                       ; far return — reloads CS, jumps to .reload_cs

.reload_cs:
    ; Reload all data segment registers with the data segment selector
    ; rdx was pushed before the far ret, so pop it back
    pop rdx

    mov ax, dx
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ret
