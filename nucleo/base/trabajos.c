#include "trabajos.h"
static uint64_t reloj(void) {
    unsigned a,d;
    __asm__ volatile("lfence; rdtsc":"=a"(a),"=d"(d)::"memory");
    return ((uint64_t)d<<32)|a;
}
void trabajos_preparar(trabajos_lote *l, trabajo_fn f, void *p, unsigned n) {
    l->funcion=f;l->contexto=p;l->total=n;l->siguiente=0;l->completados=0;
    l->cancelado=0;l->activo=0;
    for(unsigned i=0;i<TRABAJOS_MAX_CPU;i++){l->ejecutados[i]=0;l->ciclos[i]=0;}
    __atomic_store_n(&l->activo,1,__ATOMIC_RELEASE);
}
int trabajos_tomar(trabajos_lote *l,unsigned cpu) {
    if(cpu>=TRABAJOS_MAX_CPU || !__atomic_load_n(&l->activo,__ATOMIC_ACQUIRE))return 0;
    unsigned i=__atomic_fetch_add(&l->siguiente,1,__ATOMIC_RELAXED);
    if(i>=l->total)return 0;
    if(!__atomic_load_n(&l->cancelado,__ATOMIC_ACQUIRE)) {
        uint64_t t=reloj();l->funcion(l->contexto,i,cpu);
        l->ciclos[cpu]+=reloj()-t;l->ejecutados[cpu]++;
    }
    __atomic_fetch_add(&l->completados,1,__ATOMIC_RELEASE);
    return 1;
}
void trabajos_cancelar(trabajos_lote *l) {__atomic_store_n(&l->cancelado,1,__ATOMIC_RELEASE);}
int trabajos_terminados(const trabajos_lote *l) {
    return __atomic_load_n(&l->completados,__ATOMIC_ACQUIRE)==l->total;
}
