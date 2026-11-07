#include "abi_contexto.h"
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

void *i915_abi_padre_rsp,*i915_abi_hijo_rsp;
struct i915_contexto_traza i915_abi_padre_traza,i915_abi_hijo_traza;
uint64_t i915_abi_padre_entrada,i915_abi_padre_llamada,i915_abi_hijo_entrada;
uint32_t i915_abi_hijo_fallos;
static uint8_t ocupado;
extern uint32_t i915_abi_contexto_una_vez(void);
extern void i915_abi_contexto_hijo(void);
extern void i915_abi_entrada_fn(void *);

int i915_abi_contexto_probar(struct i915_abi_resultado *out) {
    if(!out || i915_scheduler_cpu_actual()!=0)return -1;
#ifndef TAEK_HOST_TEST
    if(!trabajos_es_coordinador() || trabajos_en_curso())return -1;
#endif
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return -1;
    *out=(struct i915_abi_resultado){0};
    void *stack=ALLOC(TASK_STACK_SIZE_BYTES);
    if(!stack){__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;}
    struct i915_contexto_marco *f=i915_contexto_preparar(stack,TASK_STACK_SIZE_BYTES,
                                                       i915_abi_contexto_hijo,0xB220000000000004);
    if(!f){FREE(stack);__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;}
    f->rbp=0xB220000000000001;f->rbx=0xB220000000000002;
    f->r13=0xB220000000000008;f->r14=0xB220000000000010;f->r15=0xB220000000000020;
    f->traza=&i915_abi_hijo_traza;
    i915_abi_padre_rsp=0;i915_abi_hijo_rsp=f;
    i915_abi_padre_traza=(struct i915_contexto_traza){0};
    i915_abi_hijo_traza=(struct i915_contexto_traza){0};
    i915_abi_hijo_fallos=0;i915_abi_hijo_entrada=0;
    for(unsigned i=0;i<1024;i++){
        out->fallos|=i915_abi_contexto_una_vez();
        out->ciclos++;
    }
    out->fallos|=i915_abi_hijo_fallos;
    out->padre_entrada=i915_abi_padre_entrada;
    out->padre_llamada=i915_abi_padre_llamada;
    out->hijo_entrada=i915_abi_hijo_entrada;
    out->padre=i915_abi_padre_traza;out->hijo=i915_abi_hijo_traza;
    if((out->padre_entrada&15)!=8 || (out->padre_llamada&15)!=0 ||
       (out->hijo_entrada&15)!=0 || (out->padre.entrada_rsp&15)!=8 ||
       (out->padre.salida_rsp&15)!=8 || (out->hijo.entrada_rsp&15)!=8 ||
       (out->hijo.salida_rsp&15)!=8)out->fallos|=128;
    /* Direct probe is suspended only in our ASM target, with no scheduler,
     * IRQ, workqueue or external reference. Discard it after return to parent. */
    i915_abi_hijo_rsp=0;FREE(stack);
    if(!out->fallos){
        /* Normal production task, exact ASM entry, real trampoline and exit. */
        i915_tareas_iniciar();
        struct task_struct *t=i915_tarea_crear(i915_abi_entrada_fn,&out->funcion_entrada,"abi-entrada",0);
        if(!t)out->fallos|=256;
        else {
            for(unsigned i=0;i<16 && t->state!=TASK_DEAD;i++)i915_tarea_ceder();
            if(t->state!=TASK_DEAD || (out->funcion_entrada&15)!=8)out->fallos|=256;
            i915_tarea_destruir(t);
        }
    }
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);
    return out->fallos?-3:0;
}
