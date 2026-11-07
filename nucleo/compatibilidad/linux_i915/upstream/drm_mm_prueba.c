/* Compilar contra cabeceras Linux fijadas, nunca contra tipos homonimos locales. */
#include <drm/drm_mm.h>
#include "../upstream_mm.h"
static struct drm_mm gestores[2];
static struct drm_mm_node nodos[8];
static void verificar(struct taek_upstream_mm_resultado *r,int ok,unsigned bit){
    r->checks++;if(!ok)r->fallos|=bit;
}
int taek_upstream_drm_mm_probar(struct taek_upstream_mm_resultado *r){
    if(!r)return -EINVAL;
    *r=(struct taek_upstream_mm_resultado){0};
    memset(gestores,0,sizeof(gestores));memset(nodos,0,sizeof(nodos));
    drm_mm_init(&gestores[0],0,0x100000);
    drm_mm_init(&gestores[1],0x100000000ULL,0x10000);
    nodos[0].start=0x20000;nodos[0].size=0x20000;
    verificar(r,drm_mm_reserve_node(&gestores[0],&nodos[0])==0,1);
    nodos[1].start=0x21000;nodos[1].size=0x1000;
    verificar(r,drm_mm_reserve_node(&gestores[0],&nodos[1])==-ENOSPC && !drm_mm_node_allocated(&nodos[1]),2);
    verificar(r,drm_mm_insert_node_in_range(&gestores[0],&nodos[2],0x4000,0x1000,0,0,0x100000,DRM_MM_INSERT_LOW)==0,4);
    verificar(r,nodos[2].start==0 && nodos[2].size==0x4000,8);r->bajo=nodos[2].start;
    verificar(r,drm_mm_insert_node_in_range(&gestores[0],&nodos[3],0x8000,0x4000,0,0,0x100000,DRM_MM_INSERT_HIGH)==0,16);
    verificar(r,nodos[3].start==0xf8000 && !(nodos[3].start%0x4000),32);r->alto=nodos[3].start;
    verificar(r,drm_mm_insert_node_in_range(&gestores[0],&nodos[4],0x2000,0x3000,0,0x40000,0x48000,DRM_MM_INSERT_LOW)==0,64);
    verificar(r,nodos[4].start==0x42000 && !(nodos[4].start%0x3000),128);
    verificar(r,drm_mm_insert_node_in_range(&gestores[0],&nodos[5],0x200000,0x1000,0,0,0x100000,DRM_MM_INSERT_LOW)==-ENOSPC && !drm_mm_node_allocated(&nodos[5]),256);
    verificar(r,drm_mm_insert_node_in_range(&gestores[0],&nodos[6],0x1000,0x1000,0,0x20000,0x40000,DRM_MM_INSERT_LOW)==-ENOSPC && !drm_mm_node_allocated(&nodos[6]),512);
    verificar(r,drm_mm_insert_node_in_range(&gestores[1],&nodos[7],0x1000,0x1000,0,0x100000000ULL,0x100010000ULL,DRM_MM_INSERT_LOW)==0 && nodos[7].start==0x100000000ULL,1024);
    verificar(r,__drm_mm_interval_first(&gestores[0],0x20000,0x20fff)==&nodos[0],2048);
    if(drm_mm_node_allocated(&nodos[0]))drm_mm_remove_node(&nodos[0]);
    /* El helper interno devuelve el sentinel, no NULL. Comprobar vacio
     * mediante el iterador publico con limites dentro del gestor. */
    unsigned coincidencias=0;struct drm_mm_node *coincide;
    drm_mm_for_each_node_in_range(coincide,&gestores[0],0x20000,0x21000)coincidencias++;
    verificar(r,coincidencias==0,4096);
    nodos[0].start=0x20000;nodos[0].size=0x20000;
    verificar(r,drm_mm_reserve_node(&gestores[0],&nodos[0])==0,8192);
    unsigned vistos=0;u64 anterior=0;struct drm_mm_node *n;
    drm_mm_for_each_node(n,&gestores[0]){
        verificar(r,n->start>=anterior && n->start+n->size<=0x100000,16384);
        anterior=n->start+n->size;vistos++;
    }
    verificar(r,vistos==4,32768);
    /* WARN original debe informar y conservar los nodos, sin trap Linux. */
    drm_mm_takedown(&gestores[0]);
    verificar(r,!drm_mm_clean(&gestores[0]) && drm_mm_node_allocated(&nodos[0]) &&
                __drm_mm_interval_first(&gestores[0],0x20000,0x20fff)==&nodos[0],131072);
    for(unsigned i=0;i<8;i++)if(drm_mm_node_allocated(&nodos[i]))drm_mm_remove_node(&nodos[i]);
    verificar(r,drm_mm_clean(&gestores[0]) && drm_mm_clean(&gestores[1]),65536);
    r->limpio=drm_mm_clean(&gestores[0]) && drm_mm_clean(&gestores[1]);
    drm_mm_takedown(&gestores[0]);drm_mm_takedown(&gestores[1]);
    return r->fallos?-EIO:0;
}
