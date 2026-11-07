#ifndef I915_SCHEDULER_PRUEBA_H
#define I915_SCHEDULER_PRUEBA_H
#include <stdint.h>
#include "cpu_runtime.h"
struct i915_smp_cpu_resultado {uint32_t fallos,ciclos,hijos,iniciales;uint64_t fs_raiz;};
struct i915_smp_resultado {
    uint32_t cpus,entradas,fallos,heap_limpio;
    struct i915_smp_cpu_resultado cpu[I915_MAX_CPU];
};
int i915_scheduler_smp_probar(struct i915_smp_resultado *out);
#endif
