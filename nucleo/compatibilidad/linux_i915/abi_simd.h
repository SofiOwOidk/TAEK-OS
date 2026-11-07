#ifndef I915_ABI_SIMD_H
#define I915_ABI_SIMD_H
#include "tareas.h"
struct i915_simd_resultado {uint32_t fallos,ciclos,inicial_limpio,sse2;};
int i915_simd_contexto_probar(struct i915_simd_resultado *out);
#endif
