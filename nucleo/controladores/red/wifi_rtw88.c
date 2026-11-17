/* Driver WiFi propio: Realtek RTL8822CE (10ec:c822), corte D, 2T2R.
 *
 * Codigo nativo de TAEK que sigue paso a paso a rtw88 (Linux 6.6.78):
 *   H153  deteccion        rtw_pci_io_mapping(), rtw_chip_parameter_setup()
 *   H154  MAC + efuse      rtw_mac_power_on(), rtw_dump_{physical,logical}_efuse_map()
 *   H155  firmware         rtw_pci_setup(), __rtw_download_firmware(), rtw_dump_hw_feature()
 * Registros y secuencias: rtw8822c_registros.h / rtw8822c_secuencias.h, GENERADOS
 * desde la fuente por herramientas/intel/extraer_tablas_rtw88.py (nunca a mano). */
#include "wifi_rtw88.h"
#include "rtw8822c_registros.h"
#include "rtw8822c_secuencias.h"
#include "../consola.h"
#include "../multimedia/reproductor/reproductor.h"
#include "../../arquitectura/x86_64/pci.h"
#include "../../arquitectura/x86_64/serial.h"
#include "../../base/paginacion.h"
#include "../../base/memoria.h"
#include "../../base/dma.h"
#include "../../base/tiempo.h"
#include "../../base/sha256.h"
#include "../vfs.h"
#include "../particiones.h"
#include "red.h"
#include "stream.h"
#include "red_cerrojo.h"   /* H211: cerrojo de la red (Fase C) */

#define RTW_VENDOR_REALTEK   0x10EC
#define RTW_DEV_8822CE       0xC822
#define BIT_GET_VENDOR_ID(x) (((x) >> 16) & 0xF)
#define BIT_GET_CHIP_VER(x)  (((x) >> 12) & 0xF)

/* Parametros de rtw8822c_hw_spec: generados en rtw8822c_registros.h (RTW8822C_*). */
#define RTW_RPWM_PCIE        0x03D9  /* hci.rpwm_addr para PCIe (main.c, asignacion en codigo) */
#define EFUSE_FISICO         RTW8822C_PHY_EFUSE_SIZE
#define EFUSE_LOGICO         RTW8822C_LOG_EFUSE_SIZE
#define EFUSE_PROTEGIDO      RTW8822C_PTCT_EFUSE_SIZE
#define TX_PKT_DESC          RTW8822C_TX_PKT_DESC_SZ
#define TX_BUF_DESC          RTW8822C_TX_BUF_DESC_SZ
#define RTW_PWR_POLLING_CNT  20000   /* main.h */
#define HW_FEATURE_LEN       13      /* main.h */
#define DLFW_BLOQUE          0x1000  /* download_firmware_to_mem(): max_size */

static struct wifi_info g_wifi;
static volatile uint8_t *g_wifi_mmio = 0;
static void wifi_potencia_tx(uint8_t canal);   /* H161 */
static void wifi_puerto_sta(void);             /* H161 */
static const struct dispositivo_pci *g_wifi_pci = 0;

/* ------------------------------------------------------------------ MMIO */
static inline uint8_t  wifi_leer8(uint32_t r)  { return *(volatile uint8_t  *)(g_wifi_mmio + r); }
static inline uint16_t wifi_leer16(uint32_t r) { return *(volatile uint16_t *)(g_wifi_mmio + r); }
static inline uint32_t wifi_leer32(uint32_t r) { return *(volatile uint32_t *)(g_wifi_mmio + r); }
static inline void wifi_escribir8(uint32_t r, uint8_t v)   { *(volatile uint8_t  *)(g_wifi_mmio + r) = v; }
static inline void wifi_escribir16(uint32_t r, uint16_t v) { *(volatile uint16_t *)(g_wifi_mmio + r) = v; }
static inline void wifi_escribir32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(g_wifi_mmio + r) = v; }
static inline void wifi_poner8(uint32_t r, uint8_t b)  { wifi_escribir8(r, (uint8_t)(wifi_leer8(r) | b)); }
static inline void wifi_quitar8(uint32_t r, uint8_t b) { wifi_escribir8(r, (uint8_t)(wifi_leer8(r) & ~b)); }
static inline void wifi_poner32(uint32_t r, uint32_t b)  { wifi_escribir32(r, wifi_leer32(r) | b); }
static inline void wifi_quitar32(uint32_t r, uint32_t b) { wifi_escribir32(r, wifi_leer32(r) & ~b); }

/* le32_encode_bits(): coloca v en el campo descrito por la mascara. */
static inline uint32_t campo(uint32_t mascara, uint32_t v) { return (v << __builtin_ctz(mascara)) & mascara; }

/* check_hw_ready() de util.c: 1000 x 10 us sobre rtw_read32_mask(). */
static int wifi_hw_listo(uint32_t reg, uint32_t mascara, uint32_t objetivo) {
    for (int i = 0; i < 1000; i++) {
        if (((wifi_leer32(reg) & mascara) >> __builtin_ctz(mascara)) == objetivo) return 1;
        esperar_microsegundos(10);
    }
    return 0;
}

const struct wifi_info *wifi_obtener_info(void) { RED_EXCLUSIVA(); return &g_wifi; }

/* ================================================================== H153 */
static const struct barra_pci *wifi_elegir_bar(const struct dispositivo_pci *d, int *indice) {
    /* rtw88 usa BAR2 fijo; si el enumerador no lo marco valido se acepta la
     * primera barra MMIO y se registra cual fue. */
    if (d->barras[2].valida && !d->barras[2].es_io && d->barras[2].dir_base) { *indice = 2; return &d->barras[2]; }
    for (int i = 0; i < 6; i++) {
        if (d->barras[i].valida && !d->barras[i].es_io && d->barras[i].dir_base) { *indice = i; return &d->barras[i]; }
    }
    return 0;
}

int wifi_iniciar(void) { RED_EXCLUSIVA();
    memset(&g_wifi, 0, sizeof(g_wifi));
    const struct dispositivo_pci *dev = 0;
    for (int i = 0; i < pci_obtener_conteo(); i++) {
        const struct dispositivo_pci *d = pci_obtener_dispositivo(i);
        if (d && d->id_proveedor == RTW_VENDOR_REALTEK && d->id_dispositivo == RTW_DEV_8822CE) { dev = d; break; }
    }
    if (!dev) {
        serial_imprimir_linea("[WIFI] RTL8822CE (10ec:c822) no presente en el bus PCI.");
        return -1;
    }
    g_wifi_pci = dev;
    g_wifi.presente = 1;
    g_wifi.bus = dev->bus; g_wifi.ranura = dev->ranura; g_wifi.funcion = dev->funcion;
    g_wifi.id_proveedor = dev->id_proveedor; g_wifi.id_dispositivo = dev->id_dispositivo;
    g_wifi.sub_proveedor = dev->sub_proveedor; g_wifi.sub_dispositivo = dev->sub_dispositivo;
    g_wifi.revision_pci = dev->revision;

    int indice = -1;
    const struct barra_pci *bar = wifi_elegir_bar(dev, &indice);
    if (!bar) {
        serial_imprimir_linea("[WIFI] RTL8822CE sin BAR MMIO valida.");
        return -1;
    }
    g_wifi.bar2_fisica = bar->dir_base;
    g_wifi.bar2_tamano = bar->tamano ? bar->tamano : WIFI_MMIO_MAX;
    uint64_t tam = g_wifi.bar2_tamano > WIFI_MMIO_MAX ? WIFI_MMIO_MAX : g_wifi.bar2_tamano;

    /* Solo decodificacion de memoria (bit 1). El bus master se activa solo
     * mientras haya anillos DMA programados (H155, wifi_cargar_firmware). */
    uint16_t cmd = pci_leer_config_16(dev->bus, dev->ranura, dev->funcion, 0x04);
    if (!(cmd & 0x0002)) pci_escribir_config_16(dev->bus, dev->ranura, dev->funcion, 0x04, (uint16_t)(cmd | 0x0002));

    for (uint64_t p = 0; p < (tam + TAMANO_PAGINA - 1) / TAMANO_PAGINA; p++) {
        paginacion_mapear(WIFI_MMIO_VIRTUAL_BASE + p * TAMANO_PAGINA,
                          g_wifi.bar2_fisica + p * TAMANO_PAGINA, PAGINA_ATRIBUTOS_MMIO);
    }
    g_wifi_mmio = (volatile uint8_t *)WIFI_MMIO_VIRTUAL_BASE;
    g_wifi.mmio_listo = 1;

    g_wifi.sys_cfg1 = wifi_leer32(REG_SYS_CFG1);
    g_wifi.sys_cfg2 = wifi_leer32(REG_SYS_CFG2);
    g_wifi.corte = (uint8_t)BIT_GET_CHIP_VER(g_wifi.sys_cfg1);
    g_wifi.proveedor_chip = (uint8_t)BIT_GET_VENDOR_ID(g_wifi.sys_cfg1);
    g_wifi.cadenas_rf = (g_wifi.sys_cfg1 & BIT_RF_TYPE_ID) ? 2 : 1;
    g_wifi.chip_mp = (g_wifi.sys_cfg1 & BIT_RTL_ID) ? 0 : 1;

    serial_imprimir("[WIFI] RTL8822CE PCI ");
    serial_imprimir_dec(dev->bus); serial_imprimir(":");
    serial_imprimir_dec(dev->ranura); serial_imprimir(".");
    serial_imprimir_dec(dev->funcion);
    serial_imprimir(" BAR"); serial_imprimir_dec((uint64_t)indice);
    serial_imprimir("="); serial_imprimir_hex(g_wifi.bar2_fisica);
    serial_imprimir(" SYS_CFG1="); serial_imprimir_hex(g_wifi.sys_cfg1);
    serial_imprimir_linea("");

    /* 0xFFFFFFFF = el chip no contesta (D3cold o BAR sin decodificar). */
    return g_wifi.sys_cfg1 == 0xFFFFFFFFu ? -2 : 0;
}

/* ================================================================== H154
 * Encendido de la MAC: pre_system_cfg -> power_switch(on) -> init_system_cfg. */

/* rtw_mac_pre_system_cfg(), rama PCIe de un chip WCPU_11AC. */
static void wifi_pre_system_cfg(void) {
    wifi_escribir8(REG_RSV_CTRL, 0);
    wifi_poner32(REG_HCI_OPT_CTRL, BIT_USB_SUS_DIS);
    wifi_poner32(REG_PAD_CTRL1, BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL);
    wifi_quitar32(REG_LED_CFG, BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN);
    wifi_poner32(REG_GPIO_MUXCFG, BIT_WLRFE_4_5_EN);
    wifi_quitar8(REG_SYS_FUNC_EN, (uint8_t)(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST));
    wifi_quitar8(REG_RF_CTRL, (uint8_t)(BIT_RF_SDM_RSTB | BIT_RF_RSTB | BIT_RF_EN));
    wifi_quitar32(REG_WLRF1, BIT_WLRF1_BBRF_EN);
}

static int wifi_espera_pwr(uint16_t reg, uint8_t mascara, uint8_t objetivo) {
    objetivo &= mascara;
    for (int i = 0; i < RTW_PWR_POLLING_CNT; i++) {
        if ((wifi_leer8(reg) & mascara) == objetivo) return 0;
        esperar_microsegundos(50);
    }
    return -1;
}

/* rtw_pwr_cmd_polling(): en PCIe, si falla, se alterna BIT_PFM_WOWL y se reintenta. */
static int wifi_pwr_polling(const struct rtw_secuencia_pwr *c) {
    if (wifi_espera_pwr(c->desplazamiento, c->mascara, c->valor) == 0) return 0;
    uint8_t v = wifi_leer8(REG_SYS_PW_CTRL);
    wifi_escribir8(REG_SYS_PW_CTRL, (uint8_t)(v | BIT_PFM_WOWL));
    wifi_escribir8(REG_SYS_PW_CTRL, (uint8_t)(v & ~BIT_PFM_WOWL));
    if (wifi_espera_pwr(c->desplazamiento, c->mascara, c->valor) == 0) return 0;
    g_wifi.error_registro = c->desplazamiento;
    return -1;
}

/* rtw_pwr_seq_parser() + rtw_sub_pwr_seq_parser() para interfaz PCIe. */
static int wifi_ejecutar_flujo(const struct rtw_secuencia_pwr *const *flujo) {
    uint8_t corte = (uint8_t)(1u << (g_wifi.corte + 1));      /* cut_version_to_mask */
    for (; *flujo; flujo++) {
        for (const struct rtw_secuencia_pwr *c = *flujo; c->orden != RTW_PWR_CMD_END; c++) {
            if (!(c->mascara_interfaz & RTW_PWR_INTF_PCI_MSK) || !(c->mascara_corte & corte)) continue;
            if (c->base == RTW_PWR_ADDR_SDIO) continue;
            switch (c->orden) {
            case RTW_PWR_CMD_WRITE: {
                uint8_t v = wifi_leer8(c->desplazamiento);
                v = (uint8_t)((v & ~c->mascara) | (c->valor & c->mascara));
                wifi_escribir8(c->desplazamiento, v);
                break;
            }
            case RTW_PWR_CMD_POLLING:
                if (wifi_pwr_polling(c)) return -1;
                break;
            case RTW_PWR_CMD_DELAY:
                if (c->valor == RTW_PWR_DELAY_US) esperar_microsegundos(c->desplazamiento);
                else esperar_milisegundos(c->desplazamiento);
                break;
            default:
                break;
            }
        }
    }
    return 0;
}

/* rtw_mac_power_switch(). Devuelve 1 si ya estaba en el estado pedido (-EALREADY). */
static int wifi_power_switch(int encender) {
    uint8_t rpwm = wifi_leer8(RTW_RPWM_PCIE);
    if (wifi_leer16(REG_MCUFW_CTRL) == FW_READY) {               /* firmware aun vivo */
        rpwm = (uint8_t)((rpwm ^ BIT_RPWM_TOGGLE) & BIT_RPWM_TOGGLE);
        wifi_escribir8(RTW_RPWM_PCIE, rpwm);
    }
    int encendido = wifi_leer8(REG_CR) != 0xEA;
    if (encender == encendido) return 1;
    return wifi_ejecutar_flujo(encender ? card_enable_flow_8822c : card_disable_flow_8822c) ? -1 : 0;
}

/* __rtw_mac_init_system_cfg(). */
static void wifi_init_system_cfg(void) {
    wifi_poner32(REG_CPU_DMEM_CON, BIT_WL_PLATFORM_RST | BIT_DDMA_EN);
    wifi_poner8(REG_SYS_FUNC_EN + 1, RTW8822C_SYS_FUNC_EN);
    wifi_escribir8(REG_CR_EXT + 3, (uint8_t)((wifi_leer8(REG_CR_EXT + 3) & 0xF0) | 0x0C));
    uint32_t fw = wifi_leer32(REG_MCUFW_CTRL);
    if (fw & BIT_BOOT_FSPI_EN) {
        wifi_escribir32(REG_MCUFW_CTRL, fw & ~BIT_BOOT_FSPI_EN);
        wifi_quitar32(REG_GPIO_MUXCFG, BIT_FSPI_EN);
    }
}

static int wifi_mac_power_on(void) {
    wifi_pre_system_cfg();
    int r = wifi_power_switch(1);
    if (r == 1) {                       /* ya encendida: apagar y repetir, como rtw88 */
        wifi_power_switch(0);
        wifi_pre_system_cfg();
        r = wifi_power_switch(1);
    }
    if (r) return -1;
    wifi_init_system_cfg();
    return 0;
}

/* ------------------------------------------------------------------ efuse */
static uint8_t g_efuse_fisico[EFUSE_FISICO];
static uint8_t g_efuse_logico[EFUSE_LOGICO];

/* rtw_dump_physical_efuse_map(): banco WiFi, LDO 2.5 V apagado (solo lectura). */
static int wifi_volcar_efuse_fisico(void) {
    wifi_escribir32(REG_LDO_EFUSE_CTRL, wifi_leer32(REG_LDO_EFUSE_CTRL) & ~BIT_MASK_EFUSE_BANK_SEL); /* banco 0 */
    wifi_quitar8(REG_ANAPARLDO_POW_MAC, (uint8_t)BIT_LDOE25_PON);
    uint32_t ctl = wifi_leer32(REG_EFUSE_CTRL);
    for (uint32_t dir = 0; dir < EFUSE_FISICO; dir++) {
        ctl &= ~(BIT_MASK_EF_DATA | BITS_EF_ADDR);
        ctl |= (dir & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR;
        wifi_escribir32(REG_EFUSE_CTRL, ctl & ~BIT_EF_FLAG);
        uint32_t n = 0;
        do {
            esperar_microsegundos(1);
            ctl = wifi_leer32(REG_EFUSE_CTRL);
            if (++n > 100000) { g_wifi.error_registro = (uint16_t)dir; return -1; }
        } while (!(ctl & BIT_EF_FLAG));
        g_efuse_fisico[dir] = (uint8_t)(ctl & BIT_MASK_EF_DATA);
    }
    return 0;
}

/* rtw_dump_logical_efuse_map(): cabeceras de 1 o 2 bytes, 4 palabras por bloque. */
static int wifi_efuse_logico(void) {
    memset(g_efuse_logico, 0xFF, sizeof(g_efuse_logico));
    uint32_t f = 0, limite = EFUSE_FISICO - EFUSE_PROTEGIDO;
    while (f < limite) {
        uint8_t h1 = g_efuse_fisico[f], h2 = g_efuse_fisico[f + 1];
        if (h1 == 0xFF || ((h1 & 0x1F) == 0x0F && h2 == 0xFF)) break;
        uint8_t bloque, palabras;
        if ((h1 & 0x1F) == 0x0F) { bloque = (uint8_t)(((h2 & 0xF0) >> 1) | ((h1 >> 5) & 0x07)); palabras = h2 & 0xF; f += 2; }
        else { bloque = (uint8_t)((h1 & 0xF0) >> 4); palabras = h1 & 0xF; f += 1; }
        for (int i = 0; i < 4; i++) {
            if (palabras & (1u << i)) continue;                  /* 1 = palabra no escrita */
            uint32_t l = ((uint32_t)bloque << 3) + ((uint32_t)i << 1);
            if (f + 1 > limite || l + 1 > EFUSE_LOGICO) return -1;
            g_efuse_logico[l] = g_efuse_fisico[f];
            g_efuse_logico[l + 1] = g_efuse_fisico[f + 1];
            f += 2;
        }
    }
    g_wifi.efuse_fisico_usado = (uint16_t)f;
    return 0;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* rtw_parse_efuse_map() + rtw8822c_read_efuse(), con la MAC ya encendida. */
static int wifi_efuse_con_mac_encendida(void) {
    g_wifi.efuse_leido = 0;
    if (wifi_volcar_efuse_fisico()) { g_wifi.error_encendido = WIFI_ERR_EFUSE; return -1; }
    if (wifi_efuse_logico()) { g_wifi.error_encendido = WIFI_ERR_EFUSE_LOGICO; return -1; }
    const uint8_t *m = g_efuse_logico;  /* struct rtw8822c_efuse */
    g_wifi.rtl_id = le16(m + 0x00);
    g_wifi.canal_plan = m[0xB8];
    g_wifi.cristal = (uint8_t)(m[0xB9] & XCAP_MASK);
    g_wifi.placa = m[0xC1];
    g_wifi.bt = m[0xC3];
    g_wifi.rfe = m[0xCA];
    g_wifi.pais[0] = (char)m[0xCB]; g_wifi.pais[1] = (char)m[0xCC]; g_wifi.pais[2] = 0;
    g_wifi.termico_a = m[0xD0];
    g_wifi.termico_b = m[0xD1];
    memcpy(g_wifi.mac, m + 0x120, 6);   /* rtw8822ce_efuse.mac_addr */
    g_wifi.efuse_vid = le16(m + 0x126);
    g_wifi.efuse_did = le16(m + 0x128);
    g_wifi.efuse_svid = le16(m + 0x12A);
    g_wifi.efuse_sdid = le16(m + 0x12C);
    g_wifi.efuse_leido = 1;
    return 0;
}

int wifi_leer_efuse(void) { RED_EXCLUSIVA();
    if (g_wifi.radio_activa) wifi_apagar();
    g_wifi.efuse_leido = 0;
    g_wifi.error_encendido = 0;
    g_wifi.error_registro = 0;
    if (!g_wifi.mmio_listo || g_wifi.sys_cfg1 == 0xFFFFFFFFu) { g_wifi.error_encendido = WIFI_ERR_SIN_MMIO; return -1; }
    g_wifi.cr_antes = wifi_leer8(REG_CR);
    if (wifi_mac_power_on()) { g_wifi.error_encendido = WIFI_ERR_ESPERA_PWR; return -1; }
    int r = wifi_efuse_con_mac_encendida();
    wifi_power_switch(0);               /* rtw_chip_efuse_disable(): sin DMA no hay hci_stop */
    return r;
}

/* ================================================================== H155
 * Firmware: pagina reservada por la cola de beacon PCI + DDMA interno. */

struct wifi_bd_tx {                     /* struct rtw_pci_tx_buffer_desc */
    uint16_t buf_size;
    uint16_t psb_len;
    uint32_t dma;
} __attribute__((packed));

static uint8_t *g_anillo_bcn;   static uint64_t g_anillo_bcn_fis;  /* 1 entrada = TX_BUF_DESC */
static uint8_t *g_paquete;      static uint64_t g_paquete_fis;     /* TX_PKT_DESC + DLFW_BLOQUE */
#define WIFI_PAQUETE_BYTES 8192u

/* H158: anillo RX MPDU (rtw_pci_init_rx_ring). rtw88 usa RTK_MAX_RX_DESC_NUM = 512
 * buffers de RTK_PCI_RX_BUF_SIZE; aqui 32 (384 KiB) para no gastar 5,8 MB de DMA.
 * Debe existir ANTES de MAC_TRX_ENABLE: con HCI_RXDMA_EN y la radio cargada el
 * chip escribe en RXBD_DESA lo que reciba. */
#define WIFI_RX_N       256u                 /* H182: 256 (H176: 128; rtw88 usa 512) */
static uint32_t g_rx_n = WIFI_RX_N;          /* entradas reales (128 si no hubo 3 MiB de DMA) */
#define WIFI_RX_PASO    12288u               /* >= RTK_PCI_RX_BUF_SIZE (11478), alineado a pagina */
struct wifi_bd_rx {                          /* struct rtw_pci_rx_buffer_desc */
    uint16_t buf_size;
    uint16_t total_pkt_size;
    uint32_t dma;
} __attribute__((packed));
static uint8_t *g_anillo_rx;    static uint64_t g_anillo_rx_fis;
static uint8_t *g_rx_bufs;      static uint64_t g_rx_bufs_fis;
static uint32_t g_rx_rp, g_rx_tag;   /* r.rp/r.wp y rtwpci->rx_tag: a 0 en cada rtw_pci_setup() */

/* H161: anillo TX de la cola de gestion (RTW_TX_QUEUE_MGMT). rtw88 usa
 * RTK_DEFAULT_TX_DESC_NUM = 128; aqui 16, con un buffer de 4 KiB por entrada. */
#define WIFI_TXM_N      64u                  /* H177: ACK de TCP en rafaga (antes 16) */
#define WIFI_TXM_PASO   4096u
static uint8_t *g_anillo_mgmt;  static uint64_t g_anillo_mgmt_fis;
static uint8_t *g_tx_bufs;      static uint64_t g_tx_bufs_fis;
static uint32_t g_txm_wp;

static int wifi_reservar_dma(void) {
    if (!g_anillo_bcn) g_anillo_bcn = dma_asignar_bufer_contiguo(4096, 4096, &g_anillo_bcn_fis);
    if (!g_paquete) g_paquete = dma_asignar_bufer_contiguo(WIFI_PAQUETE_BYTES, 4096, &g_paquete_fis);
    if (!g_anillo_rx) g_anillo_rx = dma_asignar_bufer_contiguo(4096, 4096, &g_anillo_rx_fis);
    if (!g_rx_bufs) {                                     /* H182: 256 entradas (3 MiB); si no hay, 128 como H176 */
        g_rx_n = WIFI_RX_N;
        g_rx_bufs = dma_asignar_bufer_contiguo(WIFI_RX_N * WIFI_RX_PASO, 4096, &g_rx_bufs_fis);
        if (!g_rx_bufs || g_rx_bufs_fis + WIFI_RX_N * WIFI_RX_PASO > 0x100000000ULL) {
            g_rx_n = 128;
            g_rx_bufs = dma_asignar_bufer_contiguo(128 * WIFI_RX_PASO, 4096, &g_rx_bufs_fis);
        }
    }
    if (!g_anillo_mgmt) g_anillo_mgmt = dma_asignar_bufer_contiguo(4096, 4096, &g_anillo_mgmt_fis);
    if (!g_tx_bufs) g_tx_bufs = dma_asignar_bufer_contiguo(WIFI_TXM_N * WIFI_TXM_PASO, 4096, &g_tx_bufs_fis);
    if (!g_anillo_bcn || !g_paquete || !g_anillo_rx || !g_rx_bufs || !g_anillo_mgmt || !g_tx_bufs) return -1;
    if (g_anillo_mgmt_fis + 4096 > 0x100000000ULL || g_tx_bufs_fis + WIFI_TXM_N * WIFI_TXM_PASO > 0x100000000ULL) return -1;
    /* Descriptores y DESA de rtw88 llevan direcciones de 32 bits. */
    if (g_anillo_bcn_fis + 4096 > 0x100000000ULL || g_paquete_fis + WIFI_PAQUETE_BYTES > 0x100000000ULL ||
        g_anillo_rx_fis + 4096 > 0x100000000ULL || g_rx_bufs_fis + g_rx_n * WIFI_RX_PASO > 0x100000000ULL)
        return -1;
    return 0;
}

/* rtw_pci_reset_rx_desc() para todo el anillo. */
static void wifi_rx_rellenar(void) {
    struct wifi_bd_rx *bd = (struct wifi_bd_rx *)g_anillo_rx;
    memset(g_anillo_rx, 0, g_rx_n * RTW8822C_RX_BUF_DESC_SZ);
    for (uint32_t i = 0; i < g_rx_n; i++) {
        bd[i].buf_size = (uint16_t)RTK_PCI_RX_BUF_SIZE;
        bd[i].total_pkt_size = 0;
        bd[i].dma = (uint32_t)(g_rx_bufs_fis + i * WIFI_RX_PASO);
    }
    dma_sincronizar_cpu_a_dispositivo(g_anillo_rx, g_rx_n * RTW8822C_RX_BUF_DESC_SZ);
}

/* H157: pci_enable_device()/pci_set_master() de Linux habilitan tambien la
 * cadena de puentes (pci_enable_bridge). Un puente tipo 1 con Bus Master
 * Enable en 0 responde Unsupported Request a las lecturas de memoria que suben
 * desde su bus secundario: el chip ve Received Master Abort (estado bit 13) y
 * el descriptor nunca se lee. Se busca cada puente cuyo rango [secundario,
 * subordinado] contiene el bus de la tarjeta y se le activa MEM + BM. */
static void wifi_habilitar_puentes(void) {
    const struct dispositivo_pci *w = g_wifi_pci;
    uint8_t objetivo = w->bus;
    for (int vueltas = 0; vueltas < 8 && objetivo != 0; vueltas++) {
        int hallado = 0;
        for (int i = 0; i < pci_obtener_conteo(); i++) {
            const struct dispositivo_pci *p = pci_obtener_dispositivo(i);
            if (!p || (p->tipo_cabecera & 0x7F) != 1) continue;
            uint8_t sec = pci_leer_config_8(p->bus, p->ranura, p->funcion, 0x19);
            uint8_t sub = pci_leer_config_8(p->bus, p->ranura, p->funcion, 0x1A);
            if (objetivo < sec || objetivo > sub) continue;
            uint16_t cmd = pci_leer_config_16(p->bus, p->ranura, p->funcion, 0x04);
            uint16_t nuevo = (uint16_t)(cmd | 0x0006);
            if (nuevo != cmd) pci_escribir_config_16(p->bus, p->ranura, p->funcion, 0x04, nuevo);
            if (!g_wifi.puente_hallado) {   /* el inmediato: el primero que se encuentra para w->bus */
                g_wifi.puente_hallado = 1;
                g_wifi.puente_bus = p->bus; g_wifi.puente_ranura = p->ranura; g_wifi.puente_funcion = p->funcion;
                g_wifi.puente_cmd_antes = cmd;
                g_wifi.puente_cmd_despues = pci_leer_config_16(p->bus, p->ranura, p->funcion, 0x04);
            }
            objetivo = p->bus;              /* seguir hacia arriba */
            hallado = 1;
            break;
        }
        if (!hallado) break;
    }
}

static void wifi_bus_master(int activar) {
    const struct dispositivo_pci *d = g_wifi_pci;
    if (activar) {
        wifi_habilitar_puentes();
        /* Limpiar abortos previos (RW1C) para que el diagnostico sea de este intento. */
        pci_escribir_config_16(d->bus, d->ranura, d->funcion, 0x06, 0x3000);
    }
    uint16_t cmd = pci_leer_config_16(d->bus, d->ranura, d->funcion, 0x04);
    cmd = activar ? (uint16_t)(cmd | 0x0006) : (uint16_t)(cmd & ~0x0004);
    pci_escribir_config_16(d->bus, d->ranura, d->funcion, 0x04, cmd);
}

/* rtw_pci_setup() = rtw_pci_reset_buf_desc() + rtw_pci_dma_reset(). Solo se
 * crea el anillo de beacon; RTK_PCI_CTRL+3 |= 0xF7 (igual que rtw88) deja las
 * demas colas detenidas, asi que sus DESA no se usan. */
static void wifi_pci_setup(void) {
    g_wifi.diag_desa_hi_previo = wifi_leer32(RTK_PCI_TXBD_DESA_BCNQ + 4);
    wifi_escribir32(RTK_PCI_HIMR0, 0);
    wifi_escribir32(RTK_PCI_HIMR1, 0);
    wifi_escribir32(RTK_PCI_HIMR3, 0);
    wifi_escribir8(RTK_PCI_CTRL + 3, (uint8_t)(wifi_leer8(RTK_PCI_CTRL + 3) | 0xF7));
    wifi_escribir32(RTK_PCI_TXBD_DESA_BCNQ, (uint32_t)g_anillo_bcn_fis);
    /* H161: cola de gestion (r.rp = r.wp = 0, NUM y DESA como rtw_pci_reset_buf_desc). */
    memset(g_anillo_mgmt, 0, WIFI_TXM_N * TX_BUF_DESC);
    dma_sincronizar_cpu_a_dispositivo(g_anillo_mgmt, WIFI_TXM_N * TX_BUF_DESC);
    g_txm_wp = 0;
    wifi_escribir16(RTK_PCI_TXBD_NUM_MGMTQ, (uint16_t)(WIFI_TXM_N & TRX_BD_IDX_MASK));
    wifi_escribir32(RTK_PCI_TXBD_DESA_MGMTQ, (uint32_t)g_anillo_mgmt_fis);
    /* H158: anillo RX MPDU, igual que el final de rtw_pci_reset_buf_desc(). */
    wifi_rx_rellenar();
    g_rx_rp = 0;
    g_rx_tag = 0;                                    /* rtw_pci_dma_reset(): rx_tag = 0 */
    wifi_escribir16(RTK_PCI_RXBD_NUM_MPDUQ, (uint16_t)(g_rx_n & TRX_BD_IDX_MASK));
    wifi_escribir32(RTK_PCI_RXBD_DESA_MPDUQ, (uint32_t)g_anillo_rx_fis);
    wifi_escribir32(RTK_PCI_TXBD_RWPTR_CLR, 0xFFFFFFFFu);
    wifi_poner32(RTK_PCI_TXBD_H2CQ_CSR, BIT_CLR_H2CQ_HOST_IDX | BIT_CLR_H2CQ_HW_IDX);
    wifi_poner32(RTK_PCI_CTRL, BIT_RST_TRXDMA_INTF | BIT_RX_TAG_EN);
}

/* rtw_fw_write_data_rsvd_page() + rtw_pci_write_data_rsvd_page() +
 * rtw_tx_rsvd_page_pkt_info_update(RSVD_BEACON) + rtw_tx_fill_tx_desc(). */
static int wifi_escribir_pagina_reservada(uint16_t pagina, const uint8_t *datos, uint32_t n) {
    wifi_escribir16(REG_FIFOPAGE_CTRL_2, (uint16_t)((pagina & BIT_MASK_BCN_HEAD_1_V1) | BIT_BCN_VALID_V1));
    uint8_t cr1 = wifi_leer8(REG_CR + 1);
    wifi_escribir8(REG_CR + 1, (uint8_t)(cr1 | (BIT_ENSWBCN >> 8)));
    uint8_t txq = wifi_leer8(REG_FWHW_TXQ_CTRL + 2);
    wifi_escribir8(REG_FWHW_TXQ_CTRL + 2, (uint8_t)(txq & ~(BIT_EN_BCNQ_DL >> 16)));

    uint32_t total = TX_PKT_DESC + n;
    uint32_t *w = (uint32_t *)g_paquete;
    memset(g_paquete, 0, TX_PKT_DESC);
    memcpy(g_paquete + TX_PKT_DESC, datos, n);
    /* rtw88 deduce BMC de hdr->addr1 (bytes 4..9 de los datos), aunque sea firmware. */
    uint32_t bmc = (n >= 10 && (datos[4] & 1)) ? 1 : 0;
    w[0] = campo(RTW_TX_DESC_W0_TXPKTSIZE, n) | campo(RTW_TX_DESC_W0_OFFSET, TX_PKT_DESC) |
           campo(RTW_TX_DESC_W0_BMC, bmc) | RTW_TX_DESC_W0_LS | RTW_TX_DESC_W0_DISQSELSEQ;
    w[1] = campo(RTW_TX_DESC_W1_QSEL, TX_DESC_QSEL_BEACON) | campo(RTW_TX_DESC_W1_RATE_ID, RTW_RATEID_B_20M);
    w[3] = campo(RTW_TX_DESC_W3_HW_SSN_SEL, 0) | RTW_TX_DESC_W3_USE_RATE | RTW_TX_DESC_W3_DISDATAFB;
    w[4] = campo(RTW_TX_DESC_W4_DATARATE, DESC_RATE1M);
    w[8] = RTW_TX_DESC_W8_EN_HWSEQ;
    dma_sincronizar_cpu_a_dispositivo(g_paquete, total);

    struct wifi_bd_tx *bd = (struct wifi_bd_tx *)g_anillo_bcn;
    memset(bd, 0, TX_BUF_DESC);
    bd[0].psb_len = (uint16_t)(((total - 1) / 128 + 1) | (1u << RTK_PCI_TXBD_OWN_OFFSET));
    bd[0].buf_size = TX_PKT_DESC;
    bd[0].dma = (uint32_t)g_paquete_fis;
    bd[1].buf_size = (uint16_t)n;
    bd[1].dma = (uint32_t)(g_paquete_fis + TX_PKT_DESC);
    dma_sincronizar_cpu_a_dispositivo(bd, TX_BUF_DESC);

    wifi_poner8(RTK_PCI_TXBD_BCN_WORK, (uint8_t)BIT_PCI_BCNQ_FLAG);
    int ok = wifi_hw_listo(REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1, 1);
    if (!ok && !g_wifi.diag_valido) {
        /* H156: foto antes del restore, para ver en que punto se corto el DMA. */
        const struct dispositivo_pci *d = g_wifi_pci;
        dma_sincronizar_dispositivo_a_cpu(bd, TX_BUF_DESC);
        g_wifi.diag_valido = 1;
        g_wifi.diag_pci_cmd = pci_leer_config_16(d->bus, d->ranura, d->funcion, 0x04);
        g_wifi.diag_pci_estado = pci_leer_config_16(d->bus, d->ranura, d->funcion, 0x06);
        g_wifi.diag_pci_ctrl = wifi_leer32(RTK_PCI_CTRL);
        g_wifi.diag_desa_lo = wifi_leer32(RTK_PCI_TXBD_DESA_BCNQ);
        g_wifi.diag_desa_hi = wifi_leer32(RTK_PCI_TXBD_DESA_BCNQ + 4);
        g_wifi.diag_bcn_work = wifi_leer8(RTK_PCI_TXBD_BCN_WORK);
        g_wifi.diag_fifopage2 = wifi_leer16(REG_FIFOPAGE_CTRL_2);
        g_wifi.diag_cr = wifi_leer8(REG_CR);
        g_wifi.diag_txdma_estado = wifi_leer32(REG_TXDMA_STATUS);
        g_wifi.diag_hisr0 = wifi_leer32(0x0B4);   /* RTK_PCI_HISR0 */
        g_wifi.diag_hisr1 = wifi_leer32(0x0BC);   /* RTK_PCI_HISR1 */
        g_wifi.diag_hisr3 = wifi_leer32(0x10BC);  /* RTK_PCI_HISR3 */
        g_wifi.diag_mcufw = wifi_leer16(REG_MCUFW_CTRL);
        g_wifi.diag_bd_psb = ((volatile struct wifi_bd_tx *)bd)->psb_len;
        g_wifi.diag_anillo_fis = g_anillo_bcn_fis;
        g_wifi.diag_paquete_fis = g_paquete_fis;
        g_wifi.diag_bloque = n;
    }

    /* restore: fifo.rsvd_boundary (0 hasta que priority_queue_cfg() lo fije). */
    wifi_escribir16(REG_FIFOPAGE_CTRL_2, (uint16_t)(g_wifi.rsvd_limite | BIT_BCN_VALID_V1));
    wifi_escribir8(REG_FWHW_TXQ_CTRL + 2, txq);
    wifi_escribir8(REG_CR + 1, cr1);
    return ok ? 0 : -1;
}

/* iddma_enable() + iddma_download_firmware(). */
static int wifi_iddma(uint32_t origen, uint32_t destino, uint32_t largo, int primero) {
    if (!wifi_hw_listo(REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0)) return -1;
    uint32_t ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN | (largo & BIT_MASK_DDMACH0_DLEN);
    if (!primero) ctrl |= BIT_DDMACH0_CHKSUM_CONT;
    wifi_escribir32(REG_DDMA_CH0SA, origen);
    wifi_escribir32(REG_DDMA_CH0DA, destino);
    wifi_escribir32(REG_DDMA_CH0CTRL, ctrl);
    return wifi_hw_listo(REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0) ? 0 : -1;
}

/* check_fw_checksum(). */
static int wifi_checksum_fw(uint32_t destino) {
    uint8_t ctl = wifi_leer8(REG_MCUFW_CTRL);
    int imem = destino < OCPBASE_DMEM_88XX;
    if (wifi_leer32(REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) {
        if (imem) { ctl |= BIT_IMEM_DW_OK; ctl &= (uint8_t)~BIT_IMEM_CHKSUM_OK; }
        else      { ctl |= BIT_DMEM_DW_OK; ctl &= (uint8_t)~BIT_DMEM_CHKSUM_OK; }
        wifi_escribir8(REG_MCUFW_CTRL, ctl);
        return -1;
    }
    ctl |= imem ? (BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK) : (BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
    wifi_escribir8(REG_MCUFW_CTRL, ctl);
    return 0;
}

/* download_firmware_to_mem() con src = 0 (pagina reservada 0). */
static int wifi_fw_a_memoria(const uint8_t *datos, uint32_t destino, uint32_t largo) {
    wifi_poner32(REG_DDMA_CH0CTRL, BIT_DDMACH0_RESET_CHKSUM_STS);
    uint32_t desplazamiento = 0;
    int primero = 1;
    while (largo) {
        uint32_t n = largo >= DLFW_BLOQUE ? DLFW_BLOQUE : largo;
        if (wifi_escribir_pagina_reservada(0, datos + desplazamiento, n)) { g_wifi.fw_error = WIFI_FW_PAGINA; return -1; }
        if (wifi_iddma(OCPBASE_TXBUF_88XX + 0 + TX_PKT_DESC, destino + desplazamiento, n, primero)) {
            g_wifi.fw_error = WIFI_FW_DDMA; return -1;
        }
        primero = 0;
        desplazamiento += n;
        largo -= n;
    }
    if (wifi_checksum_fw(destino)) { g_wifi.fw_error = WIFI_FW_CHECKSUM; return -1; }
    return 0;
}

/* ltecoex_read_reg() / ltecoex_reg_write() (util.c). */
static int wifi_ltecoex_leer(uint16_t off, uint32_t *v) {
    if (!wifi_hw_listo(LTECOEX_ACCESS_CTRL, LTECOEX_READY, 1)) return -1;
    wifi_escribir32(LTECOEX_ACCESS_CTRL, 0x800F0000u | off);
    *v = wifi_leer32(LTECOEX_READ_DATA);
    return 0;
}
static int wifi_ltecoex_escribir(uint16_t off, uint32_t v) {
    if (!wifi_hw_listo(LTECOEX_ACCESS_CTRL, LTECOEX_READY, 1)) return -1;
    wifi_escribir32(LTECOEX_WRITE_DATA, v);
    wifi_escribir32(LTECOEX_ACCESS_CTRL, 0xC00F0000u | off);
    return 0;
}

/* wlan_cpu_enable(). */
static void wifi_cpu(int activar) {
    if (activar) {
        wifi_poner8(REG_RSV_CTRL + 1, (uint8_t)BIT_WLMCU_IOIF);
        wifi_poner8(REG_SYS_FUNC_EN + 1, (uint8_t)BIT_FEN_CPUEN);
    } else {
        wifi_quitar8(REG_SYS_FUNC_EN + 1, (uint8_t)BIT_FEN_CPUEN);
        wifi_quitar8(REG_RSV_CTRL + 1, (uint8_t)BIT_WLMCU_IOIF);
    }
}

/* __rtw_download_firmware() para WCPU_11AC. */
static int wifi_descargar_firmware(const uint8_t *fw, uint64_t bytes) {
    /* check_firmware_size() con struct rtw_fw_hdr (fw.h). */
    if (bytes < FW_HDR_SIZE) { g_wifi.fw_error = WIFI_FW_TAMANO; return -1; }
    uint32_t dmem = le32(fw + 0x24), imem = le32(fw + 0x30);
    uint32_t emem = (fw[0x18] & (1u << 4)) ? le32(fw + 0x34) : 0;
    g_wifi.fw_dmem = dmem; g_wifi.fw_imem = imem; g_wifi.fw_emem = emem;
    dmem += FW_HDR_CHKSUM_SIZE;
    imem += FW_HDR_CHKSUM_SIZE;
    emem += emem ? FW_HDR_CHKSUM_SIZE : 0;
    if ((uint64_t)FW_HDR_SIZE + dmem + imem + emem != bytes) { g_wifi.fw_error = WIFI_FW_TAMANO; return -1; }

    uint32_t ltecoex;
    if (wifi_ltecoex_leer(0x38, &ltecoex)) { g_wifi.fw_error = WIFI_FW_LTECOEX; return -1; }
    wifi_cpu(0);

    /* download_firmware_reg_backup(): 6 registros, mismo orden que rtw88. */
    uint8_t  b_pq = wifi_leer8(REG_TXDMA_PQ_MAP + 1);
    wifi_escribir8(REG_TXDMA_PQ_MAP + 1, (uint8_t)(RTW_DMA_MAPPING_HIGH << 6));
    uint8_t  b_cr = wifi_leer8(REG_CR);
    wifi_escribir8(REG_CR, (uint8_t)(BIT_HCI_TXDMA_EN | BIT_TXDMA_EN));
    wifi_escribir32(REG_H2CQ_CSR, BIT_H2CQ_FULL);
    uint16_t b_fifo = wifi_leer16(REG_FIFOPAGE_INFO_1);
    uint32_t b_rqpn = wifi_leer32(REG_RQPN_CTRL_2) | BIT_LD_RQPN;
    wifi_escribir16(REG_FIFOPAGE_INFO_1, 0x200);
    wifi_escribir32(REG_RQPN_CTRL_2, b_rqpn);
    uint8_t  b_bcn = wifi_leer8(REG_BCN_CTRL);
    wifi_escribir8(REG_BCN_CTRL, (uint8_t)((b_bcn & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT));

    /* download_firmware_reset_platform(). */
    wifi_quitar8(REG_CPU_DMEM_CON + 2, (uint8_t)(BIT_WL_PLATFORM_RST >> 16));
    wifi_quitar8(REG_SYS_CLK_CTRL + 1, (uint8_t)(BIT_CPU_CLK_EN >> 8));
    wifi_poner8(REG_CPU_DMEM_CON + 2, (uint8_t)(BIT_WL_PLATFORM_RST >> 16));
    wifi_poner8(REG_SYS_CLK_CTRL + 1, (uint8_t)(BIT_CPU_CLK_EN >> 8));

    /* start_download_firmware(): DMEM, IMEM y EMEM opcional. */
    uint16_t v = (uint16_t)((wifi_leer16(REG_MCUFW_CTRL) & 0x3800) | BIT_MCUFWDL_EN);
    wifi_escribir16(REG_MCUFW_CTRL, v);
    int r = wifi_fw_a_memoria(fw + FW_HDR_SIZE, le32(fw + 0x20) & ~(1u << 31), dmem);
    if (!r) r = wifi_fw_a_memoria(fw + FW_HDR_SIZE + dmem, le32(fw + 0x3C) & ~(1u << 31), imem);
    if (!r && emem) r = wifi_fw_a_memoria(fw + FW_HDR_SIZE + dmem + imem, le32(fw + 0x38) & ~(1u << 31), emem);
    if (r) goto falla;

    /* download_firmware_reg_restore() (rtw_restore_reg en el orden del backup). */
    wifi_escribir8(REG_TXDMA_PQ_MAP + 1, b_pq);
    wifi_escribir8(REG_CR, b_cr);
    wifi_escribir32(REG_H2CQ_CSR, BIT_H2CQ_FULL);
    wifi_escribir16(REG_FIFOPAGE_INFO_1, b_fifo);
    wifi_escribir32(REG_RQPN_CTRL_2, b_rqpn);
    wifi_escribir8(REG_BCN_CTRL, b_bcn);

    /* download_firmware_end_flow(). */
    wifi_escribir32(REG_TXDMA_STATUS, BTI_PAGE_OVF);
    uint16_t ctl = wifi_leer16(REG_MCUFW_CTRL);
    if ((ctl & BIT_CHECK_SUM_OK) == BIT_CHECK_SUM_OK)
        wifi_escribir16(REG_MCUFW_CTRL, (uint16_t)((ctl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN));

    wifi_cpu(1);
    if (wifi_ltecoex_escribir(0x38, ltecoex)) { g_wifi.fw_error = WIFI_FW_LTECOEX; goto falla; }

    /* download_firmware_validate(). */
    if (!wifi_hw_listo(REG_MCUFW_CTRL, FW_READY_MASK, FW_READY)) {
        g_wifi.fw_dbg7 = wifi_leer32(REG_FW_DBG7);
        g_wifi.fw_error = (g_wifi.fw_dbg7 & FW_KEY_MASK) == ILLEGAL_KEY_GROUP ? WIFI_FW_LLAVE : WIFI_FW_VALIDACION;
        goto falla;
    }
    g_wifi.fw_mcufw_ctrl = wifi_leer16(REG_MCUFW_CTRL);
    wifi_pci_setup();                    /* "reset desc and index": rtw_hci_setup() */
    return 0;

falla:
    g_wifi.fw_mcufw_ctrl = wifi_leer16(REG_MCUFW_CTRL);
    wifi_quitar8(REG_MCUFW_CTRL, (uint8_t)BIT_MCUFWDL_EN);
    wifi_poner8(REG_SYS_FUNC_EN + 1, (uint8_t)BIT_FEN_CPUEN);
    return -1;
}

/* rtw_dump_hw_feature(): el firmware responde en REG_C2HEVT al C2H_HW_FEATURE_DUMP. */
static int wifi_hw_feature(void) {
    g_wifi.hw_feature_id = wifi_leer8(REG_C2HEVT);
    if (g_wifi.hw_feature_id != C2H_HW_FEATURE_REPORT) return -1;
    for (int i = 0; i < HW_FEATURE_LEN; i++) g_wifi.hw_feature[i] = wifi_leer8(REG_C2HEVT + 2 + i);
    wifi_escribir8(REG_C2HEVT, 0);
    return 0;
}

static int wifi_cargar_firmware_datos(const uint8_t *fw, uint64_t bytes);
static int g_con_radio;             /* H158: seguir con rtw_mac_init + tablas antes de apagar */
static int wifi_fase3(void);

int wifi_iniciar_radio(const char *ruta_usb) { RED_EXCLUSIVA();
    g_con_radio = 1;
    int r = wifi_cargar_firmware(ruta_usb);
    g_con_radio = 0;
    return r;
}

int wifi_cargar_firmware(const char *ruta_usb) { RED_EXCLUSIVA();
    if (g_wifi.radio_activa) wifi_apagar();
    g_wifi.fw_cargado = 0;
    g_wifi.fw_error = 0;
    g_wifi.fw_error_vfs = 0;
    g_wifi.fw_bytes = 0;
    g_wifi.fw_desde_usb = ruta_usb != 0;
    memset(g_wifi.fw_sha256, 0, sizeof(g_wifi.fw_sha256));
    if (!ruta_usb) {
        uint64_t bytes = 0;
        const uint8_t *fw = taek_modulo_arranque("rtw88:fw", &bytes);
        if (!fw) { g_wifi.fw_error = WIFI_FW_SIN_MODULO; return -1; }
        return wifi_cargar_firmware_datos(fw, bytes);
    }
    void *buf = 0; size_t n = 0; int es_dma = 0;
    int r = vfs_leer_archivo_binario(ruta_usb, &buf, &n, &es_dma);
    if (r || !buf) { g_wifi.fw_error_vfs = r; g_wifi.fw_error = WIFI_FW_SIN_MODULO; return -1; }
    r = wifi_cargar_firmware_datos(buf, n);
    vfs_liberar_archivo_binario(buf, n, es_dma);
    return r;
}

static int wifi_cargar_firmware_datos(const uint8_t *fw, uint64_t bytes) {
    g_wifi.diag_valido = 0;
    g_wifi.fw_dbg7 = 0;
    g_wifi.fw_mcufw_ctrl = 0;
    g_wifi.hw_feature_id = 0;
    g_wifi.error_encendido = 0;
    g_wifi.error_registro = 0;
    g_wifi.fw_bytes = bytes;
    g_wifi.rsvd_limite = 0;             /* fifo aun sin configurar */
    g_wifi.fase3_hecha = 0;
    g_wifi.fase3_error = 0;
    g_wifi.fase3b_hecha = 0;
    struct sha256 h;
    sha256_iniciar(&h);
    sha256_actualizar(&h, fw, (size_t)bytes);
    sha256_final(&h, g_wifi.fw_sha256);
    if (!g_wifi.mmio_listo || g_wifi.sys_cfg1 == 0xFFFFFFFFu) { g_wifi.error_encendido = WIFI_ERR_SIN_MMIO; return -1; }
    if (bytes >= FW_HDR_SIZE) {
        g_wifi.fw_firma = le16(fw + 0x00);
        g_wifi.fw_version = le16(fw + 0x04);
        g_wifi.fw_subversion = fw[0x06];
        g_wifi.fw_subindice = fw[0x07];
        g_wifi.fw_mes = fw[0x10]; g_wifi.fw_dia = fw[0x11];
        g_wifi.fw_hora = fw[0x12]; g_wifi.fw_minuto = fw[0x13];
        g_wifi.fw_anio = le16(fw + 0x14);
        g_wifi.fw_h2c_ver = le16(fw + 0x1C);
    }
    if (wifi_reservar_dma()) { g_wifi.fw_error = WIFI_FW_SIN_DMA; return -1; }

    /* rtw_chip_efuse_enable(): hci_setup -> mac_power_on -> C2H_HW_FEATURE_DUMP -> descarga. */
    g_wifi.cr_antes = wifi_leer8(REG_CR);
    wifi_bus_master(1);
    wifi_pci_setup();
    int r = -1;
    if (wifi_mac_power_on()) { g_wifi.error_encendido = WIFI_ERR_ESPERA_PWR; g_wifi.fw_error = WIFI_FW_ENCENDIDO; goto apagar; }
    wifi_escribir8(REG_C2HEVT, C2H_HW_FEATURE_DUMP);
    if (wifi_descargar_firmware(fw, bytes)) goto apagar;
    g_wifi.fw_cargado = 1;
    /* rtw_chip_efuse_info_setup(): efuse y hw feature con el firmware corriendo. */
    wifi_efuse_con_mac_encendida();
    if (wifi_hw_feature()) { g_wifi.fw_error = WIFI_FW_HW_FEATURE; goto apagar; }
    r = 0;
    if (g_con_radio) r = wifi_fase3();
    /* RXBD_IDX: hw (27:16) cuantas tramas escribio el chip en el anillo RX. */
    g_wifi.rx_idx = wifi_leer32(RTK_PCI_RXBD_IDX_MPDUQ);
    if (r == 0 && g_con_radio == 2) {   /* H160: wifi_encender() deja la radio viva */
        g_wifi.radio_activa = 1;
        wifi_puerto_sta();              /* H161: rtw_ops_add_interface(STATION) */
        return 0;
    }

apagar:
    /* rtw_chip_efuse_disable(): hci_stop + power_off. Sin anillos activos no
     * debe quedar bus master. */
    wifi_escribir32(RTK_PCI_HIMR0, 0);
    wifi_escribir32(RTK_PCI_HIMR1, 0);
    wifi_escribir32(RTK_PCI_HIMR3, 0);
    wifi_power_switch(0);
    wifi_bus_master(0);
    return r;
}

/* ================================================================== H158
 * rtw_mac_init() + primera mitad de rtw8822c_phy_set_param(). */
#include "rtw8822c_tablas.h"

static inline void wifi_escribir32_mascara(uint32_t r, uint32_t mascara, uint32_t v) {   /* rtw_write32_mask */
    uint32_t orig = wifi_leer32(r);
    wifi_escribir32(r, (orig & ~mascara) | ((v << __builtin_ctz(mascara)) & mascara));
}

/* txdma_queue_mapping() con rqpn_table_8822c[1] (PCIe). */
static void wifi_txdma_queue_mapping(void) {
    wifi_escribir16(REG_TXDMA_PQ_MAP, (uint16_t)TXDMA_PQ_MAP_PCIE);
    wifi_escribir8(REG_CR, 0);
    wifi_escribir8(REG_CR, (uint8_t)MAC_TRX_ENABLE);
    wifi_escribir32(REG_H2CQ_CSR, BIT_H2CQ_FULL);
    g_wifi.cr_final = wifi_leer32(REG_CR);
}

/* set_trx_fifo_info() + __priority_queue_cfg() con page_table_8822c[1]. */
static uint16_t g_rsvd_h2cq_addr;
static int wifi_priority_queue_cfg(void) {
    uint32_t txff_pg = RTW8822C_TXFF_SIZE >> 7;
    uint32_t rsvd_pg = RTW8822C_RSVD_DRV_PG_NUM + RSVD_PG_H2C_EXTRAINFO_NUM + RSVD_PG_H2C_STATICINFO_NUM +
                       RSVD_PG_H2CQ_NUM + RSVD_PG_CPU_INSTRUCTION_NUM + RSVD_PG_FW_TXBUF_NUM + RTW8822C_CSI_BUF_PG_NUM;
    if (rsvd_pg > txff_pg) { g_wifi.fase3_error = WIFI_F3_FIFO; return -1; }
    uint32_t acq_pg = txff_pg - rsvd_pg;
    uint32_t limite = txff_pg - rsvd_pg;
    uint32_t cur = txff_pg - RTW8822C_CSI_BUF_PG_NUM;        /* rsvd_csibuf_addr */
    cur -= RSVD_PG_FW_TXBUF_NUM;                              /* rsvd_fw_txbuf_addr */
    cur -= RSVD_PG_CPU_INSTRUCTION_NUM;                       /* rsvd_cpu_instr_addr */
    cur -= RSVD_PG_H2CQ_NUM;  g_rsvd_h2cq_addr = (uint16_t)cur;
    cur -= RSVD_PG_H2C_STATICINFO_NUM;
    cur -= RSVD_PG_H2C_EXTRAINFO_NUM;
    cur -= RTW8822C_RSVD_DRV_PG_NUM;                          /* rsvd_drv_addr */
    if (cur != limite) { g_wifi.fase3_error = WIFI_F3_FIFO; return -1; }
    g_wifi.rsvd_limite = (uint16_t)limite;

    uint32_t pubq = acq_pg - RTW8822C_PG_PCIE_HQ - RTW8822C_PG_PCIE_LQ - RTW8822C_PG_PCIE_NQ -
                    RTW8822C_PG_PCIE_EXQ - RTW8822C_PG_PCIE_GAPQ;
    g_wifi.pubq = (uint16_t)pubq;
    wifi_escribir16(REG_FIFOPAGE_INFO_1, RTW8822C_PG_PCIE_HQ);
    wifi_escribir16(REG_FIFOPAGE_INFO_2, RTW8822C_PG_PCIE_LQ);
    wifi_escribir16(REG_FIFOPAGE_INFO_3, RTW8822C_PG_PCIE_NQ);
    wifi_escribir16(REG_FIFOPAGE_INFO_4, RTW8822C_PG_PCIE_EXQ);
    wifi_escribir16(REG_FIFOPAGE_INFO_5, (uint16_t)pubq);
    wifi_poner32(REG_RQPN_CTRL_2, BIT_LD_RQPN);
    wifi_escribir16(REG_FIFOPAGE_CTRL_2, (uint16_t)limite);
    wifi_poner8(REG_FWHW_TXQ_CTRL + 2, (uint8_t)(BIT_EN_WR_FREE_TAIL >> 16));
    wifi_escribir16(REG_BCNQ_BDNY_V1, (uint16_t)limite);
    wifi_escribir16(REG_FIFOPAGE_CTRL_2 + 2, (uint16_t)limite);
    wifi_escribir16(REG_BCNQ1_BDNY_V1, (uint16_t)limite);
    wifi_escribir32(REG_RXFF_BNDY, RTW8822C_RXFF_SIZE - C2H_PKT_BUF - 1);
    wifi_poner8(REG_AUTO_LLT_V1, (uint8_t)BIT_AUTO_INIT_LLT_V1);
    if (!wifi_hw_listo(REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0)) { g_wifi.fase3_error = WIFI_F3_LLT; return -1; }
    wifi_escribir8(REG_CR + 3, 0);
    return 0;
}

/* init_h2c(). */
static int wifi_init_h2c(void) {
    uint32_t addr = (uint32_t)g_rsvd_h2cq_addr << TX_PAGE_SIZE_SHIFT;
    uint32_t tam = RSVD_PG_H2CQ_NUM << TX_PAGE_SIZE_SHIFT;
    wifi_escribir32(REG_H2C_HEAD, (wifi_leer32(REG_H2C_HEAD) & 0xFFFC0000u) | addr);
    wifi_escribir32(REG_H2C_READ_ADDR, (wifi_leer32(REG_H2C_READ_ADDR) & 0xFFFC0000u) | addr);
    wifi_escribir32(REG_H2C_TAIL, (wifi_leer32(REG_H2C_TAIL) & 0xFFFC0000u) | (addr + tam));
    wifi_escribir8(REG_H2C_INFO, (uint8_t)((wifi_leer8(REG_H2C_INFO) & 0xFC) | 0x01));
    wifi_escribir8(REG_H2C_INFO, (uint8_t)((wifi_leer8(REG_H2C_INFO) & 0xFB) | 0x04));
    wifi_escribir8(REG_TXDMA_OFFSET_CHK + 1, (uint8_t)((wifi_leer8(REG_TXDMA_OFFSET_CHK + 1) & 0x7F) | 0x80));
    uint32_t wp = wifi_leer32(REG_H2C_PKT_WRITEADDR) & 0x3FFFF;
    uint32_t rp = wifi_leer32(REG_H2C_PKT_READADDR) & 0x3FFFF;
    uint32_t libre = wp >= rp ? tam - (wp - rp) : rp - wp;
    g_wifi.h2cq_tam = tam;
    g_wifi.h2cq_libre = libre;
    if (tam != libre) { g_wifi.fase3_error = WIFI_F3_H2CQ; return -1; }
    return 0;
}

/* rtw8822c_mac_init(): mismo orden que rtw8822c.c, valores WLAN_* extraidos. */
static void wifi_mac_init_8822c(void) {
    /* txq control */
    wifi_poner8(REG_FWHW_TXQ_CTRL, 0x80);                     /* BIT(7) & ~BIT(1) & ~BIT(2) */
    wifi_escribir8(REG_FWHW_TXQ_CTRL + 1, WLAN_TXQ_RPT_EN);
    /* sifs control */
    wifi_escribir16(REG_SPEC_SIFS, WLAN_SIFS_DUR_TUNE);
    wifi_escribir32(REG_SIFS, WLAN_SIFS_CFG);
    wifi_escribir16(REG_RESP_SIFS_CCK, (uint16_t)(WLAN_SIFS_CCK_CTX | WLAN_SIFS_CCK_IRX << 8));
    wifi_escribir16(REG_RESP_SIFS_OFDM, (uint16_t)(WLAN_SIFS_OFDM_CTX | WLAN_SIFS_OFDM_IRX << 8));
    /* rate fallback control */
    wifi_escribir32(REG_DARFRC, WLAN_DATA_RATE_FB_CNT_1_4);
    wifi_escribir32(REG_DARFRCH, WLAN_DATA_RATE_FB_CNT_5_8);
    wifi_escribir32(REG_RARFRCH, WLAN_RTS_RATE_FB_CNT_5_8);
    wifi_escribir32(REG_ARFR0, WLAN_DATA_RATE_FB_RATE0);
    wifi_escribir32(REG_ARFRH0, WLAN_DATA_RATE_FB_RATE0_H);
    wifi_escribir32(REG_ARFR1_V1, WLAN_RTS_RATE_FB_RATE1);
    wifi_escribir32(REG_ARFRH1_V1, WLAN_RTS_RATE_FB_RATE1_H);
    wifi_escribir32(REG_ARFR4, WLAN_RTS_RATE_FB_RATE4);
    wifi_escribir32(REG_ARFRH4, WLAN_RTS_RATE_FB_RATE4_H);
    wifi_escribir32(REG_ARFR5, WLAN_RTS_RATE_FB_RATE5);
    wifi_escribir32(REG_ARFRH5, WLAN_RTS_RATE_FB_RATE5_H);
    /* protocol configuration */
    wifi_escribir8(REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
    wifi_poner8(REG_TX_HANG_CTRL, (uint8_t)BIT_EN_EOF_V1);
    wifi_escribir8(REG_PRECNT_CTRL, (uint8_t)(PRE_TXCNT_CFG & 0xFF));
    wifi_escribir8(REG_PRECNT_CTRL + 1, (uint8_t)(PRE_TXCNT_CFG >> 8));
    wifi_escribir32(REG_PROT_MODE_CTRL, PROT_MODE_CFG);
    wifi_escribir16(REG_BAR_MODE_CTRL + 2, (uint16_t)(WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8));
    wifi_escribir8(REG_FAST_EDCA_VOVI_SETTING, FAST_EDCA_VO_TH);
    wifi_escribir8(REG_FAST_EDCA_VOVI_SETTING + 2, FAST_EDCA_VI_TH);
    wifi_escribir8(REG_FAST_EDCA_BEBK_SETTING, FAST_EDCA_BE_TH);
    wifi_escribir8(REG_FAST_EDCA_BEBK_SETTING + 2, FAST_EDCA_BK_TH);
    /* close BA parser */
    wifi_quitar8(REG_LIFETIME_EN, (uint8_t)BIT_BA_PARSER_EN);
    wifi_quitar32(REG_RRSR, BITS_RRSR_RSC);
    /* EDCA configuration */
    wifi_escribir32(REG_EDCA_VO_PARAM, WLAN_EDCA_VO_PARAM);
    wifi_escribir32(REG_EDCA_VI_PARAM, WLAN_EDCA_VI_PARAM);
    wifi_escribir32(REG_EDCA_BE_PARAM, WLAN_EDCA_BE_PARAM);
    wifi_escribir32(REG_EDCA_BK_PARAM, WLAN_EDCA_BK_PARAM);
    wifi_escribir8(REG_PIFS, WLAN_PIFS_TIME);
    wifi_quitar8(REG_TX_PTCL_CTRL + 1, (uint8_t)(BIT_SIFS_BK_EN >> 8));
    wifi_poner8(REG_RD_CTRL + 1, (uint8_t)((BIT_DIS_TXOP_CFE | BIT_DIS_LSIG_CFE | BIT_DIS_STBC_CFE) >> 8));
    /* MAC clock configuration */
    wifi_quitar32(REG_AFE_CTRL1, BIT_MAC_CLK_SEL);
    wifi_escribir8(REG_USTIME_TSF, MAC_CLK_SPEED);
    wifi_escribir8(REG_USTIME_EDCA, MAC_CLK_SPEED);
    wifi_poner8(REG_MISC_CTRL, (uint8_t)(BIT_EN_FREE_CNT | BIT_DIS_SECOND_CCA));
    wifi_quitar8(REG_TIMER0_SRC_SEL, (uint8_t)BIT_TSFT_SEL_TIMER0);
    wifi_escribir16(REG_TXPAUSE, 0x0000);
    wifi_escribir8(REG_SLOT, WLAN_SLOT_TIME);
    wifi_escribir32(REG_RD_NAV_NXT, WLAN_NAV_CFG);
    wifi_escribir16(REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
    /* beacon control */
    wifi_poner8(REG_BCN_CTRL, (uint8_t)BIT_EN_BCN_FUNCTION);
    wifi_escribir32(REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
    wifi_escribir8(REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
    wifi_escribir8(REG_BCN_CTRL_CLINT0, WLAN_BCN_CTRL_CLT0);
    wifi_escribir8(REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
    wifi_escribir8(REG_BCN_MAX_ERR, WLAN_BCN_MAX_ERR);
    /* WMAC configuration */
    wifi_escribir32(REG_MAR, WLAN_MULTI_ADDR);
    wifi_escribir32(REG_MAR + 4, WLAN_MULTI_ADDR);
    wifi_escribir8(REG_BBPSF_CTRL + 2, WLAN_RESP_TXRATE);
    wifi_escribir8(REG_ACKTO, WLAN_ACK_TO);
    wifi_escribir8(REG_ACKTO_CCK, WLAN_ACK_TO_CCK);
    wifi_escribir16(REG_EIFS, WLAN_EIFS_DUR_TUNE);
    wifi_escribir8(REG_NAV_CTRL + 2, WLAN_NAV_MAX);
    wifi_escribir8(REG_WMAC_TRXPTCL_CTL_H + 2, WLAN_BAR_ACK_TYPE);
    wifi_escribir32(REG_RXFLTMAP0, WLAN_RX_FILTER0);
    wifi_escribir16(REG_RXFLTMAP2, WLAN_RX_FILTER2);
    wifi_escribir32(REG_RCR, WLAN_RCR_CFG);
    wifi_escribir8(REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
    wifi_escribir8(REG_TCR + 2, WLAN_TX_FUNC_CFG2);
    wifi_escribir8(REG_TCR + 1, WLAN_TX_FUNC_CFG1);
    wifi_poner32(REG_GENERAL_OPTION, BIT_DUMMY_FCS_READY_MASK_EN);
    wifi_escribir32(REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
    wifi_escribir8(REG_WMAC_OPTION_FUNCTION_1, WLAN_MAC_OPT_NORM_FUNC1);
    /* init low power */
    uint16_t v = (uint16_t)(wifi_leer16(REG_RXPSF_CTRL + 2) & 0xF00F);
    v |= (uint16_t)(RXGCK_FIFOTHR_1 >> 16);
    wifi_escribir16(REG_RXPSF_CTRL + 2, v);
    v = (uint16_t)(RXPSF_PKTLENTHR_1 | BIT_RXPSF_CTRLEN | BIT_RXPSF_VHTCHKEN | BIT_RXPSF_HTCHKEN |
                   BIT_RXPSF_OFDMCHKEN | BIT_RXPSF_CCKCHKEN | BIT_RXPSF_OFDMRST);
    wifi_escribir16(REG_RXPSF_CTRL, v);
    wifi_escribir32(REG_RXPSF_TYPE_CTRL, 0xFFFFFFFFu);
    /* rx ignore configuration */
    v = wifi_leer16(REG_RXPSF_CTRL);
    v &= (uint16_t)~(BIT_RXPSF_MHCHKEN | BIT_RXPSF_CCKRST | BIT_RXPSF_CONT_ERRCHKEN);
    v = (uint16_t)((v & ~BITS_RXPSF_ERRTHR) | RXPSF_ERRTHR_7);   /* BIT_SET_RXPSF_ERRTHR(v, 0x07) */
    wifi_escribir16(REG_RXPSF_CTRL, v);
    wifi_poner8(REG_SND_PTCL_CTRL, (uint8_t)BIT_DIS_CHK_VHTSIGB_CRC);
    /* Interrupt migration configuration */
    wifi_escribir32(REG_INT_MIG, WLAN_MAC_INT_MIG_CFG);
}

/* rtw_drv_info_cfg() + rtw_pci_interface_cfg(). */
static void wifi_drv_info_e_interfaz(void) {
    wifi_escribir8(REG_RX_DRVINFO_SZ, PHY_STATUS_SIZE);
    wifi_escribir8(REG_TRXFF_BNDY + 1, (uint8_t)((wifi_leer8(REG_TRXFF_BNDY + 1) & 0xF0) | 0xF));
    wifi_poner32(REG_RCR, BIT_APP_PHYSTS);
    wifi_quitar32(REG_WMAC_OPTION_FUNCTION + 4, (1u << 8) | (1u << 9));
    if (g_wifi.corte >= RTW_CHIP_VER_CUT_D)
        wifi_escribir32_mascara(REG_HCI_MIX_CFG, BIT_PCIE_EMAC_PDN_AUX_TO_FAST_CLK, 1);
}

/* ---------------------------------------------------------------- RF */
static const uint32_t g_rf_base[2] = { RTW8822C_RF_BASE_ADDR_A, RTW8822C_RF_BASE_ADDR_B };
static const uint32_t g_rf_sipi[2] = { RTW8822C_RF_SIPI_ADDR_A, RTW8822C_RF_SIPI_ADDR_B };

/* rtw_phy_write_rf_reg_mix() con mascara completa (la unica que usan las tablas). */
static void wifi_rf_escribir(uint32_t ruta, uint32_t dir, uint32_t dato) {
    dir &= 0xFF;
    if (dir != 0x00) {                                        /* rtw_phy_write_rf_reg(): acceso directo */
        wifi_escribir32_mascara(g_rf_base[ruta] + (dir << 2), RFREG_MASK, dato);
        esperar_microsegundos(1);
    } else {                                                  /* rtw_phy_write_rf_reg_sipi() */
        wifi_escribir32(g_rf_sipi[ruta], ((dir << 20) | (dato & 0x000FFFFF)) & 0x0FFFFFFF);
        esperar_microsegundos(13);
    }
}

/* rtw_phy_read_rf() (lectura directa). */
static uint32_t wifi_rf_leer(uint32_t ruta, uint32_t dir) {
    return wifi_leer32(g_rf_base[ruta] + ((dir & 0xFF) << 2)) & RFREG_MASK;
}

/* ------------------------------------------------- tablas con condicion */
/* struct rtw_phy_cond (little endian): rfe:8 intf:4 pkg:4 plat:4 intf_rsvd:4 cut:4 branch:2 neg:1 pos:1 */
#define COND_POS(w)    (((w) >> 31) & 1u)
#define COND_NEG(w)    (((w) >> 30) & 1u)
#define COND_RAMA(w)   (((w) >> 28) & 3u)
#define COND_CORTE(w)  (((w) >> 24) & 0xFu)
#define COND_PKG(w)    (((w) >> 12) & 0xFu)
#define COND_INTF(w)   (((w) >> 8) & 0xFu)
#define COND_RFE(w)    ((w) & 0xFFu)

static uint32_t g_cond_corte, g_cond_pkg, g_cond_intf, g_cond_rfe;

/* rtw_phy_setup_phy_cond(): pkg_type no se fija en el 8822C -> 0 -> 15. */
static void wifi_condiciones(void) {
    g_cond_corte = g_wifi.corte ? g_wifi.corte : 15;
    g_cond_pkg = 15;
    g_cond_intf = INTF_PCIE;
    g_cond_rfe = g_wifi.rfe;
}

/* check_positive(). */
static int wifi_cond_positiva(uint32_t c) {
    if (COND_CORTE(c) && COND_CORTE(c) != g_cond_corte) return 0;
    if (COND_PKG(c) && COND_PKG(c) != g_cond_pkg) return 0;
    if (COND_INTF(c) && COND_INTF(c) != g_cond_intf) return 0;
    if (COND_RFE(c) != g_cond_rfe) return 0;
    return 1;
}

/* rtw_phy_cfg_{mac,agc,bb,rf}(). */
static void wifi_tabla_cfg(int cfg, uint32_t ruta, uint32_t dir, uint32_t dato) {
    switch (cfg) {
    case RTW_TABLA_MAC: wifi_escribir8(dir, (uint8_t)dato); break;
    case RTW_TABLA_AGC: wifi_escribir32(dir, dato); break;
    case RTW_TABLA_BB:
        if (dir == 0xFE) esperar_milisegundos(50);
        else if (dir == 0xFD) esperar_milisegundos(5);
        else if (dir == 0xFC) esperar_milisegundos(1);
        else if (dir == 0xFB) esperar_microsegundos(50);
        else if (dir == 0xFA) esperar_microsegundos(5);
        else if (dir == 0xF9) esperar_microsegundos(1);
        else wifi_escribir32(dir, dato);
        break;
    case RTW_TABLA_RF:
        if (dir == 0xFFE) esperar_milisegundos(50);
        else if (dir == 0xFE) esperar_microsegundos(100);
        else { wifi_rf_escribir(ruta, dir, dato); esperar_microsegundos(1); }
        break;
    }
}

/* rtw_parse_tbl_phy_cond(). */
static void wifi_cargar_tabla(int indice, const uint32_t *t, uint32_t pares, int cfg, uint32_t ruta) {
    uint32_t pos_cond = 0;
    int coincide = 1, saltado = 0;
    uint32_t hechas = 0, omitidas = 0;
    for (uint32_t i = 0; i < pares; i++) {
        uint32_t a = t[2 * i], d = t[2 * i + 1];
        if (COND_POS(a)) {
            switch (COND_RAMA(a)) {
            case BRANCH_ENDIF: coincide = 1; saltado = 0; break;
            case BRANCH_ELSE:  coincide = saltado ? 0 : 1; break;
            default:           pos_cond = a; break;          /* IF / ELIF */
            }
        } else if (COND_NEG(a)) {
            if (!saltado) {
                if (wifi_cond_positiva(pos_cond)) { coincide = 1; saltado = 1; }
                else { coincide = 0; saltado = 0; }
            } else {
                coincide = 0;
            }
        } else if (coincide) {
            wifi_tabla_cfg(cfg, ruta, a, d);
            hechas++;
        } else {
            omitidas++;
        }
    }
    g_wifi.escrituras[indice] = hechas;
    g_wifi.omitidas[indice] = omitidas;
}

/* rtw8822c_header_file_init(). */
static void wifi_header_file_init(int previo) {
    wifi_poner32(REG_3WIRE, BIT_3WIRE_TX_EN | BIT_3WIRE_RX_EN);
    wifi_poner32(REG_3WIRE, BIT_3WIRE_PI_ON);
    wifi_poner32(REG_3WIRE2, BIT_3WIRE_TX_EN | BIT_3WIRE_RX_EN);
    wifi_poner32(REG_3WIRE2, BIT_3WIRE_PI_ON);
    if (previo) wifi_quitar32(REG_ENCCK, BIT_CCK_OFDM_BLK_EN);
    else        wifi_poner32(REG_ENCCK, BIT_CCK_OFDM_BLK_EN);
}

/* ================================================================== H159
 * Segunda mitad de rtw8822c_phy_set_param(): config_trx_mode, rtw_phy_init,
 * rtw8822c_rf_init (DACK, x2, thermal/power trim, PA bias) y rtw_bf_phy_init. */

static inline uint32_t wifi_leer32_mascara(uint32_t r, uint32_t m) {         /* rtw_read32_mask */
    return (wifi_leer32(r) & m) >> __builtin_ctz(m);
}
static inline void wifi_poner16(uint32_t r, uint16_t b)  { wifi_escribir16(r, (uint16_t)(wifi_leer16(r) | b)); }
static inline void wifi_quitar16(uint32_t r, uint16_t b) { wifi_escribir16(r, (uint16_t)(wifi_leer16(r) & ~b)); }

/* rtw_phy_read_rf(): lectura directa con mascara. */
static uint32_t wifi_rf_leer_mascara(uint32_t ruta, uint32_t dir, uint32_t mascara) {
    mascara &= RFREG_MASK;
    return (wifi_leer32(g_rf_base[ruta] + ((dir & 0xFF) << 2)) & mascara) >> __builtin_ctz(mascara);
}

/* rtw_phy_write_rf_reg_mix() con mascara: directo si dir != 0, SIPI (lectura-modificacion) si dir == 0. */
static void wifi_rf_escribir_mascara(uint32_t ruta, uint32_t dir, uint32_t mascara, uint32_t dato) {
    dir &= 0xFF;
    mascara &= RFREG_MASK;
    if (dir != 0x00) {
        wifi_escribir32_mascara(g_rf_base[ruta] + (dir << 2), mascara, dato);
        esperar_microsegundos(1);
        return;
    }
    if (mascara != RFREG_MASK) {
        uint32_t viejo = wifi_rf_leer_mascara(ruta, 0x00, RFREG_MASK);
        dato = (viejo & ~mascara) | (dato << __builtin_ctz(mascara));
    }
    wifi_escribir32(g_rf_sipi[ruta], ((dir << 20) | (dato & 0x000FFFFF)) & 0x0FFFFFFF);
    esperar_microsegundos(13);
}

/* rtw8822c_bb_reset(). */
static void wifi_bb_reset(void) {
    wifi_poner16(REG_SYS_FUNC_EN, (uint16_t)BIT_FEN_BB_RSTB);
    wifi_quitar16(REG_SYS_FUNC_EN, (uint16_t)BIT_FEN_BB_RSTB);
    wifi_poner16(REG_SYS_FUNC_EN, (uint16_t)BIT_FEN_BB_RSTB);
}

/* rtw8822c_toggle_igi(). */
static void wifi_toggle_igi(void) {
    uint32_t igi = wifi_leer32_mascara(REG_RXIGI, 0x7F);
    wifi_escribir32_mascara(REG_RXIGI, 0x7F, igi - 2);
    wifi_escribir32_mascara(REG_RXIGI, 0x7F00, igi - 2);
    wifi_escribir32_mascara(REG_RXIGI, 0x7F, igi);
    wifi_escribir32_mascara(REG_RXIGI, 0x7F00, igi);
}

/* rtw8822c_config_trx_mode(tx = AB, rx = AB, is_tx2_path = false): 2T2R (hal->antenna_tx/rx). */
static void wifi_config_trx_mode(uint8_t tx, uint8_t rx) {
    wifi_escribir32_mascara(REG_ORITXCODE, MASK20BITS, ((tx | rx) & BB_PATH_A) ? 0x33312 : 0x11111);
    wifi_escribir32_mascara(REG_ORITXCODE2, MASK20BITS, ((tx | rx) & BB_PATH_B) ? 0x33312 : 0x11111);
    /* config_rx_path -> config_cck_rx_path */
    if (rx == BB_PATH_A || rx == BB_PATH_B) {
        wifi_escribir32_mascara(REG_CCANRX, 0x00060000, 0x0);
        wifi_escribir32_mascara(REG_CCANRX, 0x00600000, 0x0);
    } else if (rx == BB_PATH_AB) {
        wifi_escribir32_mascara(REG_CCANRX, 0x00600000, 0x1);
        wifi_escribir32_mascara(REG_CCANRX, 0x00060000, 0x1);
    }
    if (rx == BB_PATH_A)       wifi_escribir32_mascara(REG_RXCCKSEL, 0x0F000000, 0x0);
    else if (rx == BB_PATH_B)  wifi_escribir32_mascara(REG_RXCCKSEL, 0x0F000000, 0x5);
    else if (rx == BB_PATH_AB) wifi_escribir32_mascara(REG_RXCCKSEL, 0x0F000000, 0x1);
    /* config_ofdm_rx_path */
    uint32_t dos = rx == BB_PATH_AB ? 1 : 0;
    if (rx == BB_PATH_A || rx == BB_PATH_B || rx == BB_PATH_AB) {
        wifi_escribir32_mascara(REG_RXFNCTL, 0x300, dos);
        wifi_escribir32_mascara(REG_RXFNCTL, 0x600000, dos);
        wifi_escribir32_mascara(REG_AGCSWSH, 1u << 17, dos);
        wifi_escribir32_mascara(REG_ANTWTPD, 1u << 20, dos);
        wifi_escribir32_mascara(REG_MRCM, 1u << 24, dos);
    }
    wifi_escribir32_mascara(0x824, 0x0F000000, rx);
    wifi_escribir32_mascara(0x824, 0x000F0000, rx);
    /* config_tx_path(tx, tx_path_sel_1ss = A, tx_path_cck = A, false) */
    wifi_escribir32_mascara(REG_RXCCKSEL, 0xF0000000, 0x8);  /* config_cck_tx_path(A) */
    wifi_bb_reset();
    if (tx == BB_PATH_A) {                                   /* config_ofdm_tx_path(tx, A) */
        wifi_escribir32_mascara(REG_ANTMAP0, 0xFF, 0x11);
        wifi_escribir32_mascara(REG_TXLGMAP, 0xFF, 0x0);
    } else if (tx == BB_PATH_B) {
        wifi_escribir32_mascara(REG_ANTMAP0, 0xFF, 0x12);
        wifi_escribir32_mascara(REG_TXLGMAP, 0xFF, 0x0);
    } else {
        wifi_escribir32_mascara(REG_ANTMAP0, 0xFF, 0x31);    /* tx_path_sel_1ss == BB_PATH_A */
        wifi_escribir32_mascara(REG_TXLGMAP, 0xFFFF, 0x0400);
    }
    wifi_bb_reset();
    wifi_bb_reset();
    wifi_toggle_igi();
    g_wifi.antmap0 = wifi_leer32(REG_ANTMAP0);
    g_wifi.txlgmap = wifi_leer32(REG_TXLGMAP);
    g_wifi.rxcckscl = wifi_leer32(REG_RXCCKSEL);
}

/* rtw_phy_init(): en el 8822C lo que toca hardware es adaptivity_init (cfo_init y el resto
 * son estado de software). rtw_phy_set_edcca_th(MAX, MAX) con rtw8822c_edcca_th[]. */
static void wifi_phy_init(void) {
    wifi_escribir32_mascara(0x84C, 0x00FF0000, RTW8822C_EDCCA_MAX + 0x80);   /* L2H, MASKBYTE2 */
    wifi_escribir32_mascara(0x84C, 0xFF000000, RTW8822C_EDCCA_MAX + 0x80);   /* H2L, MASKBYTE3 */
    wifi_quitar32(REG_TX_PTCL_CTRL, BIT_DIS_EDCCA);
    wifi_poner32(REG_RD_CTRL, BIT_EDCCA_MSK_CNTDOWN_EN);
    wifi_quitar32(REG_EDCCA_DECISION, BIT_EDCCA_OPTION);
    g_wifi.edcca_84c = wifi_leer32(0x84C);
    g_wifi.igi = wifi_leer32_mascara(0x1D70, 0x7F);          /* chip->dig[0]: igi_history[0] */
}

/* ---------------------------------------------------------------- DACK */
/* Estado de dm_info que sobrevive entre encendidos (rtw88 lo conserva en rtw_dev). */
static uint32_t g_dack_adck[2];
static uint16_t g_dack_msbk[2][2][DACK_MSBK_BACKUP_NUM];
static uint8_t  g_dack_dck[2][2][2];
static uint32_t g_iv[DACK_SN_8822C], g_qv[DACK_SN_8822C];

static int wifi_hw_listo_dack(uint32_t r, uint32_t m, uint32_t obj) {
    if (wifi_hw_listo(r, m, obj)) return 1;
    g_wifi.dack_esperas_fallidas++;
    return 0;
}
static uint32_t wifi_dir_escritura(uint8_t ruta) { return ruta == 0 ? 0x1800 : 0x4100; }   /* get_path_write_addr */
static uint32_t wifi_dir_lectura(uint8_t ruta)   { return ruta == 0 ? 0x2800 : 0x4500; }   /* get_path_read_addr */

static void wifi_minmax(uint32_t v, uint32_t *min, uint32_t *max) {   /* rtw8822c_rf_minmax_cmp */
    if (v >= 0x200) {
        if (*min >= 0x200) { if (*min > v) *min = v; } else { *min = v; }
        if (*max >= 0x200) { if (*max < v) *max = v; }
    } else {
        if (*min < 0x200) { if (*min > v) *min = v; }
        if (*max >= 0x200) { *max = v; } else { if (*max < v) *max = v; }
    }
}
static void wifi_iq_orden2(uint32_t *a, uint32_t *b) {                /* __rtw8822c_dac_iq_sort */
    uint32_t t;
    if ((*a >= 0x200 && *b >= 0x200) || (*a < 0x200 && *b < 0x200)) {
        if (*a > *b) { t = *a; *a = *b; *b = t; }
    } else if (*a < 0x200 && *b >= 0x200) {
        t = *a; *a = *b; *b = t;
    }
}
static void wifi_iq_orden(uint32_t *iv, uint32_t *qv) {
    for (uint32_t i = 0; i < DACK_SN_8822C - 1; i++)
        for (uint32_t j = 0; j < DACK_SN_8822C - 1 - i; j++) {
            wifi_iq_orden2(&iv[j], &iv[j + 1]);
            wifi_iq_orden2(&qv[j], &qv[j + 1]);
        }
}
static uint32_t wifi_iq_desplazamiento(const uint32_t *vec) {          /* rtw8822c_dac_iq_offset */
    uint32_t p = 0, m = 0, t;
    for (uint32_t i = 10; i < DACK_SN_8822C - 10; i++) {
        if (vec[i] > 0x200) m = (0x400 - vec[i]) + m;
        else p = vec[i] + p;
    }
    if (p > m) { t = (p - m) / (DACK_SN_8822C - 20); }
    else { t = (m - p) / (DACK_SN_8822C - 20); if (t != 0) t = 0x400 - t; }
    return t;
}
static int wifi_iq_ok(uint32_t v) {                                   /* rtw8822c_dac_iq_check */
    return !((v >= 0x200 && (0x400 - v) > 0x64) || (v < 0x200 && v > 0x64));
}
static void wifi_dac_rf_modo(uint32_t *iv_out, uint32_t *qv_out) {   /* rtw8822c_dac_cal_rf_mode */
    uint32_t *iv = g_iv, *qv = g_qv, tmp;
    int i = 0, cnt = 0;
    while (i < (int)DACK_SN_8822C && cnt < 10000) {                   /* dac_cal_iq_sample */
        cnt++;
        tmp = wifi_leer32_mascara(0x2DBC, 0x3FFFFF);
        iv[i] = (tmp & 0x3FF000) >> 12;
        qv[i] = tmp & 0x3FF;
        if (wifi_iq_ok(iv[i]) && wifi_iq_ok(qv[i])) i++;
    }
    int c = 0;                                                        /* dac_cal_iq_search */
    do {
        uint32_t i_min = iv[0], i_max = iv[0], q_min = qv[0], q_max = qv[0], di, dq;
        for (uint32_t k = 0; k < DACK_SN_8822C; k++) { wifi_minmax(iv[k], &i_min, &i_max); wifi_minmax(qv[k], &q_min, &q_max); }
        if ((i_max < 0x200 && i_min < 0x200) || (i_max >= 0x200 && i_min >= 0x200)) di = i_max - i_min;
        else di = i_max + (0x400 - i_min);
        if ((q_max < 0x200 && q_min < 0x200) || (q_max >= 0x200 && q_min >= 0x200)) dq = q_max - q_min;
        else dq = q_max + (0x400 - q_min);
        wifi_iq_orden(iv, qv);
        if (di > 5 || dq > 5) {
            tmp = wifi_leer32_mascara(0x2DBC, 0x3FFFFF);
            iv[0] = (tmp & 0x3FF000) >> 12; qv[0] = tmp & 0x3FF;
            tmp = wifi_leer32_mascara(0x2DBC, 0x3FFFFF);
            iv[DACK_SN_8822C - 1] = (tmp & 0x3FF000) >> 12; qv[DACK_SN_8822C - 1] = tmp & 0x3FF;
        } else {
            break;
        }
    } while (c++ < 100);
    *iv_out = wifi_iq_desplazamiento(iv);
    *qv_out = wifi_iq_desplazamiento(qv);
}

static void wifi_dac_bb_setting(void) {
    wifi_escribir32_mascara(0x1D58, 0xFF8, 0x1FF);
    wifi_escribir32_mascara(0x1A00, 0x3, 0x2);
    wifi_escribir32_mascara(0x1A14, 0x300, 0x3);
    wifi_escribir32(0x1D70, 0x7E7E7E7E);
    wifi_escribir32_mascara(0x180C, 0x3, 0x0);
    wifi_escribir32_mascara(0x410C, 0x3, 0x0);
    wifi_escribir32(0x1B00, 0x00000008);
    wifi_escribir8(0x1BCC, 0x3F);
    wifi_escribir32(0x1B00, 0x0000000A);
    wifi_escribir8(0x1BCC, 0x3F);
    wifi_escribir32_mascara(0x1E24, 1u << 31, 0x0);
    wifi_escribir32_mascara(0x1E28, 0xF, 0x3);
}

static void wifi_dac_cal_adc(uint8_t ruta, uint32_t *adc_ic, uint32_t *adc_qc) {
    uint32_t ic = 0, qc = 0, base = wifi_dir_escritura(ruta);
    uint32_t sel = ruta == 0 ? 0xA0000 : 0x80000;
    wifi_escribir32_mascara(base + 0x30, 1u << 30, 0x0);                   /* ADCK step1 */
    if (ruta == 1) wifi_escribir32(base + 0x30, 0x30DB8041);
    wifi_escribir32(base + 0x60, 0xF0040FF0);
    wifi_escribir32(base + 0x0C, 0xDFF00220);
    wifi_escribir32(base + 0x10, 0x02DD08C4);
    wifi_escribir32(base + 0x0C, 0x10000260);
    wifi_rf_escribir_mascara(0, 0x0, RFREG_MASK, 0x10000);
    wifi_rf_escribir_mascara(1, 0x0, RFREG_MASK, 0x10000);
    for (int i = 0; i < 10; i++) {
        wifi_escribir32(0x1C3C, sel + 0x8003);
        wifi_escribir32(0x1C24, 0x00010002);
        wifi_dac_rf_modo(&ic, &qc);
        if (ic != 0) { ic = 0x400 - ic; *adc_ic = ic; }                   /* compensation value */
        if (qc != 0) { qc = 0x400 - qc; *adc_qc = qc; }
        uint32_t t = (ic & 0x3FF) | ((qc & 0x3FF) << 10);
        wifi_escribir32(base + 0x68, t);
        g_dack_adck[ruta] = t;
        wifi_escribir32(0x1C3C, sel + 0x8103);                            /* check ADC DC offset */
        wifi_dac_rf_modo(&ic, &qc);
        if (ic >= 0x200) ic = 0x400 - ic;
        if (qc >= 0x200) qc = 0x400 - qc;
        if (ic < 5 && qc < 5) break;
    }
    wifi_escribir32(0x1C3C, 0x00000003);                                  /* ADCK step2 */
    wifi_escribir32(base + 0x0C, 0x10000260);
    wifi_escribir32(base + 0x10, 0x02D508C4);
    wifi_rf_escribir_mascara(ruta, 0x8F, 1u << 13, 0x1);                  /* release pull low switch */
}

static void wifi_dac_step1(uint8_t ruta) {
    uint32_t base = wifi_dir_escritura(ruta), lect = wifi_dir_lectura(ruta);
    wifi_escribir32(base + 0x68, g_dack_adck[ruta]);
    wifi_escribir32(base + 0x0C, 0xDFF00220);
    if (ruta == 0) { wifi_escribir32(base + 0x60, 0xF0040FF0); wifi_escribir32(0x1C38, 0xFFFFFFFF); }
    wifi_escribir32(base + 0x10, 0x02D508C5);
    wifi_escribir32(0x9B4, 0xDB66DB00);
    wifi_escribir32(base + 0xB0, 0x0A11FB88);
    wifi_escribir32(base + 0xBC, 0x0008FF81);
    wifi_escribir32(base + 0xC0, 0x0003D208);
    wifi_escribir32(base + 0xCC, 0x0A11FB88);
    wifi_escribir32(base + 0xD8, 0x0008FF81);
    wifi_escribir32(base + 0xDC, 0x0003D208);
    wifi_escribir32(base + 0xB8, 0x60000000);
    esperar_milisegundos(2);
    wifi_escribir32(base + 0xBC, 0x000AFF8D);
    esperar_milisegundos(2);
    wifi_escribir32(base + 0xB0, 0x0A11FB89);
    wifi_escribir32(base + 0xCC, 0x0A11FB89);
    esperar_milisegundos(1);
    wifi_escribir32(base + 0xB8, 0x62000000);
    wifi_escribir32(base + 0xD4, 0x62000000);
    esperar_milisegundos(20);
    if (!wifi_hw_listo_dack(lect + 0x08, 0x7FFF80, 0xFFFF) || !wifi_hw_listo_dack(lect + 0x34, 0x7FFF80, 0xFFFF)) { }
    wifi_escribir32(base + 0xB8, 0x02000000);
    esperar_milisegundos(1);
    wifi_escribir32(base + 0xBC, 0x0008FF87);
    wifi_escribir32(0x9B4, 0xDB6DB600);
    wifi_escribir32(base + 0x10, 0x02D508C5);
    wifi_escribir32(base + 0xBC, 0x0008FF87);
    wifi_escribir32(base + 0x60, 0xF0000000);
}

static void wifi_dac_step2(uint8_t ruta, uint32_t *ic_out, uint32_t *qc_out) {
    uint32_t base = wifi_dir_escritura(ruta), ic, qc;
    wifi_escribir32_mascara(base + 0xBC, 0xF0000000, 0x0);
    wifi_escribir32_mascara(base + 0xC0, 0xF, 0x8);
    wifi_escribir32_mascara(base + 0xD8, 0xF0000000, 0x0);
    wifi_escribir32_mascara(base + 0xDC, 0xF, 0x8);
    wifi_escribir32(0x1B00, 0x00000008);
    wifi_escribir8(0x1BCC, 0x03F);
    wifi_escribir32(base + 0x0C, 0xDFF00220);
    wifi_escribir32(base + 0x10, 0x02D508C5);
    wifi_escribir32(0x1C3C, 0x00088103);
    wifi_dac_rf_modo(&ic, &qc);
    if (ic != 0) ic = 0x400 - ic;                                         /* compensation value */
    if (qc != 0) qc = 0x400 - qc;
    if (ic < 0x300) { ic = ic * 2 * 6 / 5; ic = ic + 0x80; }
    else { ic = (0x400 - ic) * 2 * 6 / 5; ic = 0x7F - ic; }
    if (qc < 0x300) { qc = qc * 2 * 6 / 5; qc = qc + 0x80; }
    else { qc = (0x400 - qc) * 2 * 6 / 5; qc = 0x7F - qc; }
    *ic_out = ic;
    *qc_out = qc;
}

static void wifi_dac_step3(uint8_t ruta, uint32_t adc_ic, uint32_t adc_qc, uint32_t *ic_in, uint32_t *qc_in,
                           uint32_t *i_out, uint32_t *q_out) {
    uint32_t base = wifi_dir_escritura(ruta), lect = wifi_dir_lectura(ruta);
    uint32_t ic = *ic_in, qc = *qc_in;
    wifi_escribir32(base + 0x0C, 0xDFF00220);
    wifi_escribir32(base + 0x10, 0x02D508C5);
    wifi_escribir32(0x9B4, 0xDB66DB00);
    wifi_escribir32(base + 0xB0, 0x0A11FB88);
    wifi_escribir32(base + 0xBC, 0xC008FF81);
    wifi_escribir32(base + 0xC0, 0x0003D208);
    wifi_escribir32_mascara(base + 0xBC, 0xF0000000, ic & 0xF);
    wifi_escribir32_mascara(base + 0xC0, 0xF, (ic & 0xF0) >> 4);
    wifi_escribir32(base + 0xCC, 0x0A11FB88);
    wifi_escribir32(base + 0xD8, 0xE008FF81);
    wifi_escribir32(base + 0xDC, 0x0003D208);
    wifi_escribir32_mascara(base + 0xD8, 0xF0000000, qc & 0xF);
    wifi_escribir32_mascara(base + 0xDC, 0xF, (qc & 0xF0) >> 4);
    wifi_escribir32(base + 0xB8, 0x60000000);
    esperar_milisegundos(2);
    wifi_escribir32_mascara(base + 0xBC, 0xE, 0x6);
    esperar_milisegundos(2);
    wifi_escribir32(base + 0xB0, 0x0A11FB89);
    wifi_escribir32(base + 0xCC, 0x0A11FB89);
    esperar_milisegundos(1);
    wifi_escribir32(base + 0xB8, 0x62000000);
    wifi_escribir32(base + 0xD4, 0x62000000);
    esperar_milisegundos(20);
    if (!wifi_hw_listo_dack(lect + 0x24, 0x07F80000, ic) || !wifi_hw_listo_dack(lect + 0x50, 0x07F80000, qc)) { }
    wifi_escribir32(base + 0xB8, 0x02000000);
    esperar_milisegundos(1);
    wifi_escribir32_mascara(base + 0xBC, 0xE, 0x3);
    wifi_escribir32(0x9B4, 0xDB6DB600);
    /* check DAC DC offset */
    uint32_t t = ((adc_ic + 0x10) & 0x3FF) | (((adc_qc + 0x10) & 0x3FF) << 10);
    wifi_escribir32(base + 0x68, t);
    wifi_escribir32(base + 0x10, 0x02D508C5);
    wifi_escribir32(base + 0x60, 0xF0000000);
    wifi_dac_rf_modo(&ic, &qc);
    ic = ic >= 0x10 ? ic - 0x10 : 0x400 - (0x10 - ic);
    qc = qc >= 0x10 ? qc - 0x10 : 0x400 - (0x10 - qc);
    *i_out = ic;
    *q_out = qc;
    if (ic >= 0x200) ic = 0x400 - ic;
    if (qc >= 0x200) qc = 0x400 - qc;
    *ic_in = ic;
    *qc_in = qc;
}

static void wifi_dac_step4(uint8_t ruta) {
    uint32_t base = wifi_dir_escritura(ruta);
    wifi_escribir32(base + 0x68, 0x0);
    wifi_escribir32(base + 0x10, 0x02D508C4);
    wifi_escribir32_mascara(base + 0xBC, 0x1, 0x0);
    wifi_escribir32_mascara(base + 0x30, 1u << 30, 0x1);
}

static void wifi_dac_respaldo_vec(uint8_t ruta, uint8_t vec, uint32_t w, uint32_t r) {
    for (uint32_t i = 0; i < DACK_MSBK_BACKUP_NUM; i++) {
        wifi_escribir32_mascara(w, 0xF0000000, i);
        g_dack_msbk[ruta][vec][i] = (uint16_t)wifi_leer32_mascara(r, 0x7FC0000);
    }
}
static void wifi_dac_respaldo_ruta(uint8_t ruta) {
    wifi_dac_respaldo_vec(ruta, 0, wifi_dir_escritura(ruta) + 0xB0, wifi_dir_lectura(ruta) + 0x10);
    wifi_dac_respaldo_vec(ruta, 1, wifi_dir_escritura(ruta) + 0xB0 + 0x1C, wifi_dir_lectura(ruta) + 0x10 + 0x2C);
}
/* rtw8822c_dac_cal_backup_dck(): se respetan los indices de rtw88 (B usa [0][1]/[1][0] cruzados). */
static void wifi_dac_respaldo_dck(void) {
    g_dack_dck[0][0][0] = (uint8_t)wifi_leer32_mascara(REG_DCKA_I_0, 0xF0000000);
    g_dack_dck[0][0][1] = (uint8_t)wifi_leer32_mascara(REG_DCKA_I_1, 0xF);
    g_dack_dck[0][1][0] = (uint8_t)wifi_leer32_mascara(REG_DCKA_Q_0, 0xF0000000);
    g_dack_dck[0][1][1] = (uint8_t)wifi_leer32_mascara(REG_DCKA_Q_1, 0xF);
    g_dack_dck[1][0][0] = (uint8_t)wifi_leer32_mascara(REG_DCKB_I_0, 0xF0000000);
    g_dack_dck[1][1][0] = (uint8_t)wifi_leer32_mascara(REG_DCKB_I_1, 0xF);
    g_dack_dck[1][0][1] = (uint8_t)wifi_leer32_mascara(REG_DCKB_Q_0, 0xF0000000);
    g_dack_dck[1][1][1] = (uint8_t)wifi_leer32_mascara(REG_DCKB_Q_1, 0xF);
}
static void wifi_dac_respaldo(void) {                                 /* rtw8822c_dac_cal_backup */
    uint32_t t0 = wifi_leer32(0x1860), t1 = wifi_leer32(0x4160), t2 = wifi_leer32(0x9B4);
    wifi_escribir32(0x9B4, 0xDB66DB00);
    wifi_quitar32(0x1830, 1u << 30);
    wifi_escribir32_mascara(0x1860, 0xFC000000, 0x3C);
    wifi_dac_respaldo_ruta(0);
    wifi_quitar32(0x4130, 1u << 30);
    wifi_escribir32_mascara(0x4160, 0xFC000000, 0x3C);
    wifi_dac_respaldo_ruta(1);
    wifi_dac_respaldo_dck();
    wifi_poner32(0x1830, 1u << 30);
    wifi_poner32(0x4130, 1u << 30);
    wifi_escribir32(0x1860, t0);
    wifi_escribir32(0x4160, t1);
    wifi_escribir32(0x9B4, t2);
}

static void wifi_dac_restaurar_dck(void) {                            /* rtw8822c_dac_cal_restore_dck */
    wifi_poner32(REG_DCKA_I_0, 1u << 19);
    wifi_escribir32_mascara(REG_DCKA_I_0, 0xF0000000, g_dack_dck[0][0][0]);
    wifi_escribir32_mascara(REG_DCKA_I_1, 0xF, g_dack_dck[0][0][1]);
    wifi_poner32(REG_DCKA_Q_0, 1u << 19);
    wifi_escribir32_mascara(REG_DCKA_Q_0, 0xF0000000, g_dack_dck[0][1][0]);
    wifi_escribir32_mascara(REG_DCKA_Q_1, 0xF, g_dack_dck[0][1][1]);
    wifi_poner32(REG_DCKB_I_0, 1u << 19);
    wifi_escribir32_mascara(REG_DCKB_I_0, 0xF0000000, g_dack_dck[1][0][0]);
    wifi_escribir32_mascara(REG_DCKB_I_1, 0xF, g_dack_dck[1][0][1]);
    wifi_poner32(REG_DCKB_Q_0, 1u << 19);
    wifi_escribir32_mascara(REG_DCKB_Q_0, 0xF0000000, g_dack_dck[1][1][0]);
    wifi_escribir32_mascara(REG_DCKB_Q_1, 0xF, g_dack_dck[1][1][1]);
}

static void wifi_dac_restaurar_preparar(void) {                       /* rtw8822c_dac_cal_restore_prepare */
    wifi_escribir32(0x9B4, 0xDB66DB00);
    wifi_escribir32_mascara(0x18B0, 1u << 27, 0x0);
    wifi_escribir32_mascara(0x18CC, 1u << 27, 0x0);
    wifi_escribir32_mascara(0x41B0, 1u << 27, 0x0);
    wifi_escribir32_mascara(0x41CC, 1u << 27, 0x0);
    wifi_escribir32_mascara(0x1830, 1u << 30, 0x0);
    wifi_escribir32_mascara(0x1860, 0xFC000000, 0x3C);
    wifi_escribir32_mascara(0x18B4, 1u << 0, 0x1);
    wifi_escribir32_mascara(0x18D0, 1u << 0, 0x1);
    wifi_escribir32_mascara(0x4130, 1u << 30, 0x0);
    wifi_escribir32_mascara(0x4160, 0xFC000000, 0x3C);
    wifi_escribir32_mascara(0x41B4, 1u << 0, 0x1);
    wifi_escribir32_mascara(0x41D0, 1u << 0, 0x1);
    wifi_escribir32_mascara(0x18B0, 0xF00, 0x0);
    wifi_escribir32_mascara(0x18C0, 1u << 14, 0x0);
    wifi_escribir32_mascara(0x18CC, 0xF00, 0x0);
    wifi_escribir32_mascara(0x18DC, 1u << 14, 0x0);
    wifi_escribir32_mascara(0x18B0, 1u << 0, 0x0);
    wifi_escribir32_mascara(0x18CC, 1u << 0, 0x0);
    wifi_escribir32_mascara(0x18B0, 1u << 0, 0x1);
    wifi_escribir32_mascara(0x18CC, 1u << 0, 0x1);
    wifi_dac_restaurar_dck();
    wifi_escribir32_mascara(0x18C0, 0x38000, 0x7);
    wifi_escribir32_mascara(0x18DC, 0x38000, 0x7);
    wifi_escribir32_mascara(0x41C0, 0x38000, 0x7);
    wifi_escribir32_mascara(0x41DC, 0x38000, 0x7);
    wifi_escribir32_mascara(0x18B8, (1u << 26) | (1u << 25), 0x1);
    wifi_escribir32_mascara(0x18D4, (1u << 26) | (1u << 25), 0x1);
    wifi_escribir32_mascara(0x41B0, 0xF00, 0x0);
    wifi_escribir32_mascara(0x41C0, 1u << 14, 0x0);
    wifi_escribir32_mascara(0x41CC, 0xF00, 0x0);
    wifi_escribir32_mascara(0x41DC, 1u << 14, 0x0);
    wifi_escribir32_mascara(0x41B0, 1u << 0, 0x0);
    wifi_escribir32_mascara(0x41CC, 1u << 0, 0x0);
    wifi_escribir32_mascara(0x41B0, 1u << 0, 0x1);
    wifi_escribir32_mascara(0x41CC, 1u << 0, 0x1);
    wifi_escribir32_mascara(0x41B8, (1u << 26) | (1u << 25), 0x1);
    wifi_escribir32_mascara(0x41D4, (1u << 26) | (1u << 25), 0x1);
}

static int wifi_dac_restaurar_espera(uint32_t objetivo, uint32_t conmutar) {
    uint32_t cnt = 0;
    do {
        wifi_escribir32_mascara(conmutar, (1u << 26) | (1u << 25), 0x0);
        wifi_escribir32_mascara(conmutar, (1u << 26) | (1u << 25), 0x2);
        if (wifi_leer32_mascara(objetivo, 0xF) == 0x6) return 1;
    } while (cnt++ < 100);
    return 0;
}

static int wifi_dac_restaurar_ruta(uint8_t ruta) {                    /* rtw8822c_dac_cal_restore_path */
    uint32_t w_i = wifi_dir_escritura(ruta) + 0xB0, r_i = wifi_dir_lectura(ruta) + 0x08;
    uint32_t w_q = w_i + 0x1C, r_q = r_i + 0x2C;
    if (!wifi_dac_restaurar_espera(r_i, w_i + 0x8)) return 0;
    for (uint32_t i = 0; i < DACK_MSBK_BACKUP_NUM; i++) {
        wifi_escribir32_mascara(w_i + 0x4, 1u << 2, 0x0);
        wifi_escribir32_mascara(w_i + 0x4, 0xFF8, g_dack_msbk[ruta][0][i]);
        wifi_escribir32_mascara(w_i, 0xF0000000, i);
        wifi_escribir32_mascara(w_i + 0x4, 1u << 2, 0x1);
    }
    wifi_escribir32_mascara(w_i + 0x4, 1u << 2, 0x0);
    if (!wifi_dac_restaurar_espera(r_q, w_q + 0x8)) return 0;
    for (uint32_t i = 0; i < DACK_MSBK_BACKUP_NUM; i++) {
        wifi_escribir32_mascara(w_q + 0x4, 1u << 2, 0x0);
        wifi_escribir32_mascara(w_q + 0x4, 0xFF8, g_dack_msbk[ruta][1][i]);
        wifi_escribir32_mascara(w_q, 0xF0000000, i);
        wifi_escribir32_mascara(w_q + 0x4, 1u << 2, 0x1);
    }
    wifi_escribir32_mascara(w_q + 0x4, 1u << 2, 0x0);
    wifi_escribir32_mascara(w_i + 0x8, (1u << 26) | (1u << 25), 0x0);
    wifi_escribir32_mascara(w_q + 0x8, (1u << 26) | (1u << 25), 0x0);
    wifi_escribir32_mascara(w_i + 0x4, 1u << 0, 0x0);
    wifi_escribir32_mascara(w_q + 0x4, 1u << 0, 0x0);
    return 1;
}

static int wifi_dac_restaurar(void) {                                 /* rtw8822c_dac_cal_restore */
    if (g_dack_msbk[0][0][0] == 0 && g_dack_msbk[0][1][0] == 0 &&
        g_dack_msbk[1][0][0] == 0 && g_dack_msbk[1][1][0] == 0)
        return 0;
    uint32_t t0 = wifi_leer32(0x1860), t1 = wifi_leer32(0x4160), t2 = wifi_leer32(0x9B4);
    wifi_dac_restaurar_preparar();
    if (!wifi_hw_listo(0x2808, 0x7FFF80, 0xFFFF) || !wifi_hw_listo(0x2834, 0x7FFF80, 0xFFFF) ||
        !wifi_hw_listo(0x4508, 0x7FFF80, 0xFFFF) || !wifi_hw_listo(0x4534, 0x7FFF80, 0xFFFF))
        return 0;
    if (!wifi_dac_restaurar_ruta(0) || !wifi_dac_restaurar_ruta(1)) return 0;
    wifi_escribir32_mascara(0x1830, 1u << 30, 0x1);
    wifi_escribir32_mascara(0x4130, 1u << 30, 0x1);
    wifi_escribir32(0x1860, t0);
    wifi_escribir32(0x4160, t1);
    wifi_escribir32_mascara(0x18B0, 1u << 27, 0x1);
    wifi_escribir32_mascara(0x18CC, 1u << 27, 0x1);
    wifi_escribir32_mascara(0x41B0, 1u << 27, 0x1);
    wifi_escribir32_mascara(0x41CC, 1u << 27, 0x1);
    wifi_escribir32(0x9B4, t2);
    return 1;
}

/* rtw8822c_rf_dac_cal(). */
static void wifi_dack(void) {
    static const uint32_t regs[DACK_REG_8822C] = { 0x180C, 0x1810, 0x410C, 0x4110, 0x1C3C, 0x1C24, 0x1D70, 0x9B4,
                                                  0x1A00, 0x1A14, 0x1D58, 0x1C38, 0x1E24, 0x1E28, 0x1860, 0x4160 };
    uint32_t respaldo[DACK_REG_8822C];
    uint32_t respaldo_rf[DACK_RF_8822C * DACK_PATH_8822C];
    g_wifi.dack_esperas_fallidas = 0;
    g_wifi.dack_restaurado = 0;
    if (wifi_dac_restaurar()) { g_wifi.dack_restaurado = 1; return; }

    /* rtw8822c_dac_backup_reg(). OJO: rtw88 indexa backup_rf[path * i + i]; con DACK_RF_8822C = 1
     * ambas rutas usan la entrada 0 y la ruta A se restaura con el valor de B. Se replica igual. */
    for (uint32_t i = 0; i < DACK_REG_8822C; i++) respaldo[i] = wifi_leer32(regs[i]);
    for (uint32_t ruta = 0; ruta < DACK_PATH_8822C; ruta++)
        for (uint32_t i = 0; i < DACK_RF_8822C; i++)
            respaldo_rf[ruta * i + i] = wifi_rf_leer_mascara(ruta, 0x8F, RFREG_MASK);

    wifi_dac_bb_setting();
    for (uint8_t ruta = 0; ruta < 2; ruta++) {
        uint32_t adc_ic = 0, adc_qc = 0, ic = 0, qc = 0, i_o = 0, q_o = 0, n = 0;
        wifi_dac_cal_adc(ruta, &adc_ic, &adc_qc);
        for (n = 0; n < 10; n++) {
            wifi_dac_step1(ruta);
            wifi_dac_step2(ruta, &ic, &qc);
            g_wifi.dack_ic[ruta] = ic;
            g_wifi.dack_qc[ruta] = qc;
            wifi_dac_step3(ruta, adc_ic, adc_qc, &ic, &qc, &i_o, &q_o);
            if (ic < 5 && qc < 5) break;
        }
        wifi_dac_step4(ruta);
        g_wifi.dack_i[ruta] = i_o;
        g_wifi.dack_q[ruta] = q_o;
        g_wifi.dack_vueltas[ruta] = n + 1;
        g_wifi.dack_adck[ruta] = g_dack_adck[ruta];
    }
    wifi_escribir32(0x1B00, 0x00000008);
    wifi_escribir32_mascara(0x4130, 1u << 30, 0x1);
    wifi_escribir8(0x1BCC, 0x0);
    wifi_escribir32(0x1B00, 0x0000000A);
    wifi_escribir8(0x1BCC, 0x0);
    /* rtw8822c_dac_restore_reg() */
    for (uint32_t i = 0; i < DACK_REG_8822C; i++) wifi_escribir32(regs[i], respaldo[i]);
    for (uint32_t ruta = 0; ruta < DACK_PATH_8822C; ruta++)
        for (uint32_t i = 0; i < DACK_RF_8822C; i++)
            wifi_rf_escribir_mascara(ruta, 0x8F, RFREG_MASK, respaldo_rf[ruta * i + i]);
    wifi_dac_respaldo();
}

/* rtw_read8_physical_efuse(). */
static uint8_t wifi_efuse_fisico8(uint16_t dir) {
    wifi_escribir32_mascara(REG_EFUSE_CTRL, 0x3FF00, dir);
    wifi_quitar32(REG_EFUSE_CTRL, BIT_EF_FLAG);
    for (int i = 0; i < 100; i++) {                       /* read_poll_timeout(1000 us, 100000 us) */
        if (wifi_leer32(REG_EFUSE_CTRL) & BIT_EF_FLAG) return wifi_leer8(REG_EFUSE_CTRL);
        esperar_milisegundos(1);
    }
    return (uint8_t)EFUSE_READ_FAIL;
}

/* rtw8822c_rf_x2_check(). */
static void wifi_x2_check(void) {
    esperar_milisegundos(1);
    g_wifi.x2k_ocupado = (uint8_t)wifi_rf_leer_mascara(0, 0xB8, 1u << 15);
    if (g_wifi.x2k_ocupado == 1) {
        wifi_rf_escribir_mascara(0, 0xB8, RFREG_MASK, 0xC4440);
        wifi_rf_escribir_mascara(0, 0xBA, RFREG_MASK, 0x6840D);
        wifi_rf_escribir_mascara(0, 0xB8, RFREG_MASK, 0x80440);
        esperar_milisegundos(1);
    }
}

/* rtw8822c_thermal_trim(). */
static void wifi_thermal_trim(void) {
    static const uint16_t ef[2] = { PPG_THERMAL_A, PPG_THERMAL_B };
    g_wifi.termico_trim[0] = g_wifi.termico_trim[1] = 0xFF;
    for (uint32_t ruta = 0; ruta < 2; ruta++) {
        uint8_t pg = wifi_efuse_fisico8(ef[ruta]);
        if (pg == EFUSE_READ_FAIL) return;
        uint8_t t = (uint8_t)(((pg >> 1) & 0x7) | ((pg & 1u) << 3));   /* GENMASK(3,1) | BIT(0) -> BIT(3) */
        g_wifi.termico_trim[ruta] = t;
        wifi_rf_escribir_mascara(ruta, 0x43, RF_THEMAL_MASK, t);
    }
}

/* rtw8822c_power_trim() + rtw8822c_set_power_trim(). */
static void wifi_power_trim(void) {
    static const uint16_t ef2g[3] = { PPG_2GL_TXAB, PPG_2GM_TXAB, PPG_2GH_TXAB };
    static const uint16_t ef5g[2][5] = { { PPG_5GL1_TXA, PPG_5GL2_TXA, PPG_5GM1_TXA, PPG_5GM2_TXA, PPG_5GH1_TXA },
                                         { PPG_5GL1_TXB, PPG_5GL2_TXB, PPG_5GM1_TXB, PPG_5GM2_TXB, PPG_5GH1_TXB } };
    int8_t g[2][8];
    memset(g, 0, sizeof(g));
    int poner = 0;
    for (uint32_t i = 0; i < 3; i++) {
        uint8_t pg = wifi_efuse_fisico8(ef2g[i]);
        if (pg == EFUSE_READ_FAIL) continue;
        poner = 1;
        g[0][i] = (int8_t)((pg & PPG_2G_A_MASK) >> __builtin_ctz(PPG_2G_A_MASK));
        g[1][i] = (int8_t)((pg & PPG_2G_B_MASK) >> __builtin_ctz(PPG_2G_B_MASK));
    }
    for (uint32_t i = 0; i < 5; i++)
        for (uint32_t ruta = 0; ruta < 2; ruta++) {
            uint8_t pg = wifi_efuse_fisico8(ef5g[ruta][i]);
            if (pg == EFUSE_READ_FAIL) continue;
            poner = 1;
            g[ruta][i + 3] = (int8_t)(pg & PPG_5G_MASK);
        }
    memcpy(g_wifi.bb_gain, g, sizeof(g));
    g_wifi.power_trim_aplicado = poner;
    if (poner) {
        static const uint8_t idx[15] = { 0, 1, 2, 2, 3, 4, 5, 6, 7, 3, 4, 5, 6, 7, 7 };   /* RF_SET_POWER_TRIM */
        for (uint32_t ruta = 0; ruta < 2; ruta++) {
            wifi_rf_escribir_mascara(ruta, 0xEE, 1u << 19, 1);
            for (uint32_t s = 0; s < 15; s++) {
                wifi_rf_escribir_mascara(ruta, 0x33, RFREG_MASK, s);
                wifi_rf_escribir_mascara(ruta, 0x3F, RFREG_MASK, (uint32_t)(uint8_t)g[ruta][idx[s]]);
            }
            wifi_rf_escribir_mascara(ruta, 0xEE, 1u << 19, 0);
        }
    }
    wifi_escribir32_mascara(REG_DIS_DPD, DIS_DPD_MASK, DIS_DPD_RATEALL);
}

/* rtw8822c_pa_bias(). */
static void wifi_pa_bias(void) {
    static const uint16_t ef2g[2] = { PPG_PABIAS_2GA, PPG_PABIAS_2GB };
    static const uint16_t ef5g[2] = { PPG_PABIAS_5GA, PPG_PABIAS_5GB };
    for (uint32_t ruta = 0; ruta < 2; ruta++) g_wifi.pa_bias_2g[ruta] = g_wifi.pa_bias_5g[ruta] = 0xFF;
    for (uint32_t ruta = 0; ruta < 2; ruta++) {
        uint8_t pg = wifi_efuse_fisico8(ef2g[ruta]);
        if (pg == EFUSE_READ_FAIL) return;
        pg &= PPG_PABIAS_MASK;
        g_wifi.pa_bias_2g[ruta] = pg;
        wifi_rf_escribir_mascara(ruta, RF_PA, RF_PABIAS_2G_MASK, pg);
    }
    for (uint32_t ruta = 0; ruta < 2; ruta++) {
        uint8_t pg = (uint8_t)(wifi_efuse_fisico8(ef5g[ruta]) & PPG_PABIAS_MASK);
        g_wifi.pa_bias_5g[ruta] = pg;
        wifi_rf_escribir_mascara(ruta, RF_PA, RF_PABIAS_5G_MASK, pg);
    }
}

/* rtw_bf_phy_init(). */
static void wifi_bf_phy_init(void) {
    uint32_t t = wifi_leer32(REG_MU_TX_CTL);
    t |= BIT_MU_P1_WAIT_STATE_EN;
    t &= ~BIT_MASK_R_MU_RL;
    t |= (0xAu << BIT_SHIFT_R_MU_RL) & BIT_MASK_R_MU_RL;
    t &= ~BIT_EN_MU_MIMO;
    t &= ~BIT_MASK_R_MU_TABLE_VALID;
    wifi_escribir32(REG_MU_TX_CTL, t);
    wifi_escribir8(REG_WMAC_MU_BF_OPTION, (uint8_t)((3u << BIT_SHIFT_WMAC_TXMU_ACKPOLICY) | BIT_WMAC_TXMU_ACKPOLICY_EN));
    wifi_escribir16(REG_WMAC_MU_BF_CTL, 0);
    wifi_poner32(REG_TXBF_CTRL, BIT_USE_NDPA_PARAMETER);
    wifi_escribir8(REG_NDPA_OPT_CTRL, 0x10);
    wifi_escribir32_mascara(REG_BBPSF_CTRL, BIT_MASK_CSI_RATE, DESC_RATE6M);
}

/* Segunda mitad de rtw8822c_phy_set_param(), en su orden. */
static void wifi_fase3b(void) {
    wifi_config_trx_mode(BB_PATH_AB, BB_PATH_AB);   /* hal->antenna_tx/rx = BB_PATH_AB por BIT_RF_TYPE_ID (2T2R) */
    wifi_phy_init();
    /* cck_gi_u/l_bnd: lecturas de 0x1a98/0x1aa8/0x1a70 para dm_info (software, se usan en RX). */
    wifi_dack();                                    /* rtw8822c_rf_init(): dac_cal, x2, thermal, power, pa_bias */
    wifi_x2_check();
    wifi_thermal_trim();
    wifi_power_trim();
    wifi_pa_bias();
    /* rtw8822c_pwrtrack_init(): solo software. */
    wifi_bf_phy_init();
    g_wifi.fase3b_hecha = 1;
}

/* rtw_mac_init() -> rtw8822c_phy_set_param() completo (H158 + H159). */
static int wifi_fase3(void) {
    memset(g_wifi.escrituras, 0, sizeof(g_wifi.escrituras));
    memset(g_wifi.omitidas, 0, sizeof(g_wifi.omitidas));
    /* rtw_init_trx_cfg() */
    wifi_txdma_queue_mapping();
    if (wifi_priority_queue_cfg()) return -1;
    if (wifi_init_h2c()) return -1;
    wifi_mac_init_8822c();
    wifi_drv_info_e_interfaz();

    /* rtw8822c_phy_set_param(): power on BB/RF domain */
    wifi_poner8(REG_SYS_FUNC_EN, (uint8_t)(BIT_FEN_BB_GLB_RST | BIT_FEN_BB_RSTB));
    wifi_poner8(REG_RF_CTRL, (uint8_t)(BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB));
    wifi_poner32(REG_WLRF1, BIT_WLRF1_BBRF_EN);
    wifi_escribir32_mascara(REG_DIS_DPD, DIS_DPD_MASK, DIS_DPD_RATEALL);   /* disable low rate DPD */
    wifi_header_file_init(1);

    /* rtw_phy_load_tables(): mac, bb, agc, (sin agc_btg en RFE 1), rfk init, rf_tbl[0..1] = {B, A} */
    wifi_condiciones();
    wifi_cargar_tabla(0, rtw8822c_mac, RTW8822C_MAC_PARES, RTW8822C_MAC_CFG, 0);
    wifi_cargar_tabla(1, rtw8822c_bb, RTW8822C_BB_PARES, RTW8822C_BB_CFG, 0);
    wifi_cargar_tabla(2, rtw8822c_agc, RTW8822C_AGC_PARES, RTW8822C_AGC_CFG, 0);
    /* rtw_load_rfk_table() */
    wifi_escribir32_mascara(0x1E24, 1u << 17, 1);
    wifi_escribir32_mascara(0x1CD0, 1u << 28, 1);
    wifi_escribir32_mascara(0x1CD0, 1u << 29, 1);
    wifi_escribir32_mascara(0x1CD0, 1u << 30, 1);
    wifi_escribir32_mascara(0x1CD0, 1u << 31, 0);
    wifi_cargar_tabla(3, rtw8822c_array_mp_cal_init, RTW8822C_ARRAY_MP_CAL_INIT_PARES,
                      RTW8822C_ARRAY_MP_CAL_INIT_CFG, 0);
    wifi_cargar_tabla(4, rtw8822c_rf_b, RTW8822C_RF_B_PARES, RTW8822C_RF_B_CFG, RTW8822C_RF_B_RUTA);
    wifi_cargar_tabla(5, rtw8822c_rf_a, RTW8822C_RF_A_PARES, RTW8822C_RF_A_CFG, RTW8822C_RF_A_RUTA);

    /* cristal: rtw_write32_mask(REG_ANAPAR_XTAL_0, 0xfffc00, cap | cap << 7) */
    uint32_t cap = g_wifi.cristal & 0x7F;
    wifi_escribir32_mascara(REG_ANAPAR_XTAL_0, 0xFFFC00u, cap | (cap << 7));
    g_wifi.xtal0 = wifi_leer32(REG_ANAPAR_XTAL_0);
    wifi_header_file_init(0);
    wifi_fase3b();                                  /* H159 */

    g_wifi.rf_a_00 = wifi_rf_leer(0, 0x00); g_wifi.rf_a_18 = wifi_rf_leer(0, 0x18);
    g_wifi.rf_b_00 = wifi_rf_leer(1, 0x00); g_wifi.rf_b_18 = wifi_rf_leer(1, 0x18);
    g_wifi.fase3_hecha = 1;
    return 0;
}

/* ================================================================== H160
 * rtw8822c_set_channel() a 20 MHz, vaciado del anillo RX (rtw_pci_rx_napi) y
 * escaneo pasivo: solo se escucha, no se transmite nada. */

int wifi_encender(const char *ruta_usb) { RED_EXCLUSIVA();
    g_con_radio = 2;
    int r = wifi_cargar_firmware(ruta_usb);
    g_con_radio = 0;
    return r;
}

/* rtw_chip_efuse_disable()/rtw_power_off(): sin anillos activos no debe quedar bus master. */
void wifi_apagar(void) { RED_EXCLUSIVA();
    if (!g_wifi_mmio) return;
    wifi_escribir32(RTK_PCI_HIMR0, 0);
    wifi_escribir32(RTK_PCI_HIMR1, 0);
    wifi_escribir32(RTK_PCI_HIMR3, 0);
    wifi_power_switch(0);
    wifi_bus_master(0);
    g_wifi.radio_activa = 0;
}

static int wifi_ch_5g(uint8_t c) {      /* IS_CH_5G_BAND */
    return (c >= 36 && c <= 48) || (c >= 52 && c <= 64) || (c >= 100 && c <= 144) || (c >= 149 && c <= 177);
}

/* rtw8822c_set_channel_bb(). H176: 20 u 80 MHz (c = canal central, idx = subcanal primario). */
static void wifi_canal_bb(uint8_t c, uint8_t bw, uint8_t idx) {
    if (c <= 14) {
        wifi_quitar32(REG_BGCTRL, BITS_RX_IQ_WEIGHT);
        wifi_poner32(REG_TXF4, 1u << 20);
        wifi_quitar32(REG_CCK_CHECK, BIT_CHECK_CCK_EN);
        wifi_quitar32(REG_CCKTXONLY, BIT_BB_CCK_CHECK_EN);
        wifi_escribir32_mascara(REG_CCAMSK, 0x3F000000, 0xF);
        wifi_escribir32_mascara(REG_RXAGCCTL0, BITS_RXAGC_CCK, 0x5);
        wifi_escribir32_mascara(REG_RXAGCCTL, BITS_RXAGC_CCK, 0x5);
        wifi_escribir32_mascara(REG_RXAGCCTL0, BITS_RXAGC_OFDM, 0x6);
        wifi_escribir32_mascara(REG_RXAGCCTL, BITS_RXAGC_OFDM, 0x6);
        if (c == 13 || c == 14)      wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x969);
        else if (c == 11 || c == 12) wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x96A);
        else                         wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x9AA);
        if (c == 14) {
            wifi_escribir32_mascara(REG_TXF0, MASKHWORD, 0x3DA0);
            wifi_escribir32_mascara(REG_TXF1, MASKDWORD, 0x4962C931);
            wifi_escribir32_mascara(REG_TXF2, MASKLWORD, 0x6AA3);
            wifi_escribir32_mascara(REG_TXF3, MASKHWORD, 0xAA7B);
            wifi_escribir32_mascara(REG_TXF4, MASKLWORD, 0xF3D7);
            wifi_escribir32_mascara(REG_TXF5, MASKDWORD, 0x0);
            wifi_escribir32_mascara(REG_TXF6, MASKDWORD, 0xFF012455);
            wifi_escribir32_mascara(REG_TXF7, MASKDWORD, 0xFFFF);
        } else {
            wifi_escribir32_mascara(REG_TXF0, MASKHWORD, 0x5284);
            wifi_escribir32_mascara(REG_TXF1, MASKDWORD, 0x3E18FEC8);
            wifi_escribir32_mascara(REG_TXF2, MASKLWORD, 0x0A88);
            wifi_escribir32_mascara(REG_TXF3, MASKHWORD, 0xACC4);
            wifi_escribir32_mascara(REG_TXF4, MASKLWORD, 0xC8B2);
            wifi_escribir32_mascara(REG_TXF5, MASKDWORD, 0x00FAF0DE);
            wifi_escribir32_mascara(REG_TXF6, MASKDWORD, 0x00122344);
            wifi_escribir32_mascara(REG_TXF7, MASKDWORD, 0x0FFFFFFF);
        }
        wifi_escribir32_mascara(REG_TXDFIR0, 0x70, c == 13 ? 0x3 : 0x1);
    } else if (wifi_ch_5g(c)) {
        wifi_poner32(REG_CCKTXONLY, BIT_BB_CCK_CHECK_EN);
        wifi_poner32(REG_CCK_CHECK, BIT_CHECK_CCK_EN);
        wifi_poner32(REG_BGCTRL, BITS_RX_IQ_WEIGHT);
        wifi_quitar32(REG_TXF4, 1u << 20);
        wifi_escribir32_mascara(REG_CCAMSK, 0x3F000000, 0x22);
        wifi_escribir32_mascara(REG_TXDFIR0, 0x70, 0x3);
        uint32_t agc = (c >= 36 && c <= 64) ? 0x1 : (c >= 100 && c <= 144) ? 0x2 : 0x3;   /* BAND_1|2, 3, 4 */
        wifi_escribir32_mascara(REG_RXAGCCTL0, BITS_RXAGC_OFDM, agc);
        wifi_escribir32_mascara(REG_RXAGCCTL, BITS_RXAGC_OFDM, agc);
        if (c >= 36 && c <= 51)        wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x494);
        else if (c >= 52 && c <= 55)   wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x493);
        else if (c >= 56 && c <= 111)  wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x453);
        else if (c >= 112 && c <= 119) wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x452);
        else if (c >= 120 && c <= 172) wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x412);
        else if (c >= 173 && c <= 177) wifi_escribir32_mascara(REG_SCOTRK, 0xFFF, 0x411);
    }
    if (bw == RTW_CHANNEL_WIDTH_80) {             /* case RTW_CHANNEL_WIDTH_80 */
        wifi_escribir32_mascara(REG_TXBWCTL, 0xF, 0xA);
        wifi_escribir32_mascara(REG_TXBWCTL, 0xC0, 0x0);
        wifi_escribir32_mascara(REG_TXBWCTL, 0xFF00, (uint32_t)(idx | (idx << 4)));
        wifi_escribir32_mascara(REG_SBD, BITS_SUBTUNE, 0x6);
        wifi_escribir32_mascara(REG_PT_CHSMO, BIT_PT_OPT, 0x1);
        return;
    }
    /* case RTW_CHANNEL_WIDTH_20 */
    wifi_escribir32_mascara(REG_DFIRBW, 0x3FF0, 0x19B);
    wifi_escribir32_mascara(REG_TXBWCTL, 0xF, 0x0);
    wifi_escribir32_mascara(REG_TXBWCTL, 0xFFC0, 0x0);
    wifi_escribir32_mascara(REG_TXCLK, 0x700, 0x7);
    wifi_escribir32_mascara(REG_TXCLK, 0x700000, 0x6);
    wifi_escribir32_mascara(REG_CCK_SOURCE, BIT_NBI_EN, 0x0);
    wifi_escribir32_mascara(REG_SBD, BITS_SUBTUNE, 0x1);
    wifi_escribir32_mascara(REG_PT_CHSMO, BIT_PT_OPT, 0x0);
}

/* rtw_set_channel_mac(channel, bw, primary_ch_idx). H176: 20 u 80 MHz. */
static void wifi_canal_mac(uint8_t c, uint8_t bw, uint8_t idx) {
    uint8_t sc = (uint8_t)TXSC_20M_0;
    uint32_t rfmod = 0;
    if (bw == RTW_CHANNEL_WIDTH_80) {
        sc = idx == RTW_SC_20_UPPER ? (uint8_t)TXSC_80M_SC20_UPPER : idx == RTW_SC_20_UPMOST ? (uint8_t)TXSC_80M_SC20_UPMOST :
             idx == RTW_SC_20_LOWER ? (uint8_t)TXSC_80M_SC20_LOWER : (uint8_t)TXSC_80M_SC20_LOWEST;
        rfmod = BIT_RFMOD_80M;
    }
    wifi_escribir8(REG_DATA_SC, sc);
    wifi_escribir32(REG_WMAC_TRXPTCL_CTL, (wifi_leer32(REG_WMAC_TRXPTCL_CTL) & ~BIT_RFMOD) | rfmod);
    uint32_t v = wifi_leer32(REG_AFE_CTRL1) & ~BIT_MAC_CLK_SEL;               /* WCPU_11AC */
    v |= (MAC_CLK_HW_DEF_80M << BIT_SHIFT_MAC_CLK_SEL);
    wifi_escribir32(REG_AFE_CTRL1, v);
    wifi_escribir8(REG_USTIME_TSF, MAC_CLK_SPEED);
    wifi_escribir8(REG_USTIME_EDCA, MAC_CLK_SPEED);
    uint8_t v8 = (uint8_t)(wifi_leer8(REG_CCK_CHECK) & ~BIT_CHECK_CCK_EN);
    if (wifi_ch_5g(c)) v8 |= (uint8_t)BIT_CHECK_CCK_EN;
    wifi_escribir8(REG_CCK_CHECK, v8);
}

/* rtw8822c_rstb_3wire(). */
static void wifi_rstb_3wire(int activar) {
    if (activar) {
        wifi_escribir32_mascara(REG_RSTB, BIT_RSTB_3WIRE, 0x1);
        wifi_escribir32_mascara(REG_ANAPAR_A, BIT_ANAPAR_UPDATE, 0x1);
        wifi_escribir32_mascara(REG_ANAPAR_B, BIT_ANAPAR_UPDATE, 0x1);
    } else {
        wifi_escribir32_mascara(REG_RSTB, BIT_RSTB_3WIRE, 0x0);
    }
}

/* rtw8822c_set_channel_rf(). H176: 20 u 80 MHz. */
static void wifi_canal_rf(uint8_t c, uint8_t bw) {
    const uint32_t BANDA = (1u << 16) | (1u << 9) | (1u << 8), B5G = (1u << 16) | (1u << 8);
    const uint32_t RFSI = (1u << 18) | (1u << 17), BW = (1u << 13) | (1u << 12), BW20 = (1u << 13) | (1u << 12);
    uint32_t r18 = wifi_rf_leer_mascara(0, 0x18, RFREG_MASK);
    r18 &= ~(BANDA | MASKBYTE0 | RFSI | BW);
    r18 |= c <= 14 ? 0 : B5G;
    r18 |= c & MASKBYTE0;
    if (c >= 149 && c <= 177) r18 |= 1u << 18;                  /* RF18_RFSI_GT_CH140 (BAND_4) */
    else if (c >= 100 && c <= 144) r18 |= 1u << 17;             /* RF18_RFSI_GE_CH80 (BAND_3) */
    r18 |= bw == RTW_CHANNEL_WIDTH_80 ? (1u << 12) : BW20;              /* RF18_BW_80M : RF18_BW_20M */
    uint32_t rxbb = bw == RTW_CHANNEL_WIDTH_80 ? 0x8 : 0x18;
    wifi_rstb_3wire(0);
    for (uint32_t ruta = 0; ruta < 2; ruta++) {
        wifi_rf_escribir_mascara(ruta, RF_LUTWE2, 0x04, 0x01);
        wifi_rf_escribir_mascara(ruta, RF_LUTWA, 0x1F, 0x12);
        wifi_rf_escribir_mascara(ruta, RF_LUTWD0, 0xFFFFF, rxbb);
        wifi_rf_escribir_mascara(ruta, RF_LUTWE2, 0x04, 0x00);
    }
    wifi_rf_escribir_mascara(0, RF_CFGCH, RFREG_MASK, r18);
    wifi_rf_escribir_mascara(1, RF_CFGCH, RFREG_MASK, r18);
    wifi_rstb_3wire(1);
}

/* rtw_set_channel() con rtw_update_channel(): c = canal central, bw, idx = RTW_SC_* del primario. */
static void wifi_fijar_canal_ancho(uint8_t c, uint8_t bw, uint8_t idx) {
    if (!g_wifi.radio_activa) return;
    wifi_canal_bb(c, bw, idx);            /* rtw8822c_set_channel() */
    wifi_canal_mac(c, bw, idx);
    wifi_canal_rf(c, bw);
    wifi_toggle_igi();
    g_wifi.canal = c;
    g_wifi.ancho = bw;
    wifi_potencia_tx(c);                  /* rtw_phy_set_tx_power_level(center_chan) */
}

void wifi_fijar_canal(uint8_t c) { RED_EXCLUSIVA(); wifi_fijar_canal_ancho(c, RTW_CHANNEL_WIDTH_20, RTW_SC_DONT_CARE); }

/* rtw_update_channel(): subcanal primario dentro de 80 MHz (frecuencias en MHz = 5 * canal). */
static uint8_t wifi_idx_primario_80(uint8_t primario, uint8_t centro) {
    if (primario > centro) return (primario - centro) * 5 == 10 ? RTW_SC_20_UPPER : RTW_SC_20_UPMOST;
    return (centro - primario) * 5 == 10 ? RTW_SC_20_LOWER : RTW_SC_20_LOWEST;
}

/* ---------------------------------------------------------------- RX */
static int g_sonda_activa;                /* H161 */
static uint8_t g_sonda_bssid[6];
static struct wifi_red g_redes[WIFI_MAX_REDES];
static uint32_t g_n_redes;
static uint8_t g_cck_l, g_cck_u;          /* dm_info->cck_gi_l/u_bnd (phy_set_param) */

static void wifi_registrar_red(const uint8_t *f, uint32_t n, int8_t rssi) {
    if (n < 24 + 12) return;
    const uint8_t *bssid = f + 16;
    uint16_t cap = (uint16_t)(f[34] | (f[35] << 8));
    char ssid[33] = { 0 };
    uint8_t canal = g_wifi.canal, rsn = 0, wpa = 0;
    uint8_t grupo[4] = { 0 }, ccmp = 0, psk = 0, sae = 0, ie[40], ie_l = 0;
    uint16_t caps = 0;
    uint8_t rsnx = 0, rsnx_visto = 0;
    uint8_t ht = 0, vht = 0, vht_ancho = 0, vht_centro = 0, wmm = 0;   /* H176 */
    for (uint32_t p = 36; p + 2 <= n; ) {   /* IEs */
        uint8_t id = f[p], l = f[p + 1];
        if (p + 2 + l > n) break;
        const uint8_t *d = f + p + 2;
        if (id == 0 && l <= 32) { memcpy(ssid, d, l); ssid[l] = 0; }
        else if (id == 3 && l == 1) canal = d[0];
        else if (id == 48 && l >= 8) {        /* RSN: version, grupo, pairwise[], AKM[], caps */
            rsn = 1;
            ie_l = l > sizeof(ie) ? (uint8_t)sizeof(ie) : l;
            memcpy(ie, d, ie_l);
            memcpy(grupo, d + 2, 4);
            uint32_t q = 6, np = (uint32_t)(d[q] | (d[q + 1] << 8)); q += 2;
            for (uint32_t i = 0; i < np && q + 4 <= l; i++, q += 4)
                if (d[q] == 0x00 && d[q + 1] == 0x0F && d[q + 2] == 0xAC && d[q + 3] == 4) ccmp = 1;
            if (q + 2 <= l) {
                uint32_t na = (uint32_t)(d[q] | (d[q + 1] << 8)); q += 2;
                for (uint32_t i = 0; i < na && q + 4 <= l; i++, q += 4) {
                    if (d[q] == 0x00 && d[q + 1] == 0x0F && d[q + 2] == 0xAC && d[q + 3] == 2) psk = 1;
                    if (d[q] == 0x00 && d[q + 1] == 0x0F && d[q + 2] == 0xAC && d[q + 3] == 8) sae = 1;
                }
            }
            if (q + 2 <= l) caps = (uint16_t)(d[q] | (d[q + 1] << 8));
        }
        else if (id == 221 && l >= 4 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xF2 && d[3] == 0x01) wpa = 1;
        else if (id == 244 && l >= 1) { rsnx = d[0]; rsnx_visto = 1; }   /* H164: RSNXE */
        else if (id == 45) ht = 1;                                            /* H176: HT Capabilities */
        else if (id == 191) vht = 1;                                          /* VHT Capabilities */
        else if (id == 192 && l >= 3) { vht_ancho = d[0]; vht_centro = d[1]; }   /* VHT Operation */
        else if (id == 221 && l >= 5 && d[0] == 0x00 && d[1] == 0x50 && d[2] == 0xF2 && d[3] == 0x02 && d[4] <= 1) wmm = 1;
        p += 2 + l;
    }
    for (uint32_t i = 0; i < g_n_redes; i++) {
        if (memcmp(g_redes[i].bssid, bssid, 6) == 0) {
            if (rssi > g_redes[i].rssi) g_redes[i].rssi = rssi;
            if (g_redes[i].vistas < 0xFFFF) g_redes[i].vistas++;
            return;
        }
    }
    if (g_n_redes >= WIFI_MAX_REDES) return;
    struct wifi_red *r = &g_redes[g_n_redes++];
    memcpy(r->bssid, bssid, 6);
    memcpy(r->ssid, ssid, sizeof(ssid));
    r->canal = canal;
    r->rssi = rssi;
    r->privada = (cap >> 4) & 1;
    r->rsn = rsn;
    r->wpa = wpa;
    r->vistas = 1;
    memcpy(r->rsn_grupo, grupo, 4);
    r->rsn_ccmp = ccmp; r->rsn_psk = psk; r->rsn_sae = sae; r->rsn_caps = caps;
    memcpy(r->rsn_ie, ie, ie_l); r->rsn_ie_largo = ie_l;
    r->rsnx = rsnx; r->rsnx_visto = rsnx_visto;
    r->ht = ht; r->vht = vht; r->vht_ancho = vht_ancho; r->vht_centro = vht_centro; r->wmm = wmm;
}
static const struct wifi_red *g_red_fallida;   /* H163: para diagnosticar el RSN IE */

/* H163: buzon de tramas dirigidas a nosotros desde el BSSID con el que se conecta. */
static int g_sta_escucha;
static uint8_t g_sta_bssid[6];
static uint8_t g_buzon[1024];
static uint32_t g_buzon_largo;
static int g_buzon_tipo;               /* 1 auth, 2 assoc resp, 3 EAPOL, 4 deauth/disassoc */
static void wifi_buzon(const uint8_t *f, uint32_t n) {
    if (!g_sta_escucha || n < 24 || memcmp(f + 4, g_wifi.mac, 6) || memcmp(f + 10, g_sta_bssid, 6)) return;
    uint16_t fc = (uint16_t)(f[0] | (f[1] << 8));
    uint8_t tipo = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
    int t = 0;
    if (tipo == 0 && sub == 11) t = 1;
    else if (tipo == 0 && (sub == 1 || sub == 3)) t = 2;
    else if (tipo == 0 && (sub == 12 || sub == 10)) t = 4;
    else if (tipo == 2) {
        uint32_t h = 24 + ((sub & 8) ? 2 : 0) + ((fc & 0x8000) ? 4 : 0);
        static const uint8_t llc[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };
        if (n >= h + 8 + 4 && memcmp(f + h, llc, 8) == 0 && !(fc & 0x4000)) t = 3;   /* sin cifrar */
    }
    if (!t || n > sizeof(g_buzon)) return;
    memcpy(g_buzon, f, n);
    g_buzon_largo = n;
    g_buzon_tipo = t;
}

/* query_phy_status(): potencia de la ruta A en dBm (pagina 0 CCK con correccion de ganancia, 1 OFDM). */
static int8_t wifi_rssi(const uint8_t *phy) {
    uint32_t w0 = (uint32_t)phy[0] | ((uint32_t)phy[1] << 8) | ((uint32_t)phy[2] << 16) | ((uint32_t)phy[3] << 24);
    uint8_t pagina = phy[0] & 0xF;
    int8_t p = (int8_t)(uint8_t)((w0 >> 8) & 0xFF);         /* PWDB_A, guardado en s8 como rtw88 */
    if (pagina == 0) {
        uint8_t g = (uint8_t)((w0 >> 16) & 0x3F);            /* GAIN_A */
        if (g < g_cck_l) p = (int8_t)(p + ((g_cck_l - g) << 1));
        else if (g > g_cck_u) p = (int8_t)(p - ((g - g_cck_u) << 1));
    }
    return (int8_t)(p - 110);
}

/* rtw_pci_rx_napi() + rtw8822c_query_rx_desc(). Devuelve tramas procesadas. */
static void wifi_rx_datos(const uint8_t *f, uint32_t n);   /* H165 */
static void wifi_rx_gestion_enlace(const uint8_t *f, uint32_t n);
static uint32_t wifi_rx_vaciar_ex(int analizar);
static int g_rx_en_curso;
static int g_rx_hw;                       /* H179: la trama en curso ya viene descifrada por el hardware */
static uint32_t g_rx_max_pend, g_eth_max;   /* H182 */
static uint32_t g_cam_icv;
static uint64_t g_ciclos_rx, g_ciclos_ccmp_rx, g_ciclos_tx;   /* H178: perfilado con rdtsc */
void wifi_ciclos(uint64_t *rx, uint64_t *ccmp_rx, uint64_t *tx) { *rx = g_ciclos_rx; *ccmp_rx = g_ciclos_ccmp_rx; *tx = g_ciclos_tx; }
static uint32_t wifi_rx_vaciar(int analizar) {
    if (g_rx_en_curso) return 0;                  /* H165: la pila puede reentrar desde la consola */
    g_rx_en_curso = 1;
    uint64_t c0 = rdtsc();
    uint32_t r = wifi_rx_vaciar_ex(analizar);
    g_ciclos_rx += rdtsc() - c0;
    g_rx_en_curso = 0;
    return r;
}
static uint32_t wifi_rx_vaciar_ex(int analizar) {
    uint32_t hw = (wifi_leer32(RTK_PCI_RXBD_IDX_MPDUQ) & TRX_BD_HW_IDX_MASK) >> 16;
    uint32_t n = hw >= g_rx_rp ? hw - g_rx_rp : g_rx_n - (g_rx_rp - hw);
    if (n > g_rx_max_pend) g_rx_max_pend = n;                     /* H182: ocupacion maxima del anillo */
    uint32_t hechas = 0;
    struct wifi_bd_rx *bd = (struct wifi_bd_rx *)g_anillo_rx;
    while (n--) {
        dma_sincronizar_dispositivo_a_cpu(&bd[g_rx_rp], sizeof(bd[0]));
        if (bd[g_rx_rp].total_pkt_size != g_rx_tag) g_wifi.rx_tag_mal++;     /* rtw_pci_dma_check */
        g_rx_tag = (g_rx_tag + 1) % 8192;                                    /* RX_TAG_MAX */
        uint8_t *buf = g_rx_bufs + g_rx_rp * WIFI_RX_PASO;
        dma_sincronizar_dispositivo_a_cpu(buf, RTK_PCI_RX_BUF_SIZE);
        uint32_t w0 = *(volatile uint32_t *)(buf + 0), w2 = *(volatile uint32_t *)(buf + 8);
        uint32_t largo = w0 & 0x3FFF, drv = ((w0 >> 16) & 0xF) * 8, desp = (w0 >> 24) & 0x3;
        g_wifi.rx_tramas++;
        if (w2 & (1u << 28)) g_wifi.rx_c2h++;
        else if (w0 & (1u << 14)) g_wifi.rx_crc++;
        else if (analizar && RTW8822C_RX_PKT_DESC_SZ + drv + desp + largo <= RTK_PCI_RX_BUF_SIZE && largo >= 28) {
            const uint8_t *f = buf + RTW8822C_RX_PKT_DESC_SZ + desp + drv;
            /* H165: WLAN_RCR_CFG lleva BIT_APP_FCS (rtw88 declara RX_INCLUDES_FCS): el largo del
             * descriptor incluye los 4 bytes del FCS al final. Sin recortarlos, el MIC de CCMP
             * se buscaria 4 bytes corrido. */
            largo -= 4;
            /* H179: GET_RX_DESC_SWDEC (w0 bit 27), ENC_TYPE (w0[22:20]) e ICV_ERR (w0 bit 15). */
            g_rx_hw = !(w0 & (1u << 27)) && ((w0 >> 20) & 7) != RX_DESC_ENC_NONE;
            if (g_rx_hw && (w0 & (1u << 15))) { g_cam_icv++; goto siguiente_rx; }
            int8_t rssi = -127;
            if (w0 & (1u << 26)) rssi = wifi_rssi(buf + RTW8822C_RX_PKT_DESC_SZ + desp);
            else g_wifi.rx_sin_phy++;
            uint16_t fc = (uint16_t)(f[0] | (f[1] << 8));
            uint8_t tipo = (fc >> 2) & 3, sub = (fc >> 4) & 0xF;
            wifi_buzon(f, largo);                         /* H163 */
            if (tipo == 2) {                              /* H176: GET_RX_DESC_RX_RATE (w3[6:0]) */
                uint8_t tasa = (uint8_t)(*(volatile uint32_t *)(buf + 12) & 0x7F);
                g_wifi.rx_tasa = tasa;
                if (tasa >= DESC_RATEVHT1SS_MCS0) g_wifi.rx_vht++;
                else if (tasa >= DESC_RATEMCS0) g_wifi.rx_ht++;
            }
            if (tipo == 2) wifi_rx_datos(f, largo);       /* H165 */
            else if (tipo == 0) wifi_rx_gestion_enlace(f, largo);
            if (tipo == 0) {
                g_wifi.rx_mgmt++;
                if (sub == 8 || sub == 5) { g_wifi.rx_beacons++; wifi_registrar_red(f, largo, rssi); }
                /* H161: probe response dirigido a nosotros desde la red sondeada */
                if (sub == 5 && g_sonda_activa && memcmp(f + 4, g_wifi.mac, 6) == 0 &&
                    memcmp(f + 16, g_sonda_bssid, 6) == 0) {
                    g_wifi.sonda_respuestas++;
                    if (rssi > g_wifi.sonda_rssi) g_wifi.sonda_rssi = rssi;
                }
            }
        }
    siguiente_rx:
        g_rx_hw = 0;
        /* rtw_pci_sync_rx_desc_device(): devolver el buffer al chip */
        memset(&bd[g_rx_rp], 0, sizeof(bd[0]));
        bd[g_rx_rp].buf_size = (uint16_t)RTK_PCI_RX_BUF_SIZE;
        bd[g_rx_rp].dma = (uint32_t)(g_rx_bufs_fis + g_rx_rp * WIFI_RX_PASO);
        dma_sincronizar_cpu_a_dispositivo(&bd[g_rx_rp], sizeof(bd[0]));
        if (++g_rx_rp >= g_rx_n) g_rx_rp = 0;
        hechas++;
    }
    wifi_escribir16(RTK_PCI_RXBD_IDX_MPDUQ, (uint16_t)g_rx_rp);
    return hechas;
}

static const uint8_t g_canales[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
    149, 153, 157, 161, 165 };

int wifi_escanear(uint32_t ms_por_canal) { RED_EXCLUSIVA();
    if (!g_wifi.radio_activa) return -1;
    g_n_redes = 0;
    g_wifi.rx_tramas = g_wifi.rx_crc = g_wifi.rx_c2h = g_wifi.rx_mgmt = 0;
    g_wifi.rx_beacons = g_wifi.rx_tag_mal = g_wifi.rx_sin_phy = 0;
    /* cck_gi_u/l_bnd como en rtw8822c_phy_set_param() */
    g_cck_u = (uint8_t)((wifi_leer32_mascara(0x1A98, 0xC000) << 4) | wifi_leer32_mascara(0x1AA8, 0xF0000));
    g_cck_l = (uint8_t)((wifi_leer32_mascara(0x1A98, 0xC0) << 4) | wifi_leer32_mascara(0x1A70, 0x0F000000));
    for (uint32_t i = 0; i < sizeof(g_canales); i++) {
        wifi_fijar_canal(g_canales[i]);
        wifi_rx_vaciar(0);                       /* descartar lo del canal anterior */
        for (uint32_t t = 0; t < ms_por_canal; t += 5) {
            esperar_milisegundos(5);
            wifi_rx_vaciar(1);
        }
    }
    return (int)g_n_redes;
}

void wifi_imprimir_escaneo(void) { RED_EXCLUSIVA();
    const char *h = "0123456789ABCDEF";
    consola_imprimir("[WIFI] Escaneo pasivo: "); consola_imprimir_dec(g_n_redes);
    consola_imprimir(" redes  tramas "); consola_imprimir_dec(g_wifi.rx_tramas);
    consola_imprimir("  mgmt "); consola_imprimir_dec(g_wifi.rx_mgmt);
    consola_imprimir("  beacons "); consola_imprimir_dec(g_wifi.rx_beacons);
    consola_imprimir("  CRC "); consola_imprimir_dec(g_wifi.rx_crc);
    consola_imprimir("  C2H "); consola_imprimir_dec(g_wifi.rx_c2h);
    consola_imprimir("  tag "); consola_imprimir_dec(g_wifi.rx_tag_mal);
    consola_imprimir_linea("");
    consola_imprimir_linea("       Canal  dBm  Seguridad  BSSID              SSID");
    for (uint32_t i = 0; i < g_n_redes; i++) {
        const struct wifi_red *r = &g_redes[i];
        char linea[96]; uint32_t k = 0;
        linea[k++] = ' '; linea[k++] = ' '; linea[k++] = ' '; linea[k++] = ' '; linea[k++] = ' '; linea[k++] = ' '; linea[k++] = ' ';
        linea[k++] = (char)(r->canal >= 100 ? '0' + r->canal / 100 : ' ');
        linea[k++] = (char)(r->canal >= 10 ? '0' + (r->canal / 10) % 10 : ' ');
        linea[k++] = (char)('0' + r->canal % 10);
        linea[k++] = ' '; linea[k++] = ' ';
        int v = r->rssi; linea[k++] = v < 0 ? '-' : ' '; if (v < 0) v = -v;
        linea[k++] = (char)(v >= 100 ? '0' + v / 100 : ' ');
        linea[k++] = (char)('0' + (v / 10) % 10); linea[k++] = (char)('0' + v % 10);
        linea[k++] = ' '; linea[k++] = ' ';
        const char *seg = r->rsn ? "WPA2/3   " : r->wpa ? "WPA      " : r->privada ? "WEP      " : "abierta  ";
        for (const char *s = seg; *s; s++) linea[k++] = *s;
        linea[k++] = ' '; linea[k++] = ' ';
        for (int j = 0; j < 6; j++) { linea[k++] = h[r->bssid[j] >> 4]; linea[k++] = h[r->bssid[j] & 0xF]; if (j < 5) linea[k++] = ':'; }
        linea[k++] = ' '; linea[k++] = ' ';
        linea[k] = 0;
        consola_imprimir(linea);
        if (!r->ssid[0]) { consola_imprimir_linea("(oculta)"); continue; }
        /* SSID: solo ASCII imprimible; el resto como '?'. */
        char s[33]; uint32_t j = 0;
        for (; r->ssid[j] && j < 32; j++) s[j] = (r->ssid[j] >= 0x20 && r->ssid[j] < 0x7F) ? r->ssid[j] : '?';
        s[j] = 0;
        consola_imprimir_linea(s);
    }
}

/* ================================================================== H161
 * Potencia TX (rtw_phy_set_tx_power_level + rtw8822c_set_tx_power_index),
 * puerto STA y TX por la cola de gestion. */

static const uint8_t g_rs_tasas[6][10] = {          /* rtw_rate_section[] */
    { DESC_RATE1M, DESC_RATE2M, DESC_RATE5_5M, DESC_RATE11M },
    { DESC_RATE6M, DESC_RATE9M, DESC_RATE12M, DESC_RATE18M, DESC_RATE24M, DESC_RATE36M, DESC_RATE48M, DESC_RATE54M },
    { DESC_RATEMCS0, DESC_RATEMCS1, DESC_RATEMCS2, DESC_RATEMCS3, DESC_RATEMCS4, DESC_RATEMCS5, DESC_RATEMCS6, DESC_RATEMCS7 },
    { DESC_RATEMCS8, DESC_RATEMCS9, DESC_RATEMCS10, DESC_RATEMCS11, DESC_RATEMCS12, DESC_RATEMCS13, DESC_RATEMCS14, DESC_RATEMCS15 },
    { DESC_RATEVHT1SS_MCS0, DESC_RATEVHT1SS_MCS1, DESC_RATEVHT1SS_MCS2, DESC_RATEVHT1SS_MCS3, DESC_RATEVHT1SS_MCS4,
      DESC_RATEVHT1SS_MCS5, DESC_RATEVHT1SS_MCS6, DESC_RATEVHT1SS_MCS7, DESC_RATEVHT1SS_MCS8, DESC_RATEVHT1SS_MCS9 },
    { DESC_RATEVHT2SS_MCS0, DESC_RATEVHT2SS_MCS1, DESC_RATEVHT2SS_MCS2, DESC_RATEVHT2SS_MCS3, DESC_RATEVHT2SS_MCS4,
      DESC_RATEVHT2SS_MCS5, DESC_RATEVHT2SS_MCS6, DESC_RATEVHT2SS_MCS7, DESC_RATEVHT2SS_MCS8, DESC_RATEVHT2SS_MCS9 } };
static const uint8_t g_rs_tam[6] = { 4, 8, 8, 8, 10, 10 };     /* rtw_rate_size[] */

static int8_t  g_off_2g[2][DESC_RATE_MAX], g_off_5g[2][DESC_RATE_MAX];   /* tx_pwr_by_rate_offset_* */
static int8_t  g_base_2g[2][RTW_RATE_SECTION_MAX], g_base_5g[2][RTW_RATE_SECTION_MAX];
static int8_t  g_lim_2g[RTW_CHANNEL_WIDTH_40 + 1][RTW_RATE_SECTION_MAX][RTW_MAX_CHANNEL_NUM_2G];   /* solo REGD_WW */
static int8_t  g_lim_5g[RTW_CHANNEL_WIDTH_40 + 1][RTW_RATE_SECTION_MAX][RTW_MAX_CHANNEL_NUM_5G];
static uint8_t g_pwr_tbl[2][DESC_RATE_MAX];                                  /* hal->tx_pwr_tbl */
static int     g_potencia_lista;

static const uint8_t g_canales_5g[RTW_MAX_CHANNEL_NUM_5G] = {                 /* rtw_channel_idx_5g */
    36, 38, 40, 42, 44, 46, 48, 52, 54, 56, 58, 60, 62, 64, 100, 102, 104, 106, 108, 110, 112,
    116, 118, 120, 122, 124, 126, 128, 132, 134, 136, 138, 140, 142, 144,
    149, 151, 153, 155, 157, 159, 161, 165, 167, 169, 171, 173, 175, 177 };

static int wifi_canal_idx(int banda5g, uint8_t c) {                         /* rtw_channel_to_idx */
    if (!banda5g) return (c >= 1 && c <= RTW_MAX_CHANNEL_NUM_2G) ? c - 1 : -1;
    for (int i = 0; i < (int)RTW_MAX_CHANNEL_NUM_5G; i++) if (g_canales_5g[i] == c) return i;
    return -1;
}

/* rtw_phy_get_rate_values_of_txpwr_by_rate(): direccion -> tasas (is_pwr_by_rate_dec = false). */
static uint32_t wifi_tasas_de_dir(uint32_t dir, uint32_t mascara, uint32_t val, uint8_t *t, uint8_t *p) {
    static const uint8_t grupo[][4] = {
        { DESC_RATE1M, DESC_RATE2M, DESC_RATE5_5M, DESC_RATE11M },        /* 0x20 */
        { DESC_RATE6M, DESC_RATE9M, DESC_RATE12M, DESC_RATE18M },         /* 0x24 */
        { DESC_RATE24M, DESC_RATE36M, DESC_RATE48M, DESC_RATE54M },       /* 0x28 */
        { DESC_RATEMCS0, DESC_RATEMCS1, DESC_RATEMCS2, DESC_RATEMCS3 },   /* 0x2C */
        { DESC_RATEMCS4, DESC_RATEMCS5, DESC_RATEMCS6, DESC_RATEMCS7 },   /* 0x30 */
        { DESC_RATEMCS8, DESC_RATEMCS9, DESC_RATEMCS10, DESC_RATEMCS11 }, /* 0x34 */
        { DESC_RATEMCS12, DESC_RATEMCS13, DESC_RATEMCS14, DESC_RATEMCS15 },               /* 0x38 */
        { DESC_RATEVHT1SS_MCS0, DESC_RATEVHT1SS_MCS1, DESC_RATEVHT1SS_MCS2, DESC_RATEVHT1SS_MCS3 },   /* 0x3C */
        { DESC_RATEVHT1SS_MCS4, DESC_RATEVHT1SS_MCS5, DESC_RATEVHT1SS_MCS6, DESC_RATEVHT1SS_MCS7 },   /* 0x40 */
        { DESC_RATEVHT1SS_MCS8, DESC_RATEVHT1SS_MCS9, DESC_RATEVHT2SS_MCS0, DESC_RATEVHT2SS_MCS1 },   /* 0x44 */
        { DESC_RATEVHT2SS_MCS2, DESC_RATEVHT2SS_MCS3, DESC_RATEVHT2SS_MCS4, DESC_RATEVHT2SS_MCS5 },   /* 0x48 */
        { DESC_RATEVHT2SS_MCS6, DESC_RATEVHT2SS_MCS7, DESC_RATEVHT2SS_MCS8, DESC_RATEVHT2SS_MCS9 } }; /* 0x4C */
    uint32_t fam = dir & ~0xFFu, off = dir & 0xFF;
    (void)mascara;
    if ((fam == 0xC00 || fam == 0xE00 || fam == 0x1800 || fam == 0x1A00) && off >= 0x20 && off <= 0x4C && !(off & 3)) {
        const uint8_t *g = grupo[(off - 0x20) / 4];
        for (int i = 0; i < 4; i++) { t[i] = g[i]; p[i] = (uint8_t)((val >> (i * 8)) & 0xFF); }
        return 4;
    }
    /* Otras familias (0x830.., 0xE00-0xE1C, 0x86C, 0xCD8-0xCE8) no aparecen en bb_pg_type0 (se cuentan abajo). */
    return 0;
}

/* rtw_parse_tbl_bb_pg + rtw_phy_tx_power_by_rate_config + rtw_parse_tbl_txpwr_lmt (WW) +
 * rtw_xref_txpwr_lmt + rtw_phy_tx_power_limit_config. Una sola vez: son datos fijos. */
static uint32_t g_bb_pg_ignoradas;
static void wifi_potencia_tablas(void) {
    memset(g_off_2g, 0, sizeof(g_off_2g)); memset(g_off_5g, 0, sizeof(g_off_5g));
    g_bb_pg_ignoradas = 0;
    for (uint32_t i = 0; i < RTW8822C_BB_PG_TYPE0_FILAS; i++) {
        const uint32_t *f = rtw8822c_bb_pg_type0[i];       /* band, rf_path, tx_num, addr, bitmask, data */
        if (f[3] == 0xFE || f[3] == 0xFFE) continue;       /* msleep(50) en rtw88: sin efecto en datos */
        uint8_t t[4], p[4];
        uint32_t n = wifi_tasas_de_dir(f[3], f[4], f[5], t, p);
        if (!n || f[1] >= 2 || f[0] > 1) { g_bb_pg_ignoradas++; continue; }
        for (uint32_t k = 0; k < n; k++) {
            if (f[0] == 0) g_off_2g[f[1]][t[k]] = (int8_t)p[k];
            else           g_off_5g[f[1]][t[k]] = (int8_t)p[k];
        }
    }
    for (int ruta = 0; ruta < 2; ruta++)                    /* tx_power_by_rate_config */
        for (int rs = 0; rs < (int)RTW_RATE_SECTION_MAX; rs++) {
            uint8_t b = rs >= (int)RTW_RATE_SECTION_VHT_1S ? g_rs_tasas[rs][g_rs_tam[rs] - 3] : g_rs_tasas[rs][g_rs_tam[rs] - 1];
            g_base_2g[ruta][rs] = g_off_2g[ruta][b];
            g_base_5g[ruta][rs] = g_off_5g[ruta][b];
            for (int k = 0; k < g_rs_tam[rs]; k++) {
                g_off_2g[ruta][g_rs_tasas[rs][k]] = (int8_t)(g_off_2g[ruta][g_rs_tasas[rs][k]] - g_base_2g[ruta][rs]);
                g_off_5g[ruta][g_rs_tasas[rs][k]] = (int8_t)(g_off_5g[ruta][g_rs_tasas[rs][k]] - g_base_5g[ruta][rs]);
            }
        }
    memset(g_lim_2g, 0x7F, sizeof(g_lim_2g)); memset(g_lim_5g, 0x7F, sizeof(g_lim_5g));   /* init_tx_power_limit */
    for (uint32_t i = 0; i < RTW8822C_TXPWR_LMT_TYPE0_FILAS; i++) {
        const int16_t *f = rtw8822c_txpwr_lmt_type0[i];     /* regd, band, bw, rs, ch, txpwr_lmt */
        int8_t lmt = (int8_t)f[5];
        if (lmt < -0x7F) lmt = -0x7F;                       /* clamp_t(s8, -max, max) */
        if (f[2] > (int)RTW_CHANNEL_WIDTH_40 || f[3] >= (int)RTW_RATE_SECTION_MAX) continue;   /* bw80: no usado a 20 MHz */
        int idx = wifi_canal_idx(f[1] == 1, (uint8_t)f[4]);
        if (idx < 0) continue;
        int8_t *ww = f[1] == 0 ? &g_lim_2g[f[2]][f[3]][idx] : &g_lim_5g[f[2]][f[3]][idx];
        if (lmt < *ww) *ww = lmt;                           /* tx_pwr_limit_*[RTW_REGD_WW] = min */
    }
    for (int bw = 0; bw <= (int)RTW_CHANNEL_WIDTH_40; bw++)                 /* rtw_xref_txpwr_lmt (WW) */
        for (int c = 0; c < (int)RTW_MAX_CHANNEL_NUM_5G; c++)
            for (int k = 0; k < 2; k++) {
                int rs_ht = k ? RTW_RATE_SECTION_HT_2S : RTW_RATE_SECTION_HT_1S;
                int rs_vht = k ? RTW_RATE_SECTION_VHT_2S : RTW_RATE_SECTION_VHT_1S;
                int8_t *ht = &g_lim_5g[bw][rs_ht][c], *vht = &g_lim_5g[bw][rs_vht][c];
                if (*ht == *vht) continue;
                if (*ht == 0x7F) *ht = *vht; else if (*vht == 0x7F) *vht = *ht;
            }
    for (int bw = 0; bw <= (int)RTW_CHANNEL_WIDTH_40; bw++)                 /* tx_power_limit_config: base ruta 0 */
        for (int rs = 0; rs < (int)RTW_RATE_SECTION_MAX; rs++) {
            for (int c = 0; c < (int)RTW_MAX_CHANNEL_NUM_2G; c++) g_lim_2g[bw][rs][c] = (int8_t)(g_lim_2g[bw][rs][c] - g_base_2g[0][rs]);
            for (int c = 0; c < (int)RTW_MAX_CHANNEL_NUM_5G; c++) g_lim_5g[bw][rs][c] = (int8_t)(g_lim_5g[bw][rs][c] - g_base_5g[0][rs]);
        }
    g_potencia_lista = 1;
}

static int wifi_rs_de_tasa(uint8_t t) {                    /* rtw_phy_rate_to_rate_section */
    if (t <= DESC_RATE11M) return RTW_RATE_SECTION_CCK;
    if (t <= DESC_RATE54M) return RTW_RATE_SECTION_OFDM;
    if (t >= DESC_RATEMCS0 && t <= DESC_RATEMCS7) return RTW_RATE_SECTION_HT_1S;
    if (t >= DESC_RATEMCS8 && t <= DESC_RATEMCS15) return RTW_RATE_SECTION_HT_2S;
    if (t >= DESC_RATEVHT1SS_MCS0 && t <= DESC_RATEVHT1SS_MCS9) return RTW_RATE_SECTION_VHT_1S;
    if (t >= DESC_RATEVHT2SS_MCS0 && t <= DESC_RATEVHT2SS_MCS9) return RTW_RATE_SECTION_VHT_2S;
    return RTW_RATE_SECTION_MAX;
}
static int8_t s4(uint8_t v) { return (int8_t)((v & 8) ? (v | 0xF0) : (v & 0x0F)); }   /* s8 campo:4 */

/* rtw_get_channel_group(). */
static uint8_t wifi_grupo(uint8_t c, uint8_t tasa) {
    if (c <= 2 || (c >= 36 && c <= 42)) return 0;
    if (c <= 5 || (c >= 44 && c <= 50)) return 1;
    if (c <= 8 || (c >= 52 && c <= 58)) return 2;
    if (c <= 11 || (c >= 60 && c <= 64)) return 3;
    if (c <= 13 || (c >= 100 && c <= 106)) return 4;
    if (c == 14) return tasa <= DESC_RATE11M ? 5 : 4;
    if (c >= 108 && c <= 114) return 5;
    if (c >= 116 && c <= 122) return 6;
    if (c >= 124 && c <= 130) return 7;
    if (c >= 132 && c <= 138) return 8;
    if (c >= 140 && c <= 144) return 9;
    if (c >= 149 && c <= 155) return 10;
    if (c >= 157 && c <= 161) return 11;
    if (c >= 165 && c <= 171) return 12;
    return 13;
}

/* rtw_phy_get_tx_power_index(ruta, tasa, BW20, canal, RTW_REGD_WW). efuse.txpwr_idx_table[ruta] en el
 * mapa logico 0x10 + ruta * 0x2A (struct rtw_txpwr_idx: 2G de 18 B, 5G de 24 B). */
static uint8_t wifi_indice_potencia(int ruta, uint8_t t, uint8_t c, uint8_t *base_o, int8_t *off_o, int8_t *lim_o) {
    const uint8_t *e = g_efuse_logico + 0x10 + ruta * 0x2A;
    uint8_t grupo = wifi_grupo(c, t), base;
    int mcs = (t >= DESC_RATEMCS0 && t <= DESC_RATEMCS15) || (t >= DESC_RATEVHT1SS_MCS0 && t <= DESC_RATEVHT2SS_MCS9);
    int dos = (t >= DESC_RATEMCS8 && t <= DESC_RATEMCS15) || t >= DESC_RATEVHT2SS_MCS0;
    int banda5g = c > 14;
    int8_t off;
    if (!banda5g) {                                        /* rtw_phy_get_2g_tx_power_index */
        base = t <= DESC_RATE11M ? e[grupo] : e[6 + grupo];
        if (t >= DESC_RATE6M && t <= DESC_RATE54M) base = (uint8_t)(base + s4(e[11]) * 2);             /* ht_1s.ofdm */
        if (mcs) { base = (uint8_t)(base + s4(e[11] >> 4) * 2); if (dos) base = (uint8_t)(base + s4(e[12]) * 2); }  /* bw20 */
        off = g_off_2g[ruta][t];
    } else {                                               /* rtw_phy_get_5g_tx_power_index */
        base = e[18 + grupo];
        if (!mcs) base = (uint8_t)(base + s4(e[32]) * 2);                                              /* ht_1s.ofdm */
        else { base = (uint8_t)(base + s4(e[32] >> 4) * 2); if (dos) base = (uint8_t)(base + s4(e[33]) * 2); }  /* bw20 */
        off = g_off_5g[ruta][t];
    }
    int rs = wifi_rs_de_tasa(t);
    int idx = wifi_canal_idx(banda5g, c);
    int8_t lim = 0x7F;                                     /* rtw_phy_get_tx_power_limit (bw 20 para todas) */
    if (rs < (int)RTW_RATE_SECTION_MAX && idx >= 0)
        lim = banda5g ? g_lim_5g[0][rs][idx] : g_lim_2g[0][rs][idx];
    int8_t sar = 0x7F;                                     /* RTW_SAR_SOURCE_NONE */
    int8_t o = off; if (lim < o) o = lim; if (sar < o) o = sar;
    /* en_dis_dpd con dpd_ratemask = DIS_DPD_RATEALL: -6 * txgi_factor(2) */
    if (t == DESC_RATE6M || t == DESC_RATE9M || t == DESC_RATEMCS0 || t == DESC_RATEMCS1 || t == DESC_RATEMCS8 ||
        t == DESC_RATEMCS9 || t == DESC_RATEVHT1SS_MCS0 || t == DESC_RATEVHT1SS_MCS1 ||
        t == DESC_RATEVHT2SS_MCS0 || t == DESC_RATEVHT2SS_MCS1)
        o = (int8_t)(o - 12);
    uint8_t p = (uint8_t)(base + o + 0);                   /* + pwr_remnant (0 sin seguimiento termico) */
    if (p > 0x7F) p = 0x7F;                                /* max_power_index (u8, como rtw88) */
    if (base_o) *base_o = base;
    if (off_o) *off_o = off;
    if (lim_o) *lim_o = lim;
    return p;
}

/* rtw_phy_set_tx_power_level(canal) + rtw8822c_set_tx_power_index(). */
static void wifi_potencia_tx(uint8_t c) {
    if (!g_potencia_lista) wifi_potencia_tablas();
    for (int ruta = 0; ruta < 2; ruta++)
        for (int rs = c <= 14 ? RTW_RATE_SECTION_CCK : RTW_RATE_SECTION_OFDM; rs < (int)RTW_RATE_SECTION_MAX; rs++)
            for (int k = 0; k < g_rs_tam[rs]; k++)
                g_pwr_tbl[ruta][g_rs_tasas[rs][k]] = wifi_indice_potencia(ruta, g_rs_tasas[rs][k], c, 0, 0, 0);
    static const uint32_t ref_cck[2] = { 0x18A0, 0x41A0 }, ref_ofdm[2] = { 0x18E8, 0x41E8 };
    for (int ruta = 0; ruta < 2; ruta++) {                 /* set_write_tx_power_ref */
        wifi_escribir32_mascara(0x1C90, 1u << 15, 0);
        wifi_escribir32_mascara(ref_cck[ruta], 0x7F0000, g_pwr_tbl[ruta][DESC_RATE11M]);
    }
    for (int ruta = 0; ruta < 2; ruta++) {
        wifi_escribir32_mascara(0x1C90, 1u << 15, 0);
        wifi_escribir32_mascara(ref_ofdm[ruta], 0x1FC00, g_pwr_tbl[ruta][DESC_RATEMCS7]);
    }
    int8_t diff[4];
    for (int rs = 0; rs < (int)RTW_RATE_SECTION_MAX; rs++)
        for (int k = 0; k < g_rs_tam[rs]; k++) {
            uint8_t t = g_rs_tasas[rs][k];
            uint8_t ra = rs == 0 ? g_pwr_tbl[0][DESC_RATE11M] : g_pwr_tbl[0][DESC_RATEMCS7];
            uint8_t rb = rs == 0 ? g_pwr_tbl[1][DESC_RATE11M] : g_pwr_tbl[1][DESC_RATEMCS7];
            int8_t da = (int8_t)((int8_t)g_pwr_tbl[0][t] - (int8_t)ra), db = (int8_t)((int8_t)g_pwr_tbl[1][t] - (int8_t)rb);
            diff[t % 4] = da < db ? da : db;
            if (t % 4 == 3) {                              /* rtw8822c_set_tx_power_diff(t - 3) */
                uint32_t v = (uint32_t)(diff[0] & 0x7F) | ((uint32_t)(diff[1] & 0x7F) << 8) |
                             ((uint32_t)(diff[2] & 0x7F) << 16) | ((uint32_t)(diff[3] & 0x7F) << 24);
                wifi_escribir32_mascara(0x1C90, 1u << 15, 0x0);
                wifi_escribir32(0x3A00 + ((t - 3) & 0xFC), v);
            }
        }
    for (int ruta = 0; ruta < 2; ruta++) {
        g_wifi.pot_idx_6m[ruta] = wifi_indice_potencia(ruta, DESC_RATE6M, c, &g_wifi.pot_base[ruta],
                                                       &g_wifi.pot_offset_6m[ruta], &g_wifi.pot_limite_6m[ruta]);
        g_wifi.pot_idx_mcs7[ruta] = g_pwr_tbl[ruta][DESC_RATEMCS7];
        g_wifi.pot_idx_1m[ruta] = g_pwr_tbl[ruta][DESC_RATE1M];
    }
}

/* ================================================================== H162
 * Coex minimo para 5 GHz. La 8822CE es combo con antena compartida (efuse bt_setting
 * bit0): al encender, la antena es del BT y GNT_WL queda bajo, asi que el PA de WiFi
 * no transmite. Se replica lo que rtw88 deja en 5 GHz: rtw_coex_power_on_setting(),
 * rtw8822c_coex_cfg_init(), rtw_coex_set_ant_path(COEX_SET_ANT_5G) y la rama 5G de
 * rtw8822c_coex_cfg_gnt_fix(). 2.4 GHz (PTA dinamico con BT) queda pendiente. */
static inline void wifi_escribir8_mascara(uint32_t r, uint8_t m, uint8_t v) {   /* rtw_write8_mask */
    uint8_t o = wifi_leer8(r);
    wifi_escribir8(r, (uint8_t)((o & ~m) | ((uint8_t)(v << __builtin_ctz(m)) & m)));
}
static void wifi_coex_ind(uint16_t dir, uint32_t mascara, uint32_t v) {          /* rtw_coex_write_indirect_reg */
    uint32_t t = 0;
    if (wifi_ltecoex_leer(dir, &t)) return;
    t = (t & ~mascara) | ((v << __builtin_ctz(mascara)) & mascara);
    wifi_ltecoex_escribir(dir, t);
}
static uint8_t g_kt_ver;
static void wifi_coex_5g(void) {
    /* rtw_coex_power_on_setting() */
    wifi_poner8(REG_SYS_FUNC_EN, (uint8_t)(BIT_FEN_BB_GLB_RST | BIT_FEN_BB_RSTB));
    wifi_coex_ind(LTE_COEX_CTRL, BIT_LTE_COEX_EN, 0x0);           /* rtw8822c_coex_cfg_rfe_type */
    wifi_coex_ind(LTE_WL_TRX_CTRL, MASKLWORD, 0xFFFF);
    wifi_coex_ind(LTE_BT_TRX_CTRL, MASKLWORD, 0xFFFF);
    wifi_escribir8(0xFF1A, 0x0);                                   /* "red x issue" */
    wifi_escribir8_mascara(REG_PAD_CTRL1 + 2, (uint8_t)(BIT_BTGP_SPI_EN >> 16), 0);   /* coex_cfg_gnt_debug */
    wifi_escribir8_mascara(REG_PAD_CTRL1 + 3, (uint8_t)(BIT_BTGP_JTAG_EN >> 24), 0);
    wifi_escribir8_mascara(REG_GPIO_MUXCFG + 2, (uint8_t)(BIT_FSPI_EN >> 16), 0);
    wifi_escribir8_mascara(REG_PAD_CTRL1 + 1, (uint8_t)(BIT_LED1DIS >> 8), 0);
    wifi_escribir8_mascara(REG_SYS_SDIO_CTRL + 3, (uint8_t)(BIT_DBG_GNT_WL_BT >> 24), 0);
    /* rtw8822c_coex_cfg_init() */
    wifi_poner8(REG_BCN_CTRL, (uint8_t)BIT_EN_BCN_FUNCTION);
    wifi_escribir8_mascara(REG_BT_TDMA_TIME, (uint8_t)BIT_MASK_SAMPLE_RATE, 0x5);
    wifi_escribir8(REG_BT_STAT_CTRL, 0x1);
    wifi_poner32(REG_GPIO_MUXCFG, BIT_BT_PTA_EN);
    wifi_poner32(REG_GPIO_MUXCFG, BIT_PO_BT_PTA_PINS);
    wifi_poner8(REG_QUEUE_CTRL, (uint8_t)BIT_PTA_WL_TX_EN);
    wifi_quitar8(REG_QUEUE_CTRL, (uint8_t)BIT_PTA_EDCCA_EN);
    wifi_escribir16(REG_BT_COEX_V2, (uint16_t)(wifi_leer16(REG_BT_COEX_V2) | BIT_GNT_BT_POLARITY));
    wifi_quitar8(REG_DUMMY_PAGE4_V1, (uint8_t)BIT_BTCCA_CTRL);
    wifi_rf_escribir_mascara(1, RF_MODOPT, 0xFFFFF, 0x40000);
    /* rtw_coex_set_ant_path(COEX_SET_ANT_5G) */
    wifi_coex_ind(LTE_COEX_CTRL, 0xC000, COEX_GNT_SET_HW_PTA);    /* GNT_BT */
    wifi_coex_ind(LTE_COEX_CTRL, 0x0C00, COEX_GNT_SET_HW_PTA);
    wifi_coex_ind(LTE_COEX_CTRL, 0x3000, COEX_GNT_SET_SW_HIGH);   /* GNT_WL */
    wifi_coex_ind(LTE_COEX_CTRL, 0x0300, COEX_GNT_SET_SW_HIGH);
    wifi_poner8(REG_SYS_SDIO_CTRL + 3, (uint8_t)(BIT_LTE_MUX_CTRL_PATH >> 24));   /* coex_ctrl_owner(WL); sin btg_reg */
    /* rtw8822c_coex_cfg_gnt_fix(), wl_coex_mode = 5G, under_5g */
    g_kt_ver = (uint8_t)((wifi_leer8(0xF1) >> 4) & 0xF);
    uint32_t rf1 = g_kt_ver == 0 ? 0x40021 : 0x40000;
    if (g_wifi.bt & 1) rf1 |= 1u << 13;                            /* share_ant: BT en S1 */
    wifi_rf_escribir_mascara(1, 0x1, 0xFFFFF, rf1);
    wifi_escribir8_mascara(REG_ANAPAR + 2, (uint8_t)(BIT_ANAPAR_BTPS >> 16), 1);
    wifi_escribir8_mascara(REG_RSTB_SEL + 1, (uint8_t)BIT_DAC_OFF_ENABLE, 0);
    wifi_escribir8_mascara(REG_RSTB_SEL + 3, (uint8_t)BIT_DAC_OFF_ENABLE, 1);
    wifi_escribir8_mascara(REG_IGN_GNTBT4, (uint8_t)BIT_PI_IGNORE_GNT_BT, 1);
    if (g_kt_ver >= 3) {
        wifi_escribir8_mascara(REG_IGN_GNT_BT1, (uint8_t)BIT_PI_IGNORE_GNT_BT, 0);
        wifi_escribir8_mascara(REG_NOMASK_TXBT, (uint8_t)BIT_NOMASK_TXBT_ENABLE, 1);
    } else {
        wifi_escribir8_mascara(REG_IGN_GNT_BT1, (uint8_t)BIT_PI_IGNORE_GNT_BT, 1);
    }
}

/* rtw_ops_add_interface(NL80211_IFTYPE_STATION) -> rtw_vif_port_config(), puerto 0. */
static void wifi_puerto_sta(void) {
    wifi_coex_5g();                                                /* H162 */
    for (int i = 0; i < 6; i++) wifi_escribir8(0x0610 + i, g_wifi.mac[i]);   /* rtw_vif_write_addr(mac_addr) */
    wifi_escribir32_mascara(REG_CR, 0x30000, RTW_NET_NO_LINK);                /* net_type */
    wifi_escribir8(REG_BCN_CTRL, (uint8_t)BIT_EN_BCN_FUNCTION);               /* bcn_ctrl, mascara 0xff */
}

/* rtw_pci_tx_write_data() en RTW_TX_QUEUE_MGMT con rtw_tx_mgmt_pkt_info_update().
 * H165: tasa fija por trama; los datos tambien salen por esta cola (sin RA ni AMPDU). */
static int wifi_tx_trama(const uint8_t *trama, uint32_t n, uint8_t tasa);
static int wifi_tx_gestion(const uint8_t *trama, uint32_t n) {
    return wifi_tx_trama(trama, n, g_wifi.canal > 14 ? DESC_RATE6M : DESC_RATE1M);   /* tasa minima: sin basic_rates */
}
static int wifi_tx_trama(const uint8_t *trama, uint32_t n, uint8_t tasa) {
    if (n + TX_PKT_DESC > WIFI_TXM_PASO) return -1;
    uint32_t hw_rp = wifi_leer16(RTK_PCI_TXBD_IDX_MGMTQ + 2) & TRX_BD_IDX_MASK;   /* __pci_get_hw_tx_ring_rp */
    if (((g_txm_wp + 1) % WIFI_TXM_N) == hw_rp) return -2;                        /* anillo lleno */
    uint8_t *buf = g_tx_bufs + g_txm_wp * WIFI_TXM_PASO;
    uint64_t fis = g_tx_bufs_fis + g_txm_wp * WIFI_TXM_PASO;
    memset(buf, 0, TX_PKT_DESC);
    memcpy(buf + TX_PKT_DESC, trama, n);
    uint32_t *w = (uint32_t *)buf;
    int banda5g = g_wifi.canal > 14;
    uint32_t bmc = trama[4] & 1;
    w[0] = campo(RTW_TX_DESC_W0_TXPKTSIZE, n) | campo(RTW_TX_DESC_W0_OFFSET, TX_PKT_DESC) |
           campo(RTW_TX_DESC_W0_BMC, bmc) | RTW_TX_DESC_W0_LS | RTW_TX_DESC_W0_DISQSELSEQ;
    w[1] = campo(RTW_TX_DESC_W1_QSEL, TX_DESC_QSEL_MGMT) |
           campo(RTW_TX_DESC_W1_RATE_ID, banda5g ? RTW_RATEID_G : RTW_RATEID_B_20M);
    w[3] = campo(RTW_TX_DESC_W3_HW_SSN_SEL, 0) | RTW_TX_DESC_W3_USE_RATE | RTW_TX_DESC_W3_DISDATAFB;
    w[4] = campo(RTW_TX_DESC_W4_DATARATE, tasa);
    w[8] = RTW_TX_DESC_W8_EN_HWSEQ;
    dma_sincronizar_cpu_a_dispositivo(buf, TX_PKT_DESC + n);
    struct wifi_bd_tx *bd = (struct wifi_bd_tx *)(g_anillo_mgmt + g_txm_wp * TX_BUF_DESC);
    memset(bd, 0, TX_BUF_DESC);
    uint32_t total = TX_PKT_DESC + n;
    bd[0].psb_len = (uint16_t)((total - 1) / 128 + 1);
    bd[0].buf_size = TX_PKT_DESC;
    bd[0].dma = (uint32_t)fis;
    bd[1].buf_size = (uint16_t)n;
    bd[1].dma = (uint32_t)(fis + TX_PKT_DESC);
    dma_sincronizar_cpu_a_dispositivo(bd, TX_BUF_DESC);
    if (++g_txm_wp >= WIFI_TXM_N) g_txm_wp = 0;
    wifi_escribir16(RTK_PCI_TXBD_IDX_MGMTQ, (uint16_t)(g_txm_wp & TRX_BD_IDX_MASK));   /* rtw_pci_tx_kick_off_queue */
    g_wifi.tx_enviadas++;
    return 0;
}

/* Probe request dirigido (IEEE 802.11 9.3.3.9): SSID + tasas soportadas de la banda. */
static uint32_t wifi_armar_probe(uint8_t *f, const char *ssid) {
    uint32_t n = 0, l = 0;
    while (ssid[l] && l < 32) l++;
    f[n++] = 0x40; f[n++] = 0x00;                       /* FC: gestion, subtipo 4 */
    f[n++] = 0; f[n++] = 0;                             /* duracion */
    for (int i = 0; i < 6; i++) f[n++] = 0xFF;          /* addr1 difusion */
    for (int i = 0; i < 6; i++) f[n++] = g_wifi.mac[i]; /* addr2 nosotros */
    for (int i = 0; i < 6; i++) f[n++] = 0xFF;          /* addr3 BSSID comodin */
    f[n++] = 0; f[n++] = 0;                             /* secuencia: la pone el hardware (EN_HWSEQ) */
    f[n++] = 0; f[n++] = (uint8_t)l; memcpy(f + n, ssid, l); n += l;
    if (g_wifi.canal > 14) {
        static const uint8_t t5[] = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
        f[n++] = 1; f[n++] = sizeof(t5); memcpy(f + n, t5, sizeof(t5)); n += sizeof(t5);
    } else {
        static const uint8_t t2[] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
        static const uint8_t x2[] = { 0x30, 0x48, 0x60, 0x6C };
        f[n++] = 1; f[n++] = sizeof(t2); memcpy(f + n, t2, sizeof(t2)); n += sizeof(t2);
        f[n++] = 50; f[n++] = sizeof(x2); memcpy(f + n, x2, sizeof(x2)); n += sizeof(x2);
    }
    return n;
}

int wifi_sondear(const char *ssid) { RED_EXCLUSIVA();
    g_wifi.sonda_respuestas = 0;
    g_wifi.sonda_rssi = -127;
    if (!g_wifi.radio_activa) return -1;
    /* Solo a una red cuyo beacon ya se oyo (regla de "beacon hint" para canales DFS/no-IR). */
    int k = -1;
    for (uint32_t i = 0; i < g_n_redes; i++) {
        uint32_t j = 0;
        while (ssid[j] && g_redes[i].ssid[j] == ssid[j]) j++;
        if (!ssid[j] && !g_redes[i].ssid[j]) { if (k < 0 || g_redes[i].rssi > g_redes[k].rssi) k = (int)i; }
    }
    if (k < 0) return -2;
    if (g_redes[k].canal <= 14) return -4;      /* H162: coex de 2.4 GHz aun no portado */
    memcpy(g_sonda_bssid, g_redes[k].bssid, 6);
    wifi_fijar_canal(g_redes[k].canal);
    /* Salvaguarda de TAEK (no esta en rtw88): un indice > 0x5A o saturado indicaria un error en la
     * cadena efuse/bb_pg/limite; en ese caso no se transmite. */
    if (g_wifi.pot_idx_6m[0] > 0x5A || g_wifi.pot_idx_6m[1] > 0x5A) return -3;
    wifi_rx_vaciar(0);
    uint8_t trama[96];
    uint32_t n = wifi_armar_probe(trama, ssid);
    g_sonda_activa = 1;
    for (int intento = 0; intento < 3 && !g_wifi.sonda_respuestas; intento++) {
        if (wifi_tx_gestion(trama, n)) break;
        for (int t = 0; t < 60; t++) { esperar_milisegundos(5); wifi_rx_vaciar(1); }
    }
    g_sonda_activa = 0;
    g_wifi.tx_hw_rp = wifi_leer16(RTK_PCI_TXBD_IDX_MGMTQ + 2) & TRX_BD_IDX_MASK;
    return (int)g_wifi.sonda_respuestas;
}

void wifi_imprimir_sonda(void) { RED_EXCLUSIVA();
    consola_imprimir("[WIFI] Potencia TX canal "); consola_imprimir_dec(g_wifi.canal);
    consola_imprimir(" (REGD_WW): 6M A/B idx "); consola_imprimir_dec(g_wifi.pot_idx_6m[0]);
    consola_imprimir("/"); consola_imprimir_dec(g_wifi.pot_idx_6m[1]);
    consola_imprimir("  base "); consola_imprimir_dec(g_wifi.pot_base[0]);
    consola_imprimir("/"); consola_imprimir_dec(g_wifi.pot_base[1]);
    consola_imprimir("  offset "); consola_imprimir_dec((uint64_t)(int64_t)g_wifi.pot_offset_6m[0] & 0xFF);
    consola_imprimir("  limite "); consola_imprimir_dec((uint64_t)(uint8_t)g_wifi.pot_limite_6m[0]);
    consola_imprimir("  MCS7 "); consola_imprimir_dec(g_wifi.pot_idx_mcs7[0]);
    consola_imprimir("/"); consola_imprimir_dec(g_wifi.pot_idx_mcs7[1]);
    consola_imprimir("  bb_pg ignoradas "); consola_imprimir_dec(g_bb_pg_ignoradas);
    consola_imprimir_linea("");
    consola_imprimir("[WIFI] Sonda: TX enviadas "); consola_imprimir_dec(g_wifi.tx_enviadas);
    consola_imprimir("  hw rp MGMT "); consola_imprimir_dec(g_wifi.tx_hw_rp);
    consola_imprimir("  wp "); consola_imprimir_dec(g_txm_wp);
    consola_imprimir("  probe responses a nuestra MAC: "); consola_imprimir_dec(g_wifi.sonda_respuestas);
    if (g_wifi.sonda_respuestas) {
        int v = g_wifi.sonda_rssi;
        consola_imprimir("  ("); if (v < 0) { consola_imprimir("-"); v = -v; }
        consola_imprimir_dec((uint64_t)v); consola_imprimir(" dBm)");
    }
    consola_imprimir_linea("");
}

/* ================================================================== H163
 * STA: autenticacion Open System, asociacion con RSN (WPA2-PSK, CCMP) y suplicante
 * propio para el intercambio de 4 vias (IEEE 802.11-2020 12.7.6). La clave se lee de
 * /wifi.conf del USB, se usa para derivar la PMK y se borra; nunca se imprime. */
#include "wifi_wpa.h"
#include "cripto.h"

static struct wifi_sta g_sta;

static void wifi_borrar(void *p, uint32_t n) { volatile uint8_t *v = p; while (n--) *v++ = 0; }
static uint32_t strlen_seguro(const char *s) { uint32_t n = 0; while (n < 64 && s[n]) n++; return n; }
static int strcmp_seguro(const char *a, const char *b) {
    for (uint32_t i = 0; i < 64; i++) { if (a[i] != b[i]) return 1; if (!a[i]) return 0; }
    return 1;
}

/* /wifi.conf: lineas "ssid=..." y "clave=...". 0 = OK. */
static int wifi_leer_conf(char ssid[33], char clave[64]) {
    void *buf = 0; size_t n = 0; int es_dma = 0;
    if (vfs_leer_archivo_binario("/wifi.conf", &buf, &n, &es_dma) || !buf) return -1;
    const char *t = buf;
    ssid[0] = clave[0] = 0;
    for (size_t i = 0; i < n; ) {
        size_t f = i; while (f < n && t[f] != '\n' && t[f] != '\r') f++;
        if (f - i > 5 && !memcmp(t + i, "ssid=", 5) && f - i - 5 <= 32) { memcpy(ssid, t + i + 5, f - i - 5); ssid[f - i - 5] = 0; }
        if (f - i > 6 && !memcmp(t + i, "clave=", 6) && f - i - 6 <= 63) { memcpy(clave, t + i + 6, f - i - 6); clave[f - i - 6] = 0; }
        i = f + 1;
    }
    wifi_borrar(buf, (uint32_t)n);
    vfs_liberar_archivo_binario(buf, n, es_dma);
    return (ssid[0] && clave[0]) ? 0 : -2;
}

static int wifi_esperar_buzon(int tipo, uint32_t ms) {
    g_buzon_tipo = 0;
    for (uint32_t t = 0; t < ms; t += 2) {
        esperar_milisegundos(2);
        wifi_rx_vaciar(1);
        if (g_buzon_tipo == tipo || g_buzon_tipo == 4) return g_buzon_tipo;
    }
    return 0;
}

static uint32_t wifi_cabecera(uint8_t *f, uint16_t fc, const uint8_t *bssid) {
    f[0] = (uint8_t)fc; f[1] = (uint8_t)(fc >> 8); f[2] = 0; f[3] = 0;
    memcpy(f + 4, bssid, 6); memcpy(f + 10, g_wifi.mac, 6); memcpy(f + 16, bssid, 6);
    f[22] = 0; f[23] = 0;                         /* secuencia por hardware */
    return 24;
}

/* H176: modo de la asociacion. 0 = 802.11a (como H165-H175), 1 = 802.11ac (WMM, HT, VHT, 80 MHz),
 * 2 = 802.11ac + Block Ack de recepcion (agregacion A-MPDU). Comando 'wifi modo a|ac|ba'. */
static int g_wifi_modo = 2;
int wifi_modo(int m) { if (m >= 0 && m <= 2) g_wifi_modo = m; return g_wifi_modo; }

/* rtw_init_ht_cap() + rtw_init_vht_cap() (rtw88) e ieee80211_ie_build_ht_cap/vht_cap (mac80211), para el
 * 8822C con 2 rutas. Desviaciones: MPDU VHT de 3895 B y A-MSDU de 3839 (no 11454/7935), sin beamformee
 * ni HTC-VHT. SM PS deshabilitado (mac80211, IEEE80211_SMPS_OFF). Devuelve los bytes escritos. */
static uint32_t wifi_ies_ht_vht_wmm(uint8_t *d, int vht) {
    uint32_t n = 0;
    d[n++] = 45; d[n++] = 26;                                       /* HT Capabilities */
    uint16_t cap = 0x0001 | 0x0002 | 0x000C | 0x0020 | 0x0040 | 0x0080 | 0x0100 | 0x1000;
    /* LDPC | 20/40 | SM PS off | SGI20 | SGI40 | TX STBC | RX STBC 1 | DSSS/CCK40 */
    d[n++] = (uint8_t)cap; d[n++] = (uint8_t)(cap >> 8);
    d[n++] = 0x03 | (4 << 2);                                       /* A-MPDU 64K, densidad 2 us */
    uint8_t mcs[16] = { 0xFF, 0xFF, 0, 0, 0x01, 0, 0, 0, 0, 0, 0x2C, 0x01, 0x01, 0, 0, 0 };   /* MCS0-15+32, 300, TX_DEFINED */
    memcpy(d + n, mcs, 16); n += 16;
    memset(d + n, 0, 2 + 4 + 1); n += 7;                            /* ext, TxBF, ASEL */
    if (vht) {
        d[n++] = 191; d[n++] = 12;                                  /* VHT Capabilities */
        uint32_t vc = 0x00000020 | 0x00000010 | 0x00000080 | 0x00000100 | 0x03800000;
        /* SGI80 | RXLDPC | TXSTBC | RXSTBC_1 | A-MPDU 2^20 */
        d[n++] = (uint8_t)vc; d[n++] = (uint8_t)(vc >> 8); d[n++] = (uint8_t)(vc >> 16); d[n++] = (uint8_t)(vc >> 24);
        uint8_t m[8] = { 0xFA, 0xFF, 0x0C, 0x03, 0xFA, 0xFF, 0x0C, 0x03 };   /* MCS 0-9 en 2 flujos, 780 Mbit/s */
        memcpy(d + n, m, 8); n += 8;
    }
    static const uint8_t wmm[9] = { 221, 7, 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00 };   /* WMM Information, QoS info 0 */
    memcpy(d + n, wmm, 9); n += 9;
    return n;
}

/* H164: suite de cifrado de gestion de grupo del AP (tras caps y la lista de PMKID). 1 = presente. */
static int wifi_rsn_gestion(const struct wifi_red *r, uint8_t suite[4]) {
    const uint8_t *d = r->rsn_ie;
    uint32_t l = r->rsn_ie_largo, q = 6;
    if (q + 2 > l) return 0;
    q += 2 + 4u * (uint32_t)(d[q] | (d[q + 1] << 8));               /* pairwise */
    if (q + 2 > l) return 0;
    q += 2 + 4u * (uint32_t)(d[q] | (d[q + 1] << 8));               /* AKM */
    q += 2;                                                        /* caps */
    if (q + 2 > l) return 0;
    q += 2 + 16u * (uint32_t)(d[q] | (d[q + 1] << 8));              /* PMKID */
    if (q + 4 > l) return 0;
    memcpy(suite, d + q, 4);
    return 1;
}

/* Nuestro RSN IE: grupo = el del AP, pairwise CCMP, AKM PSK (2) o SAE (8).
 * H164: con PMF, caps MFPC (y MFPR si el AP lo exige) y la suite de gestion del AP. */
static uint32_t wifi_rsn_ie(uint8_t *d, const struct wifi_red *r, uint8_t akm, uint8_t mfp) {
    uint32_t n = 2;
    d[0] = 48;
    d[n++] = 1; d[n++] = 0;
    memcpy(d + n, r->rsn_grupo, 4); n += 4;
    d[n++] = 1; d[n++] = 0; d[n++] = 0x00; d[n++] = 0x0F; d[n++] = 0xAC; d[n++] = 4;
    d[n++] = 1; d[n++] = 0; d[n++] = 0x00; d[n++] = 0x0F; d[n++] = 0xAC; d[n++] = akm;
    uint16_t caps = mfp ? (uint16_t)(0x0080 | (r->rsn_caps & 0x0040)) : 0;
    d[n++] = (uint8_t)caps; d[n++] = (uint8_t)(caps >> 8);
    uint8_t suite[4];
    if (mfp && wifi_rsn_gestion(r, suite)) {
        d[n++] = 0; d[n++] = 0;                                    /* sin PMKID */
        memcpy(d + n, suite, 4); n += 4;
    }
    d[1] = (uint8_t)(n - 2);
    return n;
}

/* EAPOL-Key (802.1X-2004 + 802.11 12.7.2): desplazamientos desde el inicio de la PDU EAPOL. */
#define EK_INFO   5
#define EK_LARGO  7
#define EK_REPLAY 9
#define EK_NONCE  17
#define EK_MIC    81
#define EK_DLARGO 97
#define EK_DATOS  99

static int wifi_tx_eapol(const uint8_t *bssid, const uint8_t *eapol, uint32_t n) {
    uint8_t f[300];
    uint32_t k = wifi_cabecera(f, 0x0108, bssid);  /* datos, ToDS */
    static const uint8_t llc[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E };
    memcpy(f + k, llc, 8); k += 8;
    if (k + n > sizeof(f)) return -1;
    memcpy(f + k, eapol, n);
    return wifi_tx_gestion(f, k + n);
}

/* MIC segun la version del descriptor: 2 = HMAC-SHA1-128 (PSK), 0 = AES-128-CMAC (SAE, H164). */
static void wifi_mic(uint16_t info, const uint8_t kck[16], const uint8_t *e, uint32_t n, uint8_t mic[16]) {
    if ((info & 7) == 0) wpa_mic_cmac(kck, e, n, mic);
    else wpa_mic(kck, e, n, mic);
}

static uint32_t wifi_armar_eapol(uint8_t *e, uint8_t ver, uint16_t info, const uint8_t *replay,
                                 const uint8_t *nonce, const uint8_t *datos, uint16_t dl, const uint8_t kck[16]) {
    uint32_t n = EK_DATOS + dl;
    memset(e, 0, n);
    e[0] = ver; e[1] = 3; e[2] = (uint8_t)((n - 4) >> 8); e[3] = (uint8_t)(n - 4);
    e[4] = 2;                                      /* descriptor RSN */
    e[EK_INFO] = (uint8_t)(info >> 8); e[EK_INFO + 1] = (uint8_t)info;
    memcpy(e + EK_REPLAY, replay, 8);
    if (nonce) memcpy(e + EK_NONCE, nonce, 32);
    e[EK_DLARGO] = (uint8_t)(dl >> 8); e[EK_DLARGO + 1] = (uint8_t)dl;
    if (dl) memcpy(e + EK_DATOS, datos, dl);
    uint8_t mic[16];
    wifi_mic(info, kck, e, n, mic);
    memcpy(e + EK_MIC, mic, 16);
    return n;
}

/* Devuelve puntero a la PDU EAPOL dentro del buzon y su largo (segun su cabecera). */
static const uint8_t *wifi_eapol_del_buzon(uint32_t *largo) {
    uint16_t fc = (uint16_t)(g_buzon[0] | (g_buzon[1] << 8));
    uint8_t sub = (fc >> 4) & 0xF;
    uint32_t h = 24 + ((sub & 8) ? 2 : 0) + ((fc & 0x8000) ? 4 : 0) + 8;
    const uint8_t *e = g_buzon + h;
    if (h + 4 > g_buzon_largo) return 0;
    uint32_t n = 4 + (uint32_t)((e[2] << 8) | e[3]);
    if (h + n > g_buzon_largo || n < EK_DATOS || e[1] != 3 || e[4] != 2) return 0;
    *largo = n;
    return e;
}

/* ================================================================== H164
 * SAE (802.11-2020 12.4, grupo 19, PWE por hunting-and-pecking) en tramas Authentication
 * con algoritmo 3: commit (secuencia 1) y confirm (secuencia 2). */
static struct wpa_sae g_sae;

/* Espera una trama Authentication con la secuencia dada; descarta las demas (p. ej. commits repetidos). */
static int wifi_esperar_auth(uint16_t seq, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 20) {
        int k = wifi_esperar_buzon(1, 20);
        if (k == 4) return 4;
        if (k == 1 && g_buzon_largo >= 30 && (uint16_t)(g_buzon[26] | (g_buzon[27] << 8)) == seq) return 1;
    }
    return 0;
}

/* 0 = SAE aceptado (g_sae.pmk listo). Si no, codigo de error de wifi_imprimir_conexion. */
static int wifi_auth_sae(uint8_t *f) {
    static uint8_t token[200];
    uint32_t tl = 0, n;
    int recibido = 0;
    for (int intento = 0; intento < 5 && !recibido; intento++) {
        n = wifi_cabecera(f, 0x00B0, g_sta.bssid);
        f[n++] = 3; f[n++] = 0; f[n++] = 1; f[n++] = 0; f[n++] = 0; f[n++] = 0;   /* SAE, commit, status 0 */
        f[n++] = 19; f[n++] = 0;                                                     /* grupo 19 */
        memcpy(f + n, token, tl); n += tl;
        memcpy(f + n, g_sae.escalar, 32); n += 32;
        memcpy(f + n, g_sae.elemento, 64); n += 64;
        wifi_tx_gestion(f, n);
        int k = wifi_esperar_auth(1, 600);
        if (k == 4) return 20;
        if (k != 1) continue;
        uint16_t alg = (uint16_t)(g_buzon[24] | (g_buzon[25] << 8));
        uint16_t est = (uint16_t)(g_buzon[28] | (g_buzon[29] << 8));
        g_sta.sae_estado = est;
        g_sta.auth_estado = est;
        if (alg != 3) return 37;
        if (est == 76) {                                       /* anti-clogging: repetir con el token */
            if (g_buzon_largo < 32 || g_buzon_largo - 32 > sizeof(token)) return 32;
            tl = g_buzon_largo - 32;
            memcpy(token, g_buzon + 32, tl);
            g_sta.sae_tokens++;
            continue;
        }
        if (est) return 32;
        if (g_buzon_largo < 30 + 2 + 96) return 33;
        if ((g_buzon[30] | (g_buzon[31] << 8)) != 19) return 36;
        g_sta.sae_commits_ap++;
        if (sae_procesar(&g_sae, g_buzon + 32, g_buzon + 64, cripto_azar_bytes)) return 33;
        recibido = 1;
    }
    if (!recibido) return 10;
    uint8_t conf[32], v[32];
    sae_confirm(&g_sae, 1, 1, conf);
    for (int intento = 0; intento < 3; intento++) {
        n = wifi_cabecera(f, 0x00B0, g_sta.bssid);
        f[n++] = 3; f[n++] = 0; f[n++] = 2; f[n++] = 0; f[n++] = 0; f[n++] = 0;   /* SAE, confirm */
        f[n++] = 1; f[n++] = 0;                                                      /* send-confirm */
        memcpy(f + n, conf, 32); n += 32;
        wifi_tx_gestion(f, n);
        int k = wifi_esperar_auth(2, 600);
        if (k == 4) return 20;
        if (k != 1) continue;
        uint16_t est = (uint16_t)(g_buzon[28] | (g_buzon[29] << 8));
        g_sta.auth_estado = est;
        if (est) { g_sta.sae_estado = est; return 32; }
        if (g_buzon_largo < 30 + 34) return 34;
        sae_confirm(&g_sae, (uint16_t)(g_buzon[30] | (g_buzon[31] << 8)), 0, v);
        if (memcmp(v, g_buzon + 32, 32)) return 35;
        return 0;
    }
    return 34;
}

/* ================================================================== H165
 * Enlace de datos. Tras el 4 vias la asociacion queda abierta y las claves viven aqui (no
 * en la CAM): CCMP por software, verificado con el vector del estandar. Las tramas de datos
 * son 802.11 sin QoS (no anunciamos WMM ni HT): ToDS al AP, FromDS desde el AP, LLC/SNAP. */
#define WIFI_ETH_N 256u                    /* H182: 256 (H176: 128) */
static struct {
    int      activa;
    uint8_t  tk[16], kck[16], kek[16];
    uint8_t  gtk[4][16], gtk_ok[4];
    uint64_t pn_tx, pn_rx[17], pn_grp[4];       /* pn_rx por TID; 16 = sin QoS */
    uint8_t  replay[8];
    uint8_t  vd, eapol_ver, tasa;
    uint16_t razon_caida;
    uint32_t rx_datos, rx_mic_mal, rx_repetidas, rx_sin_clave, rx_llenas, rx_eapol;
    uint32_t tx_datos, tx_fallos, rekeys_grupo, caidas, deauth_sin_proteger, rx_dhcp;
    uint8_t  qos;                                 /* H176: WMM negociado: datos QoS (TID 0) */
    uint32_t ba_sesiones, ba_rechazadas, ba_dup, ba_huecos, ba_reordenadas, amsdu, sa_query, accion_otra;
    uint32_t tx_llenas;                           /* H177: veces que el anillo TX estaba lleno */
} g_enlace;
static int  g_cam_activa;                 /* H179: definida con la CAM, mas abajo */
static void wifi_cam_instalar(void);
static uint8_t  g_eth[WIFI_ETH_N][RED_MAX_TRAMA];
static uint16_t g_eth_l[WIFI_ETH_N];
static uint32_t g_eth_cab, g_eth_cola;

/* KDE de los datos de clave (12.7.2): GTK 00-0F-AC:1 e IGTK 00-0F-AC:9. */
static void wifi_kde(const uint8_t *d, uint32_t n) {
    for (uint32_t q = 0; q + 2 <= n; ) {
        uint8_t id = d[q], l = d[q + 1];
        if (q + 2 + l > n) break;
        if (id == 0xDD && l >= 6 && d[q + 2] == 0x00 && d[q + 3] == 0x0F && d[q + 4] == 0xAC) {
            if (d[q + 5] == 1) {
                uint8_t kid = d[q + 6] & 3;
                g_sta.gtk_id = kid; g_sta.gtk_largo = (uint8_t)(l - 6);
                if (l - 6 == 16) { memcpy(g_enlace.gtk[kid], d + q + 8, 16); g_enlace.gtk_ok[kid] = 1; g_enlace.pn_grp[kid] = 0; }
            } else if (d[q + 5] == 9 && l >= 12) {
                g_sta.igtk_id = (uint16_t)(d[q + 6] | (d[q + 7] << 8)); g_sta.igtk_largo = (uint8_t)(l - 12);
            }
        }
        if (id == 0xDD && l == 0) break;
        q += 2u + l;
    }
}

static int wifi_enviar_eth(const void *datos, uint32_t n);

/* Handshake de grupo (12.7.7): mensaje 1 cifrado con la PTK -> nueva GTK y mensaje 2. */
static void wifi_eapol_enlace(const uint8_t *e, uint32_t n) {
    g_enlace.rx_eapol++;
    if (n < EK_DATOS || e[1] != 3 || e[4] != 2) return;
    uint32_t el = 4 + (uint32_t)((e[2] << 8) | e[3]);
    if (el > n || el < EK_DATOS) return;
    uint16_t info = (uint16_t)((e[EK_INFO] << 8) | e[EK_INFO + 1]);
    if (!(info & 0x0100) || !(info & 0x0080) || (info & 0x0008)) return;   /* MIC, ACK, grupo (PTK rekey: no) */
    if (memcmp(e + EK_REPLAY, g_enlace.replay, 8) <= 0) return;
    static uint8_t copia[600], claro[300];
    if (el > sizeof(copia)) return;
    memcpy(copia, e, el);
    memset(copia + EK_MIC, 0, 16);
    uint8_t mic[16];
    wifi_mic(info, g_enlace.kck, copia, el, mic);
    if (memcmp(mic, e + EK_MIC, 16)) return;
    uint16_t dl = (uint16_t)((e[EK_DLARGO] << 8) | e[EK_DLARGO + 1]);
    if (dl < 16 || dl > sizeof(claro) + 8 || EK_DATOS + dl > el || wpa_unwrap(g_enlace.kek, e + EK_DATOS, dl, claro)) return;
    wifi_kde(claro, dl - 8u);
    wifi_borrar(claro, sizeof(claro));
    memcpy(g_enlace.replay, e + EK_REPLAY, 8);
    if (g_cam_activa) wifi_cam_instalar();                                 /* H179: GTK nueva a la CAM */
    uint8_t r[EK_DATOS + 4 + 14];
    uint8_t *m2 = r + 14;
    memcpy(r, g_sta.bssid, 6); memcpy(r + 6, g_wifi.mac, 6); r[12] = 0x88; r[13] = 0x8E;
    uint32_t ml = wifi_armar_eapol(m2, g_enlace.eapol_ver, (uint16_t)(0x0300 | g_enlace.vd), g_enlace.replay, 0, 0, 0, g_enlace.kck);
    wifi_enviar_eth(r, 14 + ml);
    g_enlace.rekeys_grupo++;
}

/* ================================================================== H179
 * CCMP de recepcion por hardware, como rtw88: rtw_sec_enable_sec_engine() y rtw_sec_write_cam() (sec.c),
 * con default_key_search: las entradas 0-3 son las claves de grupo por keyidx y la de pares va desde la 4
 * con la MAC del AP. La trama descifrada conserva la cabecera CCMP y los 8 bytes del MIC (mac80211 recibe
 * RX_FLAG_DECRYPTED sin IV_STRIPPED ni MIC_STRIPPED). El TX sigue por software (cuesta 0-2 % de CPU).
 * Respaldo: SWDEC = 1 (sin clave en la CAM) -> software; ICV_ERR -> se descarta; si las tramas "descifradas"
 * no traen LLC/SNAP, la CAM se apaga sola. */
static int g_cam_pedida = 1;
static uint32_t g_cam_rx_hw, g_cam_rx_sw, g_cam_formato_mal;

static void wifi_sec_motor(void) {                                          /* rtw_sec_enable_sec_engine() */
    wifi_escribir16(REG_CR, (uint16_t)(wifi_leer16(REG_CR) | RTW_SEC_ENGINE_EN));
    uint16_t c = wifi_leer16(RTW_SEC_CONFIG);
    c |= RTW_SEC_TX_DEC_EN | RTW_SEC_RX_DEC_EN;
    c |= RTW_SEC_TX_UNI_USE_DK | RTW_SEC_RX_UNI_USE_DK | RTW_SEC_TX_BC_USE_DK | RTW_SEC_RX_BC_USE_DK;
    wifi_escribir16(RTW_SEC_CONFIG, c);
}
static void wifi_cam_escribir(uint8_t idx, uint8_t keyidx, int grupo, const uint8_t *a, const uint8_t *k) {   /* rtw_sec_write_cam() */
    uint32_t cmd = RTW_SEC_CMD_WRITE_ENABLE | RTW_SEC_CMD_POLLING, dir = (uint32_t)idx << RTW_SEC_CAM_ENTRY_SHIFT;
    for (int i = 7; i >= 0; i--) {
        uint32_t v;
        if (i == 0) v = (uint32_t)(keyidx & 3) | ((RTW_CAM_AES & 7) << 2) | ((uint32_t)(grupo ? 1 : 0) << 6) | (1u << 15) |
                        ((uint32_t)a[0] << 16) | ((uint32_t)a[1] << 24);
        else if (i == 1) v = (uint32_t)a[2] | ((uint32_t)a[3] << 8) | ((uint32_t)a[4] << 16) | ((uint32_t)a[5] << 24);
        else if (i >= 6) v = 0;
        else { int j = (i - 2) << 2; v = (uint32_t)k[j] | ((uint32_t)k[j + 1] << 8) | ((uint32_t)k[j + 2] << 16) | ((uint32_t)k[j + 3] << 24); }
        wifi_escribir32(RTW_SEC_WRITE_REG, v);
        wifi_escribir32(RTW_SEC_CMD_REG, cmd | (dir + (uint32_t)i));
    }
}
static void wifi_cam_borrar(uint8_t idx) {                                  /* rtw_sec_clear_cam() */
    wifi_escribir32(RTW_SEC_WRITE_REG, 0);
    wifi_escribir32(RTW_SEC_CMD_REG, RTW_SEC_CMD_WRITE_ENABLE | RTW_SEC_CMD_POLLING | ((uint32_t)idx << RTW_SEC_CAM_ENTRY_SHIFT));
}
static void wifi_cam_apagar(void) {
    for (uint8_t i = 0; i <= RTW_SEC_DEFAULT_KEY_NUM; i++) wifi_cam_borrar(i);
    g_cam_activa = 0;
}
static const uint8_t g_difusion[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static void wifi_cam_instalar(void) {                                       /* rtw_ops_set_key(SET_KEY) */
    if (!g_cam_pedida || !g_enlace.activa) return;
    wifi_sec_motor();
    wifi_cam_escribir((uint8_t)RTW_SEC_DEFAULT_KEY_NUM, 0, 0, g_sta.bssid, g_enlace.tk);   /* pares: primera libre >= 4 */
    for (uint8_t k = 0; k < 4; k++) if (g_enlace.gtk_ok[k]) wifi_cam_escribir(k, k, 1, g_difusion, g_enlace.gtk[k]);
    g_cam_activa = 1;
}
int wifi_cam(int pedir) { RED_EXCLUSIVA();
    if (pedir == 0) { g_cam_pedida = 0; if (g_cam_activa) wifi_cam_apagar(); }
    else if (pedir == 1) { g_cam_pedida = 1; g_cam_formato_mal = 0; wifi_cam_instalar(); }
    return g_cam_activa;
}

/* Descifrado: si el hardware ya lo hizo, solo se quita la cabecera CCMP y el MIC y se lee el PN. */
static int wifi_descifrar(const uint8_t *tk, const uint8_t *f, size_t n, uint8_t *claro, size_t *cl, uint64_t *pn) {
    if (!g_rx_hw) { g_cam_rx_sw++; return wpa_ccmp_descifrar(tk, f, n, claro, cl, pn); }
    size_t hl = wpa_largo_cabecera(f);
    if (n < hl + 16 || !(f[hl + 3] & 0x20)) return -1;
    const uint8_t *c = f + hl;
    if (pn) *pn = (uint64_t)c[0] | ((uint64_t)c[1] << 8) | ((uint64_t)c[4] << 16) | ((uint64_t)c[5] << 24) |
                  ((uint64_t)c[6] << 32) | ((uint64_t)c[7] << 40);
    *cl = n - hl - 16;
    memcpy(claro, c + 8, *cl);
    g_cam_rx_hw++;
    return 0;
}
/* H176: entrega de un MSDU (LLC/SNAP + carga) como trama Ethernet II, o al suplicante si es EAPOL. */
static void wifi_entregar_msdu(const uint8_t *da, const uint8_t *sa, const uint8_t *m, uint32_t ml) {
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    if (ml < 8 || memcmp(m, snap, 6)) {
        if (g_rx_hw && ++g_cam_formato_mal > 32 && g_cam_activa) { wifi_cam_apagar(); g_cam_pedida = 0; }   /* H179: respaldo */
        return;
    }
    if (!memcmp(sa, g_wifi.mac, 6)) return;                                  /* eco de nuestras difusiones */
    g_enlace.rx_datos++;
    g_cam_formato_mal = 0;                                                   /* H179: el formato es el esperado */
    uint16_t et = (uint16_t)((m[6] << 8) | m[7]);
    if (et == 0x888E) { wifi_eapol_enlace(m + 8, ml - 8); return; }
    if (et == 0x0800 && ml >= 8 + 28 && m[8 + 9] == 17 && m[8 + 22] == 0 && m[8 + 23] == 68) g_enlace.rx_dhcp++;
    uint32_t largo = 14 + ml - 8;
    if (largo > RED_MAX_TRAMA) return;
    uint32_t sig = (g_eth_cab + 1) % WIFI_ETH_N;
    if (sig == g_eth_cola) { g_enlace.rx_llenas++; return; }
    uint8_t *e = g_eth[g_eth_cab];
    memcpy(e, da, 6); memcpy(e + 6, sa, 6); e[12] = (uint8_t)(et >> 8); e[13] = (uint8_t)et;
    memcpy(e + 14, m + 8, ml - 8);
    g_eth_l[g_eth_cab] = (uint16_t)largo;
    g_eth_cab = sig;
}

/* MSDU o A-MSDU (802.11-2020 9.3.2.2.2: subtramas DA|SA|largo BE|MSDU, rellenas a 4 bytes). */
static void wifi_entregar(const uint8_t *da, const uint8_t *sa, const uint8_t *c, uint32_t cl, int amsdu) {
    if (!amsdu) { wifi_entregar_msdu(da, sa, c, cl); return; }
    g_enlace.amsdu++;
    for (uint32_t q = 0; q + 14 <= cl; ) {
        uint32_t l = (uint32_t)((c[q + 12] << 8) | c[q + 13]);
        if (q + 14 + l > cl) break;
        wifi_entregar_msdu(c + q, c + q + 6, c + q + 14, l);
        q += (14 + l + 3) & ~3u;
    }
}

/* ------------------------------------------------- Block Ack de recepcion (H176)
 * Ventana de reordenamiento por TID como ieee80211_sta_manage_reorder_buf() de mac80211: el AP manda
 * rafagas A-MPDU (el hardware separa los MPDU y responde el BlockAck); aqui se ordenan por numero de
 * secuencia y recien al liberar se comprueba el PN (los reenvios conservan su PN original). */
#define WIFI_BA_SESIONES 2u
#define WIFI_BA_VENT     64u
#define WIFI_BA_SLOT     1600u
#define WIFI_BA_ESPERA   40u                 /* ms con un hueco antes de saltarlo */
static struct wifi_ba {
    int      activa;
    uint8_t  tid;
    uint16_t inicio, tam, guardadas;
    uint64_t t_hueco;
    uint8_t  ocupado[WIFI_BA_VENT], amsdu[WIFI_BA_VENT];
    uint16_t largo[WIFI_BA_VENT];
    uint64_t pn[WIFI_BA_VENT];
    uint8_t  da[WIFI_BA_VENT][6], sa[WIFI_BA_VENT][6];
    uint8_t  datos[WIFI_BA_VENT][WIFI_BA_SLOT];
} g_ba[WIFI_BA_SESIONES];

static struct wifi_ba *wifi_ba_de(uint8_t tid) {
    for (uint32_t i = 0; i < WIFI_BA_SESIONES; i++) if (g_ba[i].activa && g_ba[i].tid == tid) return &g_ba[i];
    return 0;
}
static int seq_menor(uint16_t a, uint16_t b) { return ((a - b) & 0xFFF) > 2048; }
static uint16_t seq_dif(uint16_t a, uint16_t b) { return (uint16_t)((a - b) & 0xFFF); }

/* Comprueba el PN en orden de secuencia y entrega. */
static void wifi_ba_soltar(struct wifi_ba *b, uint32_t i) {
    if (b->pn[i] > g_enlace.pn_rx[b->tid]) {
        g_enlace.pn_rx[b->tid] = b->pn[i];
        wifi_entregar(b->da[i], b->sa[i], b->datos[i], b->largo[i], b->amsdu[i]);
    } else {
        g_enlace.rx_repetidas++;
    }
    b->ocupado[i] = 0;
    b->guardadas--;
}
static void wifi_ba_contiguas(struct wifi_ba *b) {
    while (b->ocupado[b->inicio % WIFI_BA_VENT]) {
        wifi_ba_soltar(b, b->inicio % WIFI_BA_VENT);
        b->inicio = (uint16_t)((b->inicio + 1) & 0xFFF);
    }
    b->t_hueco = b->guardadas ? tiempo_obtener_milisegundos() : 0;
}
static void wifi_ba_avanzar(struct wifi_ba *b, uint16_t nuevo) {
    while (b->inicio != nuevo) {
        uint32_t i = b->inicio % WIFI_BA_VENT;
        if (b->ocupado[i]) wifi_ba_soltar(b, i);
        b->inicio = (uint16_t)((b->inicio + 1) & 0xFFF);
    }
}
static void wifi_ba_recibir(struct wifi_ba *b, uint16_t sn, uint64_t pn, const uint8_t *da, const uint8_t *sa,
                            const uint8_t *c, uint32_t cl, int amsdu) {
    if (seq_menor(sn, b->inicio)) { g_enlace.ba_dup++; return; }
    if (seq_dif(sn, b->inicio) >= b->tam) wifi_ba_avanzar(b, (uint16_t)((sn - b->tam + 1) & 0xFFF));
    uint32_t i = sn % WIFI_BA_VENT;
    if (b->ocupado[i]) { g_enlace.ba_dup++; return; }
    if (cl > WIFI_BA_SLOT) {                         /* no cabe (A-MSDU grande): se entrega ya */
        if (sn == b->inicio) b->inicio = (uint16_t)((b->inicio + 1) & 0xFFF);
        if (pn > g_enlace.pn_rx[b->tid]) { g_enlace.pn_rx[b->tid] = pn; wifi_entregar(da, sa, c, cl, amsdu); }
        wifi_ba_contiguas(b);
        return;
    }
    if (sn != b->inicio) g_enlace.ba_reordenadas++;
    memcpy(b->datos[i], c, cl);
    memcpy(b->da[i], da, 6); memcpy(b->sa[i], sa, 6);
    b->largo[i] = (uint16_t)cl; b->amsdu[i] = (uint8_t)amsdu; b->pn[i] = pn;
    b->ocupado[i] = 1;
    b->guardadas++;
    wifi_ba_contiguas(b);
}
/* Hueco que no se lleno en WIFI_BA_ESPERA ms (el AP se rindio o el BAR no nos llega): saltarlo. */
static void wifi_ba_temporizador(void) {
    uint64_t ahora = tiempo_obtener_milisegundos();
    for (uint32_t s = 0; s < WIFI_BA_SESIONES; s++) {
        struct wifi_ba *b = &g_ba[s];
        if (!b->activa || !b->guardadas || !b->t_hueco || ahora - b->t_hueco < WIFI_BA_ESPERA) continue;
        g_enlace.ba_huecos++;
        for (uint32_t g = 0; g < WIFI_BA_VENT && !b->ocupado[b->inicio % WIFI_BA_VENT]; g++)
            b->inicio = (uint16_t)((b->inicio + 1) & 0xFFF);
        wifi_ba_contiguas(b);
    }
}
static void wifi_ba_cerrar(struct wifi_ba *b) {
    if (!b->activa) return;
    for (uint32_t k = 0; k < b->tam && b->guardadas; k++) {
        uint32_t i = b->inicio % WIFI_BA_VENT;
        if (b->ocupado[i]) wifi_ba_soltar(b, i);
        b->inicio = (uint16_t)((b->inicio + 1) & 0xFFF);
    }
    b->activa = 0;
}

static void wifi_rx_datos(const uint8_t *f, uint32_t n) {
    if (!g_enlace.activa || n < 24) return;
    if ((f[1] & 3) != 2 || memcmp(f + 10, g_sta.bssid, 6)) return;           /* FromDS desde nuestro AP */
    uint8_t sub = (f[0] >> 4) & 0xF;
    if (sub & 4) return;                                                     /* sin datos (null) */
    int grupo = f[4] & 1;
    if (!grupo && memcmp(f + 4, g_wifi.mac, 6)) return;
    if (!(f[1] & 0x40)) { g_enlace.rx_sin_clave++; return; }                 /* sin proteger: se descarta */
    if (f[1] & 0x04) return;                                                 /* fragmentos: no soportados */
    uint32_t hl = (uint32_t)wpa_largo_cabecera(f);
    if (n < hl + 16 + 8) return;
    uint8_t kid = f[hl + 3] >> 6;
    const uint8_t *tk = grupo ? (g_enlace.gtk_ok[kid] ? g_enlace.gtk[kid] : 0) : g_enlace.tk;
    if (!tk) { g_enlace.rx_sin_clave++; return; }
    int qos = (sub & 8) != 0;
    int tid = qos ? (f[24] & 0x0F) : 16;
    int amsdu = qos && (f[24] & 0x80);
    static uint8_t claro[2400];
    size_t cl = 0;
    uint64_t pn = 0;
    if (n - hl - 16 > sizeof(claro)) return;
    uint64_t c0 = rdtsc();
    int mal = wifi_descifrar(tk, f, n, claro, &cl, &pn);                    /* H179: hardware o software */
    g_ciclos_ccmp_rx += rdtsc() - c0;
    if (mal) { g_enlace.rx_mic_mal++; return; }
    const uint8_t *da = f + 4, *sa = f + 16;
    struct wifi_ba *b = (!grupo && qos) ? wifi_ba_de((uint8_t)tid) : 0;
    if (b) {                                                                 /* H176: reordenar antes del PN */
        uint16_t sn = (uint16_t)((f[22] | (f[23] << 8)) >> 4);
        wifi_ba_recibir(b, sn, pn, da, sa, claro, (uint32_t)cl, amsdu);
        return;
    }
    uint64_t *ult = grupo ? &g_enlace.pn_grp[kid] : &g_enlace.pn_rx[tid];
    if (pn <= *ult) { g_enlace.rx_repetidas++; return; }
    *ult = pn;
    wifi_entregar(da, sa, claro, (uint32_t)cl, amsdu);
}

/* H176: trama de accion hacia el AP. Con PMF las categorias robustas (Block Ack, SA Query) van con CCMP. */
static void wifi_tx_accion(const uint8_t *cuerpo, uint32_t cn) {
    uint8_t f[64], fp[96];
    if (cn > 40) return;
    uint32_t k = wifi_cabecera(f, 0x00D0, g_sta.bssid);
    memcpy(f + k, cuerpo, cn); k += cn;
    if (g_sta.mfp) {
        size_t np = wpa_ccmp_cifrar(g_enlace.tk, ++g_enlace.pn_tx, 0, f, k, fp);
        if (np) wifi_tx_gestion(fp, (uint32_t)np);
    } else {
        wifi_tx_gestion(f, k);
    }
}

/* ADDBA Request del AP (802.11-2020 9.6.4.2): aceptar con la ventana pedida (max. 64) si el modo lo permite. */
static void wifi_addba(const uint8_t *c, uint32_t cl) {
    if (cl < 9) return;
    uint8_t token = c[2];
    uint16_t par = (uint16_t)(c[3] | (c[4] << 8)), espera = (uint16_t)(c[5] | (c[6] << 8));
    uint16_t ssc = (uint16_t)(c[7] | (c[8] << 8));
    uint8_t tid = (uint8_t)((par >> 2) & 0xF);
    uint16_t tam = (uint16_t)(par >> 6);
    if (!tam || tam > WIFI_BA_VENT) tam = WIFI_BA_VENT;
    struct wifi_ba *b = wifi_ba_de(tid);
    if (!b && g_wifi_modo >= 2)
        for (uint32_t i = 0; i < WIFI_BA_SESIONES && !b; i++) if (!g_ba[i].activa) b = &g_ba[i];
    uint16_t estado = 37;                                                    /* REQUEST_DECLINED */
    if (b) {
        wifi_ba_cerrar(b);
        memset(b->ocupado, 0, sizeof(b->ocupado));
        b->activa = 1; b->tid = tid; b->tam = tam; b->inicio = (uint16_t)(ssc >> 4);
        b->guardadas = 0; b->t_hueco = 0;
        estado = 0;
        g_enlace.ba_sesiones++;
    } else {
        g_enlace.ba_rechazadas++;
    }
    uint16_t rp = (uint16_t)((tam << 6) | (tid << 2) | (1u << 1));            /* inmediata, sin A-MSDU */
    uint8_t r[9] = { 3, 1, token, (uint8_t)estado, (uint8_t)(estado >> 8),
                     (uint8_t)rp, (uint8_t)(rp >> 8), (uint8_t)espera, (uint8_t)(espera >> 8) };
    wifi_tx_accion(r, sizeof(r));
}

/* Gestion con el enlace activo: deauth/disassoc del AP (con PMF solo protegidos) y, H176, acciones:
 * ADDBA/DELBA (Block Ack) y SA Query (PMF, 802.11-2020 11.13). */
static void wifi_rx_gestion_enlace(const uint8_t *f, uint32_t n) {
    if (!g_enlace.activa || n < 26) return;
    uint8_t sub = (f[0] >> 4) & 0xF;
    if ((sub != 12 && sub != 10 && sub != 13) || memcmp(f + 4, g_wifi.mac, 6) || memcmp(f + 10, g_sta.bssid, 6)) return;
    if (g_sta.mfp && !(f[1] & 0x40)) {
        if (sub != 13) g_enlace.deauth_sin_proteger++;
        return;
    }
    static uint8_t claro[256];
    const uint8_t *c = f + 24;
    uint32_t cl = n - 24;
    if (f[1] & 0x40) {
        size_t l = 0;
        if (n > 24 + 16 + sizeof(claro) || wifi_descifrar(g_enlace.tk, f, n, claro, &l, 0)) return;   /* MIC invalido */
        c = claro; cl = (uint32_t)l;
    }
    if (sub == 13) {
        if (cl >= 2 && c[0] == 3 && c[1] == 0) { wifi_addba(c, cl); return; }
        if (cl >= 6 && c[0] == 3 && c[1] == 2) {                           /* DELBA */
            uint16_t par = (uint16_t)(c[2] | (c[3] << 8));
            struct wifi_ba *b = (par & 0x0800) ? wifi_ba_de((uint8_t)(par >> 12)) : 0;   /* el AP era el originador */
            if (b) wifi_ba_cerrar(b);
            return;
        }
        if (cl >= 4 && c[0] == 8 && c[1] == 0) {                           /* SA Query Request -> Response */
            uint8_t r[4] = { 8, 1, c[2], c[3] };
            wifi_tx_accion(r, sizeof(r));
            g_enlace.sa_query++;
            return;
        }
        g_enlace.accion_otra++;
        return;
    }
    if (cl < 2) return;
    g_enlace.razon_caida = (uint16_t)(c[0] | (c[1] << 8));
    g_enlace.caidas++;
    g_enlace.activa = 0;
    for (uint32_t i = 0; i < WIFI_BA_SESIONES; i++) g_ba[i].activa = 0;
    wifi_escribir32_mascara(REG_CR, 0x30000, RTW_NET_NO_LINK);
}

/* Ethernet II -> 802.11 datos ToDS (A1 BSSID, A2 nosotros, A3 destino) + LLC/SNAP, CCMP con TK. */
static int wifi_enviar_eth_ex(const void *datos, uint32_t n);
static int wifi_enviar_eth(const void *datos, uint32_t n) {
    uint64_t c0 = rdtsc();
    int r = wifi_enviar_eth_ex(datos, n);
    g_ciclos_tx += rdtsc() - c0;
    return r;
}
static int wifi_enviar_eth_ex(const void *datos, uint32_t n) {
    if (!g_enlace.activa || n < 14 || n > 14 + 1500 + 22) return -1;
    /* (H176: +2 bytes de QoS Control caben en claro[1600]) */
    const uint8_t *e = datos;
    static uint8_t claro[1600], cif[1600 + 16];
    uint32_t k = wifi_cabecera(claro, g_enlace.qos ? 0x0188 : 0x0108, g_sta.bssid);   /* QoS Data o Data, ToDS */
    memcpy(claro + 16, e, 6);
    if (g_enlace.qos) { claro[k++] = 0; claro[k++] = 0; }                      /* QoS Control: TID 0, ack normal */
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    memcpy(claro + k, snap, 6); k += 6;
    claro[k++] = e[12]; claro[k++] = e[13];
    memcpy(claro + k, e + 14, n - 14); k += n - 14;
    size_t m = wpa_ccmp_cifrar(g_enlace.tk, ++g_enlace.pn_tx, 0, claro, k, cif);
    if (!m) { g_enlace.tx_fallos++; return -1; }
    /* H177: anillo lleno -> sondear el puntero del hardware (antes dormia 1 ms por intento). */
    uint64_t limite = tiempo_obtener_milisegundos() + 20;
    for (;;) {
        int r = wifi_tx_trama(cif, (uint32_t)m, g_enlace.tasa);
        if (r == 0) { g_enlace.tx_datos++; return 0; }
        if (r != -2 || tiempo_obtener_milisegundos() > limite) break;
        g_enlace.tx_llenas++;
        for (int k = 0; k < 200; k++) __asm__ volatile("pause");
    }
    g_enlace.tx_fallos++;
    return -1;
}

static int wifi_recibir_eth(void *buffer, uint32_t capacidad, uint32_t *longitud) {
    /* H182: vaciar el anillo tambien cada 16 tramas entregadas, no solo con la cola vacia: a ~33 000
     * tramas/s el anillo da pocos ms de colchon mientras la pila procesa un lote. */
    static uint32_t entregadas;
    if (g_eth_cola == g_eth_cab || (++entregadas & 15) == 0) { wifi_rx_vaciar(1); wifi_ba_temporizador(); }
    uint32_t ocup = (g_eth_cab - g_eth_cola + WIFI_ETH_N) % WIFI_ETH_N;
    if (ocup > g_eth_max) g_eth_max = ocup;
    if (g_eth_cola == g_eth_cab) return 0;
    uint32_t l = g_eth_l[g_eth_cola];
    if (l > capacidad) l = capacidad;
    memcpy(buffer, g_eth[g_eth_cola], l);
    *longitud = l;
    g_eth_cola = (g_eth_cola + 1) % WIFI_ETH_N;
    return 1;
}

static int wifi_enlace_activo(void) { return g_enlace.activa; }

static struct red_externa g_wifi_ext = {
    "Realtek RTL8822CE (WiFi, CCMP por software)", 0x10EC, 0xC822, 54, { 0 },
    wifi_enviar_eth, wifi_recibir_eth, wifi_enlace_activo
};

int wifi_enlazado(void) { RED_EXCLUSIVA(); return g_enlace.activa; }

/* Cierre ordenado: Deauthentication (razon 3) protegido con CCMP si hay PMF. No toca la interfaz
 * de la pila: 'wifi desconectar' llama antes a wifi_red_cable(), y al reconectar la pila sigue en WiFi. */
void wifi_desconectar(void) { RED_EXCLUSIVA();
    if (g_enlace.activa) {
        uint8_t f[32], fp[64];
        uint32_t n = wifi_cabecera(f, 0x00C0, g_sta.bssid);
        f[n++] = 3; f[n++] = 0;
        if (g_sta.mfp) {
            size_t np = wpa_ccmp_cifrar(g_enlace.tk, ++g_enlace.pn_tx, 0, f, n, fp);
            if (np) wifi_tx_gestion(fp, (uint32_t)np);
        } else {
            wifi_tx_gestion(f, n);
        }
        esperar_milisegundos(20);
        wifi_escribir32_mascara(REG_CR, 0x30000, RTW_NET_NO_LINK);
    }
    g_sta_escucha = 0;
    for (uint32_t i = 0; i < WIFI_BA_SESIONES; i++) g_ba[i].activa = 0;   /* H176 */
    if (g_cam_activa) wifi_cam_apagar();                                   /* H179 */
    wifi_borrar(&g_enlace, sizeof(g_enlace));
    g_eth_cab = g_eth_cola = 0;
}

/* `wifi red`: la pila de TAEK pasa a usar el WiFi (DHCP). Si el DHCP falla, o si el stream
 * de consola no vuelve a conectar en 'espera_ms', regresa al cable con la configuracion previa. */
static struct red_config g_cfg_cable;
static int g_cfg_cable_ok;

static void wifi_volver_al_cable(void) {
    red_usar_externa(0);
    if (g_cfg_cable_ok && g_cfg_cable.configurada)
        red_ip_estatica(g_cfg_cable.ip, g_cfg_cable.mascara, g_cfg_cable.puerta, g_cfg_cable.dns[0]);
    red_stream_pausar(1);
    red_stream_pausar(0);
}

static void wifi_imprimir_ip(const char *pre, const uint8_t ip[4]) {
    char b[20];
    consola_imprimir(pre); consola_imprimir(red_ip_a_texto(ip, b, sizeof(b)));
}

int wifi_red_usar(uint32_t espera_ms) { RED_EXCLUSIVA();
    if (!g_enlace.activa) {
        consola_imprimir_linea("[WIFI] Conectando con /wifi.conf...");
        int r = wifi_conectar();
        wifi_imprimir_conexion();
        /* H172: status 30 = el AP aun tiene nuestra asociacion PMF anterior (p. ej. tras un reinicio sin
         * deauth) y la esta comprobando con SA Query; la borra en ~1 s. Reintentar. */
        for (int i = 0; r && i < 3 && g_sta.auth_estado == 30; i++) {
            consola_imprimir_linea("[WIFI] El AP pide reintentar (status 30, SA Query de PMF); otra vez en 2 s...");
            esperar_milisegundos(2000);
            r = wifi_conectar();
            wifi_imprimir_conexion();
        }
        if (r) return -1;
    }
    if (red_externa_activa() != &g_wifi_ext) {
        g_cfg_cable = *red_obtener_config();
        g_cfg_cable_ok = 1;
    }
    memcpy(g_wifi_ext.mac, g_wifi.mac, 6);
    consola_imprimir_linea("[WIFI] La pila IPv4 pasa al WiFi; DHCP...");
    red_stream_pausar(1);
    red_usar_externa(&g_wifi_ext);
    uint64_t t0 = tiempo_obtener_milisegundos();
    /* H167: el servidor DHCP del router tarda >2 s en ofrecer (comprueba la IP antes); red_dhcp
     * reparte el plazo en 3 pasos, asi que 15 s dan 5 s por paso. Hasta 3 intentos. */
    int r = -1;
    for (int intento = 0; intento < 3 && r; intento++) {
        r = red_dhcp(30000);
        if (r) { consola_imprimir("[WIFI] DHCP sin respuesta ("); consola_imprimir_dec((uint64_t)(-r)); consola_imprimir_linea("), reintentando..."); }
    }
    uint32_t ms = (uint32_t)(tiempo_obtener_milisegundos() - t0);
    if (r) {
        wifi_volver_al_cable();
        consola_imprimir("[WIFI] DHCP por WiFi fallo ("); consola_imprimir_dec((uint64_t)(-r));
        consola_imprimir_linea("); la pila volvio al cable. La asociacion WiFi sigue abierta.");
        uint32_t dv, dx, dt; uint8_t du;
        red_dhcp_contadores(&dv, &dx, &dt, &du);
        consola_imprimir("       Respuestas DHCP vistas "); consola_imprimir_dec(dv);
        consola_imprimir("  xid ajeno "); consola_imprimir_dec(dx);
        consola_imprimir("  tipo inesperado "); consola_imprimir_dec(dt);
        consola_imprimir("  ultimo tipo "); consola_imprimir_dec(du);
        consola_imprimir("  (WiFi: UDP->68 "); consola_imprimir_dec(g_enlace.rx_dhcp); consola_imprimir_linea(")");
        wifi_imprimir_enlace();
        return -2;
    }
    const struct red_config *c = red_obtener_config();
    wifi_imprimir_ip("[WIFI] DHCP concedido en ", c->ip);
    consola_imprimir(" ms "); consola_imprimir_dec(ms);
    wifi_imprimir_ip("  puerta ", c->puerta);
    wifi_imprimir_ip("  DNS ", c->dns[0]);
    wifi_imprimir_ip("  servidor ", c->servidor_dhcp);
    consola_imprimir_linea("");
    red_stream_pausar(0);
    if (!red_stream_activo() || !espera_ms) return 0;
    consola_imprimir_linea("[WIFI] Esperando que el stream de consola reconecte por WiFi...");
    uint64_t fin = tiempo_obtener_milisegundos() + espera_ms;
    while (tiempo_obtener_milisegundos() < fin) {
        red_stream_atender();
        if (red_stream_conectado()) {
            consola_imprimir("[WIFI] Stream de consola reconectado por WiFi en ");
            consola_imprimir_dec(tiempo_obtener_milisegundos() - t0); consola_imprimir_linea(" ms.");
            wifi_imprimir_enlace();
            return 0;
        }
        if (!g_enlace.activa) break;
        esperar_milisegundos(2);
    }
    wifi_volver_al_cable();
    consola_imprimir_linea(g_enlace.activa ? "[WIFI] El stream no reconecto por WiFi: la pila volvio al cable."
                                           : "[WIFI] El AP cerro la asociacion: la pila volvio al cable.");
    wifi_imprimir_enlace();
    return -3;
}

/* H166: sin cable (o con el enlace WiFi caido) desde el bucle del stream: radio, escaneo,
 * asociacion y DHCP por WiFi. Como mucho un intento por minuto. 0 = la pila quedo en WiFi. */
int wifi_red_automatica(void) { RED_EXCLUSIVA();
    static uint64_t proximo;
    uint64_t ahora = tiempo_obtener_milisegundos();
    if (ahora < proximo) return -1;
    proximo = ahora + 60000;
    consola_imprimir_linea(red_enlace_cable() ? "[WIFI AUTO] /wifi.auto: la pila pasa a la red de /wifi.conf..."
                                              : "[WIFI AUTO] Sin enlace por cable: intentando la red de /wifi.conf...");
    if (!g_wifi.radio_activa && wifi_encender("/rtw8822c_fw.bin") != 0) {
        consola_imprimir_linea("[WIFI AUTO] No se pudo encender la radio.");
        return -1;
    }
    if (!g_enlace.activa) wifi_escanear(120);
    int r = wifi_red_usar(0);
    if (!r) consola_imprimir_linea("[WIFI AUTO] Pila IPv4 por WiFi.");
    return r;
}

/* H166: /wifi.auto en el USB = preferir el WiFi al arrancar aunque haya cable. */
int wifi_preferida(void) { RED_EXCLUSIVA();
    void *b = 0; size_t n = 0; int dma = 0;
    if (vfs_leer_archivo_binario("/wifi.auto", &b, &n, &dma) || !b) return 0;
    vfs_liberar_archivo_binario(b, n, dma);
    return 1;
}

int wifi_red_cable(void) { RED_EXCLUSIVA();
    if (red_externa_activa() != &g_wifi_ext) return -1;
    wifi_volver_al_cable();
    return 0;
}

static void wifi_hex2_pre(uint8_t v);
void wifi_imprimir_enlace(void) { RED_EXCLUSIVA();
    const char *h = "0123456789ABCDEF";
    char b[18];
    for (int i = 0; i < 6; i++) { b[i * 3] = h[g_sta.bssid[i] >> 4]; b[i * 3 + 1] = h[g_sta.bssid[i] & 0xF]; b[i * 3 + 2] = i < 5 ? ':' : 0; }
    consola_imprimir("[WIFI] Enlace "); consola_imprimir(g_enlace.activa ? "ACTIVO con " : "inactivo; ultimo AP ");
    consola_imprimir(b);
    consola_imprimir(" canal "); consola_imprimir_dec(g_sta.canal);
    consola_imprimir("  pila: "); consola_imprimir_linea(red_externa_activa() == &g_wifi_ext ? "WiFi" : "cable");
    consola_imprimir("       RX datos "); consola_imprimir_dec(g_enlace.rx_datos);
    consola_imprimir("  MIC mal "); consola_imprimir_dec(g_enlace.rx_mic_mal);
    consola_imprimir("  repetidas "); consola_imprimir_dec(g_enlace.rx_repetidas);
    consola_imprimir("  sin clave "); consola_imprimir_dec(g_enlace.rx_sin_clave);
    consola_imprimir("  cola llena "); consola_imprimir_dec(g_enlace.rx_llenas);
    consola_imprimir("  EAPOL "); consola_imprimir_dec(g_enlace.rx_eapol);
    consola_imprimir_linea("");
    consola_imprimir("       TX datos "); consola_imprimir_dec(g_enlace.tx_datos);
    consola_imprimir("  fallos "); consola_imprimir_dec(g_enlace.tx_fallos);
    consola_imprimir("  anillo lleno "); consola_imprimir_dec(g_enlace.tx_llenas);
    consola_imprimir("  PN "); consola_imprimir_dec(g_enlace.pn_tx);
    consola_imprimir("  tasa "); consola_imprimir_dec(g_enlace.tasa == DESC_RATE54M ? 54 : 24);
    consola_imprimir(" Mbps  rekeys GTK "); consola_imprimir_dec(g_enlace.rekeys_grupo);
    consola_imprimir("  caidas "); consola_imprimir_dec(g_enlace.caidas);
    if (g_enlace.caidas) { consola_imprimir(" (razon "); consola_imprimir_dec(g_enlace.razon_caida); consola_imprimir(")"); }
    consola_imprimir_linea("");
    /* H176 */
    static const char *modos[] = { "11a", "11ac", "11ac+BA" };
    consola_imprimir("       Modo "); consola_imprimir(modos[g_wifi_modo]);
    consola_imprimir("  anunciado "); consola_imprimir(g_sta.ac == 2 ? "VHT" : g_sta.ac == 1 ? "HT" : "11a");
    consola_imprimir("  ancho "); consola_imprimir_dec(g_sta.ancho); consola_imprimir(" MHz  QoS "); consola_imprimir_dec(g_enlace.qos);
    consola_imprimir("  RX ultima tasa 0x"); wifi_hex2_pre(g_wifi.rx_tasa);
    consola_imprimir("  HT "); consola_imprimir_dec(g_wifi.rx_ht);
    consola_imprimir("  VHT "); consola_imprimir_dec(g_wifi.rx_vht);
    consola_imprimir_linea("");
    consola_imprimir("       Block Ack: sesiones "); consola_imprimir_dec(g_enlace.ba_sesiones);
    consola_imprimir("  rechazadas "); consola_imprimir_dec(g_enlace.ba_rechazadas);
    for (uint32_t i = 0; i < WIFI_BA_SESIONES; i++) if (g_ba[i].activa) {
        consola_imprimir("  [TID "); consola_imprimir_dec(g_ba[i].tid); consola_imprimir(" ventana "); consola_imprimir_dec(g_ba[i].tam);
        consola_imprimir("]");
    }
    consola_imprimir("  reordenadas "); consola_imprimir_dec(g_enlace.ba_reordenadas);
    consola_imprimir("  duplicadas "); consola_imprimir_dec(g_enlace.ba_dup);
    consola_imprimir("  huecos "); consola_imprimir_dec(g_enlace.ba_huecos);
    consola_imprimir("  A-MSDU "); consola_imprimir_dec(g_enlace.amsdu);
    consola_imprimir("  SA Query "); consola_imprimir_dec(g_enlace.sa_query);
    consola_imprimir_linea("");
    consola_imprimir("       Anillo RX "); consola_imprimir_dec(g_rx_n); consola_imprimir(" max ocupado "); consola_imprimir_dec(g_rx_max_pend);   /* H182 */
    consola_imprimir("  cola Ethernet "); consola_imprimir_dec(WIFI_ETH_N); consola_imprimir(" max "); consola_imprimir_dec(g_eth_max);
    consola_imprimir_linea("");
    g_rx_max_pend = 0; g_eth_max = 0;
    consola_imprimir("       CCMP RX: CAM "); consola_imprimir(g_cam_activa ? "activa" : g_cam_pedida ? "pedida" : "apagada");   /* H179 */
    consola_imprimir("  por hardware "); consola_imprimir_dec(g_cam_rx_hw);
    consola_imprimir("  por software "); consola_imprimir_dec(g_cam_rx_sw);
    consola_imprimir("  ICV mal "); consola_imprimir_dec(g_cam_icv);
    consola_imprimir("  formato mal "); consola_imprimir_dec(g_cam_formato_mal);
    consola_imprimir_linea("");
}

int wifi_conectar(void) { RED_EXCLUSIVA();
    wifi_desconectar();                           /* H165: una sola asociacion */
    memset(&g_sta, 0, sizeof(g_sta));
    if (!g_wifi.radio_activa) { g_sta.error = 1; return -1; }
    char ssid[33], clave[64];
    if (wifi_leer_conf(ssid, clave)) { g_sta.error = 2; return -1; }
    int k = -1;
    for (uint32_t i = 0; i < g_n_redes; i++)
        if (!strcmp_seguro(g_redes[i].ssid, ssid) && (k < 0 || g_redes[i].rssi > g_redes[k].rssi)) k = (int)i;
    if (k < 0) { wifi_borrar(clave, sizeof(clave)); g_sta.error = 3; return -1; }
    const struct wifi_red *red = &g_redes[k];
    if (red->canal <= 14) { wifi_borrar(clave, sizeof(clave)); g_sta.error = 4; return -1; }   /* coex 2.4 GHz */
    g_red_fallida = red;
    /* H164: SAE (WPA3) si el AP lo ofrece; si no, PSK. PMF si el AP anuncia MFPC y vamos por SAE. */
    if (!red->rsn || !red->rsn_ccmp || (!red->rsn_psk && !red->rsn_sae)) { wifi_borrar(clave, sizeof(clave)); g_sta.error = 5; return -1; }
    g_sta.akm = red->rsn_sae ? 8 : 2;
    g_sta.mfp = g_sta.akm == 8 && (red->rsn_caps & 0x0080);
    if ((red->rsn_caps & 0x0040) && !g_sta.mfp) { wifi_borrar(clave, sizeof(clave)); g_sta.error = 6; return -1; }   /* MFPR sin SAE */
    memcpy(g_sta.bssid, red->bssid, 6);
    g_sta.canal = red->canal;
    g_sta.rssi = red->rssi;

    uint8_t pmk[32], ptk[48], snonce[32], anonce[32], replay[8], e[256], rsn[32];
    uint64_t t0 = tiempo_obtener_milisegundos();
    int r;
    if (g_sta.akm == 2) {
        /* PMK = PBKDF2(clave, SSID); la clave se borra en cuanto se usa. */
        r = wpa_pmk(clave, (const uint8_t *)ssid, strlen_seguro(ssid), pmk);
        wifi_borrar(clave, sizeof(clave));
        g_sta.pmk_ms = (uint32_t)(tiempo_obtener_milisegundos() - t0);
        if (r) { g_sta.error = 7; return -1; }
    } else {
        /* SAE: PWE (hunting-and-pecking) y commit antes de sintonizar; la clave solo entra al PWE. */
        memset(&g_sae, 0, sizeof(g_sae));
        r = sae_pwe(&g_sae, g_wifi.mac, g_sta.bssid, clave, strlen_seguro(clave));
        wifi_borrar(clave, sizeof(clave));
        if (r) { g_sta.error = 30; return -1; }
        if (sae_commit(&g_sae, 0, 0, cripto_azar_bytes)) { wifi_borrar(&g_sae, sizeof(g_sae)); g_sta.error = 31; return -1; }
        g_sta.sae_contador_pwe = g_sae.contador_pwe;
        g_sta.sae_ms = (uint32_t)(tiempo_obtener_milisegundos() - t0);
        memset(pmk, 0, sizeof(pmk));
    }

    wifi_fijar_canal(red->canal);
    if (g_wifi.pot_idx_6m[0] > 0x5A || g_wifi.pot_idx_6m[1] > 0x5A) {
        wifi_borrar(pmk, 32); wifi_borrar(&g_sae, sizeof(g_sae)); g_sta.error = 8; return -1;
    }
    for (int i = 0; i < 6; i++) wifi_escribir8(0x0618 + i, g_sta.bssid[i]);     /* PORT_SET_BSSID */
    memcpy(g_sta_bssid, g_sta.bssid, 6);
    g_sta_escucha = 1;
    wifi_rx_vaciar(0);
    uint8_t f[256];
    uint32_t n;

    if (g_sta.akm == 8) {
        /* 1) H164: autenticacion SAE (commit/confirm). */
        int ea = wifi_auth_sae(f);
        if (!ea) { memcpy(pmk, g_sae.pmk, 32); g_sta.paso = WIFI_STA_AUTH; }
        wifi_borrar(&g_sae, sizeof(g_sae));
        if (ea) { g_sta.error = ea; goto fin; }
    } else {
        /* 1) Autenticacion Open System: algoritmo 0, secuencia 1. */
        n = wifi_cabecera(f, 0x00B0, g_sta.bssid);
        f[n++] = 0; f[n++] = 0; f[n++] = 1; f[n++] = 0; f[n++] = 0; f[n++] = 0;
        for (int i = 0; i < 3 && g_buzon_tipo != 1; i++) { wifi_tx_gestion(f, n); wifi_esperar_buzon(1, 200); }
        if (g_buzon_tipo != 1) { g_sta.error = g_buzon_tipo == 4 ? 20 : 10; goto fin; }
        g_sta.auth_estado = (uint16_t)(g_buzon[28] | (g_buzon[29] << 8));
        if (g_sta.auth_estado) { g_sta.error = 11; goto fin; }
        g_sta.paso = WIFI_STA_AUTH;
    }

    /* 2) Asociacion: capacidades ESS|Privacy, intervalo 10, SSID, tasas 11a, RSN. */
    n = wifi_cabecera(f, 0x0000, g_sta.bssid);
    f[n++] = 0x11; f[n++] = 0x00; f[n++] = 10; f[n++] = 0;
    uint32_t sl = strlen_seguro(ssid);
    f[n++] = 0; f[n++] = (uint8_t)sl; memcpy(f + n, ssid, sl); n += sl;
    static const uint8_t t5[] = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
    f[n++] = 1; f[n++] = sizeof(t5); memcpy(f + n, t5, sizeof(t5)); n += sizeof(t5);
    uint32_t rl = wifi_rsn_ie(rsn, red, g_sta.akm, g_sta.mfp);
    memcpy(f + n, rsn, rl); n += rl;
    g_sta.ac = 0;
    if (g_wifi_modo >= 1 && red->ht && red->wmm) {                         /* H176 */
        n += wifi_ies_ht_vht_wmm(f + n, red->vht);
        g_sta.ac = red->vht ? 2 : 1;
    }
    for (int i = 0; i < 3 && g_buzon_tipo != 2; i++) { wifi_tx_gestion(f, n); wifi_esperar_buzon(2, 300); }
    if (g_buzon_tipo != 2) { g_sta.error = g_buzon_tipo == 4 ? 21 : 12; goto fin; }
    g_sta.assoc_estado = (uint16_t)(g_buzon[26] | (g_buzon[27] << 8));
    g_sta.aid = (uint16_t)((g_buzon[28] | (g_buzon[29] << 8)) & 0x3FFF);
    if (g_sta.assoc_estado) { g_sta.error = 13; goto fin; }
    g_sta.paso = WIFI_STA_ASSOC;
    wifi_escribir32_mascara(REG_CR, 0x30000, RTW_NET_MGD_LINKED);              /* PORT_SET_NET_TYPE */
    wifi_escribir32_mascara(0x06A8, 0x7FF, g_sta.aid);                         /* PORT_SET_AID */
    /* H176: con VHT a 80 MHz, al ancho del BSS (mac80211 lo fija antes; aqui tras el assoc OK, para que un
     * fallo de 80 MHz se vea como 4 vias sin mensaje 1 y no como asociacion fallida). */
    if (g_sta.ac == 2 && red->vht_ancho == 1 && red->vht_centro > 14) {
        wifi_fijar_canal_ancho(red->vht_centro, RTW_CHANNEL_WIDTH_80, wifi_idx_primario_80(red->canal, red->vht_centro));
        g_sta.ancho = 80;
    } else {
        g_sta.ancho = 20;
    }

    /* 3) Mensaje 1 de 4: ANonce y contador de repeticion. */
    uint32_t el = 0;
    const uint8_t *m = 0;
    for (int i = 0; i < 2000 && !m; i += 50) {
        if (wifi_esperar_buzon(3, 50) == 4) { g_sta.error = 22; goto fin; }
        if (g_buzon_tipo == 3) {
            m = wifi_eapol_del_buzon(&el);
            if (m) {
                uint16_t info = (uint16_t)((m[EK_INFO] << 8) | m[EK_INFO + 1]);
                if (!(info & 0x0080) || (info & 0x0100) || !(info & 0x0008)) m = 0;   /* ACK, sin MIC, pairwise */
                else if ((info & 7) != (g_sta.akm == 8 ? 0 : 2)) { g_sta.error = 14; goto fin; }   /* v2 PSK, v0 SAE */
            }
        }
    }
    if (!m) { g_sta.error = 15; goto fin; }
    g_sta.msg1_vistos++;
    g_sta.eapol_ver = m[0];
    memcpy(anonce, m + EK_NONCE, 32);
    memcpy(replay, m + EK_REPLAY, 8);
    g_sta.paso = WIFI_STA_MSG1;
    cripto_azar_bytes(snonce, 32);
    if (g_sta.akm == 8) wpa_ptk_sha256(pmk, g_sta.bssid, g_wifi.mac, anonce, snonce, ptk);   /* H164 */
    else wpa_ptk(pmk, g_sta.bssid, g_wifi.mac, anonce, snonce, ptk);
    uint16_t vd = g_sta.akm == 8 ? 0 : 2;                                      /* version del descriptor */
    wifi_borrar(pmk, 32);

    /* 4) Mensaje 2 de 4: SNonce, nuestro RSN IE, MIC con KCK. */
    n = wifi_armar_eapol(e, g_sta.eapol_ver, (uint16_t)(0x0108 | vd), replay, snonce, rsn, (uint16_t)rl, ptk);
    uint8_t ultimo_replay[8];
    memcpy(ultimo_replay, replay, 8);
    m = 0;
    for (int intento = 0; intento < 3 && !m; intento++) {
        wifi_tx_eapol(g_sta.bssid, e, n);
        g_sta.paso = WIFI_STA_MSG2;
        for (int i = 0; i < 1000 && !m; i += 50) {
            if (wifi_esperar_buzon(3, 50) == 4) { g_sta.error = 23; goto fin; }
            if (g_buzon_tipo != 3) continue;
            const uint8_t *c = wifi_eapol_del_buzon(&el);
            if (!c) continue;
            uint16_t info = (uint16_t)((c[EK_INFO] << 8) | c[EK_INFO + 1]);
            if (!(info & 0x0100)) { g_sta.msg1_vistos++; continue; }            /* msg1 repetido */
            if (memcmp(c + EK_REPLAY, ultimo_replay, 8) <= 0) continue;           /* replay no crece */
            if (memcmp(c + EK_NONCE, anonce, 32)) continue;
            /* MIC: con el campo MIC en cero */
            static uint8_t copia[600];
            if (el > sizeof(copia)) continue;
            memcpy(copia, c, el);
            memset(copia + EK_MIC, 0, 16);
            uint8_t mic[16];
            if ((info & 7) != vd || !(info & 0x1000)) continue;                  /* version y datos cifrados */
            wifi_mic(info, ptk, copia, el, mic);
            if (memcmp(mic, c + EK_MIC, 16)) { g_sta.error = 16; goto fin; }   /* clave incorrecta */
            m = c;
        }
    }
    if (!m) { g_sta.error = 17; goto fin; }
    g_sta.msg3_vistos++;
    g_sta.paso = WIFI_STA_MSG3;
    memcpy(replay, m + EK_REPLAY, 8);
    {   /* GTK: datos cifrados con AES Key Wrap (KEK) -> KDE 00-0F-AC:1 */
        uint16_t dl = (uint16_t)((m[EK_DLARGO] << 8) | m[EK_DLARGO + 1]);
        static uint8_t claro[300];
        if (dl >= 16 && dl <= sizeof(claro) + 8 && EK_DATOS + dl <= el && !wpa_unwrap(ptk + 16, m + EK_DATOS, dl, claro)) {
            wifi_kde(claro, (uint32_t)dl - 8u);         /* H165: GTK (con la clave) e IGTK */
            wifi_borrar(claro, sizeof(claro));
        } else {
            g_sta.error = 18;
        }
    }

    /* 5) Mensaje 4 de 4: secure, sin datos. */
    n = wifi_armar_eapol(e, g_sta.eapol_ver, (uint16_t)(0x0308 | vd), replay, 0, 0, 0, ptk);
    wifi_tx_eapol(g_sta.bssid, e, n);
    esperar_milisegundos(20);
    if (!g_sta.error && g_sta.mfp && !g_sta.igtk_largo) g_sta.error = 19;      /* PMF sin IGTK */
    if (!g_sta.error) g_sta.paso = WIFI_STA_4VIAS;
    if (g_sta.paso == WIFI_STA_4VIAS) {
        /* H165: la asociacion queda abierta; las claves pasan al enlace de datos. */
        memcpy(g_enlace.kck, ptk, 16); memcpy(g_enlace.kek, ptk + 16, 16); memcpy(g_enlace.tk, ptk + 32, 16);
        memcpy(g_enlace.replay, replay, 8);
        g_enlace.vd = (uint8_t)vd; g_enlace.eapol_ver = g_sta.eapol_ver;
        g_enlace.pn_tx = 0;
        g_enlace.tasa = g_sta.rssi >= -60 ? DESC_RATE54M : DESC_RATE24M;
        g_enlace.qos = g_sta.ac ? 1 : 0;                                         /* H176: WMM anunciado */
        g_enlace.activa = 1;
        g_eth_cab = g_eth_cola = 0;
        wifi_cam_instalar();                                                     /* H179/H180: con el enlace ya activo */
        wifi_borrar(ptk, sizeof(ptk)); wifi_borrar(pmk, sizeof(pmk)); wifi_borrar(snonce, sizeof(snonce));
        return 0;                                 /* g_sta_escucha sigue en 1: deauth del AP */
    }

fin:
    /* Fallo: cierre ordenado (Deauthentication, razon 3: STA se va). Sin PTK el deauth va en claro. */
    if (g_sta.paso >= WIFI_STA_AUTH || g_sta.sae_commits_ap) {
        n = wifi_cabecera(f, 0x00C0, g_sta.bssid);
        f[n++] = 3; f[n++] = 0;
        if (g_sta.mfp && g_sta.paso == WIFI_STA_4VIAS) {
            uint8_t fp[64];
            uint32_t np = (uint32_t)wpa_ccmp_gestion(ptk + 32, 1, 0, f, n, fp);
            if (np) { wifi_tx_gestion(fp, np); g_sta.deauth_protegido = 1; }
            wifi_borrar(fp, sizeof(fp));
        } else {
            wifi_tx_gestion(f, n);
        }
        esperar_milisegundos(20);
        wifi_escribir32_mascara(REG_CR, 0x30000, RTW_NET_NO_LINK);
    }
    wifi_borrar(ptk, sizeof(ptk)); wifi_borrar(pmk, sizeof(pmk)); wifi_borrar(snonce, sizeof(snonce));
    g_sta_escucha = 0;
    return g_sta.paso == WIFI_STA_4VIAS ? 0 : -1;
}

static void wifi_hex2_pre(uint8_t v) {
    const char *h = "0123456789ABCDEF";
    char s[3] = { h[v >> 4], h[v & 0xF], 0 };
    consola_imprimir(s);
}

void wifi_imprimir_conexion(void) { RED_EXCLUSIVA();
    static const char *err[] = {
        [0] = "", [1] = "radio apagada (wifi escanear primero)", [2] = "no se pudo leer /wifi.conf (ssid= y clave=)",
        [3] = "la red de /wifi.conf no aparecio en el ultimo escaneo", [4] = "red en 2.4 GHz (coex con BT pendiente)",
        [5] = "la red no ofrece RSN con CCMP y PSK o SAE", [6] = "la red exige MFP sin SAE (PSK-SHA256 pendiente)",
        [7] = "clave invalida (8-63 caracteres)", [8] = "indice de potencia fuera de rango",
        [10] = "sin respuesta de autenticacion", [11] = "autenticacion rechazada", [12] = "sin respuesta de asociacion",
        [13] = "asociacion rechazada", [14] = "version del descriptor EAPOL no corresponde al AKM", [15] = "no llego el mensaje 1 de 4",
        [16] = "MIC del mensaje 3 incorrecto: la clave no coincide", [17] = "no llego el mensaje 3 de 4",
        [18] = "no se pudo desenvolver la GTK", [19] = "PMF negociado pero el mensaje 3 no trajo IGTK",
        [20] = "deauth durante la autenticacion",
        [21] = "deauth durante la asociacion", [22] = "deauth esperando el mensaje 1", [23] = "deauth esperando el mensaje 3",
        [30] = "SAE: no se hallo el PWE", [31] = "SAE: no se pudo armar el commit",
        [32] = "SAE: el AP rechazo (ver status)", [33] = "SAE: commit del AP invalido",
        [34] = "SAE: sin confirm del AP (clave incorrecta probable)", [35] = "SAE: confirm del AP no verifica",
        [36] = "SAE: el AP pide otro grupo", [37] = "SAE: el AP respondio con otro algoritmo" };
    static const char *pasos[] = { "nada", "autenticado", "asociado", "mensaje 1 recibido", "mensaje 2 enviado",
                                   "mensaje 3 verificado", "4 vias completo" };
    const char *h = "0123456789ABCDEF";
    char b[18];
    for (int i = 0; i < 6; i++) { b[i * 3] = h[g_sta.bssid[i] >> 4]; b[i * 3 + 1] = h[g_sta.bssid[i] & 0xF]; b[i * 3 + 2] = i < 5 ? ':' : 0; }
    consola_imprimir("[WIFI] Conexion a "); consola_imprimir(b);
    consola_imprimir(" canal "); consola_imprimir_dec(g_sta.canal);
    if (g_sta.akm == 8) {
        consola_imprimir("  WPA3-SAE grupo 19, PWE en vuelta "); consola_imprimir_dec(g_sta.sae_contador_pwe);
        consola_imprimir(", commit en "); consola_imprimir_dec(g_sta.sae_ms); consola_imprimir(" ms");
        consola_imprimir("  PMF "); consola_imprimir_linea(g_sta.mfp ? "si" : "no");
        consola_imprimir("       SAE: status "); consola_imprimir_dec(g_sta.sae_estado);
        consola_imprimir("  commits del AP "); consola_imprimir_dec(g_sta.sae_commits_ap);
        consola_imprimir("  tokens "); consola_imprimir_dec(g_sta.sae_tokens);
        consola_imprimir("  IGTK "); consola_imprimir_dec(g_sta.igtk_largo);
        consola_imprimir(" B id "); consola_imprimir_dec(g_sta.igtk_id);
        consola_imprimir_linea("");
    } else {
        consola_imprimir("  WPA2-PSK, PMK en "); consola_imprimir_dec(g_sta.pmk_ms); consola_imprimir_linea(" ms");
    }
    consola_imprimir("       Paso: "); consola_imprimir(pasos[g_sta.paso]);
    consola_imprimir("  auth "); consola_imprimir_dec(g_sta.auth_estado);
    consola_imprimir("  assoc "); consola_imprimir_dec(g_sta.assoc_estado);
    consola_imprimir("  AID "); consola_imprimir_dec(g_sta.aid);
    consola_imprimir("  EAPOL v"); consola_imprimir_dec(g_sta.eapol_ver);
    consola_imprimir("  msg1 x"); consola_imprimir_dec(g_sta.msg1_vistos);
    consola_imprimir("  msg3 x"); consola_imprimir_dec(g_sta.msg3_vistos);
    consola_imprimir("  GTK "); consola_imprimir_dec(g_sta.gtk_largo);
    consola_imprimir(" B id "); consola_imprimir_dec(g_sta.gtk_id);
    consola_imprimir_linea("");
    if (g_sta.error) {
        consola_imprimir("       ERROR "); consola_imprimir_dec((uint64_t)g_sta.error);
        consola_imprimir(": "); consola_imprimir_linea(g_sta.error < (int)(sizeof(err) / sizeof(err[0])) && err[g_sta.error] ? err[g_sta.error] : "?");
        if (g_red_fallida && (g_sta.error == 5 || g_sta.error == 6 || g_sta.error == 13 || g_sta.error == 32 || g_sta.error == 36)) {
            const struct wifi_red *r = g_red_fallida;
            char x[3] = { 0, 0, 0 };
            consola_imprimir("       RSN del AP: ccmp "); consola_imprimir_dec(r->rsn_ccmp);
            consola_imprimir(" psk "); consola_imprimir_dec(r->rsn_psk);
            consola_imprimir(" sae "); consola_imprimir_dec(r->rsn_sae);
            consola_imprimir(" caps 0x"); for (int s = 12; s >= 0; s -= 4) { x[0] = h[(r->rsn_caps >> s) & 0xF]; x[1] = 0; consola_imprimir(x); }
            consola_imprimir("  IE:");
            for (uint32_t i = 0; i < r->rsn_ie_largo; i++) { x[0] = h[r->rsn_ie[i] >> 4]; x[1] = h[r->rsn_ie[i] & 0xF]; consola_imprimir(" "); consola_imprimir(x); }
            consola_imprimir_linea("");
            if (r->rsnx_visto) { consola_imprimir("       RSNXE: 0x"); wifi_hex2_pre(r->rsnx); consola_imprimir_linea((r->rsnx & 0x20) ? " (SAE H2E)" : ""); }
        }
    }
    if (g_enlace.activa)
        consola_imprimir_linea("       Asociacion abierta: PTK y GTK en memoria (CCMP por software). 'wifi red' usa el enlace; 'wifi desconectar' lo cierra.");
    else if (g_sta.paso >= WIFI_STA_AUTH || g_sta.sae_commits_ap)
        consola_imprimir_linea("       Asociacion cerrada con Deauthentication.");
}

/* ================================================================ consola */
static void wifi_hex2(uint8_t v) {
    const char *h = "0123456789ABCDEF";
    char s[3] = { h[v >> 4], h[v & 0xF], 0 };
    consola_imprimir(s);
}
static void wifi_hex4(uint16_t v) { wifi_hex2((uint8_t)(v >> 8)); wifi_hex2((uint8_t)v); }
static void wifi_hex8(uint32_t v) { wifi_hex4((uint16_t)(v >> 16)); wifi_hex4((uint16_t)v); }

void wifi_imprimir_efuse(void) { RED_EXCLUSIVA();
    const struct wifi_info *w = &g_wifi;
    if (!w->efuse_leido) {
        consola_imprimir("[WIFI] Efuse no leido. Error ");
        consola_imprimir_dec((uint64_t)w->error_encendido);
        consola_imprimir(" en registro 0x"); wifi_hex4(w->error_registro);
        consola_imprimir("  (REG_CR antes 0x"); wifi_hex2(w->cr_antes); consola_imprimir_linea(")");
        consola_imprimir_linea("       1=sin MMIO 2=espera de la secuencia de encendido 3=EFUSE_CTRL 4=mapa logico");
        return;
    }
    consola_imprimir("[WIFI] Efuse OK. MAC ");
    for (int i = 0; i < 6; i++) { wifi_hex2(w->mac[i]); if (i < 5) consola_imprimir(":"); }
    consola_imprimir("   rtl_id 0x"); wifi_hex4(w->rtl_id);
    consola_imprimir("   REG_CR antes 0x"); wifi_hex2(w->cr_antes);
    consola_imprimir_linea("");
    consola_imprimir("       Efuse PCI "); wifi_hex4(w->efuse_vid); consola_imprimir(":"); wifi_hex4(w->efuse_did);
    consola_imprimir(" sub "); wifi_hex4(w->efuse_svid); consola_imprimir(":"); wifi_hex4(w->efuse_sdid);
    consola_imprimir("   fisico usado "); consola_imprimir_dec(w->efuse_fisico_usado);
    consola_imprimir("/"); consola_imprimir_dec(EFUSE_FISICO - EFUSE_PROTEGIDO);
    consola_imprimir_linea("");
    consola_imprimir("       RFE "); consola_imprimir_dec(w->rfe);
    consola_imprimir("  cristal "); consola_imprimir_dec(w->cristal);
    consola_imprimir("  plan 0x"); wifi_hex2(w->canal_plan);
    consola_imprimir("  pais "); consola_imprimir(w->pais[0] >= 'A' && w->pais[0] <= 'Z' ? w->pais : "--");
    consola_imprimir("  placa 0x"); wifi_hex2(w->placa);
    consola_imprimir("  BT 0x"); wifi_hex2(w->bt);
    consola_imprimir("  termico "); consola_imprimir_dec(w->termico_a);
    consola_imprimir("/"); consola_imprimir_dec(w->termico_b);
    consola_imprimir_linea("");
    consola_imprimir_linea("       Chip apagado de nuevo (card_disable_flow_8822c).");
}

static const char *wifi_fw_texto(int e) {
    switch (e) {
    case WIFI_FW_SIN_MODULO: return "falta el modulo Limine rtw88:fw (rtw8822c_fw.bin sin comprimir)";
    case WIFI_FW_TAMANO:     return "tamano distinto de la cabecera (check_firmware_size)";
    case WIFI_FW_SIN_DMA:    return "sin buffers DMA bajo 4 GiB";
    case WIFI_FW_ENCENDIDO:  return "fallo el encendido de la MAC";
    case WIFI_FW_LTECOEX:    return "LTECOEX no listo (registro indirecto 0x38)";
    case WIFI_FW_PAGINA:     return "la pagina reservada no quedo valida (BCN_VALID por la cola de beacon PCI)";
    case WIFI_FW_DDMA:       return "el DDMA interno no termino (BIT_DDMACH0_OWN)";
    case WIFI_FW_CHECKSUM:   return "checksum del DDMA invalido";
    case WIFI_FW_VALIDACION: return "REG_MCUFW_CTRL no llego a FW_READY (0xC078)";
    case WIFI_FW_LLAVE:      return "llave de firmware ilegal (REG_FW_DBG7)";
    case WIFI_FW_HW_FEATURE: return "el firmware no respondio C2H_HW_FEATURE_REPORT";
    default:                 return "sin error";
    }
}

void wifi_imprimir_firmware(void) { RED_EXCLUSIVA();
    const struct wifi_info *w = &g_wifi;
    if (w->fw_bytes) {
        consola_imprimir("[WIFI] rtw8822c_fw.bin: "); consola_imprimir_dec(w->fw_bytes);
        consola_imprimir(" B  firma 0x"); wifi_hex4(w->fw_firma);
        consola_imprimir("  v"); consola_imprimir_dec(w->fw_version);
        consola_imprimir("."); consola_imprimir_dec(w->fw_subversion);
        consola_imprimir("."); consola_imprimir_dec(w->fw_subindice);
        consola_imprimir("  "); consola_imprimir_dec(w->fw_anio);
        consola_imprimir("-"); consola_imprimir_dec(w->fw_mes);
        consola_imprimir("-"); consola_imprimir_dec(w->fw_dia);
        consola_imprimir("  H2C v"); consola_imprimir_dec(w->fw_h2c_ver);
        consola_imprimir(w->fw_desde_usb ? "  (USB)" : "  (modulo Limine)");
        consola_imprimir_linea("");
        consola_imprimir("       SHA-256 ");
        for (int i = 0; i < 32; i++) wifi_hex2(w->fw_sha256[i]);
        consola_imprimir_linea("");
        consola_imprimir("       DMEM "); consola_imprimir_dec(w->fw_dmem);
        consola_imprimir("  IMEM "); consola_imprimir_dec(w->fw_imem);
        consola_imprimir("  EMEM "); consola_imprimir_dec(w->fw_emem);
        consola_imprimir("  REG_CR antes 0x"); wifi_hex2(w->cr_antes);
        consola_imprimir_linea("");
    }
    if (!w->fw_cargado || w->fw_error) {
        consola_imprimir("[WIFI] Firmware: FALLO ");
        consola_imprimir_dec((uint64_t)w->fw_error);
        consola_imprimir(" - "); consola_imprimir_linea(wifi_fw_texto(w->fw_error));
        if (w->fw_desde_usb && w->fw_error_vfs) {
            consola_imprimir("       VFS: "); consola_imprimir_linea(volumen_error(w->fw_error_vfs));
        }
        consola_imprimir("       REG_MCUFW_CTRL 0x"); wifi_hex4(w->fw_mcufw_ctrl);
        consola_imprimir("  REG_FW_DBG7 0x"); wifi_hex8(w->fw_dbg7);
        consola_imprimir("  C2HEVT 0x"); wifi_hex2(w->hw_feature_id);
        consola_imprimir("  espera 0x"); wifi_hex4(w->error_registro);
        consola_imprimir_linea("");
        if (w->puente_hallado) {
            consola_imprimir("       Puente PCIe "); consola_imprimir_dec(w->puente_bus); consola_imprimir(":");
            consola_imprimir_dec(w->puente_ranura); consola_imprimir("."); consola_imprimir_dec(w->puente_funcion);
            consola_imprimir(" cmd 0x"); wifi_hex4(w->puente_cmd_antes);
            consola_imprimir(" -> 0x"); wifi_hex4(w->puente_cmd_despues); consola_imprimir_linea("");
        }
        if (w->diag_valido) {
            consola_imprimir("       DIAG PCI cmd 0x"); wifi_hex4(w->diag_pci_cmd);
            consola_imprimir(" estado 0x"); wifi_hex4(w->diag_pci_estado);
            consola_imprimir("  PCI_CTRL 0x"); wifi_hex8(w->diag_pci_ctrl);
            consola_imprimir("  DESA_BCNQ 0x"); wifi_hex8(w->diag_desa_hi); wifi_hex8(w->diag_desa_lo);
            consola_imprimir(" (alto previo 0x"); wifi_hex8(w->diag_desa_hi_previo); consola_imprimir_linea(")");
            consola_imprimir("       DIAG anillo 0x"); wifi_hex8((uint32_t)w->diag_anillo_fis);
            consola_imprimir(" paquete 0x"); wifi_hex8((uint32_t)w->diag_paquete_fis);
            consola_imprimir(" bloque "); consola_imprimir_dec(w->diag_bloque);
            consola_imprimir("  BD psb_len 0x"); wifi_hex4(w->diag_bd_psb);
            consola_imprimir("  BCN_WORK 0x"); wifi_hex2(w->diag_bcn_work);
            consola_imprimir("  FIFOPAGE_CTRL_2 0x"); wifi_hex4(w->diag_fifopage2);
            consola_imprimir_linea("");
            consola_imprimir("       DIAG REG_CR 0x"); wifi_hex2(w->diag_cr);
            consola_imprimir("  TXDMA_STATUS 0x"); wifi_hex8(w->diag_txdma_estado);
            consola_imprimir("  HISR0 0x"); wifi_hex8(w->diag_hisr0);
            consola_imprimir("  HISR1 0x"); wifi_hex8(w->diag_hisr1);
            consola_imprimir("  HISR3 0x"); wifi_hex8(w->diag_hisr3);
            consola_imprimir("  MCUFW 0x"); wifi_hex4(w->diag_mcufw);
            consola_imprimir_linea("");
        }
        if (!w->fw_cargado) return;
    } else {
        consola_imprimir("[WIFI] Firmware CORRIENDO: REG_MCUFW_CTRL 0x"); wifi_hex4(w->fw_mcufw_ctrl);
        consola_imprimir_linea(" (FW_READY)");
        if (w->puente_hallado) {
            consola_imprimir("       Puente PCIe "); consola_imprimir_dec(w->puente_bus); consola_imprimir(":");
            consola_imprimir_dec(w->puente_ranura); consola_imprimir("."); consola_imprimir_dec(w->puente_funcion);
            consola_imprimir(" cmd 0x"); wifi_hex4(w->puente_cmd_antes);
            consola_imprimir(" -> 0x"); wifi_hex4(w->puente_cmd_despues); consola_imprimir_linea("");
        }
    }
    if (w->hw_feature_id == C2H_HW_FEATURE_REPORT) {
        const uint8_t *h = w->hw_feature;
        uint32_t c = le32(h + 4);         /* GET_EFUSE_HW_CAP_*: palabra 1 */
        consola_imprimir("       HW feature:");
        for (int i = 0; i < HW_FEATURE_LEN; i++) { consola_imprimir(" "); wifi_hex2(h[i]); }
        consola_imprimir_linea("");
        consola_imprimir("       hci "); consola_imprimir_dec(c & 0xF);
        consola_imprimir("  bw "); consola_imprimir_dec((c >> 16) & 0x7);
        consola_imprimir("  nss "); consola_imprimir_dec((c >> 19) & 0x3);
        consola_imprimir("  antenas "); consola_imprimir_dec((c >> 21) & 0x7);
        consola_imprimir("  ptcl "); consola_imprimir_dec((c >> 26) & 0x3);
        consola_imprimir_linea("");
    }
    if (w->efuse_leido) {
        consola_imprimir("       MAC efuse ");
        for (int i = 0; i < 6; i++) { wifi_hex2(w->mac[i]); if (i < 5) consola_imprimir(":"); }
        consola_imprimir_linea("");
    }
    consola_imprimir_linea("       Chip apagado de nuevo; bus master desactivado.");
}

void wifi_imprimir_radio(void) { RED_EXCLUSIVA();
    const struct wifi_info *w = &g_wifi;
    if (!w->fw_cargado) { wifi_imprimir_firmware(); return; }
    consola_imprimir("[WIFI] Firmware v"); consola_imprimir_dec(w->fw_version);
    consola_imprimir("."); consola_imprimir_dec(w->fw_subversion);
    consola_imprimir("."); consola_imprimir_dec(w->fw_subindice);
    consola_imprimir("  REG_MCUFW_CTRL 0x"); wifi_hex4(w->fw_mcufw_ctrl);
    consola_imprimir("  MAC "); for (int i = 0; i < 6; i++) { wifi_hex2(w->mac[i]); if (i < 5) consola_imprimir(":"); }
    consola_imprimir_linea("");
    if (!w->fase3_hecha) {
        static const char *f3[] = { "", "LLT: BIT_AUTO_INIT_LLT_V1 no volvio a 0", "init_h2c: H2C queue mismatch",
                                    "set_trx_fifo_info: wrong rsvd driver address", "sin buffers RX" };
        consola_imprimir("[WIFI] rtw_mac_init: FALLO ");
        consola_imprimir_dec((uint64_t)w->fase3_error);
        consola_imprimir(" - "); consola_imprimir_linea(w->fase3_error > 0 && w->fase3_error <= 4 ? f3[w->fase3_error] : "?");
        consola_imprimir("       H2C libre "); consola_imprimir_dec(w->h2cq_libre);
        consola_imprimir("/"); consola_imprimir_dec(w->h2cq_tam); consola_imprimir_linea("");
        return;
    }
    consola_imprimir("[WIFI] rtw_mac_init OK: REG_CR 0x"); wifi_hex8(w->cr_final);
    consola_imprimir("  rsvd_boundary "); consola_imprimir_dec(w->rsvd_limite);
    consola_imprimir("  pubq "); consola_imprimir_dec(w->pubq);
    consola_imprimir("  LLT OK  H2C "); consola_imprimir_dec(w->h2cq_libre);
    consola_imprimir("/"); consola_imprimir_dec(w->h2cq_tam); consola_imprimir_linea(" libre");
    static const char *nombres[6] = { "mac", "bb", "agc", "mp_cal_init", "rf_b", "rf_a" };
    consola_imprimir("[WIFI] Tablas PHY (corte "); consola_imprimir_dec(g_cond_corte);
    consola_imprimir(", pkg "); consola_imprimir_dec(g_cond_pkg);
    consola_imprimir(", RFE "); consola_imprimir_dec(g_cond_rfe); consola_imprimir_linea("):");
    consola_imprimir("      ");
    for (int i = 0; i < 6; i++) {
        consola_imprimir(" "); consola_imprimir(nombres[i]); consola_imprimir(" ");
        consola_imprimir_dec(w->escrituras[i]); consola_imprimir("/-"); consola_imprimir_dec(w->omitidas[i]);
    }
    consola_imprimir_linea("");
    consola_imprimir("       RF A: 0x00=0x"); wifi_hex8(w->rf_a_00);
    consola_imprimir(" 0x18=0x"); wifi_hex8(w->rf_a_18);
    consola_imprimir("   RF B: 0x00=0x"); wifi_hex8(w->rf_b_00);
    consola_imprimir(" 0x18=0x"); wifi_hex8(w->rf_b_18);
    consola_imprimir_linea("");
    consola_imprimir("       Cristal "); consola_imprimir_dec(w->cristal);
    consola_imprimir("  REG_ANAPAR_XTAL_0 0x"); wifi_hex8(w->xtal0);
    consola_imprimir("  RXBD_IDX 0x"); wifi_hex8(w->rx_idx);
    consola_imprimir("  (tramas DMA RX: "); consola_imprimir_dec((w->rx_idx >> 16) & 0xFFF);
    consola_imprimir_linea(")");
    if (w->fase3b_hecha) {
        consola_imprimir("[WIFI] TRX 2T2R: ANTMAP0 0x"); wifi_hex8(w->antmap0);
        consola_imprimir("  TXLGMAP 0x"); wifi_hex8(w->txlgmap);
        consola_imprimir("  RXCCKSEL 0x"); wifi_hex8(w->rxcckscl);
        consola_imprimir("  IGI 0x"); wifi_hex2((uint8_t)w->igi);
        consola_imprimir("  EDCCA 0x84C 0x"); wifi_hex8(w->edcca_84c);
        consola_imprimir_linea("");
        consola_imprimir(w->dack_restaurado ? "[WIFI] DACK: RESTAURADO de la corrida anterior" : "[WIFI] DACK: calculado");
        consola_imprimir("  esperas fallidas "); consola_imprimir_dec(w->dack_esperas_fallidas);
        consola_imprimir_linea("");
        if (!w->dack_restaurado) {
            for (int r = 0; r < 2; r++) {
                consola_imprimir(r == 0 ? "       Ruta A" : "       Ruta B");
                consola_imprimir(": ADCK 0x"); wifi_hex8(w->dack_adck[r]);
                consola_imprimir("  ic/qc 0x"); wifi_hex4((uint16_t)w->dack_ic[r]);
                consola_imprimir("/0x"); wifi_hex4((uint16_t)w->dack_qc[r]);
                consola_imprimir("  i/q 0x"); wifi_hex4((uint16_t)w->dack_i[r]);
                consola_imprimir("/0x"); wifi_hex4((uint16_t)w->dack_q[r]);
                consola_imprimir("  vueltas "); consola_imprimir_dec(w->dack_vueltas[r]);
                consola_imprimir_linea("");
            }
        }
        consola_imprimir("[WIFI] x2k "); consola_imprimir(w->x2k_ocupado ? "ocupado (recalibrado)" : "libre");
        consola_imprimir("  thermal trim A/B 0x"); wifi_hex2(w->termico_trim[0]);
        consola_imprimir("/0x"); wifi_hex2(w->termico_trim[1]);
        consola_imprimir("  PA bias 2G 0x"); wifi_hex2(w->pa_bias_2g[0]); consola_imprimir("/0x"); wifi_hex2(w->pa_bias_2g[1]);
        consola_imprimir(" 5G 0x"); wifi_hex2(w->pa_bias_5g[0]); consola_imprimir("/0x"); wifi_hex2(w->pa_bias_5g[1]);
        consola_imprimir_linea("");
        consola_imprimir("       Power trim "); consola_imprimir(w->power_trim_aplicado ? "aplicado:" : "sin datos en efuse");
        for (int r = 0; r < 2 && w->power_trim_aplicado; r++) {
            consola_imprimir(r == 0 ? " A" : "  B");
            for (int i = 0; i < 8; i++) { consola_imprimir(" "); consola_imprimir_dec((uint64_t)(uint8_t)w->bb_gain[r][i]); }
        }
        consola_imprimir_linea("");
    }
    consola_imprimir_linea("       Siguiente: set_channel y RX de beacons. Chip apagado de nuevo.");
}

void wifi_imprimir_estado(void) { RED_EXCLUSIVA();
    const struct wifi_info *w = &g_wifi;
    if (!w->presente) {
        consola_imprimir_linea("[WIFI] RTL8822CE (10ec:c822) no detectada.");
        return;
    }
    consola_imprimir("[WIFI] Realtek RTL8822CE en PCI ");
    consola_imprimir_dec(w->bus); consola_imprimir(":");
    consola_imprimir_dec(w->ranura); consola_imprimir(".");
    consola_imprimir_dec(w->funcion);
    consola_imprimir("  sub "); wifi_hex4(w->sub_proveedor);
    consola_imprimir(":"); wifi_hex4(w->sub_dispositivo);
    consola_imprimir("  rev "); wifi_hex2(w->revision_pci);
    consola_imprimir_linea("");
    if (!w->mmio_listo) { consola_imprimir_linea("       MMIO sin mapear."); return; }
    consola_imprimir("       BAR MMIO 0x"); wifi_hex8((uint32_t)w->bar2_fisica);
    consola_imprimir(" ("); consola_imprimir_dec(w->bar2_tamano / 1024); consola_imprimir_linea(" KiB)");
    consola_imprimir("       REG_SYS_CFG1 = 0x"); wifi_hex8(w->sys_cfg1);
    consola_imprimir("   REG_SYS_CFG2 = 0x"); wifi_hex8(w->sys_cfg2);
    consola_imprimir_linea("");
    if (w->sys_cfg1 == 0xFFFFFFFFu) {
        consola_imprimir_linea("       El chip no responde por MMIO (lee 0xFFFFFFFF).");
        return;
    }
    char corte[2] = { (char)('A' + (w->corte & 0xF)), 0 };
    consola_imprimir("       Corte "); consola_imprimir(corte);
    consola_imprimir("  RF "); consola_imprimir_dec(w->cadenas_rf);
    consola_imprimir("T"); consola_imprimir_dec(w->cadenas_rf);
    consola_imprimir("R  proveedor "); consola_imprimir_dec(w->proveedor_chip);
    consola_imprimir(w->chip_mp ? "  chip MP" : "  chip de prueba (no MP)");
    consola_imprimir_linea("");
    consola_imprimir_linea("       Driver propio. Ordenes: 'wifi efuse', 'wifi firmware'.");
}
