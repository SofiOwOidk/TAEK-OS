#include "i915_gem.h"
#include "i915_drv.h"
#include "arquitectura/x86_64/serial.h"
#include <string.h>

#ifndef TAEK_HOST_TEST
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#include "base/paginacion.h"
#else
#include <stdlib.h>
void *asignar_memoria(uint64_t n);
void  liberar_memoria(void *p);
#endif
#define i915_kmalloc(_n) asignar_memoria(_n)
#define i915_kfree(_p)   liberar_memoria(_p)

static struct drm_i915_gem_object g_gem_objetos[I915_GEM_MAX_OBJETOS];
static uint32_t                   g_proximo_handle = 1;
static uint64_t                   g_ggtt_base_libre = 0;
static uint64_t                   g_ggtt_cursor_libre = 0;
static uint64_t                   g_ggtt_limite_max = 0;
static int                        g_gem_iniciado = 0;

int i915_gem_iniciar(uint64_t ggtt_base_libre, uint64_t ggtt_limite) {
    memset(g_gem_objetos, 0, sizeof(g_gem_objetos));
    g_proximo_handle = 1;
    g_ggtt_base_libre = ggtt_base_libre;
    g_ggtt_cursor_libre = ggtt_base_libre;
    g_ggtt_limite_max = ggtt_limite;
    g_gem_iniciado = 1;
    serial_imprimir("[I915_GEM] Gestor GEM iniciado. Rango GGTT libre: 0x");
    serial_imprimir_hex(g_ggtt_cursor_libre);
    serial_imprimir(" - 0x");
    serial_imprimir_hex(g_ggtt_limite_max);
    serial_imprimir_linea("");
    return 0;
}

void i915_gem_destruir(void) {
    if (!g_gem_iniciado) return;
    for (int i = 0; i < I915_GEM_MAX_OBJETOS; i++) {
        if (g_gem_objetos[i].handle != 0) {
            i915_gem_liberar_objeto(&g_gem_objetos[i]);
        }
    }
    g_gem_iniciado = 0;
}

struct drm_i915_gem_object *i915_gem_crear_objeto(size_t tamano) {
    if (!g_gem_iniciado || tamano == 0) return NULL;

    size_t tamano_alineado = (tamano + I915_GEM_PAGE_SIZE - 1) & ~(I915_GEM_PAGE_SIZE - 1);
    uint32_t n_paginas = (uint32_t)(tamano_alineado / I915_GEM_PAGE_SIZE);

    // Buscar una ranura libre en la tabla GEM
    struct drm_i915_gem_object *obj = NULL;
    for (int i = 0; i < I915_GEM_MAX_OBJETOS; i++) {
        if (g_gem_objetos[i].handle == 0) {
            obj = &g_gem_objetos[i];
            break;
        }
    }
    if (!obj) {
        serial_imprimir_linea("[I915_GEM] ERROR: Tabla de objetos GEM agotada.");
        return NULL;
    }

    // Asignar páginas físicas independientes
    struct page **pages = i915_alloc_paginas_dispersas(n_paginas, GFP_KERNEL | GFP_ZERO);
    if (!pages) {
        serial_imprimir_linea("[I915_GEM] ERROR: Memoria física insuficiente para objeto GEM.");
        return NULL;
    }

    void *cpu_buf = i915_kmalloc(tamano_alineado);
    if (!cpu_buf) {
        i915_free_paginas_dispersas(pages, n_paginas);
        serial_imprimir_linea("[I915_GEM] ERROR: Memoria virtual insuficiente para buffer CPU.");
        return NULL;
    }
    memset(cpu_buf, 0, tamano_alineado);

    memset(obj, 0, sizeof(*obj));
    obj->handle = g_proximo_handle++;
    obj->tamano = tamano_alineado;
    obj->n_paginas = n_paginas;
    obj->paginas = pages;
    obj->dir_cpu = cpu_buf;
    obj->pinned = 0;
    obj->mapeado_ggtt = 0;
    obj->en_uso_gpu = false;

    return obj;
}

struct drm_i915_gem_object *i915_gem_buscar_handle(uint32_t handle) {
    if (!g_gem_iniciado || handle == 0) return NULL;
    for (int i = 0; i < I915_GEM_MAX_OBJETOS; i++) {
        if (g_gem_objetos[i].handle == handle) {
            return &g_gem_objetos[i];
        }
    }
    return NULL;
}

int i915_gem_pin_en_ggtt(struct drm_i915_gem_object *obj, uint64_t *gpu_addr_out) {
    if (!obj || !gpu_addr_out) return -EINVAL;

    if (obj->mapeado_ggtt) {
        obj->pinned++;
        *gpu_addr_out = obj->ggtt_offset;
        return 0;
    }

    // Asignar rango en GGTT respetando la preservación del GOP
    uint64_t tam_bytes = (uint64_t)obj->n_paginas * 4096ULL;
    if (g_ggtt_cursor_libre + tam_bytes > g_ggtt_limite_max) {
        serial_imprimir_linea("[I915_GEM] ERROR: Espacio de direcciones GGTT agotado.");
        return -ENOMEM;
    }

    uint64_t offset_gpu = g_ggtt_cursor_libre;
    g_ggtt_cursor_libre += tam_bytes;

    // Escribir las PTEs correspondientes en la tabla GGTT
    for (uint32_t p = 0; p < obj->n_paginas; p++) {
        phys_addr_t pa = page_to_phys(obj->paginas[p]);
        uint64_t pte_val = (uint64_t)pa | GEN8_PTE_VALID | GEN8_PTE_CACHE_LLC;
        uint32_t pte_offset = (uint32_t)(GEN8_GGTT_PTE_OFFSET + ((offset_gpu / 4096 + p) * 8));

        // Escritura de 64 bits dividida en 2 palabras de 32 bits
        i915_escribir_mmio_32(pte_offset, (uint32_t)(pte_val & 0xFFFFFFFFU));
        i915_escribir_mmio_32(pte_offset + 4, (uint32_t)(pte_val >> 32));
    }

    obj->ggtt_offset = offset_gpu;
    obj->mapeado_ggtt = 1;
    obj->pinned = 1;
    *gpu_addr_out = offset_gpu;

    return 0;
}

int i915_gem_unpin_de_ggtt(struct drm_i915_gem_object *obj) {
    if (!obj || !obj->mapeado_ggtt) return -EINVAL;

    if (obj->pinned > 1) {
        obj->pinned--;
        return 0;
    }

    // Limpiar PTEs en la tabla GGTT
    for (uint32_t p = 0; p < obj->n_paginas; p++) {
        uint32_t pte_offset = (uint32_t)(GEN8_GGTT_PTE_OFFSET + ((obj->ggtt_offset / 4096 + p) * 8));
        i915_escribir_mmio_32(pte_offset, 0);
        i915_escribir_mmio_32(pte_offset + 4, 0);
    }

    obj->mapeado_ggtt = 0;
    obj->pinned = 0;
    obj->ggtt_offset = 0;

    // Recalcular cursor libre de GGTT como el extremo superior de los objetos actualmente mapeados
    uint64_t max_offset = g_ggtt_base_libre;
    for (int i = 0; i < I915_GEM_MAX_OBJETOS; i++) {
        if (g_gem_objetos[i].handle != 0 && g_gem_objetos[i].mapeado_ggtt) {
            uint64_t fin = g_gem_objetos[i].ggtt_offset + ((uint64_t)g_gem_objetos[i].n_paginas * 4096ULL);
            if (fin > max_offset) {
                max_offset = fin;
            }
        }
    }
    g_ggtt_cursor_libre = max_offset;

    return 0;
}

int i915_gem_liberar_objeto(struct drm_i915_gem_object *obj) {
    if (!obj || obj->handle == 0) return -EINVAL;

    if (obj->en_uso_gpu) {
        serial_imprimir_linea("[I915_GEM] ADVERTENCIA: Intento de liberar objeto activo en GPU. Rechazado por quiescencia.");
        return -EBUSY;
    }

    if (obj->mapeado_ggtt) {
        i915_gem_unpin_de_ggtt(obj);
    }

    if (obj->dir_cpu) {
        i915_kfree(obj->dir_cpu);
        obj->dir_cpu = NULL;
    }

    if (obj->paginas) {
        i915_free_paginas_dispersas(obj->paginas, obj->n_paginas);
        obj->paginas = NULL;
    }

    memset(obj, 0, sizeof(*obj));
    return 0;
}

void *i915_gem_mmap_cpu(struct drm_i915_gem_object *obj) {
    if (!obj || !obj->paginas) return NULL;
    return obj->dir_cpu;
}
