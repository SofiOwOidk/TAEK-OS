#ifndef ARQUITECTURA_X86_64_FPU_H
#define ARQUITECTURA_X86_64_FPU_H

// Habilita x87/SSE2 para las unidades multimedia que los solicitan explícitamente.
void x86_64_fpu_iniciar(void);
/* Cada AP inicializa CR0/CR4/MXCSR antes de recibir trabajos. Cada ejecutor
 * posee sus registros XMM durante el lote; las trampas
 * conservan x87/MXCSR/XMM0..15 por FXSAVE64/FXRSTOR64 sobre stack alineado.
 * Deshabilita OSXSAVE heredado: AVX sigue prohibido. El scheduler i915 guarda
 * x87/MMX/MXCSR/XMM0..15 por tarea. No admite YMM/ZMM ni acredita TLS. */
int x86_64_fpu_sse2_lista(void);
typedef struct {unsigned sse2,xsave,avx2,ymm_habilitado;} x86_64_capacidades_simd;
x86_64_capacidades_simd x86_64_fpu_capacidades_cpu(void);

#endif
