#include "abi_irq.h"
#include "tareas.h"
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#include "base/trabajos.h"
#include "base/tiempo.h"
#include "arquitectura/x86_64/apic.h"
#include "arquitectura/x86_64/tls.h"
#define VECTOR 82
struct hijo {struct i915_irq_resultado *r;unsigned cpu,indice;};
struct ensayo {struct i915_irq_resultado *r;struct hijo args[I915_MAX_CPU][2];struct task_struct *t[I915_MAX_CPU][2];};
static struct ensayo *activo;
static uint8_t ocupado;
static uint64_t flags(void){uint64_t n;__asm__ volatile("pushfq; popq %0":"=r"(n)::"memory");return n;}
static void irq(int enable){if(enable)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static void fail(struct i915_irq_cpu *c,unsigned bit){__atomic_fetch_or(&c->fallos,bit,__ATOMIC_RELAXED);}
static void interrupcion(struct marco_interrupcion *m){
    struct ensayo *e=__atomic_load_n(&activo,__ATOMIC_ACQUIRE);if(!e)return;
    unsigned cpu=i915_scheduler_cpu_actual();
    if(cpu>=e->r->cpus){__atomic_fetch_or(&e->r->fallos,128,__ATOMIC_RELAXED);return;}
    struct i915_irq_cpu *c=&e->r->cpu[cpu];struct task_struct *t=i915_tarea_actual();
    uintptr_t self;__asm__ volatile("movq %%fs:0,%0":"=r"(self)::"memory");
    if(!(m->rflags&512) || !t || t->cpu_id!=cpu || (uintptr_t)t->tls_base!=self || self!=taek_tls_base_actual())fail(c,32);
    __atomic_fetch_add(&c->recibidos,1,__ATOMIC_RELEASE);
}
static void tarea(void *arg){
    struct hijo *h=arg;struct i915_irq_cpu *c=&h->r->cpu[h->cpu];
    if(flags()&512)c->iniciales++;else fail(c,8);
    for(unsigned i=0;i<64;i++){
        if(h->indice){
            /* Medir antes del STI explicito de este hijo. Una restauracion
             * omitida puede entregar todo mas tarde y ocultar ese defecto. */
            unsigned esperado=c->emitidos;uint64_t start=tiempo_obtener_milisegundos();
            while(__atomic_load_n(&c->recibidos,__ATOMIC_ACQUIRE)<esperado &&
                  tiempo_obtener_milisegundos()-start<2)__asm__ volatile("pause");
            if(esperado==i+1 && __atomic_load_n(&c->recibidos,__ATOMIC_ACQUIRE)==esperado)c->en_contexto++;
            else fail(c,16);
        }
        irq(h->indice!=0);
        if(!h->indice){c->emitidos++;apic_enviar_self_ipi(VECTOR);}
        i915_tarea_ceder();
        if(!!(flags()&512)!=(h->indice!=0))fail(c,h->indice?2:1);
    }
}
static void cpu_trabajo(void *arg,unsigned region,unsigned cpu){
    (void)region;struct ensayo *e=arg;
    if(cpu>=e->r->cpus){__atomic_fetch_or(&e->r->fallos,1,__ATOMIC_RELAXED);return;}
    struct i915_irq_cpu *c=&e->r->cpu[cpu];uint64_t original=flags();
    __atomic_fetch_or(&e->r->entradas,1u<<cpu,__ATOMIC_RELEASE);
    uint64_t start=tiempo_obtener_milisegundos();
    while(__atomic_load_n(&e->r->entradas,__ATOMIC_ACQUIRE)!=((1u<<e->r->cpus)-1)){
        if(tiempo_obtener_milisegundos()-start>3000){__atomic_fetch_or(&e->r->fallos,2,__ATOMIC_RELAXED);break;}
        __asm__ volatile("pause");
    }
    if(!__atomic_load_n(&e->r->fallos,__ATOMIC_RELAXED)){
        for(unsigned i=0;i<64;i++){
            irq(i&1);i915_tarea_ceder();c->ciclos++;
            if(!!(flags()&512)!=(i&1))fail(c,4);
        }
        struct task_struct *a=e->t[cpu][0],*b=e->t[cpu][1];
        for(unsigned i=0;i<8 && (a->state!=TASK_DEAD || b->state!=TASK_DEAD);i++)i915_tarea_ceder();
        if(a->state!=TASK_DEAD || b->state!=TASK_DEAD)fail(c,64);
    }
    /* Recuperacion independiente del primitivo: el mutante deja IF=0.
     * Drenar la IPI pendiente antes de soltar el handler o volver al pool. */
    irq(1);start=tiempo_obtener_milisegundos();
    while(__atomic_load_n(&c->recibidos,__ATOMIC_ACQUIRE)<c->emitidos && tiempo_obtener_milisegundos()-start<2)
        __asm__ volatile("pause");
    if(c->recibidos!=c->emitidos || c->emitidos!=64 || c->en_contexto!=64)fail(c,16);
    irq(!!(original&512));
}
int i915_irq_contexto_probar(struct i915_irq_resultado *out){
    if(!out)return -1;
    uint64_t original=flags();
    *out=(struct i915_irq_resultado){.if_antes=!!(original&512),.if_despues=!!(original&512)};
    if(!trabajos_es_coordinador())out->admision|=1;
    if(trabajos_en_curso())out->admision|=2;
    if(i915_scheduler_cpu_actual()!=0)out->admision|=4;
    if(!apic_obtener_estado()->activo)out->admision|=8;
    if(out->admision)return -1;
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE)){out->admision=16;return -1;}
    /* El BSP puede entrar con IF=0. El ensayo conserva ese estado;
     * las tareas nuevas, por contrato, se inician con IF=1. */
    out->cpus=trabajos_cpu_activas();
    if(!out->cpus || out->cpus>I915_MAX_CPU || idt_reservar_manejador(VECTOR,interrupcion)){
        out->fallos=1;__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;
    }
    memoria_estadisticas_t antes,despues;memoria_obtener_estadisticas(&antes);
    struct ensayo e={.r=out};unsigned creadas=0;int previo=trabajos_bsp_calcula();
    __atomic_store_n(&activo,&e,__ATOMIC_RELEASE);
    for(unsigned cpu=0;cpu<out->cpus;cpu++)for(unsigned j=0;j<2;j++){
        e.args[cpu][j]=(struct hijo){.r=out,.cpu=cpu,.indice=j};
        e.t[cpu][j]=i915_tarea_crear(tarea,&e.args[cpu][j],"irq-contexto",cpu);
        if(e.t[cpu][j])creadas++;else out->fallos|=4;
    }
    if(creadas==2*out->cpus){
        if(!trabajos_configurar_distribucion(out->cpus,1))out->fallos|=8;
        else {
            if(!trabajos_ejecutar(cpu_trabajo,&e,out->cpus,NULL,NULL))out->fallos|=8;
            if(!trabajos_configurar_distribucion(out->cpus,previo))out->fallos|=8;
        }
    }
    for(unsigned cpu=0;cpu<out->cpus;cpu++){
        if(out->cpu[cpu].ciclos!=64 || out->cpu[cpu].iniciales!=2)fail(&out->cpu[cpu],64);
        if(out->cpu[cpu].fallos)out->fallos|=16;
        for(unsigned j=0;j<2;j++)if(e.t[cpu][j])i915_tarea_destruir(e.t[cpu][j]);
    }
    __atomic_store_n(&activo,NULL,__ATOMIC_RELEASE);
    if(idt_liberar_manejador(VECTOR,interrupcion))out->fallos|=32;
    memoria_obtener_estadisticas(&despues);
    if(antes.heap_bytes_en_uso==despues.heap_bytes_en_uso && antes.heap_bloques_activos==despues.heap_bloques_activos &&
       despues.canarios_intactos)out->heap_limpio=1;else out->fallos|=64;
    out->if_despues=!!(flags()&512);
    if(out->if_despues!=out->if_antes)out->fallos|=256;
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);return out->fallos?-3:0;
}
