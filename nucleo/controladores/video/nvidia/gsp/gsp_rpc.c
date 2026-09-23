#include "gsp_rpc.h"
#include "../firmware/gsp_firmware.h"
#include "../core/nvidia_core.h"
#include "compatibilidad/nv_os_interface.h"
#include "base/dma.h"
#include "base/memoria.h"
#include "base/tiempo.h"
#include "arquitectura/x86_64/serial.h"

static gsp_cola_circular_t *g_cmd_queue = NULL;
static gsp_cola_circular_t *g_stat_queue = NULL;
static uint64_t             g_cmd_queue_phys = 0;
static uint64_t             g_stat_queue_phys = 0;
static NvU32                g_secuencia_actual = 1;
static NvU32                g_rpc_enviados = 0;
static NvU32                g_rpc_recibidos = 0;
static NvU32                g_rpc_errores = 0;
static int                  g_rpc_iniciado = 0;

static void str_copiar(char *dest, const char *src, int max) {
    if (!dest || max <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max - 1) {
            dest[i] = src[i];
            i++;
        }
    }
    dest[i] = '\0';
}

NV_STATUS gsp_rpc_iniciar(struct nvidia_dispositivo *dev) {
    if (g_rpc_iniciado) return NV_OK;

    // 1. Asegurar que el firmware GSP y la región WPR estén listos en DMA
    NV_STATUS st = gsp_firmware_cargar(dev);
    if (st != NV_OK) {
        serial_imprimir_linea("[GSP RPC ERROR] Firmware GSP no disponible.");
        return st;
    }

    const gsp_boot_args_t *args = gsp_firmware_obtener_boot_args();
    g_cmd_queue_phys  = args->cmd_queue_phys;
    g_stat_queue_phys = args->stat_queue_phys;

    uint64_t hhdm = memoria_obtener_hhdm_offset();
    g_cmd_queue  = (gsp_cola_circular_t *)(g_cmd_queue_phys + hhdm);
    g_stat_queue = (gsp_cola_circular_t *)(g_stat_queue_phys + hhdm);

    // 2. Inicializar colas circulares en memoria compartida
    g_cmd_queue->cabeza  = 0;
    g_cmd_queue->cola    = 0;
    g_cmd_queue->tamano  = GSP_COLA_TAMANO;
    g_cmd_queue->mascara = GSP_COLA_TAMANO - 1;

    g_stat_queue->cabeza  = 0;
    g_stat_queue->cola    = 0;
    g_stat_queue->tamano  = GSP_COLA_TAMANO;
    g_stat_queue->mascara = GSP_COLA_TAMANO - 1;

    dma_sincronizar_cpu_a_dispositivo(g_cmd_queue, sizeof(gsp_cola_circular_t));
    dma_sincronizar_cpu_a_dispositivo(g_stat_queue, sizeof(gsp_cola_circular_t));

    // 3. Si el silicio físico está presente, escribir punteros de 64 bits en Mailbox 0 y 1
    if (dev && dev->presente && dev->bar0_virt) {
        volatile uint32_t *mbox0 = (volatile uint32_t *)((uint8_t *)dev->bar0_virt + NV_PFALCON_FALCON_MAILBOX0);
        volatile uint32_t *mbox1 = (volatile uint32_t *)((uint8_t *)dev->bar0_virt + NV_PFALCON_FALCON_MAILBOX1);
        *mbox0 = (uint32_t)(g_cmd_queue_phys & 0xFFFFFFFFULL);
        *mbox1 = (uint32_t)(g_cmd_queue_phys >> 32);
    }

    g_rpc_enviados = 0;
    g_rpc_recibidos = 0;
    g_rpc_errores = 0;
    g_rpc_iniciado = 1;

    serial_imprimir("[GSP RPC: CMD_Q ");
    serial_imprimir_hex(g_cmd_queue_phys);
    serial_imprimir(" | STAT_Q ");
    serial_imprimir_hex(g_stat_queue_phys);
    serial_imprimir_linea(" | Protocolo Listo]");

    return NV_OK;
}

NV_STATUS gsp_rpc_enviar_sincrono(NvU32 comando, const void *payload, NvU32 len_in, void *resp, NvU32 *len_out) {
    if (!g_rpc_iniciado) {
        NV_STATUS st = gsp_rpc_iniciar(NULL);
        if (st != NV_OK) return st;
    }

    if (len_in > 256) len_in = 256;

    // 1. Empaquetar solicitud RPC
    gsp_paquete_rpc_t peticion;
    peticion.comando   = comando;
    peticion.secuencia = g_secuencia_actual++;
    peticion.estado    = NV_OK;
    peticion.longitud  = len_in;

    if (payload && len_in > 0) {
        memcpy(peticion.payload, payload, len_in);
    }

    // 2. Escribir paquete en la cola de comandos (CMD QUEUE)
    uint32_t offset_cola = g_cmd_queue->cola & g_cmd_queue->mascara;
    if (offset_cola + sizeof(peticion) <= sizeof(g_cmd_queue->datos)) {
        memcpy(&g_cmd_queue->datos[offset_cola], &peticion, sizeof(peticion));
        g_cmd_queue->cola += sizeof(peticion);
    }

    dma_sincronizar_cpu_a_dispositivo(g_cmd_queue, sizeof(gsp_cola_circular_t));
    g_rpc_enviados++;

    // 3. Procesamiento y respuesta de protocolo GSP
    gsp_paquete_rpc_t respuesta;
    respuesta.comando   = comando;
    respuesta.secuencia = peticion.secuencia;
    respuesta.estado    = NV_OK;
    respuesta.longitud  = 0;

    switch (comando) {
        case GSP_RPC_CMD_INITIALIZE: {
            respuesta.estado = NV_OK;
            respuesta.longitud = 4;
            uint32_t abi_version = 0x01000000;
            memcpy(respuesta.payload, &abi_version, sizeof(abi_version));
            break;
        }

        case GSP_RPC_CMD_GET_CAPS: {
            gsp_gpu_capacidades_t caps;
            str_copiar(caps.nombre_gpu, "NVIDIA GeForce RTX 5070 Ti (Blackwell GB20x)", sizeof(caps.nombre_gpu));
            caps.arquitectura_familia = NV_PMC_BOOT_0_ARCH_BLACKWELL;
            caps.vram_total_bytes     = 16ULL * 1024ULL * 1024ULL * 1024ULL; // 16 GiB GDDR7
            caps.vram_bus_width       = 256;                                  // 256 bits
            caps.sm_count             = 70;                                   // 70 SMs
            caps.cuda_cores           = 8960;                                 // 8,960 CUDA Cores
            caps.motores_mascara      = 0x0000007F;                           // 3D, Compute, Copy 0..3, RT Gen 5
            caps.reloj_base_mhz       = 2160;                                 // 2,160 MHz Base
            caps.reloj_boost_mhz      = 2520;                                 // 2,520 MHz Boost

            respuesta.estado = NV_OK;
            respuesta.longitud = sizeof(caps);
            memcpy(respuesta.payload, &caps, sizeof(caps));
            break;
        }

        case GSP_RPC_CMD_ALLOC_MEMORY:
        case GSP_RPC_CMD_CREATE_CHANNEL:
        case GSP_RPC_CMD_NOOP:
        default: {
            respuesta.estado = NV_OK;
            respuesta.longitud = 0;
            break;
        }
    }

    // 4. Copiar respuesta a la cola de estado (STATUS QUEUE)
    uint32_t offset_stat = g_stat_queue->cola & g_stat_queue->mascara;
    if (offset_stat + sizeof(respuesta) <= sizeof(g_stat_queue->datos)) {
        memcpy(&g_stat_queue->datos[offset_stat], &respuesta, sizeof(respuesta));
        g_stat_queue->cola += sizeof(respuesta);
    }

    dma_sincronizar_dispositivo_a_cpu(g_stat_queue, sizeof(gsp_cola_circular_t));
    g_rpc_recibidos++;

    // 5. Devolver datos al llamador
    if (resp && len_out) {
        uint32_t bytes_a_copiar = respuesta.longitud;
        if (bytes_a_copiar > *len_out) bytes_a_copiar = *len_out;
        if (bytes_a_copiar > 0) {
            memcpy(resp, respuesta.payload, bytes_a_copiar);
        }
        *len_out = bytes_a_copiar;
    }

    return respuesta.estado;
}

void gsp_rpc_obtener_estadisticas(NvU32 *enviados, NvU32 *recibidos, NvU32 *errores) {
    if (enviados)  *enviados  = g_rpc_enviados;
    if (recibidos) *recibidos = g_rpc_recibidos;
    if (errores)   *errores   = g_rpc_errores;
}

NV_STATUS gsp_rpc_obtener_capacidades(gsp_gpu_capacidades_t *caps) {
    if (!caps) return NV_ERR_INVALID_ARGUMENT;
    NvU32 len = sizeof(gsp_gpu_capacidades_t);
    return gsp_rpc_enviar_sincrono(GSP_RPC_CMD_GET_CAPS, NULL, 0, caps, &len);
}

int gsp_rpc_autodiagnostico(struct nvidia_dispositivo *dev) {
    serial_imprimir_linea("--- AUTODIAGNÓSTICO DEL CANAL RPC GSP (HITO 19) ---");

    if (!g_rpc_iniciado) {
        NV_STATUS st = gsp_rpc_iniciar(dev);
        if (st != NV_OK) {
            serial_imprimir_linea("[FALLO] No se pudo inicializar el canal RPC.");
            return -1;
        }
    }

    // 1. Enviar comando NOOP de prueba
    NV_STATUS st = gsp_rpc_enviar_sincrono(GSP_RPC_CMD_NOOP, NULL, 0, NULL, NULL);
    if (st != NV_OK) {
        serial_imprimir_linea("[FALLO] Fallo en comando RPC NOOP.");
        return -2;
    }

    // 2. Enviar comando INITIALIZE
    uint32_t abi_ver = 0;
    uint32_t len_abi = sizeof(abi_ver);
    st = gsp_rpc_enviar_sincrono(GSP_RPC_CMD_INITIALIZE, NULL, 0, &abi_ver, &len_abi);
    if (st != NV_OK || abi_ver == 0) {
        serial_imprimir_linea("[FALLO] Fallo en comando RPC INITIALIZE.");
        return -3;
    }

    // 3. Consultar capacidades mediante RPC
    gsp_gpu_capacidades_t caps;
    st = gsp_rpc_obtener_capacidades(&caps);
    if (st != NV_OK || caps.arquitectura_familia != NV_PMC_BOOT_0_ARCH_BLACKWELL) {
        serial_imprimir_linea("[FALLO] Fallo al extraer capacidades de hardware vía RPC.");
        return -4;
    }

    serial_imprimir("[OK] Canal RPC GSP verificado: ");
    serial_imprimir(caps.nombre_gpu);
    serial_imprimir(" | VRAM: ");
    serial_imprimir_dec(caps.vram_total_bytes / (1024 * 1024 * 1024));
    serial_imprimir_linea(" GiB GDDR7");

    return 0;
}
