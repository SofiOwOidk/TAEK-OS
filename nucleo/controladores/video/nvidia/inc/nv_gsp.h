#ifndef NV_GSP_H
#define NV_GSP_H

#include "nvtypes.h"
#include "nvstatus.h"

// ============================================================================
// NVIDIA GSP (GPU SYSTEM PROCESSOR) - PROTOCOLO RPC DE COMUNICACIÓN
// Arquitectura para GeForce RTX 5000 (Blackwell), RTX 4000 (Ada), RTX 3000 (Ampere)
// ============================================================================

#define NV_PMC_BOOT_0                     0x00000000
#define NV_PMC_BOOT_0_ARCHITECTURE_MASK   0x1FF00000
#define NV_PMC_BOOT_0_ARCH_BLACKWELL      0x19000000  // RTX 5070 Ti / GB20x
#define NV_PMC_BOOT_0_ARCH_ADA            0x17000000  // RTX 4000 / AD10x
#define NV_PMC_BOOT_0_ARCH_AMPERE         0x16000000  // RTX 3000 / GA10x

#define GSP_FW_SIGNATURE 0x00505347ULL // "GSP\0"

#define GSP_RPC_CMD_NOOP                  0x00000000
#define GSP_RPC_CMD_INITIALIZE            0x00000001
#define GSP_RPC_CMD_GET_CAPS              0x00000002
#define GSP_RPC_CMD_ALLOC_MEMORY          0x00000003

struct nv_gsp_mensaje_rpc {
    NvU32 comando;
    NvU32 longitud;
    NvU32 secuencia;
    NV_STATUS estado;
    NvU8  payload[256];
};

#endif // NV_GSP_H
