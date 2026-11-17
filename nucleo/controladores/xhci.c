#include "xhci.h"
#include "hid_descriptor.h"
#include "teclado.h"
#include "xhci_bulk_plan.h"
#include "usb_msc.h"
#include "consola.h"
#include "../arquitectura/x86_64/puertos.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/paginacion.h"
#include "../base/dma.h"
#include "../base/tiempo.h"
#include "../base/memoria.h"
#include "../base/huevo.h"
#include "../base/atencion.h"
#include "../base/cerrojo.h"
#include "../base/hilos.h"
#ifndef TAEK_XHCI_TRAZA_IO
#define TAEK_XHCI_TRAZA_IO 0
#endif

// --- REGISTROS MMIO XHCI ---
#define REG_CAPLENGTH      0x00
#define REG_HCIVERSION     0x02
#define REG_HCSPARAMS1     0x04
#define REG_HCSPARAMS2     0x08
#define REG_HCSPARAMS3     0x0C
#define REG_HCCPARAMS1     0x10
#define REG_DBOFF          0x14
#define REG_RTSOFF         0x18

// Registros Operacionales (relativos a op_base = base + CAPLENGTH)
#define REG_OP_USBCMD      0x00
#define REG_OP_USBSTS      0x04
#define REG_OP_PAGESIZE    0x08
#define REG_OP_DNCTRL      0x14
#define REG_OP_CRCR        0x18
#define REG_OP_DCBAAP      0x30
#define REG_OP_CONFIG      0x38
#define REG_OP_PORTSC_BASE 0x400

// Banderas de USBCMD
#define USBCMD_RS          (1U << 0)  // Run/Stop
#define USBCMD_HCRST       (1U << 1)  // Host Controller Reset
#define USBCMD_INTE        (1U << 2)  // Interrupter Enable

// Banderas de USBSTS
#define USBSTS_HCH         (1U << 0)  // Host Controller Halted
#define USBSTS_EINT        (1U << 3)  // Event Interrupt
#define USBSTS_PCD         (1U << 4)  // Port Change Detect
#define USBSTS_CNR         (1U << 11) // Controller Not Ready

// Banderas de PORTSC
#define PORTSC_CCS         (1U << 0)  // Current Connect Status
#define PORTSC_PED         (1U << 1)  // Port Enabled/Disabled
#define PORTSC_PR          (1U << 4)  // Port Reset
#define PORTSC_PLS_MASK    (0xFU << 5)// Port Link State
#define PORTSC_PP          (1U << 9)  // Port Power
#define PORTSC_SPEED_MASK  (0xFU << 10)
#define PORTSC_PRC         (1U << 21) // Port Reset Change (R/W1C)
#define PORTSC_CSC         (1U << 17) // Connect Status Change (R/W1C)
#define PORTSC_PEC         (1U << 18) // Port Enabled/Disabled Change (R/W1C)

// Máscaras neutras idénticas al kernel de Linux (drivers/usb/host/xhci.h xhci_port_state_to_neutral)
#define XHCI_PORT_RO       ((1U << 0) | (1U << 3) | (0xFU << 10) | (1U << 30))
#define XHCI_PORT_RWS      ((0xFU << 5) | (1U << 9) | (0x3U << 14) | (0x7U << 25))

// Esta función preserva los bits de estado de solo lectura (RO) y de configuración estática (RWS),
// forzando a cero todos los bits de cambio R/W1C (PED, CSC, PEC, PRC) y los disparadores de reset
static inline uint32_t xhci_portsc_neutral(uint32_t val) {
    return (val & XHCI_PORT_RO) | (val & XHCI_PORT_RWS);
}

// Tipos de TRB
#define TRB_TIPO_NORMAL        1
#define TRB_TIPO_SETUP_STAGE   2
#define TRB_TIPO_DATA_STAGE    3
#define TRB_TIPO_STATUS_STAGE  4
#define TRB_TIPO_LINK          6
#define TRB_TIPO_ENABLE_SLOT   9
#define TRB_TIPO_DISABLE_SLOT  10
#define TRB_TIPO_ADDRESS_DEV   11
#define TRB_TIPO_CONFIG_EP     12
#define TRB_TIPO_EVAL_CTX      13
#define TRB_TIPO_RESET_EP      14
#define TRB_TIPO_STOP_EP       15
#define TRB_TIPO_SET_DEQUEUE   16
#define TRB_TIPO_TRANSFER_EVT  32
#define TRB_TIPO_CMD_COMP_EVT  33
#define TRB_TIPO_PORT_STATUS   34

// Estructura de Descriptores USB Estándar
struct __attribute__((packed)) usb_descriptor_cabecera {
    uint8_t  longitud;
    uint8_t  tipo_descriptor;
};

struct __attribute__((packed)) usb_descriptor_dispositivo {
    uint8_t  longitud;
    uint8_t  tipo_descriptor;
    uint16_t bcd_usb;
    uint8_t  clase_dispositivo;
    uint8_t  subclase_dispositivo;
    uint8_t  protocolo_dispositivo;
    uint8_t  tamano_max_paquete_ep0;
    uint16_t id_proveedor;
    uint16_t id_producto;
    uint16_t bcd_dispositivo;
    uint8_t  indice_fabricante;
    uint8_t  indice_producto;
    uint8_t  indice_numero_serie;
    uint8_t  num_configuraciones;
};

struct __attribute__((packed)) usb_descriptor_configuracion {
    uint8_t  longitud;
    uint8_t  tipo_descriptor;
    uint16_t longitud_total;
    uint8_t  num_interfaces;
    uint8_t  valor_configuracion;
    uint8_t  indice_configuracion;
    uint8_t  atributos;
    uint8_t  consumo_max;
};

struct __attribute__((packed)) usb_descriptor_interfaz {
    uint8_t  longitud;
    uint8_t  tipo_descriptor;
    uint8_t  numero_interfaz;
    uint8_t  ajuste_alternativo;
    uint8_t  num_endpoints;
    uint8_t  clase_interfaz;
    uint8_t  subclase_interfaz;
    uint8_t  protocolo_interfaz;
    uint8_t  indice_interfaz;
};

struct __attribute__((packed)) usb_descriptor_endpoint {
    uint8_t  longitud;
    uint8_t  tipo_descriptor;
    uint8_t  direccion_endpoint;
    uint8_t  atributos;
    uint16_t tamano_max_paquete;
    uint8_t  intervalo;
};

struct __attribute__((packed)) usb_paquete_control {
    uint8_t  tipo_peticion;
    uint8_t  peticion;
    uint16_t valor;
    uint16_t indice;
    uint16_t longitud;
};

// --- ESTADO GLOBAL DEL CONTROLADOR ---
static struct estado_xhci g_estado = {0};

// Punteros base de registros MMIO
static uint64_t g_mmio_base = 0;
static uint64_t g_op_base   = 0;
static uint64_t g_rts_base  = 0;
static uint64_t g_db_base   = 0;

static uint8_t  g_cap_length = 0;
static uint8_t  g_tamano_contexto = 32; // 32 o 64 bytes según HCCPARAMS1.CSZ

// Estructuras DMA del controlador
static uint64_t *g_dcbaa = NULL;
static uint64_t  g_dcbaa_fisica = 0;

// Scratchpad Buffers (xHCI 1.2 §4.20 y §6.1, inspirado en Stellux xHCI)
static uint32_t  g_max_scratchpad_buffers = 0;
static uint64_t *g_scratchpad_array = NULL;
static uint64_t  g_scratchpad_array_fisica = 0;
static void     *g_scratchpad_pages = NULL;
static uint64_t  g_scratchpad_pages_fisica = 0;

// Command Ring
static volatile struct trb_xhci *g_cmd_ring = NULL;
static uint64_t                  g_cmd_ring_fisica = 0;
static uint32_t                  g_cmd_idx = 0;
static uint8_t                   g_cmd_cycle = 1;
static uint8_t                   g_cmd_ring_disponible = 1;

// Event Ring
static volatile struct trb_xhci *g_event_ring = NULL;
static uint64_t                  g_event_ring_fisica = 0;
static struct erst_entrada_xhci *g_erst = NULL;
static uint64_t                  g_erst_fisica = 0;
static uint32_t                  g_event_idx = 0;
static uint8_t                   g_event_cycle = 1;
struct xhci_evento_pendiente {
    struct trb_xhci trb;
    uint8_t listo;
};
static struct xhci_evento_pendiente g_evento_comando;
static struct xhci_evento_pendiente g_eventos_transfer[XHCI_MAX_SLOTS + 1][32];
static uint8_t g_cambio_puerto_pendiente;

// --- TELEMETRIA DE TRANSFERENCIAS BULK (sin salida en el camino caliente) ---
// Captura con marca TSC del primer evento de cada TRB de una transferencia
// segmentada, para fechar la finalizacion del DATA completo.
#define XHCI_CAPTURA_EVENTOS_MAX 8
// Longitud maxima por TRB Normal (campo Transfer Length de 17 bits: 131071).
// Se usa 64 KiB por alineacion y margen; el resto se reparte en varios TRBs.
#define XHCI_BULK_TRAMO_MAX 65536U
struct xhci_captura_bulk {
    int      activo;
    uint8_t  slot;
    uint8_t  dci;
    uint32_t esperados;
    uint32_t recibidos;
    uint64_t ciclos[XHCI_CAPTURA_EVENTOS_MAX];
    uint8_t  codigos[XHCI_CAPTURA_EVENTOS_MAX];
    uint64_t trbs[XHCI_BULK_PLAN_MAX];
    uint32_t cantidad_trbs;
    int terminado, fallo;
    uint64_t trb_error;
};
static struct xhci_captura_bulk g_captura_bulk;

static uint64_t g_bulk_ciclos_espera = 0; // CPU ociosa en esperar_milisegundos
static uint64_t g_bulk_ciclos_sondeo = 0; // MMIO/eventos en xhci_bombear_eventos
static uint64_t g_ciclos_timbre = 0;      // doorbell (incluye su traza serial)

// Transfer Ring para EP0 dedicado por cada ranura (Slot 1..XHCI_MAX_SLOTS)
static volatile struct trb_xhci *g_slot_ep0_ring[XHCI_MAX_SLOTS + 1] = {0};
static uint64_t                  g_slot_ep0_ring_fisica[XHCI_MAX_SLOTS + 1] = {0};
static uint32_t                  g_slot_ep0_idx[XHCI_MAX_SLOTS + 1] = {0};
static uint8_t                   g_slot_ep0_cycle[XHCI_MAX_SLOTS + 1] = {0};

// Estructura para gestión de cada Endpoint de Interrupción del Teclado (Multi-Endpoint / Compuesto / Multi-Slot)
struct xhci_ep_teclado {
    uint8_t  slot_id;       // Ranura del dispositivo xHCI (1..XHCI_MAX_SLOTS)
    uint8_t  puerto_idx;    // Puerto raíz de silicio (1..XHCI_MAX_PUERTOS)
    uint8_t  ep_addr;       // Dirección de hardware (ej: 0x81, 0x82)
    uint8_t  ep_dci;        // Device Context Index en xHCI (ej: 3, 5)
    uint16_t ep_max_pkt;    // Tamaño máximo de paquete (ej: 8, 16, 64)
    uint8_t  ep_intervalo;  // Intervalo en ms
    uint8_t  iface_num;     // Número de interfaz USB
    uint8_t  es_boot;       // Interfaz HID Boot Keyboard (subclase/protocolo 1/1)
    uint8_t  es_raton;      // Interfaz HID Boot Mouse (subclase/protocolo 1/2)
    uint8_t  raton_boot_confirmado;  // Listo para entregar reportes de raton (boot o, desde H138, reporte)
    uint32_t raton_generacion;
    int      activo;
    /* H138: como Linux y Windows, el raton se deja en protocolo reporte si su descriptor de reporte se entiende */
    uint16_t hid_rep_lon;            // Largo del descriptor de reporte segun el descriptor HID de la interfaz
    uint8_t  raton_reporte;          // 1: se decodifica con 'raton', no con los 3 bytes boot
    struct hid_raton raton;

    // Anillo de transferencia DMA (Transfer Ring)
    volatile struct trb_xhci *ring;
    uint64_t                  ring_fisica;
    uint32_t                  idx;
    uint8_t                   cycle;

    // Búfer DMA para recibir reportes de este endpoint
    uint8_t         *bufer;
    uint64_t         bufer_fisica;
    uint8_t          ultimo_reporte[64];
    uint32_t         ultimo_tam;
};

static struct xhci_ep_teclado g_teclado_eps[XHCI_MAX_TECLADO_EPS];
#define MOUSE_COLA 64
static struct taek_hid_mouse_reporte g_mouse_cola[MOUSE_COLA];
static unsigned g_mouse_inicio,g_mouse_total;
static int g_mouse_propietario,g_mouse_ep=-1;
static uint32_t g_mouse_generacion,g_mouse_generacion_ep;
static uint64_t g_mouse_secuencia,g_mouse_perdidos;
static int mouse_compatible(unsigned e){
    return g_teclado_eps[e].activo && g_teclado_eps[e].es_raton && g_teclado_eps[e].raton_boot_confirmado;
}
static int mouse_buscar(void){
    for(unsigned e=0;e<XHCI_MAX_TECLADO_EPS;e++)if(mouse_compatible(e)){g_mouse_ep=(int)e;g_mouse_generacion_ep=g_teclado_eps[e].raton_generacion;return 1;}
    g_mouse_ep=-1;return 0;
}
/* H208, Fase B del gestor de hilos: un cerrojo para todo el estado del xHCI. Lo toma cada funcion publica al entrar
 * y lo suelta al salir por cualquier return (cleanup). Es recursivo por CPU: una funcion publica puede llamar a otra.
 * Quien espera una transferencia lo tiene tomado y bombea el anillo el mismo, como antes; el hilo de CPU 1 bombea
 * solo cuando esta libre (cerrojo_intentar), asi nadie espera un evento que solo el otro puede recoger. */
static cerrojo_t g_cerrojo_xhci = CERROJO("xhci");
static void xhci_soltar_exclusivo(int *p) { (void)p; cerrojo_soltar(&g_cerrojo_xhci); }
#define XHCI_EXCLUSIVO() int xhci_exclusivo_ __attribute__((cleanup(xhci_soltar_exclusivo), unused)) = (cerrojo_tomar(&g_cerrojo_xhci), 1)
static int xhci_hilos_activos(void);   /* H210: 1 si los hilos de CPU 1 atienden el USB (al final del archivo) */

int xhci_hid_mouse_tomar(void){
    if(__atomic_load_n(&g_mouse_propietario,__ATOMIC_ACQUIRE))return -1;   /* H211: anillo3 lo intenta en cada vuelta si otro lo tiene */
    XHCI_EXCLUSIVO();
    if(g_mouse_propietario)return -1;
    g_mouse_propietario=1;g_mouse_inicio=g_mouse_total=0;g_mouse_perdidos=0;return mouse_buscar();
}
void xhci_hid_mouse_liberar(void){ XHCI_EXCLUSIVO();g_mouse_propietario=0;g_mouse_inicio=g_mouse_total=0;g_mouse_ep=-1;}
uint64_t xhci_hid_mouse_perdidos(void){return g_mouse_perdidos;}
int xhci_hid_mouse_leer(struct taek_hid_mouse_reporte *out){
    /* H210: cola vacia y el mismo raton de siempre: se vuelve sin cerrojo (anillo3 lo pide 64 veces por vuelta).
     * H211: tambien sin raton encontrado todavia (mouse_encolar lo busca solo cuando llega un reporte, con el cerrojo
     * del hilo de eventos) y sin ser el dueño. Antes esos dos casos tomaban el cerrojo en cada llamada: 315 millones
     * de tomas y 49 427 esperas del hilo de eventos en 220 s de video. */
    if(xhci_hilos_activos() && out && !__atomic_load_n(&g_mouse_total,__ATOMIC_ACQUIRE)){
        if(!__atomic_load_n(&g_mouse_propietario,__ATOMIC_ACQUIRE))return -1;
        int ep=__atomic_load_n(&g_mouse_ep,__ATOMIC_ACQUIRE);
        if(ep<0)return 0;
        if(g_teclado_eps[ep].activo && g_teclado_eps[ep].raton_generacion==g_mouse_generacion_ep)return 0;
    }
    XHCI_EXCLUSIVO();
    if(!g_mouse_propietario || !out)return -1;
    if(g_mouse_ep>=0 && (!mouse_compatible((unsigned)g_mouse_ep) || g_teclado_eps[g_mouse_ep].raton_generacion!=g_mouse_generacion_ep)){
        g_mouse_inicio=g_mouse_total=0;g_mouse_ep=-1;return -2;
    }
    if(g_mouse_ep<0)mouse_buscar();
    if(!g_mouse_total)return 0;
    *out=g_mouse_cola[g_mouse_inicio];g_mouse_inicio=(g_mouse_inicio+1)%MOUSE_COLA;g_mouse_total--;return 1;
}
static void mouse_encolar(struct xhci_ep_teclado *ep,const uint8_t *data,unsigned n){
    if(!g_mouse_propietario || !ep->raton_boot_confirmado || n<3 || n>8)return;
    if(g_mouse_ep<0)mouse_buscar();
    /* H138: vale cualquier raton confirmado, no solo el primero que se encontro (con dos receptores, el segundo no movia nada) */
    if(!mouse_compatible((unsigned)(ep-g_teclado_eps)))return;
    if(g_mouse_total==MOUSE_COLA){g_mouse_inicio=(g_mouse_inicio+1)%MOUSE_COLA;g_mouse_total--;g_mouse_perdidos++;}
    struct taek_hid_mouse_reporte *r=&g_mouse_cola[(g_mouse_inicio+g_mouse_total)%MOUSE_COLA];
    memset(r,0,sizeof(*r));r->secuencia=++g_mouse_secuencia;r->dispositivo=ep->raton_generacion;r->longitud=(uint8_t)n;memcpy(r->datos,data,n);g_mouse_total++;
}

/* Un reporte de raton en formato boot: a quien tenga el raton (menu o modulo) o a los contadores de diagnostico */
static void raton_entregar(struct xhci_ep_teclado *ep, const uint8_t *d, unsigned n) {
    if (g_mouse_propietario) { mouse_encolar(ep, d, n); return; }
    if (ep->raton_boot_confirmado && n >= 3) {
        g_estado.reportes_raton_recibidos++;
        g_estado.botones_raton = d[0];
        g_estado.movimiento_raton_x += (int8_t)d[1];
        g_estado.movimiento_raton_y += (int8_t)d[2];
    }
}

/* Copia cruda de cada transferencia HID para 'raton crudo'. Solo lectura del bufer ya recibido. */
#define CRUDO_COLA 64
static struct xhci_hid_crudo g_crudo[CRUDO_COLA];
static unsigned g_crudo_inicio, g_crudo_total;
static int g_crudo_activo;
static uint64_t g_crudo_perdidos;
void xhci_hid_crudo_activar(int activo) { XHCI_EXCLUSIVO(); g_crudo_activo = activo; g_crudo_inicio = g_crudo_total = 0; g_crudo_perdidos = 0; }
uint64_t xhci_hid_crudo_perdidos(void) { return g_crudo_perdidos; }
int xhci_hid_crudo_leer(struct xhci_hid_crudo *out) { XHCI_EXCLUSIVO();
    if (!g_crudo_total) return 0;
    *out = g_crudo[g_crudo_inicio];
    g_crudo_inicio = (g_crudo_inicio + 1) % CRUDO_COLA;
    g_crudo_total--;
    return 1;
}
static void crudo_guardar(const struct xhci_ep_teclado *ep, uint8_t cc, const uint8_t *d, unsigned n) {
    if (!g_crudo_activo) return;
    if (g_crudo_total == CRUDO_COLA) { g_crudo_inicio = (g_crudo_inicio + 1) % CRUDO_COLA; g_crudo_total--; g_crudo_perdidos++; }
    struct xhci_hid_crudo *c = &g_crudo[(g_crudo_inicio + g_crudo_total) % CRUDO_COLA];
    memset(c, 0, sizeof(*c));
    c->ms = tiempo_obtener_milisegundos();
    c->slot = ep->slot_id; c->dci = ep->ep_dci; c->cc = cc; c->len = (uint8_t)(n > 255 ? 255 : n);
    c->iface = ep->iface_num; c->es_raton = ep->es_raton; c->confirmado = ep->raton_boot_confirmado;
    for (unsigned i = 0; i < n && i < sizeof(c->datos); i++) c->datos[i] = d[i];
    g_crudo_total++;
}

static void xhci_recontar_hid(void) {
    uint8_t teclados[XHCI_MAX_SLOTS + 1] = {0};
    uint8_t ratones[XHCI_MAX_SLOTS + 1] = {0};
    int n_teclados = 0, n_ratones = 0;
    for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
        const struct xhci_ep_teclado *ep = &g_teclado_eps[e];
        if (!ep->activo || !ep->slot_id || ep->slot_id > XHCI_MAX_SLOTS) continue;
        if (ep->es_raton) {
            if (!ratones[ep->slot_id]++) n_ratones++;
        } else if (!teclados[ep->slot_id]++) n_teclados++;
    }
    g_estado.teclados_activos = n_teclados;
    g_estado.teclado_detectado = n_teclados > 0;
    g_estado.ratones_activos = n_ratones;
}

// Estructura para gestión de Endpoints Bulk de almacenamiento masivo (USB MSC)
#define XHCI_MAX_BULK_EPS 8
struct xhci_ep_bulk {
    uint8_t  slot_id;
    uint8_t  ep_dci;
    uint16_t ep_max_pkt;
    int      es_in;
    int      activo;

    volatile struct trb_xhci *ring;
    uint64_t                  ring_fisica;
    uint32_t                  idx;
    uint8_t                   cycle;
};

static struct xhci_ep_bulk g_bulk_eps[XHCI_MAX_BULK_EPS];
static uint8_t g_puerto_estado_ccs[XHCI_MAX_PUERTOS + 1];

// Mapeo bidireccional y Device Contexts de ranuras y puertos
static uint8_t *g_slot_dev_ctx[XHCI_MAX_SLOTS + 1] = {0};
static uint64_t g_slot_dev_ctx_fisica[XHCI_MAX_SLOTS + 1] = {0};
static uint8_t  g_slot_puerto[XHCI_MAX_SLOTS + 1] = {0};
static uint8_t  g_puerto_slot[XHCI_MAX_PUERTOS + 1] = {0};
static uint16_t g_slot_vid[XHCI_MAX_SLOTS + 1] = {0};
static uint16_t g_slot_pid[XHCI_MAX_SLOTS + 1] = {0};
static uint8_t *g_teclado_dev_ctx = NULL; // Puntero de conveniencia al último dev_ctx activo
static uint64_t g_teclado_dev_ctx_fisica = 0;

/* H139, hubs. Un dispositivo detras de un hub se direcciona con su "route string" (un nibble por nivel: el puerto
 * de cada hub en el camino, xHCI 1.2 seccion 8.9) y su puerto raiz. Si es lento (Full o Low Speed) y cuelga de un
 * hub de alta velocidad, el controlador habla con el a traves del traductor (TT) de ese hub: hay que decirle que
 * hub y que puerto. xhci_configurar_puerto lee estos datos de g_enum; para un puerto raiz quedan en cero. */
static struct {
    uint32_t ruta;
    uint8_t  profundidad;   // Hubs entre la raiz y el dispositivo
    uint8_t  tt_slot, tt_puerto;
    uint8_t  slot;          // Resultado: slot asignado (0 si fallo)
} g_enum;

#define XHCI_MAX_HUBS 4
#define HUB_MAX_PUERTOS 15
struct xhci_hub {
    uint8_t  activo, slot, raiz, velocidad, puertos, es_ss, profundidad, tt_slot, tt_puerto;
    uint32_t ruta;
    uint8_t  hijo[HUB_MAX_PUERTOS + 1];   // Slot de lo que cuelga de cada puerto (0 = nada)
};
static struct xhci_hub g_hubs[XHCI_MAX_HUBS];
static void xhci_hub_iniciar(uint8_t slot_id, uint8_t velocidad, uint8_t config_val, uint8_t raiz);
static void xhci_hubs_sondear(void);

static inline int xhci_conteo_endpoints_activos(void) {
    int c = 0;
    for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
        if (g_teclado_eps[e].activo) c++;
    }
    return c;
}

// Búfer circular de caracteres listos para la terminal
#define TAM_BUFFER_TECLAS 256
static char     g_buffer_teclado[TAM_BUFFER_TECLAS];
static uint32_t g_buf_cabeza = 0;
static uint32_t g_buf_cola = 0;

/* H204, Fase 0 del gestor de hilos: contadores del anillo de eventos y bitacora de sucesos de los puertos raiz. */
static struct xhci_fase0 g_fase0;
static volatile uint32_t g_bombeo_profundidad;   /* Pasadas por el anillo en curso (mas de 1 = llamada anidada) */
const struct xhci_fase0 *xhci_fase0(void) { return &g_fase0; }
static void xhci_suceso(uint8_t puerto, uint32_t portsc, uint8_t tipo, uint8_t intento) {
    struct xhci_suceso_puerto *s = &g_fase0.sucesos[g_fase0.sucesos_total % XHCI_SUCESOS];
    s->ms = tiempo_obtener_milisegundos(); s->portsc = portsc;
    s->puerto = puerto; s->tipo = tipo; s->intento = intento; s->reservado = 0;
    g_fase0.sucesos_total++;
}
unsigned xhci_fase0_sucesos(struct xhci_suceso_puerto *out, unsigned max) { XHCI_EXCLUSIVO();
    unsigned n = g_fase0.sucesos_total < XHCI_SUCESOS ? g_fase0.sucesos_total : XHCI_SUCESOS;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; i++)
        out[i] = g_fase0.sucesos[(g_fase0.sucesos_total - n + i) % XHCI_SUCESOS];
    return n;
}
void xhci_fase0_cero(void) { XHCI_EXCLUSIVO();
    uint32_t total = g_fase0.sucesos_total;
    struct xhci_suceso_puerto copia[XHCI_SUCESOS];
    for (unsigned i = 0; i < XHCI_SUCESOS; i++) copia[i] = g_fase0.sucesos[i];
    memset(&g_fase0, 0, sizeof(g_fase0));
    /* La bitacora de sucesos no se borra: cada uno lleva su hora */
    g_fase0.sucesos_total = total;
    for (unsigned i = 0; i < XHCI_SUCESOS; i++) g_fase0.sucesos[i] = copia[i];
}
/* Reintentos del reset de puerto (H204): el numero del proximo intento (0 = ninguno) y su hora */
#define XHCI_REINTENTOS 3
static struct { uint8_t intento; uint64_t cuando_ms; } g_reintento[XHCI_MAX_PUERTOS + 1];
static void xhci_reintento_programar(uint8_t p, uint8_t intento) {
    static const uint16_t espera_ms[XHCI_REINTENTOS] = { 100, 300, 700 };
    if (p > XHCI_MAX_PUERTOS || intento < 1 || intento > XHCI_REINTENTOS) return;
    g_reintento[p].intento = intento;
    g_reintento[p].cuando_ms = tiempo_obtener_milisegundos() + espera_ms[intento - 1];
}
static void xhci_reintentos_atender(void);
/* Una linea por suceso, con los bits de PORTSC que separan una caida electrica de un fallo de enlace:
 * OCA/OCC sobrecorriente, PLS estado del enlace, CSC/PEC/PRC/PLC cambios pendientes. */
static void xhci_suceso_imprimir(uint8_t p, uint32_t portsc, const char *que) {
    consola_imprimir("[USB-F0] t="); consola_imprimir_dec(tiempo_obtener_milisegundos());
    consola_imprimir(" ms puerto "); consola_imprimir_dec(p); consola_imprimir(" "); consola_imprimir(que);
    consola_imprimir(" PORTSC="); consola_imprimir_hex(portsc);
    consola_imprimir(" CCS="); consola_imprimir_dec(portsc & 1u);
    consola_imprimir(" PED="); consola_imprimir_dec((portsc >> 1) & 1u);
    consola_imprimir(" OCA="); consola_imprimir_dec((portsc >> 3) & 1u);
    consola_imprimir(" PLS="); consola_imprimir_dec((portsc >> 5) & 0xFu);
    consola_imprimir(" PP="); consola_imprimir_dec((portsc >> 9) & 1u);
    consola_imprimir(" VEL="); consola_imprimir_dec((portsc >> 10) & 0xFu);
    consola_imprimir(" CSC="); consola_imprimir_dec((portsc >> 17) & 1u);
    consola_imprimir(" PEC="); consola_imprimir_dec((portsc >> 18) & 1u);
    consola_imprimir(" OCC="); consola_imprimir_dec((portsc >> 20) & 1u);
    consola_imprimir(" PRC="); consola_imprimir_dec((portsc >> 21) & 1u);
    consola_imprimir(" PLC="); consola_imprimir_dec((portsc >> 22) & 1u);
    consola_imprimir_linea("");
}

// --- TABLAS DE TRADUCCIÓN USB HID (BOOT PROTOCOL) A ASCII ---
static const char g_hid_a_ascii_normal[128] = {
    0, 0, 0, 0,
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 27, '\b', '\t', ' ', '-', '=', '[', ']', '\\', 0, ';', '\'', '`',
    ',', '.', '/', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    '/', '*', '-', '+', '\n', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
    '<',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const char g_hid_a_ascii_shift[128] = {
    0, 0, 0, 0,
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
    'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
    '!', '@', '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 27, '\b', '\t', ' ', '_', '+', '{', '}', '|', 0, ':', '"', '~',
    '<', '>', '?', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0,
    '/', '*', '-', '+', '\n', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '.',
    '>',
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// --- FUNCIONES AUXILIARES DE LECTURA/ESCRITURA MMIO ---
static inline uint32_t mmio_leer32(uint64_t dir) {
    uint32_t val = *(volatile uint32_t *)dir;
    __asm__ volatile ("" ::: "memory");
    return val;
}

static inline void mmio_escribir32(uint64_t dir, uint32_t val) {
    *(volatile uint32_t *)dir = val;
    __asm__ volatile ("" ::: "memory");
}

static inline uint64_t mmio_leer64(uint64_t dir) {
    uint64_t val = *(volatile uint64_t *)dir;
    __asm__ volatile ("" ::: "memory");
    return val;
}

static inline void mmio_escribir64(uint64_t dir, uint64_t val) {
    *(volatile uint64_t *)dir = val;
    __asm__ volatile ("" ::: "memory");
}

// Contadores forenses nucleares de vueltas completas (Wraparounds)
static uint32_t g_evt_ring_wraparounds = 0;
static uint32_t g_cmd_ring_wraparounds = 0;

// Tocar timbre (Doorbell) con instrumentación nuclear de telemetría y mfence
static inline void xhci_tocar_timbre(uint8_t slot, uint32_t ep_o_cmd) {
    uint64_t t_costo0 = rdtsc();
    uint64_t reg_db = g_db_base + ((uint64_t)slot * 4);
    __asm__ volatile ("mfence" ::: "memory");
#if TAEK_XHCI_TRAZA_IO
    uint64_t tsc = rdtsc();
    uint64_t ms = tiempo_obtener_milisegundos();
    serial_imprimir("  [xHCI DB] Ring: Slot=");
    serial_imprimir_dec(slot);
    serial_imprimir(" Target=");
    serial_imprimir_hex(ep_o_cmd);
    serial_imprimir(" Reg=");
    serial_imprimir_hex(reg_db);
    serial_imprimir(" TSC=");
    serial_imprimir_hex(tsc);
    serial_imprimir(" (ms=");
    serial_imprimir_dec((uint32_t)ms);
    serial_imprimir_linea(")");
#endif
    mmio_escribir32(reg_db, ep_o_cmd);
    __asm__ volatile ("mfence" ::: "memory");
    g_ciclos_timbre += rdtsc() - t_costo0;
}

static inline void xhci_sincronizar_evento_actual(void) {
    dma_sincronizar_dispositivo_a_cpu((const void *)&g_event_ring[g_event_idx], sizeof(g_event_ring[g_event_idx]));
}

static void xhci_procesar_evento_hid(const struct trb_xhci *evt);

// Único lugar que extrae un TRB y avanza ERDP. Los consumidores reciben copias.
static int xhci_extraer_evento(struct trb_xhci *salida) {
    if (!g_event_ring || !salida) return 0;
    xhci_sincronizar_evento_actual();
    volatile struct trb_xhci *actual = &g_event_ring[g_event_idx];
    if ((actual->control & 1U) != g_event_cycle) return 0;
    uint8_t tipo = (actual->control >> 10) & 0x3F;
    if (tipo == TRB_TIPO_CMD_COMP_EVT && g_evento_comando.listo) return 0;
    if (tipo == TRB_TIPO_TRANSFER_EVT) {
        uint8_t slot = (actual->control >> 24) & 0xFF;
        uint8_t dci = (actual->control >> 16) & 0x1F;
        int es_hid = 0;
        for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++)
            if (g_teclado_eps[e].activo && g_teclado_eps[e].slot_id == slot &&
                g_teclado_eps[e].ep_dci == dci) { es_hid = 1; break; }
        if (!es_hid && slot > 0 && slot <= XHCI_MAX_SLOTS &&
            g_eventos_transfer[slot][dci].listo) return 0;
    }
    salida->parametro = actual->parametro;
    salida->estado = actual->estado;
    salida->control = actual->control;
    if (++g_event_idx == XHCI_TAM_ANILLO) {
        g_event_idx = 0;
        g_event_cycle = !g_event_cycle;
        g_evt_ring_wraparounds++;
    }
    uint64_t erdp = g_event_ring_fisica + (uint64_t)g_event_idx * sizeof(struct trb_xhci);
    mmio_escribir64(g_rts_base + 0x20 + 0x18, erdp | (1U << 3));
    mmio_escribir32(g_rts_base + 0x20, 0x03);
    return 1;
}

static int xhci_capturar_bulk_evento(const struct trb_xhci *evt) {
    uint8_t slot=(evt->control>>24)&0xff,dci=(evt->control>>16)&0x1f;
    if(!g_captura_bulk.activo||g_captura_bulk.slot!=slot||g_captura_bulk.dci!=dci)return 0;
    int asociado=0;
    for(uint32_t j=0;j<g_captura_bulk.cantidad_trbs;++j)
        if(evt->parametro==g_captura_bulk.trbs[j])asociado=1;
    if(!asociado)return 1; // evento tardio de otro TD: consumir sin atribuirlo al actual
    uint8_t cc=(evt->estado>>24)&0xff;
    if(g_captura_bulk.recibidos<XHCI_CAPTURA_EVENTOS_MAX) {
        uint32_t i=g_captura_bulk.recibidos++;
        g_captura_bulk.ciclos[i]=rdtsc();g_captura_bulk.codigos[i]=cc;
    }
    int fallo=xhci_bulk_resultado_evento(cc,evt->estado&0xffffffU);
    if(fallo){g_captura_bulk.fallo=fallo;g_captura_bulk.trb_error=evt->parametro;}
    if(fallo||evt->parametro==g_captura_bulk.trbs[g_captura_bulk.cantidad_trbs-1])
        g_captura_bulk.terminado=1;
    return 1;
}
/* H204: noinline para que la direccion de retorno de una llamada anidada sea la de quien llamo de verdad. */
static __attribute__((noinline)) void xhci_bombear_eventos(void) {
#ifdef TAEK_XHCI_HOST_TEST
    extern void xhci_prueba_eventos(void);
    xhci_prueba_eventos();return;
#endif
    /* Fase 0: el hueco desde la pasada anterior y si esta pasada empieza dentro de otra. Se cuenta, no se corta:
     * una llamada anidada que volviera sin bombear dejaria a su transferencia esperando hasta su tiempo limite. */
    atencion_marcar(&atencion_usb);
    if (g_bombeo_profundidad && !g_fase0.reentradas++)
        g_fase0.reentrada_desde = (uint64_t)(uintptr_t)__builtin_return_address(0);
    g_bombeo_profundidad++;
    uint32_t sacados = 0;
    struct trb_xhci evt;
    for (uint32_t i = 0; i < XHCI_TAM_ANILLO && xhci_extraer_evento(&evt); i++) {
        sacados++;
        uint8_t tipo = (evt.control >> 10) & 0x3F;
        uint8_t slot = (evt.control >> 24) & 0xFF;
        uint8_t dci = (evt.control >> 16) & 0x1F;
        uint8_t cc = (evt.estado >> 24) & 0xFF;
        g_estado.ultimo_evento_trb_tipo = tipo;
        g_estado.paquetes_recibidos++;
        if (tipo == TRB_TIPO_CMD_COMP_EVT) {
            g_evento_comando.trb = evt;
            g_evento_comando.listo = 1;
        } else if (tipo == TRB_TIPO_TRANSFER_EVT) {
            g_estado.eventos_transferencia++;
            g_estado.ultimo_codigo_transfer = cc;
            g_estado.ultimo_dci_transfer = dci;
            if (cc != 1 && cc != 13) g_estado.fallos_transferencia++;
            int es_hid = 0;
            for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
                if (g_teclado_eps[e].activo && g_teclado_eps[e].slot_id == slot &&
                    g_teclado_eps[e].ep_dci == dci) { es_hid = 1; break; }
            }
            if (g_captura_bulk.activo && !es_hid &&
                g_captura_bulk.slot == slot && g_captura_bulk.dci == dci) {
                xhci_capturar_bulk_evento(&evt);
                continue;
            }
            if (es_hid) {
                xhci_procesar_evento_hid(&evt);
            } else if (slot > 0 && slot <= XHCI_MAX_SLOTS && dci < 32) {
                struct xhci_evento_pendiente *b = &g_eventos_transfer[slot][dci];
                b->trb = evt;
                b->listo = 1;
            } else {
                serial_imprimir_linea("[xHCI] Evento de transferencia con slot/DCI inválido.");
            }
        } else if (tipo == 34) { // Port Status Change Event
            g_cambio_puerto_pendiente = 1;
            /* H204: el Port ID va en los bits 31:24 del primer DWORD. PORTSC se lee ahora, antes de que el escaneo
             * limpie los bits de cambio: un rebote corto (CCS 1-0-1 entre dos escaneos) solo queda aqui. */
            uint8_t puerto = (uint8_t)((evt.parametro >> 24) & 0xFF);
            g_fase0.cambios_puerto++;
            if (puerto >= 1 && puerto <= g_estado.max_puertos)
                xhci_suceso(puerto, mmio_leer32(g_op_base + REG_OP_PORTSC_BASE + (uint64_t)(puerto - 1) * 0x10),
                            XHCI_SUCESO_EVENTO, 0);
        } else {
            if (tipo == 37) {   /* Host Controller Event; el codigo 21 es Event Ring Full Error */
                g_fase0.eventos_hc++;
                if (cc == 21) g_fase0.anillo_lleno++;
            }
            serial_imprimir("[xHCI] Evento no gestionado, tipo=");
            serial_imprimir_dec(tipo);
            serial_imprimir(" codigo=");
            serial_imprimir_dec(cc);
            serial_imprimir_linea("");
        }
    }
    g_bombeo_profundidad--;
    if (sacados > g_fase0.eventos_max) g_fase0.eventos_max = sacados;
}

// Volcado crudo hexadecimal y checksum de Input Context (para auditoría offline byte a byte)
static void xhci_volcar_input_context_crudo(const char *etiqueta, const uint8_t *in_ctx) {
    if (!in_ctx) return;
    serial_imprimir("\n[xHCI DUMP CTX] ===== ");
    serial_imprimir(etiqueta);
    serial_imprimir_linea(" =====");

    const uint32_t *dw = (const uint32_t *)in_ctx;
    uint32_t checksum_xor = 0;
    uint32_t checksum_sum = 0;
    uint32_t total_dw = (33 * (uint32_t)g_tamano_contexto) / 4;

    for (uint32_t i = 0; i < total_dw; i++) {
        checksum_xor ^= dw[i];
        checksum_sum += dw[i];
    }

    serial_imprimir("  Checksum: XOR=");
    serial_imprimir_hex(checksum_xor);
    serial_imprimir(" SUM=");
    serial_imprimir_hex(checksum_sum);
    serial_imprimir_linea("");

    // 1. Control Context (offset 0)
    serial_imprimir("  [CTRL CTX] ");
    for (int i = 0; i < 8; i++) {
        serial_imprimir("DW");
        serial_imprimir_dec(i);
        serial_imprimir("=");
        serial_imprimir_hex(dw[i]);
        serial_imprimir(" ");
    }
    serial_imprimir_linea("");

    // 2. Slot Context (offset g_tamano_contexto)
    const uint32_t *slot_dw = (const uint32_t *)(in_ctx + g_tamano_contexto);
    serial_imprimir("  [SLOT CTX] ");
    for (int i = 0; i < 8; i++) {
        serial_imprimir("DW");
        serial_imprimir_dec(i);
        serial_imprimir("=");
        serial_imprimir_hex(slot_dw[i]);
        serial_imprimir(" ");
    }
    serial_imprimir_linea("");

    // 3. EP0 Context (offset 2 * g_tamano_contexto)
    const uint32_t *ep0_dw = (const uint32_t *)(in_ctx + 2 * g_tamano_contexto);
    serial_imprimir("  [EP0  CTX] ");
    for (int i = 0; i < 8; i++) {
        serial_imprimir("DW");
        serial_imprimir_dec(i);
        serial_imprimir("=");
        serial_imprimir_hex(ep0_dw[i]);
        serial_imprimir(" ");
    }
    serial_imprimir_linea("");

    // 4. Endpoints Contexts adicionales activos si están presentes (DCI >= 2)
    uint32_t add_flags = dw[1];
    for (int dci = 2; dci <= 31; dci++) {
        if (add_flags & (1U << dci)) {
            const uint32_t *ep_dw = (const uint32_t *)(in_ctx + (dci + 1) * g_tamano_contexto);
            serial_imprimir("  [EP");
            serial_imprimir_dec(dci);
            serial_imprimir(" CTX] ");
            for (int i = 0; i < 8; i++) {
                serial_imprimir("DW");
                serial_imprimir_dec(i);
                serial_imprimir("=");
                serial_imprimir_hex(ep_dw[i]);
                serial_imprimir(" ");
            }
            serial_imprimir_linea("");
        }
    }
    serial_imprimir_linea("[xHCI DUMP CTX] ==============================\n");
}

// --- CESIÓN DE CONTROL BIOS -> OS (USBLEGSUP) ---
static void xhci_negociar_cesion_bios(uint64_t virt_bar0, uint32_t xecp) {
    if (xecp == 0) return;
    uint32_t offset = xecp * 4;

    while (offset) {
        uint32_t cap = mmio_leer32(virt_bar0 + offset);
        uint8_t cap_id = cap & 0xFF;
        uint8_t siguiente = (cap >> 8) & 0xFF;

        if (cap_id == 1) { // USB Legacy Support
            serial_imprimir("[xHCI] Capacidad USB Legacy encontrada. Solicitando cesión de control BIOS -> OS... ");
            // Reclamar propiedad para el SO (bit 24)
            mmio_escribir32(virt_bar0 + offset, cap | (1U << 24));

            // Esperar a que la BIOS libere su semáforo (bit 16)
            int timeout = 100;
            while ((mmio_leer32(virt_bar0 + offset) & (1U << 16)) && timeout > 0) {
                esperar_milisegundos(5);
                timeout--;
            }

            if (timeout > 0) {
                serial_imprimir_linea("[OK: Propiedad Obtenida]");
            } else {
                serial_imprimir_linea("[AVISO: Timeout en cesión BIOS, forzando toma de control]");
            }

            // Desactivar SMIs en USBLEGCTLSTS (offset + 4) para evitar interferencias
            mmio_escribir32(virt_bar0 + offset + 4, 0x00000000);
            break;
        }

        if (siguiente == 0) break;
        offset += siguiente * 4;
    }
}

// --- DIAGNÓSTICO FORENSE DEL CONTROLADOR XHCI ---
void xhci_imprimir_diagnostico_completo(void) { XHCI_EXCLUSIVO();
    if (!g_estado.controlador_detectado) {
        consola_imprimir_linea_color("[!] xHCI: Controlador no detectado en el bus PCI.", COLOR_ERROR_DEFAULT);
        return;
    }

    consola_imprimir_linea_color("================== [ DIAGNOSTICO FORENSE USB xHCI ] ==================", COLOR_AVISO_DEFAULT);

    // 1. Registros Operacionales
    uint32_t usbcmd = mmio_leer32(g_op_base + REG_OP_USBCMD);
    uint32_t usbsts = mmio_leer32(g_op_base + REG_OP_USBSTS);
    uint64_t crcr   = mmio_leer64(g_op_base + REG_OP_CRCR);
    uint64_t dcbaap = mmio_leer64(g_op_base + REG_OP_DCBAAP);
    uint32_t config = mmio_leer32(g_op_base + REG_OP_CONFIG);

    consola_imprimir("  USBCMD: "); consola_imprimir_hex(usbcmd);
    consola_imprimir(" (RS="); consola_imprimir_dec(usbcmd & USBCMD_RS ? 1 : 0);
    consola_imprimir(" INTE="); consola_imprimir_dec(usbcmd & USBCMD_INTE ? 1 : 0);
    consola_imprimir(" HCRST="); consola_imprimir_dec(usbcmd & USBCMD_HCRST ? 1 : 0);
    consola_imprimir_linea(")");

    consola_imprimir("  USBSTS: "); consola_imprimir_hex(usbsts);
    consola_imprimir(" (HCH="); consola_imprimir_dec(usbsts & USBSTS_HCH ? 1 : 0);
    consola_imprimir(" HSE="); consola_imprimir_dec(usbsts & (1U << 2) ? 1 : 0);
    consola_imprimir(" EINT="); consola_imprimir_dec(usbsts & USBSTS_EINT ? 1 : 0);
    consola_imprimir(" PCD="); consola_imprimir_dec(usbsts & USBSTS_PCD ? 1 : 0);
    consola_imprimir(" CNR="); consola_imprimir_dec(usbsts & USBSTS_CNR ? 1 : 0);
    consola_imprimir_linea(")");

    consola_imprimir("  CRCR  : "); consola_imprimir_hex(crcr);
    consola_imprimir(" (RCS="); consola_imprimir_dec((uint32_t)(crcr & 1));
    consola_imprimir(" CS="); consola_imprimir_dec((uint32_t)((crcr >> 1) & 1));
    consola_imprimir(" CA="); consola_imprimir_dec((uint32_t)((crcr >> 2) & 1));
    consola_imprimir(" CRR="); consola_imprimir_dec((uint32_t)((crcr >> 3) & 1));
    consola_imprimir(" CRP="); consola_imprimir_hex(crcr & ~0x3FULL);
    consola_imprimir_linea(")");

    consola_imprimir("  DCBAAP: "); consola_imprimir_hex(dcbaap);
    consola_imprimir(" | CONFIG: "); consola_imprimir_hex(config);
    if (g_dcbaa) {
        consola_imprimir(" | DCBAA[0]: "); consola_imprimir_hex(g_dcbaa[0]);
        if (g_estado.max_slots >= 1) {
            consola_imprimir(" | DCBAA[1]: "); consola_imprimir_hex(g_dcbaa[1]);
        }
        if (g_estado.max_slots >= 2) {
            consola_imprimir(" | DCBAA[2]: "); consola_imprimir_hex(g_dcbaa[2]);
        }
        if (g_estado.teclado_slot_id > 2 && g_estado.teclado_slot_id <= g_estado.max_slots) {
            consola_imprimir(" | DCBAA[Slot");
            consola_imprimir_dec(g_estado.teclado_slot_id);
            consola_imprimir("]: "); consola_imprimir_hex(g_dcbaa[g_estado.teclado_slot_id]);
        }
    }
    consola_imprimir_linea("");

    // 2. Registros de Interrupter 0
    uint64_t intr0  = g_rts_base + 0x20;
    uint32_t iman   = mmio_leer32(intr0 + 0x00);
    uint32_t erstsz = mmio_leer32(intr0 + 0x08);
    uint64_t erstba = mmio_leer64(intr0 + 0x10);
    uint64_t erdp   = mmio_leer64(intr0 + 0x18);

    consola_imprimir("  IMAN  : "); consola_imprimir_hex(iman);
    consola_imprimir(" (IP="); consola_imprimir_dec(iman & 1);
    consola_imprimir(" IE="); consola_imprimir_dec((iman >> 1) & 1);
    consola_imprimir(") | ERSTSZ: "); consola_imprimir_dec(erstsz);
    consola_imprimir_linea("");

    consola_imprimir("  ERSTBA: "); consola_imprimir_hex(erstba);
    consola_imprimir(" | ERDP: "); consola_imprimir_hex(erdp);
    consola_imprimir(" (EHB="); consola_imprimir_dec((uint32_t)((erdp >> 3) & 1));
    consola_imprimir_linea(")");

    // 3. Estado de Anillos Software vs Hardware
    consola_imprimir_linea_color("------------------ [ ANILLOS DMA (SW vs HW) ] ------------------", COLOR_PROMPT_DEFAULT);
    consola_imprimir("  CMD Ring SW  : Phys="); consola_imprimir_hex(g_cmd_ring_fisica);
    consola_imprimir(" | Idx="); consola_imprimir_dec(g_cmd_idx);
    consola_imprimir(" | Cycle="); consola_imprimir_dec(g_cmd_cycle);
    consola_imprimir(" | Wraps="); consola_imprimir_dec(g_cmd_ring_wraparounds);
    consola_imprimir_linea("");

    consola_imprimir("  EVT Ring SW  : Phys="); consola_imprimir_hex(g_event_ring_fisica);
    consola_imprimir(" | Idx="); consola_imprimir_dec(g_event_idx);
    consola_imprimir(" | Cycle="); consola_imprimir_dec(g_event_cycle);
    consola_imprimir(" | Wraps="); consola_imprimir_dec(g_evt_ring_wraparounds);
    consola_imprimir_linea("");

    // 4. Volcado de TRBs de Comandos
    consola_imprimir_linea_color("------------------ [ ANILLO DE COMANDOS (TRBs) ] ------------------", COLOR_PROMPT_DEFAULT);
    for (int i = 0; i < 10 && i < XHCI_TAM_ANILLO; i++) {
        volatile struct trb_xhci *ctrb = &g_cmd_ring[i];
        uint8_t tipo = (ctrb->control >> 10) & 0x3F;
        uint8_t c = ctrb->control & 1;
        const char *nombre = "OTRO";
        if (tipo == TRB_TIPO_ENABLE_SLOT) nombre = "ENABLE_SLOT";
        else if (tipo == TRB_TIPO_DISABLE_SLOT) nombre = "DISABLE_SLOT";
        else if (tipo == TRB_TIPO_ADDRESS_DEV) nombre = "ADDRESS_DEV";
        else if (tipo == TRB_TIPO_CONFIG_EP) nombre = "CONFIG_EP";
        else if (tipo == TRB_TIPO_EVAL_CTX) nombre = "EVAL_CTX";
        else if (tipo == TRB_TIPO_LINK) nombre = "LINK";

        consola_imprimir("  Cmd["); consola_imprimir_dec(i); consola_imprimir("]: ");
        if (i == (int)g_cmd_idx) consola_imprimir("-> "); else consola_imprimir("   ");
        consola_imprimir(nombre);
        consola_imprimir(" (Tipo="); consola_imprimir_dec(tipo);
        consola_imprimir(" Cyc="); consola_imprimir_dec(c);
        consola_imprimir(" Param="); consola_imprimir_hex(ctrb->parametro);
        consola_imprimir(" Ctrl="); consola_imprimir_hex(ctrb->control);
        consola_imprimir_linea(")");
    }

    // 5. Volcado de TRBs de Eventos
    consola_imprimir_linea_color("------------------ [ ANILLO DE EVENTOS (TRBs) ] ------------------", COLOR_PROMPT_DEFAULT);
    dma_sincronizar_dispositivo_a_cpu((const void *)g_event_ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
    int evt_inicio = (int)g_event_idx - 4;
    if (evt_inicio < 0) evt_inicio = 0;
    int evt_fin = evt_inicio + 12;
    if (evt_fin > XHCI_TAM_ANILLO) evt_fin = XHCI_TAM_ANILLO;

    for (int i = evt_inicio; i < evt_fin; i++) {
        volatile struct trb_xhci *etrb = &g_event_ring[i];
        uint8_t tipo = (etrb->control >> 10) & 0x3F;
        uint8_t c = etrb->control & 1;
        uint8_t cc = (etrb->estado >> 24) & 0xFF;
        uint8_t slot = (etrb->control >> 24) & 0xFF;

        const char *nombre = "DESCONOCIDO";
        if (tipo == TRB_TIPO_CMD_COMP_EVT) nombre = "CMD_COMP";
        else if (tipo == TRB_TIPO_PORT_STATUS) nombre = "PORT_STATUS";
        else if (tipo == TRB_TIPO_TRANSFER_EVT) nombre = "TRANSFER";

        consola_imprimir("  Evt["); consola_imprimir_dec(i); consola_imprimir("]: ");
        if (i == (int)g_event_idx) consola_imprimir("-> "); else consola_imprimir("   ");
        consola_imprimir(nombre);
        consola_imprimir(" (Tipo="); consola_imprimir_dec(tipo);
        consola_imprimir(" CC="); consola_imprimir_dec(cc);
        consola_imprimir(" Slot="); consola_imprimir_dec(slot);
        consola_imprimir(" Cyc="); consola_imprimir_dec(c);
        consola_imprimir(" Ctrl="); consola_imprimir_hex(etrb->control);
        consola_imprimir(" Param="); consola_imprimir_hex(etrb->parametro);
        consola_imprimir_linea(")");
    }

    // 6. Volcado de Endpoints y Contexto de Hardware de Teclados Activos
    consola_imprimir_linea_color("------------------ [ TECLADOS USB (ENDPOINTS & HW CONTEXT) ] ------------------", COLOR_PROMPT_DEFAULT);
    consola_imprimir("  Estado Global: Detectado=");
    consola_imprimir_dec(g_estado.teclado_detectado);
    consola_imprimir(" | Teclados Activos=");
    consola_imprimir_dec(g_estado.teclados_activos);
    consola_imprimir(" | Total EPs Armados=");
    consola_imprimir_dec(xhci_conteo_endpoints_activos());
    consola_imprimir_linea("");

    int ep_impresos = 0;
    for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
        struct xhci_ep_teclado *ep = &g_teclado_eps[e];
        if (!ep->activo) continue;
        ep_impresos++;

        uint8_t *dev_ctx = g_slot_dev_ctx[ep->slot_id];
        uint8_t ep_state = 0;
        uint64_t hw_tr_deq = 0;
        uint8_t hw_dcs = 0;
        const char *nom_state = "Desconocido";

        if (dev_ctx) {
            dma_sincronizar_dispositivo_a_cpu(dev_ctx, 32 * g_tamano_contexto);
            uint32_t *hw_ep_ctx = (uint32_t *)(dev_ctx + (ep->ep_dci) * g_tamano_contexto);
            ep_state = hw_ep_ctx[0] & 0x07;
            if (ep_state == 0) nom_state = "Disabled";
            else if (ep_state == 1) nom_state = "Running";
            else if (ep_state == 2) nom_state = "Halted";
            else if (ep_state == 3) nom_state = "Stopped";
            else if (ep_state == 4) nom_state = "Error";

            hw_tr_deq = ((uint64_t)hw_ep_ctx[3] << 32) | (hw_ep_ctx[2] & ~0x0F);
            hw_dcs = hw_ep_ctx[2] & 1;
        }

        consola_imprimir("  EP[Slot "); consola_imprimir_dec(ep->slot_id);
        consola_imprimir(" Ptr"); consola_imprimir_dec(ep->puerto_idx);
        consola_imprimir(" DCI "); consola_imprimir_dec(ep->ep_dci);
        consola_imprimir("]: HW State="); consola_imprimir(nom_state);
        consola_imprimir(" ("); consola_imprimir_dec(ep_state);
        consola_imprimir(") HW Deq="); consola_imprimir_hex(hw_tr_deq);
        consola_imprimir(" DCS="); consola_imprimir_dec(hw_dcs);
        consola_imprimir(" | SW Idx="); consola_imprimir_dec(ep->idx);
        consola_imprimir(" Cyc="); consola_imprimir_dec(ep->cycle);
        consola_imprimir_linea("");

        // Imprimir estado de los primeros 2 TRBs del anillo
        dma_sincronizar_dispositivo_a_cpu((const void *)ep->ring, 2 * sizeof(struct trb_xhci));
        consola_imprimir("    TRB[0]: Ctrl="); consola_imprimir_hex(ep->ring[0].control);
        consola_imprimir(" Param="); consola_imprimir_hex(ep->ring[0].parametro);
        consola_imprimir(" Len="); consola_imprimir_dec(ep->ring[0].estado & 0x1FFFF);
        consola_imprimir(" | TRB[1]: Ctrl="); consola_imprimir_hex(ep->ring[1].control);
        consola_imprimir_linea("");
    }

    if (ep_impresos == 0) {
        // Inspección directa por hardware de ranuras activas en DCBAA si dev_ctx no está cacheado para teclado
        int ranuras_vistas = 0;
        if (g_dcbaa) {
            uint64_t hhdm = memoria_obtener_hhdm_offset();
            for (uint8_t s = 1; s <= g_estado.max_slots; s++) {
                if (g_dcbaa[s] != 0) {
                    ranuras_vistas++;
                    uint8_t *dev_ctx = (uint8_t *)(g_dcbaa[s] + hhdm);
                    dma_sincronizar_dispositivo_a_cpu(dev_ctx, 32 * g_tamano_contexto);
                    uint32_t *slot_ctx = (uint32_t *)dev_ctx;
                    uint8_t slot_state = (slot_ctx[3] >> 27) & 0x1F;
                    uint8_t dev_addr   = slot_ctx[3] & 0xFF;
                    uint8_t max_dci    = (slot_ctx[0] >> 27) & 0x1F;

                    consola_imprimir("  Slot "); consola_imprimir_dec(s);
                    consola_imprimir(": Estado="); consola_imprimir_dec(slot_state);
                    consola_imprimir(" Addr="); consola_imprimir_dec(dev_addr);
                    consola_imprimir(" MaxDCI="); consola_imprimir_dec(max_dci);
                    consola_imprimir(" DevCtxPhys="); consola_imprimir_hex(g_dcbaa[s]);
                    consola_imprimir_linea("");

                    for (uint8_t dci = 1; dci <= max_dci && dci < 32; dci++) {
                        uint32_t *hw_ep = (uint32_t *)(dev_ctx + dci * g_tamano_contexto);
                        uint8_t ep_state = hw_ep[0] & 0x07;
                        if (ep_state != 0 || dci == 1) {
                            uint64_t deq = ((uint64_t)hw_ep[3] << 32) | (hw_ep[2] & ~0x0F);
                            uint8_t dcs = hw_ep[2] & 1;
                            consola_imprimir("    EP[DCI "); consola_imprimir_dec(dci);
                            consola_imprimir("]: State="); consola_imprimir_dec(ep_state);
                            consola_imprimir(" HW Deq="); consola_imprimir_hex(deq);
                            consola_imprimir(" DCS="); consola_imprimir_dec(dcs);
                            consola_imprimir_linea("");
                        }
                    }
                }
            }
        }
        if (ranuras_vistas == 0) {
            consola_imprimir_linea("  [i] Sin dispositivos enumerados ni contextos asignados en DCBAA.");
        }
    }

    // Log serie equivalente para capturadoras UART
    serial_imprimir_linea("\n--- VOLCADO FORENSE XHCI ---");
    serial_imprimir("USBCMD="); serial_imprimir_hex(usbcmd);
    serial_imprimir(" USBSTS="); serial_imprimir_hex(usbsts);
    serial_imprimir(" CRCR="); serial_imprimir_hex(crcr);
    serial_imprimir(" ERDP="); serial_imprimir_hex(erdp);
    serial_imprimir(" CmdIdx="); serial_imprimir_dec(g_cmd_idx);
    serial_imprimir(" CmdWraps="); serial_imprimir_dec(g_cmd_ring_wraparounds);
    serial_imprimir(" EvtIdx="); serial_imprimir_dec(g_event_idx);
    serial_imprimir(" EvtWraps="); serial_imprimir_dec(g_evt_ring_wraparounds);
    serial_imprimir_linea("\n----------------------------\n");

    consola_imprimir_linea_color("========================================================================", COLOR_AVISO_DEFAULT);
}

// --- GESTIÓN DEL ANILLO DE COMANDOS ---
static int xhci_enviar_comando(uint64_t param, uint32_t status, uint32_t control, struct trb_xhci *out_evt) {
    // No se puede publicar trabajo en un anillo si un aborto anterior no
    // consiguió detener el Command Ring.
    if (!g_cmd_ring_disponible) return -2;
    g_evento_comando.listo = 0;

    uint64_t tsc_inicio = rdtsc();

    uint32_t trb_ctrl = control | (g_cmd_cycle ? 1 : 0);
    uint8_t trb_tipo = (trb_ctrl >> 10) & 0x3F;
    uint8_t trb_slot = (trb_ctrl >> 24) & 0xFF;

    // Snapshot y telemetría nuclear ANTES de tocar el Doorbell
    serial_imprimir("\n[xHCI CMD] Encolando Cmd[");
    serial_imprimir_dec(g_cmd_idx);
    serial_imprimir("]: Tipo=");
    serial_imprimir_dec(trb_tipo);
    serial_imprimir(" Slot=");
    serial_imprimir_dec(trb_slot);
    serial_imprimir(" Param=");
    serial_imprimir_hex(param);
    serial_imprimir(" Status=");
    serial_imprimir_hex(status);
    serial_imprimir(" Ctrl=");
    serial_imprimir_hex(trb_ctrl);
    serial_imprimir(" Cyc=");
    serial_imprimir_dec(g_cmd_cycle);
    serial_imprimir(" TSC=");
    serial_imprimir_hex(tsc_inicio);
    serial_imprimir_linea("");

    // Colocar TRB en el anillo de comandos
    uint64_t esperado_cmd_trb = g_cmd_ring_fisica + (uint64_t)g_cmd_idx * sizeof(struct trb_xhci);
    volatile struct trb_xhci *trb = &g_cmd_ring[g_cmd_idx];
    trb->parametro = param;
    trb->estado    = status;
    trb->control   = trb_ctrl;
    dma_sincronizar_cpu_a_dispositivo((const void *)trb, sizeof(*trb));
    __asm__ volatile ("mfence" ::: "memory");

    g_cmd_idx++;
    if (g_cmd_idx >= XHCI_TAM_ANILLO - 1) {
        // Enlazar de vuelta al inicio
        volatile struct trb_xhci *link = &g_cmd_ring[XHCI_TAM_ANILLO - 1];
        link->parametro = g_cmd_ring_fisica;
        link->estado    = 0;
        link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) | (g_cmd_cycle ? 1 : 0);
        dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
        __asm__ volatile ("mfence" ::: "memory");
        g_cmd_idx = 0;
        g_cmd_cycle = !g_cmd_cycle;
        g_cmd_ring_wraparounds++;
        serial_imprimir("  [xHCI CMD] Link TRB ejecutado. CmdWraparounds=");
        serial_imprimir_dec(g_cmd_ring_wraparounds);
        serial_imprimir_linea("");
    }

    // Lectura de USBSTS inmediatamente antes de sonar el Doorbell
    uint32_t usbsts_pre = mmio_leer32(g_op_base + REG_OP_USBSTS);
    serial_imprimir("  [xHCI CMD] USBSTS pre-DB=");
    serial_imprimir_hex(usbsts_pre);
    serial_imprimir(" (HCH=");
    serial_imprimir_dec(usbsts_pre & 1);
    serial_imprimir_linea(")");

    // Tocar timbre del host controller (slot 0, comando 0)
    xhci_tocar_timbre(0, 0);

    // El dispatcher conserva los eventos de otros endpoints durante la espera.
    int timeout = 5000;
    while (timeout > 0) {
        xhci_bombear_eventos();
        if (g_evento_comando.listo) {
            struct trb_xhci evt = g_evento_comando.trb;
            g_evento_comando.listo = 0;
            if (evt.parametro != esperado_cmd_trb) {
                serial_imprimir_linea("[xHCI] Finalización de comando atrasada; ignorada.");
                continue;
            }
            if (out_evt) *out_evt = evt;
            uint8_t codigo = (evt.estado >> 24) & 0xFF;
            return codigo == 1 ? 0 : (int)codigo;
        }
        esperar_milisegundos(1);
        timeout--;
    }

    uint64_t tsc_timeout = rdtsc();
    uint32_t usbsts = mmio_leer32(g_op_base + REG_OP_USBSTS);
    uint64_t crcr = mmio_leer64(g_op_base + REG_OP_CRCR);
    uint64_t erdp = mmio_leer64(g_rts_base + 0x20 + 0x18);
    volatile struct trb_xhci *evt_actual = &g_event_ring[g_event_idx];
    uint8_t ciclo_leido = evt_actual->control & 1;
    uint8_t ciclo_esperado = g_event_cycle;

    consola_imprimir_color("    [!] xHCI TIMEOUT en comando! Telemetría Nuclear:\n", COLOR_ERROR_DEFAULT);
    consola_imprimir_color("        Cmd Tipo=", COLOR_ERROR_DEFAULT);
    consola_imprimir_dec(trb_tipo);
    consola_imprimir_color(" | Slot=", COLOR_ERROR_DEFAULT);
    consola_imprimir_dec(trb_slot);
    consola_imprimir_color(" | USBSTS pre=", COLOR_ERROR_DEFAULT);
    consola_imprimir_hex(usbsts_pre);
    consola_imprimir_color(" post=", COLOR_ERROR_DEFAULT);
    consola_imprimir_hex(usbsts);
    consola_imprimir_linea("");

    consola_imprimir_color("        EvtIdx=", COLOR_ERROR_DEFAULT);
    consola_imprimir_dec(g_event_idx);
    consola_imprimir_color(" | CycEsp=", COLOR_ERROR_DEFAULT);
    consola_imprimir_dec(ciclo_esperado);
    consola_imprimir_color(" | CycLeido=", COLOR_ERROR_DEFAULT);
    consola_imprimir_dec(ciclo_leido);
    consola_imprimir_color(" | EvtWraps=", COLOR_ERROR_DEFAULT);
    consola_imprimir_dec(g_evt_ring_wraparounds);
    consola_imprimir_linea("");

    consola_imprimir_color("        TRB actual en EvtIdx: Ctrl=", COLOR_ERROR_DEFAULT);
    consola_imprimir_hex(evt_actual->control);
    consola_imprimir_color(" Estado=", COLOR_ERROR_DEFAULT);
    consola_imprimir_hex(evt_actual->estado);
    consola_imprimir_linea("");

    serial_imprimir("\n  [xHCI NUCLEAR TIMEOUT] Cmd Tipo=");
    serial_imprimir_dec(trb_tipo);
    serial_imprimir(" Slot=");
    serial_imprimir_dec(trb_slot);
    serial_imprimir(" ciclos=");
    serial_imprimir_hex(tsc_timeout - tsc_inicio);
    serial_imprimir(" USBSTS_pre=");
    serial_imprimir_hex(usbsts_pre);
    serial_imprimir(" USBSTS_post=");
    serial_imprimir_hex(usbsts);
    serial_imprimir(" (HCH=");
    serial_imprimir_dec(usbsts & 1);
    serial_imprimir_linea(")");

    serial_imprimir("  EVENT RING ACTUAL: EvtIdx=");
    serial_imprimir_dec(g_event_idx);
    serial_imprimir(" CycEsp=");
    serial_imprimir_dec(ciclo_esperado);
    serial_imprimir(" CycLeido=");
    serial_imprimir_dec(ciclo_leido);
    serial_imprimir(" Wraparounds=");
    serial_imprimir_dec(g_evt_ring_wraparounds);
    serial_imprimir_linea("");

    serial_imprimir("  TRB en EvtIdx: Param=");
    serial_imprimir_hex(evt_actual->parametro);
    serial_imprimir(" Estado=");
    serial_imprimir_hex(evt_actual->estado);
    serial_imprimir(" Ctrl=");
    serial_imprimir_hex(evt_actual->control);
    serial_imprimir_linea("");

    serial_imprimir("  ERDP=");
    serial_imprimir_hex(erdp);
    serial_imprimir(" (EHB=");
    serial_imprimir_dec((uint32_t)((erdp >> 3) & 1));
    serial_imprimir(") CRCR=");
    serial_imprimir_hex(crcr);
    serial_imprimir_linea("");

    // Volcado de diagnóstico forense inmediato en pantalla
    xhci_imprimir_diagnostico_completo();

    // Intento de recuperación y desbloqueo del Command Ring (xHCI 1.2 §4.6.1.2 Command Abort)
    consola_imprimir_linea_color("    [!] Abortando comando en hardware (CRCR.CA = 1)...", COLOR_AVISO_DEFAULT);
    serial_imprimir_linea("  [xHCI] Emitiendo Command Abort (CA = 1)...");

    // Escribir CA = 1 en el registro CRCR sin alterar CRP cuando CRR=1.
    // Según xHCI 1.2 §5.4.5, cuando CRR=1 el campo CRP (bits 63:6) es reservado (RsvdP).
    // Escribir un puntero físico base en CRP mientras el controlador corre hace que chipsets Intel
    // como Sunrise Point-LP (8086:9d2f) ignoren la petición de aborto.
    // Por ello, se emite una escritura MMIO de 32 bits únicamente sobre el bit CA (1U << 2).
    uint64_t crcr_actual = mmio_leer64(g_op_base + REG_OP_CRCR);
    if (crcr_actual & (1ULL << 3) /* CRR */) {
        mmio_escribir32(g_op_base + REG_OP_CRCR, (1U << 2) /* CA */);
    }

    // Esperar a que el controlador detenga el Command Ring (CRR -> 0).
    // xHCI 1.2 §4.6.1.2 concede hasta 5000 ms; usamos 200 iteraciones de 5 ms (1000 ms).
    // Si a los 500 ms sigue activo, reintentamos con escritura de 64 bits como contingencia.
    int abort_timeout = 200;
    while ((mmio_leer64(g_op_base + REG_OP_CRCR) & (1ULL << 3) /* CRR */) && abort_timeout > 0) {
        esperar_milisegundos(5);
        abort_timeout--;
        if (abort_timeout == 100) {
            mmio_escribir64(g_op_base + REG_OP_CRCR, g_cmd_ring_fisica | (1ULL << 2) /* CA */ | (g_cmd_cycle ? 1 : 0));
        }
    }

    if (mmio_leer64(g_op_base + REG_OP_CRCR) & (1ULL << 3)) {
        // No tocar CRCR mientras el controlador aún posee el anillo.
        g_cmd_ring_disponible = 0;
        serial_imprimir_linea("  [xHCI ERROR] Command Ring sigue activo tras Command Abort; controlador bloqueado.");
        return -1;
    }

    xhci_bombear_eventos();
    g_evento_comando.listo = 0;

    // El CRCR requiere su puntero de anillo alineado a 64 bytes. No puede
    // apuntarse a g_cmd_idx (un TRB mide 16 bytes): hacerlo pone bits de
    // dirección en campos de control/reservados. Con el anillo ya detenido,
    // reconstruirlo desde su base y reiniciar el ciclo evita reutilizar un
    // TRB cuyo estado de ejecución quedó incierto durante el timeout.
    for (uint32_t i = 0; i < XHCI_TAM_ANILLO; i++) {
        g_cmd_ring[i].parametro = 0;
        g_cmd_ring[i].estado = 0;
        g_cmd_ring[i].control = 0;
    }
    volatile struct trb_xhci *link = &g_cmd_ring[XHCI_TAM_ANILLO - 1];
    link->parametro = g_cmd_ring_fisica;
    link->estado = 0;
    link->control = (TRB_TIPO_LINK << 10) | (1U << 1) | 1U;
    g_cmd_idx = 0;
    g_cmd_cycle = 1;
    g_cmd_ring_wraparounds = 0;
    dma_sincronizar_cpu_a_dispositivo((const void *)g_cmd_ring,
                                      XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
    __asm__ volatile ("mfence" ::: "memory");
    uint64_t nuevo_crcr = g_cmd_ring_fisica | 1U;
    mmio_escribir64(g_op_base + REG_OP_CRCR, nuevo_crcr);
    uint64_t crcr_post_abort = mmio_leer64(g_op_base + REG_OP_CRCR);
    g_cmd_ring_disponible = 1;
    serial_imprimir("  [xHCI] Command Ring restaurado: CRCR=");
    serial_imprimir_hex(crcr_post_abort);
    serial_imprimir_linea("");

    // Limpiar banderas residuales de interrupciones
    mmio_escribir32(g_op_base + REG_OP_USBSTS, USBSTS_EINT | USBSTS_PCD);
    mmio_escribir32(g_rts_base + 0x20 + 0x00, 0x03); // IMAN: IP y IE

    return -1; // Timeout
}

// --- SONDEO ACTIVO Y LECTURA DE TECLADO ---
static void xhci_procesar_evento_hid(const struct trb_xhci *evt) {
    uint8_t tipo = (evt->control >> 10) & 0x3F;
    uint8_t slot_id = (evt->control >> 24) & 0xFF;
    uint8_t ep_dci = (evt->control >> 16) & 0x1F;
    uint8_t codigo = (evt->estado >> 24) & 0xFF;
        if (tipo == TRB_TIPO_TRANSFER_EVT) {
            g_estado.eventos_transferencia++;
            g_estado.ultimo_codigo_transfer = codigo;
            g_estado.ultimo_dci_transfer = ep_dci;
            if (codigo != 1 && codigo != 13) g_estado.fallos_transferencia++;

            // Localizar cuál de nuestros endpoints configurados generó el evento
            struct xhci_ep_teclado *ep = NULL;
            for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
                if (g_teclado_eps[e].activo && g_teclado_eps[e].slot_id == slot_id && g_teclado_eps[e].ep_dci == ep_dci) {
                    ep = &g_teclado_eps[e];
                    break;
                }
            }

            if (!ep) {
                serial_imprimir("  [xHCI TRANSFER NO ASOCIADO] Slot=");
                serial_imprimir_dec(slot_id);
                serial_imprimir(" DCI=");
                serial_imprimir_dec(ep_dci);
                serial_imprimir(" CC=");
                serial_imprimir_dec(codigo);
                serial_imprimir_linea("");
            } else if (codigo != 1 && codigo != 13) {
                crudo_guardar(ep, codigo, 0, 0);
            }
            if (ep && (codigo == 1 || codigo == 13)) { // 1 = Success, 13 = Short Packet
                g_estado.reportes_hid_recibidos++;
                uint32_t residual = evt->estado & 0xFFFFFF;
                uint32_t tam_recibido = (residual <= ep->ep_max_pkt) ? (ep->ep_max_pkt - residual) : 0;
                /* A zero-length completion contains no HID report. */
                if (tam_recibido > 64) tam_recibido = 64;

                uint8_t *buf = ep->bufer;
                dma_sincronizar_dispositivo_a_cpu(buf, ep->ep_max_pkt);
                crudo_guardar(ep, codigo, buf, tam_recibido);
                int reporte_cambio = (tam_recibido != ep->ultimo_tam);
                for (uint32_t i = 0; i < tam_recibido && i < sizeof(ep->ultimo_reporte); i++) {
                    if (buf[i] != ep->ultimo_reporte[i]) reporte_cambio = 1;
                }
                if (TAEK_XHCI_TRAZA_IO && reporte_cambio) {
                    serial_imprimir("[HID] slot=");
                    serial_imprimir_dec(slot_id);
                    serial_imprimir(" dci=");
                    serial_imprimir_dec(ep_dci);
                    serial_imprimir(" cc=");
                    serial_imprimir_dec(codigo);
                    serial_imprimir(" len=");
                    serial_imprimir_dec(tam_recibido);
                    serial_imprimir(" data=");
                    for (uint32_t i = 0; i < tam_recibido && i < 16; i++) {
                        serial_imprimir_hex(buf[i]);
                        serial_imprimir(" ");
                    }
                    serial_imprimir_linea("");
                }
                g_estado.ultimo_tamano_reporte = (uint8_t)tam_recibido;
                for (uint32_t i = 0; i < sizeof(g_estado.ultimo_reporte); i++)
                    g_estado.ultimo_reporte[i] = (i < tam_recibido) ? buf[i] : 0;
                g_estado.etapa_enumeracion = 7;
                uint8_t mod = 0;
                int shift = 0;

                // Mouse HID: boot (botones, X/Y con signo, rueda) o, desde H138, protocolo reporte traducido a boot.
                if (ep->es_raton) {
                    /* H142: el receptor 248A:8367 anuncia reportes con ID 1, pero en la laptop manda 3 bytes sin ID
                     * (botones, X, Y): el formato boot, seguramente porque el firmware lo dejo en protocolo boot antes
                     * de ceder el USB. Con el ID esperado delante, '00 C1 2E' se tiraba como "otro reporte" y
                     * '01 0A 17' se leia corrido. Un reporte de 3 o 4 bytes mas corto que el descrito se lee como boot. */
                    unsigned descrito = ep->raton.id ? 1u + (ep->raton.bits + 7u) / 8u : (ep->raton.bits + 7u) / 8u;
                    int boot_en_reporte = ep->raton_reporte && ep->raton.id && (tam_recibido == 3 || tam_recibido == 4) &&
                                          tam_recibido < descrito;
                    if (boot_en_reporte) {
                        raton_entregar(ep, buf, tam_recibido);
                    } else if (ep->raton_reporte) {
                        int32_t dx, dy, rueda; uint32_t bot;
                        if (hid_decodificar_raton(&ep->raton, buf, tam_recibido, &dx, &dy, &bot, &rueda) == 0) {
                            /* Un raton de alta resolucion puede mover mas de 127 en un reporte: se reparte en varios */
                            for (int k = 0; k < 8; k++) {
                                int32_t px = dx > 127 ? 127 : dx < -127 ? -127 : dx;
                                int32_t py = dy > 127 ? 127 : dy < -127 ? -127 : dy;
                                int32_t pr = rueda > 127 ? 127 : rueda < -127 ? -127 : rueda;
                                uint8_t boot[4] = { (uint8_t)(bot & 7u), (uint8_t)(int8_t)px, (uint8_t)(int8_t)py, (uint8_t)(int8_t)pr };
                                raton_entregar(ep, boot, 4);
                                dx -= px; dy -= py; rueda -= pr;
                                if (!dx && !dy && !rueda) break;
                            }
                        }
                    } else {
                        raton_entregar(ep, buf, tam_recibido);
                    }
                }
                // CASO A: Endpoint Boot estándar (prioridad absoluta para no confundir modificadores con Report IDs)
                else if (ep->es_boot) {
                    mod = buf[0];
                    shift = (mod & 0x02) || (mod & 0x20);
                    int ctrl = (mod & 0x01) || (mod & 0x10);

                    for (int k = 2; k < 8 && k < (int)tam_recibido; k++) {
                        uint8_t scancode = buf[k];
                        if (scancode == 0) continue;

                        int ya_estaba = 0;
                        for (int old = 2; old < 8 && old < (int)ep->ultimo_tam; old++) {
                            if (ep->ultimo_reporte[old] == scancode) {
                                ya_estaba = 1;
                                break;
                            }
                        }

                        if (!ya_estaba && scancode < 128) {
                            char c = scancode == 0x52 ? (ctrl ? TECLA_CTRL_ARRIBA : TECLA_ARRIBA) : scancode == 0x51 ? (ctrl ? TECLA_CTRL_ABAJO : TECLA_ABAJO) : scancode == 0x50 ? TECLA_IZQUIERDA : scancode == 0x4F ? TECLA_DERECHA : scancode == 0x4C ? TECLA_SUPRIMIR : scancode == 0x3B ? TECLA_F2 : (shift ? g_hid_a_ascii_shift[scancode] : g_hid_a_ascii_normal[scancode]);
                            if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                                c = (c >= 'a' && c <= 'z') ? (c - 'a' + 1) : (c - 'A' + 1);
                            }
                            if (c != 0) {
                                uint32_t sig_cabeza = (g_buf_cabeza + 1) % TAM_BUFFER_TECLAS;
                                if (sig_cabeza != g_buf_cola) {
                                    g_buffer_teclado[g_buf_cabeza] = c;
                                    g_buf_cabeza = sig_cabeza;
                                    g_estado.ultimo_slot_tecla = ep->slot_id;
                                    g_estado.ultimo_puerto_tecla = ep->puerto_idx;
                                    g_estado.ultimo_caracter = (uint8_t)c;
                                } else g_fase0.teclas_perdidas++;   /* H204 */
                            }
                        }
                    }
                }
                // CASO B: Report ID = 0x04 (NKRO - N-Key Rollover en 16 bytes: 1 byte ID + 15 bytes mapa de bits de 120 teclas)
                else if (tam_recibido >= 16 && buf[0] == 0x04) {
                    for (int byte_idx = 1; byte_idx < 16 && byte_idx < (int)tam_recibido; byte_idx++) {
                        uint8_t actual = buf[byte_idx];
                        uint8_t anterior = (byte_idx < (int)ep->ultimo_tam) ? ep->ultimo_reporte[byte_idx] : 0;
                        uint8_t recien_pulsados = actual & ~anterior;

                        if (recien_pulsados) {
                            for (int bit = 0; bit < 8; bit++) {
                                if (recien_pulsados & (1 << bit)) {
                                    uint8_t scancode = (uint8_t)(4 + (byte_idx - 1) * 8 + bit);
                                    if (scancode < 128) {
                                        char c = g_hid_a_ascii_normal[scancode];
                                        if (c != 0) {
                                            uint32_t sig_cabeza = (g_buf_cabeza + 1) % TAM_BUFFER_TECLAS;
                                            if (sig_cabeza != g_buf_cola) {
                                                g_buffer_teclado[g_buf_cabeza] = c;
                                                g_buf_cabeza = sig_cabeza;
                                                g_estado.ultimo_slot_tecla = ep->slot_id;
                                                g_estado.ultimo_puerto_tecla = ep->puerto_idx;
                                                g_estado.ultimo_caracter = (uint8_t)c;
                                            } else g_fase0.teclas_perdidas++;   /* H204 */
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // CASO C: Teclado compuesto no-Boot con Report ID = 0x01 o 0x02 (byte 0=ReportID, byte 1=mod, byte 2..7=teclas)
                else if (tam_recibido >= 8 && (buf[0] == 0x01 || buf[0] == 0x02)) {
                    mod = buf[1];
                    shift = (mod & 0x02) || (mod & 0x20);
                    int ctrl = (mod & 0x01) || (mod & 0x10);

                    for (int k = 2; k < 8 && k < (int)tam_recibido; k++) {
                        uint8_t scancode = buf[k];
                        if (scancode == 0) continue;

                        int ya_estaba = 0;
                        for (int old = 2; old < 8 && old < (int)ep->ultimo_tam; old++) {
                            if (ep->ultimo_reporte[old] == scancode) {
                                ya_estaba = 1;
                                break;
                            }
                        }

                        if (!ya_estaba && scancode < 128) {
                            char c = scancode == 0x52 ? (ctrl ? TECLA_CTRL_ARRIBA : TECLA_ARRIBA) : scancode == 0x51 ? (ctrl ? TECLA_CTRL_ABAJO : TECLA_ABAJO) : scancode == 0x50 ? TECLA_IZQUIERDA : scancode == 0x4F ? TECLA_DERECHA : scancode == 0x4C ? TECLA_SUPRIMIR : scancode == 0x3B ? TECLA_F2 : (shift ? g_hid_a_ascii_shift[scancode] : g_hid_a_ascii_normal[scancode]);
                            if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                                c = (c >= 'a' && c <= 'z') ? (c - 'a' + 1) : (c - 'A' + 1);
                            }
                            if (c != 0) {
                                uint32_t sig_cabeza = (g_buf_cabeza + 1) % TAM_BUFFER_TECLAS;
                                if (sig_cabeza != g_buf_cola) {
                                    g_buffer_teclado[g_buf_cabeza] = c;
                                    g_buf_cabeza = sig_cabeza;
                                    g_estado.ultimo_slot_tecla = ep->slot_id;
                                    g_estado.ultimo_puerto_tecla = ep->puerto_idx;
                                    g_estado.ultimo_caracter = (uint8_t)c;
                                } else g_fase0.teclas_perdidas++;   /* H204 */
                            }
                        }
                    }
                }
                // CASO D: Reporte Boot estándar plano de 8 bytes (fallback para endpoints sin marca es_boot)
                else if (tam_recibido >= 8) {
                    mod = buf[0];
                    shift = (mod & 0x02) || (mod & 0x20);
                    int ctrl = (mod & 0x01) || (mod & 0x10);

                    for (int k = 2; k < 8 && k < (int)tam_recibido; k++) {
                        uint8_t scancode = buf[k];
                        if (scancode == 0) continue;

                        int ya_estaba = 0;
                        for (int old = 2; old < 8 && old < (int)ep->ultimo_tam; old++) {
                            if (ep->ultimo_reporte[old] == scancode) {
                                ya_estaba = 1;
                                break;
                            }
                        }

                        if (!ya_estaba && scancode < 128) {
                            char c = scancode == 0x52 ? (ctrl ? TECLA_CTRL_ARRIBA : TECLA_ARRIBA) : scancode == 0x51 ? (ctrl ? TECLA_CTRL_ABAJO : TECLA_ABAJO) : scancode == 0x50 ? TECLA_IZQUIERDA : scancode == 0x4F ? TECLA_DERECHA : scancode == 0x4C ? TECLA_SUPRIMIR : scancode == 0x3B ? TECLA_F2 : (shift ? g_hid_a_ascii_shift[scancode] : g_hid_a_ascii_normal[scancode]);
                            if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                                c = (c >= 'a' && c <= 'z') ? (c - 'a' + 1) : (c - 'A' + 1);
                            }
                            if (c != 0) {
                                uint32_t sig_cabeza = (g_buf_cabeza + 1) % TAM_BUFFER_TECLAS;
                                if (sig_cabeza != g_buf_cola) {
                                    g_buffer_teclado[g_buf_cabeza] = c;
                                    g_buf_cabeza = sig_cabeza;
                                    g_estado.ultimo_slot_tecla = ep->slot_id;
                                    g_estado.ultimo_puerto_tecla = ep->puerto_idx;
                                    g_estado.ultimo_caracter = (uint8_t)c;
                                } else g_fase0.teclas_perdidas++;   /* H204 */
                            }
                        }
                    }
                }

                // Guardar copia del reporte en este endpoint
                ep->ultimo_tam = tam_recibido;
                for (uint32_t i = 0; i < tam_recibido; i++) {
                    ep->ultimo_reporte[i] = buf[i];
                }

                // Re-encolar Normal TRB con el ep_max_pkt de este endpoint
                volatile struct trb_xhci *trb = &ep->ring[ep->idx];
                trb->parametro = ep->bufer_fisica;
                trb->estado    = ep->ep_max_pkt;
                trb->control   = (TRB_TIPO_NORMAL << 10) | (1U << 2) /* ISP */ | (1U << 5) /* IOC */ | (ep->cycle ? 1 : 0);
                dma_sincronizar_cpu_a_dispositivo((const void *)trb, sizeof(*trb));

                ep->idx++;
                if (ep->idx >= XHCI_TAM_ANILLO - 1) {
                    volatile struct trb_xhci *link = &ep->ring[XHCI_TAM_ANILLO - 1];
                    link->parametro = ep->ring_fisica;
                    link->estado    = 0;
                    link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) /* TC */ | (ep->cycle ? 1 : 0);
                    dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
                    __asm__ volatile ("mfence" ::: "memory");
                    ep->idx = 0;
                    ep->cycle = !ep->cycle;
                }

                // Tocar timbre del Endpoint correspondiente
                xhci_tocar_timbre(ep->slot_id, ep->ep_dci);
            } else if (ep) {
                serial_imprimir("[HID ERROR] slot=");
                serial_imprimir_dec(slot_id);
                serial_imprimir(" dci=");
                serial_imprimir_dec(ep_dci);
                serial_imprimir(" completion_code=");
                serial_imprimir_dec(codigo);
                serial_imprimir_linea("");
                // En caso de error o advertencia, rearmar el TRB
                volatile struct trb_xhci *trb = &ep->ring[ep->idx];
                trb->parametro = ep->bufer_fisica;
                trb->estado    = ep->ep_max_pkt;
                trb->control   = (TRB_TIPO_NORMAL << 10) | (1U << 2) /* ISP */ | (1U << 5) /* IOC */ | (ep->cycle ? 1 : 0);
                dma_sincronizar_cpu_a_dispositivo((const void *)trb, sizeof(*trb));

                ep->idx++;
                if (ep->idx >= XHCI_TAM_ANILLO - 1) {
                    volatile struct trb_xhci *link = &ep->ring[XHCI_TAM_ANILLO - 1];
                    link->parametro = ep->ring_fisica;
                    link->estado    = 0;
                    link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) /* TC */ | (ep->cycle ? 1 : 0);
                    dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
                    __asm__ volatile ("mfence" ::: "memory");
                    ep->idx = 0;
                    ep->cycle = !ep->cycle;
                }
                xhci_tocar_timbre(ep->slot_id, ep->ep_dci);
            }
        }

}

static int sondeo_breve;
void xhci_configurar_sondeo_breve(int activo){sondeo_breve=activo;}
/* H210, Fase B2: con los hilos de CPU 1 encendidos, ellos bombean y escanean; CPU 0 no hace nada aqui. Las llamadas
 * de siempre quedan como respaldo para cuando el hilo se apaga ('usb hilo apagar'). */
static int xhci_hilos_activos(void);
static void xhci_puertos_pasada(int breve);
void xhci_sondeo(void) {
    /* H210: con los hilos encendidos se vuelve sin tocar el cerrojo: CPU 0 lo pedia miles de veces por ms y el hilo
     * de eventos lo encontraba ocupado (huecos de 10 ms) */
    if (xhci_hilos_activos()) return;
    XHCI_EXCLUSIVO();
    if (!g_estado.inicializado) return;
    xhci_puertos_pasada(sondeo_breve);
}
/* El sondeo de siempre: escanear puertos, bombear el anillo, cambios pendientes, hubs y reintentos. breve: sin
 * enumeracion ni resets (el reproductor lo pide mientras un lote tiene dependencias). */
static void xhci_puertos_pasada(int sondeo_breve) {

    // Escanear cambios físicos en puertos raíz (Hotplug reactivo)
    // Se ejecuta de inmediato si el controlador reporta Port Change Detect (PCD) en USBSTS,
    // o periódicamente cada 200 ms como red de seguridad sin sobrecarga.
    static uint64_t g_ultimo_escaneo_puertos = 0;
    uint64_t ahora = tiempo_obtener_milisegundos();
    uint32_t usbsts = mmio_leer32(g_op_base + REG_OP_USBSTS);
    if (!sondeo_breve && ((usbsts & USBSTS_PCD) || (ahora - g_ultimo_escaneo_puertos >= 200))) {
        g_ultimo_escaneo_puertos = ahora;
        if (usbsts & USBSTS_PCD) {
            mmio_escribir32(g_op_base + REG_OP_USBSTS, USBSTS_PCD);
        }
        xhci_escanear_cambios_puertos(1);
    }

    xhci_bombear_eventos();
    if (!sondeo_breve && g_cambio_puerto_pendiente) {
        g_cambio_puerto_pendiente = 0;
        xhci_escanear_cambios_puertos(1);
    }
    if (!sondeo_breve) xhci_hubs_sondear();   /* H139: conexiones en los puertos de los hubs, cada 500 ms */
    if (!sondeo_breve) xhci_reintentos_atender();   /* H204: reintentos de reset que ya tocan */
}

int xhci_hay_datos(void) {
    /* H210: sin cerrojo si los hilos llenan el bufer: solo se comparan dos indices */
    if (xhci_hilos_activos()) return __atomic_load_n(&g_buf_cabeza, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_buf_cola, __ATOMIC_RELAXED);
    XHCI_EXCLUSIVO();
    xhci_sondeo();
    return (g_buf_cabeza != g_buf_cola);
}

char xhci_leer_caracter(void) {
    /* H210: si no hay nada, se vuelve sin cerrojo; si hay, se toma para sacar la tecla */
    if (xhci_hilos_activos() && __atomic_load_n(&g_buf_cabeza, __ATOMIC_ACQUIRE) == __atomic_load_n(&g_buf_cola, __ATOMIC_RELAXED)) return 0;
    XHCI_EXCLUSIVO();
    if (!g_estado.inicializado) return 0;
    xhci_sondeo();
    if (g_buf_cabeza == g_buf_cola) return 0;
    char c = g_buffer_teclado[g_buf_cola];
    g_buf_cola = (g_buf_cola + 1) % TAM_BUFFER_TECLAS;
    return c;
}

const struct estado_xhci *xhci_obtener_estado(void) {
    return &g_estado;
}

// --- TRANSFERENCIAS DE CONTROL USB VÍA EP0 RING ---
// Envía un paquete de control USB (Setup → Data → Status) sobre el anillo EP0 del slot dado.
// Devuelve 0 si la transferencia completó, código de error si falló.
static int xhci_transferencia_control(uint8_t slot_id, uint8_t tipo_peticion, uint8_t peticion,
                                       uint16_t valor, uint16_t indice, uint16_t longitud,
                                       uint8_t *datos, uint64_t datos_fisica) {
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS || !g_slot_ep0_ring[slot_id]) return -1;
    volatile struct trb_xhci *ep0_ring = g_slot_ep0_ring[slot_id];
    uint64_t ep0_ring_fisica = g_slot_ep0_ring_fisica[slot_id];
    uint32_t ep0_idx = g_slot_ep0_idx[slot_id];
    uint8_t ep0_cycle = g_slot_ep0_cycle[slot_id];

    g_estado.ultima_peticion_control = peticion;
    g_estado.ultimo_codigo_control = 0xFF;
    g_estado.ultimo_valor_control = valor;
    g_estado.ultimo_indice_control = indice;
    g_estado.ultimo_largo_control = longitud;
    g_estado.transferencias_control++;

    /* H139: aqui habia un enlace anticipado (en la posicion 60, 61 o 62) para que Setup, Data y Status quedaran
     * juntos. Dejaba posiciones sin escribir en esa vuelta; el anillo nace en ceros (ciclo 0), y en las vueltas de
     * ciclo 0 esos ceros parecian TRB validos: el controlador seguia de largo y se salia del anillo. Aparecio al
     * sondear hubs (cientos de transferencias por EP0). No hace falta: Setup, Data y Status son TD distintos
     * (xHCI 1.2, 4.11.2.2) y un enlace entre ellos es legal. El enlace queda siempre en la ultima posicion. */

    // --- Setup Stage TRB ---
    volatile struct trb_xhci *setup = &ep0_ring[ep0_idx];
    // El paquete de setup va empaquetado en el campo parametro (8 bytes little-endian)
    uint64_t setup_data = (uint64_t)tipo_peticion |
                          ((uint64_t)peticion << 8) |
                          ((uint64_t)valor << 16) |
                          ((uint64_t)indice << 32) |
                          ((uint64_t)longitud << 48);
    setup->parametro = setup_data;
    setup->estado    = 8; // Siempre 8 bytes para setup
    uint32_t trt = 0; // Transfer Type
    if (longitud > 0) {
        trt = (tipo_peticion & 0x80) ? 3 : 2; // 3 = IN Data, 2 = OUT Data
    }
    setup->control = (TRB_TIPO_SETUP_STAGE << 10) | (1U << 6) /* IDT */ | (trt << 16) | (ep0_cycle ? 1 : 0);

    ep0_idx++;
    if (ep0_idx >= XHCI_TAM_ANILLO - 1) {
        volatile struct trb_xhci *link = &ep0_ring[XHCI_TAM_ANILLO - 1];
        link->parametro = ep0_ring_fisica;
        link->estado    = 0;
        link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) | (ep0_cycle ? 1 : 0);
        dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
        __asm__ volatile ("mfence" ::: "memory");
        ep0_idx = 0;
        ep0_cycle = !ep0_cycle;
    }

    // --- Data Stage TRB (si hay datos) ---
    if (longitud > 0 && datos && datos_fisica) {
        volatile struct trb_xhci *data_trb = &ep0_ring[ep0_idx];
        data_trb->parametro = datos_fisica;
        data_trb->estado    = longitud;
        uint32_t dir_flag = (tipo_peticion & 0x80) ? (1U << 16) : 0; // DIR: 1=IN, 0=OUT
        data_trb->control = (TRB_TIPO_DATA_STAGE << 10) | dir_flag | (ep0_cycle ? 1 : 0);

        ep0_idx++;
        if (ep0_idx >= XHCI_TAM_ANILLO - 1) {
            volatile struct trb_xhci *link = &ep0_ring[XHCI_TAM_ANILLO - 1];
            link->parametro = ep0_ring_fisica;
            link->estado    = 0;
            link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) | (ep0_cycle ? 1 : 0);
            dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
            __asm__ volatile ("mfence" ::: "memory");
            ep0_idx = 0;
            ep0_cycle = !ep0_cycle;
        }
    }

    // --- Status Stage TRB ---
    volatile struct trb_xhci *status_trb = &ep0_ring[ep0_idx];
    status_trb->parametro = 0;
    status_trb->estado    = 0;
    // Según especificación oficial USB 2.0/3.x y xHCI 1.2 §4.11.2.2:
    // - Si longitud > 0 y (tipo_peticion & 0x80): Control Read (GET_DESCRIPTOR). Datos vienen IN, status es OUT (DIR = 0).
    // - Si longitud == 0: Control sin datos (SET_CONFIGURATION, SET_PROTOCOL, SET_IDLE).
    //   La etapa de estado es un handshake IN del dispositivo al host. DIR DEBE ser 1 (IN, bit 16).
    // - Si longitud > 0 y !(tipo_peticion & 0x80): Control Write con datos (SET_REPORT).
    //   Datos van OUT, status es IN (DIR = 1).
    uint32_t status_dir = (longitud > 0 && (tipo_peticion & 0x80)) ? 0 : (1U << 16);
    status_trb->control = (TRB_TIPO_STATUS_STAGE << 10) | (1U << 5) /* IOC */ | status_dir | (ep0_cycle ? 1 : 0);

    ep0_idx++;
    if (ep0_idx >= XHCI_TAM_ANILLO - 1) {
        volatile struct trb_xhci *link = &ep0_ring[XHCI_TAM_ANILLO - 1];
        link->parametro = ep0_ring_fisica;
        link->estado    = 0;
        link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) | (ep0_cycle ? 1 : 0);
        dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
        __asm__ volatile ("mfence" ::: "memory");
        ep0_idx = 0;
        ep0_cycle = !ep0_cycle;
    }

    g_slot_ep0_idx[slot_id] = ep0_idx;
    g_slot_ep0_cycle[slot_id] = ep0_cycle;
    dma_sincronizar_cpu_a_dispositivo((const void *)ep0_ring, XHCI_TAM_ANILLO * sizeof(*ep0_ring));

    g_eventos_transfer[slot_id][1].listo = 0;
    // Tocar timbre de EP0 (Target = DCI 1 = EP0)
    xhci_tocar_timbre(slot_id, 1);

    int timeout = 2000;
    while (timeout > 0) {
        xhci_bombear_eventos();
        struct xhci_evento_pendiente *b = &g_eventos_transfer[slot_id][1];
        if (b->listo) {
            uint8_t codigo = (b->trb.estado >> 24) & 0xFF;
            b->listo = 0;
            g_estado.ultimo_codigo_control = codigo;
            if (codigo == 1 || codigo == 13) {
                if (datos && longitud > 0) dma_sincronizar_dispositivo_a_cpu(datos, longitud);
                return 0;
            }
            g_estado.fallos_control++;
            return (int)codigo;
        }
        esperar_milisegundos(1);
        timeout--;
    }

    serial_imprimir_linea("  [xHCI AVISO] Timeout en transferencia de control");
    {
        /* H139: que hay en la cabeza del anillo de eventos (un evento sin dueño detiene a todos los demas) */
        volatile struct trb_xhci *cab = &g_event_ring[g_event_idx];
        uint8_t ct = (cab->control >> 10) & 0x3F, cs = (cab->control >> 24) & 0xFF, cd = (cab->control >> 16) & 0x1F;
        serial_imprimir("  [xHCI AVISO] Pedido slot="); serial_imprimir_dec(slot_id);
        serial_imprimir(" | cabeza de eventos: tipo="); serial_imprimir_dec(ct);
        serial_imprimir(" slot="); serial_imprimir_dec(cs); serial_imprimir(" dci="); serial_imprimir_dec(cd);
        serial_imprimir(" cc="); serial_imprimir_dec((cab->estado >> 24) & 0xFF);
        serial_imprimir(" ciclo_ok="); serial_imprimir_dec((cab->control & 1U) == g_event_cycle);
        serial_imprimir(" comando_pendiente="); serial_imprimir_dec(g_evento_comando.listo);
        serial_imprimir(" transfer_pendiente="); serial_imprimir_dec(cs <= XHCI_MAX_SLOTS ? g_eventos_transfer[cs][cd].listo : 99);
        serial_imprimir_linea("");
        if (g_slot_dev_ctx[slot_id]) {
            /* Estado del slot (DW3 bits 31:27) y de EP0 (DW0 bits 2:0: 1 corre, 2 detenido por error, 3 parado, 4 error) */
            dma_sincronizar_dispositivo_a_cpu(g_slot_dev_ctx[slot_id], 2 * g_tamano_contexto);
            const uint32_t *sc = (const uint32_t *)g_slot_dev_ctx[slot_id];
            const uint32_t *e0 = (const uint32_t *)(g_slot_dev_ctx[slot_id] + g_tamano_contexto);
            serial_imprimir("  [xHCI AVISO] slot_estado="); serial_imprimir_dec(sc[3] >> 27);
            serial_imprimir(" ep0_estado="); serial_imprimir_dec(e0[0] & 7);
            serial_imprimir(" ep0_deq="); serial_imprimir_hex(((uint64_t)e0[3] << 32) | (e0[2] & ~0xFu));
            serial_imprimir(" anillo_idx="); serial_imprimir_dec(g_slot_ep0_idx[slot_id]);
            serial_imprimir(" usbsts="); serial_imprimir_hex(mmio_leer32(g_op_base + REG_OP_USBSTS));
            serial_imprimir_linea("");
            uint64_t base = g_slot_ep0_ring_fisica[slot_id], deq = ((uint64_t)e0[3] << 32) | (e0[2] & ~0xFu);
            int64_t pos = (int64_t)(deq - base) / 16;
            serial_imprimir("  [xHCI AVISO] anillo base="); serial_imprimir_hex(base);
            serial_imprimir(" posicion del controlador="); serial_imprimir_dec((uint64_t)pos);
            serial_imprimir(" ciclo_productor="); serial_imprimir_dec(g_slot_ep0_cycle[slot_id]);
            serial_imprimir(" dcs="); serial_imprimir_dec(e0[2] & 1);
            if (pos >= 0 && pos < XHCI_TAM_ANILLO) {
                volatile struct trb_xhci *t = &g_slot_ep0_ring[slot_id][pos];
                serial_imprimir(" TRB alli: tipo="); serial_imprimir_dec((t->control >> 10) & 0x3F);
                serial_imprimir(" ciclo="); serial_imprimir_dec(t->control & 1);
            }
            serial_imprimir_linea("");
        }
    }
    g_estado.fallos_control++;
    return -1;
}

// Ejecuta una transferencia síncrona en un endpoint Bulk IN o Bulk OUT
int xhci_bulk_limpiar_halt(uint8_t slot_id,uint8_t ep_dci){ XHCI_EXCLUSIVO();
    if(!g_estado.inicializado || !slot_id || slot_id>XHCI_MAX_SLOTS || ep_dci<2 || ep_dci>31)return -1;
    struct xhci_ep_bulk *ep=NULL;
    for(unsigned i=0;i<XHCI_MAX_BULK_EPS;i++)if(g_bulk_eps[i].activo && g_bulk_eps[i].slot_id==slot_id && g_bulk_eps[i].ep_dci==ep_dci){ep=&g_bulk_eps[i];break;}
    if(!ep || !ep->ring || ep->idx>=XHCI_TAM_ANILLO-1)return -1;
    uint32_t target=((uint32_t)slot_id<<24)|((uint32_t)ep_dci<<16);
    uint16_t addr=(ep_dci/2)|((ep_dci&1)?0x80:0);
    uint64_t dequeue=ep->ring_fisica+(uint64_t)ep->idx*sizeof(struct trb_xhci);
    struct trb_xhci evento;
#ifdef TAEK_XHCI_HOST_TEST
    extern int xhci_prueba_recuperacion(unsigned,uint64_t,uint32_t);
    if(xhci_prueba_recuperacion(1,0,target|(TRB_TIPO_RESET_EP<<10)))return -2;
    if(xhci_prueba_recuperacion(2,addr,slot_id))return -3;
    if(xhci_prueba_recuperacion(3,dequeue|ep->cycle,target|(TRB_TIPO_SET_DEQUEUE<<10)))return -4;
    (void)evento;
#else
    if(xhci_enviar_comando(0,0,target|(TRB_TIPO_RESET_EP<<10),&evento))return -2;
    if(xhci_transferencia_control(slot_id,0x02,1,0,addr,0,NULL,0))return -3;
    if(xhci_enviar_comando(dequeue|ep->cycle,0,target|(TRB_TIPO_SET_DEQUEUE<<10),&evento))return -4;
#endif
    g_eventos_transfer[slot_id][ep_dci].listo=0;return 0;
}
int xhci_transferencia_bulk(uint8_t slot_id, uint8_t ep_dci, void *buffer, uint64_t buffer_fisica, uint32_t longitud, int es_in, int timeout_ms) { XHCI_EXCLUSIVO();
    if (!g_estado.inicializado || slot_id == 0 || ep_dci == 0) return -1;

    // Fuera del alcance de un unico TRB (17 bits): repartir en varios.
    if (longitud > XHCI_BULK_TRAMO_MAX || longitud > 65536U-(uint32_t)(buffer_fisica&65535U)) {
        return xhci_transferencia_bulk_segmentada(slot_id, ep_dci, buffer,
                                                  buffer_fisica, longitud, es_in,
                                                  0, timeout_ms, NULL, NULL);
    }

    struct xhci_ep_bulk *bep = NULL;
    for (int i = 0; i < XHCI_MAX_BULK_EPS; i++) {
        if (g_bulk_eps[i].activo && g_bulk_eps[i].slot_id == slot_id && g_bulk_eps[i].ep_dci == ep_dci) {
            bep = &g_bulk_eps[i];
            break;
        }
    }
    if (!bep || !bep->ring) {
        serial_imprimir("  [xHCI BULK ERROR] Endpoint no encontrado: Slot=");
        serial_imprimir_dec(slot_id);
        serial_imprimir(" DCI=");
        serial_imprimir_dec(ep_dci);
        serial_imprimir_linea("");
        return -2;
    }

    if (!es_in && buffer && longitud > 0) {
        dma_sincronizar_cpu_a_dispositivo(buffer, longitud);
    }

    volatile struct trb_xhci *trb = &bep->ring[bep->idx];
    trb->parametro = buffer_fisica;
    trb->estado    = longitud; // Transfer Length
    uint32_t ctrl  = (TRB_TIPO_NORMAL << 10) | (1U << 5) /* IOC */ | (bep->cycle ? 1 : 0);
    if (es_in) ctrl |= (1U << 2); // ISP: Interrupt on Short Packet
    trb->control   = ctrl;

    bep->idx++;
    if (bep->idx >= XHCI_TAM_ANILLO - 1) {
        volatile struct trb_xhci *link = &bep->ring[XHCI_TAM_ANILLO - 1];
        link->parametro = bep->ring_fisica;
        link->estado    = 0;
        link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) /* TC */ | (bep->cycle ? 1 : 0);
        dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
        bep->idx = 0;
        bep->cycle = !bep->cycle;
    }

    dma_sincronizar_cpu_a_dispositivo((const void *)bep->ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
    __asm__ volatile ("mfence" ::: "memory");

    g_eventos_transfer[slot_id][ep_dci].listo = 0;
    // Tocar el timbre del endpoint correspondiente (Target = ep_dci)
    xhci_tocar_timbre(slot_id, ep_dci);

    int timeout = (timeout_ms > 0) ? timeout_ms : 2000;
    while (timeout > 0) {
        uint64_t t_sondeo = rdtsc();
        xhci_bombear_eventos();
        g_bulk_ciclos_sondeo += rdtsc() - t_sondeo;
        struct xhci_evento_pendiente *b = &g_eventos_transfer[slot_id][ep_dci];
        if (b->listo) {
            uint8_t codigo = (b->trb.estado >> 24) & 0xFF;
            b->listo = 0;
            if (codigo == 1 || codigo == 13) {
                if (es_in && buffer && longitud > 0)
                    dma_sincronizar_dispositivo_a_cpu(buffer, longitud);
                return 0;
            }
            return -(int)codigo;
        }
        uint64_t t_espera = rdtsc();
        esperar_milisegundos(1);
        g_bulk_ciclos_espera += rdtsc() - t_espera;
        timeout--;
    }

    serial_imprimir_linea("  [xHCI AVISO] Timeout en transferencia Bulk");
    return -99;
}

// Encola un TRB Normal en el anillo del endpoint Bulk, gestionando el enlace
// de fin de anillo. Factoriza el mismo patron que usa la ruta no segmentada.
static void xhci_encolar_bulk_trb(struct xhci_ep_bulk *bep, uint64_t phys,
                                  uint32_t estado, uint32_t control) {
    volatile struct trb_xhci *trb = &bep->ring[bep->idx];
    trb->parametro = phys;
    trb->estado    = estado;
    trb->control   = control | (bep->cycle ? 1 : 0);

    bep->idx++;
    if (bep->idx >= XHCI_TAM_ANILLO - 1) {
        volatile struct trb_xhci *link = &bep->ring[XHCI_TAM_ANILLO - 1];
        link->parametro = bep->ring_fisica;
        link->estado    = 0;
        link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) /* TC */ |
            (control & (1U << 4)) /* TD continua tras el Link */ | (bep->cycle ? 1 : 0);
        dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
        bep->idx = 0;
        bep->cycle = !bep->cycle;
    }
}

// Transferencia Bulk segmentada dentro de un unico TD y con IOC solo final.
// La finalizacion fecha el DATA completo; no mide el primer byte/paquete.
int xhci_transferencia_bulk_segmentada(uint8_t slot_id, uint8_t ep_dci,
                                       void *buffer, uint64_t buffer_fisica,
                                       uint32_t longitud, int es_in,
                                       uint32_t primer_tramo, int timeout_ms,
                                       uint64_t *ciclos_primer_evento,
                                       uint64_t *ciclos_fin_evento) { XHCI_EXCLUSIVO();
    if (ciclos_primer_evento) *ciclos_primer_evento = 0;
    if (ciclos_fin_evento) *ciclos_fin_evento = 0;

    if (!g_estado.inicializado || slot_id == 0 || ep_dci == 0) return -1;
    if (longitud == 0) return -1;

    struct xhci_ep_bulk *bep = NULL;
    for (int i = 0; i < XHCI_MAX_BULK_EPS; i++) {
        if (g_bulk_eps[i].activo && g_bulk_eps[i].slot_id == slot_id &&
            g_bulk_eps[i].ep_dci == ep_dci) {
            bep = &g_bulk_eps[i];
            break;
        }
    }
    if (!bep || !bep->ring) return -2;

    if (!es_in && buffer && longitud > 0) {
        dma_sincronizar_cpu_a_dispositivo(buffer, longitud);
    }

    struct xhci_bulk_tramo plan[XHCI_BULK_PLAN_MAX];
    int cuenta=xhci_bulk_planificar(buffer_fisica,longitud,primer_tramo,bep->ep_max_pkt,es_in,plan);
    if(cuenta<0)return cuenta; // validar antes de modificar el anillo
    uint32_t trbs=(uint32_t)cuenta,eventos=0;
    for(uint32_t i=0;i<trbs;++i) {
        g_captura_bulk.trbs[i]=bep->ring_fisica+(uint64_t)bep->idx*sizeof(struct trb_xhci);
        if(plan[i].control&(1U<<5))++eventos;
        xhci_encolar_bulk_trb(bep,plan[i].fisica,plan[i].estado,plan[i].control);
    }

    dma_sincronizar_cpu_a_dispositivo((const void *)bep->ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
    __asm__ volatile ("mfence" ::: "memory");

    g_eventos_transfer[slot_id][ep_dci].listo = 0;
    g_captura_bulk.activo = 1;
    g_captura_bulk.slot = slot_id;
    g_captura_bulk.dci = ep_dci;
    g_captura_bulk.esperados = eventos;
    g_captura_bulk.recibidos = 0;
    g_captura_bulk.cantidad_trbs=trbs;g_captura_bulk.terminado=0;g_captura_bulk.fallo=0;

    xhci_tocar_timbre(slot_id, ep_dci);

    int timeout = (timeout_ms > 0) ? timeout_ms : 2000;
    while (timeout > 0 && !g_captura_bulk.terminado) {
        uint64_t t_sondeo = rdtsc();
        xhci_bombear_eventos();
        g_bulk_ciclos_sondeo += rdtsc() - t_sondeo;
        if (g_captura_bulk.terminado) break;
        uint64_t t_espera = rdtsc();
        esperar_milisegundos(1);
        g_bulk_ciclos_espera += rdtsc() - t_espera;
        timeout--;
    }

    int resultado = 0;
    if (!g_captura_bulk.terminado) {
        serial_imprimir_linea("  [xHCI AVISO] Timeout en transferencia Bulk segmentada");
        g_captura_bulk.activo = 0;
        return -99;
    }

    if (ciclos_primer_evento) *ciclos_primer_evento = g_captura_bulk.ciclos[0];
    if (ciclos_fin_evento) *ciclos_fin_evento = g_captura_bulk.ciclos[g_captura_bulk.recibidos - 1];

    for (uint32_t i = 0; i < g_captura_bulk.recibidos; i++) {
        uint8_t cc = g_captura_bulk.codigos[i];
        if (cc != 1 && cc != 13) { resultado = -(int)cc; break; }
    }
    if(g_captura_bulk.fallo) {
        resultado=g_captura_bulk.fallo;
        serial_imprimir("[XHCI_BULK_ERROR] SLOT=");serial_imprimir_dec(slot_id);
        serial_imprimir(" DCI=");serial_imprimir_dec(ep_dci);
        serial_imprimir(" CC=");serial_imprimir_dec((uint64_t)(-resultado));
        serial_imprimir(" TRB=");serial_imprimir_hex(g_captura_bulk.trb_error);
        serial_imprimir_linea("");
    }
    if (resultado == 0 && es_in && buffer && longitud > 0) {
        dma_sincronizar_dispositivo_a_cpu(buffer, longitud);
    }

    g_captura_bulk.activo = 0;
    return resultado;
}

// --- TELEMETRIA EXPORTADA HACIA USB MSC ---
uint64_t xhci_telemetria_espera_ciclos(void) { return g_bulk_ciclos_espera; }
uint64_t xhci_telemetria_sondeo_ciclos(void) { return g_bulk_ciclos_sondeo; }
uint64_t xhci_telemetria_timbre_ciclos(void) { return g_ciclos_timbre; }
void     xhci_telemetria_reiniciar(void) { XHCI_EXCLUSIVO();
    g_bulk_ciclos_espera = 0;
    g_bulk_ciclos_sondeo = 0;
    g_ciclos_timbre = 0;
}

// Libera un Device Slot de hardware limpiando la entrada DCBAA y liberando búferes DMA (xHCI 1.2 §4.3.4)
static void xhci_liberar_slot(uint8_t slot_id,
                             uint8_t *in_ctx, uint64_t in_ctx_fisica,
                             uint8_t *dev_ctx, uint64_t dev_ctx_fisica,
                             uint8_t *desc_buf, uint64_t desc_buf_fisica) {
    if (slot_id > 0) {
        serial_imprimir("  [xHCI] Liberando Slot ID ");
        serial_imprimir_dec(slot_id);
        serial_imprimir_linea("...");
        xhci_enviar_comando(0, 0, (TRB_TIPO_DISABLE_SLOT << 10) | ((uint32_t)slot_id << 24), NULL);

        // Limpieza mandatoria de DCBAA (xHCI 1.2 §4.3.4) y sincronización a DRAM
        if (g_dcbaa && slot_id <= g_estado.max_slots) {
            g_dcbaa[slot_id] = 0;
            dma_sincronizar_cpu_a_dispositivo(&g_dcbaa[slot_id], sizeof(uint64_t));
            serial_imprimir_linea("  [xHCI] DCBAA[slot] limpiado a 0.");
        }
    }

    if (desc_buf && desc_buf_fisica) {
        dma_liberar_bufer_contiguo(desc_buf, desc_buf_fisica, 1024);
    }
    if (in_ctx && in_ctx_fisica) {
        dma_liberar_bufer_contiguo(in_ctx, in_ctx_fisica, 33 * g_tamano_contexto);
    }
    if (dev_ctx && dev_ctx_fisica) {
        dma_liberar_bufer_contiguo(dev_ctx, dev_ctx_fisica, 32 * g_tamano_contexto);
    }
}

/* H138: lee el descriptor de reporte de una interfaz de raton (GET_DESCRIPTOR de clase 0x22, HID 1.11 7.1.1) y, si
 * declara un raton relativo, lo deja en ep->raton. 1 si se usara el protocolo reporte; 0 para seguir con boot. Es lo
 * que hacen Linux y Windows: algunos receptores aceptan SET_PROTOCOL(boot) y luego no mandan nada en ese modo. */
static int xhci_raton_modo_reporte(uint8_t slot_id, uint8_t iface, struct xhci_ep_teclado *ep) {
    uint64_t fis = 0;
    uint8_t *buf = (uint8_t *)dma_asignar_bufer_contiguo(1024, 64, &fis);
    if (!buf) return 0;
    int r = xhci_transferencia_control(slot_id, 0x81, 0x06, 0x2200, iface, ep->hid_rep_lon, buf, fis);
    int ok = 0;
    if (!r) {
        dma_sincronizar_dispositivo_a_cpu(buf, ep->hid_rep_lon);
        ok = hid_buscar_raton(buf, ep->hid_rep_lon, &ep->raton) == 0 && ep->raton.relativo;
    }
    dma_liberar_bufer_contiguo(buf, fis, 1024);
    consola_imprimir("    -> Raton interfaz ");
    consola_imprimir_dec(iface);
    if (r) { consola_imprimir(": sin descriptor de reporte (codigo "); consola_imprimir_dec((uint64_t)(uint32_t)r); consola_imprimir_linea("), modo boot"); return 0; }
    if (!ok) { consola_imprimir_linea(": descriptor sin raton relativo, modo boot"); return 0; }
    consola_imprimir(": modo reporte, ID "); consola_imprimir_dec(ep->raton.id);
    consola_imprimir(", X "); consola_imprimir_dec(ep->raton.x.tam); consola_imprimir(" bits, Y ");
    consola_imprimir_dec(ep->raton.y.tam); consola_imprimir(" bits, "); consola_imprimir_dec(ep->raton.n_botones);
    consola_imprimir_linea(" botones");
    return 1;
}

// --- CONFIGURACIÓN DE PUERTOS Y TECLADO USB HID ---
static void xhci_configurar_puerto(uint8_t puerto_idx, uint32_t portsc) {
    g_estado.etapa_enumeracion = 1;
    uint8_t velocidad = (portsc >> 10) & 0x0F;
    consola_imprimir("  -> [xHCI] Configurando Puerto ");
    consola_imprimir_dec(puerto_idx);
    consola_imprimir(" (Velocidad: ");
    consola_imprimir_dec(velocidad);
    consola_imprimir_linea(")...");

    serial_imprimir("  [xHCI] Dispositivo detectado en Puerto ");
    serial_imprimir_dec(puerto_idx);
    serial_imprimir(" (Velocidad: ");
    serial_imprimir_dec(velocidad);
    serial_imprimir_linea(")...");

    g_enum.slot = 0;
    // Si este puerto ya tenía un slot asignado previamente, liberarlo antes de reconfigurar.
    // Detras de un hub el puerto raiz es el del hub: su slot no se toca.
    if (!g_enum.ruta && puerto_idx <= XHCI_MAX_PUERTOS && g_puerto_slot[puerto_idx] > 0) {
        uint8_t prev_slot = g_puerto_slot[puerto_idx];
        for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
            if (g_teclado_eps[e].activo && g_teclado_eps[e].slot_id == prev_slot) {
                g_teclado_eps[e].activo = 0;
            }
        }
        xhci_liberar_slot(prev_slot, NULL, 0, g_slot_dev_ctx[prev_slot], g_slot_dev_ctx_fisica[prev_slot], NULL, 0);
    }

    // 1. Enable Slot
    struct trb_xhci evt_slot;
    int res = xhci_enviar_comando(0, 0, (TRB_TIPO_ENABLE_SLOT << 10), &evt_slot);
    if (res != 0) {
        if (res == -1) {
            consola_imprimir_linea_color("    [!] Enable Slot falló: TIMEOUT (5000ms sin respuesta xHCI)", COLOR_ERROR_DEFAULT);
        } else {
            consola_imprimir_color("    [!] Error en Enable Slot (código: ", COLOR_ERROR_DEFAULT);
            consola_imprimir_dec(res);
            consola_imprimir_linea_color(")", COLOR_ERROR_DEFAULT);
        }
        serial_imprimir("  [xHCI ERROR] Falló comando Enable Slot (código: ");
        serial_imprimir_dec(res);
        serial_imprimir_linea(")");
        return;
    }

    uint8_t slot_id = (evt_slot.control >> 24) & 0xFF;
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) {
        serial_imprimir_linea("  [xHCI ERROR] Slot ID fuera de rango máximo permitido");
        return;
    }
    consola_imprimir("    -> Slot asignado: ID ");
    consola_imprimir_dec(slot_id);
    consola_imprimir_linea("");
    serial_imprimir("  [xHCI] Slot asignado con éxito: ID ");
    serial_imprimir_dec(slot_id);
    serial_imprimir_linea("");

    // 2. Asignar Device Context y registrarlo en DCBAA
    uint64_t dev_ctx_fisica = 0;
    uint8_t *dev_ctx = (uint8_t *)dma_asignar_bufer_contiguo(32 * g_tamano_contexto, 64, &dev_ctx_fisica);
    if (!dev_ctx) {
        consola_imprimir_linea_color("    [!] Error al asignar Device Context en DMA", COLOR_ERROR_DEFAULT);
        xhci_liberar_slot(slot_id, NULL, 0, NULL, 0, NULL, 0);
        return;
    }
    for (uint32_t i = 0; i < 32 * g_tamano_contexto; i++) dev_ctx[i] = 0;
    dma_sincronizar_cpu_a_dispositivo(dev_ctx, 32 * g_tamano_contexto);
    g_dcbaa[slot_id] = dev_ctx_fisica;
    dma_sincronizar_cpu_a_dispositivo(g_dcbaa, (g_estado.max_slots + 1) * sizeof(*g_dcbaa));

    // 3. Preparar Input Context para Address Device (solo Slot + EP0)
    uint64_t in_ctx_fisica = 0;
    uint8_t *in_ctx = (uint8_t *)dma_asignar_bufer_contiguo(33 * g_tamano_contexto, 64, &in_ctx_fisica);
    if (!in_ctx) {
        consola_imprimir_linea_color("    [!] Error al asignar Input Context en DMA", COLOR_ERROR_DEFAULT);
        xhci_liberar_slot(slot_id, NULL, 0, dev_ctx, dev_ctx_fisica, NULL, 0);
        return;
    }
    for (uint32_t i = 0; i < 33 * g_tamano_contexto; i++) in_ctx[i] = 0;

    // Reiniciar el EP0 ring dedicado para este slot
    if (!g_slot_ep0_ring[slot_id]) {
        g_slot_ep0_ring[slot_id] = (volatile struct trb_xhci *)dma_asignar_bufer_contiguo(XHCI_TAM_ANILLO * sizeof(struct trb_xhci), 64, &g_slot_ep0_ring_fisica[slot_id]);
        if (!g_slot_ep0_ring[slot_id]) {
            consola_imprimir_linea_color("    [!] Error al asignar EP0 Ring en DMA", COLOR_ERROR_DEFAULT);
            xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, NULL, 0);
            return;
        }
    }
    for (int i = 0; i < XHCI_TAM_ANILLO; i++) {
        g_slot_ep0_ring[slot_id][i].parametro = 0;
        g_slot_ep0_ring[slot_id][i].estado = 0;
        g_slot_ep0_ring[slot_id][i].control = 0;
    }
    g_slot_ep0_idx[slot_id] = 0;
    g_slot_ep0_cycle[slot_id] = 1;
    dma_sincronizar_cpu_a_dispositivo((const void *)g_slot_ep0_ring[slot_id], XHCI_TAM_ANILLO * sizeof(struct trb_xhci));

    // Input Control Context: Add Flags (Slot Context bit 0, EP0 bit 1)
    uint32_t *ctrl_ctx = (uint32_t *)in_ctx;
    ctrl_ctx[1] = (1U << 0) | (1U << 1);

    // Slot Context (offset g_tamano_contexto)
    uint32_t *slot_ctx = (uint32_t *)(in_ctx + g_tamano_contexto);
    slot_ctx[0] = (1U << 27) | ((uint32_t)velocidad << 20) | (g_enum.ruta & 0xFFFFF); // 1 context entry, speed, route string
    slot_ctx[1] = ((uint32_t)puerto_idx << 16);             // Root hub port
    slot_ctx[2] = (uint32_t)g_enum.tt_slot | ((uint32_t)g_enum.tt_puerto << 8);   // TT del hub de alta velocidad, si hace falta

    // EP0 Context (offset 2 * g_tamano_contexto)
    uint32_t *ep0_ctx = (uint32_t *)(in_ctx + 2 * g_tamano_contexto);
    // Según especificación oficial Intel xHCI 1.2 Sección 4.3.3:
    // Full-Speed (1) y Low-Speed (2) DEBEN inicializarse con MaxPacketSize = 8 en el contexto EP0
    // antes de emitir Address Device (BSR=0). High-Speed (3) usa 64, SuperSpeed (4+) usa 512.
    uint32_t max_paquete = (velocidad >= 4) ? 512 : ((velocidad == 3) ? 64 : 8);
    ep0_ctx[1] = (4U << 3) | (max_paquete << 16) | (3U << 1); // EP Type 4 (Control), Max Packet, CErr=3
    ep0_ctx[2] = (uint32_t)(g_slot_ep0_ring_fisica[slot_id] | 1);         // Dequeue pointer + DCS=1
    ep0_ctx[3] = (uint32_t)(g_slot_ep0_ring_fisica[slot_id] >> 32);
    ep0_ctx[4] = 8;                                          // Average TRB length

    // 4. Enviar Address Device (con BSR=0 para que asigne dirección real)
    struct trb_xhci evt_addr;
    dma_sincronizar_cpu_a_dispositivo(in_ctx, 33 * g_tamano_contexto);
    xhci_volcar_input_context_crudo("ADDRESS DEVICE", in_ctx);
    res = xhci_enviar_comando(in_ctx_fisica, 0, (TRB_TIPO_ADDRESS_DEV << 10) | ((uint32_t)slot_id << 24), &evt_addr);
    if (res != 0) {
        if (res == -1) {
            consola_imprimir_linea_color("    [!] Address Device falló: TIMEOUT (5000ms sin respuesta xHCI)", COLOR_ERROR_DEFAULT);
        } else {
            consola_imprimir_color("    [!] Error en Address Device (código: ", COLOR_ERROR_DEFAULT);
            consola_imprimir_dec(res);
            consola_imprimir_linea_color(")", COLOR_ERROR_DEFAULT);
        }
        serial_imprimir("  [xHCI ERROR] Falló Address Device (código: ");
        serial_imprimir_dec(res);
        serial_imprimir_linea(")");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, NULL, 0);
        return;
    }
    consola_imprimir_linea_color("    -> Dispositivo direccionado [OK]", COLOR_EXITO_DEFAULT);
    serial_imprimir_linea("  [xHCI] Dispositivo direccionado correctamente.");
    g_estado.etapa_enumeracion = 2;
    esperar_milisegundos(10); // Permitir que el dispositivo se estabilice

    // 5. Búfer temporal DMA para descriptores USB (1024 bytes)
    uint64_t desc_buf_fisica = 0;
    uint8_t *desc_buf = (uint8_t *)dma_asignar_bufer_contiguo(1024, 64, &desc_buf_fisica);
    if (!desc_buf) {
        consola_imprimir_linea_color("    [!] Error al asignar búfer de descriptores", COLOR_ERROR_DEFAULT);
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, NULL, 0);
        return;
    }
    for (int i = 0; i < 1024; i++) desc_buf[i] = 0;

    // 6. GET_DESCRIPTOR(Device) en 2 fases estándar según especificación USB 2.0 / xHCI 4.3.3:
    // Fase 1: Leer únicamente los primeros 8 bytes con wLength=8.
    // Al pedir solo 8 bytes, el dispositivo jamás puede enviar más de 8 bytes físicamente por el bus,
    // impidiendo de raíz el desbordamiento de búfer en silicio (Babble Detected Error, Código 3).
    for (int i = 0; i < 1024; i++) desc_buf[i] = 0;
    res = xhci_transferencia_control(slot_id, 0x80, 0x06, 0x0100, 0x0000, 8, desc_buf, desc_buf_fisica);
    if (res != 0) {
        consola_imprimir_color("    [!] Falló lectura primeros 8B descriptor (código: ", COLOR_AVISO_DEFAULT);
        consola_imprimir_dec(res);
        consola_imprimir_linea_color(")", COLOR_AVISO_DEFAULT);
        serial_imprimir("  [xHCI AVISO] No se pudieron leer primeros 8 bytes de Device Descriptor (código ");
        serial_imprimir_dec(res);
        serial_imprimir_linea("). Omitiendo dispositivo.");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, desc_buf, desc_buf_fisica);
        return;
    }

    // Byte 7 contiene bMaxPacketSize0 real del dispositivo
    uint8_t real_max_pkt0 = desc_buf[7];
    // En SuperSpeed (velocidad >= 4), bMaxPacketSize0 se codifica como exponente 2^n (e.g. 9 -> 512)
    uint32_t nuevo_max_paquete = (velocidad >= 4) ? (1U << real_max_pkt0) : real_max_pkt0;
    if (nuevo_max_paquete == 0 || nuevo_max_paquete > 1024) nuevo_max_paquete = 8;

    // Fase 2: Si el tamaño real de paquete de EP0 difiere del valor inicial asumido (8 bytes en Full-Speed),
    // actualizar el contexto de EP0 en el controlador de hardware mediante Evaluate Context.
    if (nuevo_max_paquete != max_paquete) {
        consola_imprimir("    -> Actualizando MaxPkt0 EP0: ");
        consola_imprimir_dec(max_paquete);
        consola_imprimir(" -> ");
        consola_imprimir_dec(nuevo_max_paquete);

        serial_imprimir("  [xHCI] Actualizando MaxPacketSize0 de EP0: ");
        serial_imprimir_dec(max_paquete);
        serial_imprimir(" -> ");
        serial_imprimir_dec(nuevo_max_paquete);
        serial_imprimir_linea(" vía Evaluate Context...");

        for (uint32_t i = 0; i < 33 * g_tamano_contexto; i++) in_ctx[i] = 0;
        ctrl_ctx[0] = 0;          // Drop Flags = 0
        // Según Intel xHCI 1.2 Sección 4.6.7 (pág. 126-127), el Input Control Context
        // debe tener activas las banderas A0 (Slot Context) y A1 (EP0 Context).
        ctrl_ctx[1] = (1U << 0) | (1U << 1);

        uint32_t *eval_slot_ctx = (uint32_t *)(in_ctx + g_tamano_contexto);
        eval_slot_ctx[0] = (1U << 27) | (g_enum.ruta & 0xFFFFF); // Context Entries = 1. Speed (bits 23:20) es RsvdZ en Evaluate Context!
        eval_slot_ctx[1] = ((uint32_t)puerto_idx << 16);             // Root hub port
        eval_slot_ctx[2] = (uint32_t)g_enum.tt_slot | ((uint32_t)g_enum.tt_puerto << 8);

        uint32_t *eval_ep0_ctx = (uint32_t *)(in_ctx + 2 * g_tamano_contexto);
        eval_ep0_ctx[1] = (4U << 3) | (nuevo_max_paquete << 16) | (3U << 1); // EP Type 4 (Control), MaxPacket, CErr=3
        eval_ep0_ctx[4] = nuevo_max_paquete;

        struct trb_xhci evt_eval;
        dma_sincronizar_cpu_a_dispositivo(in_ctx, 33 * g_tamano_contexto);
        xhci_volcar_input_context_crudo("EVALUATE CONTEXT", in_ctx);
        res = xhci_enviar_comando(in_ctx_fisica, 0, (TRB_TIPO_EVAL_CTX << 10) | ((uint32_t)slot_id << 24), &evt_eval);
        if (res == 0) {
            max_paquete = nuevo_max_paquete;
            consola_imprimir_linea_color(" [OK]", COLOR_EXITO_DEFAULT);
            serial_imprimir_linea("  [xHCI] Evaluate Context completado [OK].");
        } else {
            consola_imprimir_color(" [Aviso cód: ", COLOR_AVISO_DEFAULT);
            consola_imprimir_dec(res);
            consola_imprimir_linea_color("]", COLOR_AVISO_DEFAULT);
            serial_imprimir("  [xHCI AVISO] Evaluate Context retornó código: ");
            serial_imprimir_dec(res);
            serial_imprimir_linea("");
        }
    }

    // Fase 3: Con el contexto de EP0 perfectamente sincronizado con el silicio del dispositivo,
    // leer el Device Descriptor completo de 18 bytes. No habrá Babble Error porque MaxPacketSize ya es compatible.
    for (int i = 0; i < 1024; i++) desc_buf[i] = 0;
    res = xhci_transferencia_control(slot_id, 0x80, 0x06, 0x0100, 0x0000, 18, desc_buf, desc_buf_fisica);
    if (res != 0) {
        consola_imprimir_color("    [!] Falló descriptor de dispositivo (código: ", COLOR_AVISO_DEFAULT);
        consola_imprimir_dec(res);
        consola_imprimir_linea_color(")", COLOR_AVISO_DEFAULT);
        serial_imprimir("  [xHCI AVISO] No se pudo leer Device Descriptor completo (código ");
        serial_imprimir_dec(res);
        serial_imprimir_linea("). Omitiendo dispositivo.");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, desc_buf, desc_buf_fisica);
        return;
    }

    struct usb_descriptor_dispositivo *dev_desc = (struct usb_descriptor_dispositivo *)desc_buf;
    /* El mismo bufer se reutiliza para el descriptor de configuracion: VID y PID se copian ahora (H138: antes se
     * leian despues y salia el consumo del dispositivo, p. ej. 0932:0004). */
    const uint16_t vid_dispositivo = dev_desc->id_proveedor, pid_dispositivo = dev_desc->id_producto;
    const uint8_t clase_disp = dev_desc->clase_dispositivo;   // 9 = hub (H139)
    g_estado.etapa_enumeracion = 3;
    g_estado.teclado_id_proveedor = dev_desc->id_proveedor;
    g_estado.teclado_id_producto  = dev_desc->id_producto;
    consola_imprimir("    -> USB VID:");
    consola_imprimir_hex(dev_desc->id_proveedor);
    consola_imprimir(" PID:");
    consola_imprimir_hex(dev_desc->id_producto);
    consola_imprimir(" Clase:");
    consola_imprimir_dec(dev_desc->clase_dispositivo);
    consola_imprimir_linea("");

    serial_imprimir("  [xHCI] Dispositivo USB: VID=");
    serial_imprimir_hex(dev_desc->id_proveedor);
    serial_imprimir(" PID=");
    serial_imprimir_hex(dev_desc->id_producto);
    serial_imprimir(" Clase=");
    serial_imprimir_dec(dev_desc->clase_dispositivo);
    serial_imprimir(" MaxPkt=");
    serial_imprimir_dec(dev_desc->tamano_max_paquete_ep0);
    serial_imprimir_linea("");

    // 7. GET_DESCRIPTOR(Configuration) en 2 pasos dinámicos (wTotalLength real)
    // Paso A: Leer encabezado de 9 bytes para extraer wTotalLength
    for (int i = 0; i < 1024; i++) desc_buf[i] = 0;
    res = xhci_transferencia_control(slot_id, 0x80, 0x06, 0x0200, 0x0000, 9, desc_buf, desc_buf_fisica);
    if (res != 0) {
        consola_imprimir_color("    [!] Falló encabezado de configuración (código: ", COLOR_AVISO_DEFAULT);
        consola_imprimir_dec(res);
        consola_imprimir_linea_color(")", COLOR_AVISO_DEFAULT);
        serial_imprimir_linea("  [xHCI AVISO] No se pudo leer encabezado de Configuration Descriptor. Omitiendo.");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, desc_buf, desc_buf_fisica);
        return;
    }

    uint16_t total_cfg_len = desc_buf[2] | (desc_buf[3] << 8);
    if (total_cfg_len < 9) total_cfg_len = 9;
    if (total_cfg_len > 1024) total_cfg_len = 1024;

    consola_imprimir("    -> Configuración wTotalLength: ");
    consola_imprimir_dec(total_cfg_len);
    consola_imprimir_linea(" bytes");

    serial_imprimir("  [xHCI] Configuration Descriptor wTotalLength: ");
    serial_imprimir_dec(total_cfg_len);
    serial_imprimir_linea(" bytes. Leyendo descriptor completo...");

    // Paso B: Leer la longitud completa exacta de la configuración
    for (int i = 0; i < 1024; i++) desc_buf[i] = 0;
    res = xhci_transferencia_control(slot_id, 0x80, 0x06, 0x0200, 0x0000, total_cfg_len, desc_buf, desc_buf_fisica);
    if (res != 0) {
        consola_imprimir_color("    [!] Falló lectura de configuración completa (código: ", COLOR_AVISO_DEFAULT);
        consola_imprimir_dec(res);
        consola_imprimir_linea_color(")", COLOR_AVISO_DEFAULT);
        serial_imprimir_linea("  [xHCI AVISO] No se pudo leer Configuration Descriptor completo. Omitiendo.");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, desc_buf, desc_buf_fisica);
        return;
    }

    uint8_t config_val = desc_buf[5];
    g_estado.etapa_enumeracion = 4;
    int offset = 0;
    int iface_actual_num = -1;
    int iface_actual_es_hid = 0;
    int iface_actual_es_boot = 0;
    int iface_actual_es_raton = 0;
    int iface_actual_es_msc = 0;
    uint16_t iface_actual_rep_lon = 0;   // Del descriptor HID (0x21) de la interfaz actual
    int es_hub = (clase_disp == 9);

    struct xhci_ep_teclado *eps_este_dispositivo[4] = {0};
    int num_eps_este_dispositivo = 0;

    uint8_t  msc_ep_in_dci = 0;
    uint16_t msc_ep_in_max_pkt = 0;

    uint8_t  msc_ep_out_dci = 0;
    uint16_t msc_ep_out_max_pkt = 0;
    int      es_dispositivo_msc = 0;

    // Escanear todas las interfaces y registrar endpoints
    while (offset < total_cfg_len) {
        uint8_t len = desc_buf[offset];
        if (len == 0 || offset + len > total_cfg_len) break;
        uint8_t dtype = desc_buf[offset + 1];

        if (dtype == 0x21 && len >= 9 && iface_actual_es_hid && desc_buf[offset + 6] == 0x22) {
            // Descriptor HID: el primer descriptor de clase que declara es el de reporte, con su largo
            iface_actual_rep_lon = (uint16_t)(desc_buf[offset + 7] | desc_buf[offset + 8] << 8);
        } else if (dtype == 4 && len >= 9) { // Interface Descriptor
            iface_actual_num = desc_buf[offset + 2];
            iface_actual_rep_lon = 0;
            uint8_t if_class    = desc_buf[offset + 5];
            uint8_t if_subclass = desc_buf[offset + 6];
            uint8_t if_protocol = desc_buf[offset + 7];

            serial_imprimir("  [xHCI] Interfaz ");
            serial_imprimir_dec(iface_actual_num);
            serial_imprimir(": Clase=");
            serial_imprimir_dec(if_class);
            serial_imprimir(" SubClase=");
            serial_imprimir_dec(if_subclass);
            serial_imprimir(" Protocolo=");
            serial_imprimir_dec(if_protocol);

            // Aceptamos HID teclado/ratón y Mass Storage.
            if (if_class == 3) {
                iface_actual_es_hid = 1;
                iface_actual_es_boot = (if_subclass == 1 && if_protocol == 1);
                iface_actual_es_raton = (if_subclass == 1 && if_protocol == 2);
                iface_actual_es_msc = 0;
                serial_imprimir_linea(iface_actual_es_raton ? " [HID Ratón Aceptado]" :
                                      " [HID Teclado Aceptado]");
            } else if (if_class == 9) { // Hub (H139): se atiende aparte, sin endpoints aqui
                iface_actual_es_hid = 0;
                iface_actual_es_boot = 0;
                iface_actual_es_raton = 0;
                iface_actual_es_msc = 0;
                es_hub = 1;
                serial_imprimir_linea(" [Hub]");
            } else if (if_class == 8) { // Mass Storage Class (MSC)
                iface_actual_es_hid = 0;
                iface_actual_es_boot = 0;
                iface_actual_es_raton = 0;
                iface_actual_es_msc = 1;
                serial_imprimir_linea(" [USB Mass Storage (MSC) Aceptada]");
            } else {
                iface_actual_es_hid = 0;
                iface_actual_es_boot = 0;
                iface_actual_es_raton = 0;
                iface_actual_es_msc = 0;
                serial_imprimir_linea(" [Ignorada]");
            }
        } else if (dtype == 5 && len >= 7 && iface_actual_es_hid) { // Endpoint Descriptor HID
            uint8_t ep_addr = desc_buf[offset + 2];
            uint8_t ep_attr = desc_buf[offset + 3];
            uint16_t ep_max_pkt = desc_buf[offset + 4] | (desc_buf[offset + 5] << 8);
            uint8_t ep_intervalo = desc_buf[offset + 6];

            // Si es Endpoint Interrupt IN
            if ((ep_addr & 0x80) && ((ep_attr & 0x03) == 0x03)) {
                if (num_eps_este_dispositivo < 4) {
                    uint8_t ep_num = ep_addr & 0x0F;
                    uint8_t ep_dci = ep_num * 2 + 1;
                    if (ep_max_pkt == 0) ep_max_pkt = 8;

                    // Buscar una entrada disponible en g_teclado_eps
                    struct xhci_ep_teclado *ep = NULL;
                    for (int i = 0; i < XHCI_MAX_TECLADO_EPS; i++) {
                        if (!g_teclado_eps[i].activo || g_teclado_eps[i].slot_id == slot_id) {
                            int ya_elegida = 0;
                            for (int sel = 0; sel < num_eps_este_dispositivo; sel++) {
                                if (eps_este_dispositivo[sel] == &g_teclado_eps[i]) {
                                    ya_elegida = 1;
                                    break;
                                }
                            }
                            if (!ya_elegida) {
                                ep = &g_teclado_eps[i];
                                break;
                            }
                        }
                    }

                    if (ep) {
                        ep->slot_id = slot_id;
                        ep->puerto_idx = puerto_idx;
                        ep->ep_addr = ep_addr;
                        ep->ep_dci = ep_dci;
                        ep->ep_max_pkt = ep_max_pkt;
                        ep->ep_intervalo = ep_intervalo;
                        ep->iface_num = (uint8_t)iface_actual_num;
                        ep->es_boot = iface_actual_es_boot;
                        ep->es_raton = iface_actual_es_raton;
                        ep->raton_boot_confirmado=0;
                        ep->hid_rep_lon = iface_actual_rep_lon;
                        ep->raton_reporte = 0;
                        if(++g_mouse_generacion==0)g_mouse_generacion++;
                        ep->raton_generacion=g_mouse_generacion;
                        ep->activo = 0;
                        ep->idx = 0;
                        ep->cycle = 1;
                        ep->ultimo_tam = 0;
                        for (int k = 0; k < 64; k++) ep->ultimo_reporte[k] = 0;

                        serial_imprimir("    -> Endpoint Interrupt IN #");
                        serial_imprimir_dec(num_eps_este_dispositivo + 1);
                        serial_imprimir(": Addr=");
                        serial_imprimir_hex(ep_addr);
                        serial_imprimir(" (DCI=");
                        serial_imprimir_dec(ep_dci);
                        serial_imprimir(") MaxPkt=");
                        serial_imprimir_dec(ep_max_pkt);
                        serial_imprimir(" Interval=");
                        serial_imprimir_dec(ep_intervalo);
                        serial_imprimir_linea("ms");

                        eps_este_dispositivo[num_eps_este_dispositivo++] = ep;
                    }
                }
            }
        } else if (dtype == 5 && len >= 7 && iface_actual_es_msc) { // Endpoint Descriptor Bulk MSC
            uint8_t ep_addr = desc_buf[offset + 2];
            uint8_t ep_attr = desc_buf[offset + 3];
            uint16_t ep_max_pkt = desc_buf[offset + 4] | (desc_buf[offset + 5] << 8);

            // Bulk Endpoint (ep_attr & 0x03 == 0x02)
            if ((ep_attr & 0x03) == 0x02) {
                uint8_t ep_num = ep_addr & 0x0F;
                if (ep_addr & 0x80) { // Bulk IN
                    msc_ep_in_dci = ep_num * 2 + 1;
                    msc_ep_in_max_pkt = (ep_max_pkt > 0) ? ep_max_pkt : 512;
                    serial_imprimir("    -> Endpoint Bulk IN: Addr=");
                    serial_imprimir_hex(ep_addr);
                    serial_imprimir(" (DCI=");
                    serial_imprimir_dec(msc_ep_in_dci);
                    serial_imprimir(") MaxPkt=");
                    serial_imprimir_dec(msc_ep_in_max_pkt);
                    serial_imprimir_linea("");
                } else { // Bulk OUT
                    msc_ep_out_dci = ep_num * 2;
                    msc_ep_out_max_pkt = (ep_max_pkt > 0) ? ep_max_pkt : 512;
                    serial_imprimir("    -> Endpoint Bulk OUT: Addr=");
                    serial_imprimir_hex(ep_addr);
                    serial_imprimir(" (DCI=");
                    serial_imprimir_dec(msc_ep_out_dci);
                    serial_imprimir(") MaxPkt=");
                    serial_imprimir_dec(msc_ep_out_max_pkt);
                    serial_imprimir_linea("");
                }
                if (msc_ep_in_dci > 0 && msc_ep_out_dci > 0) {
                    es_dispositivo_msc = 1;
                }
            }
        }

        offset += len;
    }

    if (es_hub) {
        /* Un hub se queda con su slot: se registran sus mapeos y se encienden y recorren sus puertos */
        consola_imprimir_linea_color("    -> Hub USB detectado", COLOR_EXITO_DEFAULT);
        if (!g_enum.ruta) { g_slot_puerto[slot_id] = puerto_idx; g_puerto_slot[puerto_idx] = slot_id; }
        else g_slot_puerto[slot_id] = puerto_idx;
        g_slot_vid[slot_id] = vid_dispositivo;
        g_slot_pid[slot_id] = pid_dispositivo;
        g_slot_dev_ctx[slot_id] = dev_ctx;
        g_slot_dev_ctx_fisica[slot_id] = dev_ctx_fisica;
        dma_liberar_bufer_contiguo(desc_buf, desc_buf_fisica, 1024);
        dma_liberar_bufer_contiguo(in_ctx, in_ctx_fisica, 33 * g_tamano_contexto);
        g_enum.slot = slot_id;
        xhci_hub_iniciar(slot_id, velocidad, config_val, puerto_idx);
        return;
    }

    if (num_eps_este_dispositivo == 0 && !es_dispositivo_msc) {
        consola_imprimir_linea_color("    -> No hay endpoint HID/MSC compatible (liberando slot)", COLOR_PROMPT_DEFAULT);
        serial_imprimir_linea("  [xHCI] No se encontraron endpoints HID ni MSC compatibles. Omitiendo.");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, desc_buf, desc_buf_fisica);
        return;
    }

    consola_imprimir("    -> Endpoints HID armados: ");
    consola_imprimir_dec(num_eps_este_dispositivo);
    consola_imprimir_linea("");

    // 8. Configure Endpoint en el host controller (xHCI 4.3.5 / Linux):
    // Es obligatorio por estándar xHCI emitir Configure Endpoint ANTES de SET_CONFIGURATION.
    g_estado.etapa_enumeracion = 5;

    // Sincronizar Device Context desde el controlador hacia la CPU para leer el Slot Context activo
    dma_sincronizar_dispositivo_a_cpu(dev_ctx, 32 * g_tamano_contexto);

    // Limpiar completamente el Input Context (33 contextos de tamaño g_tamano_contexto)
    for (uint32_t i = 0; i < 33 * g_tamano_contexto; i++) in_ctx[i] = 0;

    // Input Control Context (offset 0):
    // DW0: Drop Flags = 0
    // DW1: Add Flags (Bit 0 = Slot Context, Bit ep_dci = Endpoint Contexts)
    // DW2..DW7: RsvdZ (compatibilidad universal con todas las revisiones xHCI 1.0, 1.1 y 1.2)
    ctrl_ctx[0] = 0;          // Drop Context = 0
    ctrl_ctx[1] = (1U << 0);  // Add Slot Context (bit 0)
    ctrl_ctx[7] = 0;          // RsvdZ en xHCI 1.0/1.1; garantizado 0 universal

    // Calcular el DCI máximo entre todos los endpoints armados para este dispositivo
    uint8_t max_dci = 1;
    for (int e = 0; e < num_eps_este_dispositivo; e++) {
        uint8_t ep_dci = eps_este_dispositivo[e]->ep_dci;
        if (ep_dci > max_dci) max_dci = ep_dci;
        ctrl_ctx[1] |= (1U << ep_dci); // Add Endpoint Context bit
    }

    struct xhci_ep_bulk *bep_in = NULL;
    struct xhci_ep_bulk *bep_out = NULL;
    if (es_dispositivo_msc) {
        for (int b = 0; b < XHCI_MAX_BULK_EPS; b++) {
            if (!g_bulk_eps[b].activo || g_bulk_eps[b].slot_id == slot_id) {
                if (!bep_out) {
                    bep_out = &g_bulk_eps[b];
                } else if (!bep_in && bep_out != &g_bulk_eps[b]) {
                    bep_in = &g_bulk_eps[b];
                    break;
                }
            }
        }
        if (bep_in && bep_out) {
            bep_out->slot_id = slot_id;
            bep_out->ep_dci = msc_ep_out_dci;
            bep_out->ep_max_pkt = msc_ep_out_max_pkt;
            bep_out->es_in = 0;
            bep_out->activo = 1;
            bep_out->idx = 0;
            bep_out->cycle = 1;

            bep_in->slot_id = slot_id;
            bep_in->ep_dci = msc_ep_in_dci;
            bep_in->ep_max_pkt = msc_ep_in_max_pkt;
            bep_in->es_in = 1;
            bep_in->activo = 1;
            bep_in->idx = 0;
            bep_in->cycle = 1;

            ctrl_ctx[1] |= (1U << msc_ep_out_dci) | (1U << msc_ep_in_dci);
            if (msc_ep_out_dci > max_dci) max_dci = msc_ep_out_dci;
            if (msc_ep_in_dci > max_dci) max_dci = msc_ep_in_dci;
        }
    }

    // Input Slot Context (offset g_tamano_contexto):
    // Según xHCI 1.2 Tabla 6-27 (Slot Context) y §6.2.2.1:
    // En Configure Endpoint y Evaluate Context, el campo Speed (bits 23:20) es estrictamente RsvdZ (Reserved Zero).
    // Escribir un valor distinto de cero en Speed viola la norma y provoca rechazo de parámetros o congelamiento
    // en silicio real estricto como Intel Raptor Lake PCH.
    uint32_t *dev_slot_ctx = (uint32_t *)dev_ctx;
    uint32_t speed_val = (dev_slot_ctx[0] >> 20) & 0x0F;
    if (speed_val == 0) speed_val = velocidad;

    // DW0: Preservar Route String (bits 19:0), MTT (bit 25) y Hub (bit 26) con máscara 0x060FFFFF,
    // purgar Speed a 0 (bits 23:20 = RsvdZ), y asignar limpiamente Context Entries = max_dci (bits 31:27).
    slot_ctx[0] = (dev_slot_ctx[0] & 0x060FFFFF) | ((uint32_t)max_dci << 27);

    // DW1: Copiar Max Exit Latency (bits 15:0), Root Hub Port Number (bits 23:16), Number of Ports (bits 31:24).
    // Garantizar que Root Hub Port Number no sea 0 si el controlador no lo reflejó en dev_ctx.
    slot_ctx[1] = dev_slot_ctx[1];
    if (((slot_ctx[1] >> 16) & 0xFF) == 0) {
        slot_ctx[1] |= ((uint32_t)puerto_idx << 16);
    }

    // DW2: Copiar TT Hub Slot ID, TT Port Number, TTT, Interrupter Target
    slot_ctx[2] = dev_slot_ctx[2];

    // DW3: RsvdZ obligatorio por xHCI 1.2 (Slot State = 0, Device Address = 0)
    slot_ctx[3] = 0;

    // Telemetría directa para auditar el Slot Context antes de despachar el comando
    consola_imprimir("    -> SlotCtx: DW0=");
    consola_imprimir_hex(slot_ctx[0]);
    consola_imprimir(" DW1=");
    consola_imprimir_hex(slot_ctx[1]);
    consola_imprimir(" (Vel=");
    consola_imprimir_dec(speed_val);
    consola_imprimir(" Puerto=");
    consola_imprimir_dec((slot_ctx[1] >> 16) & 0xFF);
    consola_imprimir_linea(")");

    serial_imprimir("  [xHCI CFG] SlotCtx: DW0=");
    serial_imprimir_hex(slot_ctx[0]);
    serial_imprimir(" DW1=");
    serial_imprimir_hex(slot_ctx[1]);
    serial_imprimir(" DW2=");
    serial_imprimir_hex(slot_ctx[2]);
    serial_imprimir(" DW3=");
    serial_imprimir_hex(slot_ctx[3]);
    serial_imprimir(" (Speed=");
    serial_imprimir_dec(speed_val);
    serial_imprimir(" MaxDCI=");
    serial_imprimir_dec(max_dci);
    serial_imprimir_linea(")");

    // Configurar cada Endpoint Context individual (offset (ep_dci + 1) * g_tamano_contexto)
    for (int e = 0; e < num_eps_este_dispositivo; e++) {
        struct xhci_ep_teclado *ep = eps_este_dispositivo[e];
        uint32_t *ep_ctx = (uint32_t *)(in_ctx + (ep->ep_dci + 1) * g_tamano_contexto);

        // Reiniciar y limpiar el anillo del endpoint en DRAM
        for (int k = 0; k < XHCI_TAM_ANILLO; k++) {
            ep->ring[k].parametro = 0;
            ep->ring[k].estado = 0;
            ep->ring[k].control = 0;
        }
        ep->idx = 0;
        ep->cycle = 1;
        dma_sincronizar_cpu_a_dispositivo((const void *)ep->ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));

        // xHCI 1.2 §6.2.3.6: Interval para endpoints de interrupción
        uint32_t xhci_intervalo;
        if (velocidad >= 3) {
            // High-Speed: Interval = bInterval - 1 (rango válido 0 a 15)
            xhci_intervalo = (ep->ep_intervalo > 0) ? (ep->ep_intervalo - 1) : 0;
            if (xhci_intervalo > 15) xhci_intervalo = 15;
        } else {
            // Full/Low-Speed: Interval es exponente 3 a 18 (2^(Interval-3) ms)
            if (ep->ep_intervalo <= 1) xhci_intervalo = 3;       // 1 ms
            else if (ep->ep_intervalo <= 2) xhci_intervalo = 4;  // 2 ms
            else if (ep->ep_intervalo <= 4) xhci_intervalo = 5;  // 4 ms
            else if (ep->ep_intervalo <= 8) xhci_intervalo = 6;  // 8 ms
            else if (ep->ep_intervalo <= 16) xhci_intervalo = 7; // 16 ms
            else if (ep->ep_intervalo <= 32) xhci_intervalo = 8; // 32 ms
            else xhci_intervalo = 9;                             // 64 ms
        }

        ep_ctx[0] = (xhci_intervalo << 16); // Interval (bits 23:16)
        ep_ctx[1] = (7U << 3) | ((uint32_t)ep->ep_max_pkt << 16) | (3U << 1); // Interrupt IN (7), Max Packet, CErr=3
        ep_ctx[2] = (uint32_t)(ep->ring_fisica | 1); // Dequeue pointer + DCS=1
        ep_ctx[3] = (uint32_t)(ep->ring_fisica >> 32);
        // DW4: Average TRB Length (bits 15:0) Y Max ESIT Payload Low (bits 31:16)!
        ep_ctx[4] = (uint32_t)ep->ep_max_pkt | ((uint32_t)ep->ep_max_pkt << 16);

        serial_imprimir("  [xHCI CFG] EP");
        serial_imprimir_dec(ep->ep_dci);
        serial_imprimir(" Ctx: DW0=");
        serial_imprimir_hex(ep_ctx[0]);
        serial_imprimir(" DW1=");
        serial_imprimir_hex(ep_ctx[1]);
        serial_imprimir(" DW2=");
        serial_imprimir_hex(ep_ctx[2]);
        serial_imprimir(" DW4=");
        serial_imprimir_hex(ep_ctx[4]);
        serial_imprimir_linea("");
    }

    if (es_dispositivo_msc && bep_in && bep_out) {
        // Bulk OUT
        uint32_t *ep_ctx_out = (uint32_t *)(in_ctx + (msc_ep_out_dci + 1) * g_tamano_contexto);
        for (int k = 0; k < XHCI_TAM_ANILLO; k++) {
            bep_out->ring[k].parametro = 0;
            bep_out->ring[k].estado = 0;
            bep_out->ring[k].control = 0;
        }
        bep_out->idx = 0;
        bep_out->cycle = 1;
        dma_sincronizar_cpu_a_dispositivo((const void *)bep_out->ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));

        ep_ctx_out[0] = 0; // Interval = 0
        ep_ctx_out[1] = (2U << 3) | ((uint32_t)msc_ep_out_max_pkt << 16) | (3U << 1); // Bulk OUT = 2, CErr = 3
        ep_ctx_out[2] = (uint32_t)(bep_out->ring_fisica | 1); // Dequeue pointer + DCS=1
        ep_ctx_out[3] = (uint32_t)(bep_out->ring_fisica >> 32);
        ep_ctx_out[4] = (uint32_t)msc_ep_out_max_pkt; // Average TRB Length

        // Bulk IN
        uint32_t *ep_ctx_in = (uint32_t *)(in_ctx + (msc_ep_in_dci + 1) * g_tamano_contexto);
        for (int k = 0; k < XHCI_TAM_ANILLO; k++) {
            bep_in->ring[k].parametro = 0;
            bep_in->ring[k].estado = 0;
            bep_in->ring[k].control = 0;
        }
        bep_in->idx = 0;
        bep_in->cycle = 1;
        dma_sincronizar_cpu_a_dispositivo((const void *)bep_in->ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));

        ep_ctx_in[0] = 0; // Interval = 0
        ep_ctx_in[1] = (6U << 3) | ((uint32_t)msc_ep_in_max_pkt << 16) | (3U << 1); // Bulk IN = 6, CErr = 3
        ep_ctx_in[2] = (uint32_t)(bep_in->ring_fisica | 1); // Dequeue pointer + DCS=1
        ep_ctx_in[3] = (uint32_t)(bep_in->ring_fisica >> 32);
        ep_ctx_in[4] = (uint32_t)msc_ep_in_max_pkt; // Average TRB Length

        serial_imprimir("  [xHCI CFG] Endpoints Bulk Configurados: OUT DCI=");
        serial_imprimir_dec(msc_ep_out_dci);
        serial_imprimir(" IN DCI=");
        serial_imprimir_dec(msc_ep_in_dci);
        serial_imprimir_linea("");
    }

    struct trb_xhci evt_cfg;
    dma_sincronizar_cpu_a_dispositivo(in_ctx, 33 * g_tamano_contexto);
    xhci_volcar_input_context_crudo("CONFIGURE ENDPOINT", in_ctx);
    res = xhci_enviar_comando(in_ctx_fisica, 0, (TRB_TIPO_CONFIG_EP << 10) | ((uint32_t)slot_id << 24), &evt_cfg);
    if (res != 0) {
        consola_imprimir_color("    [!] Configure Endpoint falló (código: ", COLOR_ERROR_DEFAULT);
        consola_imprimir_dec(res);
        consola_imprimir_linea_color(")", COLOR_ERROR_DEFAULT);
        serial_imprimir("  [xHCI ERROR] Configure Endpoint falló (código: ");
        serial_imprimir_dec(res);
        serial_imprimir_linea(")");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, desc_buf, desc_buf_fisica);
        return;
    }
    consola_imprimir_linea_color("    -> Configure Endpoint [OK]", COLOR_EXITO_DEFAULT);
    serial_imprimir_linea("  [xHCI] Configure Endpoint completado [OK].");

    // 9. SET_CONFIGURATION(config_val) — activa la configuración en el silicio del dispositivo
    // Ahora el host controller ya tiene los endpoints configurados y está 100% listo para tráfico.
    g_estado.etapa_enumeracion = 6;
    res = xhci_transferencia_control(slot_id, 0x00, 0x09, config_val, 0x0000, 0, NULL, 0);
    if (res != 0) {
        consola_imprimir_color("    [!] SET_CONFIGURATION falló (código: ", COLOR_ERROR_DEFAULT);
        consola_imprimir_dec(res);
        consola_imprimir_linea_color(")", COLOR_ERROR_DEFAULT);
        serial_imprimir("  [xHCI AVISO] SET_CONFIGURATION falló (código ");
        serial_imprimir_dec(res);
        serial_imprimir_linea(")");
        xhci_liberar_slot(slot_id, in_ctx, in_ctx_fisica, dev_ctx, dev_ctx_fisica, desc_buf, desc_buf_fisica);
        return;
    }
    consola_imprimir_linea_color("    -> SET_CONFIGURATION [OK]", COLOR_EXITO_DEFAULT);
    esperar_milisegundos(10);

    if (es_dispositivo_msc && bep_in && bep_out) {
        consola_imprimir_linea_color("    ==> ¡Memoria USB (Mass Storage) Configurada [OK]!", COLOR_EXITO_DEFAULT);
        usb_msc_registrar_dispositivo(slot_id, puerto_idx, msc_ep_in_dci, msc_ep_out_dci, msc_ep_in_max_pkt, msc_ep_out_max_pkt);
    }

    if (num_eps_este_dispositivo > 0) {
        // 10. Configurar protocolos e idle para cada interfaz detectada.
        // SET_PROTOCOL solo es válido para HID Boot; enviarlo a interfaces NKRO
        // propietarias puede provocar STALL y dejar el teclado en mal estado.
        g_estado.etapa_enumeracion = 7;
        for (int e = 0; e < num_eps_este_dispositivo; e++) {
            uint8_t iface = eps_este_dispositivo[e]->iface_num;
            int interfaz_ya_configurada = 0;
            for (int previo = 0; previo < e; previo++) {
                if (eps_este_dispositivo[previo]->iface_num == iface) {
                    interfaz_ya_configurada = 1;
                    break;
                }
            }
            if (interfaz_ya_configurada) continue;

            struct xhci_ep_teclado *er = eps_este_dispositivo[e];
            if (er->es_raton && er->hid_rep_lon && er->hid_rep_lon <= 1024 && xhci_raton_modo_reporte(slot_id, iface, er)) {
                // Queda en protocolo reporte (el que tiene tras SET_CONFIGURATION): no se envia SET_PROTOCOL
                for (int m = 0; m < num_eps_este_dispositivo; m++)
                    if (eps_este_dispositivo[m]->iface_num == iface && eps_este_dispositivo[m]->es_raton) {
                        eps_este_dispositivo[m]->raton = er->raton;
                        eps_este_dispositivo[m]->raton_reporte = 1;
                        eps_este_dispositivo[m]->raton_boot_confirmado = 1;
                    }
            } else if (eps_este_dispositivo[e]->es_boot || eps_este_dispositivo[e]->es_raton) {
                // SET_PROTOCOL(Boot = 0)
                int res_p = xhci_transferencia_control(slot_id, 0x21, 0x0B, 0x0000, (uint16_t)iface, 0, NULL, 0);
                serial_imprimir("  [xHCI] SET_PROTOCOL(Boot=0) Iface=");
                serial_imprimir_dec(iface);
                serial_imprimir(" Código=");
                serial_imprimir_dec(res_p);
                serial_imprimir_linea("");
                if (res_p != 0) {
                    // En caso de STALL o rechazo, limpiar la característica ENDPOINT_HALT en EP0
                    xhci_transferencia_control(slot_id, 0x02, 0x01, 0x0000, 0x0000, 0, NULL, 0);
                }
                if(!res_p)for(int m=0;m<num_eps_este_dispositivo;m++)
                    if(eps_este_dispositivo[m]->iface_num==iface && eps_este_dispositivo[m]->es_raton)eps_este_dispositivo[m]->raton_boot_confirmado=1;
            }
            // SET_IDLE(0)
            int res_i = xhci_transferencia_control(slot_id, 0x21, 0x0A, 0x0000, (uint16_t)iface, 0, NULL, 0);
            serial_imprimir("  [xHCI] SET_IDLE(0) Iface=");
            serial_imprimir_dec(iface);
            serial_imprimir(" Código=");
            serial_imprimir_dec(res_i);
            serial_imprimir_linea("");
            if (res_i != 0) {
                // En caso de STALL o rechazo, limpiar la característica ENDPOINT_HALT en EP0
                xhci_transferencia_control(slot_id, 0x02, 0x01, 0x0000, 0x0000, 0, NULL, 0);
            }
        }
        esperar_milisegundos(5);

        // 11. Armar y tocar el timbre de cada Endpoint descubierto con pipeline multi-TRB
        for (int e = 0; e < num_eps_este_dispositivo; e++) {
            struct xhci_ep_teclado *ep = eps_este_dispositivo[e];

            // Reiniciar anillo de transferencia
            for (int k = 0; k < XHCI_TAM_ANILLO; k++) {
                ep->ring[k].parametro = 0;
                ep->ring[k].estado = 0;
                ep->ring[k].control = 0;
            }

            // Pre-inicializar el Link TRB en el último elemento (XHCI_TAM_ANILLO - 1)
            volatile struct trb_xhci *link = &ep->ring[XHCI_TAM_ANILLO - 1];
            link->parametro = ep->ring_fisica;
            link->estado    = 0;
            link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) /* TC */ | 1 /* Cycle inicial */;

            ep->idx = 0;
            ep->cycle = 1;

            // Encolar ráfaga inicial de 4 TRBs Normales para mantener un pipeline continuo sin inanición
            int trbs_iniciales = 4;
            for (int t = 0; t < trbs_iniciales && t < XHCI_TAM_ANILLO - 1; t++) {
                volatile struct trb_xhci *trb = &ep->ring[t];
                trb->parametro = ep->bufer_fisica;
                trb->estado    = ep->ep_max_pkt;
                trb->control   = (TRB_TIPO_NORMAL << 10) | (1U << 2) /* ISP */ | (1U << 5) /* IOC */ | (ep->cycle ? 1 : 0);
                ep->idx++;
            }

            dma_sincronizar_cpu_a_dispositivo((const void *)ep->ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
            __asm__ volatile ("mfence" ::: "memory");

            ep->activo = 1;

            // Tocar timbre del Endpoint
            xhci_tocar_timbre(slot_id, ep->ep_dci);
        }

        int primer_teclado = -1;
        for (int e = 0; e < num_eps_este_dispositivo; e++)
            if (!eps_este_dispositivo[e]->es_raton) { primer_teclado = e; break; }
        if (primer_teclado >= 0) {
            g_estado.teclado_slot_id = slot_id;
            g_estado.teclado_ep_dci  = eps_este_dispositivo[primer_teclado]->ep_dci;
            g_estado.teclado_num_eps = (uint8_t)num_eps_este_dispositivo;
            g_estado.teclado_puerto  = puerto_idx;
            g_estado.teclado_id_proveedor = vid_dispositivo;
            g_estado.teclado_id_producto  = pid_dispositivo;
        }
        g_estado.etapa_enumeracion = 8;
        xhci_recontar_hid();

        consola_imprimir(primer_teclado >= 0 ?
                         "    ==> ¡Teclado USB Configurado y Operativo [OK]! (" :
                         "    ==> ¡Ratón USB Configurado y Operativo [OK]! (");
        consola_imprimir_dec(num_eps_este_dispositivo);
        consola_imprimir_linea_color(" Endpoints)", COLOR_EXITO_DEFAULT);

        serial_imprimir(primer_teclado >= 0 ?
                        "  [xHCI EXITOSO] ¡Teclado USB en Slot " :
                        "  [xHCI EXITOSO] ¡Ratón USB en Slot ");
        serial_imprimir_dec(slot_id);
        serial_imprimir(" (Puerto ");
        serial_imprimir_dec(puerto_idx);
        serial_imprimir(") configurado con ");
        serial_imprimir_dec(num_eps_este_dispositivo);
        serial_imprimir_linea(" Endpoint(s) armados para recepción!");
    }

    // 12. Registrar ranura, mapeos y actualizar estado global (detras de un hub, el puerto raiz es del hub)
    g_slot_puerto[slot_id] = puerto_idx;
    if (!g_enum.ruta) g_puerto_slot[puerto_idx] = slot_id;
    g_enum.slot = slot_id;
    g_slot_vid[slot_id] = vid_dispositivo;
    g_slot_pid[slot_id] = pid_dispositivo;
    g_slot_dev_ctx[slot_id] = dev_ctx;
    g_slot_dev_ctx_fisica[slot_id] = dev_ctx_fisica;

    // Liberar búferes temporales DMA no requeridos por hardware post-enumeración
    if (desc_buf && desc_buf_fisica) {
        dma_liberar_bufer_contiguo(desc_buf, desc_buf_fisica, 1024);
    }
    if (in_ctx && in_ctx_fisica) {
        dma_liberar_bufer_contiguo(in_ctx, in_ctx_fisica, 33 * g_tamano_contexto);
    }

    // Guardar contexto activo de dispositivo para posterior liberación al desconectar
    g_teclado_dev_ctx = dev_ctx;
    g_teclado_dev_ctx_fisica = dev_ctx_fisica;
}

/* ======================================================================================================
 * H139: HUBS USB (clase 9, USB 2.0 capitulo 11 y USB 3.2 capitulo 10)
 *
 * Un hub es un dispositivo mas: se enumera igual y luego se le habla por su EP0 con peticiones de clase:
 *   - GET_DESCRIPTOR de hub (0x29 en USB 2, 0x2A en USB 3): cuantos puertos tiene y cuanto tarda en dar energia.
 *   - SET_FEATURE(PORT_POWER) en cada puerto, y esperar.
 *   - GET_STATUS de puerto: dos palabras, estado (bit 0 conectado, bit 1 habilitado, bit 9 Low Speed,
 *     bit 10 High Speed) y cambios (bit 0 cambio de conexion, bit 4 reinicio terminado).
 *   - SET_FEATURE(PORT_RESET), esperar el cambio de reinicio y borrarlo: el puerto queda habilitado y su
 *     velocidad dice como direccionar al dispositivo.
 * Al controlador hay que decirle que ese slot es un hub (bit Hub, numero de puertos y tiempo de su traductor) con
 * un Configure Endpoint que solo cambia el contexto de slot, como hace Linux en xhci_update_hub_device.
 * Los cambios de conexion se descubren preguntando cada 500 ms (el endpoint de interrupcion del hub no se usa).
 * ====================================================================================================== */
#define HUB_F_PORT_RESET          4
#define HUB_F_PORT_POWER          8
#define HUB_F_C_PORT_CONNECTION  16
#define HUB_F_C_PORT_RESET       20
#define HUB_F_C_BH_PORT_RESET    29

static uint8_t *g_hub_buf;
static uint64_t g_hub_buf_fis;

static int hub_ctrl(uint8_t slot, uint8_t tipo, uint8_t pet, uint16_t valor, uint16_t indice, uint16_t lon) {
    if (lon && !g_hub_buf) {
        g_hub_buf = (uint8_t *)dma_asignar_bufer_contiguo(64, 64, &g_hub_buf_fis);
        if (!g_hub_buf) return -1;
    }
    int r = xhci_transferencia_control(slot, tipo, pet, valor, indice, lon, lon ? g_hub_buf : NULL, lon ? g_hub_buf_fis : 0);
    if (!r && lon) dma_sincronizar_dispositivo_a_cpu(g_hub_buf, lon);
    return r;
}
static int hub_estado(const struct xhci_hub *h, uint8_t p, uint16_t *st, uint16_t *ch) {
    int r = hub_ctrl(h->slot, 0xA3, 0x00, 0, p, 4);
    if (r) return r;
    *st = (uint16_t)(g_hub_buf[0] | g_hub_buf[1] << 8);
    *ch = (uint16_t)(g_hub_buf[2] | g_hub_buf[3] << 8);
    return 0;
}

/* Suelta un slot y todo lo suyo (endpoints, disco, mapeos). Si es un hub, primero lo que cuelga de el. */
static void xhci_soltar_slot(uint8_t slot_id) {
    if (!slot_id || slot_id > XHCI_MAX_SLOTS) return;
    for (int h = 0; h < XHCI_MAX_HUBS; h++) {
        if (!g_hubs[h].activo) continue;
        if (g_hubs[h].slot == slot_id) {
            g_hubs[h].activo = 0;                        /* Antes de bajar: evita volver a entrar aqui */
            for (unsigned p = 1; p <= HUB_MAX_PUERTOS; p++)
                if (g_hubs[h].hijo[p]) { uint8_t s = g_hubs[h].hijo[p]; g_hubs[h].hijo[p] = 0; xhci_soltar_slot(s); }
        } else {
            for (unsigned p = 1; p <= HUB_MAX_PUERTOS; p++) if (g_hubs[h].hijo[p] == slot_id) g_hubs[h].hijo[p] = 0;
        }
    }
    for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
        if (g_teclado_eps[e].slot_id == slot_id) {
            g_teclado_eps[e].activo = 0;
            g_teclado_eps[e].slot_id = 0;
            g_teclado_eps[e].puerto_idx = 0;
        }
    }
    for (int b = 0; b < XHCI_MAX_BULK_EPS; b++) {
        if (g_bulk_eps[b].slot_id == slot_id) {
            g_bulk_eps[b].activo = 0;
            g_bulk_eps[b].slot_id = 0;
        }
    }
    usb_msc_desregistrar_dispositivo(slot_id);
    xhci_liberar_slot(slot_id, NULL, 0, g_slot_dev_ctx[slot_id], g_slot_dev_ctx_fisica[slot_id], NULL, 0);
    g_slot_dev_ctx[slot_id] = NULL;
    g_slot_dev_ctx_fisica[slot_id] = 0;
    g_slot_puerto[slot_id] = 0;
    g_slot_vid[slot_id] = 0;
    g_slot_pid[slot_id] = 0;
    xhci_recontar_hid();
    if (g_estado.teclado_slot_id == slot_id) {
        g_estado.teclado_slot_id = 0;
        g_estado.teclado_puerto = 0;
    }
}

/* Reinicia el puerto p del hub y enumera lo que haya, con su ruta y su traductor */
static void hub_conectar(struct xhci_hub *h, uint8_t p) {
    uint16_t st = 0, ch = 0;
    consola_imprimir("  -> [HUB] Slot "); consola_imprimir_dec(h->slot);
    consola_imprimir(" puerto "); consola_imprimir_dec(p); consola_imprimir_linea(": dispositivo conectado, reiniciando...");
    if (h->profundidad + 1 > 5) { consola_imprimir_linea_color("     [!] Mas de 5 niveles de hubs: USB no lo permite.", COLOR_AVISO_DEFAULT); return; }
    if (hub_ctrl(h->slot, 0x23, 0x03, HUB_F_PORT_RESET, p, 0)) { consola_imprimir_linea_color("     [!] El hub rechazo el reinicio del puerto.", COLOR_AVISO_DEFAULT); return; }
    uint64_t t = tiempo_obtener_milisegundos();
    int listo = 0;
    while (tiempo_obtener_milisegundos() - t < 500) {
        esperar_milisegundos(10);
        if (!hub_estado(h, p, &st, &ch) && (ch & 0x10)) { listo = 1; break; }
    }
    hub_ctrl(h->slot, 0x23, 0x01, HUB_F_C_PORT_RESET, p, 0);
    if (h->es_ss) hub_ctrl(h->slot, 0x23, 0x01, HUB_F_C_BH_PORT_RESET, p, 0);
    hub_ctrl(h->slot, 0x23, 0x01, HUB_F_C_PORT_CONNECTION, p, 0);
    if (!listo || hub_estado(h, p, &st, &ch) || !(st & 1) || !(st & 2)) {
        consola_imprimir("     [!] El puerto no quedo habilitado (estado="); consola_imprimir_hex(st); consola_imprimir_linea(")");
        return;
    }
    /* Velocidad con la numeracion de xHCI: 1 Full, 2 Low, 3 High, 4 Super */
    uint8_t vel = h->es_ss ? 4 : (st & 0x0200) ? 2 : (st & 0x0400) ? 3 : 1;
    esperar_milisegundos(20);                 /* Recuperacion tras el reinicio (USB 2.0, 7.1.7.5: 10 ms minimo) */

    __typeof__(g_enum) guardado = g_enum;
    g_enum.ruta = h->ruta | ((uint32_t)(p > 15 ? 15 : p) << (4 * h->profundidad));
    g_enum.profundidad = (uint8_t)(h->profundidad + 1);
    if (h->velocidad == 3 && vel <= 2) { g_enum.tt_slot = h->slot; g_enum.tt_puerto = p; }      /* Lento tras hub rapido */
    else if (h->velocidad <= 2) { g_enum.tt_slot = h->tt_slot; g_enum.tt_puerto = h->tt_puerto; } /* Hub lento: hereda */
    else { g_enum.tt_slot = 0; g_enum.tt_puerto = 0; }
    xhci_configurar_puerto(h->raiz, (uint32_t)vel << 10);
    h->hijo[p] = g_enum.slot;
    g_enum = guardado;
}

static void hub_desconectar(struct xhci_hub *h, uint8_t p) {
    uint8_t s = h->hijo[p];
    h->hijo[p] = 0;
    consola_imprimir("  -> [HUB] Slot "); consola_imprimir_dec(h->slot);
    consola_imprimir(" puerto "); consola_imprimir_dec(p); consola_imprimir_linea(": dispositivo desconectado.");
    xhci_soltar_slot(s);
}

static void xhci_hub_iniciar(uint8_t slot_id, uint8_t velocidad, uint8_t config_val, uint8_t raiz) {
    struct xhci_hub *h = NULL;
    for (int i = 0; i < XHCI_MAX_HUBS; i++) if (!g_hubs[i].activo) { h = &g_hubs[i]; break; }
    if (!h) { consola_imprimir_linea_color("    [!] Sin espacio para mas hubs (4).", COLOR_AVISO_DEFAULT); return; }
    memset(h, 0, sizeof(*h));
    h->slot = slot_id; h->raiz = raiz; h->velocidad = velocidad; h->es_ss = velocidad >= 4;
    h->ruta = g_enum.ruta; h->profundidad = g_enum.profundidad;
    h->tt_slot = g_enum.tt_slot; h->tt_puerto = g_enum.tt_puerto;

    if (hub_ctrl(slot_id, 0x00, 0x09, config_val, 0, 0)) { consola_imprimir_linea_color("    [!] El hub rechazo SET_CONFIGURATION.", COLOR_AVISO_DEFAULT); return; }
    if (h->es_ss) hub_ctrl(slot_id, 0x20, 12, h->profundidad, 0, 0);   /* SET_HUB_DEPTH (USB 3): su nivel */
    if (hub_ctrl(slot_id, 0xA0, 0x06, h->es_ss ? 0x2A00 : 0x2900, 0, h->es_ss ? 12 : 9)) {
        consola_imprimir_linea_color("    [!] No se pudo leer el descriptor de hub.", COLOR_AVISO_DEFAULT);
        return;
    }
    uint8_t n = g_hub_buf[2];
    if (n > HUB_MAX_PUERTOS) n = HUB_MAX_PUERTOS;
    h->puertos = n;
    uint16_t caract = (uint16_t)(g_hub_buf[3] | g_hub_buf[4] << 8);
    unsigned encendido_ms = g_hub_buf[5] * 2u;
    uint32_t ttt = h->es_ss ? 0 : (caract >> 5) & 3;

    /* Slot de hub: bit Hub, numero de puertos y tiempo del traductor. Solo el contexto de slot (A0). */
    uint64_t in_fis = 0;
    uint8_t *in = (uint8_t *)dma_asignar_bufer_contiguo(33 * g_tamano_contexto, 64, &in_fis);
    if (in) {
        for (uint32_t i = 0; i < 33 * g_tamano_contexto; i++) in[i] = 0;
        uint32_t *ctrl = (uint32_t *)in;
        ctrl[1] = 1U;
        dma_sincronizar_dispositivo_a_cpu(g_slot_dev_ctx[slot_id], 32 * g_tamano_contexto);
        const uint32_t *dev = (const uint32_t *)g_slot_dev_ctx[slot_id];
        uint32_t *sc = (uint32_t *)(in + g_tamano_contexto);
        sc[0] = (dev[0] & 0x000FFFFF) | (1U << 26) | (1U << 27);   /* Ruta, Hub, una entrada (EP0); Speed es RsvdZ */
        sc[1] = (dev[1] & 0x00FFFFFF) | ((uint32_t)n << 24);
        sc[2] = (dev[2] & ~(3U << 16)) | (ttt << 16);
        sc[3] = 0;
        dma_sincronizar_cpu_a_dispositivo(in, 33 * g_tamano_contexto);
        struct trb_xhci evt;
        int r = xhci_enviar_comando(in_fis, 0, (TRB_TIPO_CONFIG_EP << 10) | ((uint32_t)slot_id << 24), &evt);
        dma_liberar_bufer_contiguo(in, in_fis, 33 * g_tamano_contexto);
        if (r) { consola_imprimir("    [!] El controlador no acepto el slot como hub (codigo "); consola_imprimir_dec((uint64_t)(uint32_t)r); consola_imprimir_linea(")"); return; }
    }

    for (uint8_t p = 1; p <= n; p++) hub_ctrl(slot_id, 0x23, 0x03, HUB_F_PORT_POWER, p, 0);
    /* Lo que el hub pide para dar energia, y despues el rebote de conexion (USB 2.0, 7.1.7.3: 100 ms) */
    esperar_milisegundos((encendido_ms > 100 ? encendido_ms : 100) + 100);
    h->activo = 1;

    consola_imprimir("    -> [HUB] Slot "); consola_imprimir_dec(slot_id);
    consola_imprimir(h->es_ss ? ": hub USB 3, " : velocidad == 3 ? ": hub USB 2 de alta velocidad, " : ": hub USB de velocidad completa, ");
    consola_imprimir_dec(n); consola_imprimir(" puertos, nivel "); consola_imprimir_dec(h->profundidad);
    consola_imprimir_linea(". Recorriendo puertos...");

    for (uint8_t p = 1; p <= n && h->activo; p++) {
        uint16_t st, ch;
        if (hub_estado(h, p, &st, &ch)) continue;
        if (ch & 1) hub_ctrl(slot_id, 0x23, 0x01, HUB_F_C_PORT_CONNECTION, p, 0);
        if (st & 1) hub_conectar(h, p);
    }
}

/* Cada 500 ms: cambios de conexion en los puertos de cada hub */
static void xhci_hubs_sondear(void) {
    static uint64_t ultimo;
    static int ocupado;
    if (ocupado) return;
    uint64_t ahora = tiempo_obtener_milisegundos();
    if (ahora - ultimo < 500) return;
    ultimo = ahora;
    ocupado = 1;
    for (int i = 0; i < XHCI_MAX_HUBS; i++) {
        struct xhci_hub *h = &g_hubs[i];
        for (uint8_t p = 1; h->activo && p <= h->puertos; p++) {
            uint16_t st, ch;
            if (hub_estado(h, p, &st, &ch) || !(ch & 1)) continue;
            hub_ctrl(h->slot, 0x23, 0x01, HUB_F_C_PORT_CONNECTION, p, 0);
            if (h->hijo[p]) hub_desconectar(h, p);
            if (st & 1) { esperar_milisegundos(100); hub_conectar(h, p); }
        }
    }
    ocupado = 0;
}

void xhci_hub_listar(void) { XHCI_EXCLUSIVO();
    int n = 0;
    for (int i = 0; i < XHCI_MAX_HUBS; i++) {
        struct xhci_hub *h = &g_hubs[i];
        if (!h->activo) continue;
        n++;
        consola_imprimir("  [HUB] slot="); consola_imprimir_dec(h->slot);
        consola_imprimir(" puerto raiz="); consola_imprimir_dec(h->raiz);
        consola_imprimir(" VID="); consola_imprimir_hex(g_slot_vid[h->slot]); consola_imprimir(" PID="); consola_imprimir_hex(g_slot_pid[h->slot]);
        consola_imprimir(h->es_ss ? " USB 3" : h->velocidad == 3 ? " USB 2 alta velocidad" : " velocidad completa");
        consola_imprimir(" puertos="); consola_imprimir_dec(h->puertos);
        consola_imprimir(" nivel="); consola_imprimir_dec(h->profundidad);
        consola_imprimir(" ruta="); consola_imprimir_hex(h->ruta);
        consola_imprimir_linea("");
        for (uint8_t p = 1; p <= h->puertos; p++) {
            uint16_t st = 0, ch = 0;
            int r = hub_estado(h, p, &st, &ch);
            consola_imprimir("        puerto "); consola_imprimir_dec(p);
            if (r) { consola_imprimir_linea(": sin respuesta"); continue; }
            consola_imprimir((st & 1) ? ": conectado" : ": libre");
            if (st & 1) consola_imprimir(h->es_ss ? " (Super)" : (st & 0x0200) ? " (Low)" : (st & 0x0400) ? " (High)" : " (Full)");
            uint8_t s = h->hijo[p];
            if (s) {
                consola_imprimir(" -> slot "); consola_imprimir_dec(s);
                consola_imprimir(" VID="); consola_imprimir_hex(g_slot_vid[s]); consola_imprimir(" PID="); consola_imprimir_hex(g_slot_pid[s]);
            } else if (st & 1) consola_imprimir(" (sin enumerar)");
            consola_imprimir_linea("");
        }
    }
    if (!n) consola_imprimir_linea("  [HUB] Ningun hub USB conectado.");
}

// Ejecuta el ciclo oficial de Reset USB 2.0 / 3.x según la especificación Intel xHCI 1.2
// y el controlador xhci-hub.c del kernel de Linux:
// 1. Limpia banderas de cambio previas (CSC, PEC, PRC).
// 2. Escribe PR = 1 con PLS_MASK en 0 (evitando que quede atrapado en Polling PLS=7).
// 3. Sostiene la señalización SE0 durante 50 ms continuos (HUB_ROOT_RESET_TIME).
// 4. Espera la confirmación de hardware (PR=0 o PRC=1) y aplica recuperación TRSTRCY = 20 ms.
// 5. Limpia PRC y banderas de cambio residuales.
// 6. Si no se habilitó en puertos de alta velocidad, intenta Warm Port Reset (WPR).
static int xhci_resetear_puerto(uint8_t p, uint32_t *portsc_out) {
    if (!g_op_base || p < 1 || p > g_estado.max_puertos) return -1;
    uint64_t reg_port = g_op_base + REG_OP_PORTSC_BASE + ((p - 1) * 0x10);
    uint32_t val = mmio_leer32(reg_port);

    // 0. Si el puerto no tiene energía, activarla y esperar estabilización
    if (!(val & PORTSC_PP)) {
        mmio_escribir32(reg_port, xhci_portsc_neutral(val) | PORTSC_PP);
        esperar_milisegundos(20);
        val = mmio_leer32(reg_port);
    }

    // 1. Limpiar cualquier bandera de cambio residual previa
    mmio_escribir32(reg_port, xhci_portsc_neutral(val) | PORTSC_CSC | PORTSC_PEC | PORTSC_PRC);
    esperar_milisegundos(2);

    // 2. Activar Port Reset (PR = 1).
    // Fundamental (Linux xhci-hub.c): borrar PLS_MASK para que el puerto transicione
    // desde Polling (PLS=7) o RxDetect a Resetting -> U0 (Enabled), impidiendo que el silicio
    // conserve el estado de Polling anterior.
    val = mmio_leer32(reg_port);
    uint32_t cmd_reset = (xhci_portsc_neutral(val) & ~PORTSC_PLS_MASK) | PORTSC_PP | PORTSC_PR;
    mmio_escribir32(reg_port, cmd_reset);

    // 3. MANTENER SEÑALIZACIÓN SE0 POR 50 MS (HUB_ROOT_RESET_TIME de Linux / USB 2.0 estándar)
    esperar_milisegundos(50);

    // 4. Esperar finalización del ciclo de reset por silicio (PR vuelve a 0 o PRC se activa)
    int rst_timeout = 100;
    while (rst_timeout > 0) {
        val = mmio_leer32(reg_port);
        if ((val & PORTSC_PRC) || !(val & PORTSC_PR)) {
            break;
        }
        esperar_milisegundos(2);
        rst_timeout -= 2;
    }

    // 5. Tiempo de recuperación del bus post-reset según USB 2.0 (T_RSTRCY = 20 ms)
    esperar_milisegundos(20);

    // 6. Limpiar la bandera de cambio de reset (PRC) y cambios de enlace residuales
    val = mmio_leer32(reg_port);
    mmio_escribir32(reg_port, xhci_portsc_neutral(val) | PORTSC_PRC | PORTSC_CSC | PORTSC_PEC);
    esperar_milisegundos(5);

    uint32_t final_portsc = mmio_leer32(reg_port);

    // 7. Si no se habilitó y es un puerto USB 3.x / SuperSpeed conectado, intentar Warm Port Reset (WPR, bit 31)
    if (!(final_portsc & PORTSC_PED) && (final_portsc & PORTSC_CCS)) {
        mmio_escribir32(reg_port, (xhci_portsc_neutral(final_portsc) & ~PORTSC_PLS_MASK) | PORTSC_PP | (1U << 31));
        esperar_milisegundos(50);
        val = mmio_leer32(reg_port);
        mmio_escribir32(reg_port, xhci_portsc_neutral(val) | (1U << 19) /* WRC */ | PORTSC_PRC | PORTSC_CSC | PORTSC_PEC);
        esperar_milisegundos(20);
        final_portsc = mmio_leer32(reg_port);
    }

    if (portsc_out) *portsc_out = final_portsc;
    return (final_portsc & PORTSC_PED) ? 0 : -1;
}

// --- INICIALIZACIÓN GENERAL DEL CONTROLADOR ---
int xhci_iniciar(void) { XHCI_EXCLUSIVO();
    if (g_estado.inicializado) return 0;
    g_estado.ultimo_codigo_control = 0xFF;
    g_estado.ultimo_codigo_transfer = 0xFF;

    serial_imprimir_linea("[xHCI] Escaneando bus PCI en busca de controladores USB 3.x xHCI...");

    // 1. Buscar controlador xHCI en PCI (Clase 0x0C, Subclase 0x03, Prog-IF 0x30)
    // Se prioriza el chipset Intel PCH (ej. 8086:7A60 de Raptor Lake HX) si coexiste con Thunderbolt/USB4
    int total_devs = pci_obtener_conteo();
    const struct dispositivo_pci *pci_xhci = NULL;

    for (int i = 0; i < total_devs; i++) {
        const struct dispositivo_pci *d = pci_obtener_dispositivo(i);
        if (d && d->clase == 0x0C && d->subclase == 0x03 && d->prog_if == 0x30) {
            if (!pci_xhci) {
                pci_xhci = d;
            }
            if (d->id_proveedor == 0x8086 && d->id_dispositivo == 0x7A60) {
                pci_xhci = d;
                break;
            }
        }
    }

    if (!pci_xhci) {
        serial_imprimir_linea("[xHCI AVISO] No se detectó controlador xHCI en el bus PCI.");
        return -1;
    }

    g_estado.controlador_detectado = 1;
    g_estado.bus            = pci_xhci->bus;
    g_estado.ranura         = pci_xhci->ranura;
    g_estado.funcion        = pci_xhci->funcion;
    g_estado.id_proveedor   = pci_xhci->id_proveedor;
    g_estado.id_dispositivo = pci_xhci->id_dispositivo;
    g_estado.dir_fisica_mmio= pci_xhci->barras[0].dir_base;
    g_estado.tamano_mmio    = (uint32_t)pci_xhci->barras[0].tamano;

    serial_imprimir("[xHCI] Encontrado controlador en ");
    serial_imprimir_hex(g_estado.bus);
    serial_imprimir(":");
    serial_imprimir_hex(g_estado.ranura);
    serial_imprimir(".");
    serial_imprimir_hex(g_estado.funcion);
    serial_imprimir(" [Vendor: ");
    serial_imprimir_hex(g_estado.id_proveedor);
    serial_imprimir(" Dev: ");
    serial_imprimir_hex(g_estado.id_dispositivo);
    serial_imprimir(" | BAR0: 0x");
    serial_imprimir_hex(g_estado.dir_fisica_mmio);
    serial_imprimir_linea("]");

    // 2. Activar Bus Master y Memory Space en PCI Command
    pci_activar_bus_master(pci_xhci);

    // 3. Mapear BAR0 en espacio virtual del kernel
    g_estado.dir_virtual_mmio = XHCI_MMIO_VIRTUAL_BASE;
    uint32_t paginas = (g_estado.tamano_mmio + TAMANO_PAGINA - 1) / TAMANO_PAGINA;
    if (paginas == 0) paginas = 16; // Mínimo 64 KiB para registros xHCI

    for (uint32_t p = 0; p < paginas; p++) {
        paginacion_mapear(g_estado.dir_virtual_mmio + (p * TAMANO_PAGINA),
                          g_estado.dir_fisica_mmio + (p * TAMANO_PAGINA),
                          PAGINA_ATRIBUTOS_MMIO);
    }

    g_mmio_base = g_estado.dir_virtual_mmio;
    g_cap_length = *(volatile uint8_t *)(g_mmio_base + REG_CAPLENGTH);
    g_op_base    = g_mmio_base + g_cap_length;

    uint32_t dboff = mmio_leer32(g_mmio_base + REG_DBOFF) & ~0x03U;
    g_db_base = g_mmio_base + dboff;

    uint32_t rtsoff = mmio_leer32(g_mmio_base + REG_RTSOFF) & ~0x1FU;
    g_rts_base = g_mmio_base + rtsoff;

    uint32_t hcsparams1 = mmio_leer32(g_mmio_base + REG_HCSPARAMS1);
    g_estado.max_slots   = hcsparams1 & 0xFF;
    g_estado.max_puertos = (hcsparams1 >> 24) & 0xFF;
    if (g_estado.max_puertos > XHCI_MAX_PUERTOS) {
        serial_imprimir_linea("[xHCI] Controlador supera la tabla de puertos; limitando sondeo.");
        g_estado.max_puertos = XHCI_MAX_PUERTOS;
    }

    uint32_t hcsparams2 = mmio_leer32(g_mmio_base + REG_HCSPARAMS2);
    uint32_t max_scratch_hi = (hcsparams2 >> 21) & 0x1F;
    uint32_t max_scratch_lo = (hcsparams2 >> 27) & 0x1F;
    g_max_scratchpad_buffers = (max_scratch_hi << 5) | max_scratch_lo;
    g_estado.max_scratchpad_buffers = g_max_scratchpad_buffers;

    uint32_t hccparams1 = mmio_leer32(g_mmio_base + REG_HCCPARAMS1);
    g_tamano_contexto = (hccparams1 & (1U << 2)) ? 64 : 32;

    serial_imprimir("[xHCI] Slots Máx: ");
    serial_imprimir_dec(g_estado.max_slots);
    serial_imprimir(" | Puertos Máx: ");
    serial_imprimir_dec(g_estado.max_puertos);
    serial_imprimir(" | Scratchpad Buffers: ");
    serial_imprimir_dec(g_max_scratchpad_buffers);
    serial_imprimir(" | Context Size: ");
    serial_imprimir_dec(g_tamano_contexto);
    serial_imprimir_linea(" bytes");

    // 4. Negociar cesión de control BIOS -> OS (USBLEGSUP)
    uint32_t xecp = (hccparams1 >> 16) & 0xFFFF;
    xhci_negociar_cesion_bios(g_mmio_base, xecp);

    // 5. Detener y reiniciar el controlador xHCI
    uint32_t usbcmd = mmio_leer32(g_op_base + REG_OP_USBCMD);
    if (usbcmd & USBCMD_RS) {
        mmio_escribir32(g_op_base + REG_OP_USBCMD, usbcmd & ~USBCMD_RS);
        int timeout = 100;
        while (!(mmio_leer32(g_op_base + REG_OP_USBSTS) & USBSTS_HCH) && timeout > 0) {
            esperar_milisegundos(1);
            timeout--;
        }
    }

    // Reset del controlador
    mmio_escribir32(g_op_base + REG_OP_USBCMD, USBCMD_HCRST);
    int timeout = 500;
    while ((mmio_leer32(g_op_base + REG_OP_USBCMD) & USBCMD_HCRST) && timeout > 0) {
        esperar_milisegundos(1);
        timeout--;
    }

    timeout = 500;
    while ((mmio_leer32(g_op_base + REG_OP_USBSTS) & USBSTS_CNR) && timeout > 0) {
        esperar_milisegundos(1);
        timeout--;
    }

    if (timeout == 0) {
        serial_imprimir_linea("[xHCI ERROR] Timeout esperando reset del controlador");
        return -2;
    }
    serial_imprimir_linea("[xHCI] Reset de controlador completado con éxito.");

    // 6. Asignar estructuras DMA principales
    // DCBAA (Device Context Base Address Array)
    uint32_t tam_dcbaa = (g_estado.max_slots + 1) * sizeof(uint64_t);
    g_dcbaa = (uint64_t *)dma_asignar_bufer_contiguo(tam_dcbaa, 64, &g_dcbaa_fisica);
    if (!g_dcbaa) return -3;
    for (uint32_t i = 0; i <= g_estado.max_slots; i++) g_dcbaa[i] = 0;

    // Configuración de Scratchpad Buffers (xHCI 1.2 §4.20 y §6.1, inspirado en Stellux xHCI):
    // Si HCSPARAMS2 indica max_scratchpad_buffers > 0, DCBAA[0] DEBE contener la dirección
    // física de la matriz de punteros a páginas de scratchpad de tamaño PAGESIZE (4096 bytes).
    // Si max_scratchpad_buffers == 0, DCBAA[0] se fija en 0.
    if (g_max_scratchpad_buffers > 0) {
        uint32_t tam_array = g_max_scratchpad_buffers * sizeof(uint64_t);
        g_scratchpad_array = (uint64_t *)dma_asignar_bufer_contiguo(tam_array, 64, &g_scratchpad_array_fisica);
        if (!g_scratchpad_array) {
            serial_imprimir_linea("[xHCI ERROR] Fallo al asignar Scratchpad Array en DMA");
            return -8;
        }

        uint64_t tam_paginas = (uint64_t)g_max_scratchpad_buffers * TAMANO_PAGINA;
        g_scratchpad_pages = dma_asignar_bufer_contiguo(tam_paginas, TAMANO_PAGINA, &g_scratchpad_pages_fisica);
        if (!g_scratchpad_pages) {
            serial_imprimir_linea("[xHCI ERROR] Fallo al asignar páginas de Scratchpad Buffers en DMA");
            return -9;
        }

        for (uint32_t s = 0; s < g_max_scratchpad_buffers; s++) {
            g_scratchpad_array[s] = g_scratchpad_pages_fisica + ((uint64_t)s * TAMANO_PAGINA);
        }
        dma_sincronizar_cpu_a_dispositivo(g_scratchpad_array, tam_array);
        dma_sincronizar_cpu_a_dispositivo(g_scratchpad_pages, tam_paginas);

        g_dcbaa[0] = g_scratchpad_array_fisica;

        serial_imprimir("  [xHCI] Scratchpad Buffers configurados: ");
        serial_imprimir_dec(g_max_scratchpad_buffers);
        serial_imprimir(" páginas (Array: ");
        serial_imprimir_hex(g_scratchpad_array_fisica);
        serial_imprimir(" | DCBAA[0]=");
        serial_imprimir_hex(g_dcbaa[0]);
        serial_imprimir_linea(")");
    } else {
        g_dcbaa[0] = 0;
        serial_imprimir_linea("  [xHCI] Scratchpad Buffers: 0 requeridos (DCBAA[0]=0)");
    }

    dma_sincronizar_cpu_a_dispositivo(g_dcbaa, tam_dcbaa);
    mmio_escribir64(g_op_base + REG_OP_DCBAAP, g_dcbaa_fisica);

    // Configurar número de slots activos en el controlador
    uint8_t slots_activar = (g_estado.max_slots > XHCI_MAX_SLOTS) ? XHCI_MAX_SLOTS : g_estado.max_slots;
    mmio_escribir32(g_op_base + REG_OP_CONFIG, slots_activar);

    // Command Ring
    g_cmd_ring = (volatile struct trb_xhci *)dma_asignar_bufer_contiguo(XHCI_TAM_ANILLO * sizeof(struct trb_xhci), 64, &g_cmd_ring_fisica);
    if (!g_cmd_ring) return -4;
    for (int i = 0; i < XHCI_TAM_ANILLO; i++) {
        g_cmd_ring[i].parametro = 0;
        g_cmd_ring[i].estado = 0;
        g_cmd_ring[i].control = 0;
    }
    // Link TRB final preinicializado apuntando al inicio del anillo
    volatile struct trb_xhci *cmd_link = &g_cmd_ring[XHCI_TAM_ANILLO - 1];
    cmd_link->parametro = g_cmd_ring_fisica;
    cmd_link->estado = 0;
    cmd_link->control = (TRB_TIPO_LINK << 10) | (1U << 1) /* Toggle Cycle */;

    g_cmd_idx = 0;
    g_cmd_cycle = 1;
    dma_sincronizar_cpu_a_dispositivo((const void *)g_cmd_ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
    mmio_escribir64(g_op_base + REG_OP_CRCR, g_cmd_ring_fisica | 1); // RCS = 1

    uint64_t crcr_post = mmio_leer64(g_op_base + REG_OP_CRCR);
    serial_imprimir("[xHCI] CRCR Configurado: Fisica=");
    serial_imprimir_hex(g_cmd_ring_fisica);
    serial_imprimir(" | Leido=");
    serial_imprimir_hex(crcr_post);
    serial_imprimir_linea("");

    // Event Ring y ERST
    g_event_ring = (volatile struct trb_xhci *)dma_asignar_bufer_contiguo(XHCI_TAM_ANILLO * sizeof(struct trb_xhci), 64, &g_event_ring_fisica);
    g_erst = (struct erst_entrada_xhci *)dma_asignar_bufer_contiguo(sizeof(struct erst_entrada_xhci), 64, &g_erst_fisica);
    if (!g_event_ring || !g_erst) return -5;

    for (int i = 0; i < XHCI_TAM_ANILLO; i++) {
        g_event_ring[i].parametro = 0;
        g_event_ring[i].estado = 0;
        g_event_ring[i].control = 0;
    }
    g_event_idx = 0;
    g_event_cycle = 1;

    g_erst[0].dir_anillo_fisica = g_event_ring_fisica;
    g_erst[0].tamano_anillo     = XHCI_TAM_ANILLO;
    g_erst[0].reservado         = 0;
    dma_sincronizar_cpu_a_dispositivo((const void *)g_event_ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
    dma_sincronizar_cpu_a_dispositivo(g_erst, sizeof(struct erst_entrada_xhci));

    // Configurar Interrupter 0 (xHCI 1.2 §4.17.1: ERSTSZ y ERDP antes de ERSTBA)
    uint64_t intr0 = g_rts_base + 0x20;
    mmio_escribir32(intr0 + 0x08, 1);                             // 1. ERSTSZ = 1
    mmio_escribir64(intr0 + 0x18, g_event_ring_fisica | (1U << 3));// 2. ERDP + EHB=1
    mmio_escribir64(intr0 + 0x10, g_erst_fisica);                 // 3. ERSTBA
    mmio_escribir32(intr0 + 0x00, 0x02);                          // 4. IMAN: Interrupt Enable

    serial_imprimir("[xHCI] Interrupter 0: ERSTBA=");
    serial_imprimir_hex(mmio_leer64(intr0 + 0x10));
    serial_imprimir(" | ERDP=");
    serial_imprimir_hex(mmio_leer64(intr0 + 0x18));
    serial_imprimir_linea("");

    // Asignar anillos y búferes auxiliares para EP0 y los endpoints de teclado
    // Asignar anillos de transferencia EP0 dedicados para cada ranura (Slot 1..XHCI_MAX_SLOTS)
    for (int s = 1; s <= XHCI_MAX_SLOTS; s++) {
        g_slot_ep0_ring[s] = (volatile struct trb_xhci *)dma_asignar_bufer_contiguo(XHCI_TAM_ANILLO * sizeof(struct trb_xhci), 64, &g_slot_ep0_ring_fisica[s]);
        if (!g_slot_ep0_ring[s]) return -6;
        for (int i = 0; i < XHCI_TAM_ANILLO; i++) {
            g_slot_ep0_ring[s][i].parametro = 0;
            g_slot_ep0_ring[s][i].estado = 0;
            g_slot_ep0_ring[s][i].control = 0;
        }
        g_slot_ep0_idx[s] = 0;
        g_slot_ep0_cycle[s] = 1;
        dma_sincronizar_cpu_a_dispositivo((const void *)g_slot_ep0_ring[s], XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
    }

    for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
        g_teclado_eps[e].ring = (volatile struct trb_xhci *)dma_asignar_bufer_contiguo(XHCI_TAM_ANILLO * sizeof(struct trb_xhci), 64, &g_teclado_eps[e].ring_fisica);
        g_teclado_eps[e].bufer = (uint8_t *)dma_asignar_bufer_contiguo(XHCI_TAM_BUFFER_REPORTE, 64, &g_teclado_eps[e].bufer_fisica);
        if (!g_teclado_eps[e].ring || !g_teclado_eps[e].bufer) return -7;
        for (int i = 0; i < XHCI_TAM_ANILLO; i++) {
            g_teclado_eps[e].ring[i].parametro = 0;
            g_teclado_eps[e].ring[i].estado = 0;
            g_teclado_eps[e].ring[i].control = 0;
        }
        dma_sincronizar_cpu_a_dispositivo((const void *)g_teclado_eps[e].ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
        g_teclado_eps[e].activo = 0;
        g_teclado_eps[e].slot_id = 0;
        g_teclado_eps[e].puerto_idx = 0;
    }

    for (int b = 0; b < XHCI_MAX_BULK_EPS; b++) {
        g_bulk_eps[b].ring = (volatile struct trb_xhci *)dma_asignar_bufer_contiguo(XHCI_TAM_ANILLO * sizeof(struct trb_xhci), 64, &g_bulk_eps[b].ring_fisica);
        if (!g_bulk_eps[b].ring) return -8;
        for (int i = 0; i < XHCI_TAM_ANILLO; i++) {
            g_bulk_eps[b].ring[i].parametro = 0;
            g_bulk_eps[b].ring[i].estado = 0;
            g_bulk_eps[b].ring[i].control = 0;
        }
        g_bulk_eps[b].idx = 0;
        g_bulk_eps[b].cycle = 1;
        dma_sincronizar_cpu_a_dispositivo((const void *)g_bulk_eps[b].ring, XHCI_TAM_ANILLO * sizeof(struct trb_xhci));
        g_bulk_eps[b].activo = 0;
        g_bulk_eps[b].slot_id = 0;
        g_bulk_eps[b].ep_dci = 0;
    }

    usb_msc_iniciar();

    // 7. Arrancar el controlador xHCI
    mmio_escribir32(g_op_base + REG_OP_USBCMD, USBCMD_RS | USBCMD_INTE);
    timeout = 100;
    while ((mmio_leer32(g_op_base + REG_OP_USBSTS) & USBSTS_HCH) && timeout > 0) {
        esperar_milisegundos(1);
        timeout--;
    }

    g_estado.inicializado = 1;
    serial_imprimir_linea("[xHCI] Controlador en ejecución (Run/Stop activo). Escaneando puertos raíz...");

    // 8. Encender energía (Port Power) en TODOS los puertos raíz concurrentemente
    for (uint8_t p = 1; p <= g_estado.max_puertos; p++) {
        uint64_t reg_port = g_op_base + REG_OP_PORTSC_BASE + ((p - 1) * 0x10);
        uint32_t portsc = mmio_leer32(reg_port);
        if (!(portsc & PORTSC_PP)) {
            mmio_escribir32(reg_port, xhci_portsc_neutral(portsc) | PORTSC_PP);
        }
    }

    // Esperar 150 ms para estabilización de consumo, encendido de microcontroladores y lógica RGB (T_PCONF USB)
    esperar_milisegundos(150);

    // 9. Escanear puertos con dispositivo conectado físicamente
    for (uint8_t p = 1; p <= g_estado.max_puertos; p++) {
        uint64_t reg_port = g_op_base + REG_OP_PORTSC_BASE + ((p - 1) * 0x10);
        uint32_t portsc = mmio_leer32(reg_port);

        // Guardar estado inicial para seguimiento de hotplug
        g_puerto_estado_ccs[p] = (portsc & PORTSC_CCS) ? 1 : 0;

        // Si hay un dispositivo conectado (Current Connect Status)
        if (portsc & PORTSC_CCS) {
            g_estado.puertos_conectados++;

            uint8_t vel = (portsc >> 10) & 0x0F;
            const char *nom_vel = "Desconocida";
            if (vel == 1) nom_vel = "Full-Speed (Teclado/Mouse/Dock)";
            else if (vel == 2) nom_vel = "Low-Speed 1.5 Mbps";
            else if (vel == 3) nom_vel = "High-Speed 480 Mbps";
            else if (vel >= 4) nom_vel = "SuperSpeed 5+ Gbps";

            consola_imprimir_color("  [USB INICIAL] Dispositivo en Puerto ", COLOR_AVISO_DEFAULT);
            consola_imprimir_dec(p);
            consola_imprimir(" (");
            consola_imprimir(nom_vel);
            consola_imprimir(", PORTSC: ");
            consola_imprimir_hex(portsc);
            consola_imprimir_linea(")");

            serial_imprimir("  [xHCI] Puerto raíz ");
            serial_imprimir_dec(p);
            serial_imprimir(": Conectado (PORTSC: ");
            serial_imprimir_hex(portsc);
            serial_imprimir_linea(").");

            // Ejecutar ciclo de Reset USB oficial incondicionalmente en cada puerto conectado
            // para devolver el microcontrolador del dispositivo a Default Address 0 (incluso si UEFI lo dejó con PED=1).
            consola_imprimir("    -> Reseteando enlace físico de Puerto ");
            consola_imprimir_dec(p);
            consola_imprimir_linea("...");
            serial_imprimir("  [xHCI] Iniciando reset USB oficial en puerto ");
            serial_imprimir_dec(p);
            serial_imprimir_linea("...");
            xhci_resetear_puerto(p, &portsc);

            serial_imprimir("  [xHCI] Puerto ");
            serial_imprimir_dec(p);
            serial_imprimir(" post-reset: PORTSC=");
            serial_imprimir_hex(portsc);
            serial_imprimir(" | Habilitado=");
            serial_imprimir_linea((portsc & PORTSC_PED) ? "[SÍ]" : "[NO]");

            if (portsc & PORTSC_PED) {
                // USB 2.0 Reset Recovery Time (TRSTRCY)
                esperar_milisegundos(50);
                // Puerto habilitado tras reset, configurar dispositivo
                xhci_configurar_puerto(p, portsc);
                if (g_estado.teclado_detectado && g_estado.teclado_puerto == p) {
                    consola_imprimir("    -> Puerto ");
                    consola_imprimir_dec(p);
                    consola_imprimir_linea_color(": ¡Teclado USB Configurado [OK]!", COLOR_EXITO_DEFAULT);
                }
            }
        }
    }

    serial_imprimir("[xHCI] Resumen de Inicialización: ");
    serial_imprimir_dec(g_estado.puertos_conectados);
    serial_imprimir(" puerto(s) conectado(s). Teclado USB: ");
    if (g_estado.teclado_detectado) {
        serial_imprimir_linea("[DETECTADO Y OPERATIVO]");
    } else {
        serial_imprimir_linea("[NO DETECTADO]");
    }
    serial_imprimir("[xHCI TELEMETRÍA] fase=");
    serial_imprimir_dec(g_estado.etapa_enumeracion);
    serial_imprimir(" control_req=");
    serial_imprimir_hex(g_estado.ultima_peticion_control);
    serial_imprimir(" valor=");
    serial_imprimir_hex(g_estado.ultimo_valor_control);
    serial_imprimir(" indice=");
    serial_imprimir_hex(g_estado.ultimo_indice_control);
    serial_imprimir(" largo=");
    serial_imprimir_dec(g_estado.ultimo_largo_control);
    serial_imprimir(" cc=");
    serial_imprimir_dec(g_estado.ultimo_codigo_control);
    serial_imprimir(" transferencias_control=");
    serial_imprimir_dec(g_estado.transferencias_control);
    serial_imprimir(" fallos_control=");
    serial_imprimir_dec(g_estado.fallos_control);
    serial_imprimir_linea("");

    return 0;
}

// Obtiene el estado físico de un puerto raíz específico (1..max_puertos)
int xhci_obtener_info_puerto(uint8_t puerto, uint32_t *portsc_out, int *conectado_out, int *habilitado_out, uint8_t *velocidad_out) { XHCI_EXCLUSIVO();
    if (!g_estado.inicializado || puerto < 1 || puerto > g_estado.max_puertos) return -1;
    uint64_t reg_port = g_op_base + REG_OP_PORTSC_BASE + ((puerto - 1) * 0x10);
    uint32_t portsc = mmio_leer32(reg_port);
    if (portsc_out) *portsc_out = portsc;
    if (conectado_out) *conectado_out = (portsc & PORTSC_CCS) ? 1 : 0;
    if (habilitado_out) *habilitado_out = (portsc & PORTSC_PED) ? 1 : 0;
    if (velocidad_out) *velocidad_out = (uint8_t)((portsc >> 10) & 0x0F);
    return 0;
}

// Escanea cambios de conexión/desconexión en puertos raíz y emite notificaciones
int xhci_escanear_cambios_puertos(int verbose) { XHCI_EXCLUSIVO();
    if (!g_estado.inicializado) return 0;

    int cambios = 0;

    for (uint8_t p = 1; p <= g_estado.max_puertos; p++) {
        uint64_t reg_port = g_op_base + REG_OP_PORTSC_BASE + ((p - 1) * 0x10);
        uint32_t portsc = mmio_leer32(reg_port);

        // Si el puerto no tenía energía (Port Power), activarla
        if (!(portsc & PORTSC_PP)) {
            mmio_escribir32(reg_port, xhci_portsc_neutral(portsc) | PORTSC_PP);
            continue;
        }

        uint8_t ccs = (portsc & PORTSC_CCS) ? 1 : 0;
        uint8_t ccs_anterior = g_puerto_estado_ccs[p];

        // 1. Detección de Conexión: el puerto pasó de desconectado (0) a conectado (1)
        if (ccs == 1 && ccs_anterior == 0) {
            cambios++;
            g_puerto_estado_ccs[p] = 1;
            g_estado.puertos_conectados++;
            xhci_suceso(p, portsc, XHCI_SUCESO_CONEXION, 0);
            if (verbose) xhci_suceso_imprimir(p, portsc, "CONEXION");

            uint8_t vel = (portsc >> 10) & 0x0F;
            const char *nom_vel = "Desconocida";
            if (vel == 1) nom_vel = "Full-Speed 12 Mbps (Teclado/Mouse/Dock)";
            else if (vel == 2) nom_vel = "Low-Speed 1.5 Mbps";
            else if (vel == 3) nom_vel = "High-Speed 480 Mbps";
            else if (vel >= 4) nom_vel = "SuperSpeed 5+ Gbps";

            serial_imprimir("\n[xHCI HOTPLUG] Conexión detectada en Puerto ");
            serial_imprimir_dec(p);
            serial_imprimir(" (Velocidad: ");
            serial_imprimir(nom_vel);
            serial_imprimir(", PORTSC: ");
            serial_imprimir_hex(portsc);
            serial_imprimir_linea(")");

            if (verbose) {
                consola_imprimir_linea("");
                consola_imprimir_linea_color("==================================================================", COLOR_PROMPT_DEFAULT);
                consola_imprimir_color(" [!] SE HA CONECTADO UN DISPOSITIVO EN: PUERTO ", COLOR_EXITO_DEFAULT);
                consola_imprimir_dec(p);
                consola_imprimir_linea_color("", COLOR_EXITO_DEFAULT);
                consola_imprimir("     Velocidad detectada : ");
                consola_imprimir_linea_color(nom_vel, COLOR_EXITO_DEFAULT);
                consola_imprimir("     Estado eléctrico    : PORTSC = ");
                consola_imprimir_hex(portsc);
                consola_imprimir_linea("");
                consola_imprimir_linea_color("==================================================================", COLOR_PROMPT_DEFAULT);
            }

            // 1. Debounce mecánico y estabilización de consumo (USB 2.0 §7.1.7.3 TATTDB >= 100 ms)
            esperar_milisegundos(100);
            portsc = mmio_leer32(reg_port);
            if (!(portsc & PORTSC_CCS)) continue; // Desconexión transitoria o falso contacto

            // Realizar reset oficial de puerto USB
            xhci_resetear_puerto(p, &portsc);

            if (portsc & PORTSC_PED) {
                // USB 2.0 Reset Recovery Time (TRSTRCY)
                esperar_milisegundos(50);
                if (verbose) {
                    consola_imprimir("  -> Puerto ");
                    consola_imprimir_dec(p);
                    consola_imprimir_linea_color(": Enlace reseteado y habilitado [OK]. Inicializando...", COLOR_EXITO_DEFAULT);
                }
                xhci_configurar_puerto(p, portsc);
                if (g_estado.teclado_detectado && g_estado.teclado_puerto == p) {
                    if (verbose) {
                        consola_imprimir("  -> Puerto ");
                        consola_imprimir_dec(p);
                        consola_imprimir_linea_color(": ¡Teclado USB listo para escribir en la terminal!", COLOR_EXITO_DEFAULT);
                    }
                } else if (!g_estado.teclado_detectado) {
                    if (verbose) {
                        consola_imprimir("  -> Puerto ");
                        consola_imprimir_dec(p);
                        consola_imprimir_linea_color(": Configuración finalizada (sin teclado detectado o fallo en paso previo).", COLOR_AVISO_DEFAULT);
                    }
                }
            } else {
                if (verbose) {
                    consola_imprimir("  -> Puerto ");
                    consola_imprimir_dec(p);
                    consola_imprimir_linea_color(": Aviso - No se pudo habilitar el enlace tras el reset.", COLOR_AVISO_DEFAULT);
                }
                /* H204: antes el puerto quedaba muerto hasta volver a enchufar. Ahora se reintenta en pasadas
                 * siguientes del sondeo (+100, +300 y +700 ms), sin esperar aqui. */
                xhci_suceso(p, portsc, XHCI_SUCESO_SIN_ENLACE, 0);
                if (verbose) xhci_suceso_imprimir(p, portsc, "SIN ENLACE tras el reset; reintento 1 en 100 ms");
                xhci_reintento_programar(p, 1);
            }
        }
        // 2. Detección de Desconexión: el puerto pasó de conectado (1) a desconectado (0)
        else if (ccs == 0 && ccs_anterior == 1) {
            cambios++;
            g_puerto_estado_ccs[p] = 0;
            if (g_estado.puertos_conectados > 0) g_estado.puertos_conectados--;
            xhci_suceso(p, portsc, XHCI_SUCESO_DESCONEXION, 0);
            if (verbose) xhci_suceso_imprimir(p, portsc, "DESCONEXION");
            g_reintento[p].intento = 0;   /* H204: se fue el dispositivo; no hay enlace que reintentar */

            serial_imprimir("\n[xHCI HOTPLUG] Desconexión detectada en Puerto ");
            serial_imprimir_dec(p);
            serial_imprimir_linea("");

            if (verbose) {
                consola_imprimir_linea("");
                consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
                consola_imprimir_color(" [!] SE HA DESCONECTADO EL DISPOSITIVO DEL: PUERTO ", COLOR_AVISO_DEFAULT);
                consola_imprimir_dec(p);
                consola_imprimir_linea_color("", COLOR_AVISO_DEFAULT);
                if (g_estado.teclado_puerto == p) {
                    consola_imprimir_linea_color("     (El teclado USB ha sido desconectado del sistema)", COLOR_ERROR_DEFAULT);
                }
                consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
            }

            uint8_t slot_id = (p <= XHCI_MAX_PUERTOS) ? g_puerto_slot[p] : 0;
            if (slot_id > 0 && slot_id <= XHCI_MAX_SLOTS) {
                xhci_soltar_slot(slot_id);   /* Si era un hub, tambien lo que colgaba de el (H139) */
                g_puerto_slot[p] = 0;
            }

            // Limpiar banderas de cambio residuales en el registro del puerto (CSC, PEC, PRC)
            mmio_escribir32(reg_port, xhci_portsc_neutral(portsc) | PORTSC_CSC | PORTSC_PEC | PORTSC_PRC);
        }
    }

    return cambios;
}

/* H204, Fase 0: reintentos del reset de un puerto que quedo conectado y sin enlace. Cada uno corre en una pasada del
 * sondeo cuando llega su hora; entre uno y otro, CPU 0 sigue con lo suyo. El reset en si sigue tardando lo que
 * tarda (unos 100 ms de espera activa dentro de xhci_resetear_puerto), igual que el primero. */
static void xhci_reintentos_atender(void) {
    uint64_t ahora = tiempo_obtener_milisegundos();
    for (uint8_t p = 1; p <= g_estado.max_puertos && p <= XHCI_MAX_PUERTOS; p++) {
        uint8_t intento = g_reintento[p].intento;
        if (!intento || ahora < g_reintento[p].cuando_ms) continue;
        g_reintento[p].intento = 0;
        uint64_t reg_port = g_op_base + REG_OP_PORTSC_BASE + ((uint64_t)(p - 1) * 0x10);
        uint32_t portsc = mmio_leer32(reg_port);
        if (!(portsc & PORTSC_CCS)) {
            xhci_suceso_imprimir(p, portsc, "ya sin dispositivo; se cancela el reintento");
            continue;   /* El escaneo vera la desconexion */
        }
        if (g_puerto_slot[p]) continue;   /* Algo lo configuro entretanto */
        g_fase0.reintentos++;
        if (!(portsc & PORTSC_PED)) xhci_resetear_puerto(p, &portsc);
        if (portsc & PORTSC_PED) {
            g_fase0.recuperados++;
            xhci_suceso(p, portsc, XHCI_SUCESO_RECUPERADO, intento);
            consola_imprimir("[USB-F0] Puerto "); consola_imprimir_dec(p);
            consola_imprimir(": enlace recuperado en el reintento "); consola_imprimir_dec(intento);
            consola_imprimir_linea(". Inicializando...");
            esperar_milisegundos(50);   /* TRSTRCY, igual que tras el primer reset */
            xhci_configurar_puerto(p, portsc);
        } else if (intento < XHCI_REINTENTOS) {
            xhci_suceso(p, portsc, XHCI_SUCESO_REINTENTO_FALLO, intento);
            xhci_suceso_imprimir(p, portsc, intento == 1 ? "reintento 1 sin enlace; el 2 en 300 ms" : "reintento 2 sin enlace; el 3 en 700 ms");
            xhci_reintento_programar(p, (uint8_t)(intento + 1));
        } else {
            g_fase0.perdidos++;
            xhci_suceso(p, portsc, XHCI_SUCESO_PERDIDO, intento);
            xhci_suceso_imprimir(p, portsc, "SIN ENLACE tras 3 reintentos: desenchufar y volver a enchufar");
        }
    }
}

// Re-escanea puertos en caliente (hotplug / encendido tardío de MCU)
int xhci_escanear_puertos_pendientes(void) { XHCI_EXCLUSIVO();
    return xhci_escanear_cambios_puertos(1);
}

// Fuerza un ciclo de reset oficial en un puerto específico y reconfigura
int xhci_forzar_reset_puerto(uint8_t puerto) { XHCI_EXCLUSIVO();
    if (!g_estado.inicializado || puerto < 1 || puerto > g_estado.max_puertos) return -1;
    uint64_t reg_port = g_op_base + REG_OP_PORTSC_BASE + ((puerto - 1) * 0x10);
    uint32_t portsc = mmio_leer32(reg_port);

    consola_imprimir_linea("");
    consola_imprimir("==> [USB] Forzando ciclo de Reset oficial en Puerto ");
    consola_imprimir_dec(puerto);
    consola_imprimir(" (PORTSC inicial: 0x");
    consola_imprimir_hex(portsc);
    consola_imprimir_linea(")...");

    // Si este puerto tenía un slot asignado, liberar su slot y limpiar estado
    // antes del reset para permitir una re-enumeración completa y limpia.
    uint8_t slot_id = (puerto <= XHCI_MAX_PUERTOS) ? g_puerto_slot[puerto] : 0;
    if (slot_id > 0 && slot_id <= XHCI_MAX_SLOTS) {
        for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
            if (g_teclado_eps[e].slot_id == slot_id) {
                g_teclado_eps[e].activo = 0;
                g_teclado_eps[e].slot_id = 0;
                g_teclado_eps[e].puerto_idx = 0;
            }
        }
        for (int b = 0; b < XHCI_MAX_BULK_EPS; b++) {
            if (g_bulk_eps[b].slot_id == slot_id) {
                g_bulk_eps[b].activo = 0;
                g_bulk_eps[b].slot_id = 0;
            }
        }
        xhci_soltar_slot(slot_id);   /* Lo anterior queda cubierto aqui; si era un hub, tambien sus hijos (H139) */
        g_puerto_slot[puerto] = 0;
    }

    xhci_resetear_puerto(puerto, &portsc);

    consola_imprimir("  -> PORTSC post-reset: 0x");
    consola_imprimir_hex(portsc);
    consola_imprimir(" | Habilitado: ");
    consola_imprimir_linea((portsc & PORTSC_PED) ? "[SÍ]" : "[NO]");

    if (portsc & PORTSC_PED) {
        // USB 2.0 Reset Recovery Time (TRSTRCY - spec pide min 10ms, recomendado 20-50ms)
        // para dar tiempo a que el transceptor y microcontrolador del dispositivo se estabilicen
        esperar_milisegundos(50);
        consola_imprimir_linea_color("  -> Enlace activo. Procediendo a configurar dispositivo...", COLOR_EXITO_DEFAULT);
        xhci_configurar_puerto(puerto, portsc);
        if (g_estado.teclado_detectado && g_estado.teclado_puerto == puerto) {
            consola_imprimir_linea_color("  -> ¡Teclado USB listo y operativo!", COLOR_EXITO_DEFAULT);
        }
    } else {
        consola_imprimir_linea_color("  -> Aviso: El puerto no respondió con PED=1 tras el reset.", COLOR_AVISO_DEFAULT);
    }

    return 0;
}

/* 'raton crudo': que endpoints HID hay armados y de quien son. */
void xhci_hid_listar(void) { XHCI_EXCLUSIVO();
    int n = 0;
    for (unsigned e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
        const struct xhci_ep_teclado *ep = &g_teclado_eps[e];
        if (!ep->activo) continue;
        n++;
        consola_imprimir("  [HID] slot="); consola_imprimir_dec(ep->slot_id);
        consola_imprimir(" puerto="); consola_imprimir_dec(ep->puerto_idx);
        consola_imprimir(" VID="); consola_imprimir_hex(ep->slot_id <= XHCI_MAX_SLOTS ? g_slot_vid[ep->slot_id] : 0);
        consola_imprimir(" PID="); consola_imprimir_hex(ep->slot_id <= XHCI_MAX_SLOTS ? g_slot_pid[ep->slot_id] : 0);
        consola_imprimir(" interfaz="); consola_imprimir_dec(ep->iface_num);
        consola_imprimir(" DCI="); consola_imprimir_dec(ep->ep_dci);
        consola_imprimir(" paquete="); consola_imprimir_dec(ep->ep_max_pkt);
        consola_imprimir(" intervalo="); consola_imprimir_dec(ep->ep_intervalo);
        consola_imprimir(ep->es_raton ? (ep->raton_reporte ? " RATON modo reporte" : ep->raton_boot_confirmado ? " RATON boot confirmado" : " RATON sin confirmar") :
                         ep->es_boot ? " TECLADO boot" : " HID");
        if (ep->es_raton) { consola_imprimir(" descriptor="); consola_imprimir_dec(ep->hid_rep_lon); }
        consola_imprimir_linea("");
    }
    if (!n) consola_imprimir_linea("  [HID] Ningun endpoint HID armado.");
    consola_imprimir("  [HID] Duenio del raton: "); consola_imprimir_linea(g_mouse_propietario ? "tomado (menu o modulo)" : "libre");
}

/* ------------------------------------------------------------------ H208, Fase B: hilo de eventos en CPU 1
 * Bombea el anillo de eventos cada ~2 ms (hilo_dormir_ms(1) con tictac de 1 ms), con prioridad de tiempo real, aunque
 * CPU 0 este ocupada en una descarga o abriendo un video. Si el cerrojo lo tiene CPU 0 (una transferencia en curso, que
 * bombea ella misma), no espera: vuelve en la proxima vuelta. Apagado, el hilo duerme en un evento y CPU 1 no lleva
 * tictac por el. El escaneo de puertos, los resets y la enumeracion siguen en CPU 0 (xhci_sondeo), como antes. */
static evento_t g_ev_hilo_xhci = EVENTO("xhci_hilo");
static hilo_t *g_hilo_xhci;
static volatile int g_hilo_xhci_modo;
static uint64_t g_hilo_bombeos, g_hilo_ocupado;

static void hilo_xhci_eventos(void *u) {
    (void)u;
    for (;;) {
        if (!__atomic_load_n(&g_hilo_xhci_modo, __ATOMIC_ACQUIRE)) { hilo_esperar(&g_ev_hilo_xhci); continue; }
        if (g_estado.inicializado) {
            /* H210: CPU 0 toma el cerrojo por microsegundos (leer una tecla); se insiste hasta ~50 us. Si sigue ocupado
             * es una transferencia, que bombea ella misma: se vuelve en la proxima vuelta */
            int tomado = 0;
            uint64_t limite = rdtsc() + tiempo_ciclos_por_ms() / 20u;
            while (!(tomado = cerrojo_intentar(&g_cerrojo_xhci)) && rdtsc() < limite) __asm__ volatile("pause");
            if (tomado) {
                xhci_bombear_eventos();
                g_hilo_bombeos++;
                cerrojo_soltar(&g_cerrojo_xhci);
            } else {
                g_hilo_ocupado++;
            }
        }
        hilo_dormir_ms(1);
    }
}

/* H210, Fase B2: el resto de lo que hacia xhci_sondeo en CPU 0 (escanear puertos, hubs, reintentos de reset y
 * enumerar), cada 10 ms en CPU 1 con prioridad normal. Las esperas de un reset o de una enumeracion siguen siendo
 * activas y con el cerrojo tomado, como antes en CPU 0: mientras duran, el hilo de eventos (misma CPU) espera, pero las
 * transferencias de la enumeracion bombean el anillo ellas mismas. */
static evento_t g_ev_hilo_puertos = EVENTO("xhci_puertos");
static hilo_t *g_hilo_puertos;
static uint64_t g_puertos_pasadas;
static void hilo_xhci_puertos(void *u) {
    (void)u;
    for (;;) {
        if (!__atomic_load_n(&g_hilo_xhci_modo, __ATOMIC_ACQUIRE)) { hilo_esperar(&g_ev_hilo_puertos); continue; }
        if (g_estado.inicializado && cerrojo_intentar(&g_cerrojo_xhci)) {
            xhci_puertos_pasada(0);
            g_puertos_pasadas++;
            cerrojo_soltar(&g_cerrojo_xhci);
        }
        hilo_dormir_ms(10);
    }
}
static int xhci_hilos_activos(void) {
    return __atomic_load_n(&g_hilo_xhci_modo, __ATOMIC_ACQUIRE) && g_hilo_xhci && g_hilo_puertos;
}
uint64_t xhci_hilo_puertos_pasadas(void) { return g_puertos_pasadas; }

int xhci_hilo(int encender) {
    if (encender && !g_hilo_xhci) {
        g_hilo_xhci = hilo_crear(hilo_xhci_eventos, 0, "xhci_eventos", 1, HILO_TIEMPO_REAL, 0);
        if (!g_hilo_xhci) return -1;
    }
    if (encender && !g_hilo_puertos) {
        g_hilo_puertos = hilo_crear(hilo_xhci_puertos, 0, "xhci_puertos", 1, HILO_NORMAL, 0);
        if (!g_hilo_puertos) return -1;
    }
    int antes = g_hilo_xhci_modo;
    __atomic_store_n(&g_hilo_xhci_modo, encender ? 1 : 0, __ATOMIC_RELEASE);
    if (encender && !antes) { evento_senalar(&g_ev_hilo_xhci); evento_senalar(&g_ev_hilo_puertos); }
    return g_hilo_xhci_modo;
}

void xhci_hilo_estado(int *modo, uint64_t *bombeos, uint64_t *ocupado) {
    if (modo) *modo = g_hilo_xhci_modo && g_hilo_xhci;
    if (bombeos) *bombeos = g_hilo_bombeos;
    if (ocupado) *ocupado = g_hilo_ocupado;
}