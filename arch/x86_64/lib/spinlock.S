bits 64



section .text
global spin_lock
global spin_unlock
global spin_lock_irqsave
global spin_unlock_irqrestore

spin_lock:
    lock bts dword [rdi], 0
    jnc .acquired
.spin_with_pause:
    pause
    test dword [rdi], 1
    jnz .spin_with_pause
    jmp spin_lock
.acquired:
    ret

spin_unlock:
    mov dword [rdi], 0
    ret

spin_lock_irqsave:
    pushfq
    pop  rax
    cli
.try_acquire:
    lock bts dword [rdi], 0
    jnc .acquired
.spin:
    pause
    test dword [rdi], 1
    jnz .spin
    jmp .try_acquire
.acquired:
    ret


spin_unlock_irqrestore:
    mov dword [rdi], 0          ; release lock
    push rsi
    popfq                       ; restore RFLAGS (re-enables interrupts if they were on)
    ret




