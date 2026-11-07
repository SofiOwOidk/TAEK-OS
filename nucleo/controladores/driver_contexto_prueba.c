#include "driver_contexto_prueba.h"
#include "../base/trabajos.h"
#include "../base/tiempo.h"
#include "../base/memoria.h"
#include "../arquitectura/x86_64/idt.h"
#include "../arquitectura/x86_64/apic.h"
#include "../compatibilidad/linux_i915/cpu_runtime.h"
/* Exclusivo; no sustituye handlers existentes. EOI lo realiza el dispatcher. */
#define CONTEXTO_VECTOR 83u
#define IF_BIT UINT64_C(512)
static struct {
    driver_contexto_lectura_fn lectura;
    unsigned cpus,entradas,fallos,recibidas,emitidas,reservado,midiendo;
    int lote[TRABAJOS_MAX_CPU],irq;
} ensayo;
static uint8_t ocupado;
static uint64_t flags(void){uint64_t f;__asm__ volatile("pushfq; popq %0":"=r"(f)::"memory");return f;}
static void irq_restaurar(uint64_t f){if(f&IF_BIT)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static uint64_t ciclos(void){unsigned a,d;__asm__ volatile("lfence; rdtsc":"=a"(a),"=d"(d)::"memory");return ((uint64_t)d<<32)|a;}
static int agotado(uint64_t inicio,unsigned cpu,unsigned ms){
    uint64_t ticks=trabajos_ticks_por_ms(cpu);
    return !ticks || ciclos()-inicio>ticks*ms;
}
static void fallo(unsigned bit){__atomic_fetch_or(&ensayo.fallos,bit,__ATOMIC_RELAXED);}
static void interrupcion(struct marco_interrupcion *m){
    if(__atomic_load_n(&ensayo.midiendo,__ATOMIC_ACQUIRE)){
        /* Fuera del lote: cero debe venir de IRQ y no de trabajos_en_curso. */
        if(m->num_interrupcion!=CONTEXTO_VECTOR || !(m->rflags&IF_BIT) ||
           !trabajos_es_coordinador() || i915_scheduler_cpu_actual()!=0 ||
           trabajos_en_curso() || !idt_en_interrupcion())fallo(16);
        ensayo.irq=ensayo.lectura();if(ensayo.irq!=0)fallo(32);
    }
    __atomic_fetch_add(&ensayo.recibidas,1,__ATOMIC_RELEASE);
}
static void trabajo(void *arg,unsigned region,unsigned cpu){
    (void)arg;(void)region;
    if(cpu>=ensayo.cpus || cpu>=TRABAJOS_MAX_CPU){fallo(1);return;}
    ensayo.lote[cpu]=ensayo.lectura();
    if(ensayo.lote[cpu]!=0 || !trabajos_en_curso() || idt_en_interrupcion() ||
       i915_scheduler_cpu_actual()!=cpu || !!trabajos_es_coordinador()!=(cpu==0))fallo(2);
    __atomic_fetch_or(&ensayo.entradas,1u<<cpu,__ATOMIC_RELEASE);
    uint64_t inicio=ciclos();unsigned todos=(1u<<ensayo.cpus)-1;
    /* Cada CPU retiene su primera region hasta que los otros entran. Evita
     * que un ejecutor rapido consuma todo el lote y simule evidencia AP. */
    while(__atomic_load_n(&ensayo.entradas,__ATOMIC_ACQUIRE)!=todos){
        if(agotado(inicio,cpu,3000)){fallo(4);break;}
        __asm__ volatile("pause");
    }
}
int driver_contexto_probar(driver_contexto_lectura_fn lectura,struct driver_contexto_resultado *out){
    if(!out)return -1;
    uint64_t original=flags();
    *out=(struct driver_contexto_resultado){.if_antes=!!(original&IF_BIT),.if_despues=!!(original&IF_BIT),.irq=-1};
    if(!lectura || !trabajos_es_coordinador() || trabajos_en_curso() || idt_en_interrupcion() ||
       i915_scheduler_cpu_actual()!=0 || !apic_obtener_estado()->activo || !trabajos_ticks_por_ms(0))out->admision|=1;
    unsigned cpus=trabajos_cpu_activas();out->cpus=cpus;
    if(cpus<2 || cpus>TRABAJOS_MAX_CPU)out->admision|=2;
    if(out->admision)return -1;
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE)){out->admision=4;return -1;}
    __asm__ volatile("cli":::"memory");
    /* Una entrega tardia de un ensayo fallido solo actualiza estado residente.
     * No reemplazar contador/callback ni soltar la reserva antes de drenarla. */
    if(ensayo.reservado){
        __asm__ volatile("sti":::"memory");uint64_t inicio=ciclos();
        while(__atomic_load_n(&ensayo.recibidas,__ATOMIC_ACQUIRE)<ensayo.emitidas && !agotado(inicio,0,100))__asm__ volatile("pause");
        __asm__ volatile("cli":::"memory");
        if(ensayo.recibidas!=ensayo.emitidas){out->fallos=64;out->vector_retenido=1;goto fin;}
        if(idt_liberar_manejador(CONTEXTO_VECTOR,interrupcion)){out->fallos=128;out->vector_retenido=1;goto fin;}
        ensayo.reservado=0;
    }
    if(idt_reservar_manejador(CONTEXTO_VECTOR,interrupcion)){out->admision=8;goto fin;}
    ensayo.reservado=1;ensayo.lectura=lectura;ensayo.cpus=cpus;
    ensayo.entradas=ensayo.fallos=ensayo.recibidas=ensayo.emitidas=ensayo.midiendo=0;ensayo.irq=-1;
    for(unsigned i=0;i<TRABAJOS_MAX_CPU;i++)ensayo.lote[i]=-1;
    memoria_estadisticas_t antes,despues;memoria_obtener_estadisticas(&antes);
    out->antes=lectura();if(out->antes!=1)fallo(256);
    int previo=trabajos_bsp_calcula();
    if(!trabajos_configurar_distribucion(cpus,1))fallo(512);
    else {
        __asm__ volatile("sti":::"memory");
        if(!trabajos_ejecutar(trabajo,NULL,cpus,NULL,NULL))fallo(512);
        __asm__ volatile("cli":::"memory");
        if(!trabajos_configurar_distribucion(cpus,previo))fallo(512);
    }
    if(ensayo.entradas!=((1u<<cpus)-1))fallo(8);
    /* g.en_callback continua en uno durante esta IPI BSP real. */
    __atomic_store_n(&ensayo.midiendo,1,__ATOMIC_RELEASE);ensayo.emitidas=1;
    apic_enviar_self_ipi(CONTEXTO_VECTOR);
    __asm__ volatile("sti":::"memory");uint64_t inicio=ciclos();
    while(!__atomic_load_n(&ensayo.recibidas,__ATOMIC_ACQUIRE) && !agotado(inicio,0,100))__asm__ volatile("pause");
    __asm__ volatile("cli":::"memory");
    __atomic_store_n(&ensayo.midiendo,0,__ATOMIC_RELEASE);
    if(ensayo.recibidas!=ensayo.emitidas){fallo(64);out->vector_retenido=1;}
    else if(idt_liberar_manejador(CONTEXTO_VECTOR,interrupcion)){fallo(128);out->vector_retenido=1;}
    else ensayo.reservado=0;
    out->despues=lectura();if(out->despues!=1)fallo(1024);
    memoria_obtener_estadisticas(&despues);
    if(antes.heap_bytes_en_uso==despues.heap_bytes_en_uso && antes.heap_bloques_activos==despues.heap_bloques_activos && despues.canarios_intactos)out->heap_limpio=1;
    else fallo(2048);
    out->entradas=ensayo.entradas;out->fallos=ensayo.fallos;out->irq=ensayo.irq;out->irq_recibidas=ensayo.recibidas;
    for(unsigned i=0;i<TRABAJOS_MAX_CPU;i++)out->lote[i]=ensayo.lote[i];
fin:
    irq_restaurar(original);out->if_despues=!!(flags()&IF_BIT);
    if(out->if_antes!=out->if_despues)out->fallos|=4096;
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);return out->admision?-1:out->fallos?-3:0;
}
