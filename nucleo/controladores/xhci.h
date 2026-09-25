#ifndef CONTROLADORES_XHCI_H
#define CONTROLADORES_XHCI_H

#include <stdint.h>
#include <stddef.h>
#include "../arquitectura/x86_64/pci.h"

// Base virtual donde se mapea el espacio de registros MMIO del controlador xHCI
// Ubicado en 0xFFFFFE0004000000ULL para evitar cualquier colisión con Intel VT-d (0x2000000) o HDA (0x3000000)
#define XHCI_MMIO_VIRTUAL_BASE 0xFFFFFE0004000000ULL

// Número máximo de dispositivos y puertos gestionados
#define XHCI_MAX_SLOTS   8
#define XHCI_MAX_PUERTOS 32
#define XHCI_TAM_ANILLO  64

// Estructura de un bloque de petición de transferencia (TRB - Transfer Request Block)
struct __attribute__((packed)) trb_xhci {
    uint64_t parametro;
    uint32_t estado;
    uint32_t control;
};

// Estructura de entrada en la tabla de segmentos del anillo de eventos (ERST)
struct __attribute__((packed)) erst_entrada_xhci {
    uint64_t dir_anillo_fisica;
    uint32_t tamano_anillo;
    uint32_t reservado;
};

#define XHCI_MAX_TECLADO_EPS 16
#define XHCI_TAM_BUFFER_REPORTE 1024

// Estado público del subsistema xHCI
struct estado_xhci {
    int      controlador_detectado;
    int      inicializado;
    uint8_t  bus;
    uint8_t  ranura;
    uint8_t  funcion;
    uint16_t id_proveedor;
    uint16_t id_dispositivo;
    uint64_t dir_fisica_mmio;
    uint64_t dir_virtual_mmio;
    uint32_t tamano_mmio;
    uint8_t  max_slots;
    uint8_t  max_puertos;
    uint32_t max_scratchpad_buffers;
    int      puertos_conectados;
    int      teclado_detectado;
    int      teclados_activos;
    uint8_t  teclado_slot_id;
    uint8_t  teclado_ep_dci;
    uint8_t  teclado_num_eps;
    uint8_t  teclado_puerto;
    uint8_t  ultimo_slot_tecla;
    uint8_t  ultimo_puerto_tecla;
    uint16_t teclado_id_proveedor;
    uint16_t teclado_id_producto;
    uint64_t paquetes_recibidos;
    uint64_t reportes_hid_recibidos;
    uint32_t ultimo_evento_trb_tipo;
    uint8_t  ultimo_caracter;
    uint8_t  etapa_enumeracion;       // 1=slot, 2=address, 3=device desc, 4=config desc, 5=HID, 6=endpoints, 7=recepción
    uint8_t  ultima_peticion_control;
    uint8_t  ultimo_codigo_control;
    uint16_t ultimo_valor_control;
    uint16_t ultimo_indice_control;
    uint16_t ultimo_largo_control;
    uint8_t  ultimo_codigo_transfer;
    uint8_t  ultimo_dci_transfer;
    uint8_t  ultimo_tamano_reporte;
    uint8_t  ultimo_reporte[16];
    uint32_t transferencias_control;
    uint32_t fallos_control;
    uint32_t eventos_transferencia;
    uint32_t fallos_transferencia;
};

// --- API PÚBLICA DEL CONTROLADOR USB XHCI (ANILLO 0 EN ESPAÑOL) ---

// Inicializa el controlador xHCI, negocia la cesión BIOS/OS y configura anillos DMA
int  xhci_iniciar(void);

// Indica si hay datos pendientes de leer en el búfer de teclado USB
int  xhci_hay_datos(void);

// Lee un carácter ASCII decodificado desde el teclado USB (no bloqueante, retorna 0 si no hay)
char xhci_leer_caracter(void);

// Sondea activamente el anillo de eventos de xHCI para capturar eventos USB pendientes
void xhci_sondeo(void);

// Devuelve el estado actual de detección e inicialización de xHCI
const struct estado_xhci *xhci_obtener_estado(void);

// Obtiene el estado físico de un puerto raíz específico (1..max_puertos)
int  xhci_obtener_info_puerto(uint8_t puerto, uint32_t *portsc_out, int *conectado_out, int *habilitado_out, uint8_t *velocidad_out);

// Re-escanea puertos en caliente (hotplug / encendido tardío de MCU)
int  xhci_escanear_puertos_pendientes(void);

// Detecta cambios de conexión/desconexión en puertos raíz y notifica en pantalla
int  xhci_escanear_cambios_puertos(int verbose);

// Fuerza un ciclo de reset oficial en un puerto específico y reconfigura
int  xhci_forzar_reset_puerto(uint8_t puerto);

// Despliega un volcado de diagnóstico forense de los registros y anillos xHCI
void xhci_imprimir_diagnostico_completo(void);

#endif // CONTROLADORES_XHCI_H
