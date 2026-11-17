[bits 64]
section .text
global anillo3_entrar
global anillo3_volver

; int64_t anillo3_entrar(uint64_t *rsp_guardado, uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1)
; Guarda el contexto del nucleo y baja a Ring 3. Regresa solo cuando anillo3_volver lo restaura.
anillo3_entrar:
    pushfq
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    cli
    mov [rdi], rsp
    push qword 0x2B     ; SS usuario
    push rdx            ; RSP usuario
    push qword 0x202    ; RFLAGS: IF=1, IOPL=0
    push qword 0x33     ; CS usuario
    push rsi            ; RIP usuario
    mov rdi, rcx
    mov rsi, r8
    xor eax, eax        ; Ningun registro del nucleo llega al programa
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; void anillo3_volver(uint64_t rsp_guardado, int64_t valor, uint64_t fs_base)
; Abandona la pila de la interrupcion. El FS del nucleo vuelve antes de reactivar IF.
anillo3_volver:
    mov rsp, rdi
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov ecx, 0xC0000100
    mov eax, edx
    shr rdx, 32
    wrmsr
    mov rax, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    popfq
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
