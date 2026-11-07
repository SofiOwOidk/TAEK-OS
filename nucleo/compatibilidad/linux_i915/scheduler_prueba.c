#include "scheduler_prueba.h"
#include "tareas.h"
#include "abi_tls.h"
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#include "base/trabajos.h"
#include "base/tiempo.h"
#include "arquitectura/x86_64/tls.h"
#include "arquitectura/x86_64/apic.h"
#define INICIAL UINT64_C(0x54aee1100000cafe)
struct hijo {struct i915_smp_resultado *r;struct task_struct *t;unsigned cpu,indice;};
struct ensayo {struct i915_smp_resultado *r;struct hijo hijos[I915_MAX_CPU][2];};
static uint8_t ocupado;
static uintptr_t self(void){uintptr_t p;__asm__ volatile("movq %%fs:0,%0":"=r"(p)::"memory");return p;}
static void tarea(void *arg){
    struct hijo *h=arg;struct i915_smp_cpu_resultado *r=&h->r->cpu[h->cpu];
    uintptr_t base=(uintptr_t)h->t->tls_base;
    if(i915_tarea_actual()!=h->t || i915_scheduler_cpu_actual()!=h->cpu)r->fallos|=1;
    if(h->cpu && i915_tarea_crear(tarea,h,"rechazo-asignacion-AP",h->cpu)!=NULL)r->fallos|=256;
    if(self()!=base || taek_tls_base_actual()!=base)r->fallos|=2;
    if(i915_tls_verificar(INICIAL,0))r->iniciales++;else r->fallos|=4;
    uintptr_t local=(uintptr_t)&base,stack=(uintptr_t)h->t->stack_base;
    if(local<stack || local>=stack+h->t->stack_size)r->fallos|=8;
    uint64_t val=UINT64_C(0x5555000000000000)+((uint64_t)h->cpu<<32)+((uint64_t)h->indice<<24);
    uint8_t byte=(uint8_t)(h->cpu*16+h->indice+1);
    i915_tls_escribir(val,byte);
    for(unsigned i=0;i<128;i++){
        i915_tarea_ceder();
        if(i915_scheduler_cpu_actual()!=h->cpu || i915_tarea_actual()!=h->t)r->fallos|=1;
        if(self()!=base || taek_tls_base_actual()!=base)r->fallos|=2;
        if(!i915_tls_verificar(val,byte))r->fallos|=16;
        val++;byte++;i915_tls_escribir(val,byte);r->hijos++;
    }
}
static void cpu_trabajo(void *arg,unsigned region,unsigned cpu){
    (void)region;struct ensayo *e=arg;struct i915_smp_resultado *r=e->r;
    if(cpu>=r->cpus){__atomic_fetch_or(&r->fallos,1,__ATOMIC_RELAXED);return;}
    struct i915_smp_cpu_resultado *c=&r->cpu[cpu];
    c->fs_raiz=taek_tls_base_actual();
    if(i915_scheduler_cpu_actual()!=cpu || !i915_tarea_actual() ||
       i915_tarea_actual()->cpu_id!=cpu || self()!=c->fs_raiz)c->fallos|=32;
    __atomic_fetch_or(&r->entradas,1u<<cpu,__ATOMIC_RELEASE);
    uint64_t start=tiempo_obtener_milisegundos();
    while(__atomic_load_n(&r->entradas,__ATOMIC_ACQUIRE)!=((1u<<r->cpus)-1)){
        if(tiempo_obtener_milisegundos()-start>3000){__atomic_fetch_or(&r->fallos,2,__ATOMIC_RELAXED);break;}
        __asm__ volatile("pause");
    }
    if(__atomic_load_n(&r->fallos,__ATOMIC_RELAXED))return;
    for(unsigned i=0;i<128;i++){
        i915_tarea_ceder();c->ciclos++;
        if(taek_tls_base_actual()!=c->fs_raiz || self()!=c->fs_raiz)c->fallos|=64;
    }
    struct task_struct *a=e->hijos[cpu][0].t,*b=e->hijos[cpu][1].t;
    for(unsigned i=0;i<8 && (a->state!=TASK_DEAD || b->state!=TASK_DEAD);i++)i915_tarea_ceder();
    if(a->state!=TASK_DEAD || b->state!=TASK_DEAD || c->hijos!=256)c->fallos|=128;
}
int i915_scheduler_smp_probar(struct i915_smp_resultado *out){
    if(!out || !trabajos_es_coordinador() || trabajos_en_curso() || i915_scheduler_cpu_actual()!=0)return -1;
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return -1;
    *out=(struct i915_smp_resultado){.cpus=trabajos_cpu_activas()};
    if(!out->cpus || out->cpus>I915_MAX_CPU){__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;}
    if(i915_scheduler_cpu(I915_CPU_INVALIDA)!=-1 || i915_scheduler_cpu_actual()!=0 ||
       i915_scheduler_registrar_cpu(I915_MAX_CPU,0)!=-1 ||
       i915_scheduler_registrar_cpu(1,apic_obtener_estado()->id)!=-1 ||
       i915_scheduler_cpu_actual()!=0)out->fallos|=128;
    memoria_estadisticas_t antes,despues;memoria_obtener_estadisticas(&antes);
    struct ensayo e={.r=out};unsigned creadas=0;
    int previo_bsp=trabajos_bsp_calcula();
    for(unsigned cpu=0;cpu<out->cpus;cpu++)for(unsigned j=0;j<2;j++){
        struct hijo *h=&e.hijos[cpu][j];*h=(struct hijo){.r=out,.cpu=cpu,.indice=j};
        h->t=i915_tarea_crear(tarea,h,"scheduler-smp",cpu);
        if(h->t)creadas++;else out->fallos|=4;
    }
    if(creadas==out->cpus*2){
        if(!trabajos_configurar_distribucion(out->cpus,1))out->fallos|=8;
        else {
            if(!trabajos_ejecutar(cpu_trabajo,&e,out->cpus,NULL,NULL))out->fallos|=8;
            if(!trabajos_configurar_distribucion(out->cpus,previo_bsp))out->fallos|=16;
        }
    }
    for(unsigned cpu=0;cpu<out->cpus;cpu++)for(unsigned j=0;j<2;j++)if(e.hijos[cpu][j].t)
        i915_tarea_destruir(e.hijos[cpu][j].t);
    for(unsigned cpu=0;cpu<out->cpus;cpu++){
        if(out->cpu[cpu].ciclos!=128 || out->cpu[cpu].hijos!=256 ||
           out->cpu[cpu].iniciales!=2 || !out->cpu[cpu].fs_raiz)out->cpu[cpu].fallos|=128;
        if(out->cpu[cpu].fallos)out->fallos|=32;
    }
    memoria_obtener_estadisticas(&despues);
    if(antes.heap_bytes_en_uso==despues.heap_bytes_en_uso &&
       antes.heap_bloques_activos==despues.heap_bloques_activos && despues.canarios_intactos)out->heap_limpio=1;
    else out->fallos|=64;
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);return out->fallos?-3:0;
}
