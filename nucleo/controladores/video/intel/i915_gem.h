#ifndef CONTROLADORES_VIDEO_INTEL_I915_GEM_H
#define CONTROLADORES_VIDEO_INTEL_I915_GEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "compatibilidad/linux_i915/linux_types.h"
#include "compatibilidad/linux_i915/memoria_i915.h"

#define I915_GEM_MAX_OBJETOS 128
#define I915_GEM_PAGE_SIZE   4096

// Estructura de Objeto de Memoria Gráfica GEM (Graphics Execution Manager)
struct drm_i915_gem_object {
    uint32_t          handle;          // Identificador numérico único
    size_t            tamano;          // Tamaño en bytes (alineado a 4096)
    uint32_t          n_paginas;       // Número de páginas físicas
    struct page     **paginas;         // Vector de páginas dispersas
    uint64_t          ggtt_offset;     // Dirección virtual en la GGTT de la GPU (0 = no mapeado)
    int               pinned;          // Contador de fijaciones en memoria física
    int               mapeado_ggtt;    // 1 si está insertado en la tabla GGTT
    void             *dir_cpu;         // Dirección virtual CPU para acceso directo del host
    uint32_t          dominio_lectura; // Sincronización de caché CPU vs GPU
    uint32_t          dominio_escritura;
    bool              en_uso_gpu;      // Marcar mientras el motor procesa el buffer
};

// Inicialización del gestor de objetos GEM
int  i915_gem_iniciar(uint64_t ggtt_base_libre, uint64_t ggtt_limite);
void i915_gem_destruir(void);

// Creación y liberación de objetos GEM
struct drm_i915_gem_object *i915_gem_crear_objeto(size_t tamano);
int  i915_gem_liberar_objeto(struct drm_i915_gem_object *obj);
struct drm_i915_gem_object *i915_gem_buscar_handle(uint32_t handle);

// Mapeo y pinning en la tabla global de traducción GGTT
int  i915_gem_pin_en_ggtt(struct drm_i915_gem_object *obj, uint64_t *gpu_addr_out);
int  i915_gem_unpin_de_ggtt(struct drm_i915_gem_object *obj);

// Acceso CPU coherente
void *i915_gem_mmap_cpu(struct drm_i915_gem_object *obj);

#endif // CONTROLADORES_VIDEO_INTEL_I915_GEM_H
