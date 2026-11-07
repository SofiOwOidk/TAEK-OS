#include "abi_tls.h"
#include "tareas.h"
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#include "base/trabajos.h"
#include "arquitectura/x86_64/tls.h"

/* Objetos TLS reales del compilador/enlazador, no campos de task_struct.
 * La reserva de .tbss incluye alineacion y bytes cero. */
#define INICIAL UINT64_C(0x54aee1100000cafe)
static _Thread_local volatile uint64_t dato=INICIAL;
static _Thread_local volatile uint8_t ceros[64] __attribute__((aligned(64)));
static uint8_t ocupado;
struct hijo {struct i915_tls_resultado *r;uintptr_t esperado,padre;unsigned indice;};

/* Cada lectura vuelve a resolver FS:0 y los offsets TLS. Impide que una
 * direccion guardada en un registro disfrace una restauracion FS ausente. */
__attribute__((noinline)) static uintptr_t i915_tls_direccion(void){
    __asm__ volatile("":::"memory");return (uintptr_t)&ceros[0];
}
__attribute__((noinline)) static uintptr_t i915_tls_self(void){
    uintptr_t p;__asm__ volatile("movq %%fs:0,%0":"=r"(p)::"memory");return p;
}
__attribute__((noinline)) int i915_tls_verificar(uint64_t valor,uint8_t byte){
    __asm__ volatile("":::"memory");
    if(dato!=valor)return 0;
    for(unsigned i=0;i<64;i++)if(ceros[i]!=byte)return 0;
    return 1;
}
__attribute__((noinline)) void i915_tls_escribir(uint64_t valor,uint8_t byte){
    __asm__ volatile("":::"memory");dato=valor;
    for(unsigned i=0;i<64;i++)ceros[i]=byte;
}
static void tarea(void *arg){
    struct hijo *h=arg;struct i915_tls_resultado *r=h->r;
    uint32_t bit=1u<<h->indice;
    if(taek_tls_base_actual()!=h->esperado || i915_tls_self()!=h->esperado)r->fallos|=bit;
    uintptr_t addr=i915_tls_direccion();
    if((addr&63) || addr>=i915_tls_self() || i915_tls_self()-addr>r->imagen_bytes)r->fallos|=1u<<2;
    if(i915_tls_verificar(INICIAL,0))r->iniciales++;else r->fallos|=1u<<3;
    if(i915_tls_self()==h->esperado && i915_tls_self()!=h->padre)r->privados++;
    else r->fallos|=1u<<4;
    uint64_t valor=UINT64_C(0x1111000000000000)+(uint64_t)h->indice*UINT64_C(0x2222000000000000);
    uint8_t byte=(uint8_t)(0x35+0x41*h->indice);
    i915_tls_escribir(valor,byte);
    for(unsigned i=0;i<1024;i++){
        i915_tarea_ceder();
        if(taek_tls_base_actual()!=h->esperado || i915_tls_self()!=h->esperado)r->fallos|=bit;
        if(!i915_tls_verificar(valor,byte))r->fallos|=1u<<5;
        valor++;byte++;i915_tls_escribir(valor,byte);r->hijo_ciclos[h->indice]++;
    }
}
int i915_tls_contexto_probar(struct i915_tls_resultado *out){
    if(!out || !trabajos_es_coordinador() || trabajos_en_curso() ||
       i915_scheduler_cpu_actual()!=0)return -1;
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return -1;
    *out=(struct i915_tls_resultado){.imagen_bytes=taek_tls_imagen_bytes()};
    uintptr_t padre=taek_tls_base_actual();
    if(!padre || i915_tls_self()!=padre || !out->imagen_bytes){
        out->fallos=1u<<6;__atomic_clear(&ocupado,__ATOMIC_RELEASE);return -2;
    }
    i915_tareas_iniciar();
    memoria_estadisticas_t antes,despues;memoria_obtener_estadisticas(&antes);
    uint64_t original=dato;uint8_t original_ceros[64];
    for(unsigned i=0;i<64;i++)original_ceros[i]=ceros[i];
    const uint64_t sentinel=UINT64_C(0xbbbb1234aaaa9876);
    i915_tls_escribir(sentinel,0xa7);
    struct hijo args[2]={{.r=out,.padre=padre,.indice=0},{.r=out,.padre=padre,.indice=1}};
    struct task_struct *t[2]={NULL,NULL};
    for(unsigned i=0;i<2;i++){
        t[i]=i915_tarea_crear(tarea,&args[i],"tls-estatico",0);
        if(!t[i]){out->fallos|=1u<<7;break;}
        args[i].esperado=(uintptr_t)t[i]->tls_base;
    }
    if(t[0] && t[1]){
        if(args[0].esperado==args[1].esperado)out->fallos|=1u<<4;
        for(unsigned i=0;i<1024;i++){
            i915_tarea_ceder();out->ciclos++;
            if(taek_tls_base_actual()!=padre || i915_tls_self()!=padre)out->fallos|=1u<<6;
            if(!i915_tls_verificar(sentinel,0xa7))out->fallos|=1u<<8;
        }
        for(unsigned i=0;i<8 && (t[0]->state!=TASK_DEAD || t[1]->state!=TASK_DEAD);i++)i915_tarea_ceder();
        if(t[0]->state!=TASK_DEAD || t[1]->state!=TASK_DEAD ||
           out->hijo_ciclos[0]!=1024 || out->hijo_ciclos[1]!=1024)out->fallos|=1u<<9;
    }
    for(unsigned i=0;i<2;i++)if(t[i])i915_tarea_destruir(t[i]);
    /* La prueba conserva el estado del llamador incluso en el mutante. */
    dato=original;for(unsigned i=0;i<64;i++)ceros[i]=original_ceros[i];
    memoria_obtener_estadisticas(&despues);
    if(antes.heap_bytes_en_uso==despues.heap_bytes_en_uso &&
       antes.heap_bloques_activos==despues.heap_bloques_activos && despues.canarios_intactos)out->heap_limpio=1;
    else out->fallos|=1u<<10;
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);return out->fallos?-3:0;
}
