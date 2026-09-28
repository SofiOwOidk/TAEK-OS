section .text
global smp_entrada
extern smp_trabajador
smp_entrada:
    cli
    mov rax, [rdi + 24] ; limine_smp_info.extra_argument (verificado con offsetof)
    mov rsp, [rax]      ; datos_cpu.pila
    and rsp, -16
    xor rbp, rbp
    mov rdi, rax
    call smp_trabajador
.fin:
    cli
    hlt
    jmp .fin
section .note.GNU-stack noalloc noexec nowrite progbits
