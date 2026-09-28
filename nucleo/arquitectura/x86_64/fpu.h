#ifndef ARQUITECTURA_X86_64_FPU_H
#define ARQUITECTURA_X86_64_FPU_H

// Habilita x87/SSE2 para las unidades multimedia que los solicitan explícitamente.
void x86_64_fpu_iniciar(void);
/* Cada AP inicializa CR0/CR4/MXCSR antes de recibir trabajos. Cada ejecutor
 * posee sus registros XMM durante el lote; las trampas
 * conservan x87/MXCSR/XMM0..15 por FXSAVE64/FXRSTOR64 sobre stack alineado.
 * No habilita OSXSAVE/XCR0/YMM: AVX sigue prohibido. Si se añade scheduler,
 * debe guardar/restaurar este estado por tarea antes de permitir multimedia. */
int x86_64_fpu_sse2_lista(void);
typedef struct {unsigned sse2,xsave,avx2,ymm_habilitado;} x86_64_capacidades_simd;
x86_64_capacidades_simd x86_64_fpu_capacidades_cpu(void);

#endif
