#ifndef NVIDIA_CORE_H
#define NVIDIA_CORE_H

#include "../inc/nvtypes.h"
#include "../inc/nvstatus.h"
#include "../inc/nv_gsp.h"
#include "../firmware/gsp_firmware.h"
#include "../gsp/gsp_rpc.h"

// ============================================================================
// NVIDIA RESOURCE MANAGER CORE (HITO 15 + HITO 20)
// Máquina de estados operativos de la GPU, inicialización y extracción de capacidades
// ============================================================================

typedef enum {
    NV_GPU_ESTADO_NO_INICIADO = 0,
    NV_GPU_ESTADO_RESET,
    NV_GPU_ESTADO_FIRMWARE_LISTO,
    NV_GPU_ESTADO_GSP_INICIANDO,
    NV_GPU_ESTADO_OPERATIVO,
    NV_GPU_ESTADO_FALLO
} nv_gpu_estado_t;

struct nvidia_dispositivo {
    NvBool          presente;
    NvU8            bus;
    NvU8            slot;
    NvU8            func;
    NvU16           vendor_id;
    NvU16           device_id;
    NvU32           chip_id;
    char            chip_name[64];
    nv_gpu_estado_t estado;

    // Espacio de Control MMIO (BAR0)
    NvU64           bar0_phys;
    NvU64           bar0_size;
    void           *bar0_virt;

    // Espacio de Memoria de Video VRAM (BAR1)
    NvU64           bar1_phys;
    NvU64           bar1_size;

    // Capacidades de Hardware obtenidas vía GSP RPC
    gsp_gpu_capacidades_t caps;

    // Búfer DMA Coherente compartido con el GSP
    NvBool          gsp_buffer_listo;
    NvU64           gsp_shared_phys;
    void           *gsp_shared_virt;
};

NV_STATUS nvidia_core_iniciar(void);
NV_STATUS nvidia_gpu_inicializar_completo(void);
const struct nvidia_dispositivo *nvidia_core_obtener_dispositivo(void);
NV_STATUS nvidia_core_autodiagnostico(void);
const char *nvidia_core_estado_texto(NV_STATUS st);
const char *nvidia_gpu_estado_nombre(nv_gpu_estado_t estado);

#endif // NVIDIA_CORE_H
