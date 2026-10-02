#ifndef LINUX_I915_MEMORIA_H
#define LINUX_I915_MEMORIA_H

#include "linux_types.h"

// ----------------------------------------------------------------------------
// PÁGINA FÍSICA. Cada descriptor cubre una página de 4 KiB. Los bloques de
// orden >0 agrupan descriptores consecutivos en la tabla; `head` apunta a la
// cabeza del bloque y `bloque` (sólo en la cabeza) es la asignación real que se
// devuelve al asignador. Cada página lleva su propio refcount; el bloque sólo se
// libera cuando TODAS sus páginas tienen refcount 0 y no hay pins ni mappings.
// La dirección física se traduce página a página: no se supone continuidad.
// ----------------------------------------------------------------------------
struct page {
    void       *virt;       /* dirección CPU de esta página */
    phys_addr_t phys_addr;  /* dirección física real y traducida (0 = no traducida) */
    atomic_t    refcount;   /* get_page / put_page (por página) */
    atomic_t    pins;       /* pin / unpin */
    atomic_t    mappings;   /* mapear / desmapear */
    unsigned    order;      /* orden del bloque al que pertenece */
    unsigned    estado;     /* 0 libre, 1 asignada */
    struct page *head;      /* cabeza del bloque (== sí misma si orden 0) */
    void       *bloque;     /* base real de la asignación (sólo en la cabeza) */
};

static inline void get_page(struct page *page) { if (page && page->estado) atomic_inc(&page->refcount); }
void put_page(struct page *page);

int  i915_page_pin(struct page *page);
void i915_page_unpin(struct page *page);
int  i915_page_mapear(struct page *page);
void i915_page_desmapear(struct page *page);

static inline phys_addr_t page_to_phys(const struct page *page) { return page ? page->phys_addr : 0; }
static inline void        *page_to_virt(const struct page *page) { return page ? page->virt : NULL; }
struct page               *i915_phys_to_page(phys_addr_t pa);

// ----------------------------------------------------------------------------
// ASIGNADOR POR PÁGINAS (PMM real sobre el heap/arena del núcleo)
// ----------------------------------------------------------------------------
#define PAGE_ESTADO_LIBRE       0
#define PAGE_ESTADO_ASIGNADA    1
#define PAGE_ESTADO_DESTRUYENDO 2

struct page *alloc_page(gfp_t gfp_mask);
struct page *alloc_pages(gfp_t gfp_mask, unsigned int order);
int          __free_pages(struct page *page, unsigned int order);

/* Asignador explícito de páginas dispersas para superficies/SG (cada página independiente) */
struct page **i915_alloc_paginas_dispersas(unsigned int n_paginas, gfp_t gfp_mask);
void          i915_free_paginas_dispersas(struct page **pages, unsigned int n_paginas);

typedef struct {
    unsigned long asignadas;   /* bloques en uso */
    unsigned long libres;      /* descriptores reutilizables */
    unsigned long paginas;     /* páginas de 4 KiB en uso */
    unsigned long refs;        /* suma de refcount */
    unsigned long pins;        /* suma de fijaciones */
    unsigned long mappings;    /* suma de mappings */
    unsigned long fugas;       /* liberaciones con refs/pins/mappings vivos */
} i915_pmm_estadisticas;

void i915_pmm_obtener_estadisticas(i915_pmm_estadisticas *st);

#ifdef TAEK_HOST_TEST
/* Gancho de prueba: permite inyectar una traducción física no contigua y
 * verificar que el PMM traduce página a página (no calcula pa + j*PAGE_SIZE). */
typedef int (*i915_traductor_host_fn)(void *virt, phys_addr_t *salida);
void i915_pmm_establecer_traductor(i915_traductor_host_fn f);
#endif

// ----------------------------------------------------------------------------
// SCATTER-GATHER. `sg_alloc_table_from_pages` referencia únicamente las páginas
// que la tabla representa realmente (las necesarias para cubrir `size`) y
// `sg_free_table` libera exactamente esas mismas; las páginas no usadas no se
// tocan.
// ----------------------------------------------------------------------------
struct scatterlist {
    struct page *page;
    unsigned int offset;
    unsigned int length;
    dma_addr_t   dma_address;
    unsigned int dma_length;
};

struct sg_table {
    struct scatterlist *sgl;
    unsigned int        nents;
    unsigned int        orig_nents;
    struct page       **paginas;    /* referencias tomadas por la tabla */
    unsigned int        n_paginas;
};

#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg)     ((sg)->dma_length)
#define sg_page(sg)        ((sg)->page)

int  sg_alloc_table(struct sg_table *table, unsigned int nents, gfp_t gfp_mask);
void sg_free_table(struct sg_table *table);
int  sg_alloc_table_from_pages(struct sg_table *sgt, struct page **pages,
                               unsigned int n_pages, unsigned int offset,
                               unsigned long size, gfp_t gfp_mask);
struct page *sg_get_page(struct scatterlist *sg, unsigned int page_idx);

// ----------------------------------------------------------------------------
// DMA: la dirección DMA/IOVA NO se deduce de la física. El dispositivo aporta
// la traducción; sin ella el mapeo falla. La máscara se comprueba sobre TODO el
// rango [dirección, dirección+longitud), no sólo sobre la dirección inicial.
// ----------------------------------------------------------------------------
#define DMA_BIDIRECTIONAL 0
#define DMA_TO_DEVICE     1
#define DMA_FROM_DEVICE   2
#define DMA_NONE          3

typedef dma_addr_t (*i915_dma_traducir_fn)(void *ctx, phys_addr_t pa, size_t len);

struct dma_dispositivo {
    i915_dma_traducir_fn traducir;   /* obligatorio; NULL => -EINVAL */
    void                *ctx;
    dma_addr_t           mascara;    /* bits de dirección válidos (0 = todos) */
    unsigned             max_segmento; /* bytes por segmento (0 = sin límite) */
};

int  dma_map_sg(void *dev, struct scatterlist *sg, int nents, int dir);
void dma_unmap_sg(void *dev, struct scatterlist *sg, int nents, int dir);

#endif // LINUX_I915_MEMORIA_H
