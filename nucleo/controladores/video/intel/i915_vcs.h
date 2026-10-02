#ifndef CONTROLADORES_VIDEO_INTEL_I915_VCS_H
#define CONTROLADORES_VIDEO_INTEL_I915_VCS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define I915_VCS_RING_SIZE_BYTES (16 * 1024) // 16 KiB (4 páginas)

// Estado del motor de video VCS (Video Command Streamer)
typedef struct {
    int      iniciado;
    uint64_t mmio_vcs_base;
    uint64_t ring_gpu_addr;
    void    *ring_cpu_vaddr;
    uint32_t ring_head;
    uint32_t ring_tail;
    uint32_t ring_tamano;
    bool     quiescente;
    uint32_t trabajos_completados;
    uint32_t timeouts_registrados;
} i915_vcs_motor_t;

// API del motor VCS para decodificación y submission
int  i915_vcs_motor_iniciar(void);
void i915_vcs_motor_detener(void);
int  i915_vcs_esperar_quiescencia(uint32_t timeout_ms);
int  i915_vcs_reset(void);

// Envío de paquetes de comandos al Video Command Streamer
int  i915_vcs_enviar_batch(uint64_t batch_gpu_addr, uint32_t batch_dwords);

// Hito M16: Primer trabajo real VCS verificable con canario en memoria física
int  i915_vcs_ejecutar_primer_trabajo(uint32_t firma_esperada, uint32_t *resultado_out);

#endif // CONTROLADORES_VIDEO_INTEL_I915_VCS_H
