#ifndef CONTROLADORES_RED_WIFI_RTW88_H
#define CONTROLADORES_RED_WIFI_RTW88_H

#include <stdint.h>

/* Driver WiFi propio para la Realtek RTL8822CE (10ec:c822). Plan:
 * OS_experimental/planes/PLAN_WIFI_RTL8822CE.md. Cada funcion cita la de rtw88
 * (Linux 6.6.78) que replica; registros, tablas y secuencias salen de la fuente
 * con herramientas/intel/extraer_tablas_rtw88.py. */

/* Ventana libre: 0x...06 es e1000, 0x...07 es I2C DesignWare. */
#define WIFI_MMIO_VIRTUAL_BASE 0xFFFFFE0008000000ULL
#define WIFI_MMIO_MAX          (64u * 1024u)

struct wifi_info {
    int      presente;
    int      mmio_listo;
    uint8_t  bus, ranura, funcion;
    uint16_t id_proveedor, id_dispositivo;
    uint16_t sub_proveedor, sub_dispositivo;
    uint8_t  revision_pci;
    uint64_t bar2_fisica;
    uint64_t bar2_tamano;
    uint32_t sys_cfg1;      /* REG_SYS_CFG1 (0x00F0) crudo */
    uint32_t sys_cfg2;      /* REG_SYS_CFG2 (0x00FC) crudo */
    uint8_t  corte;         /* BIT_GET_CHIP_VER: 0=A, 1=B, ... */
    uint8_t  proveedor_chip;/* BIT_GET_VENDOR_ID */
    uint8_t  cadenas_rf;    /* 2 si BIT_RF_TYPE_ID (2T2R), si no 1 */
    uint8_t  chip_mp;       /* 1 = chip de produccion (BIT_RTL_ID en 0) */

    /* H154: encendido de la MAC y efuse. */
    int      efuse_leido;
    int      error_encendido;     /* 0 o codigo WIFI_ERR_* */
    uint16_t error_registro;      /* registro/direccion de la ultima espera fallida */
    uint8_t  cr_antes;            /* REG_CR antes de encender (0xEA = apagado) */
    uint8_t  mac[6];              /* efuse logico 0x120 (rtw8822ce_efuse.mac_addr) */
    uint16_t rtl_id;              /* efuse 0x00 */
    uint16_t efuse_vid, efuse_did, efuse_svid, efuse_sdid; /* efuse 0x126.. */
    uint8_t  canal_plan, cristal, rfe, placa, bt, termico_a, termico_b;
    char     pais[3];
    uint16_t efuse_fisico_usado;  /* bytes fisicos con datos */

    /* H155: firmware. */
    int      fw_cargado;
    int      fw_error;            /* 0 o WIFI_FW_* */
    uint64_t fw_bytes;
    uint16_t fw_firma, fw_version, fw_h2c_ver;
    uint8_t  fw_subversion, fw_subindice, fw_mes, fw_dia, fw_hora, fw_minuto;
    uint16_t fw_anio;
    uint32_t fw_dmem, fw_imem, fw_emem; /* tamanos sin checksum */
    uint16_t fw_mcufw_ctrl;       /* REG_MCUFW_CTRL al terminar (FW_READY = 0xC078) */
    uint32_t fw_dbg7;             /* REG_FW_DBG7 si fallo la validacion */
    uint8_t  hw_feature_id;       /* REG_C2HEVT: 0x19 = C2H_HW_FEATURE_REPORT */
    uint8_t  hw_feature[13];      /* HW_FEATURE_LEN */
    uint8_t  fw_sha256[32];
    int      fw_desde_usb;        /* 1 = leido del USB por VFS, 0 = modulo Limine */
    int      fw_error_vfs;        /* codigo de vfs_leer_archivo_binario si fallo */

    /* H156: foto de diagnostico del DMA de pagina reservada (primer fallo). */
    int      diag_valido;
    uint16_t diag_pci_cmd, diag_pci_estado;
    uint32_t diag_pci_ctrl, diag_desa_lo, diag_desa_hi, diag_desa_hi_previo;
    uint8_t  diag_bcn_work;
    uint16_t diag_fifopage2, diag_bd_psb, diag_mcufw;
    uint8_t  diag_cr;
    uint32_t diag_txdma_estado, diag_hisr0, diag_hisr1, diag_hisr3;
    uint64_t diag_anillo_fis, diag_paquete_fis;
    uint32_t diag_bloque;         /* bytes del bloque que fallo */

    /* H157: puente PCIe aguas arriba (root port) que debe reenviar el DMA. */
    int      puente_hallado;
    uint8_t  puente_bus, puente_ranura, puente_funcion;
    uint16_t puente_cmd_antes, puente_cmd_despues;

    /* H158: rtw_mac_init() + carga de tablas PHY (primera mitad de phy_set_param). */
    int      fase3_hecha;
    int      fase3_error;         /* 0 o WIFI_F3_* */
    uint16_t rsvd_limite;         /* fifo.rsvd_boundary */
    uint16_t pubq;                /* paginas de la cola publica */
    uint32_t h2cq_libre, h2cq_tam;
    uint32_t escrituras[6];       /* por tabla: mac, bb, agc, mp_cal_init, rf_b, rf_a */
    uint32_t omitidas[6];         /* pares descartados por condicion */
    uint32_t rf_a_00, rf_a_18, rf_b_00, rf_b_18; /* lectura directa de RF tras cargar */
    uint32_t xtal0;               /* REG_ANAPAR_XTAL_0 tras fijar el cristal */
    uint32_t rx_idx;              /* RTK_PCI_RXBD_IDX_MPDUQ al terminar (hw 27:16, host 11:0) */
    uint32_t cr_final;            /* REG_CR tras txdma_queue_mapping */

    /* H159: segunda mitad de rtw8822c_phy_set_param(). */
    int      fase3b_hecha;
    int      dack_restaurado;     /* 1 = rtw8822c_dac_cal_restore() reuso vectores previos */
    uint32_t dack_ic[2], dack_qc[2];   /* ic/qc de step2 por ruta (A, B) */
    uint32_t dack_i[2], dack_q[2];     /* i/q tras step3 */
    uint32_t dack_adck[2];             /* dm_info->dack_adck */
    uint32_t dack_vueltas[2];          /* iteraciones de step1..3 por ruta */
    uint32_t dack_esperas_fallidas;    /* check_hw_ready() en falso durante DACK */
    uint8_t  x2k_ocupado;
    uint8_t  termico_trim[2];          /* valor escrito en RF 0x43 o 0xFF si no habia efuse */
    uint8_t  pa_bias_2g[2], pa_bias_5g[2];
    int      power_trim_aplicado;
    int8_t   bb_gain[2][8];
    uint32_t igi;                      /* REG_RXIGI & 0x7f tras toggle_igi */
    uint32_t edcca_84c;
    uint32_t antmap0, txlgmap, rxcckscl;

    /* H160: radio encendida, canal y RX. */
    int      radio_activa;
    uint8_t  canal;                    /* canal central sintonizado */
    uint8_t  ancho;                    /* H176: RTW_CHANNEL_WIDTH_20 (0) u 80 (2) */
    uint32_t rx_tramas, rx_crc, rx_c2h, rx_mgmt, rx_beacons, rx_tag_mal, rx_sin_phy;
    uint32_t rx_ht, rx_vht;            /* H176: MPDU por tipo de tasa (RX desc rx_rate) */
    uint8_t  rx_tasa;                  /* ultima tasa DESC_RATE* recibida en datos */

    /* H161: potencia TX y TX de gestion. */
    uint8_t  pot_base[2], pot_idx_6m[2], pot_idx_mcs7[2], pot_idx_1m[2];
    int8_t   pot_offset_6m[2], pot_limite_6m[2];
    uint32_t tx_enviadas, tx_hw_rp;
    uint32_t sonda_respuestas;     /* probe responses con addr1 == nuestra MAC */
    int8_t   sonda_rssi;
};

#define WIFI_MAX_REDES 48
struct wifi_red {
    uint8_t  bssid[6];
    char     ssid[33];
    uint8_t  canal;               /* DS Parameter Set, o el canal sintonizado */
    int8_t   rssi;                /* dBm (estado PHY, maximo visto) */
    uint8_t  privada;             /* capability bit 4 */
    uint8_t  rsn;                 /* IE 48 presente (WPA2/WPA3) */
    uint8_t  wpa;                 /* IE 221 de Microsoft WPA presente */
    uint16_t vistas;
    /* H163: RSN del AP */
    uint8_t  rsn_grupo[4];        /* suite de cifrado de grupo */
    uint8_t  rsn_ccmp;            /* pairwise incluye 00-0F-AC:4 */
    uint8_t  rsn_psk;             /* AKM incluye 00-0F-AC:2 */
    uint8_t  rsn_sae;             /* AKM incluye 00-0F-AC:8 (WPA3) */
    uint16_t rsn_caps;
    uint8_t  rsn_ie[40];          /* cuerpo del RSN IE (publico, va en cada beacon) */
    uint8_t  rsn_ie_largo;
    uint8_t  rsnx;                /* H164: primer octeto del RSNXE (IE 244); bit 5 = SAE H2E */
    uint8_t  rsnx_visto;
    uint8_t  ht, vht, wmm;        /* H176: IE 45, IE 191, WMM (00-50-F2:2) */
    uint8_t  vht_ancho, vht_centro;   /* VHT Operation: 1 = 80 MHz, canal central del segmento 0 */
};

/* H163: estado de la conexion STA (sin claves: nunca se guardan aqui). */
struct wifi_sta {
    int      paso;                /* ultimo paso alcanzado, ver WIFI_STA_* */
    int      error;
    uint16_t auth_estado, assoc_estado, aid;
    uint8_t  bssid[6];
    uint8_t  canal;
    int8_t   rssi;
    uint32_t pmk_ms;              /* tiempo de PBKDF2 */
    uint8_t  eapol_ver, gtk_largo, gtk_id;
    uint32_t msg1_vistos, msg3_vistos;
    /* H164: WPA3-SAE y PMF */
    uint8_t  akm;                 /* 2 = PSK, 8 = SAE */
    uint8_t  mfp;                 /* PMF negociado (MFPC del AP y nuestro) */
    uint16_t sae_estado;          /* status del commit del AP */
    uint32_t sae_contador_pwe, sae_ms, sae_tokens, sae_commits_ap;
    uint16_t igtk_id;
    uint8_t  igtk_largo;
    uint8_t  deauth_protegido;    /* el Deauthentication final salio con CCMP */
    uint8_t  ac;                  /* H176: 0 = 11a, 1 = HT, 2 = VHT anunciados en el assoc */
    uint8_t  ancho;               /* 20 u 80 MHz tras la asociacion */
};
#define WIFI_STA_AUTH      1
#define WIFI_STA_ASSOC     2
#define WIFI_STA_MSG1      3
#define WIFI_STA_MSG2      4
#define WIFI_STA_MSG3      5
#define WIFI_STA_4VIAS     6

int  wifi_conectar(void);
void wifi_imprimir_conexion(void);
/* H165: enlace de datos (CCMP por software) e interfaz de la pila IPv4. */
int  wifi_enlazado(void);
void wifi_desconectar(void);
int  wifi_red_usar(uint32_t espera_stream_ms);   /* 0 = DHCP por WiFi y stream reconectado */
int  wifi_red_cable(void);
int  wifi_preferida(void);                   /* H166: existe /wifi.auto */
int  wifi_red_automatica(void);              /* H166: arranque sin cable; un intento por minuto */
void wifi_imprimir_enlace(void);
int  wifi_modo(int modo);
int  wifi_cam(int pedir);                       /* H179: CCMP RX por hardware: 1 = on, 0 = off, -1 = consultar */
void wifi_ciclos(uint64_t *rx, uint64_t *ccmp_rx, uint64_t *tx);   /* H178: ciclos TSC acumulados */                      /* H176: 0 = 11a, 1 = 11ac, 2 = 11ac + Block Ack; -1 = consultar */

#define WIFI_F3_LLT             1   /* BIT_AUTO_INIT_LLT_V1 no volvio a 0 */
#define WIFI_F3_H2CQ            2   /* "H2C queue mismatch" de init_h2c() */
#define WIFI_F3_FIFO            3   /* "wrong rsvd driver address" */
#define WIFI_F3_RX_DMA          4   /* sin buffers RX bajo 4 GiB */

#define WIFI_ERR_SIN_MMIO       1
#define WIFI_ERR_ESPERA_PWR     2
#define WIFI_ERR_EFUSE          3
#define WIFI_ERR_EFUSE_LOGICO   4

#define WIFI_FW_SIN_MODULO      1   /* falta el modulo Limine rtw88:fw */
#define WIFI_FW_TAMANO          2   /* check_firmware_size() fallo */
#define WIFI_FW_SIN_DMA         3   /* sin buffers DMA bajo 4 GiB */
#define WIFI_FW_ENCENDIDO       4   /* rtw_mac_power_on() fallo */
#define WIFI_FW_LTECOEX         5   /* ltecoex_read_reg/ltecoex_reg_write */
#define WIFI_FW_PAGINA          6   /* la pagina reservada no quedo valida (BCN_VALID) */
#define WIFI_FW_DDMA            7   /* el DDMA interno no termino */
#define WIFI_FW_CHECKSUM        8   /* BIT_DDMACH0_CHKSUM_STS */
#define WIFI_FW_VALIDACION      9   /* REG_MCUFW_CTRL != FW_READY */
#define WIFI_FW_LLAVE          10   /* REG_FW_DBG7 = ILLEGAL_KEY_GROUP */
#define WIFI_FW_HW_FEATURE     11   /* REG_C2HEVT != C2H_HW_FEATURE_REPORT */

int wifi_iniciar(void);
const struct wifi_info *wifi_obtener_info(void);
void wifi_imprimir_estado(void);

/* Enciende la MAC (card_enable_flow_8822c), vuelca el efuse y apaga. 0 = OK. */
int wifi_leer_efuse(void);
void wifi_imprimir_efuse(void);

/* H155: rtw_chip_efuse_info_setup() completo: hci_setup, power_on, descarga de
 * rtw8822c_fw.bin (modulo Limine "rtw88:fw") por pagina reservada + DDMA,
 * validacion FW_READY, efuse y C2H_HW_FEATURE_REPORT; luego apaga. 0 = OK.
 * ruta_usb != NULL: lee el firmware del USB montado (p. ej. "/rtw8822c_fw.bin"). */
int wifi_cargar_firmware(const char *ruta_usb);
void wifi_imprimir_firmware(void);

/* H158: firmware + rtw_mac_init() + tablas MAC/BB/AGC/RFK/RF y cristal (rtw_power_on
 * hasta la mitad de rtw8822c_phy_set_param). Con anillo RX real; al final apaga. */
int wifi_iniciar_radio(const char *ruta_usb);
void wifi_imprimir_radio(void);

/* H160: deja la radio encendida (firmware + mac_init + phy) para escanear. */
int  wifi_encender(const char *ruta_usb);
void wifi_apagar(void);
/* rtw8822c_set_channel() a 20 MHz. */
void wifi_fijar_canal(uint8_t canal);
/* Escaneo pasivo: sintoniza cada canal, escucha y reune beacons/probe responses. */
int  wifi_escanear(uint32_t ms_por_canal);
void wifi_imprimir_escaneo(void);

/* H161: probe request dirigido (solo a una red ya oida en el ultimo escaneo) y espera
 * de probe responses a nuestra MAC. Devuelve respuestas recibidas o <0. */
int  wifi_sondear(const char *ssid);
void wifi_imprimir_sonda(void);

#endif
