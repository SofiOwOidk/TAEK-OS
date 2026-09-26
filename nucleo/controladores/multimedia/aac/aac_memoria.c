#include "aac_memoria.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(TAEK_KERNEL) || defined(__kernel__) || !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
#include "base/memoria.h"
static void *def_asignar(size_t n) { return asignar_memoria(n); }
static void def_liberar(void *p) { liberar_memoria(p); }
#else
#include <stdlib.h>
static void *def_asignar(size_t n) { return malloc(n); }
static void def_liberar(void *p) { free(p); }
#endif

static void *(*g_asignar)(size_t bytes) = def_asignar;
static void (*g_liberar)(void *ptr) = def_liberar;

void aac_fijar_asignador(void *(*asignar)(size_t bytes), void (*liberar)(void *ptr)) {
    if (asignar) g_asignar = asignar;
    if (liberar) g_liberar = liberar;
}

void *helix_malloc(int size) {
    if (size <= 0) return 0;
    return g_asignar((size_t)size);
}

void helix_free(void *ptr) {
    if (ptr) g_liberar(ptr);
}

#ifdef __cplusplus
}
#endif
