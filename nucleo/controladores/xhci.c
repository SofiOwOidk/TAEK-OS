#include "xhci.h"
#include "consola.h"
#include "../arquitectura/x86_64/puertos.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/paginacion.h"
#include "../base/dma.h"
#include "../base/tiempo.h"
#include "../base/memoria.h"
#include "../base/huevo.h"

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
    int      activo;

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
    uint64_t tsc = rdtsc();
    uint64_t ms = tiempo_obtener_milisegundos();
    uint64_t reg_db = g_db_base + ((uint64_t)slot * 4);
    __asm__ volatile ("mfence" ::: "memory");
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
    mmio_escribir32(reg_db, ep_o_cmd);
    __asm__ volatile ("mfence" ::: "memory");
}

static inline void xhci_sincronizar_evento_actual(void) {
    dma_sincronizar_dispositivo_a_cpu((const void *)&g_event_ring[g_event_idx], sizeof(g_event_ring[g_event_idx]));
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
void xhci_imprimir_diagnostico_completo(void) {
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

    uint64_t tsc_inicio = rdtsc();
    uint64_t ms_inicio = tiempo_obtener_milisegundos();

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

    // Esperar el evento de finalización en el anillo de eventos
    int timeout = 5000;
    while (timeout > 0) {
        xhci_sincronizar_evento_actual();
        volatile struct trb_xhci *evt = &g_event_ring[g_event_idx];
        uint8_t ciclo_leido = evt->control & 1;
        uint8_t ciclo_esperado = g_event_cycle;

        if (ciclo_leido == ciclo_esperado) {
            uint64_t tsc_fin = rdtsc();
            uint64_t ms_fin = tiempo_obtener_milisegundos();
            uint32_t delta_ms = (uint32_t)(ms_fin - ms_inicio);
            uint64_t delta_ciclos = tsc_fin - tsc_inicio;
            uint32_t usbsts_post = mmio_leer32(g_op_base + REG_OP_USBSTS);

            uint8_t tipo = (evt->control >> 10) & 0x3F;
            uint8_t codigo_comp = (evt->estado >> 24) & 0xFF;
            uint8_t evt_slot = (evt->control >> 24) & 0xFF;

            serial_imprimir("  [xHCI EVT RECIBIDO] Evt[");
            serial_imprimir_dec(g_event_idx);
            serial_imprimir("]: Tipo=");
            serial_imprimir_dec(tipo);
            serial_imprimir(" CC=");
            serial_imprimir_dec(codigo_comp);
            serial_imprimir(" Slot=");
            serial_imprimir_dec(evt_slot);
            serial_imprimir(" CycLeido=");
            serial_imprimir_dec(ciclo_leido);
            serial_imprimir(" CycEsp=");
            serial_imprimir_dec(ciclo_esperado);
            serial_imprimir(" (dt=");
            serial_imprimir_dec(delta_ms);
            serial_imprimir(" ms, ciclos=");
            serial_imprimir_hex(delta_ciclos);
            serial_imprimir(" USBSTS_post=");
            serial_imprimir_hex(usbsts_post);
            serial_imprimir_linea(")");

            if (tipo == TRB_TIPO_CMD_COMP_EVT) {
                if (out_evt) *out_evt = *evt;

                // Avanzar puntero de dequeue
                g_event_idx++;
                if (g_event_idx >= XHCI_TAM_ANILLO) {
                    g_event_idx = 0;
                    g_event_cycle = !g_event_cycle;
                    g_evt_ring_wraparounds++;
                    serial_imprimir("  [xHCI EVT] Wraparound Anillo Eventos=");
                    serial_imprimir_dec(g_evt_ring_wraparounds);
                    serial_imprimir_linea("");
                }

                uint64_t erdp_pre = mmio_leer64(g_rts_base + 0x20 + 0x18);
                uint64_t nuevo_erdp = g_event_ring_fisica + ((uint64_t)g_event_idx * sizeof(struct trb_xhci));
                mmio_escribir64(g_rts_base + 0x20 + 0x18, nuevo_erdp | (1U << 3)); // EHB = 1
                uint64_t erdp_post = mmio_leer64(g_rts_base + 0x20 + 0x18);
                mmio_escribir32(g_rts_base + 0x20 + 0x00, 0x03); // Limpiar IP en IMAN

                serial_imprimir("  [xHCI ERDP] Pre=");
                serial_imprimir_hex(erdp_pre);
                serial_imprimir(" (EHB=");
                serial_imprimir_dec((uint32_t)((erdp_pre >> 3) & 1));
                serial_imprimir(") -> Escrito=");
                serial_imprimir_hex(nuevo_erdp | 8);
                serial_imprimir(" -> Post=");
                serial_imprimir_hex(erdp_post);
                serial_imprimir(" (EHB=");
                serial_imprimir_dec((uint32_t)((erdp_post >> 3) & 1));
                serial_imprimir_linea(")");

                return (codigo_comp == 1) ? 0 : (int)codigo_comp;
            }

            // Consumir eventos intermedios (ej: Port Status Change) para no trabar el anillo de eventos
            g_event_idx++;
            if (g_event_idx >= XHCI_TAM_ANILLO) {
                g_event_idx = 0;
                g_event_cycle = !g_event_cycle;
                g_evt_ring_wraparounds++;
            }
            uint64_t erdp_pre = mmio_leer64(g_rts_base + 0x20 + 0x18);
            uint64_t nuevo_erdp = g_event_ring_fisica + ((uint64_t)g_event_idx * sizeof(struct trb_xhci));
            mmio_escribir64(g_rts_base + 0x20 + 0x18, nuevo_erdp | (1U << 3));
            uint64_t erdp_post = mmio_leer64(g_rts_base + 0x20 + 0x18);
            mmio_escribir32(g_rts_base + 0x20 + 0x00, 0x03);

            serial_imprimir("  [xHCI ERDP INTERMEDIO] Pre=");
            serial_imprimir_hex(erdp_pre);
            serial_imprimir(" Post=");
            serial_imprimir_hex(erdp_post);
            serial_imprimir_linea("");
            continue;
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

    // Consumir el evento Command Aborted / Command Ring Stopped si llegó al Event Ring
    xhci_sincronizar_evento_actual();
    while ((g_event_ring[g_event_idx].control & 1) == g_event_cycle) {
        volatile struct trb_xhci *e = &g_event_ring[g_event_idx];
        uint8_t t = (e->control >> 10) & 0x3F;
        uint8_t cc = (e->estado >> 24) & 0xFF;
        serial_imprimir("  [xHCI ABORT EVENTO] Tipo: ");
        serial_imprimir_dec(t);
        serial_imprimir(" Codigo: ");
        serial_imprimir_dec(cc);
        serial_imprimir_linea("");

        g_event_idx++;
        if (g_event_idx >= XHCI_TAM_ANILLO) {
            g_event_idx = 0;
            g_event_cycle = !g_event_cycle;
            g_evt_ring_wraparounds++;
        }
        uint64_t nuevo_erdp = g_event_ring_fisica + (g_event_idx * sizeof(struct trb_xhci));
        mmio_escribir64(g_rts_base + 0x20 + 0x18, nuevo_erdp | (1U << 3));
        mmio_escribir32(g_rts_base + 0x20 + 0x00, 0x03);
        xhci_sincronizar_evento_actual();
        if (t == TRB_TIPO_CMD_COMP_EVT && (cc == 24 || cc == 26)) {
            break;
        }
    }

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
void xhci_sondeo(void) {
    if (!g_estado.inicializado) return;

    // Escanear cambios físicos en puertos raíz (Hotplug reactivo)
    // Se ejecuta de inmediato si el controlador reporta Port Change Detect (PCD) en USBSTS,
    // o periódicamente cada 200 ms como red de seguridad sin sobrecarga.
    static uint64_t g_ultimo_escaneo_puertos = 0;
    uint64_t ahora = tiempo_obtener_milisegundos();
    uint32_t usbsts = mmio_leer32(g_op_base + REG_OP_USBSTS);
    if ((usbsts & USBSTS_PCD) || (ahora - g_ultimo_escaneo_puertos >= 200)) {
        g_ultimo_escaneo_puertos = ahora;
        if (usbsts & USBSTS_PCD) {
            mmio_escribir32(g_op_base + REG_OP_USBSTS, USBSTS_PCD);
        }
        xhci_escanear_cambios_puertos(1);
    }

    // Verificar si hay eventos pendientes en el Event Ring
    while (1) {
        xhci_sincronizar_evento_actual();
        volatile struct trb_xhci *evt = &g_event_ring[g_event_idx];
        uint8_t ciclo = evt->control & 1;
        if (ciclo != g_event_cycle) break;

        uint8_t tipo = (evt->control >> 10) & 0x3F;
        uint8_t slot_id = (evt->control >> 24) & 0xFF;

        uint8_t ep_dci = (evt->control >> 16) & 0x1F;
        uint8_t codigo = (evt->estado >> 24) & 0xFF;

        g_estado.ultimo_evento_trb_tipo = tipo;
        g_estado.paquetes_recibidos++;
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
            } else if (codigo == 1 || codigo == 13) { // 1 = Success, 13 = Short Packet
                g_estado.reportes_hid_recibidos++;
                uint32_t residual = evt->estado & 0xFFFFFF;
                uint32_t tam_recibido = (residual <= ep->ep_max_pkt) ? (ep->ep_max_pkt - residual) : ep->ep_max_pkt;
                if (tam_recibido == 0) tam_recibido = ep->ep_max_pkt;
                if (tam_recibido > 64) tam_recibido = 64;

                uint8_t *buf = ep->bufer;
                dma_sincronizar_dispositivo_a_cpu(buf, ep->ep_max_pkt);
                int reporte_cambio = (tam_recibido != ep->ultimo_tam);
                for (uint32_t i = 0; i < tam_recibido && i < sizeof(ep->ultimo_reporte); i++) {
                    if (buf[i] != ep->ultimo_reporte[i]) reporte_cambio = 1;
                }
                if (reporte_cambio) {
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

                // CASO A: Endpoint Boot estándar (prioridad absoluta para no confundir modificadores con Report IDs)
                if (ep->es_boot) {
                    mod = buf[0];
                    shift = (mod & 0x02) || (mod & 0x20);

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
                            char c = shift ? g_hid_a_ascii_shift[scancode] : g_hid_a_ascii_normal[scancode];
                            if (c != 0) {
                                uint32_t sig_cabeza = (g_buf_cabeza + 1) % TAM_BUFFER_TECLAS;
                                if (sig_cabeza != g_buf_cola) {
                                    g_buffer_teclado[g_buf_cabeza] = c;
                                    g_buf_cabeza = sig_cabeza;
                                    g_estado.ultimo_slot_tecla = ep->slot_id;
                                    g_estado.ultimo_puerto_tecla = ep->puerto_idx;
                                    g_estado.ultimo_caracter = (uint8_t)c;
                                }
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
                                            }
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
                            char c = shift ? g_hid_a_ascii_shift[scancode] : g_hid_a_ascii_normal[scancode];
                            if (c != 0) {
                                uint32_t sig_cabeza = (g_buf_cabeza + 1) % TAM_BUFFER_TECLAS;
                                if (sig_cabeza != g_buf_cola) {
                                    g_buffer_teclado[g_buf_cabeza] = c;
                                    g_buf_cabeza = sig_cabeza;
                                    g_estado.ultimo_slot_tecla = ep->slot_id;
                                    g_estado.ultimo_puerto_tecla = ep->puerto_idx;
                                    g_estado.ultimo_caracter = (uint8_t)c;
                                }
                            }
                        }
                    }
                }
                // CASO D: Reporte Boot estándar plano de 8 bytes (fallback para endpoints sin marca es_boot)
                else if (tam_recibido >= 8) {
                    mod = buf[0];
                    shift = (mod & 0x02) || (mod & 0x20);

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
                            char c = shift ? g_hid_a_ascii_shift[scancode] : g_hid_a_ascii_normal[scancode];
                            if (c != 0) {
                                uint32_t sig_cabeza = (g_buf_cabeza + 1) % TAM_BUFFER_TECLAS;
                                if (sig_cabeza != g_buf_cola) {
                                    g_buffer_teclado[g_buf_cabeza] = c;
                                    g_buf_cabeza = sig_cabeza;
                                    g_estado.ultimo_slot_tecla = ep->slot_id;
                                    g_estado.ultimo_puerto_tecla = ep->puerto_idx;
                                    g_estado.ultimo_caracter = (uint8_t)c;
                                }
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

        // Avanzar puntero de dequeue del Event Ring
        g_event_idx++;
        if (g_event_idx >= XHCI_TAM_ANILLO) {
            g_event_idx = 0;
            g_event_cycle = !g_event_cycle;
            g_evt_ring_wraparounds++;
        }

        uint64_t erdp = g_event_ring_fisica + (g_event_idx * sizeof(struct trb_xhci));
        mmio_escribir64(g_rts_base + 0x20 + 0x18, erdp | (1U << 3));
        mmio_escribir32(g_rts_base + 0x20 + 0x00, 0x03);
    }
}

int xhci_hay_datos(void) {
    xhci_sondeo();
    return (g_buf_cabeza != g_buf_cola);
}

char xhci_leer_caracter(void) {
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

    // Garantizar que haya al menos 4 posiciones libres para que los TRBs del TD (Setup, Data, Status)
    // queden completamente contiguos y no se dividan a través del Link TRB sin bandera Chain.
    if (ep0_idx >= XHCI_TAM_ANILLO - 4) {
        volatile struct trb_xhci *link = &ep0_ring[ep0_idx];
        link->parametro = ep0_ring_fisica;
        link->estado    = 0;
        link->control   = (TRB_TIPO_LINK << 10) | (1U << 1) /* TC */ | (ep0_cycle ? 1 : 0);
        dma_sincronizar_cpu_a_dispositivo((const void *)link, sizeof(*link));
        __asm__ volatile ("mfence" ::: "memory");
        ep0_idx = 0;
        ep0_cycle = !ep0_cycle;
    }

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

    // Tocar timbre de EP0 (Target = DCI 1 = EP0)
    xhci_tocar_timbre(slot_id, 1);

    // Esperar el evento de transferencia completada (timeout extendido a 2000 ms)
    int timeout = 2000;
    while (timeout > 0) {
        xhci_sincronizar_evento_actual();
        volatile struct trb_xhci *evt = &g_event_ring[g_event_idx];
        uint8_t ciclo_evt = evt->control & 1;

        if (ciclo_evt == g_event_cycle) {
            uint8_t tipo = (evt->control >> 10) & 0x3F;
            uint8_t codigo = (evt->estado >> 24) & 0xFF;
            uint8_t evt_slot = (evt->control >> 24) & 0xFF;
            uint8_t evt_ep   = (evt->control >> 16) & 0x1F;

            // Avanzar dequeue
            g_event_idx++;
            if (g_event_idx >= XHCI_TAM_ANILLO) {
                g_event_idx = 0;
                g_event_cycle = !g_event_cycle;
                g_evt_ring_wraparounds++;
            }
            uint64_t erdp = g_event_ring_fisica + (g_event_idx * sizeof(struct trb_xhci));
            mmio_escribir64(g_rts_base + 0x20 + 0x18, erdp | (1U << 3));
            mmio_escribir32(g_rts_base + 0x20 + 0x00, 0x03);

            if (tipo == TRB_TIPO_TRANSFER_EVT && evt_slot == slot_id && evt_ep == 1) {
                g_estado.ultimo_codigo_control = codigo;
                // Éxito = código 1 (Success) o 13 (Short Packet = OK para descriptores)
                if (codigo == 1 || codigo == 13) {
                    if (datos && longitud > 0) {
                        dma_sincronizar_dispositivo_a_cpu(datos, longitud);
                    }
                    return 0;
                }
                serial_imprimir("  [xHCI] Transferencia de control completada con código: ");
                serial_imprimir_dec(codigo);
                serial_imprimir_linea("");
                g_estado.fallos_control++;
                return (int)codigo;
            }
            // Evento intermedio o de otro endpoint/dispositivo, ignorar y seguir esperando
            continue;
        }
        esperar_milisegundos(1);
        timeout--;
    }

    serial_imprimir_linea("  [xHCI AVISO] Timeout en transferencia de control");
    g_estado.fallos_control++;
    return -1;
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

    // Si este puerto ya tenía un slot asignado previamente, liberarlo antes de reconfigurar
    if (puerto_idx <= XHCI_MAX_PUERTOS && g_puerto_slot[puerto_idx] > 0) {
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
    slot_ctx[0] = (1U << 27) | ((uint32_t)velocidad << 20); // 1 context entry, speed
    slot_ctx[1] = ((uint32_t)puerto_idx << 16);             // Root hub port

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
        eval_slot_ctx[0] = (1U << 27); // Context Entries = 1. Speed (bits 23:20) es RsvdZ en Evaluate Context!
        eval_slot_ctx[1] = ((uint32_t)puerto_idx << 16);             // Root hub port

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

    struct xhci_ep_teclado *eps_este_dispositivo[4] = {0};
    int num_eps_este_dispositivo = 0;

    // Escanear todas las interfaces y registrar endpoints Interrupt IN
    while (offset < total_cfg_len) {
        uint8_t len = desc_buf[offset];
        if (len == 0 || offset + len > total_cfg_len) break;
        uint8_t dtype = desc_buf[offset + 1];

        if (dtype == 4 && len >= 9) { // Interface Descriptor
            iface_actual_num = desc_buf[offset + 2];
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

            // Aceptamos interfaces HID de teclado (Clase 3, Protocolo 1=Boot Keyboard o 0=Compuesto/NKRO)
            // Descartamos explícitamente ratones (Protocolo 2=Mouse) para que no secuestren el controlador de teclado
            if (if_class == 3 && if_protocol != 2) {
                iface_actual_es_hid = 1;
                iface_actual_es_boot = (if_subclass == 1 && if_protocol == 1);
                serial_imprimir_linea(" [HID Teclado Aceptada]");
            } else {
                iface_actual_es_hid = 0;
                iface_actual_es_boot = 0;
                if (if_class == 3 && if_protocol == 2) {
                    serial_imprimir_linea(" [HID Ratón Omitido]");
                } else {
                    serial_imprimir_linea(" [Ignorada]");
                }
            }
        } else if (dtype == 5 && len >= 7 && iface_actual_es_hid) { // Endpoint Descriptor
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
        }

        offset += len;
    }

    if (num_eps_este_dispositivo == 0) {
        consola_imprimir_linea_color("    -> No es teclado HID (liberando slot)", COLOR_PROMPT_DEFAULT);
        serial_imprimir_linea("  [xHCI] No se encontraron endpoints de teclado HID compatibles. Omitiendo.");
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
        // Obligatorio por xHCI 1.2 §6.2.3.8 para endpoints periódicos; si Max ESIT Payload
        // es 0, los controladores Intel Raptor Lake congelan el microcódigo del scheduler.
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

        if (eps_este_dispositivo[e]->es_boot) {
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

    // 12. Registrar ranura, mapeos y actualizar estado global
    g_slot_puerto[slot_id] = puerto_idx;
    g_puerto_slot[puerto_idx] = slot_id;
    g_slot_vid[slot_id] = dev_desc->id_proveedor;
    g_slot_pid[slot_id] = dev_desc->id_producto;
    g_slot_dev_ctx[slot_id] = dev_ctx;
    g_slot_dev_ctx_fisica[slot_id] = dev_ctx_fisica;

    g_estado.teclado_detectado = 1;
    g_estado.teclado_slot_id = slot_id;
    g_estado.teclado_ep_dci  = eps_este_dispositivo[0]->ep_dci;
    g_estado.teclado_num_eps = (uint8_t)num_eps_este_dispositivo;
    g_estado.teclado_puerto  = puerto_idx;
    g_estado.teclado_id_proveedor = dev_desc->id_proveedor;
    g_estado.teclado_id_producto  = dev_desc->id_producto;
    g_estado.etapa_enumeracion = 8;

    // Calcular cuántos dispositivos de teclado distintos están activos concurrentemente
    int total_teclados = 0;
    uint8_t slots_contados[XHCI_MAX_SLOTS + 1] = {0};
    for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
        if (g_teclado_eps[e].activo && g_teclado_eps[e].slot_id > 0) {
            if (!slots_contados[g_teclado_eps[e].slot_id]) {
                slots_contados[g_teclado_eps[e].slot_id] = 1;
                total_teclados++;
            }
        }
    }
    g_estado.teclados_activos = total_teclados;

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

    consola_imprimir("    ==> ¡Teclado USB Configurado y Operativo [OK]! (");
    consola_imprimir_dec(num_eps_este_dispositivo);
    consola_imprimir_linea_color(" Endpoints)", COLOR_EXITO_DEFAULT);

    serial_imprimir("  [xHCI EXITOSO] ¡Teclado USB en Slot ");
    serial_imprimir_dec(slot_id);
    serial_imprimir(" (Puerto ");
    serial_imprimir_dec(puerto_idx);
    serial_imprimir(") configurado con ");
    serial_imprimir_dec(num_eps_este_dispositivo);
    serial_imprimir_linea(" Endpoint(s) armados para recepción!");
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
int xhci_iniciar(void) {
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
int xhci_obtener_info_puerto(uint8_t puerto, uint32_t *portsc_out, int *conectado_out, int *habilitado_out, uint8_t *velocidad_out) {
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
int xhci_escanear_cambios_puertos(int verbose) {
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
            }
        }
        // 2. Detección de Desconexión: el puerto pasó de conectado (1) a desconectado (0)
        else if (ccs == 0 && ccs_anterior == 1) {
            cambios++;
            g_puerto_estado_ccs[p] = 0;
            if (g_estado.puertos_conectados > 0) g_estado.puertos_conectados--;

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
                for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
                    if (g_teclado_eps[e].slot_id == slot_id) {
                        g_teclado_eps[e].activo = 0;
                        g_teclado_eps[e].slot_id = 0;
                        g_teclado_eps[e].puerto_idx = 0;
                    }
                }
                xhci_liberar_slot(slot_id, NULL, 0, g_slot_dev_ctx[slot_id], g_slot_dev_ctx_fisica[slot_id], NULL, 0);
                g_slot_dev_ctx[slot_id] = NULL;
                g_slot_dev_ctx_fisica[slot_id] = 0;
                g_slot_puerto[slot_id] = 0;
                g_puerto_slot[p] = 0;
                g_slot_vid[slot_id] = 0;
                g_slot_pid[slot_id] = 0;

                int num_teclados = 0;
                uint8_t slots_vistos[XHCI_MAX_SLOTS + 1] = {0};
                for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
                    if (g_teclado_eps[e].activo && g_teclado_eps[e].slot_id > 0) {
                        if (!slots_vistos[g_teclado_eps[e].slot_id]) {
                            slots_vistos[g_teclado_eps[e].slot_id] = 1;
                            num_teclados++;
                        }
                    }
                }
                g_estado.teclados_activos = num_teclados;
                g_estado.teclado_detectado = (num_teclados > 0);
                if (g_estado.teclado_slot_id == slot_id) {
                    g_estado.teclado_slot_id = 0;
                    g_estado.teclado_puerto = 0;
                }
            }

            // Limpiar banderas de cambio residuales en el registro del puerto (CSC, PEC, PRC)
            mmio_escribir32(reg_port, xhci_portsc_neutral(portsc) | PORTSC_CSC | PORTSC_PEC | PORTSC_PRC);
        }
    }

    return cambios;
}

// Re-escanea puertos en caliente (hotplug / encendido tardío de MCU)
int xhci_escanear_puertos_pendientes(void) {
    return xhci_escanear_cambios_puertos(1);
}

// Fuerza un ciclo de reset oficial en un puerto específico y reconfigura
int xhci_forzar_reset_puerto(uint8_t puerto) {
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
        xhci_liberar_slot(slot_id, NULL, 0, g_slot_dev_ctx[slot_id], g_slot_dev_ctx_fisica[slot_id], NULL, 0);
        g_slot_dev_ctx[slot_id] = NULL;
        g_slot_dev_ctx_fisica[slot_id] = 0;
        g_slot_puerto[slot_id] = 0;
        g_puerto_slot[puerto] = 0;
        g_slot_vid[slot_id] = 0;
        g_slot_pid[slot_id] = 0;

        int num_teclados = 0;
        uint8_t slots_vistos[XHCI_MAX_SLOTS + 1] = {0};
        for (int e = 0; e < XHCI_MAX_TECLADO_EPS; e++) {
            if (g_teclado_eps[e].activo && g_teclado_eps[e].slot_id > 0) {
                if (!slots_vistos[g_teclado_eps[e].slot_id]) {
                    slots_vistos[g_teclado_eps[e].slot_id] = 1;
                    num_teclados++;
                }
            }
        }
        g_estado.teclados_activos = num_teclados;
        g_estado.teclado_detectado = (num_teclados > 0);
        if (g_estado.teclado_slot_id == slot_id) {
            g_estado.teclado_slot_id = 0;
            g_estado.teclado_puerto = 0;
        }
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
