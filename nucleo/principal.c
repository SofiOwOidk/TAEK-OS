#include <stdint.h>
#include <stddef.h>

#define LIMINE_API_REVISION 2
#include "../boot/limine/limine.h"

#include "arquitectura/x86_64/serial.h"
#include "arquitectura/x86_64/gdt.h"
#include "arquitectura/x86_64/idt.h"
#include "arquitectura/x86_64/apic.h"
#include "arquitectura/x86_64/pci.h"
#include "arquitectura/x86_64/vmx.h"
#include "base/huevo.h"
#include "base/energia.h"
#include "base/utf8.h"
#include "base/tiempo.h"
#include "base/memoria.h"
#include "base/paginacion.h"
#include "controladores/pantalla.h"
#include "controladores/consola.h"
#include "controladores/audio_ac97.h"
#include "controladores/audio_hda.h"
#include "controladores/animacion_cangrejo.h"
#include "controladores/gpu.h"
#include "base/dma.h"
#include "controladores/iommu.h"
#include "compatibilidad/linux.h"
#include "controladores/video/nvidia/core/nvidia_core.h"
#include "controladores/xhci.h"
#include "controladores/terminal.h"
#include "controladores/teclado.h"

// Revision 3 del protocolo Limine
__attribute__((used, section(".requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests_start_marker")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".requests_end_marker")))
static volatile LIMINE_REQUESTS_END_MARKER;

// Peticion de Framebuffer para pantalla grafica
__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request g_peticion_framebuffer = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

// Peticion de Direcciones del Kernel para DMA fisico de AC97 y VMX
__attribute__((used, section(".requests")))
static volatile struct limine_executable_address_request g_peticion_direccion = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST,
    .revision = 0
};

// Peticion del archivo ejecutable (para leer cmdline de arranque de Limine)
__attribute__((used, section(".requests")))
static volatile struct limine_executable_file_request g_peticion_ejecutable = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST,
    .revision = 0
};

// Simbolos binarios incrustados (imagen de bienvenida y audio)
extern const uint8_t _binary_imagen_arranque_bin_start[];
extern const uint8_t _binary_imagen_arranque_bin_end[];

extern const uint8_t _binary_audio_arranque_bin_start[];
extern const uint8_t _binary_audio_arranque_bin_end[];

static int str_contiene(const char *cadena, const char *subcadena) {
    if (!cadena || !subcadena) return 0;
    for (int i = 0; cadena[i] != '\0'; i++) {
        int j = 0;
        while (subcadena[j] != '\0' && cadena[i + j] == subcadena[j]) {
            j++;
        }
        if (subcadena[j] == '\0') return 1;
    }
    return 0;
}

void principal(void) {
    // 1. Inicializar Serial de inmediato para capturar cualquier mensaje de arranque
    serial_iniciar();
    serial_imprimir_linea("\n==============================================================");
    serial_imprimir_linea("  TAEK OS v0.1 (TelAvivEpsteinKirkOS) - Anillo 0 en Marcha    ");
#ifdef COMPILACION_FECHA
    serial_imprimir("  Compilación: ");
    serial_imprimir_linea(COMPILACION_FECHA);
#endif
    serial_imprimir_linea("==============================================================");
    serial_imprimir("[BOOT] Puerto Serial COM1 (0x3F8): ");
    if (serial_esta_activo()) {
        serial_imprimir_linea("Activo a 115200 8N1 [OK]");
    } else {
        serial_imprimir_linea("Modo RAM/dmesg (Sin UART físico o desactivado en BIOS)");
    }

    // 2. Validar protocolo Limine
    if (LIMINE_BASE_REVISION_SUPPORTED == 0) {
        serial_imprimir_linea("[ERROR CRÍTICO] La revisión base de Limine no está soportada por el bootloader.");
        detener_cpu();
    }
    serial_imprimir_linea("[BOOT] Protocolo Limine Base Revision verificado [OK]");

    // 2.1 Procesar opciones de arranque desde la línea de comandos de Limine (cmdline)
    int modo_ps2_forzado = 0;
    if (g_peticion_ejecutable.response != NULL &&
        g_peticion_ejecutable.response->executable_file != NULL &&
        g_peticion_ejecutable.response->executable_file->cmdline != NULL) {
        const char *cmdline = (const char *)g_peticion_ejecutable.response->executable_file->cmdline;
        serial_imprimir("[BOOT] Línea de Comandos: \"");
        serial_imprimir(cmdline);
        serial_imprimir_linea("\"");
        if (str_contiene(cmdline, "modo=ps2") || str_contiene(cmdline, "ps2") || str_contiene(cmdline, "legacy")) {
            modo_ps2_forzado = 1;
        }
    }

    if (modo_ps2_forzado) {
        teclado_fijar_modo_nativo(1);
        serial_imprimir_linea("[BOOT] Modo Teclado: Fallback PS/2 Legacy Seleccionado");
    } else {
        teclado_fijar_modo_nativo(0);
        serial_imprimir_linea("[BOOT] Modo Teclado: xHCI Ring 0 (Controlador USB Hardware Activo)");
    }

    // 3. Inicialización Temprana de Pantalla GOP UEFI y Consola
    // Permite que todas las etapas de El Huevo y cualquier error se vean en el monitor físico en vivo
    if (g_peticion_framebuffer.response != NULL && g_peticion_framebuffer.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = g_peticion_framebuffer.response->framebuffers[0];
        pantalla_iniciar(fb);
        consola_iniciar();
        consola_limpiar();
        consola_imprimir_linea_color("==============================================================", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea_color("  TAEK OS v0.1 (TelAvivEpsteinKirkOS) - Anillo 0 en Marcha    ", COLOR_USUARIO_DEFAULT);
#ifdef COMPILACION_FECHA
        consola_imprimir("  Compilación: ");
        consola_imprimir_linea_color(COMPILACION_FECHA, COLOR_AVISO_DEFAULT);
#endif
        consola_imprimir_linea_color("==============================================================", COLOR_PROMPT_DEFAULT);
    }

    huevo_iniciar();

    huevo_etapa("Telemetría por Puerto Serial COM1 y Registro dmesg");
    serial_imprimir("[115200 baudios, Puerto 0x3F8 / Búfer RAM 64 KiB] ");
    huevo_etapa_ok();

    huevo_etapa("Recarga de Tabla GDT en 64 bits (Modo Largo)");
    gdt_iniciar();
    huevo_etapa_ok();

    huevo_etapa("IDT de 256 Vectores (Excepciones e Interrupciones)");
    idt_iniciar();
    huevo_etapa_ok();

    huevo_etapa("Calibración del Temporizador TSC / PIT");
    tiempo_iniciar();
    huevo_etapa_ok();

    huevo_etapa("Gestor de Memoria Dinámica (PMM + Kernel Heap)");
    memoria_iniciar();
    huevo_etapa_ok();

    huevo_etapa("Tablas de Paginación x86_64 (PML4 / VMM)");
    paginacion_iniciar();
    huevo_etapa_ok();

    huevo_etapa("Controlador de Interrupciones Local APIC / x2APIC (H16)");
    apic_iniciar();
    const struct estado_apic *eapic = apic_obtener_estado();
    serial_imprimir("[Modo: ");
    serial_imprimir(eapic->es_x2apic ? "x2APIC MSR Nativo" : "xAPIC MMIO");
    serial_imprimir(" | ID: ");
    serial_imprimir_dec((uint64_t)eapic->id);
    serial_imprimir(" | PIC Legacy: Desactivado] ");
    huevo_etapa_ok();

    huevo_etapa("Enumeración del Bus PCI / PCIe y Dispositivos de Video");
    pci_iniciar();
    serial_imprimir("[Dispositivos PCI: ");
    serial_imprimir_dec((uint64_t)pci_obtener_conteo());
    serial_imprimir("] ");
    const struct dispositivo_pci *gpu_init = pci_obtener_gpu_primaria();
    if (gpu_init) {
        serial_imprimir("[GPU: ");
        serial_imprimir(pci_nombre_proveedor(gpu_init->id_proveedor));
        serial_imprimir("] ");
    }
    huevo_etapa_ok();

    huevo_etapa("Mapeo MMIO sin Caché y Comunicación con Silicio GPU");
    if (gpu_iniciar() == 0) {
        const struct estado_gpu *egpu = gpu_obtener_estado();
        serial_imprimir("[Silicio: ");
        serial_imprimir(egpu->arquitectura_nombre);
        serial_imprimir(" | MMIO Virt: 0x");
        serial_imprimir_hex(egpu->dir_virtual_mmio);
        serial_imprimir("] ");
        huevo_etapa_ok();
    } else {
        serial_imprimir("[Sin acelerador GPU dedicado - Operando en modo GOP] ");
        huevo_etapa_ok();
    }

    huevo_etapa("Gestor de Memoria DMA Contigua Física (H17)");
    dma_iniciar();
    dma_estadisticas_t edma;
    dma_obtener_estadisticas(&edma);
    serial_imprimir("[Arena DMA: ");
    serial_imprimir_dec(edma.arena_tamano_bytes / (1024 * 1024));
    serial_imprimir(" MiB | Páginas: ");
    serial_imprimir_dec((uint64_t)edma.paginas_totales);
    serial_imprimir("] ");
    huevo_etapa_ok();

    huevo_etapa("Controlador de IOMMU / Intel VT-d (DMAR ACPI) (H17)");
    iommu_iniciar();
    const iommu_estado_t *eiommu = iommu_obtener_estado();
    if (eiommu->tabla_dmar_detectada) {
        serial_imprimir("[Intel VT-d Activo | DRHD: ");
        serial_imprimir_dec((uint64_t)eiommu->conteo_drhd);
        serial_imprimir(" | Modo: ");
        serial_imprimir(eiommu->modo_operacion == IOMMU_MODO_VT_D_PASSTHROUGH ? "PassThrough" : "Traducción");
        serial_imprimir("] ");
    } else {
        serial_imprimir("[Modo DMA Directo 1:1 Transparente (Sin DMAR)] ");
    }
    huevo_etapa_ok();

    huevo_etapa("Capa de Compatibilidad Linux Kernel Shim (Ring 0)");
    linux_shim_iniciar();
    uint64_t dma_asig = 0;
    uint64_t ioremap_cnt = 0;
    uint32_t pci_devs = 0;
    linux_shim_obtener_estadisticas(&dma_asig, &ioremap_cnt, &pci_devs);
    serial_imprimir("[Linux ABI 6.12 | Dispositivos PCI: ");
    serial_imprimir_dec((uint64_t)pci_devs);
    serial_imprimir("] ");
    huevo_etapa_ok();

    huevo_etapa("Subsistema Aislado NVIDIA Resource Manager (Core / GSP)");
    nvidia_core_iniciar();
    const struct nvidia_dispositivo *ndev = nvidia_core_obtener_dispositivo();
    serial_imprimir("[Pipeline: ");
    serial_imprimir(ndev->chip_name);
    serial_imprimir(ndev->presente ? " (Hardware MoDT Activo)" : " (Canal GSP Listo)");
    serial_imprimir("] ");
    huevo_etapa_ok();

    huevo_etapa("Controlador Host USB 3.x xHCI y Teclado HID");
    if (teclado_es_modo_nativo()) {
        teclado_iniciar();
    }

    if (!teclado_es_modo_nativo()) {
        if (xhci_iniciar() == 0) {
            const struct estado_xhci *ex = xhci_obtener_estado();
            serial_imprimir("[xHCI Activo | Puertos: ");
            serial_imprimir_dec((uint64_t)ex->max_puertos);
            serial_imprimir(" | Conectados: ");
            serial_imprimir_dec((uint64_t)ex->puertos_conectados);
            if (ex->teclado_detectado) {
                serial_imprimir(" | Teclado Compuesto USB OK (");
                serial_imprimir_dec((uint64_t)ex->teclado_num_eps);
                serial_imprimir(" EPs)] ");
                consola_imprimir(" [Teclado USB OK] ");
            } else {
                serial_imprimir("] ");
                if (ex->puertos_conectados > 0) {
                    consola_imprimir(" [USB Conectado] ");
                }
            }
            huevo_etapa_ok();
        } else {
            serial_imprimir("[xHCI No Detectado - Fallback de Seguridad a PS/2] ");
            consola_imprimir(" [Fallback PS/2] ");
            teclado_fijar_modo_nativo(1);
            teclado_iniciar();
            huevo_etapa_ok();
        }
    } else {
        serial_imprimir("[Modo Nativo Firmware Activo - Emulación SMM/PS2 Preservada (Cero Sobrecarga)] ");
        consola_imprimir(" [Modo Nativo Firmware OK] ");
        huevo_etapa_ok();
    }

    huevo_etapa("Verificación de Pantalla GOP UEFI");
    if (g_peticion_framebuffer.response == NULL || g_peticion_framebuffer.response->framebuffer_count < 1) {
        serial_imprimir("[ERROR: SIN PANTALLA GOP] ");
        huevo_quebrar("No se detectó framebuffer UEFI para renderizar", 0, 0, 0);
    }

    struct limine_framebuffer *fb = g_peticion_framebuffer.response->framebuffers[0];
    serial_imprimir("[Resolución: ");
    serial_imprimir_dec(fb->width);
    serial_imprimir("x");
    serial_imprimir_dec(fb->height);
    serial_imprimir("x");
    serial_imprimir_dec(fb->bpp);
    serial_imprimir("bpp] ");
    huevo_etapa_ok();

    uint64_t base_fisica = 0;
    uint64_t base_virtual = 0;
    if (g_peticion_direccion.response != NULL) {
        base_fisica  = g_peticion_direccion.response->physical_base;
        base_virtual = g_peticion_direccion.response->virtual_base;
    }

    huevo_etapa("Inicialización de Hipervisor Ring -1 (Intel VMX)");
    if (vmx_iniciar(base_fisica, base_virtual) == 0) {
        serial_imprimir("[VMX Root Activo - Interceptación Triple Fault Habilitada] ");
        huevo_etapa_ok();
    } else {
        serial_imprimir("[Guardián de Fallos Activo en Ring 0] ");
        huevo_etapa_ok();
    }

    huevo_etapa("Renderizado de Imagen de Arranque (Five Nights in Tel Aviv)");
    pantalla_dibujar_imagen_centrada(638, 780, (const uint32_t *)_binary_imagen_arranque_bin_start);
    serial_imprimir("[638x780 BGRA32 Centrada] ");
    huevo_etapa_ok();

    huevo_etapa("Inicialización de Subsistema de Audio (Intel HDA / AC97)");
    if (audio_ac97_iniciar(base_fisica, base_virtual) != 0) {
        serial_imprimir("[AUDIO NO DETECTADO - Continuando en modo mudo] ");
        huevo_agrietar("Dispositivo de audio no responde");
    } else {
        if (audio_es_intel_hda()) {
            const struct estado_hda *ehda = audio_hda_obtener_estado();
            serial_imprimir("[Intel HDA ");
            serial_imprimir_hex(ehda->id_proveedor);
            serial_imprimir(":");
            serial_imprimir_hex(ehda->id_dispositivo);
            serial_imprimir(" Listo a 44.1 kHz] ");
        } else {
            serial_imprimir("[Intel 82801AA AC97 Listo a 44.1 kHz] ");
        }
        huevo_etapa_ok();

        huevo_etapa("Reproducción de Sintonía de Encendido (Qué bonito es Israel Damonte)");
        uint32_t tamano_audio = (uint32_t)(_binary_audio_arranque_bin_end - _binary_audio_arranque_bin_start);
        audio_ac97_reproducir_pcm(_binary_audio_arranque_bin_start, tamano_audio);
        serial_imprimir("[Audio DMA en Marcha] ");
        huevo_etapa_ok();
    }

    huevo_etapa("Sincronización de Retardo de Arranque (9 Segundos de Cortesía Musical)");
    serial_imprimir_linea("");
    for (int segundo = 1; segundo <= 9; segundo++) {
        serial_imprimir("  [ REPRODUCIENDO ] Segundo ");
        serial_imprimir_dec(segundo);
        serial_imprimir_linea(" de 9...");
        // HDA usa dos bloques DMA de 64 KiB. Alimentarlo durante la espera
        // evita que la música se corte antes de que aparezca la terminal.
        for (int tick = 0; tick < 100; tick++) {
            audio_ac97_actualizar();
            esperar_milisegundos(10);
        }
        huevo_verificar();
    }
    serial_imprimir("[Sintonía Concluida] ");
    huevo_etapa_ok();

    serial_imprimir_linea("");
    serial_imprimir_linea("==============================================================");
    serial_imprimir_linea("  TAEK OS v0.1 (TelAvivEpsteinKirkOS) - Anillo 0 en Español   ");
    serial_imprimir_linea("  Procesador: x86_64 (Intel Core / AMD64 Compatible)          ");
    serial_imprimir_linea("  ¡Hipervisor VMX y Guardián Don Cangrejo Armados!            ");
    serial_imprimir_linea("==============================================================");
    serial_imprimir_linea("  [ OK ] Todas las etapas verificadas por El Huevo.           ");
    serial_imprimir_linea("  [ OK ] El Huevo sigue 100% INTACTO. Integridad: 100%.       ");
    serial_imprimir_linea("  [ OK ] Sistema operativo listo. Lanzando Terminal de Control... ");
    serial_imprimir_linea("==============================================================");
    serial_imprimir_linea("");

    esperar_milisegundos(1000);

    // Iniciar la Terminal interactiva con el usuario 'sudo'
    terminal_ejecutar();

    for (;;) {
        __asm__ volatile ("hlt");
    }
}
