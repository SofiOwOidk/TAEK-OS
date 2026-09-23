#include "tiempo.h"
#include "../arquitectura/x86_64/puertos.h"

static uint64_t g_ciclos_por_ms = 0;

static inline uint64_t rdtsc_interno(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t rdtsc(void) {
    return rdtsc_interno();
}

void tiempo_iniciar(void) {
    // Calibrar el contador TSC con el temporizador PIT Canal 2 durante 10 ms
    // Frecuencia PIT = 1193182 Hz -> 10 ms = 11932 ticks
    uint16_t ticks_pit = 11932;

    // Configurar canal 2 del PIT: Modo 0, LSB/MSB
    escribir_puerto_b(0x43, 0xB0);

    // Apagar altavoz pero activar compuerta del canal 2 (bit 0 = 1, bit 1 = 0)
    uint8_t p61 = leer_puerto_b(0x61);
    escribir_puerto_b(0x61, (p61 & ~0x02) | 0x01);

    // Cargar contador
    escribir_puerto_b(0x42, (uint8_t)(ticks_pit & 0xFF));
    escribir_puerto_b(0x42, (uint8_t)((ticks_pit >> 8) & 0xFF));

    uint64_t tsc_inicio = rdtsc_interno();

    // Esperar a que la salida del canal 2 pase a nivel alto (bit 5 del puerto 0x61)
    // Con salvaguarda por si el emulador no emula el canal 2
    uint32_t timeout = 5000000;
    while ((leer_puerto_b(0x61) & 0x20) == 0 && timeout > 0) {
        timeout--;
    }

    uint64_t tsc_fin = rdtsc_interno();

    if (timeout > 0 && tsc_fin > tsc_inicio) {
        g_ciclos_por_ms = (tsc_fin - tsc_inicio) / 10;
    } else {
        // Fallback tipico para CPUs modernas (~2.5 GHz -> 2,500,000 ciclos/ms)
        g_ciclos_por_ms = 2500000;
    }
}

void esperar_milisegundos(uint32_t ms) {
    if (g_ciclos_por_ms == 0) {
        tiempo_iniciar();
    }
    uint64_t fin = rdtsc_interno() + ((uint64_t)ms * g_ciclos_por_ms);
    while (rdtsc_interno() < fin) {
        __asm__ volatile ("pause");
    }
}

uint64_t tiempo_obtener_milisegundos(void) {
    if (g_ciclos_por_ms == 0) {
        tiempo_iniciar();
    }
    return rdtsc_interno() / g_ciclos_por_ms;
}

