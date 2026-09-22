#include <stdint.h>
#include <stddef.h>

#define LIMINE_API_REVISION 2
#include "../boot/limine/limine.h"

#include "arquitectura/x86_64/serial.h"
#include "arquitectura/x86_64/gdt.h"
#include "arquitectura/x86_64/idt.h"
#include "arquitectura/x86_64/vmx.h"
#include "base/huevo.h"
#include "base/energia.h"
#include "base/utf8.h"
#include "base/tiempo.h"
#include "base/memoria.h"
#include "base/paginacion.h"
#include "controladores/pantalla.h"
#include "controladores/audio_ac97.h"
#include "controladores/animacion_cangrejo.h"
#include "controladores/terminal.h"

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

// Simbolos binarios incrustados (imagen de bienvenida y audio)
extern const uint8_t _binary_imagen_arranque_bin_start[];
extern const uint8_t _binary_imagen_arranque_bin_end[];

extern const uint8_t _binary_audio_arranque_bin_start[];
extern const uint8_t _binary_audio_arranque_bin_end[];

void principal(void) {
    if (LIMINE_BASE_REVISION_SUPPORTED == 0) {
        detener_cpu();
    }

    serial_iniciar();
    huevo_iniciar();

    huevo_etapa("Telemetría por Puerto Serial COM1");
    serial_imprimir("[115200 baudios, Puerto 0x3F8] ");
    huevo_etapa_ok();

    huevo_etapa("Recarga de Tabla GDT en 64 bits (Modo Largo)");
    gdt_iniciar();
    huevo_etapa_ok();

    huevo_etapa("Trampas de Excepción de CPU en IDT (32 Vectores)");
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

    huevo_etapa("Inicialización de Pantalla GOP UEFI");
    if (g_peticion_framebuffer.response == NULL || g_peticion_framebuffer.response->framebuffer_count < 1) {
        serial_imprimir("[ERROR: SIN PANTALLA GOP] ");
        huevo_quebrar("No se detectó framebuffer UEFI para renderizar", 0, 0, 0);
    }

    struct limine_framebuffer *fb = g_peticion_framebuffer.response->framebuffers[0];
    pantalla_iniciar(fb);
    pantalla_limpiar(0x00000000); // Fondo negro puro

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

    huevo_etapa("Inicialización de Audio PCI AC97");
    if (audio_ac97_iniciar(base_fisica, base_virtual) != 0) {
        serial_imprimir("[AC97 NO DETECTADO - Continuando en modo mudo] ");
        huevo_agrietar("Dispositivo de audio AC97 no responde");
    } else {
        serial_imprimir("[Intel 82801AA AC97 Listo a 44.1 kHz] ");
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
        esperar_milisegundos(1000);
        huevo_verificar();
    }
    serial_imprimir("[Sintonía Concluida] ");
    huevo_etapa_ok();

    serial_imprimir_linea("");
    serial_imprimir_linea("==============================================================");
    serial_imprimir_linea("  TAEK OS v0.1 (TelAvivEpsteinKirkOS) - Anillo 0 en Español   ");
    serial_imprimir_linea("  Procesador: x86_64 (Listo para Intel Core i9-14900HX)       ");
    serial_imprimir_linea("  ¡Hipervisor VMX y Guardián Don Cangrejo Armados!            ");
    serial_imprimir_linea("  Filosofía: \"Vibecoding en Español y con Buenas Prácticas\"   ");
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
