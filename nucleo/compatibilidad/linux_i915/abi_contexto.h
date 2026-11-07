#ifndef I915_ABI_CONTEXTO_H
#define I915_ABI_CONTEXTO_H
#include "tareas.h"
struct i915_abi_resultado {
    uint32_t fallos, ciclos;
    uint64_t padre_entrada,padre_llamada,hijo_entrada,funcion_entrada;
    struct i915_contexto_traza padre,hijo;
};
/* BSP/cooperative test only. No GPU access, no SIMD/TLS claim. */
int i915_abi_contexto_probar(struct i915_abi_resultado *resultado);
#endif
