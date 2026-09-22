[bits 64]

extern manejador_excepciones

%macro TRAMPA_SIN_ERROR 1
global trampa%1
trampa%1:
    push qword 0        ; Codigo de error simulado
    push qword %1       ; Numero de interrupcion
    jmp trampa_comun_stub
%endmacro

%macro TRAMPA_CON_ERROR 1
global trampa%1
trampa%1:
    push qword %1       ; Numero de interrupcion (codigo de error ya empujado por la CPU)
    jmp trampa_comun_stub
%endmacro

TRAMPA_SIN_ERROR 0
TRAMPA_SIN_ERROR 1
TRAMPA_SIN_ERROR 2
TRAMPA_SIN_ERROR 3
TRAMPA_SIN_ERROR 4
TRAMPA_SIN_ERROR 5
TRAMPA_SIN_ERROR 6
TRAMPA_SIN_ERROR 7
TRAMPA_CON_ERROR 8
TRAMPA_SIN_ERROR 9
TRAMPA_CON_ERROR 10
TRAMPA_CON_ERROR 11
TRAMPA_CON_ERROR 12
TRAMPA_CON_ERROR 13
TRAMPA_CON_ERROR 14
TRAMPA_SIN_ERROR 15
TRAMPA_SIN_ERROR 16
TRAMPA_CON_ERROR 17
TRAMPA_SIN_ERROR 18
TRAMPA_SIN_ERROR 19
TRAMPA_SIN_ERROR 20
TRAMPA_CON_ERROR 21
TRAMPA_SIN_ERROR 22
TRAMPA_SIN_ERROR 23
TRAMPA_SIN_ERROR 24
TRAMPA_SIN_ERROR 25
TRAMPA_SIN_ERROR 26
TRAMPA_SIN_ERROR 27
TRAMPA_SIN_ERROR 28
TRAMPA_CON_ERROR 29
TRAMPA_CON_ERROR 30
TRAMPA_SIN_ERROR 31

trampa_comun_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        ; 1er parametro segun System V ABI: puntero a marco_interrupcion
    cld                 ; Limpiar bandera de direccion
    call manejador_excepciones

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         ; Limpiar numero de interrupcion y codigo de error
    iretq
