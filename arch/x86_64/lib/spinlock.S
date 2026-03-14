bits 64

section .text

global spin_lock
global spin_unlock


spin_lock:
    lock bts [rdi],0
    jc .spin_with_pause
    ret

.spin_with_pause:
    pause
    test dword [rdi],1
    jnz .spin_with_pause
    jmp spin_lock

spin_unlock:
    mov dword [rdi],0
    ret







