; Hardware state is sampled before any C prologue can clobber XMM/x87.
default rel
section .text
extern i915_cambiar_contexto
extern i915_simd_padre_rsp,i915_simd_hijo_rsp
extern i915_simd_patrones,i915_simd_actual
global i915_simd_una_vez,i915_simd_hijo,i915_simd_entrada_fn

i915_simd_una_vez:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 520 ; caller state area aligned to 16, original RSP %16==8
    fxsave64 [rsp]
    fxrstor64 [i915_simd_patrones]
    lea rdi,[i915_simd_padre_rsp]
    mov rsi,[i915_simd_hijo_rsp]
    xor edx,edx
    xor ecx,ecx
    call i915_cambiar_contexto
    fxsave64 [i915_simd_actual]
    fxrstor64 [rsp] ; test is transparent to the invoking C context
    add rsp,520
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; As the production naked trampoline, initial RSP %16==0.
i915_simd_hijo:
.resume:
    fxsave64 [i915_simd_actual+512]
    fxrstor64 [i915_simd_patrones+512]
    lea rdi,[i915_simd_hijo_rsp]
    mov rsi,[i915_simd_padre_rsp]
    xor edx,edx
    xor ecx,ecx
    call i915_cambiar_contexto
    jmp .resume

; Normal task function: first instruction samples initial task state.
i915_simd_entrada_fn:
    fxsave64 [rdi]
    ret
section .note.GNU-stack noalloc noexec nowrite progbits
