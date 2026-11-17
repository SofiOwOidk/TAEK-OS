; Programas hostiles de Ring 3. Binario plano mapeado en ANILLO3_BASE, sin permiso de escritura.
; Entrada: rdi = numero de prueba, rsi = direccion objetivo. El orden coincide con g_pruebas en anillo3.c.
[bits 64]
[org 0x0000008000000000]
default rel

%define DATOS 0x0000008000100000
%define SYS_SALIR 0
%define SYS_ESCRIBIR 1
%define SYS_TIEMPO 2

%macro SALIR 1
    mov eax, SYS_SALIR
    mov edi, %1
    int 0x80
%endmacro

entrada:
    cmp rdi, (tabla_fin - tabla) / 8
    jae mala_prueba
    lea rax, [tabla]
    jmp [rax + rdi * 8]
mala_prueba:
    SALIR 200

tabla:
    dq saludo, p_cli, p_hlt, p_in, p_cr3, p_lgdt, leer, escribir, saltar, leer, leer
    dq nulo, escribir_codigo, ejecutar_pila, ejecutar_datos, desbordar, dividir
    dq p_int3, p_int50, p_int32, p_syscall, p_sysenter, p_ud2, bucle, fs_ataque
    dq flags_ac_nt, paso_a_paso, punteros, conserva
tabla_fin:

; Comprueba que el nucleo no filtro registros, datos, ni estado de coma flotante.
saludo:
    mov rax, rbx
    or rax, rcx
    or rax, rdx
    or rax, rbp
    or rax, r8
    or rax, r9
    or rax, r10
    or rax, r11
    or rax, r12
    or rax, r13
    or rax, r14
    or rax, r15
    jnz .mal_registros
    sub rsp, 32
    stmxcsr [rsp]
    cmp dword [rsp], 0x1F80
    jne .mal_fp
    fnstcw [rsp]
    cmp word [rsp], 0x37F
    jne .mal_fp
    movdqu [rsp], xmm0
    mov rax, [rsp]
    or rax, [rsp + 8]
    movdqu [rsp], xmm15
    or rax, [rsp]
    or rax, [rsp + 8]
    jnz .mal_fp
    add rsp, 32
    mov rdi, DATOS
    mov ecx, 8192
    xor eax, eax
.barrer:
    or al, [rdi + rcx - 1]
    loop .barrer
    test al, al
    jnz .mal_datos
    mov rax, 0x1234567890ABCDEF
    mov [rdi], rax
    cmp [rdi], rax
    jne .mal_datos
    mov eax, SYS_ESCRIBIR
    lea rdi, [texto]
    mov esi, texto_fin - texto
    int 0x80
    cmp rax, texto_fin - texto
    jne .mal_escribir
    mov eax, SYS_TIEMPO
    int 0x80
    test rax, rax
    js .mal_escribir
    SALIR 42
.mal_registros:
    SALIR 1
.mal_fp:
    SALIR 2
.mal_datos:
    SALIR 3
.mal_escribir:
    SALIR 4

p_cli:
    cli
    SALIR 100
p_hlt:
    hlt
    SALIR 100
p_in:
    in al, 0x60
    SALIR 100
p_cr3:
    mov rax, cr3
    SALIR 100
p_lgdt:
    lgdt [rsp]
    SALIR 100
leer:
    mov rax, [rsi]
    SALIR 100
escribir:
    mov rax, 0x4141414141414141
    mov [rsi], rax
    SALIR 100
saltar:
    jmp rsi
nulo:
    xor eax, eax
    mov rax, [rax]
    SALIR 100
escribir_codigo:
    lea rax, [entrada]
    mov byte [rax], 0x90
    SALIR 100
ejecutar_pila:
    push 0xC3
    jmp rsp
ejecutar_datos:
    mov rax, DATOS
    mov byte [rax], 0xC3
    jmp rax
desbordar:
    push rax
    jmp desbordar
dividir:
    xor ecx, ecx
    mov eax, 1
    xor edx, edx
    div ecx
    SALIR 100
p_int3:
    int3
    SALIR 100
p_int50:
    int 0x50
    SALIR 100
p_int32:
    int 32
    SALIR 100
p_syscall:
    syscall
    SALIR 100
p_sysenter:
    sysenter
    SALIR 100
p_ud2:
    ud2
bucle:
    jmp bucle
; Si el nucleo no restaurara FS, su proxima interrupcion leeria %fs:0 desde la base 0.
fs_ataque:
    mov ax, 0x2B
    mov fs, ax
    mov gs, ax
.girar:
    jmp .girar
; AC anularia SMAP y NT rompe iretq si llegara al nucleo. Sobrevive a interrupciones y llamadas.
flags_ac_nt:
    pushfq
    or qword [rsp], (1 << 18) | (1 << 14)
    popfq
    mov eax, SYS_TIEMPO
    int 0x80
    mov rbx, rax
.girar:
    mov eax, SYS_TIEMPO
    int 0x80
    sub rax, rbx
    cmp rax, 30
    jb .girar
    SALIR 7
paso_a_paso:
    pushfq
    or qword [rsp], 1 << 8
    popfq
    nop
    SALIR 100
; El sistema debe rechazar cada puntero sin copiar un byte. rsi = codigo del nucleo.
punteros:
    mov r12, rsi
    mov eax, SYS_ESCRIBIR
    mov rdi, r12
    mov esi, 16
    int 0x80
    cmp rax, -14
    jne .mal1
    mov eax, SYS_ESCRIBIR
    xor edi, edi
    mov esi, 16
    int 0x80
    cmp rax, -14
    jne .mal2
    mov eax, SYS_ESCRIBIR
    mov rdi, -1
    mov esi, 2
    int 0x80
    cmp rax, -14
    jne .mal3
    mov eax, SYS_ESCRIBIR
    mov rdi, DATOS
    mov rsi, 0x7FFFFFFFFFFFFFFF
    int 0x80
    cmp rax, -14
    jne .mal4
    mov eax, SYS_ESCRIBIR
    mov rdi, DATOS + 0x10000 - 1  ; Cruza el final de la region de datos
    mov esi, 2
    int 0x80
    cmp rax, -14
    jne .mal5
    mov eax, 9999                 ; Llamada inexistente
    int 0x80
    cmp rax, -38
    jne .mal6
    SALIR 99
.mal1:
    SALIR 11
.mal2:
    SALIR 12
.mal3:
    SALIR 13
.mal4:
    SALIR 14
.mal5:
    SALIR 15
.mal6:
    SALIR 16
; Registros y xmm deben sobrevivir a interrupciones del reloj en Ring 3.
conserva:
    mov r15, 0x0123456789ABCDEF
    mov r14, 0xFEDCBA9876543210
    mov rbp, 0xA5A5A5A5A5A5A5A5
    lea rax, [patron]
    movdqu xmm0, [rax]
    movdqu xmm15, [rax]
    mov eax, SYS_TIEMPO
    int 0x80
    mov r13, rax
.girar:
    mov ecx, 2000                 ; Casi todo el tiempo en Ring 3: ahi caen los ticks de 10 ms
.vuelta:
    pause
    loop .vuelta
    mov eax, SYS_TIEMPO
    int 0x80
    sub rax, r13
    cmp rax, 60
    jb .girar
    mov rax, 0x0123456789ABCDEF
    cmp r15, rax
    jne .mal_gpr
    mov rax, 0xFEDCBA9876543210
    cmp r14, rax
    jne .mal_gpr
    mov rax, 0xA5A5A5A5A5A5A5A5
    cmp rbp, rax
    jne .mal_gpr
    sub rsp, 16
    movdqu [rsp], xmm0
    mov rax, [rsp]
    cmp rax, [patron]
    jne .mal_xmm
    movdqu [rsp], xmm15
    mov rax, [rsp + 8]
    cmp rax, [patron + 8]
    jne .mal_xmm
    add rsp, 16
    SALIR 55
.mal_gpr:
    SALIR 21
.mal_xmm:
    SALIR 22

patron:
    dq 0x1122334455667788, 0x99AABBCCDDEEFF00
texto:
    db "Hola desde Ring 3", 10
texto_fin:
