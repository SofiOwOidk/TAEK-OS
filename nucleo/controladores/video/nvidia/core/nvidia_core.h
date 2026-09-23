#ifndef NVIDIA_CORE_H
#define NVIDIA_CORE_H

#include "../inc/nvtypes.h"
#include "../inc/nvstatus.h"
#include "../inc/nv_gsp.h"

// ============================================================================
// NVIDIA RESOURCE MANAGER CORE (CÓDIGO AISLADO DEL SO)
// Se comunica con el sistema anfitrión EXCLUSIVAMENTE vía nv_os_interface
// ============================================================================

struct nvidia_dispositivo {
    NvBool presente;
    NvU8   bus;
    NvU8   slot;
    NvU8   func;
    NvU16  vendor_id;
    NvU16  device_id;
    NvU32  chip_id;
    char   chip_name[48];

    // Espacio de Control MMIO (BAR0)
    NvU64  bar0_phys;
    NvU64  bar0_size;
    void  *bar0_virt;

    // Espacio de Memoria de Video VRAM (BAR1)
    NvU64  bar1_phys;
    NvU64  bar1_size;

    // Búfer DMA Coherente compartido con el GSP
    NvBool gsp_buffer_listo;
    NvU64  gsp_shared_phys;
    void  *gsp_shared_virt;
};

NV_STATUS nvidia_core_iniciar(void);
const struct nvidia_dispositivo *nvidia_core_obtener_dispositivo(void);
NV_STATUS nvidia_core_autodiagnostico(void);
const char *nvidia_core_estado_texto(NV_STATUS st);

#endif // NVIDIA_CORE_H
