#ifndef TAEK_USUARIO_HILOS_H
#define TAEK_USUARIO_HILOS_H
/* H213, Fase D del gestor de hilos: hilos del programa sobre SYS_HILO y SYS_FUTEX.
 *
 *  - El principal sigue en la CPU 0; cada hilo creado corre en una CPU auxiliar (1 a 3) con prioridad de fondo.
 *  - Desde un hilo solo valen: taek_escribir, taek_ms, taek_ceder, taek_esperar, taek_hilo y taek_futex. Pantalla,
 *    archivos, red, audio y memoria quedan para el principal (-TAEK_ENOPERMITIDA).
 *  - malloc (usuario/memoria.h) no tiene cerrojos: las pilas y lo demas, pedirlos desde el principal.
 *  - Si un hilo falla, termina el programa entero. Si el principal termina, el nucleo detiene a los demas. */
#include "taek.h"

typedef long (*taek_hilo_fn)(void *);
struct taek_hilo_arranque { taek_hilo_fn fn; void *arg; };

/* Primera instruccion del hilo: el nucleo pasa rdi = bloque de arranque (en lo alto de su pila) y rsi = id */
static __attribute__((noreturn, used)) void taek__hilo_inicio(uint64_t arranque, uint64_t id) {
    (void)id;
    struct taek_hilo_arranque *a = (struct taek_hilo_arranque *)(uintptr_t)arranque;
    long v = a->fn(a->arg);
    taek_hilo(TAEK_HILO_SALIR, v);
    for (;;) {}
}

/* pila y bytes: un bloque de memoria del programa (por ejemplo, de malloc en el principal). tls: base de FS del hilo
 * (0 sin TLS). cpu: 1 a 3, o 0 para que elija el nucleo. Devuelve el id (1 a TAEK_HILOS_MAX) o un error negativo. */
static inline long taek_hilo_crear(taek_hilo_fn fn, void *arg, void *pila, uint64_t bytes, uint64_t tls, uint32_t cpu) {
    uint64_t tope = ((uint64_t)(uintptr_t)pila + bytes) & ~(uint64_t)15;
    tope -= 16;                                   /* El bloque de arranque queda encima de la pila del hilo */
    struct taek_hilo_arranque *a = (struct taek_hilo_arranque *)(uintptr_t)tope;
    a->fn = fn;
    a->arg = arg;
    struct taek_hilo t;
    memset(&t, 0, sizeof(t));
    t.entrada = (uint64_t)(uintptr_t)taek__hilo_inicio;
    t.arg = tope;
    t.pila = tope;
    t.tls = tls;
    t.cpu = cpu;
    return taek_hilo(TAEK_HILO_CREAR, (long)&t);
}

/* Espera a que termine (sin limite). En res quedan fin (TAEK_HILO_FIN_*), valor y, si fallo, vector, rip y cr2. */
static inline long taek_hilo_unir(long id, struct taek_hilo *res) {
    struct taek_hilo t;
    memset(&t, 0, sizeof(t));
    t.id = (uint32_t)id;
    long r;
    do r = taek_hilo(TAEK_HILO_UNIR, (long)&t); while (r == -TAEK_ETIEMPO);
    if (res) *res = t;
    return r;
}

static inline long taek_hilo_yo(void) { return taek_hilo(TAEK_HILO_YO, 0); }
static inline long taek_hilo_cpu(void) { return taek_hilo(TAEK_HILO_CPU, 0); }
static inline long taek_hilo_tls(uint64_t base) { return taek_hilo(TAEK_HILO_TLS, (long)base); }
static inline uint64_t taek_tls_leer(uint64_t desplazamiento) {
    uint64_t v;
    __asm__ volatile("movq %%fs:(%1), %0" : "=r"(v) : "r"(desplazamiento) : "memory");
    return v;
}

/* Mutex de tres estados (0 libre, 1 tomado, 2 tomado con alguien esperando) sobre un futex. */
typedef struct { volatile uint32_t v; } taek_mutex;
static inline void taek_mutex_tomar(taek_mutex *m) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        taek_futex(TAEK_FUTEX_ESPERAR, &m->v, 2, 0);
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    }
}
static inline void taek_mutex_soltar(taek_mutex *m) {
    if (__atomic_exchange_n(&m->v, 0, __ATOMIC_RELEASE) == 2) taek_futex(TAEK_FUTEX_DESPERTAR, &m->v, 1, 0);
}

#endif
