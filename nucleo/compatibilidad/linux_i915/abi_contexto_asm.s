; Exact ABI probes of the production context switch. No C wrapper can restore
; canaries before they are measured. Mutations live in tareas.c, not here.
default rel
section .text
extern i915_cambiar_contexto
extern i915_abi_padre_rsp, i915_abi_hijo_rsp
extern i915_abi_padre_traza, i915_abi_hijo_traza
extern i915_abi_padre_entrada, i915_abi_padre_llamada, i915_abi_hijo_entrada
extern i915_abi_hijo_fallos
global i915_abi_contexto_una_vez, i915_abi_contexto_hijo
global i915_abi_entrada_fn

%macro CHECK 3
    mov rcx, %2
    cmp %1, rcx
    je %%ok
    or eax, %3
%%ok:
%endmacro

i915_abi_contexto_una_vez:
    mov [i915_abi_padre_entrada], rsp ; immediately at function entry
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov rbp, 0xA110000000000001
    mov rbx, 0xA110000000000002
    mov r12, 0xA110000000000004
    mov r13, 0xA110000000000008
    mov r14, 0xA110000000000010
    mov r15, 0xA110000000000020
    lea rdi, [i915_abi_padre_rsp]
    mov rsi, [i915_abi_hijo_rsp]
    lea rdx, [i915_abi_padre_traza]
    mov [i915_abi_padre_llamada], rsp ; immediately before production call
    xor ecx,ecx ; capture hardware IF in the production primitive
    call i915_cambiar_contexto
    xor eax, eax
    CHECK rbp, 0xA110000000000001, 1
    CHECK rbx, 0xA110000000000002, 2
    CHECK r12, 0xA110000000000004, 4
    CHECK r13, 0xA110000000000008, 8
    CHECK r14, 0xA110000000000010, 16
    CHECK r15, 0xA110000000000020, 32
    mov rcx, rsp
    and ecx, 15
    jz .aligned
    or eax, 128
.aligned:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; Internal target starts as the production naked trampoline: RSP % 16 == 0,
; no normal CALL return address. A new saved frame resumes it below call.
i915_abi_contexto_hijo:
    mov [i915_abi_hijo_entrada], rsp
.resume:
    xor eax, eax
    CHECK rbp, 0xB220000000000001, 1
    CHECK rbx, 0xB220000000000002, 2
    CHECK r12, 0xB220000000000004, 4
    CHECK r13, 0xB220000000000008, 8
    CHECK r14, 0xB220000000000010, 16
    CHECK r15, 0xB220000000000020, 32
    mov rcx, rsp
    and ecx, 15
    jz .aligned
    or eax, 128
.aligned:
    or [i915_abi_hijo_fallos], eax
    lea rdi, [i915_abi_hijo_rsp]
    mov rsi, [i915_abi_padre_rsp]
    lea rdx, [i915_abi_hijo_traza]
    xor ecx,ecx
    call i915_cambiar_contexto
    jmp .resume

; Used as a normal task function. Measure before its prologue, then return
; through the real C trampoline/task teardown.
i915_abi_entrada_fn:
    mov rax, rsp
    mov [rdi], rax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
