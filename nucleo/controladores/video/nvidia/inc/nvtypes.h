#ifndef NVTYPES_H
#define NVTYPES_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// NVIDIA OPEN GPU KERNEL MODULES - TIPOS INDEPENDIENTES DE SO
// Aislado del kernel de TAEK OS para portabilidad directa
// ============================================================================

typedef uint8_t   NvU8;
typedef uint16_t  NvU16;
typedef uint32_t  NvU32;
typedef uint64_t  NvU64;

typedef int8_t    NvS8;
typedef int16_t   NvS16;
typedef int32_t   NvS32;
typedef int64_t   NvS64;

typedef uint8_t   NvBool;
#define NV_TRUE   1
#define NV_FALSE  0

typedef uint32_t  NvHandle;
typedef uint32_t  NvV32;
typedef uint64_t  NvP64;

#endif // NVTYPES_H
