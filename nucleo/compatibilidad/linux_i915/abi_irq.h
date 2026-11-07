#ifndef I915_ABI_IRQ_H
#define I915_ABI_IRQ_H
#include <stdint.h>
#include "cpu_runtime.h"
struct i915_irq_cpu {uint32_t fallos,ciclos,iniciales,emitidos,recibidos,en_contexto;};
struct i915_irq_resultado {uint32_t cpus,entradas,fallos,heap_limpio,admision,if_antes,if_despues;struct i915_irq_cpu cpu[I915_MAX_CPU];};
int i915_irq_contexto_probar(struct i915_irq_resultado *out);
#endif
