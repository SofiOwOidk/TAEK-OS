section .text
global i915_abi_probar_conmutacion_asm

; ==============================================================================
; uint32_t i915_abi_probar_conmutacion_asm(void (*conmutar)(void *),
;                                          void *ctx,
;                                          uint32_t mascara_omitir);
;
; Parámetros (System V AMD64 ABI):
;   rdi : puntero a función de conmutación (ej: wrapper de i915_tarea_ceder)
;   rsi : puntero a contexto opcional para la función
;   edx : máscara de mutación para pruebas de sensibilidad (0 = normal)
;         Bit 0 (0x01): simular fallo/omisión en rbp
;         Bit 1 (0x02): simular fallo/omisión en rbx
;         Bit 2 (0x04): simular fallo/omisión en r12
;         Bit 3 (0x08): simular fallo/omisión en r13
;         Bit 4 (0x10): simular fallo/omisión en r14
;         Bit 5 (0x20): simular fallo/omisión en r15
;
; Retorno:
;   eax : 0 si todos los registros preservaron sus canarios vivos.
;         Máscara de bits con los registros que fallaron en la restauración.
; ==============================================================================

i915_abi_probar_conmutacion_asm:
    ; 1. Salvar registros callee-saved reales del llamador
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    push rdi                ; [rsp + 16] funcion conmutar (tras 3 pushes)
    push rsi                ; [rsp + 8]  argumento ctx
    push rdx                ; [rsp]      mascara_omitir
    sub rsp, 16             ; [rsp]: tsc_inicio, [rsp+8]: padding -> RSP % 16 == 0

    ; Verificación estricta de alineación System V: RSP debe ser múltiplo de 16
    mov rax, rsp
    and rax, 0x0F
    jnz .fallo_alineacion

    ; 2. Cargar canarios únicos en los 6 registros callee-saved (incluyendo rbp)
    mov rbx, 0xCAFEBABE00000001
    mov rbp, 0xCAFEBABE00000002
    mov r12, 0xCAFEBABE00000003
    mov r13, 0xCAFEBABE00000004
    mov r14, 0xCAFEBABE00000005
    mov r15, 0xCAFEBABE00000006

    ; Marca de tiempo inicial con serialización estricta (LFENCE + RDTSC)
    lfence
    rdtsc
    shl rdx, 32
    or rax, rdx
    mov [rsp], rax          ; guardar tsc_inicio

    ; 3. Invocar la conmutación de contexto a través de la función recibida
    mov rdi, [rsp + 24]     ; rdi = ctx
    mov rax, [rsp + 32]     ; rax = conmutar
    call rax

    ; Marca de tiempo final
    lfence
    rdtsc
    shl rdx, 32
    or rax, rdx
    sub rax, [rsp]          ; rax = ciclos transcurridos

    ; 4. Reanudar ejecución: rdx = mascara_omitir
    mov rdx, [rsp + 16]

    ; 5. Aplicar mutaciones inducidas si se solicita probar sensibilidad del test
    test edx, 1
    jz .no_mut_rbp
    mov rbp, 0xDEADDEAD00000001
.no_mut_rbp:
    test edx, 2
    jz .no_mut_rbx
    mov rbx, 0xDEADDEAD00000002
.no_mut_rbx:
    test edx, 4
    jz .no_mut_r12
    mov r12, 0xDEADDEAD00000003
.no_mut_r12:
    test edx, 8
    jz .no_mut_r13
    mov r13, 0xDEADDEAD00000004
.no_mut_r13:
    test edx, 16
    jz .no_mut_r14
    mov r14, 0xDEADDEAD00000005
.no_mut_r14:
    test edx, 32
    jz .no_mut_r15
    mov r15, 0xDEADDEAD00000006
.no_mut_r15:

    ; 6. Verificar que cada registro callee-saved conserve vivo su canario exacto
    xor eax, eax            ; eax = 0 (éxito inicial)

    mov rcx, 0xCAFEBABE00000001
    cmp rbx, rcx
    je .rbx_ok
    or eax, 2
.rbx_ok:

    mov rcx, 0xCAFEBABE00000002
    cmp rbp, rcx
    je .rbp_ok
    or eax, 1
.rbp_ok:

    mov rcx, 0xCAFEBABE00000003
    cmp r12, rcx
    je .r12_ok
    or eax, 4
.r12_ok:

    mov rcx, 0xCAFEBABE00000004
    cmp r13, rcx
    je .r13_ok
    or eax, 8
.r13_ok:

    mov rcx, 0xCAFEBABE00000005
    cmp r14, rcx
    je .r14_ok
    or eax, 16
.r14_ok:

    mov rcx, 0xCAFEBABE00000006
    cmp r15, rcx
    je .r15_ok
    or eax, 32
.r15_ok:

    ; 7. Restaurar registros del llamador y retornar
    add rsp, 40
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

.fallo_alineacion:
    mov eax, 0x80           ; Código de error 0x80: RSP no cumple System V (16 bytes)
    add rsp, 40
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
