#include "dma.h"
#include "memoria.h"
#include "huevo.h"
#include "../arquitectura/x86_64/serial.h"

#define MAX_PALABRAS_BITMAP 128 // 128 * 64 bits = 8,192 páginas (32 MiB @ 4 KiB)

static uint64_t g_dma_base_fisica = 0;
static uint64_t g_dma_tamano_bytes = 0;
static uint32_t g_dma_total_paginas = 0;
static uint32_t g_dma_paginas_ocupadas = 0;
static uint32_t g_dma_asignaciones_activas = 0;
static uint32_t g_dma_max_tamano_kb = 0;
static int      g_dma_iniciado = 0;

static uint64_t g_dma_bitmap[MAX_PALABRAS_BITMAP];

static inline int bitmap_probar_bit(uint32_t bit) {
    if (bit >= g_dma_total_paginas) return 1;
    uint32_t idx = bit / 64;
    uint32_t offset = bit % 64;
    return (g_dma_bitmap[idx] & (1ULL << offset)) != 0;
}

static inline void bitmap_marcar_bit(uint32_t bit) {
    if (bit >= g_dma_total_paginas) return;
    uint32_t idx = bit / 64;
    uint32_t offset = bit % 64;
    g_dma_bitmap[idx] |= (1ULL << offset);
}

static inline void bitmap_limpiar_bit(uint32_t bit) {
    if (bit >= g_dma_total_paginas) return;
    uint32_t idx = bit / 64;
    uint32_t offset = bit % 64;
    g_dma_bitmap[idx] &= ~(1ULL << offset);
}

int dma_iniciar(void) {
    if (g_dma_iniciado) return 0;

    g_dma_base_fisica = memoria_obtener_dma_arena(&g_dma_tamano_bytes);

    if (g_dma_base_fisica == 0 || g_dma_tamano_bytes == 0) {
        serial_imprimir_linea("[DMA ALERTA] No se pudo reservar arena contigua dedicada en arranque.");
        g_dma_base_fisica = 0;
        g_dma_tamano_bytes = 0;
        g_dma_total_paginas = 0;
        g_dma_iniciado = 1;
        return -1;
    }

    g_dma_total_paginas = (uint32_t)(g_dma_tamano_bytes / TAMANO_PAGINA);
    if (g_dma_total_paginas > (MAX_PALABRAS_BITMAP * 64)) {
        g_dma_total_paginas = MAX_PALABRAS_BITMAP * 64;
    }

    for (int i = 0; i < MAX_PALABRAS_BITMAP; i++) {
        g_dma_bitmap[i] = 0;
    }

    g_dma_paginas_ocupadas = 0;
    g_dma_asignaciones_activas = 0;
    g_dma_max_tamano_kb = 0;
    g_dma_iniciado = 1;

    serial_imprimir("[DMA Arena: Base ");
    serial_imprimir_hex(g_dma_base_fisica);
    serial_imprimir(" | Tam: ");
    serial_imprimir_dec(g_dma_tamano_bytes / (1024 * 1024));
    serial_imprimir(" MiB | Páginas: ");
    serial_imprimir_dec(g_dma_total_paginas);
    serial_imprimir_linea("]");

    return 0;
}

void *dma_asignar_bufer_contiguo(uint64_t bytes, uint64_t alineacion, uint64_t *dir_fisica) {
    if (!g_dma_iniciado) dma_iniciar();
    if (bytes == 0 || !dir_fisica || g_dma_total_paginas == 0) return NULL;

    if (alineacion < TAMANO_PAGINA) alineacion = TAMANO_PAGINA;

    uint32_t pags_necesarias = (uint32_t)((bytes + TAMANO_PAGINA - 1) / TAMANO_PAGINA);
    uint32_t paso_alineacion = (uint32_t)(alineacion / TAMANO_PAGINA);
    if (paso_alineacion == 0) paso_alineacion = 1;

    for (uint32_t i = 0; i + pags_necesarias <= g_dma_total_paginas; i += paso_alineacion) {
        int bloque_libre = 1;
        for (uint32_t j = 0; j < pags_necesarias; j++) {
            if (bitmap_probar_bit(i + j)) {
                bloque_libre = 0;
                break;
            }
        }

        if (bloque_libre) {
            for (uint32_t j = 0; j < pags_necesarias; j++) {
                bitmap_marcar_bit(i + j);
            }

            g_dma_paginas_ocupadas += pags_necesarias;
            g_dma_asignaciones_activas++;

            uint32_t tam_kb = (pags_necesarias * 4096) / 1024;
            if (tam_kb > g_dma_max_tamano_kb) {
                g_dma_max_tamano_kb = tam_kb;
            }

            uint64_t phys = g_dma_base_fisica + ((uint64_t)i * TAMANO_PAGINA);
            *dir_fisica = phys;

            uint64_t hhdm = memoria_obtener_hhdm_offset();
            void *virt = (void *)(phys + hhdm);

            memset(virt, 0, (uint64_t)pags_necesarias * TAMANO_PAGINA);
            return virt;
        }
    }

    serial_imprimir_linea("[DMA ALERTA] Memoria contigua en arena DMA agotada o fragmentada.");
    return NULL;
}

void dma_liberar_bufer_contiguo(void *dir_virtual, uint64_t dir_fisica, uint64_t bytes) {
    (void)dir_virtual;
    if (bytes == 0 || g_dma_total_paginas == 0) return;

    if (dir_fisica < g_dma_base_fisica || dir_fisica >= (g_dma_base_fisica + g_dma_tamano_bytes)) {
        return;
    }

    uint32_t inicio_bit = (uint32_t)((dir_fisica - g_dma_base_fisica) / TAMANO_PAGINA);
    uint32_t pags = (uint32_t)((bytes + TAMANO_PAGINA - 1) / TAMANO_PAGINA);

    for (uint32_t j = 0; j < pags; j++) {
        if ((inicio_bit + j) < g_dma_total_paginas) {
            bitmap_limpiar_bit(inicio_bit + j);
        }
    }

    if (g_dma_paginas_ocupadas >= pags) {
        g_dma_paginas_ocupadas -= pags;
    } else {
        g_dma_paginas_ocupadas = 0;
    }

    if (g_dma_asignaciones_activas > 0) {
        g_dma_asignaciones_activas--;
    }
}

void dma_sincronizar_cpu_a_dispositivo(const void *dir_virtual, uint64_t bytes) {
    if (!dir_virtual || bytes == 0) return;

    uintptr_t inicio = (uintptr_t)dir_virtual;
    uintptr_t fin = inicio + bytes;

    // Alinear al tamaño estándar de línea de caché x86_64 (64 bytes)
    inicio &= ~63ULL;

    for (uintptr_t p = inicio; p < fin; p += 64) {
        __asm__ volatile ("clflush (%0)" :: "r"(p) : "memory");
    }

    // Barrera completa de memoria: asegura que las escrituras del CPU estén en RAM
    __asm__ volatile ("mfence" ::: "memory");
}

void dma_sincronizar_dispositivo_a_cpu(const void *dir_virtual, uint64_t bytes) {
    (void)dir_virtual;
    (void)bytes;
    // En arquitecturas coherentes con DMA snooping, mfence es suficiente
    __asm__ volatile ("mfence" ::: "memory");
}

void dma_obtener_estadisticas(dma_estadisticas_t *est) {
    if (!est) return;
    est->arena_fisica_base = g_dma_base_fisica;
    est->arena_tamano_bytes = g_dma_tamano_bytes;
    est->paginas_totales = g_dma_total_paginas;
    est->paginas_en_uso = g_dma_paginas_ocupadas;
    est->paginas_libres = (g_dma_total_paginas >= g_dma_paginas_ocupadas) ?
                           (g_dma_total_paginas - g_dma_paginas_ocupadas) : 0;
    est->asignaciones_activas = g_dma_asignaciones_activas;
    est->tamano_maximo_asignado_kb = g_dma_max_tamano_kb;
}

int dma_ejecutar_autodiagnostico(void) {
    serial_imprimir_linea("--- INICIO DE AUTODIAGNÓSTICO DMA CONTIGUO ---");

    if (!g_dma_iniciado) {
        dma_iniciar();
    }

    if (g_dma_base_fisica == 0 || g_dma_total_paginas == 0) {
        serial_imprimir_linea("[FALLO] Arena DMA contigua no disponible.");
        return -1;
    }

    // 1. Asignar búfer de prueba de 64 KiB (16 páginas de 4 KiB) alineado a 64 KiB
    uint64_t tamano_prueba = 64ULL * 1024ULL;
    uint64_t alineacion = 64ULL * 1024ULL;
    uint64_t phys_base = 0;

    uint32_t asignadas_antes = g_dma_paginas_ocupadas;
    void *virt_ptr = dma_asignar_bufer_contiguo(tamano_prueba, alineacion, &phys_base);

    if (!virt_ptr || phys_base == 0) {
        serial_imprimir_linea("[FALLO] No se pudo asignar bloque contiguo de 64 KiB.");
        return -2;
    }

    // Comprobar alineación de 64 KiB
    if ((phys_base & (alineacion - 1)) != 0) {
        serial_imprimir_linea("[FALLO] Dirección física DMA no cumple la alineación de 64 KiB.");
        dma_liberar_bufer_contiguo(virt_ptr, phys_base, tamano_prueba);
        return -3;
    }

    // 2. Verificar continuidad física estricta página por página
    uint64_t hhdm = memoria_obtener_hhdm_offset();
    for (uint32_t p = 0; p < 16; p++) {
        uint64_t phys_esperada = phys_base + ((uint64_t)p * 4096ULL);
        uint64_t virt_pagina = (uint64_t)virt_ptr + ((uint64_t)p * 4096ULL);
        uint64_t phys_calculada = virt_pagina - hhdm;

        if (phys_calculada != phys_esperada) {
            serial_imprimir_linea("[FALLO] Discontinuidad física detectada en página de prueba.");
            dma_liberar_bufer_contiguo(virt_ptr, phys_base, tamano_prueba);
            return -4;
        }
    }

    // 3. Escribir canarios de integridad y comprobar sincronización de caché
    uint64_t *canarios = (uint64_t *)virt_ptr;
    uint64_t firma_canario = 0x507071AEC0507071ULL; // "5070 TAEK"

    for (uint32_t i = 0; i < (tamano_prueba / sizeof(uint64_t)); i += 512) {
        canarios[i] = firma_canario ^ i;
    }

    dma_sincronizar_cpu_a_dispositivo(virt_ptr, tamano_prueba);

    // Validar datos leídos tras barrera
    for (uint32_t i = 0; i < (tamano_prueba / sizeof(uint64_t)); i += 512) {
        if (canarios[i] != (firma_canario ^ i)) {
            serial_imprimir_linea("[FALLO] Corrupción de datos en búfer DMA contiguo.");
            dma_liberar_bufer_contiguo(virt_ptr, phys_base, tamano_prueba);
            return -5;
        }
    }

    // 4. Liberar búfer y verificar conteo de páginas
    dma_liberar_bufer_contiguo(virt_ptr, phys_base, tamano_prueba);

    if (g_dma_paginas_ocupadas != asignadas_antes) {
        serial_imprimir_linea("[FALLO] Fuga de memoria en el gestor DMA al liberar.");
        return -6;
    }

    serial_imprimir_linea("--- AUTODIAGNÓSTICO DMA CONTIGUO EXITOSO ---");
    return 0;
}
