#include "usb_msc.h"
#include "xhci.h"
#include "consola.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/dma.h"
#include "../base/memoria.h"
#include "../base/tiempo.h"

// ============================================================================
// TAEK OS - CONTROLADOR USB MASS STORAGE CLASS (MSC / SCSI BOT)
// Arquitectura Anillo 0 en Español (Hito 48)
// ============================================================================

static struct usb_msc_dispositivo g_msc_dispositivos[USB_MSC_MAX_DISPOSITIVOS] = {0};
static int g_msc_inicializado = 0;

// Búferes DMA contiguos físicos para transacciones BOT
static struct usb_msc_cbw *g_cbw = NULL;
static uint64_t            g_cbw_fisica = 0;

static struct usb_msc_csw *g_csw = NULL;
static uint64_t            g_csw_fisica = 0;

static uint8_t            *g_msc_dma_buffer = NULL;
static uint64_t            g_msc_dma_buffer_fisica = 0;

static uint32_t            g_etiqueta_actual = 0x20260900U;

void usb_msc_iniciar(void) {
    if (g_msc_inicializado) return;

    for (int i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
        g_msc_dispositivos[i].activo = 0;
        g_msc_dispositivos[i].listo = 0;
    }

    // Asignar búferes DMA contiguos alineados a 64 bytes
    g_cbw = (struct usb_msc_cbw *)dma_asignar_bufer_contiguo(sizeof(struct usb_msc_cbw), 64, &g_cbw_fisica);
    g_csw = (struct usb_msc_csw *)dma_asignar_bufer_contiguo(sizeof(struct usb_msc_csw), 64, &g_csw_fisica);
    g_msc_dma_buffer = (uint8_t *)dma_asignar_bufer_contiguo(65536, 64, &g_msc_dma_buffer_fisica);

    if (g_cbw && g_csw && g_msc_dma_buffer) {
        g_msc_inicializado = 1;
        serial_imprimir_linea("[USB MSC] Subsistema Bulk-Only Transport inicializado en Ring 0 (DMA 64KB OK).");
    } else {
        serial_imprimir_linea("[USB MSC ERROR] No se pudieron asignar búferes DMA para Mass Storage.");
    }
}

// Ejecuta una transacción SCSI completa de 3 fases bajo el protocolo BOT
static int usb_msc_ejecutar_transaccion(struct usb_msc_dispositivo *dev,
                                       const void *cdb, uint8_t cdb_len,
                                       void *datos, uint32_t datos_len, int es_in) {
    if (!g_msc_inicializado || !dev || !dev->activo) return -1;
    if (!g_cbw || !g_csw || !g_msc_dma_buffer) return -2;

    uint32_t etiqueta = g_etiqueta_actual++;

    // --- FASE 1: Command Block Wrapper (CBW - 31 bytes enviados por Bulk OUT) ---
    for (uint32_t i = 0; i < sizeof(struct usb_msc_cbw); i++) ((uint8_t *)g_cbw)[i] = 0;
    g_cbw->firma = USB_MSC_CBW_FIRMA;
    g_cbw->etiqueta = etiqueta;
    g_cbw->longitud_transfer = datos_len;
    g_cbw->banderas = es_in ? USB_MSC_DIR_IN : USB_MSC_DIR_OUT;
    g_cbw->lun = 0;
    g_cbw->cdb_longitud = cdb_len;
    for (int i = 0; i < cdb_len && i < 16; i++) {
        g_cbw->cdb[i] = ((const uint8_t *)cdb)[i];
    }

    int res = xhci_transferencia_bulk(dev->slot_id, dev->ep_out_dci,
                                     g_cbw, g_cbw_fisica,
                                     sizeof(struct usb_msc_cbw), 0 /* OUT */, 2000);
    if (res != 0) {
        serial_imprimir("  [USB MSC] Error enviando CBW (código: ");
        serial_imprimir_dec(res);
        serial_imprimir_linea(")");
        return -10;
    }

    // --- FASE 2: Data Phase (opcional, si hay datos a transferir) ---
    if (datos_len > 0 && datos) {
        uint64_t buf_fisica = g_msc_dma_buffer_fisica;
        void *buf_ptr = g_msc_dma_buffer;

        // Si es OUT (escritura de datos al dispositivo), copiar datos al búfer DMA
        if (!es_in) {
            for (uint32_t i = 0; i < datos_len && i < 65536; i++) {
                ((uint8_t *)buf_ptr)[i] = ((const uint8_t *)datos)[i];
            }
        }

        uint8_t target_dci = es_in ? dev->ep_in_dci : dev->ep_out_dci;
        res = xhci_transferencia_bulk(dev->slot_id, target_dci,
                                     buf_ptr, buf_fisica,
                                     datos_len, es_in, 3000);
        if (res != 0) {
            serial_imprimir("  [USB MSC] Error en Data Phase (código: ");
            serial_imprimir_dec(res);
            serial_imprimir_linea(")");
            return -11;
        }

        // Si es IN (lectura de datos desde el dispositivo), copiar datos recibidos al búfer destino
        if (es_in) {
            for (uint32_t i = 0; i < datos_len && i < 65536; i++) {
                ((uint8_t *)datos)[i] = ((const uint8_t *)buf_ptr)[i];
            }
        }
    }

    // --- FASE 3: Command Status Wrapper (CSW - 13 bytes recibidos por Bulk IN) ---
    for (uint32_t i = 0; i < sizeof(struct usb_msc_csw); i++) ((uint8_t *)g_csw)[i] = 0;
    res = xhci_transferencia_bulk(dev->slot_id, dev->ep_in_dci,
                                 g_csw, g_csw_fisica,
                                 sizeof(struct usb_msc_csw), 1 /* IN */, 2000);
    if (res != 0) {
        serial_imprimir("  [USB MSC] Error recibiendo CSW (código: ");
        serial_imprimir_dec(res);
        serial_imprimir_linea(")");
        return -12;
    }

    // --- FASE 4: Validación y Chequeo de Integridad del CSW ---
    if (g_csw->firma != USB_MSC_CSW_FIRMA) {
        serial_imprimir("  [USB MSC ERROR] Firma CSW inválida: 0x");
        serial_imprimir_hex(g_csw->firma);
        serial_imprimir_linea("");
        return -13;
    }

    if (g_csw->etiqueta != etiqueta) {
        serial_imprimir_linea("  [USB MSC ERROR] Desfase en etiqueta de CSW");
        return -14;
    }

    if (g_csw->estado != USB_MSC_CSW_ESTADO_OK) {
        serial_imprimir("  [USB MSC AVISO] Comando SCSI rechazado (CSW Estado: ");
        serial_imprimir_dec(g_csw->estado);
        serial_imprimir_linea(")");
        return -15;
    }

    return 0; // Transacción SCSI BOT exitosa
}

int usb_msc_registrar_dispositivo(uint8_t slot_id, uint8_t puerto_idx,
                                  uint8_t ep_in_dci, uint8_t ep_out_dci,
                                  uint16_t ep_in_max_pkt, uint16_t ep_out_max_pkt) {
    if (!g_msc_inicializado) usb_msc_iniciar();

    // Localizar una ranura libre en g_msc_dispositivos
    int idx = -1;
    for (int i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
        if (!g_msc_dispositivos[i].activo) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        serial_imprimir_linea("  [USB MSC] Tabla de dispositivos de almacenamiento llena.");
        return -1;
    }

    struct usb_msc_dispositivo *dev = &g_msc_dispositivos[idx];
    dev->activo = 1;
    dev->slot_id = slot_id;
    dev->puerto_idx = puerto_idx;
    dev->ep_in_dci = ep_in_dci;
    dev->ep_out_dci = ep_out_dci;
    dev->ep_in_max_pkt = ep_in_max_pkt;
    dev->ep_out_max_pkt = ep_out_max_pkt;
    dev->listo = 0;
    dev->sectores_totales = 0;
    dev->tamano_sector = 512;
    dev->capacidad_bytes = 0;

    for (int i = 0; i < 9; i++) dev->fabricante[i] = ' ';
    dev->fabricante[8] = '\0';
    for (int i = 0; i < 17; i++) dev->producto[i] = ' ';
    dev->producto[16] = '\0';
    for (int i = 0; i < 5; i++) dev->revision[i] = ' ';
    dev->revision[4] = '\0';

    serial_imprimir("  [USB MSC] Inicializando unidad en Slot ");
    serial_imprimir_dec(slot_id);
    serial_imprimir(" (Puerto ");
    serial_imprimir_dec(puerto_idx);
    serial_imprimir_linea(")...");

    // 1. SCSI TEST UNIT READY (con hasta 10 reintentos para dar tiempo a pendrives físicos)
    uint8_t cdb_tur[6] = {SCSI_CMD_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    int tur_ok = 0;
    for (int intento = 0; intento < 10; intento++) {
        if (usb_msc_ejecutar_transaccion(dev, cdb_tur, 6, NULL, 0, 1) == 0) {
            tur_ok = 1;
            break;
        }
        esperar_milisegundos(50);
    }

    if (!tur_ok) {
        serial_imprimir_linea("  [USB MSC] TEST UNIT READY no respondió listo de inmediato, continuando con INQUIRY...");
    }

    // 2. SCSI INQUIRY (36 bytes de telemetría de fabricante y modelo)
    uint8_t inq_buf[36] = {0};
    uint8_t cdb_inq[6] = {SCSI_CMD_INQUIRY, 0, 0, 0, 36, 0};
    if (usb_msc_ejecutar_transaccion(dev, cdb_inq, 6, inq_buf, 36, 1) == 0) {
        for (int i = 0; i < 8; i++) {
            char c = (char)inq_buf[8 + i];
            dev->fabricante[i] = (c >= 32 && c <= 126) ? c : ' ';
        }
        dev->fabricante[8] = '\0';

        for (int i = 0; i < 16; i++) {
            char c = (char)inq_buf[16 + i];
            dev->producto[i] = (c >= 32 && c <= 126) ? c : ' ';
        }
        dev->producto[16] = '\0';

        for (int i = 0; i < 4; i++) {
            char c = (char)inq_buf[32 + i];
            dev->revision[i] = (c >= 32 && c <= 126) ? c : ' ';
        }
        dev->revision[4] = '\0';

        serial_imprimir("  [USB MSC] INQUIRY [OK] Fabricante: '");
        serial_imprimir(dev->fabricante);
        serial_imprimir("' Modelo: '");
        serial_imprimir(dev->producto);
        serial_imprimir("' Rev: '");
        serial_imprimir(dev->revision);
        serial_imprimir_linea("'");
    }

    // 3. SCSI READ CAPACITY 10 (8 bytes: LBA máximo y tamaño de sector)
    uint8_t cap_buf[8] = {0};
    uint8_t cdb_cap[10] = {SCSI_CMD_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (usb_msc_ejecutar_transaccion(dev, cdb_cap, 10, cap_buf, 8, 1) == 0) {
        uint32_t max_lba = ((uint32_t)cap_buf[0] << 24) |
                           ((uint32_t)cap_buf[1] << 16) |
                           ((uint32_t)cap_buf[2] << 8)  |
                           ((uint32_t)cap_buf[3]);
        uint32_t blk_size = ((uint32_t)cap_buf[4] << 24) |
                            ((uint32_t)cap_buf[5] << 16) |
                            ((uint32_t)cap_buf[6] << 8)  |
                            ((uint32_t)cap_buf[7]);

        if (blk_size == 0) blk_size = 512;
        dev->sectores_totales = max_lba + 1;
        dev->tamano_sector = blk_size;
        dev->capacidad_bytes = (uint64_t)dev->sectores_totales * (uint64_t)dev->tamano_sector;
        dev->listo = 1;

        uint64_t cap_mb = dev->capacidad_bytes / (1024ULL * 1024ULL);
        uint64_t cap_gb = dev->capacidad_bytes / (1024ULL * 1024ULL * 1024ULL);

        consola_imprimir("    ==> ¡Memoria USB Lista: ");
        consola_imprimir(dev->fabricante);
        consola_imprimir(" ");
        consola_imprimir(dev->producto);
        consola_imprimir(" (");
        if (cap_gb > 0) {
            consola_imprimir_dec(cap_gb);
            consola_imprimir(" GB / ");
        }
        consola_imprimir_dec(cap_mb);
        consola_imprimir(" MB) [OK]!\n");

        serial_imprimir("  [USB MSC] READ CAPACITY [OK]: ");
        serial_imprimir_dec(dev->sectores_totales);
        serial_imprimir(" sectores de ");
        serial_imprimir_dec(dev->tamano_sector);
        serial_imprimir(" bytes (Total: ");
        serial_imprimir_dec(cap_mb);
        serial_imprimir_linea(" MB)");
    } else {
        consola_imprimir_linea_color("    [!] READ CAPACITY falló o medio no montado aún.", COLOR_AVISO_DEFAULT);
    }

    return idx;
}

void usb_msc_desregistrar_dispositivo(uint8_t slot_id) {
    for (int i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
        if (g_msc_dispositivos[i].activo && g_msc_dispositivos[i].slot_id == slot_id) {
            g_msc_dispositivos[i].activo = 0;
            g_msc_dispositivos[i].listo = 0;
            serial_imprimir("  [USB MSC] Unidad en Slot ");
            serial_imprimir_dec(slot_id);
            serial_imprimir_linea(" desregistrada (dispositivo desconectado).");
        }
    }
}

int usb_msc_leer_sectores(uint8_t id_unidad, uint32_t lba, uint16_t cantidad, void *buffer_destino) {
    if (id_unidad >= USB_MSC_MAX_DISPOSITIVOS) return -1;
    struct usb_msc_dispositivo *dev = &g_msc_dispositivos[id_unidad];
    if (!dev->activo || !dev->listo) return -2;
    if (cantidad == 0) return 0;

    uint32_t tamano_total = (uint32_t)cantidad * dev->tamano_sector;
    if (tamano_total > 65536) return -3; // Límite de búfer DMA por operación

    uint8_t cdb_read[10] = {
        SCSI_CMD_READ_10,
        0,
        (uint8_t)((lba >> 24) & 0xFF),
        (uint8_t)((lba >> 16) & 0xFF),
        (uint8_t)((lba >> 8) & 0xFF),
        (uint8_t)(lba & 0xFF),
        0,
        (uint8_t)((cantidad >> 8) & 0xFF),
        (uint8_t)(cantidad & 0xFF),
        0
    };

    return usb_msc_ejecutar_transaccion(dev, cdb_read, 10, buffer_destino, tamano_total, 1 /* IN */);
}

int usb_msc_obtener_cantidad(void) {
    int total = 0;
    for (int i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
        if (g_msc_dispositivos[i].activo) total++;
    }
    return total;
}

const struct usb_msc_dispositivo *usb_msc_obtener_dispositivo(uint8_t id_unidad) {
    if (id_unidad >= USB_MSC_MAX_DISPOSITIVOS) return NULL;
    if (!g_msc_dispositivos[id_unidad].activo) return NULL;
    return &g_msc_dispositivos[id_unidad];
}
