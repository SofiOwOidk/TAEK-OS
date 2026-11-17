#ifndef CONTROLADORES_XHCI_H
#define CONTROLADORES_XHCI_H

#include <stdint.h>
#include <stddef.h>
#include "../arquitectura/x86_64/pci.h"
#include "driver_abi.h"

// Base virtual donde se mapea el espacio de registros MMIO del controlador xHCI
// Ubicado en 0xFFFFFE0004000000ULL para evitar cualquier colisión con Intel VT-d (0x2000000) o HDA (0x3000000)
#define XHCI_MMIO_VIRTUAL_BASE 0xFFFFFE0004000000ULL

// Número máximo de dispositivos y puertos gestionados
#define XHCI_MAX_SLOTS   16   /* H139: con hubs hay mas dispositivos a la vez (antes 8) */
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
    int      ratones_activos;
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
    uint64_t reportes_raton_recibidos;
    int64_t  movimiento_raton_x;
    int64_t  movimiento_raton_y;
    uint8_t  botones_raton;
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
/* BSP: evita enumeración, resets y comandos síncronos durante tareas con
 * dependencias. Los cambios de puerto se atienden en el siguiente sondeo normal. */
void xhci_configurar_sondeo_breve(int activo);

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

// Ejecuta una transferencia Bulk síncrona en un endpoint de datos (IN o OUT)
int  xhci_transferencia_bulk(uint8_t slot_id, uint8_t ep_dci, void *buffer, uint64_t buffer_fisica, uint32_t longitud, int es_in, int timeout_ms);
/* Recupera exclusivamente un Bulk Halt: Reset EP, CLEAR_FEATURE, Set Dequeue. */
int xhci_bulk_limpiar_halt(uint8_t slot_id,uint8_t ep_dci);

// Transferencia Bulk multi-TRB: reparte la fase de datos en TRBs de a lo sumo
// 64 KiB (limite de 17 bits del Normal TRB) encolados bajo un unico timbre. Un
// `primer_tramo` opcional divide TRBs, sin IOC intermedio ni otro TD.
// `primer_tramo` en bytes; 0 usa bloques uniformes. Entrega los ciclos TSC del
// evento final en ambos punteros no NULL: DATA completo, no primer byte.
// Soporta hasta 256 KiB.
int  xhci_transferencia_bulk_segmentada(uint8_t slot_id, uint8_t ep_dci,
                                        void *buffer, uint64_t buffer_fisica,
                                        uint32_t longitud, int es_in,
                                        uint32_t primer_tramo, int timeout_ms,
                                        uint64_t *ciclos_primer_evento,
                                        uint64_t *ciclos_fin_evento);

// Contadores de atribucion acumulados durante esperas Bulk (TSC, sin I/O).
uint64_t xhci_telemetria_espera_ciclos(void);   // CPU ociosa en esperar_milisegundos
uint64_t xhci_telemetria_sondeo_ciclos(void);   // MMIO/eventos bombeados
uint64_t xhci_telemetria_timbre_ciclos(void);   // doorbell (incluye traza serial)
void     xhci_telemetria_reiniciar(void);

// Despliega un volcado de diagnóstico forense de los registros y anillos xHCI
void xhci_imprimir_diagnostico_completo(void);
int xhci_hid_mouse_tomar(void);
int xhci_hid_mouse_leer(struct taek_hid_mouse_reporte *);
void xhci_hid_mouse_liberar(void);
uint64_t xhci_hid_mouse_perdidos(void);

/* Diagnostico: cada transferencia de un endpoint HID tal como llega (cualquier codigo), para ver que manda un
 * raton o teclado real. Solo copia; no cambia nada de la recepcion normal. */
struct xhci_hid_crudo { uint64_t ms; uint8_t slot, dci, cc, len, iface, es_raton, confirmado, datos[8]; };
void xhci_hid_crudo_activar(int activo);
int xhci_hid_crudo_leer(struct xhci_hid_crudo *out);
uint64_t xhci_hid_crudo_perdidos(void);
void xhci_hid_listar(void);

/* Hubs USB (H139): 'usb hub' lista los hubs, sus puertos y lo que cuelga de cada uno. */
void xhci_hub_listar(void);

/* H204, Fase 0 del gestor de hilos: lo que pasa en el anillo de eventos y en los puertos raiz. Solo mide. */
enum {
    XHCI_SUCESO_EVENTO = 1,       /* Port Status Change Event: PORTSC leido al sacar el evento del anillo */
    XHCI_SUCESO_CONEXION = 2,     /* El escaneo vio CCS de 0 a 1 */
    XHCI_SUCESO_DESCONEXION = 3,  /* El escaneo vio CCS de 1 a 0 */
    XHCI_SUCESO_SIN_ENLACE = 4,   /* Reset sin PED (el aviso de siempre); se programa el reintento 1 */
    XHCI_SUCESO_RECUPERADO = 5,   /* Un reintento dejo el enlace habilitado */
    XHCI_SUCESO_REINTENTO_FALLO = 6,
    XHCI_SUCESO_PERDIDO = 7,      /* Tres reintentos sin enlace: hay que desenchufar y volver a enchufar */
};
#define XHCI_SUCESOS 8
struct xhci_suceso_puerto { uint64_t ms; uint32_t portsc; uint8_t puerto, tipo, intento, reservado; };
struct xhci_fase0 {
    uint32_t reentradas;          /* Pasadas por el anillo que empezaron con otra en curso (llamada anidada) */
    uint32_t eventos_max;         /* Mas eventos sacados en una pasada; el anillo tiene XHCI_TAM_ANILLO */
    uint32_t eventos_hc;          /* Host Controller Events */
    uint32_t anillo_lleno;        /* ... con Event Ring Full Error (codigo 21) */
    uint32_t cambios_puerto;      /* Port Status Change Events */
    uint32_t reintentos, recuperados, perdidos;
    uint64_t teclas_perdidas;     /* Teclas que no cupieron en el bufer del teclado USB */
    uint64_t reentrada_desde;     /* Direccion de retorno de la primera llamada anidada (solo para la consola) */
    uint32_t sucesos_total;       /* Sucesos de puerto desde el arranque; los ultimos XHCI_SUCESOS estan en sucesos */
    struct xhci_suceso_puerto sucesos[XHCI_SUCESOS];
};
const struct xhci_fase0 *xhci_fase0(void);
/* Copia los ultimos sucesos del mas viejo al mas nuevo; devuelve cuantos */
unsigned xhci_fase0_sucesos(struct xhci_suceso_puerto *out, unsigned max);
void xhci_fase0_cero(void);

/* H208, Fase B: hilo de tiempo real en CPU 1 que bombea el anillo de eventos cada ~2 ms. 1 enciende, 0 apaga;
 * devuelve el modo resultante, o -1 si no se pudo crear el hilo. */
int xhci_hilo(int encender);
void xhci_hilo_estado(int *modo, uint64_t *bombeos, uint64_t *ocupado);
/* H210, Fase B2: pasadas del hilo de puertos (escaneo, hubs, reintentos y enumeracion en CPU 1) */
uint64_t xhci_hilo_puertos_pasadas(void);

#endif // CONTROLADORES_XHCI_H
