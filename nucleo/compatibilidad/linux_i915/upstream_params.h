#ifndef TAEK_UPSTREAM_PARAMS_H
#define TAEK_UPSTREAM_PARAMS_H
typedef void *(*taek_heap_alloc_fn)(unsigned long);
typedef int (*taek_heap_free_fn)(void *);
struct taek_upstream_heap_estado {
    unsigned activos,errores,fallos,asignaciones,pico;
    unsigned long bytes;
};
struct taek_upstream_params_resultado {
    unsigned checks,fallos,limpio,oom_probados,privados;
};
struct taek_upstream_heap_contrato {unsigned checks,fallos,limpio,errores_esperados,fallos_esperados;};
int taek_upstream_heap_configurar(taek_heap_alloc_fn alloc,taek_heap_free_fn free);
void taek_upstream_heap_estado(struct taek_upstream_heap_estado *out);
void taek_upstream_heap_fallar_en(unsigned llamada);
int taek_upstream_params_probar(struct taek_upstream_params_resultado *out);
int taek_upstream_heap_probar(struct taek_upstream_heap_contrato *out);
#endif
