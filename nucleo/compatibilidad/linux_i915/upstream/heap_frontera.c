/* ABI Linux real; perfil serial BSP del cargador RAM. No header GFP local.
 * Respaldo real mediante callbacks del heap nativo; presupuesto/propiedad
 * acotados. Fuera de NUMA0/NO_NODE y GFP admisibles se devuelve fallo visible. */
#include <linux/slab.h>
#include "../upstream_params.h"
#define MAX_HEAP 32
static struct {void *ptr,*raw;unsigned long bytes;unsigned long caller;} owned[MAX_HEAP];
static taek_heap_alloc_fn reservar;
static taek_heap_free_fn liberar;
static struct taek_upstream_heap_estado stats;
static unsigned fallar_en,llamadas;
int taek_upstream_heap_configurar(taek_heap_alloc_fn alloc,taek_heap_free_fn free){
    if(stats.activos)return -EBUSY;
    if(!alloc || !free)return -EINVAL;
    reservar=alloc;liberar=free;stats=(struct taek_upstream_heap_estado){0};
    fallar_en=llamadas=0;return 0;
}
void taek_upstream_heap_estado(struct taek_upstream_heap_estado *out){if(out)*out=stats;}
void taek_upstream_heap_fallar_en(unsigned llamada){fallar_en=llamada;llamadas=0;}
void *__kmalloc_node_track_caller(size_t size,gfp_t flags,int node,unsigned long caller){
    if(!size)return ZERO_SIZE_PTR;
    llamadas++;
    if(!reservar || !liberar || (node!=NUMA_NO_NODE && node!=0) ||
       ((flags&~__GFP_ZERO)!=GFP_ATOMIC && (flags&~__GFP_ZERO)!=GFP_KERNEL)){
        stats.errores++;stats.fallos++;return NULL;
    }
    unsigned long alignment=size&-size;
    if(alignment<16)alignment=16;
    const unsigned long budget=4ul*1024*1024;
    if(size>budget || alignment-1>budget-size){stats.fallos++;return NULL;}
    unsigned long backing=size+alignment-1;
    if((fallar_en && llamadas==fallar_en) || backing>budget-stats.bytes){stats.fallos++;return NULL;}
    unsigned slot=0;while(slot<MAX_HEAP && owned[slot].ptr)slot++;
    if(slot==MAX_HEAP){stats.fallos++;return NULL;}
    void *raw=reservar(backing);
    if(!raw){stats.fallos++;return NULL;}
    /* Garantía kmalloc: mínimo de arquitectura, potencia de dos y divisor
     * de potencia de dos del tamaño. Retener el backing para el free real. */
    void *ptr=(void *)(((unsigned long)raw+alignment-1)&~(alignment-1));
    owned[slot].ptr=ptr;owned[slot].raw=raw;owned[slot].bytes=backing;owned[slot].caller=caller;
    stats.bytes+=backing;stats.activos++;stats.asignaciones++;
    if(stats.activos>stats.pico)stats.pico=stats.activos;
    if(flags&__GFP_ZERO)memset(ptr,0,size);
    return ptr;
}
void kfree(const void *ptr){
    if(ZERO_OR_NULL_PTR(ptr))return;
    for(unsigned i=0;i<MAX_HEAP;i++)if(owned[i].ptr==ptr){
        if(liberar(owned[i].raw)){stats.errores++;return;}
        stats.bytes-=owned[i].bytes;stats.activos--;
        owned[i].ptr=owned[i].raw=NULL;owned[i].bytes=owned[i].caller=0;return;
    }
    /* Error de propietario/interior/doble free: no tocar memoria ajena. */
    stats.errores++;
}
