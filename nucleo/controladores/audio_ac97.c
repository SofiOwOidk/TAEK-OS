#include "audio_ac97.h"
#include "../arquitectura/x86_64/pci.h"
#include "../arquitectura/x86_64/puertos.h"
#include "../arquitectura/x86_64/serial.h"

#define AC97_VENDOR_ID  0x8086
#define AC97_DEVICE_ID  0x2415

struct entrada_bdl {
    uint32_t dir_fisica;
    uint16_t num_muestras;
    uint16_t banderas;
} __attribute__((packed));

static struct entrada_bdl g_bdl[32] __attribute__((aligned(16)));
static struct dispositivo_pci g_pci_ac97;
static uint16_t g_nambar  = 0;
static uint16_t g_nabmbar = 0;
static uint64_t g_base_fisica  = 0;
static uint64_t g_base_virtual = 0;
static int g_iniciado = 0;

static inline uint32_t virt_a_fisica(const void *ptr) {
    uint64_t v = (uint64_t)ptr;
    return (uint32_t)(v - g_base_virtual + g_base_fisica);
}

int audio_ac97_iniciar(uint64_t base_fisica_kernel, uint64_t base_virtual_kernel) {
    g_base_fisica  = base_fisica_kernel;
    g_base_virtual = base_virtual_kernel;

    if (pci_buscar_dispositivo(AC97_VENDOR_ID, AC97_DEVICE_ID, &g_pci_ac97) != 0) {
        return 1; // Tarjeta AC97 no encontrada
    }

    pci_activar_bus_master(&g_pci_ac97);

    g_nambar  = (uint16_t)(g_pci_ac97.bar0 & ~0x3);
    g_nabmbar = (uint16_t)(g_pci_ac97.bar1 & ~0x3);

    // Reiniciar mezclador
    escribir_puerto_w(g_nambar + 0x00, 0x0001);
    esperar_io();

    // Fijar volumen maestro a 0 dB (sin mute)
    escribir_puerto_w(g_nambar + 0x02, 0x0000);

    // Fijar volumen PCM Out a 0 dB (sin mute)
    escribir_puerto_w(g_nambar + 0x18, 0x0000);

    // Fijar frecuencia de muestreo a 44100 Hz (Front DAC Rate)
    escribir_puerto_w(g_nambar + 0x2C, 44100);

    g_iniciado = 1;
    return 0;
}

int audio_ac97_reproducir_pcm(const void *datos_pcm, uint32_t tamano_bytes) {
    if (!g_iniciado || !datos_pcm || tamano_bytes == 0) {
        return 1;
    }

    // Detener canal PCM Out si estaba corriendo
    escribir_puerto_b(g_nabmbar + 0x1B, 0x02); // Reset canal
    esperar_io();

    // Cada muestra stereo de 16 bits = 4 bytes (2 canales * 2 bytes)
    // El campo num_muestras en BDL cuenta muestras de 16 bits individuales
    uint32_t total_muestras_16 = tamano_bytes / 2;
    uint32_t muestras_por_bld  = 32768; // 64 KB por descriptor
    uint32_t muestras_restantes = total_muestras_16;
    const uint8_t *origen = (const uint8_t *)datos_pcm;
    int entradas = 0;

    while (muestras_restantes > 0 && entradas < 32) {
        uint32_t paquete = (muestras_restantes > muestras_por_bld) ? muestras_por_bld : muestras_restantes;

        g_bdl[entradas].dir_fisica   = virt_a_fisica(origen);
        g_bdl[entradas].num_muestras = (uint16_t)paquete;
        g_bdl[entradas].banderas     = 0;

        origen += paquete * 2;
        muestras_restantes -= paquete;
        entradas++;
    }

    if (entradas > 0) {
        // En la ultima entrada marcamos IOC (Interrupt on Completion)
        g_bdl[entradas - 1].banderas = 0x8000;
    }

    // Configurar registro base de la BDL (NABMBAR + 0x10)
    uint32_t dir_bdl_fisica = virt_a_fisica(g_bdl);
    escribir_puerto_l(g_nabmbar + 0x10, dir_bdl_fisica);

    // Configurar Ultimo Indice Valido (LVI) (NABMBAR + 0x15)
    escribir_puerto_b(g_nabmbar + 0x15, (uint8_t)(entradas - 1));

    // Limpiar banderas de estado (NABMBAR + 0x16)
    escribir_puerto_w(g_nabmbar + 0x16, 0x001C);

    // Iniciar reproduccion DMA (Bit 0 de Control Register 0x1B = RUN)
    escribir_puerto_b(g_nabmbar + 0x1B, 0x01);

    return 0;
}

int audio_ac97_esta_reproduciendo(void) {
    if (!g_iniciado) return 0;
    uint16_t estado = leer_puerto_w(g_nabmbar + 0x16);
    // Bit 0 = DMA Controller Halted (DCH). Si es 0, sigue en marcha
    return ((estado & 0x0001) == 0);
}

void audio_ac97_detener(void) {
    if (!g_iniciado) return;
    escribir_puerto_b(g_nabmbar + 0x1B, 0x00);
}
