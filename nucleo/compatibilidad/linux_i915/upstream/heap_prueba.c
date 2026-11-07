/* Cliente de la ABI slab real. Perfil BSP limitado: no reclamación ni SMP. */
#include <linux/slab.h>
#include "../upstream_params.h"
static void comprobar(struct taek_upstream_heap_contrato *r,int ok,unsigned bit){r->checks++;if(!ok)r->fallos|=bit;}
static void *pedir(size_t n,gfp_t g,int node){return __kmalloc_node_track_caller(n,g,node,0);}
int taek_upstream_heap_probar(struct taek_upstream_heap_contrato *r){
    if(!r)return -EINVAL;
    *r=(struct taek_upstream_heap_contrato){0};
    struct taek_upstream_heap_estado baseline,st;taek_upstream_heap_estado(&baseline);
    if(baseline.activos || baseline.bytes)return -EBUSY;
    void *zero=pedir(0,GFP_KERNEL,NUMA_NO_NODE);
    comprobar(r,zero==ZERO_SIZE_PTR,1);kfree(zero);kfree(NULL);
    const size_t sizes[]={1,16,32,64,192,4096};
    for(unsigned i=0;i<ARRAY_SIZE(sizes);i++){
        size_t n=sizes[i],align=n&-n;if(align<16)align=16;
        unsigned char *p=pedir(n,GFP_KERNEL|__GFP_ZERO,NUMA_NO_NODE);
        int ok=p && !((unsigned long)p&(align-1));
        if(p){for(size_t j=0;j<n;j++)if(p[j])ok=0;memset(p,0xa5,n);}
        comprobar(r,ok,2);kfree(p);
    }
    taek_upstream_heap_estado(&st);comprobar(r,!st.activos && !st.bytes && st.errores==baseline.errores,4);
    void *slots[32];unsigned count=0;
    for(unsigned i=0;i<32;i++){slots[i]=pedir(13,GFP_ATOMIC,0);if(slots[i])count++;}
    void *extra=pedir(13,GFP_ATOMIC,0);
    comprobar(r,count==32 && !extra,8);kfree(extra);
    for(unsigned i=0;i<32;i++)kfree(slots[i]);
    /* Tamaños fuera del presupuesto y overflow no llegan al backend. */
    void *huge=pedir(~(size_t)0,GFP_KERNEL,0);comprobar(r,!huge,16);kfree(huge);
    huge=pedir(4ul*1024*1024+1,GFP_KERNEL,0);comprobar(r,!huge,16);kfree(huge);
    void *unsupported=pedir(17,GFP_KERNEL|__GFP_DMA,0);comprobar(r,!unsupported,32);kfree(unsupported);
    unsupported=pedir(17,GFP_KERNEL,2);comprobar(r,!unsupported,32);kfree(unsupported);
    unsigned char *p=pedir(17,GFP_ATOMIC,0);
    if(p)kfree(p+1);
    taek_upstream_heap_estado(&st);comprobar(r,p && st.activos==1 && st.errores==baseline.errores+3,64);
    /* Una configuración inválida conserva los callbacks y recursos actuales. */
    comprobar(r,taek_upstream_heap_configurar(NULL,NULL)==-EBUSY,128);
    kfree(p);if(p)kfree(p);
    comprobar(r,taek_upstream_heap_configurar(NULL,NULL)==-EINVAL,128);
    p=pedir(17,GFP_ATOMIC,0);comprobar(r,p!=NULL,256);kfree(p);
    taek_upstream_heap_estado(&st);
    r->errores_esperados=st.errores-baseline.errores;r->fallos_esperados=st.fallos-baseline.fallos;
    r->limpio=!st.activos && !st.bytes;
    comprobar(r,r->limpio && r->errores_esperados==4 && r->fallos_esperados==5,512);
    return r->fallos?-EIO:0;
}
