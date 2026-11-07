#include "abi_simd.h"
#ifdef TAEK_HOST_TEST
#include <stdlib.h>
#define ALLOC(n) malloc(n)
#define FREE(p) free(p)
#else
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#include "base/trabajos.h"
#define ALLOC(n) asignar_memoria(n)
#define FREE(p) liberar_memoria(p)
#endif
struct fx_imagen {uint8_t datos[512];} __attribute__((aligned(16)));
struct fx_imagen i915_simd_patrones[2],i915_simd_actual[2];
static struct fx_imagen inicial;
void *i915_simd_padre_rsp,*i915_simd_hijo_rsp;
static uint8_t ocupado;
extern void i915_simd_una_vez(void);
extern void i915_simd_hijo(void);
extern void i915_simd_entrada_fn(void *);

static void cero(struct fx_imagen *p){for(unsigned i=0;i<512;i++)p->datos[i]=0;}
static void put16(struct fx_imagen *p,unsigned off,uint16_t n){p->datos[off]=n;p->datos[off+1]=n>>8;}
static void put64(struct fx_imagen *p,unsigned off,uint64_t n){for(unsigned i=0;i<8;i++)p->datos[off+i]=(uint8_t)(n>>(8*i));}
static int igual(const struct fx_imagen *a,const struct fx_imagen *b,unsigned off,unsigned len){
    for(unsigned i=off;i<off+len;i++)if(a->datos[i]!=b->datos[i])return 0;return 1;
}
static void patron(unsigned n){
    struct fx_imagen *p=&i915_simd_patrones[n];cero(p);
    put16(p,0,n?0x0b7f:0x077f); /* round up/down, all exceptions masked */
    put16(p,24,n?0x5f80:0x3f80);
    p->datos[4]=n?3:1; /* parent ST0; child ST0/ST1; TOP=0 */
    put64(p,32,UINT64_C(0x8000000000000000));put16(p,40,n?0xc000:0x3fff);
    if(n){put64(p,48,UINT64_C(0x8000000000000000));put16(p,56,0x3fff);}
    for(unsigned reg=0;reg<16;reg++){
        put64(p,160+16*reg,UINT64_C(0xa110000000000000)+(uint64_t)n*UINT64_C(0x1110000000000000)+reg);
        put64(p,168+16*reg,UINT64_C(0x1234000000000000)+(uint64_t)n*UINT64_C(0x1100000000000000)+reg);
    }
}
static uint32_t comparar(unsigned n){
    const struct fx_imagen *a=&i915_simd_actual[n],*p=&i915_simd_patrones[n];uint32_t mask=0;
    for(unsigned i=0;i<16;i++)if(!igual(a,p,160+i*16,16))mask|=1u<<i;
    if(!igual(a,p,0,5) || !igual(a,p,32,10) || (n && !igual(a,p,48,10)))mask|=1u<<16;
    if(!igual(a,p,24,4))mask|=1u<<17;
    return mask;
}
int i915_simd_contexto_probar(struct i915_simd_resultado *out){
    if(!out || i915_scheduler_cpu_actual()!=0)return -1;
#ifndef TAEK_HOST_TEST
    if(!trabajos_es_coordinador() || trabajos_en_curso())return -1;
#endif
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return -1;
    *out=(struct i915_simd_resultado){.sse2=i915_contexto_sse2_listo()};
    if(!out->sse2){__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;}
    patron(0);patron(1);cero(&i915_simd_actual[0]);cero(&i915_simd_actual[1]);
    void *stack=ALLOC(TASK_STACK_SIZE_BYTES);
    if(!stack){__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;}
    struct i915_contexto_marco *f=i915_contexto_preparar(stack,TASK_STACK_SIZE_BYTES,i915_simd_hijo,0);
    if(!f){FREE(stack);__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;}
    for(unsigned i=0;i<512;i++)f->fpu[i]=i915_simd_patrones[1].datos[i];
    i915_simd_padre_rsp=NULL;i915_simd_hijo_rsp=f;
    for(unsigned i=0;i<1024;i++){
        i915_simd_una_vez();out->ciclos++;
        out->fallos|=comparar(0)|comparar(1);
    }
    i915_simd_padre_rsp=NULL;i915_simd_hijo_rsp=NULL;FREE(stack);
    if(!out->fallos){
        cero(&inicial);i915_tareas_iniciar();
        struct task_struct *t=i915_tarea_crear(i915_simd_entrada_fn,&inicial,"simd-inicial",0);
        if(!t)out->fallos|=1u<<18;
        else {
            for(unsigned i=0;i<16 && t->state!=TASK_DEAD;i++)i915_tarea_ceder();
            struct fx_imagen expected;cero(&expected);put16(&expected,0,0x37f);put16(&expected,24,0x1f80);
            if(t->state!=TASK_DEAD || !igual(&inicial,&expected,0,5) ||
               !igual(&inicial,&expected,24,4) || !igual(&inicial,&expected,160,256))out->fallos|=1u<<18;
            else out->inicial_limpio=1;
            i915_tarea_destruir(t);
        }
    }
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);
    return out->fallos?-3:0;
}
