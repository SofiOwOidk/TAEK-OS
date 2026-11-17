/* Ver i2c_dw.h. Registros del nucleo DesignWare (databook DW_apb_i2c) y del bloque privado del LPSS de Intel,
 * los mismos que usa Linux en drivers/i2c/busses/i2c-designware-*.c y drivers/mfd/intel-lpss.c. */
#include "i2c_dw.h"
#include "consola.h"
#include "../arquitectura/x86_64/pci.h"
#include "../base/paginacion.h"
#include "../base/tiempo.h"

#define I2C_VIRTUAL UINT64_C(0xFFFFFE0007000000)

/* Nucleo DesignWare */
#define IC_CON            0x00
#define IC_TAR            0x04
#define IC_DATA_CMD       0x10
#define IC_SS_SCL_HCNT    0x14
#define IC_SS_SCL_LCNT    0x18
#define IC_FS_SCL_HCNT    0x1C
#define IC_FS_SCL_LCNT    0x20
#define IC_INTR_MASK      0x30
#define IC_RAW_INTR_STAT  0x34
#define IC_RX_TL          0x38
#define IC_TX_TL          0x3C
#define IC_CLR_INTR       0x40
#define IC_CLR_TX_ABRT    0x54
#define IC_CLR_STOP_DET   0x60
#define IC_ENABLE         0x6C
#define IC_STATUS         0x70
#define IC_TXFLR          0x74
#define IC_RXFLR          0x78
#define IC_SDA_HOLD       0x7C
#define IC_TX_ABRT_SOURCE 0x80
#define IC_ENABLE_STATUS  0x9C
#define IC_COMP_PARAM_1   0xF4
#define IC_COMP_VERSION   0xF8
#define IC_COMP_TYPE      0xFC
#define DW_TIPO           UINT32_C(0x44570140)

#define CMD_LEER    (1u << 8)
#define CMD_PARAR   (1u << 9)
#define CMD_REINICIO (1u << 10)
#define INTR_TX_ABRT   (1u << 6)
#define INTR_STOP_DET  (1u << 9)

/* Bloque privado del LPSS, en el mismo BAR */
#define LPSS_RESETS   0x204   /* Bits 0-1: funcion fuera de reset; bit 2: su DMA */
#define LPSS_CAPS     0x2FC

/* Reloj de entrada de los I2C de Sunrise Point: 120 MHz (Linux: spt_i2c_info) */
#define RELOJ_KHZ 120000

static volatile uint8_t *g_r;
static unsigned g_tx, g_rx;
static int g_listo;
static uint32_t g_aborto;

static uint32_t rd(unsigned o) { return *(volatile uint32_t *)(g_r + o); }
static void wr(unsigned o, uint32_t v) { *(volatile uint32_t *)(g_r + o) = v; }

int i2c_dw_listo(void) { return g_listo; }
uint32_t i2c_dw_aborto(void) { return g_aborto; }

static const struct dispositivo_pci *buscar(unsigned indice) {
    for (int i = 0; i < pci_obtener_conteo(); i++) {
        const struct dispositivo_pci *d = pci_obtener_dispositivo(i);
        if (d && d->id_proveedor == 0x8086 && d->id_dispositivo == 0x9D60 + indice) return d;
    }
    return 0;
}

/* Capacidad de gestion de energia PCI (ID 1): su registro de control lleva el estado D0..D3 en los bits 0-1 */
static uint8_t cap_energia(const struct dispositivo_pci *d) {
    if (!(pci_leer_config_16(d->bus, d->ranura, d->funcion, 0x06) & 0x10)) return 0;
    uint8_t p = pci_leer_config_8(d->bus, d->ranura, d->funcion, 0x34) & 0xFC;
    for (unsigned n = 0; p && n < 48; n++) {
        if (pci_leer_config_8(d->bus, d->ranura, d->funcion, p) == 1) return p;
        p = pci_leer_config_8(d->bus, d->ranura, d->funcion, (uint8_t)(p + 1)) & 0xFC;
    }
    return 0;
}

void i2c_dw_sondear(void) {
    if (!pci_esta_iniciado()) pci_iniciar();
    int vistos = 0;
    for (unsigned i = 0; i < 6; i++) {
        const struct dispositivo_pci *d = buscar(i);
        if (!d) continue;
        vistos++;
        uint8_t pm = cap_energia(d);
        uint16_t ctrl = pm ? pci_leer_config_16(d->bus, d->ranura, d->funcion, (uint8_t)(pm + 4)) : 0;
        consola_imprimir("  [I2C] bus "); consola_imprimir_dec(i);
        consola_imprimir(" PCI 8086:"); consola_imprimir_hex(d->id_dispositivo);
        consola_imprimir(" en "); consola_imprimir_dec(d->bus); consola_imprimir(":"); consola_imprimir_dec(d->ranura);
        consola_imprimir("."); consola_imprimir_dec(d->funcion);
        consola_imprimir(" BAR0="); consola_imprimir_hex(d->barras[0].dir_base);
        consola_imprimir(" energia=D"); consola_imprimir_dec(pm ? (ctrl & 3) : 0);
        consola_imprimir(pm ? "" : " (sin capacidad PM)");
        consola_imprimir(" comando="); consola_imprimir_hex(pci_leer_config_16(d->bus, d->ranura, d->funcion, 0x04));
        consola_imprimir_linea("");
    }
    if (!vistos) consola_imprimir_linea("  [I2C] Ningun controlador I2C del LPSS visible por PCI.");
    if (g_listo) {
        consola_imprimir("  [I2C] Activo: tipo="); consola_imprimir_hex(rd(IC_COMP_TYPE));
        consola_imprimir(" version="); consola_imprimir_hex(rd(IC_COMP_VERSION));
        consola_imprimir(" colas TX/RX="); consola_imprimir_dec(g_tx); consola_imprimir("/"); consola_imprimir_dec(g_rx);
        consola_imprimir(" estado="); consola_imprimir_hex(rd(IC_STATUS));
        consola_imprimir_linea("");
    }
}

static int apagar(void) {
    wr(IC_ENABLE, 0);
    uint64_t t = tiempo_obtener_milisegundos();
    while (rd(IC_ENABLE_STATUS) & 1) {
        if (tiempo_obtener_milisegundos() - t > 25) return -1;
        esperar_microsegundos(25);
    }
    return 0;
}

/* Linux, i2c_dw_scl_hcnt/lcnt con offset 0: cuentas del reloj de entrada para cada mitad de la senal SCL */
static uint32_t cuenta_alta(uint32_t t_ns, uint32_t caida_ns) { return (RELOJ_KHZ * (t_ns + caida_ns) + 500000) / 1000000 - 8; }
static uint32_t cuenta_baja(uint32_t t_ns, uint32_t caida_ns) { return (RELOJ_KHZ * (t_ns + caida_ns) + 500000) / 1000000 - 1; }

int i2c_dw_iniciar(unsigned indice, unsigned khz) {
    g_listo = 0;
    if (!pci_esta_iniciado()) pci_iniciar();
    const struct dispositivo_pci *d = buscar(indice);
    if (!d) return -1;
    uint64_t bar = d->barras[0].dir_base;
    if (!bar || d->barras[0].es_io) return -2;

    /* D0 si el firmware lo dejo dormido; la especificacion pide 10 ms al salir de D3 */
    uint8_t pm = cap_energia(d);
    if (pm) {
        uint16_t c = pci_leer_config_16(d->bus, d->ranura, d->funcion, (uint8_t)(pm + 4));
        if (c & 3) {
            pci_escribir_config_16(d->bus, d->ranura, d->funcion, (uint8_t)(pm + 4), (uint16_t)(c & ~3u));
            esperar_milisegundos(10);
        }
    }
    uint16_t cmd = pci_leer_config_16(d->bus, d->ranura, d->funcion, 0x04);
    pci_escribir_config_16(d->bus, d->ranura, d->funcion, 0x04, (uint16_t)(cmd | 0x2));   /* Memoria; sin DMA: no se usa */

    /* Una pagina por bus, cada una en su sitio de la ventana (H138: con una sola, el segundo bus no arrancaba) */
    static uint64_t mapeado[6];
    uint64_t virt = I2C_VIRTUAL + (uint64_t)indice * 4096;
    if (mapeado[indice] != (bar & ~UINT64_C(4095))) {
        if (mapeado[indice]) return -3;
        if (paginacion_mapear(virt, bar & ~UINT64_C(4095), PAGINA_ATRIBUTOS_MMIO)) return -3;
        mapeado[indice] = bar & ~UINT64_C(4095);
    }
    g_r = (volatile uint8_t *)(virt + (bar & 4095));

    wr(LPSS_RESETS, 0x7);                        /* Funcion y su DMA fuera de reset (Linux: intel_lpss_deassert_reset) */
    esperar_microsegundos(100);
    if (rd(IC_COMP_TYPE) != DW_TIPO) return -4;

    uint32_t p = rd(IC_COMP_PARAM_1);
    g_tx = ((p >> 16) & 0xFF) + 1;
    g_rx = ((p >> 8) & 0xFF) + 1;
    if (g_tx < 2 || g_rx < 2) { g_tx = g_rx = 8; }

    if (apagar()) return -5;
    /* Amo, reinicio permitido, esclavo apagado; velocidad 1 = estandar, 2 = rapida */
    wr(IC_CON, 1u | (khz >= 400 ? 2u : 1u) << 1 | 1u << 5 | 1u << 6);
    wr(IC_SS_SCL_HCNT, cuenta_alta(4000, 300));
    wr(IC_SS_SCL_LCNT, cuenta_baja(4700, 300));
    wr(IC_FS_SCL_HCNT, cuenta_alta(600, 300));
    wr(IC_FS_SCL_LCNT, cuenta_baja(1300, 300));
    wr(IC_SDA_HOLD, RELOJ_KHZ * 300 / 1000000);   /* 300 ns, el valor por omision de Linux para estos buses */
    wr(IC_INTR_MASK, 0);                          /* Sondeo: ninguna interrupcion */
    wr(IC_RX_TL, 0);
    wr(IC_TX_TL, 0);
    (void)rd(IC_CLR_INTR);
    g_listo = 1;
    return 0;
}

int i2c_dw_transferir(uint8_t dir, const uint8_t *w, unsigned nw, uint8_t *r, unsigned nr, unsigned limite_ms) {
    if (!g_listo || (!nw && !nr) || dir > 0x7F) return -1;
    if (apagar()) return -2;
    wr(IC_TAR, dir);
    (void)rd(IC_CLR_INTR);
    wr(IC_ENABLE, 1);

    unsigned escritos = 0, pedidos = 0, leidos = 0;
    uint64_t t0 = tiempo_obtener_milisegundos();
    for (;;) {
        /* Empujar ordenes mientras quepan, sin pedir mas lecturas de las que caben en la cola de vuelta */
        while ((escritos < nw || pedidos < nr) && rd(IC_TXFLR) < g_tx && pedidos - leidos < g_rx) {
            uint32_t c;
            int ultima;
            if (escritos < nw) {
                c = w[escritos++];
                ultima = escritos == nw && !nr;
            } else {
                c = CMD_LEER;
                if (!pedidos && nw) c |= CMD_REINICIO;
                pedidos++;
                ultima = pedidos == nr;
            }
            if (ultima) c |= CMD_PARAR;
            wr(IC_DATA_CMD, c);
        }
        while (leidos < nr && rd(IC_RXFLR)) r[leidos++] = (uint8_t)rd(IC_DATA_CMD);

        uint32_t raw = rd(IC_RAW_INTR_STAT);
        if (raw & INTR_TX_ABRT) {
            g_aborto = rd(IC_TX_ABRT_SOURCE);
            (void)rd(IC_CLR_TX_ABRT);
            apagar();
            return -3;
        }
        if (escritos == nw && pedidos == nr && leidos == nr && (raw & INTR_STOP_DET)) break;
        if (tiempo_obtener_milisegundos() - t0 > limite_ms) { apagar(); return -2; }
        esperar_microsegundos(20);
    }
    (void)rd(IC_CLR_STOP_DET);
    apagar();
    return 0;
}
