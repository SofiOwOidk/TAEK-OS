#ifndef BASE_MEMORIA_H
#define BASE_MEMORIA_H

#include <stdint.h>
#include <stddef.h>

#define TAMANO_PAGINA 4096ULL

// Canarios de seguridad para cada bloque de Heap (vigilados por El Huevo)
#define CANARIO_BLOQUE_INICIO 0x7AEECC05ULL        // "TAEK OS"
#define CANARIO_BLOQUE_FIN    0xCAFEBABEDEAD1000ULL

typedef struct {
    uint64_t ram_fisica_total;
    uint64_t ram_fisica_usable;
    uint64_t paginas_totales;
    uint64_t paginas_libres;
    uint64_t paginas_en_uso;
    uint64_t heap_capacidad_total;
    uint64_t heap_bytes_en_uso;
    uint64_t heap_bloques_activos;
    uint64_t heap_bloques_libres;
    int      canarios_intactos;
} memoria_estadisticas_t;

// --- API NATIVA EN ESPAÑOL (ANILLO 0) ---

// Inicializa el PMM y el Kernel Heap a partir del mapa de Limine
void memoria_iniciar(void);

// Primitivas de Páginas Físicas (PMM - 4 KiB)
uint64_t pmm_asignar_pagina_fisica(void);
void    *pmm_asignar_pagina_virtual(void);
void     pmm_liberar_pagina_fisica(uint64_t phys);

// Primitivas del Heap del Kernel
void *asignar_memoria(uint64_t bytes);
void *asignar_memoria_cero(uint64_t bytes);
void *reasignar_memoria(void *ptr, uint64_t nuevo_tamano);
void  liberar_memoria(void *ptr);

// Auditoría de Integridad (Vigilada por El Huevo)
int  memoria_verificar_integridad(void);
void memoria_obtener_estadisticas(memoria_estadisticas_t *est);
uint64_t memoria_obtener_hhdm_offset(void);

// --- SHIMS DE COMPATIBILIDAD CON LINUX (DRIVERS / SUBSISTEMAS) ---

#define GFP_KERNEL 0
#define GFP_ATOMIC 1
#define GFP_DMA    2
#define __GFP_ZERO 4

static inline void *kmalloc(size_t size, int flags) {
    if (flags & __GFP_ZERO) {
        return asignar_memoria_cero(size);
    }
    return asignar_memoria(size);
}

static inline void *kzalloc(size_t size, int flags) {
    (void)flags;
    return asignar_memoria_cero(size);
}

static inline void kfree(const void *ptr) {
    liberar_memoria((void *)ptr);
}

static inline void *vmalloc(size_t size) {
    return asignar_memoria(size);
}

static inline void vfree(const void *ptr) {
    liberar_memoria((void *)ptr);
}

static inline void *kcalloc(size_t n, size_t size, int flags) {
    (void)flags;
    return asignar_memoria_cero(n * size);
}

static inline void *krealloc(void *ptr, size_t new_size, int flags) {
    (void)flags;
    return reasignar_memoria(ptr, new_size);
}

#endif // BASE_MEMORIA_H
