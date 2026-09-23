#ifndef NVSTATUS_H
#define NVSTATUS_H

#include "nvtypes.h"

// ============================================================================
// NVIDIA OPEN GPU KERNEL MODULES - CÓDIGOS DE ESTADO (NV_STATUS)
// ============================================================================

typedef NvU32 NV_STATUS;

#define NV_OK                            0x00000000
#define NV_ERR_GENERIC                   0x00000001
#define NV_ERR_NO_MEMORY                 0x00000002
#define NV_ERR_INVALID_ARGUMENT          0x00000003
#define NV_ERR_INVALID_STATE             0x00000004
#define NV_ERR_NOT_SUPPORTED             0x00000005
#define NV_ERR_TIMEOUT                   0x00000006
#define NV_ERR_GPU_IS_LOST               0x00000007
#define NV_ERR_BUSY                      0x00000008
#define NV_ERR_INSUFFICIENT_PERMISSIONS  0x00000009
#define NV_ERR_CARD_NOT_PRESENT          0x0000000A

#endif // NVSTATUS_H
