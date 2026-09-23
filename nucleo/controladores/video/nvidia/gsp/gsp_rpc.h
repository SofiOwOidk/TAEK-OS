#ifndef GSP_RPC_H
#define GSP_RPC_H

#include "../inc/nvtypes.h"
#include "../inc/nvstatus.h"
#include "../inc/nv_gsp.h"

// ============================================================================
// NVIDIA GSP RPC & MESSAGE QUEUES (HITO 19)
// Canal de mensajería RPC sobre colas circulares en memoria DMA compartida
// ============================================================================

#define GSP_COLA_TAMANO 0x10000 // 64 KiB por cola

// Registros Falcon / GSP Mailbox en MMIO
#define NV_PFALCON_FALCON_MAILBOX0 0x00110040
#define NV_PFALCON_FALCON_MAILBOX1 0x00110044
#define NV_PFALCON_FALCON_CPUCTL   0x00110100

// Comandos del protocolo RPC con GSP
#define GSP_RPC_CMD_NOOP           0x00000000
#define GSP_RPC_CMD_INITIALIZE     0x00000001
#define GSP_RPC_CMD_GET_CAPS       0x00000002
#define GSP_RPC_CMD_ALLOC_MEMORY   0x00000003
#define GSP_RPC_CMD_CREATE_CHANNEL 0x00000004
#define GSP_RPC_CMD_UNLOAD         0x00000005

// Encabezado y búfer de cola circular compartida en RAM
typedef struct {
    volatile NvU32 cabeza; // Head (Índice de lectura)
    volatile NvU32 cola;   // Tail (Índice de escritura)
    NvU32 tamano;          // Tamaño total en bytes
    NvU32 mascara;         // Máscara binaria para avance circular
    NvU8  datos[GSP_COLA_TAMANO - 16];
} __attribute__((aligned(64))) gsp_cola_circular_t;

// Paquete de mensaje RPC
typedef struct {
    NvU32 comando;
    NvU32 secuencia;
    NV_STATUS estado;
    NvU32 longitud;
    NvU8  payload[256];
} gsp_paquete_rpc_t;

// Estructura de capacidades de hardware retornada por el GSP
typedef struct {
    char   nombre_gpu[48];
    NvU32  arquitectura_familia; // 0x190 = Blackwell GB20x
    NvU64  vram_total_bytes;     // 16 GiB
    NvU32  vram_bus_width;       // 256 bits
    NvU32  sm_count;             // 70 Streaming Multiprocessors
    NvU32  cuda_cores;           // 8,960 CUDA Cores
    NvU32  motores_mascara;      // 3D, Compute, Copy 0..3, NVDEC, RT Cores Gen 5
    NvU32  reloj_base_mhz;       // 2,160 MHz
    NvU32  reloj_boost_mhz;      // 2,520 MHz
} gsp_gpu_capacidades_t;

struct nvidia_dispositivo; // Declaración adelantada

// Inicializa las colas circulares compartidas y la señalización Mailbox
NV_STATUS gsp_rpc_iniciar(struct nvidia_dispositivo *dev);

// Envía un comando RPC sincrónico y espera la respuesta del GSP
NV_STATUS gsp_rpc_enviar_sincrono(NvU32 comando, const void *payload, NvU32 len_in, void *resp, NvU32 *len_out);

// Obtiene las métricas de tráfico del canal RPC
void      gsp_rpc_obtener_estadisticas(NvU32 *enviados, NvU32 *recibidos, NvU32 *errores);

// Consulta al GSP las capacidades del silicio GPU (VRAM, SMs, Relojes)
NV_STATUS gsp_rpc_obtener_capacidades(gsp_gpu_capacidades_t *caps);

// Autodiagnóstico del canal RPC de comunicación con GSP (Hito 19)
int       gsp_rpc_autodiagnostico(struct nvidia_dispositivo *dev);

#endif // GSP_RPC_H
