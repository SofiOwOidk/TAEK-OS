#ifndef CONTROLADORES_VIDEO_INTEL_I915_DRV_H
#define CONTROLADORES_VIDEO_INTEL_I915_DRV_H

#include <stdint.h>
#include <stddef.h>
#include "intel_info.h"

#define I915_MMIO_VIRTUAL_BASE     0xFFFFFE0010000000ULL // 16 MB ventana MMIO
#define I915_APERTURE_VIRTUAL_BASE 0xFFFFFE0020000000ULL // Ventana virtual de apertura (opcional)

// Offsets normativos Gen 9 (Kaby Lake PRM)
#define GEN8_GGTT_PTE_OFFSET       0x00800000ULL         // Base de PTEs GGTT dentro de BAR0 (8 MB)
#define GEN8_RING_VCS0_BASE        0x001C0000ULL         // Offset MMIO del Video Command Streamer (VCS0)
#define GEN8_RING_TAIL             0x00000030ULL
#define GEN8_RING_HEAD             0x00000034ULL
#define GEN8_RING_START            0x00000038ULL
#define GEN8_RING_CTL              0x0000003CULL
#define GEN8_RING_ACTHD            0x00000074ULL
#define GEN8_RING_HWS_PGA          0x00000080ULL
#define GEN8_RESET_CTL             0x000000C0ULL

// Opcodes del Command Streamer (Intel PRM Vol 2c)
#define MI_NOOP                    0x00000000U
#define MI_USER_INTERRUPT          (0x02U << 23)
#define MI_BATCH_BUFFER_END        (0x0AU << 23)
#define MI_STORE_DWORD_IMM         ((0x20U << 23) | 2U)

// Bits de PTE GGTT Gen 8/9
#define GEN8_PTE_VALID             (1ULL << 0)
#define GEN8_PTE_CACHE_LLC         (1ULL << 1)

typedef enum {
    I915_ESTADO_NO_INICIADO = 0,
    I915_ESTADO_CONSULTA_PASIVA,
    I915_ESTADO_BLOQUEADO,      // init activa rechazada: falta acreditación
    I915_ESTADO_MMIO_LISTO,
    I915_ESTADO_GGTT_LISTO,
    I915_ESTADO_VCS_LISTO,
    I915_ESTADO_FALLO
} i915_estado_motor_t;

// Motivos verificables de bloqueo de la inicialización activa.
enum {
    I915_BLOQUEO_NINGUNO = 0,
    I915_BLOQUEO_SIN_PLATAFORMA,
    I915_BLOQUEO_VTD_TRADUCCION,
    I915_BLOQUEO_PLATAFORMA_NO_ACREDITADA,
    I915_BLOQUEO_DMA_NO_ACREDITADO,
    I915_BLOQUEO_SIN_OWNERSHIP_GGTT,
    I915_BLOQUEO_INVENTARIO_PCI,
    I915_BLOQUEO_INVENTARIO_IOMMU
};

struct i915_dispositivo {
    struct intel_gpu_info info;
    uint64_t              mmio_base_virt;
    uint64_t              mmio_tamano;
    
    // GGTT Tracking & Preservación de Framebuffer GOP
    uint64_t              ggtt_ptes_virt;
    uint32_t              ggtt_total_ptes;
    uint32_t              ggtt_gop_reservadas;
    uint32_t              ggtt_proxima_libre;

    // Estado de motor de video VCS
    i915_estado_motor_t   estado;
    int                   vcs_activo;
    int                   vtd_passthrough_confirmado;
    int                   submission_habilitada; // 0 = deshabilitada (por defecto)

    // Acreditaciones exigidas antes de tocar hardware de forma activa.
    int                   platform_acreditada;   // 0 = no acreditada
    int                   dma_acreditado;        // 0 = no acreditado (1:1 verificado)
    int                   ggtt_ownership;        // 0 = no adquirida
    int                   motivo_bloqueo;        // I915_BLOQUEO_*
    uint16_t              pci_comando_leido;     // readback verificable del registro de comando
};

int  i915_driver_iniciar(void);
/* Getter sin efectos: NO inicia el controlador. Devuelve NULL si no está
 * acreditado e inicializado explícitamente. */
const struct i915_dispositivo *i915_obtener_dispositivo(void);
uint32_t i915_leer_mmio_32(uint32_t offset);
void     i915_escribir_mmio_32(uint32_t offset, uint32_t valor);

// Envío de trabajos al motor de video VCS:
// DESHABILITADO TEMPORALMENTE: requiere cerrar ownership formal de GGTT,
// direccionamiento DMA, selección de plataforma y recuperación antes de permitir submission.
int  i915_vcs_primer_trabajo(uint32_t firma_esperada, uint32_t *resultado_out);

// Consulta pasiva del estado del dispositivo Intel i915 sin inicialización MMIO
// ni activación de Bus Mastering.
void i915_imprimir_estado(void);

#endif // CONTROLADORES_VIDEO_INTEL_I915_DRV_H
