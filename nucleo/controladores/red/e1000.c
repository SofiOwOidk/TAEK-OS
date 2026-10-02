#include "red.h"
#include "../../arquitectura/x86_64/pci.h"
#include "../../arquitectura/x86_64/serial.h"
#include "../../base/paginacion.h"
#include "../../base/memoria.h"
#include "../../base/dma.h"
#include "../../base/tiempo.h"

// ------------------------------------------------------------------ Registros
#define E1000_CTRL        0x00000
#define E1000_STATUS      0x00008
#define E1000_EECD        0x00010
#define E1000_EERD        0x00014
#define E1000_CTRL_EXT    0x00018
#define E1000_MDIC        0x00020
#define E1000_ICR         0x000C0
#define E1000_IMS         0x000D0
#define E1000_IMC         0x000D8
#define E1000_RCTL        0x00100
#define E1000_TCTL        0x00400
#define E1000_TIPG        0x00410
#define E1000_RDBAL       0x02800
#define E1000_RDBAH       0x02804
#define E1000_RDLEN       0x02808
#define E1000_RDH         0x02810
#define E1000_RDT         0x02818
#define E1000_TDBAL       0x03800
#define E1000_TDBAH       0x03804
#define E1000_TDLEN       0x03808
#define E1000_TDH         0x03810
#define E1000_TDT         0x03818
#define E1000_RFCTL       0x05008
#define E1000_MTA         0x05200
#define E1000_RAL0        0x05400
#define E1000_RAH0        0x05404

// Bits de CTRL
#define E1000_CTRL_FD       0x00000001
#define E1000_CTRL_LRST     0x00000008
#define E1000_CTRL_ASDE     0x00000020
#define E1000_CTRL_SLU      0x00000040
#define E1000_CTRL_ILOS     0x00000100
#define E1000_CTRL_RST      0x04000000
#define E1000_CTRL_PHY_RST  0x80000000

// Bits de STATUS
#define E1000_STATUS_FD          0x00000001
#define E1000_STATUS_LU          0x00000002
#define E1000_STATUS_SPEED_MASK  0x000000C0
#define E1000_STATUS_SPEED_1000  0x00000080
#define E1000_STATUS_SPEED_100   0x00000040

// Bits de RCTL
#define E1000_RCTL_EN      0x00000002
#define E1000_RCTL_SBP     0x00000004
#define E1000_RCTL_UPE     0x00000008
#define E1000_RCTL_MPE     0x00000010
#define E1000_RCTL_LPE     0x00000020
#define E1000_RCTL_BAM     0x00008000
#define E1000_RCTL_SECRC   0x04000000

// Bits de TCTL
#define E1000_TCTL_EN      0x00000002
#define E1000_TCTL_PSP     0x00000008

// Descriptores legacy
#define E1000_TXD_CMD_EOP   0x01
#define E1000_TXD_CMD_IFCS  0x02
#define E1000_TXD_CMD_RS    0x08
#define E1000_TXD_STAT_DD   0x01
#define E1000_RXD_STAT_DD   0x01
#define E1000_RXD_STAT_EOP  0x02

// MDIC (acceso al PHY)
#define E1000_MDIC_REG_SHIFT 16
#define E1000_MDIC_PHY_SHIFT 21
#define E1000_MDIC_OP_WRITE  (0x1u << 26)
#define E1000_MDIC_OP_READ   (0x2u << 26)
#define E1000_MDIC_READY     (1u << 28)
#define E1000_MDIC_ERROR     (1u << 30)

#define E1000_RAH_AV 0x80000000u

#define RED_RX_DESC_COUNT 64u
#define RED_TX_DESC_COUNT 32u
#define RED_RX_BUF_SIZE   2048u
#define RED_TX_BUF_SIZE   2048u

// ------------------------------------------------------------- Descriptores
struct __attribute__((packed)) red_rx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errores;
    uint16_t special;
};

struct __attribute__((packed)) red_tx_desc {
    uint64_t buffer_addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  comando;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
};

// ------------------------------------------------------------------- Modelos
struct red_modelo {
    uint16_t id;
    uint8_t  familia;
    const char *nombre;
};

static const struct red_modelo g_modelos[] = {
    // --- e1000 clásico (8254x) ---
    { 0x100E, RED_FAMILIA_8254X, "Intel 82540EM Gigabit Ethernet" },
    { 0x100F, RED_FAMILIA_8254X, "Intel 82545EM Gigabit Ethernet" },
    { 0x1004, RED_FAMILIA_8254X, "Intel 82543GC Gigabit Ethernet" },
    { 0x1010, RED_FAMILIA_8254X, "Intel 82546EB Gigabit Ethernet" },
    { 0x1012, RED_FAMILIA_8254X, "Intel 82546EB Gigabit Ethernet" },
    { 0x1015, RED_FAMILIA_8254X, "Intel 82540EM Gigabit Ethernet" },
    { 0x1016, RED_FAMILIA_8254X, "Intel 82545EM Gigabit Ethernet" },
    { 0x1017, RED_FAMILIA_8254X, "Intel 82544EI Gigabit Ethernet" },
    { 0x1018, RED_FAMILIA_8254X, "Intel 82541EI Gigabit Ethernet" },
    { 0x1019, RED_FAMILIA_8254X, "Intel 82547EI Gigabit Ethernet" },
    { 0x101D, RED_FAMILIA_8254X, "Intel 82546EB Gigabit Ethernet" },
    { 0x1026, RED_FAMILIA_8254X, "Intel 82545GM Gigabit Ethernet" },
    { 0x1027, RED_FAMILIA_8254X, "Intel 82545GM Gigabit Ethernet" },
    { 0x1028, RED_FAMILIA_8254X, "Intel 82545GM Gigabit Ethernet" },
    { 0x1075, RED_FAMILIA_8254X, "Intel 82547GI Gigabit Ethernet" },
    { 0x1076, RED_FAMILIA_8254X, "Intel 82541GI Gigabit Ethernet" },
    { 0x1077, RED_FAMILIA_8254X, "Intel 82547GI Gigabit Ethernet" },
    { 0x1078, RED_FAMILIA_8254X, "Intel 82541GI Gigabit Ethernet" },
    { 0x1079, RED_FAMILIA_8254X, "Intel 82546GB Gigabit Ethernet" },
    { 0x107A, RED_FAMILIA_8254X, "Intel 82546GB Gigabit Ethernet" },
    { 0x107B, RED_FAMILIA_8254X, "Intel 82546GB Gigabit Ethernet" },
    { 0x107C, RED_FAMILIA_8254X, "Intel 82541PI Gigabit Ethernet" },

    // --- e1000e discreto (8257x) ---
    { 0x105E, RED_FAMILIA_8257X, "Intel 82571EB Gigabit Ethernet" },
    { 0x105F, RED_FAMILIA_8257X, "Intel 82571EB Gigabit Ethernet" },
    { 0x1060, RED_FAMILIA_8257X, "Intel 82571EB Gigabit Ethernet" },
    { 0x1096, RED_FAMILIA_8257X, "Intel 82573E Gigabit Ethernet" },
    { 0x1098, RED_FAMILIA_8257X, "Intel 82573E Gigabit Ethernet" },
    { 0x1099, RED_FAMILIA_8257X, "Intel 82573E Gigabit Ethernet" },
    { 0x109A, RED_FAMILIA_8257X, "Intel 82573L Gigabit Ethernet" },
    { 0x10A4, RED_FAMILIA_8257X, "Intel 82571EB Gigabit Ethernet" },
    { 0x10A5, RED_FAMILIA_8257X, "Intel 82571EB Gigabit Ethernet" },
    { 0x10B9, RED_FAMILIA_8257X, "Intel 82572EI Gigabit Ethernet" },
    { 0x10BA, RED_FAMILIA_8257X, "Intel 80003ES2LAN Gigabit Ethernet" },
    { 0x10BB, RED_FAMILIA_8257X, "Intel 80003ES2LAN Gigabit Ethernet" },
    { 0x10BC, RED_FAMILIA_8257X, "Intel 82571EB Gigabit Ethernet" },
    { 0x10C4, RED_FAMILIA_8257X, "Intel 82562GT Gigabit Ethernet" },
    { 0x10C5, RED_FAMILIA_8257X, "Intel 82562G Gigabit Ethernet" },
    { 0x10D3, RED_FAMILIA_8257X, "Intel 82574L Gigabit Ethernet" },
    { 0x10D4, RED_FAMILIA_8257X, "Intel 82574L Gigabit Ethernet" },
    { 0x10D5, RED_FAMILIA_8257X, "Intel 82574L Gigabit Ethernet" },
    { 0x10F5, RED_FAMILIA_ICH,   "Intel 82567LM Gigabit Ethernet" },
    { 0x10F6, RED_FAMILIA_ICH,   "Intel 82567LM Gigabit Ethernet" },
    { 0x1501, RED_FAMILIA_ICH,   "Intel 82567V Gigabit Ethernet" },
    { 0x1502, RED_FAMILIA_ICH,   "Intel 82579LM Gigabit Ethernet" },
    { 0x1503, RED_FAMILIA_ICH,   "Intel 82579V Gigabit Ethernet" },

    // --- e1000e integrado PCH (I217 / I218 / I219) ---
    { 0x153A, RED_FAMILIA_ICH, "Intel I217-LM Gigabit Ethernet" },
    { 0x153B, RED_FAMILIA_ICH, "Intel I217-V Gigabit Ethernet" },
    { 0x1559, RED_FAMILIA_ICH, "Intel I218-V Gigabit Ethernet" },
    { 0x155A, RED_FAMILIA_ICH, "Intel I218-LM Gigabit Ethernet" },
    { 0x15A0, RED_FAMILIA_ICH, "Intel I218-LM Gigabit Ethernet" },
    { 0x15A1, RED_FAMILIA_ICH, "Intel I218-V Gigabit Ethernet" },
    { 0x15A2, RED_FAMILIA_ICH, "Intel I218-LM Gigabit Ethernet" },
    { 0x15A3, RED_FAMILIA_ICH, "Intel I218-V Gigabit Ethernet" },
    { 0x15B7, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15B8, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15B9, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15BA, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15BB, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15BC, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15BD, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15BE, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15D6, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15D7, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15D8, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15DF, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15E0, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15E1, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15E2, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x15E3, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x0D4C, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x0D4D, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x0D4E, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x0D4F, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
    { 0x0D53, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15FB, RED_FAMILIA_ICH, "Intel I219-LM Gigabit Ethernet" },
    { 0x15FC, RED_FAMILIA_ICH, "Intel I219-V Gigabit Ethernet" },
};

// -------------------------------------------------------------- Estado global
static struct red_info g_info;

static volatile uint8_t *g_mmio = NULL;

static struct red_rx_desc *g_rx_ring = NULL;
static uint64_t            g_rx_ring_fisica = 0;
static uint8_t            *g_rx_buffers = NULL;
static uint64_t            g_rx_buffers_fisica = 0;

static struct red_tx_desc *g_tx_ring = NULL;
static uint64_t            g_tx_ring_fisica = 0;
static uint8_t            *g_tx_buffers = NULL;
static uint64_t            g_tx_buffers_fisica = 0;

static uint32_t g_rx_idx = 0;
static uint32_t g_tx_idx = 0;

// ------------------------------------------------------------------- MMIO
static inline uint32_t red_mmio_leer32(uint32_t reg) {
    return *(volatile uint32_t *)(g_mmio + reg);
}

static inline void red_mmio_escribir32(uint32_t reg, uint32_t valor) {
    *(volatile uint32_t *)(g_mmio + reg) = valor;
}

// --------------------------------------------------------- Búsqueda de modelo
static const struct red_modelo *red_buscar_modelo(uint16_t id) {
    for (size_t i = 0; i < sizeof(g_modelos) / sizeof(g_modelos[0]); i++) {
        if (g_modelos[i].id == id) return &g_modelos[i];
    }
    return NULL;
}

// -------------------------------------------------------------- Dirección MAC
static void red_serial_mac(const uint8_t mac[RED_MAX_MAC]) {
    const char *h = "0123456789ABCDEF";
    for (int i = 0; i < RED_MAX_MAC; i++) {
        serial_escribir_caracter(h[(mac[i] >> 4) & 0xF]);
        serial_escribir_caracter(h[mac[i] & 0xF]);
        if (i < RED_MAX_MAC - 1) serial_escribir_caracter(':');
    }
}

static int red_mac_valida(const uint8_t mac[RED_MAX_MAC]) {
    if (mac[0] & 0x01) return 0;                    // multicast
    if (mac[0] == 0 && mac[1] == 0 && mac[2] == 0 &&
        mac[3] == 0 && mac[4] == 0 && mac[5] == 0) return 0;
    if (mac[0] == 0xFF && mac[1] == 0xFF && mac[2] == 0xFF &&
        mac[3] == 0xFF && mac[4] == 0xFF && mac[5] == 0xFF) return 0;
    return 1;
}

static uint16_t red_eeprom_leer(uint8_t direccion) {
    red_mmio_escribir32(E1000_EERD, ((uint32_t)direccion << 8) | 0x1u);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = red_mmio_leer32(E1000_EERD);
        if (v & 0x2u) return (uint16_t)(v >> 16);
        esperar_microsegundos(50);
    }
    return 0;
}

static void red_leer_mac(void) {
    uint32_t ral = red_mmio_leer32(E1000_RAL0);
    uint32_t rah = red_mmio_leer32(E1000_RAH0);
    uint8_t mac[RED_MAX_MAC];
    mac[0] = (uint8_t)(ral);
    mac[1] = (uint8_t)(ral >> 8);
    mac[2] = (uint8_t)(ral >> 16);
    mac[3] = (uint8_t)(ral >> 24);
    mac[4] = (uint8_t)(rah);
    mac[5] = (uint8_t)(rah >> 8);

    if (!red_mac_valida(mac) && g_info.familia != RED_FAMILIA_ICH) {
        uint16_t w0 = red_eeprom_leer(0);
        uint16_t w1 = red_eeprom_leer(1);
        uint16_t w2 = red_eeprom_leer(2);
        mac[0] = (uint8_t)(w0);
        mac[1] = (uint8_t)(w0 >> 8);
        mac[2] = (uint8_t)(w1);
        mac[3] = (uint8_t)(w1 >> 8);
        mac[4] = (uint8_t)(w2);
        mac[5] = (uint8_t)(w2 >> 8);
    }

    if (!red_mac_valida(mac)) {
        // MAC local administrada de respaldo: 02:54:41:45:4B:xx ("TAEK")
        mac[0] = 0x02;
        mac[1] = 0x54;
        mac[2] = 0x41;
        mac[3] = 0x45;
        mac[4] = 0x4B;
        mac[5] = (uint8_t)(rdtsc() & 0xFF);
    }

    memcpy(g_info.mac, mac, RED_MAX_MAC);
}

// ------------------------------------------------------------------- PHY/MDIC
static void red_mdic_escribir(uint8_t reg, uint16_t valor) {
    red_mmio_escribir32(E1000_MDIC,
        ((uint32_t)1 << E1000_MDIC_PHY_SHIFT) |
        ((uint32_t)reg << E1000_MDIC_REG_SHIFT) |
        E1000_MDIC_OP_WRITE | (uint32_t)valor);
    for (int i = 0; i < 1000; i++) {
        if (red_mmio_leer32(E1000_MDIC) & E1000_MDIC_READY) return;
        esperar_microsegundos(50);
    }
}

static uint16_t red_mdic_leer(uint8_t reg) {
    red_mmio_escribir32(E1000_MDIC,
        ((uint32_t)1 << E1000_MDIC_PHY_SHIFT) |
        ((uint32_t)reg << E1000_MDIC_REG_SHIFT) |
        E1000_MDIC_OP_READ);
    for (int i = 0; i < 1000; i++) {
        uint32_t v = red_mmio_leer32(E1000_MDIC);
        if (v & E1000_MDIC_READY) return (uint16_t)(v & 0xFFFF);
        if (v & E1000_MDIC_ERROR) return 0xFFFF;
        esperar_microsegundos(50);
    }
    return 0xFFFF;
}

static void red_reiniciar_autonegociacion(void) {
    if (g_info.familia == RED_FAMILIA_ICH) return; // PHY gestionado por firmware/ME
    // BMCR (reg 0): reiniciar autonegociación anunciando 10/100/1000.
    red_mdic_escribir(0x00, 0x1140);
    // BMSR (reg 1), bit 5 = autonegociación completada.
    for (int i = 0; i < 200; i++) {
        uint16_t bmsr = red_mdic_leer(0x01);
        if (bmsr == 0xFFFF || (bmsr & 0x0020)) break;
        esperar_milisegundos(1);
    }
}

// ------------------------------------------------------------- Enlace físico
int red_enlace_actualizar(void) {
    if (!g_info.controlador_listo) return 0;
    uint32_t status = red_mmio_leer32(E1000_STATUS);
    g_info.enlace_activo = (status & E1000_STATUS_LU) ? 1 : 0;
    g_info.full_duplex = (status & E1000_STATUS_FD) ? 1 : 0;
    uint32_t sp = status & E1000_STATUS_SPEED_MASK;
    if (sp == E1000_STATUS_SPEED_1000) g_info.velocidad_mbps = 1000;
    else if (sp == E1000_STATUS_SPEED_100) g_info.velocidad_mbps = 100;
    else g_info.velocidad_mbps = 10;
    return g_info.enlace_activo;
}

// ------------------------------------------------------------------- Mapeo
static int red_mapear_mmio(const struct dispositivo_pci *dev) {
    uint64_t fisica = dev->barras[0].dir_base;
    uint64_t tamano = dev->barras[0].tamano;
    if (fisica == 0) return -1;
    if (tamano == 0) tamano = 128 * 1024;
    if (tamano > 1024 * 1024) tamano = 1024 * 1024;

    g_info.mmio_fisica = fisica;
    g_info.mmio_tamano = tamano;
    g_info.mmio_virtual = RED_MMIO_VIRTUAL_BASE;

    uint64_t paginas = (tamano + TAMANO_PAGINA - 1) / TAMANO_PAGINA;
    for (uint64_t p = 0; p < paginas; p++) {
        paginacion_mapear(g_info.mmio_virtual + p * TAMANO_PAGINA,
                          fisica + p * TAMANO_PAGINA,
                          PAGINA_ATRIBUTOS_MMIO);
    }
    g_mmio = (volatile uint8_t *)g_info.mmio_virtual;
    return 0;
}

// ------------------------------------------------------- Anillos DMA RX/TX
static void red_liberar_recursos(void) {
    if (g_rx_ring) dma_liberar_bufer_contiguo(g_rx_ring, g_rx_ring_fisica, RED_RX_DESC_COUNT * sizeof(struct red_rx_desc));
    if (g_rx_buffers) dma_liberar_bufer_contiguo(g_rx_buffers, g_rx_buffers_fisica, RED_RX_DESC_COUNT * RED_RX_BUF_SIZE);
    if (g_tx_ring) dma_liberar_bufer_contiguo(g_tx_ring, g_tx_ring_fisica, RED_TX_DESC_COUNT * sizeof(struct red_tx_desc));
    if (g_tx_buffers) dma_liberar_bufer_contiguo(g_tx_buffers, g_tx_buffers_fisica, RED_TX_DESC_COUNT * RED_TX_BUF_SIZE);
    g_rx_ring = NULL; g_rx_buffers = NULL; g_tx_ring = NULL; g_tx_buffers = NULL;
}

static int red_configurar_rx(void) {
    red_mmio_escribir32(E1000_RDBAL, (uint32_t)(g_rx_ring_fisica & 0xFFFFFFFFu));
    red_mmio_escribir32(E1000_RDBAH, (uint32_t)(g_rx_ring_fisica >> 32));
    red_mmio_escribir32(E1000_RDLEN, (uint32_t)(RED_RX_DESC_COUNT * sizeof(struct red_rx_desc)));
    red_mmio_escribir32(E1000_RDH, 0);
    red_mmio_escribir32(E1000_RDT, RED_RX_DESC_COUNT - 1);
    // Forzar descriptores legacy y tamaño de búfer 2048, aceptar broadcast y
    // comprobar CRC. Sin promiscuo para no inundar la pila.
    red_mmio_escribir32(E1000_RFCTL, 0);
    red_mmio_escribir32(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC);
    return 0;
}

static int red_configurar_tx(void) {
    red_mmio_escribir32(E1000_TDBAL, (uint32_t)(g_tx_ring_fisica & 0xFFFFFFFFu));
    red_mmio_escribir32(E1000_TDBAH, (uint32_t)(g_tx_ring_fisica >> 32));
    red_mmio_escribir32(E1000_TDLEN, (uint32_t)(RED_TX_DESC_COUNT * sizeof(struct red_tx_desc)));
    red_mmio_escribir32(E1000_TDH, 0);
    red_mmio_escribir32(E1000_TDT, 0);
    // IPGT=8, IPGR1=8, IPGR2=6 (gigabit), CT=0x10 y COLD=0x40.
    red_mmio_escribir32(E1000_TIPG, 0x00602008u);
    red_mmio_escribir32(E1000_TCTL,
        E1000_TCTL_EN | E1000_TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    return 0;
}

static int red_asignar_anillos(void) {
    g_rx_ring = (struct red_rx_desc *)dma_asignar_bufer_contiguo(
        RED_RX_DESC_COUNT * sizeof(struct red_rx_desc), 4096, &g_rx_ring_fisica);
    g_rx_buffers = (uint8_t *)dma_asignar_bufer_contiguo(
        RED_RX_DESC_COUNT * RED_RX_BUF_SIZE, 4096, &g_rx_buffers_fisica);
    g_tx_ring = (struct red_tx_desc *)dma_asignar_bufer_contiguo(
        RED_TX_DESC_COUNT * sizeof(struct red_tx_desc), 4096, &g_tx_ring_fisica);
    g_tx_buffers = (uint8_t *)dma_asignar_bufer_contiguo(
        RED_TX_DESC_COUNT * RED_TX_BUF_SIZE, 4096, &g_tx_buffers_fisica);

    if (!g_rx_ring || !g_rx_buffers || !g_tx_ring || !g_tx_buffers) {
        g_info.ultimo_error = 1;
        red_liberar_recursos();
        return -1;
    }

    memset(g_rx_ring, 0, RED_RX_DESC_COUNT * sizeof(struct red_rx_desc));
    memset(g_tx_ring, 0, RED_TX_DESC_COUNT * sizeof(struct red_tx_desc));
    memset(g_rx_buffers, 0, RED_RX_DESC_COUNT * RED_RX_BUF_SIZE);
    memset(g_tx_buffers, 0, RED_TX_DESC_COUNT * RED_TX_BUF_SIZE);

    for (uint32_t i = 0; i < RED_RX_DESC_COUNT; i++) {
        g_rx_ring[i].buffer_addr = g_rx_buffers_fisica + (uint64_t)i * RED_RX_BUF_SIZE;
        g_rx_ring[i].status = 0;
    }
    // Los descriptores TX se marcan como libres con el bit DD: el hardware lo
    // pondrá a 1 al completar cada transmisión y nosotros lo limpiaremos.
    for (uint32_t i = 0; i < RED_TX_DESC_COUNT; i++) {
        g_tx_ring[i].status = E1000_TXD_STAT_DD;
    }
    g_rx_idx = 0;
    g_tx_idx = 0;

    dma_sincronizar_cpu_a_dispositivo(g_rx_ring, RED_RX_DESC_COUNT * sizeof(struct red_rx_desc));
    dma_sincronizar_cpu_a_dispositivo(g_tx_ring, RED_TX_DESC_COUNT * sizeof(struct red_tx_desc));
    return 0;
}

// ---------------------------------------------------------------- API: inicio
int red_iniciar(void) {
    if (g_info.controlador_listo) return 0;
    if (g_info.presente) return -1;

    int total = pci_obtener_conteo();
    const struct dispositivo_pci *nic = NULL;
    const struct red_modelo *modelo = NULL;

    for (int i = 0; i < total; i++) {
        const struct dispositivo_pci *d = pci_obtener_dispositivo(i);
        if (!d || d->id_proveedor != 0x8086) continue;
        if (d->clase != 0x02 || d->subclase != 0x00) continue;
        if (d->barras[0].dir_base == 0 || d->barras[0].es_io) continue;

        const struct red_modelo *m = red_buscar_modelo(d->id_dispositivo);
        if (!m) continue;
        if (!nic) { nic = d; modelo = m; }
        // Priorizar controladores PCH integrados (I219) si hubiera varios.
        if (m->familia == RED_FAMILIA_ICH) { nic = d; modelo = m; break; }
    }

    if (!nic) {
        serial_imprimir_linea("[RED] No se detectó controlador Ethernet Intel soportado.");
        return -1;
    }

    g_info.presente = 1;
    g_info.bus = nic->bus;
    g_info.ranura = nic->ranura;
    g_info.funcion = nic->funcion;
    g_info.id_proveedor = nic->id_proveedor;
    g_info.id_dispositivo = nic->id_dispositivo;
    g_info.familia = modelo->familia;
    g_info.modelo = modelo->nombre;

    serial_imprimir("[RED] Controlador: ");
    serial_imprimir_linea(modelo->nombre);
    serial_imprimir("[RED] PCI ");
    serial_imprimir_dec(nic->bus);
    serial_imprimir(":");
    serial_imprimir_dec(nic->ranura);
    serial_imprimir(".");
    serial_imprimir_dec(nic->funcion);
    serial_imprimir("  Dev: ");
    serial_imprimir_hex(nic->id_proveedor);
    serial_imprimir(":");
    serial_imprimir_hex(nic->id_dispositivo);
    serial_imprimir_linea("");

    pci_activar_bus_master(nic);

    if (red_mapear_mmio(nic) != 0) {
        serial_imprimir_linea("[RED] BAR0 no válido; abortando.");
        g_info.presente = 0;
        return -1;
    }

    // Silenciar interrupciones: toda la recepción es por sondeo.
    red_mmio_escribir32(E1000_IMC, 0xFFFFFFFFu);
    (void)red_mmio_leer32(E1000_ICR);

    red_leer_mac();

    serial_imprimir("[RED] MAC: ");
    red_serial_mac(g_info.mac);
    serial_imprimir_linea("");

    // Deshabilitar RX/TX antes de reprogramar los anillos.
    uint32_t rctl = red_mmio_leer32(E1000_RCTL);
    uint32_t tctl = red_mmio_leer32(E1000_TCTL);
    red_mmio_escribir32(E1000_RCTL, rctl & ~E1000_RCTL_EN);
    red_mmio_escribir32(E1000_TCTL, tctl & ~E1000_TCTL_EN);
    esperar_milisegundos(2);

    // Tabla de multidifusión a cero y dirección propia en RAR0.
    for (uint32_t i = 0; i < 128; i++) red_mmio_escribir32(E1000_MTA + i * 4, 0);
    uint32_t ral = (uint32_t)g_info.mac[0] |
                   ((uint32_t)g_info.mac[1] << 8) |
                   ((uint32_t)g_info.mac[2] << 16) |
                   ((uint32_t)g_info.mac[3] << 24);
    uint32_t rah = (uint32_t)g_info.mac[4] |
                   ((uint32_t)g_info.mac[5] << 8) |
                   E1000_RAH_AV;
    red_mmio_escribir32(E1000_RAL0, ral);
    red_mmio_escribir32(E1000_RAH0, rah);

    if (red_asignar_anillos() != 0) {
        serial_imprimir_linea("[RED] Sin memoria DMA contigua para los anillos.");
        g_info.presente = 0;
        return -1;
    }

    red_configurar_rx();
    red_configurar_tx();

    // Enlace: set link up + auto-speed detection. El I219 conserva el estado
    // que dejó el firmware/ME (no se fuerza reset global en la familia ICH).
    uint32_t ctrl = red_mmio_leer32(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU | E1000_CTRL_ASDE;
    if (g_info.familia != RED_FAMILIA_ICH) ctrl |= E1000_CTRL_FD;
    red_mmio_escribir32(E1000_CTRL, ctrl);

    if (g_info.familia != RED_FAMILIA_ICH) red_reiniciar_autonegociacion();

    // Esperar a que la autonegociación levante el enlace (hasta ~3 s).
    for (int i = 0; i < 600; i++) {
        if (red_enlace_actualizar()) break;
        esperar_milisegundos(5);
    }
    red_enlace_actualizar();

    g_info.controlador_listo = 1;
    g_info.ultimo_error = 0;
    serial_imprimir("[RED] Enlace: ");
    serial_imprimir_linea(g_info.enlace_activo ? "ACTIVO" : "SIN CABLE/CAÍDO");
    return 0;
}

const struct red_info *red_obtener_info(void) {
    return &g_info;
}

// -------------------------------------------------------------------- TX
int red_enviar_trama(const void *datos, uint32_t longitud) {
    if (!g_info.controlador_listo || !datos || longitud == 0 || longitud > RED_TX_BUF_SIZE) {
        return -1;
    }

    uint32_t idx = g_tx_idx;
    struct red_tx_desc *d = &g_tx_ring[idx];

    // Esperar a que el descriptor vuelva a ser nuestro.
    if (!(d->status & E1000_TXD_STAT_DD)) {
        int libre = 0;
        for (int i = 0; i < 200; i++) {
            dma_sincronizar_dispositivo_a_cpu(d, sizeof(*d));
            if (d->status & E1000_TXD_STAT_DD) { libre = 1; break; }
            esperar_milisegundos(1);
        }
        if (!libre) {
            g_info.tx_errores++;
            g_info.ultimo_error = 2;
            return -1;
        }
    }
    d->status = 0;

    memcpy(g_tx_buffers + (uint64_t)idx * RED_TX_BUF_SIZE, datos, longitud);
    dma_sincronizar_cpu_a_dispositivo(g_tx_buffers + (uint64_t)idx * RED_TX_BUF_SIZE, longitud);

    d->buffer_addr = g_tx_buffers_fisica + (uint64_t)idx * RED_TX_BUF_SIZE;
    d->length = (uint16_t)longitud;
    d->cso = 0;
    d->comando = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    d->status = 0;
    d->css = 0;
    d->special = 0;
    dma_sincronizar_cpu_a_dispositivo(d, sizeof(*d));

    g_tx_idx = (idx + 1) % RED_TX_DESC_COUNT;
    red_mmio_escribir32(E1000_TDT, g_tx_idx);

    g_info.tx_paquetes++;
    g_info.tx_bytes += longitud;
    return 0;
}

// -------------------------------------------------------------------- RX
int red_recibir_trama(void *buffer, uint32_t capacidad, uint32_t *longitud) {
    if (!g_info.controlador_listo || !buffer) return 0;

    uint32_t rdh = red_mmio_leer32(E1000_RDH);
    while (g_rx_idx != rdh) {
        struct red_rx_desc *d = &g_rx_ring[g_rx_idx];
        dma_sincronizar_dispositivo_a_cpu(d, sizeof(*d));

        uint8_t status = d->status;
        if (!(status & E1000_RXD_STAT_DD)) break;

        uint16_t len = d->length;
        uint8_t errores = d->errores;
        int bueno = (status & E1000_RXD_STAT_EOP) && errores == 0 &&
                    len > 0 && len <= RED_RX_BUF_SIZE;

        if (bueno) {
            uint32_t copia = len;
            if (copia > capacidad) copia = capacidad;
            memcpy(buffer, g_rx_buffers + (uint64_t)g_rx_idx * RED_RX_BUF_SIZE, copia);
            if (longitud) *longitud = copia;
            g_info.rx_paquetes++;
            g_info.rx_bytes += len;
        } else {
            g_info.rx_descartados++;
        }

        d->status = 0;
        d->errores = 0;
        dma_sincronizar_cpu_a_dispositivo(d, sizeof(*d));
        red_mmio_escribir32(E1000_RDT, g_rx_idx);

        g_rx_idx = (g_rx_idx + 1) % RED_RX_DESC_COUNT;

        if (bueno) return 1;
        rdh = red_mmio_leer32(E1000_RDH);
    }
    return 0;
}
