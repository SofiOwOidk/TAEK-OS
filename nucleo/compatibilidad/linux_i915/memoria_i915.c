#define _GNU_SOURCE
#include "memoria_i915.h"

#ifdef I915_DEBUG_SG
#include <stdio.h>
#endif

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

#define MAX_TRACKED_PAGES 16384

static struct page     g_pool[MAX_TRACKED_PAGES];
static spinlock_t      g_pool_lock;
static bool            g_iniciado = false;
static i915_pmm_estadisticas g_stats;

#ifdef TAEK_HOST_TEST
static i915_traductor_host_fn g_traductor_host = NULL;
void i915_pmm_establecer_traductor(i915_traductor_host_fn f) { g_traductor_host = f; }
#endif

/* Traducción real de virtual a física. En host la identidad es la dirección del
 * propio búfer de prueba; en el núcleo la aporta el paginador. Nunca se inventa
 * un desplazamiento: si no hay traducción, la asignación falla. Se traduce CADA
 * página por separado; no se supone continuidad física. */
static int i915_traducir_fisica(void *virt, phys_addr_t *salida) {
#ifdef TAEK_HOST_TEST
    if (g_traductor_host) return g_traductor_host(virt, salida);
    *salida = (phys_addr_t)(uintptr_t)virt;
    return 0;
#else
    phys_addr_t pa = paginacion_obtener_fisica((uint64_t)(uintptr_t)virt);
    if (pa == 0) return -1;
    *salida = pa;
    return 0;
#endif
}

static void pmm_iniciar(void) {
    if (g_iniciado) return;
    spin_lock_init(&g_pool_lock);
    for (int i = 0; i < MAX_TRACKED_PAGES; i++) {
        g_pool[i].virt = NULL;
        g_pool[i].phys_addr = 0;
        atomic_set(&g_pool[i].refcount, 0);
        atomic_set(&g_pool[i].pins, 0);
        atomic_set(&g_pool[i].mappings, 0);
        g_pool[i].order = 0;
        g_pool[i].estado = 0;
        g_pool[i].head = NULL;
        g_pool[i].bloque = NULL;
    }
    g_stats = (i915_pmm_estadisticas){0};
    g_iniciado = true;
}

void i915_pmm_obtener_estadisticas(i915_pmm_estadisticas *st) {
    if (!st) return;
    if (!g_iniciado) pmm_iniciar();
    unsigned long flags;
    spin_lock_irqsave(&g_pool_lock, flags);
    *st = g_stats;
    spin_unlock_irqrestore(&g_pool_lock, flags);
}


int i915_page_pin(struct page *page) {
    if (!page) return -EINVAL;
    unsigned long flags; spin_lock_irqsave(&g_pool_lock, flags);
    if (page->estado != PAGE_ESTADO_ASIGNADA) {
        spin_unlock_irqrestore(&g_pool_lock, flags);
        return -EINVAL;
    }
    atomic_inc(&page->pins);
    g_stats.pins++;
    spin_unlock_irqrestore(&g_pool_lock, flags);
    return 0;
}

static void i915_evaluar_liberacion_bloque_locked(struct page *page);

void i915_page_unpin(struct page *page) {
    if (!page) return;
    unsigned long flags; spin_lock_irqsave(&g_pool_lock, flags);
    if (page->estado == PAGE_ESTADO_ASIGNADA && atomic_read(&page->pins) > 0) {
        atomic_dec(&page->pins);
        if (g_stats.pins) g_stats.pins--;
        i915_evaluar_liberacion_bloque_locked(page);
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);
}

int i915_page_mapear(struct page *page) {
    if (!page) return -EINVAL;
    unsigned long flags; spin_lock_irqsave(&g_pool_lock, flags);
    if (page->estado != PAGE_ESTADO_ASIGNADA) {
        spin_unlock_irqrestore(&g_pool_lock, flags);
        return -EINVAL;
    }
    atomic_inc(&page->mappings);
    g_stats.mappings++;
    spin_unlock_irqrestore(&g_pool_lock, flags);
    return 0;
}

void i915_page_desmapear(struct page *page) {
    if (!page) return;
    unsigned long flags; spin_lock_irqsave(&g_pool_lock, flags);
    if (page->estado == PAGE_ESTADO_ASIGNADA && atomic_read(&page->mappings) > 0) {
        atomic_dec(&page->mappings);
        if (g_stats.mappings) g_stats.mappings--;
        i915_evaluar_liberacion_bloque_locked(page);
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);
}

struct page *i915_phys_to_page(phys_addr_t pa) {
    if (!g_iniciado) pmm_iniciar();
    unsigned long flags;
    spin_lock_irqsave(&g_pool_lock, flags);
    for (int i = 0; i < MAX_TRACKED_PAGES; i++) {
        if (g_pool[i].estado != PAGE_ESTADO_LIBRE && g_pool[i].phys_addr == pa) {
            spin_unlock_irqrestore(&g_pool_lock, flags);
            return &g_pool[i];
        }
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);
    return NULL;
}

static bool bloque_limpio(const struct page *head, unsigned int n) {
    for (unsigned int j = 0; j < n; j++)
        if (atomic_read(&head[j].refcount) != 0) return false;
    return true;
}

static bool bloque_sin_pins_mappings(const struct page *head, unsigned int n) {
    for (unsigned int j = 0; j < n; j++)
        if (atomic_read(&head[j].pins) > 0 || atomic_read(&head[j].mappings) > 0) return false;
    return true;
}

/* Rutina unificada de evaluación de liberación con estado de destrucción protegido.
 * Debe invocarse con g_pool_lock adquirido. */
static void i915_evaluar_liberacion_bloque_locked(struct page *page) {
    if (!page || page->estado != PAGE_ESTADO_ASIGNADA) return;
    struct page *h = page->head ? page->head : page;
    if (h->estado != PAGE_ESTADO_ASIGNADA) return;
    unsigned int n = 1U << h->order;

    if (!bloque_limpio(h, n) || !bloque_sin_pins_mappings(h, n)) {
        return; /* aún en uso por referencias, pins o mappings */
    }

    /* Transición protegida bajo el mismo cerrojo: marcar DESTRUYENDO */
    for (unsigned int j = 0; j < n; j++) {
        h[j].estado = PAGE_ESTADO_DESTRUYENDO;
    }

    void *bloque = h->bloque;

    for (unsigned int j = 0; j < n; j++) {
        h[j].virt = NULL;
        h[j].phys_addr = 0;
        h[j].estado = PAGE_ESTADO_LIBRE;
        atomic_set(&h[j].refcount, 0);
        atomic_set(&h[j].pins, 0);
        atomic_set(&h[j].mappings, 0);
        h[j].head = NULL;
        h[j].bloque = NULL;
    }
    if (g_stats.asignadas) g_stats.asignadas--;
    if (g_stats.paginas >= n) g_stats.paginas -= n; else g_stats.paginas = 0;

    i915_kfree(bloque);
}

void put_page(struct page *page) {
    if (!page) return;
    unsigned long flags;
    spin_lock_irqsave(&g_pool_lock, flags);
    if (page->estado == PAGE_ESTADO_ASIGNADA && atomic_read(&page->refcount) > 0) {
        atomic_dec(&page->refcount);
        if (g_stats.refs) g_stats.refs--;
        i915_evaluar_liberacion_bloque_locked(page);
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);
}

struct page *alloc_page(gfp_t gfp_mask) { return alloc_pages(gfp_mask, 0); }

struct page *alloc_pages(gfp_t gfp_mask, unsigned int order) {
    if (!g_iniciado) pmm_iniciar();
    if (order > 10) return NULL;   /* límite máximo de orden 10 (4 MiB) */

    unsigned int n_paginas = 1U << order;
    size_t bytes = (size_t)n_paginas * PAGE_SIZE;
    size_t alineacion = bytes;     /* Linux 6.6: alineación natural estricta del bloque 2^order */

    void *raw = i915_kmalloc(bytes + alineacion);
    if (!raw) return NULL;
    uint8_t *virt = (uint8_t *)(((uintptr_t)raw + alineacion - 1) & ~(uintptr_t)(alineacion - 1));

    if (gfp_mask & GFP_ZERO) {
        for (size_t i = 0; i < bytes; i++) virt[i] = 0;
    }

    phys_addr_t pa_0 = 0;
    if (i915_traducir_fisica(virt, &pa_0) != 0) {
        i915_kfree(raw);
        return NULL;
    }

    /* CONTRATO LINUX 6.6: Alineación natural del bloque físico */
    if ((pa_0 & (alineacion - 1)) != 0) {
        i915_kfree(raw);
        return NULL;
    }

    /* CONTRATO LINUX 6.6: Contigüidad física estricta de las 2^order páginas */
    for (unsigned int j = 1; j < n_paginas; j++) {
        phys_addr_t pa_j = 0;
        if (i915_traducir_fisica(virt + (size_t)j * PAGE_SIZE, &pa_j) != 0 ||
            pa_j != pa_0 + (phys_addr_t)j * PAGE_SIZE) {
            i915_kfree(raw);
            return NULL;   /* rechazar páginas físicamente dispersas en alloc_pages */
        }
    }

    unsigned long flags;
    spin_lock_irqsave(&g_pool_lock, flags);

    int base_idx = -1;
    for (int i = 0; i + (int)n_paginas <= MAX_TRACKED_PAGES; i++) {
        bool libres = true;
        for (unsigned int j = 0; j < n_paginas; j++) {
            if (g_pool[i + j].estado != PAGE_ESTADO_LIBRE) { libres = false; break; }
        }
        if (libres) { base_idx = i; break; }
    }
    if (base_idx < 0) {
        spin_unlock_irqrestore(&g_pool_lock, flags);
        i915_kfree(raw);
        return NULL;
    }

    struct page *head = &g_pool[base_idx];
    for (unsigned int j = 0; j < n_paginas; j++) {
        struct page *p = &g_pool[base_idx + j];
        p->virt = virt + (size_t)j * PAGE_SIZE;
        p->phys_addr = pa_0 + (phys_addr_t)j * PAGE_SIZE;
        p->order = order;
        p->estado = PAGE_ESTADO_ASIGNADA;
        p->head = head;
        p->bloque = (j == 0) ? raw : NULL;
        atomic_set(&p->refcount, 1);
        atomic_set(&p->pins, 0);
        atomic_set(&p->mappings, 0);
    }

    g_stats.asignadas++;
    g_stats.paginas += n_paginas;
    g_stats.refs += n_paginas;
    spin_unlock_irqrestore(&g_pool_lock, flags);
    return head;
}

int __free_pages(struct page *page, unsigned int order) {
    if (!page) return -EINVAL;
    struct page *h = page->head ? page->head : page;
    if (page != h || order != h->order) return -EINVAL;
    unsigned int n = 1U << order;

    unsigned long flags;
    spin_lock_irqsave(&g_pool_lock, flags);
    if (h->estado != PAGE_ESTADO_ASIGNADA) {
        spin_unlock_irqrestore(&g_pool_lock, flags);
        return -EINVAL;
    }
    if (!bloque_sin_pins_mappings(h, n)) {
        g_stats.fugas++;
        spin_unlock_irqrestore(&g_pool_lock, flags);
        return -EBUSY;
    }
    for (unsigned int j = 0; j < n; j++) {
        if (atomic_read(&h[j].refcount) > 0) {
            atomic_dec(&h[j].refcount);
            if (g_stats.refs) g_stats.refs--;
        }
    }
    i915_evaluar_liberacion_bloque_locked(h);
    spin_unlock_irqrestore(&g_pool_lock, flags);
    return 0;
}

/* Asignador explícito de páginas dispersas para superficies/SG */
struct page **i915_alloc_paginas_dispersas(unsigned int n_paginas, gfp_t gfp_mask) {
    if (n_paginas == 0) return NULL;
    struct page **pages = i915_kmalloc((size_t)n_paginas * sizeof(struct page *));
    if (!pages) return NULL;

    for (unsigned int i = 0; i < n_paginas; i++) {
        pages[i] = alloc_page(gfp_mask);
        if (!pages[i]) {
            for (unsigned int k = 0; k < i; k++) {
                put_page(pages[k]);
            }
            i915_kfree(pages);
            return NULL;
        }
    }
    return pages;
}

void i915_free_paginas_dispersas(struct page **pages, unsigned int n_paginas) {
    if (!pages) return;
    for (unsigned int i = 0; i < n_paginas; i++) {
        if (pages[i]) put_page(pages[i]);
    }
    i915_kfree(pages);
}

// ============================================================================
// SCATTER-GATHER
// ============================================================================
int sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp_mask) {
    (void)gfp_mask;
    if (!table || nents == 0) return -EINVAL;
    struct scatterlist *sgl = i915_kmalloc((size_t)nents * sizeof(*sgl));
    if (!sgl) return -ENOMEM;
    for (unsigned int i = 0; i < nents; i++) {
        sgl[i].page = NULL; sgl[i].offset = 0; sgl[i].length = 0;
        sgl[i].dma_address = 0; sgl[i].dma_length = 0;
    }
    table->sgl = sgl; table->nents = nents; table->orig_nents = nents;
    table->paginas = NULL; table->n_paginas = 0;
    return 0;
}

/* Libera la tabla y exactamente las referencias que tomó. */
void sg_free_table(struct sg_table *table) {
    if (!table) return;
    if (table->paginas) {
        for (unsigned int i = 0; i < table->n_paginas; i++) put_page(table->paginas[i]);
        i915_kfree(table->paginas);
        table->paginas = NULL;
        table->n_paginas = 0;
    }
    if (table->sgl) i915_kfree(table->sgl);
    table->sgl = NULL; table->nents = 0; table->orig_nents = 0;
}

/* Resuelve la página `page_idx` dentro de un segmento. Sólo es válido para
 * segmentos coalescidos (físicamente contiguos por construcción). */
struct page *sg_get_page(struct scatterlist *sg, unsigned int page_idx) {
    if (!sg || !sg->page) return NULL;
    if (page_idx == 0) return sg->page;
    return i915_phys_to_page(sg->page->phys_addr + (phys_addr_t)page_idx * PAGE_SIZE);
}

/* ¿La página i continúa físicamente a la página i-1? */
static int paginas_contiguas(struct page *a, struct page *b) {
    return a && b && a->phys_addr != 0 && b->phys_addr == a->phys_addr + PAGE_SIZE;
}

int sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages,
                              unsigned int n_pages, unsigned int offset,
                              unsigned long size, gfp_t gfp_mask) {
    if (!sgt || !pages || n_pages == 0 || size == 0 || offset >= PAGE_SIZE) return -EINVAL;

    unsigned long capacidad = (unsigned long)n_pages * PAGE_SIZE;
    if ((unsigned long)offset + size > capacidad) return -EINVAL;

    /* Pasada 1: validar y contar páginas usadas y segmentos. */
    unsigned long restante = size;
    unsigned int nents = 0, usados = 0;
    for (unsigned int i = 0; i < n_pages && restante > 0; i++) {
        if (!pages[i] || !pages[i]->estado || pages[i]->phys_addr == 0) return -EINVAL;
        unsigned int disp = (i == 0) ? (unsigned int)(PAGE_SIZE - offset) : (unsigned int)PAGE_SIZE;
        unsigned long toma = disp < restante ? disp : restante;
        if (nents == 0) nents = 1;
        else if (!paginas_contiguas(pages[i - 1], pages[i])) nents++;
        (void)toma;
        restante -= toma;
        usados++;
    }
    if (restante != 0) return -EINVAL;

    int ret = sg_alloc_table(sgt, nents, gfp_mask);
    if (ret != 0) return ret;

    /* Referencias sólo para las páginas que la tabla representa. */
    sgt->paginas = i915_kmalloc((size_t)usados * sizeof(struct page *));
    if (!sgt->paginas) { sg_free_table(sgt); return -ENOMEM; }
    for (unsigned int i = 0; i < usados; i++) {
        sgt->paginas[i] = pages[i];
        get_page(pages[i]);
    }
    sgt->n_paginas = usados;

    /* Pasada 2: rellenar segmentos. */
    restante = size;
    unsigned int ent = 0;
    for (unsigned int i = 0; i < usados && restante > 0; i++) {
        unsigned int disp = (i == 0) ? (unsigned int)(PAGE_SIZE - offset) : (unsigned int)PAGE_SIZE;
        unsigned long toma = disp < restante ? disp : restante;
        if (i == 0) {
            sgt->sgl[0].page = pages[0];
            sgt->sgl[0].offset = offset;
            sgt->sgl[0].length = (unsigned int)toma;
        } else if (paginas_contiguas(pages[i - 1], pages[i])) {
            if ((unsigned long)sgt->sgl[ent].length + toma > 0xFFFFF000UL) {
                if (++ent >= nents) goto err_rollback;
                sgt->sgl[ent].page = pages[i];
                sgt->sgl[ent].offset = 0;
                sgt->sgl[ent].length = (unsigned int)toma;
            } else {
                sgt->sgl[ent].length += (unsigned int)toma;
            }
        } else {
            if (++ent >= nents) goto err_rollback;
            sgt->sgl[ent].page = pages[i];
            sgt->sgl[ent].offset = 0;
            sgt->sgl[ent].length = (unsigned int)toma;
        }
        restante -= toma;
    }
    sgt->nents = ent + 1;
    return 0;

err_rollback:
    sg_free_table(sgt);   /* libera referencias tomadas, sgl y paginas */
    return -EIO;
}

// ============================================================================
// DMA
// ============================================================================
int dma_map_sg(void *dev, struct scatterlist *sg, int nents, int dir) {
    (void)dir;
    if (!sg || nents <= 0) return -EINVAL;
    struct dma_dispositivo *d = (struct dma_dispositivo *)dev;
    if (!d || !d->traducir) return -EINVAL;   /* sin traducción NO se inventa */

    for (int i = 0; i < nents; i++) {
        if (!sg[i].page || !sg[i].page->estado || sg[i].length == 0) {
            dma_unmap_sg(dev, sg, i, dir);
            return -EINVAL;
        }
        if (d->max_segmento && sg[i].length > d->max_segmento) {
            dma_unmap_sg(dev, sg, i, dir);
            return -EIO;
        }
        dma_addr_t ini = d->traducir(d->ctx, sg[i].page->phys_addr + sg[i].offset, sg[i].length);
        if (ini == 0) {
            dma_unmap_sg(dev, sg, i, dir);
            return -EFAULT;
        }
        /* Rechazar explícitamente desbordamiento aritmético de 64 bits */
        if ((uint64_t)sg[i].length - 1 > UINT64_MAX - (uint64_t)ini) {
            dma_unmap_sg(dev, sg, i, dir);
            return -EOVERFLOW;
        }
        dma_addr_t fin = ini + (dma_addr_t)sg[i].length - 1;
        if (fin < ini) {
            dma_unmap_sg(dev, sg, i, dir);
            return -EOVERFLOW;
        }
        if (d->mascara && ((ini & ~d->mascara) || (fin & ~d->mascara))) {
            dma_unmap_sg(dev, sg, i, dir);
            return -EFAULT;   /* la máscara se aplica a TODO el rango */
        }
        if (i915_page_mapear(sg[i].page) != 0) {
            dma_unmap_sg(dev, sg, i, dir);
            return -EINVAL;
        }
        sg[i].dma_address = ini;
        sg[i].dma_length  = sg[i].length;
    }
    return nents;
}

void dma_unmap_sg(void *dev, struct scatterlist *sg, int nents, int dir) {
    (void)dev; (void)dir;
    if (!sg || nents <= 0) return;
    for (int i = 0; i < nents; i++) {
        if (sg[i].dma_address && sg[i].page) i915_page_desmapear(sg[i].page);
        sg[i].dma_address = 0;
        sg[i].dma_length  = 0;
    }
}
