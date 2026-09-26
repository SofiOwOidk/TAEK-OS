#ifndef CONTROLADORES_USB_MSC_H
#define CONTROLADORES_USB_MSC_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// TAEK OS - SUBSISTEMA USB MASS STORAGE (MSC) BULK-ONLY TRANSPORT & SCSI
// Arquitectura Anillo 0 en Español (Hito 48)
// ============================================================================

#define USB_MSC_MAX_DISPOSITIVOS 4

// Firmas oficiales de Bulk-Only Transport (USB MSC BOT)
#define USB_MSC_CBW_FIRMA 0x43425355U // "USBC" en little-endian
#define USB_MSC_CSW_FIRMA 0x53425355U // "USBS" en little-endian

// Banderas de dirección en CBW
#define USB_MSC_DIR_OUT   0x00
#define USB_MSC_DIR_IN    0x80

// Códigos de estado en CSW
#define USB_MSC_CSW_ESTADO_OK          0
#define USB_MSC_CSW_ESTADO_FALLO       1
#define USB_MSC_CSW_ESTADO_ERROR_FASE  2

// Opcodes SCSI transparentes estándar
#define SCSI_CMD_TEST_UNIT_READY  0x00
#define SCSI_CMD_REQUEST_SENSE    0x03
#define SCSI_CMD_INQUIRY          0x12
#define SCSI_CMD_READ_CAPACITY_10 0x25
#define SCSI_CMD_READ_10          0x28
#define SCSI_CMD_WRITE_10         0x2A

// Estructura oficial del Command Block Wrapper (CBW) - 31 bytes exactos
struct __attribute__((packed)) usb_msc_cbw {
    uint32_t firma;               // "USBC" (0x43425355)
    uint32_t etiqueta;            // Tag único asignado por el host
    uint32_t longitud_transfer;   // Número de bytes de datos que se esperan mover
    uint8_t  banderas;            // 0x80 = IN (dispositivo a host), 0x00 = OUT
    uint8_t  lun;                 // Logical Unit Number (bits 3:0)
    uint8_t  cdb_longitud;        // Tamaño del comando SCSI en bytes (6, 10, 12, 16)
    uint8_t  cdb[16];             // Comando SCSI (Command Descriptor Block)
};

// Estructura oficial del Command Status Wrapper (CSW) - 13 bytes exactos
struct __attribute__((packed)) usb_msc_csw {
    uint32_t firma;               // "USBS" (0x53425355)
    uint32_t etiqueta;            // Debe coincidir con la etiqueta del CBW
    uint32_t residuo;             // Diferencia entre datos esperados y transferidos
    uint8_t  estado;              // 0 = Success, 1 = Command Failed, 2 = Phase Error
};

// Estructura de información de dispositivo de almacenamiento USB
struct usb_msc_dispositivo {
    int      activo;
    uint8_t  slot_id;             // Slot asignado en xHCI
    uint8_t  puerto_idx;          // Puerto físico raíz
    uint8_t  ep_in_dci;           // DCI del Endpoint Bulk IN
    uint8_t  ep_out_dci;          // DCI del Endpoint Bulk OUT
    uint16_t ep_in_max_pkt;
    uint16_t ep_out_max_pkt;

    int      listo;               // 1 si TEST UNIT READY y READ CAPACITY pasaron con éxito
    uint32_t sectores_totales;    // Cantidad total de bloques LBA
    uint32_t tamano_sector;       // Tamaño de cada bloque en bytes (usualmente 512)
    uint64_t capacidad_bytes;     // Capacidad total en bytes

    char     fabricante[9];       // Vendor ID (ej. "SanDisk ", "Kingston")
    char     producto[17];        // Product ID (ej. "Ultra USB 3.0   ")
    char     revision[5];         // Firmware Revision (ej. "1.00")
};

// --- API PÚBLICA DEL SUBSISTEMA USB MSC ---

// Inicializa las estructuras internas del controlador Mass Storage
void usb_msc_iniciar(void);

// Registra una memoria USB configurada por xHCI e inicializa SCSI (INQUIRY, CAPACITY)
int  usb_msc_registrar_dispositivo(uint8_t slot_id, uint8_t puerto_idx,
                                   uint8_t ep_in_dci, uint8_t ep_out_dci,
                                   uint16_t ep_in_max_pkt, uint16_t ep_out_max_pkt);

// Desregistra un dispositivo cuando es desconectado
void usb_msc_desregistrar_dispositivo(uint8_t slot_id);

// Lee uno o más sectores físicos (LBA) desde la unidad USB especificada
int  usb_msc_leer_sectores(uint8_t id_unidad, uint32_t lba, uint16_t cantidad, void *buffer_destino);

// Escribe uno o más sectores físicos (LBA) hacia la unidad USB especificada (SCSI WRITE 10)
int  usb_msc_escribir_sectores(uint8_t id_unidad, uint32_t lba, uint16_t cantidad, const void *buffer_origen);

// Retorna la cantidad de unidades USB Mass Storage detectadas y operativas
int  usb_msc_obtener_cantidad(void);

// Obtiene la estructura de información de una unidad USB (0..USB_MSC_MAX_DISPOSITIVOS-1)
const struct usb_msc_dispositivo *usb_msc_obtener_dispositivo(uint8_t id_unidad);

#endif // CONTROLADORES_USB_MSC_H
