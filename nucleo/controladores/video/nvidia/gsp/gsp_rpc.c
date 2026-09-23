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

    // 3. Polling sincrónico con timeout en hardware real:
    // Esperar a que el coprocesador GSP (Falcon / RISC-V) procese el comando y avance la cola de estado (g_stat_queue->cola)
    uint32_t timeout_ms = 500; // 500 ms de tiempo límite para silicio
    uint32_t cola_inicio = g_stat_queue->cola;
    int respondio = 0;

    while (timeout_ms > 0) {
        dma_sincronizar_dispositivo_a_cpu(g_stat_queue, sizeof(gsp_cola_circular_t));
        if (g_stat_queue->cola != cola_inicio) {
            respondio = 1;
            break;
        }
        esperar_milisegundos(2);
        timeout_ms -= 2;
    }

    if (!respondio) {
        g_rpc_errores++;
        serial_imprimir("[GSP RPC TIMEOUT] El silicio no respondió al comando RPC 0x");
        serial_imprimir_hex(comando);
        serial_imprimir_linea(" (Coprocesador GSP inactivo o firmware no autenticado).");
        return NV_ERR_TIMEOUT;
    }

    // 4. Leer paquete de respuesta real generado por el microcódigo GSP en silicio
    uint32_t offset_stat = cola_inicio & g_stat_queue->mascara;
    gsp_paquete_rpc_t *respuesta = (gsp_paquete_rpc_t *)&g_stat_queue->datos[offset_stat];

    g_rpc_recibidos++;

    // 5. Devolver datos al llamador
    if (resp && len_out) {
        uint32_t bytes_a_copiar = respuesta->longitud;
        if (bytes_a_copiar > *len_out) bytes_a_copiar = *len_out;
        if (bytes_a_copiar > 0) {
            memcpy(resp, respuesta->payload, bytes_a_copiar);
        }
        *len_out = bytes_a_copiar;
    }

    return respuesta->estado;
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

    if (!dev || !dev->presente) {
        serial_imprimir_linea("[AVISO] Silicio GPU NVIDIA ausente en bus PCI. Omitiendo transacciones RPC de hardware.");
        return 0;
    }

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
        serial_imprimir_linea("[FALLO] Silicio no respondió a comando RPC NOOP.");
        return -2;
    }

    // 2. Enviar comando INITIALIZE
    uint32_t abi_ver = 0;
    uint32_t len_abi = sizeof(abi_ver);
    st = gsp_rpc_enviar_sincrono(GSP_RPC_CMD_INITIALIZE, NULL, 0, &abi_ver, &len_abi);
    if (st != NV_OK || abi_ver == 0) {
        serial_imprimir_linea("[FALLO] Silicio no respondió a comando RPC INITIALIZE.");
        return -3;
    }

    // 3. Consultar capacidades mediante RPC
    gsp_gpu_capacidades_t caps;
    st = gsp_rpc_obtener_capacidades(&caps);
    if (st != NV_OK) {
        serial_imprimir_linea("[FALLO] Fallo al extraer capacidades de hardware vía RPC.");
        return -4;
    }

    serial_imprimir("[OK] Canal RPC GSP verificado con silicio: ");
    serial_imprimir(caps.nombre_gpu);
    serial_imprimir_linea("");

    return 0;
}
