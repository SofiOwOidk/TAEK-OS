#include "i915_vcs.h"
#include "i915_drv.h"
#include "i915_gem.h"
#include "arquitectura/x86_64/serial.h"
#include "base/tiempo.h"
#include <string.h>

static i915_vcs_motor_t g_vcs;
static struct drm_i915_gem_object *g_ring_bo = NULL;

int i915_vcs_motor_iniciar(void) {
    memset(&g_vcs, 0, sizeof(g_vcs));
    g_vcs.mmio_vcs_base = GEN8_RING_VCS0_BASE;
    g_vcs.ring_tamano = I915_VCS_RING_SIZE_BYTES;

    // Asignar y fijar en GGTT el buffer circular del VCS
    g_ring_bo = i915_gem_crear_objeto(g_vcs.ring_tamano);
    if (!g_ring_bo) {
        serial_imprimir_linea("[I915_VCS] ERROR: No se pudo asignar el Ring Buffer de VCS.");
        return -1;
    }

    int ret = i915_gem_pin_en_ggtt(g_ring_bo, &g_vcs.ring_gpu_addr);
    if (ret != 0) {
        serial_imprimir_linea("[I915_VCS] ERROR: No se pudo mapear el Ring Buffer en GGTT.");
        i915_gem_liberar_objeto(g_ring_bo);
        g_ring_bo = NULL;
        return -1;
    }

    g_vcs.ring_cpu_vaddr = g_ring_bo->dir_cpu;
    memset(g_vcs.ring_cpu_vaddr, 0, g_vcs.ring_tamano);

    // Programar registros de control del anillo en el hardware VCS
    i915_escribir_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_START), (uint32_t)g_vcs.ring_gpu_addr);
    i915_escribir_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_HEAD), 0);
    i915_escribir_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_TAIL), 0);

    // Habilitar el anillo con tamaño configurado (formato Intel: (bytes - 4096) | RING_VALID)
    uint32_t ctl = (uint32_t)((g_vcs.ring_tamano - 4096) | 1U);
    i915_escribir_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_CTL), ctl);

    g_vcs.iniciado = 1;
    g_vcs.quiescente = true;

    serial_imprimir("[I915_VCS] Motor VCS0 iniciado. Ring GGTT=0x");
    serial_imprimir_hex(g_vcs.ring_gpu_addr);
    serial_imprimir_linea(" listo para admision.");
    return 0;
}

void i915_vcs_motor_detener(void) {
    if (!g_vcs.iniciado) return;

    // Desactivar el anillo en hardware
    i915_escribir_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_CTL), 0);

    if (g_ring_bo) {
        i915_gem_liberar_objeto(g_ring_bo);
        g_ring_bo = NULL;
    }
    g_vcs.iniciado = 0;
    serial_imprimir_linea("[I915_VCS] Motor VCS0 detenido limpiamente.");
}

int i915_vcs_esperar_quiescencia(uint32_t timeout_ms) {
    if (!g_vcs.iniciado) return 0;

    uint64_t t0 = tiempo_obtener_milisegundos();
    while (tiempo_obtener_milisegundos() - t0 < timeout_ms) {
        uint32_t head = i915_leer_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_HEAD)) & 0x001FFFFCU;
        uint32_t tail = i915_leer_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_TAIL)) & 0x001FFFFCU;

        if (head == tail) {
            g_vcs.quiescente = true;
            return 0; // Motor en reposo total
        }
    }

    g_vcs.quiescente = false;
    g_vcs.timeouts_registrados++;
    serial_imprimir_linea("[I915_VCS] ADVERTENCIA: Timeout esperando quiescencia del motor VCS.");
    return -110; // -ETIMEDOUT
}

int i915_vcs_reset(void) {
    serial_imprimir_linea("[I915_VCS] Ejecutando reset de recuperacion de VCS0...");
    uint32_t reg_reset = (uint32_t)(g_vcs.mmio_vcs_base + GEN8_RESET_CTL);
    i915_escribir_mmio_32(reg_reset, 1U); // Solicitar reset

    uint64_t t0 = tiempo_obtener_milisegundos();
    while (tiempo_obtener_milisegundos() - t0 < 100) {
        if ((i915_leer_mmio_32(reg_reset) & 1U) == 0) {
            serial_imprimir_linea("[I915_VCS] Reset completado con exito. Motor en estado seguro.");
            g_vcs.quiescente = true;
            return 0;
        }
    }
    serial_imprimir_linea("[I915_VCS] ERROR: Reset de VCS0 no respondio dentro de tiempo.");
    return -1;
}

int i915_vcs_enviar_batch(uint64_t batch_gpu_addr, uint32_t batch_dwords) {
    (void)batch_dwords;
    if (!g_vcs.iniciado) return -EINVAL;

    volatile uint32_t *ring = (volatile uint32_t *)g_vcs.ring_cpu_vaddr;
    uint32_t tail = g_vcs.ring_tail;

    // MI_BATCH_BUFFER_START en Ring Buffer
    ring[tail / 4]     = (0x31U << 23) | (1U << 8) | 1U;
    ring[(tail / 4) + 1] = (uint32_t)(batch_gpu_addr & 0xFFFFFFFFU);
    ring[(tail / 4) + 2] = (uint32_t)(batch_gpu_addr >> 32);
    ring[(tail / 4) + 3] = MI_NOOP;

    tail = (tail + 16) % g_vcs.ring_tamano;
    g_vcs.ring_tail = tail;

    // Disparar ejecución en hardware actualizando el timbre del anillo
    i915_escribir_mmio_32((uint32_t)(g_vcs.mmio_vcs_base + GEN8_RING_TAIL), tail);
    g_vcs.quiescente = false;
    return 0;
}

int i915_vcs_ejecutar_primer_trabajo(uint32_t firma_esperada, uint32_t *resultado_out) {
    if (!resultado_out) return -EINVAL;
    *resultado_out = 0;

    serial_imprimir("[I915_M16] Iniciando prueba de primer trabajo VCS. Firma esperada: 0x");
    serial_imprimir_hex(firma_esperada);
    serial_imprimir_linea("");

    // 1. Asignar objeto de destino (canario en memoria física propia)
    struct drm_i915_gem_object *obj_destino = i915_gem_crear_objeto(4096);
    if (!obj_destino) return -ENOMEM;

    volatile uint32_t *mem_destino = (volatile uint32_t *)obj_destino->dir_cpu;
    *mem_destino = 0x55555555U; // Inicializado con patrón diferente a la firma esperada

    uint64_t gpu_addr_destino = 0;
    if (i915_gem_pin_en_ggtt(obj_destino, &gpu_addr_destino) != 0) {
        i915_gem_liberar_objeto(obj_destino);
        return -ENOMEM;
    }

    // 2. Asignar Batch Buffer con las instrucciones de la GPU
    struct drm_i915_gem_object *obj_batch = i915_gem_crear_objeto(4096);
    if (!obj_batch) {
        i915_gem_liberar_objeto(obj_destino);
        return -ENOMEM;
    }

    uint64_t gpu_addr_batch = 0;
    if (i915_gem_pin_en_ggtt(obj_batch, &gpu_addr_batch) != 0) {
        i915_gem_liberar_objeto(obj_batch);
        i915_gem_liberar_objeto(obj_destino);
        return -ENOMEM;
    }

    volatile uint32_t *cmd = (volatile uint32_t *)obj_batch->dir_cpu;
    cmd[0] = (0x20U << 23) | 2U; // MI_STORE_DWORD_IMM (4 DWORDs)
    cmd[1] = (uint32_t)(gpu_addr_destino & 0xFFFFFFFFU);
    cmd[2] = (uint32_t)(gpu_addr_destino >> 32);
    cmd[3] = firma_esperada;
    cmd[4] = MI_USER_INTERRUPT;
    cmd[5] = MI_BATCH_BUFFER_END;
    cmd[6] = MI_NOOP;
    cmd[7] = MI_NOOP;

    obj_destino->en_uso_gpu = true;
    obj_batch->en_uso_gpu = true;

    // 3. Enviar el trabajo al motor VCS0
    i915_vcs_enviar_batch(gpu_addr_batch, 8);

    // 4. Sondeo de finalización con timeout normativo de 2000 ms
    uint64_t t0 = tiempo_obtener_milisegundos();
    bool exito = false;
    while (tiempo_obtener_milisegundos() - t0 < 2000) {
        if (*mem_destino == firma_esperada) {
            exito = true;
            break;
        }
    }

    if (!exito) {
        serial_imprimir_linea("[I915_M16] FALLO: Timeout de 2000 ms superado sin recibir firma.");
        i915_vcs_reset();
        // Por regla de quiescencia, NO liberar los buffers si la GPU podría seguir escribiéndolos
        return -110;
    }

    *resultado_out = *mem_destino;
    obj_destino->en_uso_gpu = false;
    obj_batch->en_uso_gpu = false;

    // 5. Verificación de quiescencia y liberación simétrica
    i915_vcs_esperar_quiescencia(500);
    i915_gem_liberar_objeto(obj_batch);
    i915_gem_liberar_objeto(obj_destino);

    g_vcs.trabajos_completados++;
    serial_imprimir_linea("[I915_M16] EXITO: Firma verificada en memoria destino escrita por GPU VCS0.");
    return 0;
}
