#include "audio_ac97.h"
#include "audio_hda.h"
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
static int g_usar_hda = 0;

// Variables de estado del flujo de audio
static const uint8_t *g_audio_datos          = 0;
static uint32_t       g_audio_tamano         = 0;
static uint32_t       g_audio_cursor         = 0;
static int            g_audio_en_bucle       = 0;
static int            g_audio_activo         = 0;
static uint8_t        g_bdl_indice_escritura = 0;
static uint8_t        g_bdl_ultimo_civ       = 0;
static int            g_entradas_en_cola     = 0;

static inline uint32_t virt_a_fisica(const void *ptr) {
    uint64_t v = (uint64_t)ptr;
    return (uint32_t)(v - g_base_virtual + g_base_fisica);
}

int audio_ac97_iniciar(uint64_t base_fisica_kernel, uint64_t base_virtual_kernel) {
    g_base_fisica  = base_fisica_kernel;
    g_base_virtual = base_virtual_kernel;

    // 1. Probar primero si hay un controlador Intel HDA moderno (Hardware real / Laptop 8086:8d71 / MoDT)
    if (audio_hda_iniciar() == 0) {
        g_usar_hda = 1;
        g_iniciado = 1;
        return 0;
    }

    // 2. Si no hay HDA, buscar tarjeta AC97 tradicional (Emulador QEMU)
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

static int audio_ac97_reproducir_flujo(const void *datos_pcm, uint32_t tamano_bytes, int bucle) {
    if (!g_iniciado || !datos_pcm || tamano_bytes == 0) {
        return 1;
    }

    // Detener canal PCM Out si estaba corriendo
    escribir_puerto_b(g_nabmbar + 0x1B, 0x02); // Reset canal
    esperar_io();

    g_audio_datos          = (const uint8_t *)datos_pcm;
    g_audio_tamano         = tamano_bytes;
    g_audio_cursor         = 0;
    g_audio_en_bucle       = bucle;
    g_audio_activo         = 1;
    g_bdl_indice_escritura = 0;
    g_bdl_ultimo_civ       = 0;
    g_entradas_en_cola     = 0;

    // Llenar descriptores iniciales del BDL (hasta 32 entradas de 64 KB = ~11.8s)
    while (g_entradas_en_cola < 32 && g_audio_activo) {
        if (g_audio_cursor < g_audio_tamano) {
            uint32_t restante = g_audio_tamano - g_audio_cursor;
            uint32_t paquete  = (restante > 65536) ? 65536 : restante;
            paquete &= ~3; // Múltiplo de frame estéreo de 16 bits (4 bytes)
            if (paquete == 0) paquete = restante;

            g_bdl[g_bdl_indice_escritura].dir_fisica   = virt_a_fisica(g_audio_datos + g_audio_cursor);
            g_bdl[g_bdl_indice_escritura].num_muestras = (uint16_t)(paquete / 2);
            g_bdl[g_bdl_indice_escritura].banderas     = 0;

            g_audio_cursor += paquete;
            if (g_audio_cursor >= g_audio_tamano) {
                if (g_audio_en_bucle) {
                    g_audio_cursor = 0; // Se reiniciará desde el principio tras recorrer toda la canción
                } else {
                    g_bdl[g_bdl_indice_escritura].banderas = 0x8000; // IOC en la última muestra
                    g_audio_activo = 0;
                }
            }

            g_bdl_indice_escritura = (g_bdl_indice_escritura + 1) % 32;
            g_entradas_en_cola++;
        } else if (g_audio_en_bucle) {
            g_audio_cursor = 0;
        } else {
            break;
        }
    }

    if (g_entradas_en_cola == 0) return 1;

    // Configurar dirección física base de la BDL (NABMBAR + 0x10)
    uint32_t dir_bdl_fisica = virt_a_fisica(g_bdl);
    escribir_puerto_l(g_nabmbar + 0x10, dir_bdl_fisica);

    // Configurar Último Índice Válido (LVI) (NABMBAR + 0x15)
    uint8_t lvi = (uint8_t)((g_bdl_indice_escritura + 31) % 32);
    escribir_puerto_b(g_nabmbar + 0x15, lvi);

    // Limpiar banderas de estado (NABMBAR + 0x16)
    escribir_puerto_w(g_nabmbar + 0x16, 0x001C);

    // Iniciar reproducción DMA (Bit 0 de Control Register 0x1B = RUN)
    escribir_puerto_b(g_nabmbar + 0x1B, 0x01);

    return 0;
}

void audio_ac97_actualizar(void) {
    if (!g_iniciado) return;
    if (g_usar_hda) {
        audio_hda_actualizar();
        return;
    }
    if (!g_audio_activo && g_entradas_en_cola == 0) {
        return;
    }

    uint8_t civ = (uint8_t)(leer_puerto_b(g_nabmbar + 0x14) & 0x1F);

    if (civ != g_bdl_ultimo_civ) {
        uint8_t avanzadas = (uint8_t)((civ - g_bdl_ultimo_civ + 32) % 32);
        g_entradas_en_cola -= avanzadas;
        if (g_entradas_en_cola < 0) g_entradas_en_cola = 0;
        g_bdl_ultimo_civ = civ;
    }

    // Rellenar descriptores liberados por el hardware
    while (g_entradas_en_cola < 32 && (g_audio_activo || (g_audio_en_bucle && g_audio_tamano > 0))) {
        if (g_audio_cursor < g_audio_tamano) {
            uint32_t restante = g_audio_tamano - g_audio_cursor;
            uint32_t paquete  = (restante > 65536) ? 65536 : restante;
            paquete &= ~3;
            if (paquete == 0) paquete = restante;

            g_bdl[g_bdl_indice_escritura].dir_fisica   = virt_a_fisica(g_audio_datos + g_audio_cursor);
            g_bdl[g_bdl_indice_escritura].num_muestras = (uint16_t)(paquete / 2);
            g_bdl[g_bdl_indice_escritura].banderas     = 0;

            g_audio_cursor += paquete;
            if (g_audio_cursor >= g_audio_tamano) {
                if (g_audio_en_bucle) {
                    // ¡Canción completa (2m 42s) terminada! Ahora vuelve al inicio para entrar en bucle.
                    g_audio_cursor = 0;
                } else {
                    g_bdl[g_bdl_indice_escritura].banderas = 0x8000;
                    g_audio_activo = 0;
                }
            }

            // Actualizar LVI de hardware para que el DMA continúe sin detenerse
            escribir_puerto_b(g_nabmbar + 0x15, g_bdl_indice_escritura);
            g_bdl_indice_escritura = (g_bdl_indice_escritura + 1) % 32;
            g_entradas_en_cola++;
        } else if (g_audio_en_bucle) {
            g_audio_cursor = 0;
        } else {
            break;
        }
    }

    // Si el DMA se detuvo por falta momentánea de búfer pero ya hay datos en cola, reanudar
    uint16_t sr = leer_puerto_w(g_nabmbar + 0x16);
    if ((sr & 0x0001) && g_entradas_en_cola > 0) {
        escribir_puerto_w(g_nabmbar + 0x16, 0x001C);
        escribir_puerto_b(g_nabmbar + 0x1B, 0x01);
    }
}

int audio_es_intel_hda(void) {
    return g_usar_hda;
}

int audio_esta_iniciado(void) {
    return g_iniciado;
}

int audio_ac97_reproducir_pcm(const void *datos_pcm, uint32_t tamano_bytes) {
    if (g_usar_hda) return audio_hda_reproducir_pcm(datos_pcm, tamano_bytes);
    return audio_ac97_reproducir_flujo(datos_pcm, tamano_bytes, 0);
}

int audio_ac97_reproducir_pcm_bucle(const void *datos_pcm, uint32_t tamano_bytes) {
    if (g_usar_hda) return audio_hda_reproducir_pcm_bucle(datos_pcm, tamano_bytes);
    return audio_ac97_reproducir_flujo(datos_pcm, tamano_bytes, 1);
}

int audio_ac97_esta_reproduciendo(void) {
    if (!g_iniciado) return 0;
    if (g_usar_hda) return audio_hda_esta_reproduciendo();
    audio_ac97_actualizar();
    if (g_audio_activo || g_entradas_en_cola > 0) {
        return 1;
    }
    uint16_t estado = leer_puerto_w(g_nabmbar + 0x16);
    return ((estado & 0x0001) == 0);
}

void audio_ac97_detener(void) {
    if (!g_iniciado) return;
    if (g_usar_hda) {
        audio_hda_detener();
        return;
    }
    escribir_puerto_b(g_nabmbar + 0x1B, 0x00);
    g_audio_activo         = 0;
    g_audio_en_bucle       = 0;
    g_entradas_en_cola     = 0;
    g_audio_cursor         = 0;
}
