#ifndef BASE_DMA_H
#define BASE_DMA_H

#include <stdint.h>
#include <stddef.h>

// Estadísticas del subsistema de memoria DMA físicamente contigua
typedef struct {
    uint64_t arena_fisica_base;
    uint64_t arena_tamano_bytes;
    uint32_t paginas_totales;
    uint32_t paginas_en_uso;
    uint32_t paginas_libres;
    uint32_t asignaciones_activas;
    uint32_t tamano_maximo_asignado_kb;
} dma_estadisticas_t;

// --- API NATIVA EN ESPAÑOL (ANILLO 0) ---

// Inicializa el gestor de memoria DMA física contigua y su mapa de bits
int   dma_iniciar(void);

// Asigna un bloque de páginas físicamente contiguas con la alineación solicitada
// Retorna el puntero virtual (HHDM) y escribe la dirección física en *dir_fisica
void *dma_asignar_bufer_contiguo(uint64_t bytes, uint64_t alineacion, uint64_t *dir_fisica);

// Libera un búfer DMA asignado previamente
void  dma_liberar_bufer_contiguo(void *dir_virtual, uint64_t dir_fisica, uint64_t bytes);

// Barrera de coherencia: Fuerza el vaciado de las líneas de caché de CPU (clflush/clflushopt + mfence)
void  dma_sincronizar_cpu_a_dispositivo(const void *dir_virtual, uint64_t bytes);

// Barrera de sincronización: Asegura visibilidad de escrituras DMA de hardware en CPU
void  dma_sincronizar_dispositivo_a_cpu(const void *dir_virtual, uint64_t bytes);

// Obtiene las métricas en vivo del gestor DMA contiguo
void  dma_obtener_estadisticas(dma_estadisticas_t *est);

// Autodiagnóstico riguroso de continuidad física, sincronización de caché y canarios de memoria
int   dma_ejecutar_autodiagnostico(void);

#endif // BASE_DMA_H
