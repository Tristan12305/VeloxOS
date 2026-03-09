bits 64

section .text

global paging_read_cr3
paging_read_cr3:
    mov rax, cr3
    ret

global paging_load_cr3
paging_load_cr3:
    mov cr3, rdi
    ret
