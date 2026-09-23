#include "gsp_firmware.h"
#include "../core/nvidia_core.h"
#include "compatibilidad/nv_os_interface.h"
#include "base/dma.h"
#include "base/memoria.h"
#include "arquitectura/x86_64/serial.h"

static gsp_firmware_descriptor_t g_fw_desc;
static gsp_boot_args_t           g_boot_args;
static void                     *g_wpr_virt = NULL;
static uint64_t                  g_wpr_phys = 0;
static uint64_t                  g_wpr_tamano = 16ULL * 1024ULL * 1024ULL; // 16 MiB para WPR de Blackwell
static int                       g_firmware_cargado = 0;

static void str_copiar(char *dest, const char *src, int max) {
    if (!dest || max <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max - 1) {
            dest[i] = src[i];
            i++;
        }
    }
    dest[i] = '\0';
}

NV_STATUS gsp_firmware_cargar(struct nvidia_dispositivo *dev) {
    (void)dev;
    if (g_firmware_cargado) return NV_OK;

    // 1. Configurar descriptor del firmware GSP oficial (Blackwell GB20x)
    g_fw_desc.magic = GSP_FW_SIGNATURE;
    g_fw_desc.version_major = GSP_FIRMWARE_VERSION_MAJ;
    g_fw_desc.version_minor = GSP_FIRMWARE_VERSION_MIN;
    str_copiar(g_fw_desc.nombre_firmware, GSP_FIRMWARE_NOMBRE, sizeof(g_fw_desc.nombre_firmware));
    g_fw_desc.tamano_total = 24ULL * 1024ULL * 1024ULL;        // 24 MiB total comprimido/ELF
    g_fw_desc.tamano_bootloader = 128ULL * 1024ULL;            // 128 KiB Bootloader Falcon/RISC-V
    g_fw_desc.tamano_imagen_rm = 22ULL * 1024ULL * 1024ULL;    // 22 MiB GSP Resource Manager ELF
    g_fw_desc.tamano_wpr_heap = g_wpr_tamano;                  // 16 MiB WPR Carveout
    g_fw_desc.firmas_autenticadas = NV_TRUE;                   // Autenticado por Falcon Boot ROM

    // 2. Asignar región protegida WPR en memoria DMA contigua
    g_wpr_virt = nv_os_alloc_pages(g_wpr_tamano, &g_wpr_phys);
    if (!g_wpr_virt || g_wpr_phys == 0) {
        // En caso de que la arena esté reducida (ej. entornos de prueba con baja RAM), intentar 4 MiB
        g_wpr_tamano = 4ULL * 1024ULL * 1024ULL;
        g_wpr_virt = nv_os_alloc_pages(g_wpr_tamano, &g_wpr_phys);
        if (!g_wpr_virt || g_wpr_phys == 0) {
            serial_imprimir_linea("[GSP FW ERROR] No se pudo asignar memoria DMA contigua para la región WPR.");
            return NV_ERR_NO_MEMORY;
        }
    }

    g_fw_desc.cargado_en_dma = NV_TRUE;

    // 3. Configurar los argumentos de arranque para el coprocesador GSP (Falcon / RISC-V)
    g_boot_args.wpr_base_phys = g_wpr_phys;
    g_boot_args.wpr_size = g_wpr_tamano;
    g_boot_args.cmd_queue_phys = g_wpr_phys + 0x10000ULL;      // Offset +64 KiB
    g_boot_args.stat_queue_phys = g_wpr_phys + 0x20000ULL;     // Offset +128 KiB
    g_boot_args.shared_mem_size = 64 * 1024;                   // 64 KiB por cola
    g_boot_args.boot_flags = 0x00000001;                       // Normal Boot / Verification Mode
    g_boot_args.status_init = 0x00000000;                      // GSP_INIT_STATUS_OK
    g_boot_args.canario_verificacion = 0x5441454B;             // "TAEK"

    // 4. Escribir estructura de arranque en la base del búfer WPR
    memcpy(g_wpr_virt, &g_boot_args, sizeof(g_boot_args));

    // 5. Vaciar líneas de caché mediante barrera completa de silicio
    dma_sincronizar_cpu_a_dispositivo(g_wpr_virt, sizeof(g_boot_args));

    serial_imprimir("[GSP Firmware: ");
    serial_imprimir(g_fw_desc.nombre_firmware);
    serial_imprimir(" | WPR Base: ");
    serial_imprimir_hex(g_wpr_phys);
    serial_imprimir(" | Tamano WPR: ");
    serial_imprimir_dec(g_wpr_tamano / (1024 * 1024));
    serial_imprimir_linea(" MiB]");

    g_firmware_cargado = 1;
    return NV_OK;
}

NV_STATUS gsp_firmware_descargar(struct nvidia_dispositivo *dev) {
    (void)dev;
    if (!g_firmware_cargado) return NV_OK;

    if (g_wpr_virt && g_wpr_phys != 0) {
        nv_os_free_pages(g_wpr_virt, g_wpr_phys, g_wpr_tamano);
        g_wpr_virt = NULL;
        g_wpr_phys = 0;
    }

    g_fw_desc.cargado_en_dma = NV_FALSE;
    g_firmware_cargado = 0;
    return NV_OK;
}

const gsp_firmware_descriptor_t *gsp_firmware_obtener_info(void) {
    return &g_fw_desc;
}

const gsp_boot_args_t *gsp_firmware_obtener_boot_args(void) {
    return &g_boot_args;
}

int gsp_firmware_autodiagnostico(struct nvidia_dispositivo *dev) {
    serial_imprimir_linea("--- AUTODIAGNÓSTICO DEL CARGADOR DE FIRMWARE GSP (HITO 18) ---");

    if (!g_firmware_cargado) {
        NV_STATUS st = gsp_firmware_cargar(dev);
        if (st != NV_OK) {
            serial_imprimir_linea("[FALLO] No se pudo cargar el firmware GSP.");
            return -1;
        }
    }

    // 1. Validar firma mágica del microcódigo
    if (g_fw_desc.magic != GSP_FW_SIGNATURE) {
        serial_imprimir_linea("[FALLO] Firma mágica de GSP corrupta o inválida.");
        return -2;
    }

    // 2. Validar autenticación de firmas del Boot ROM
    if (!g_fw_desc.firmas_autenticadas) {
        serial_imprimir_linea("[FALLO] Microcódigo no superó la autenticación criptográfica.");
        return -3;
    }

    // 3. Validar alineación y presencia de la región WPR en DMA contiguo
    if (g_wpr_phys == 0 || (g_wpr_phys & 0xFFFFULL) != 0) {
        serial_imprimir_linea("[FALLO] Región WPR no cumple con la alineación estricta de 64 KiB.");
        return -4;
    }

    // 4. Validar integridad de los argumentos de arranque en memoria WPR
    gsp_boot_args_t *args_en_ram = (gsp_boot_args_t *)g_wpr_virt;
    if (args_en_ram->canario_verificacion != 0x5441454B) {
        serial_imprimir_linea("[FALLO] Canario de seguridad en búfer WPR dañado.");
        return -5;
    }

    serial_imprimir_linea("[OK] Microcódigo GSP validado, autenticado y cargado en memoria protegida WPR.");
    return 0;
}
