#ifndef I915_ABI_TLS_H
#define I915_ABI_TLS_H
#include <stdint.h>
struct i915_tls_resultado {
    uint32_t fallos,ciclos,hijo_ciclos[2],iniciales,privados,heap_limpio;
    uint64_t imagen_bytes;
};
int i915_tls_contexto_probar(struct i915_tls_resultado *out);
/* Canarios TLS compartidos por los diagnosticos, privados por hilo nativo. */
int i915_tls_verificar(uint64_t valor,uint8_t byte);
void i915_tls_escribir(uint64_t valor,uint8_t byte);
#endif
