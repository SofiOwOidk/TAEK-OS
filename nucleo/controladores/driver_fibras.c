/*
 * driver_fibras.c -- Motor residente de fibras cooperativas (TaekOS, ABI 1).
 *
 * Reutiliza exclusivamente el primitivo de cambio de contexto real:
 *   i915_contexto_preparar() / i915_cambiar_contexto() / i915_contexto_marco
 * (nucleo/compatibilidad/linux_i915/tareas.h, H120). No toca el planificador
 * global ni define estructuras Linux homonimas.
 *
 * Garantia de IF: las transiciones y publicaciones se hacen con IRQ
 * deshabilitadas en nativo. Al primitivo se le pasa como cuarto argumento el IF
 * que tenia el contexto antes del CLI, de modo que al reanudar cada contexto
 * recupere su propio IF. Una raiz que entra con IF=0 no termina con IF=1. Las
 * fibras nuevas arrancan con IF=1 (worker nuevo del primitivo).
 *
 * En TAEK_HOST_TEST no se emite CLI/STI ni se toca FS real: el primitivo ya lo
 * evita; aqui solo se prueban las transiciones y la pila real.
 */

#include "driver_fibras.h"
#include "../compatibilidad/linux_i915/tareas.h"

/* Token de despertar coalescido: un bit por fibra. */
#define FIBRA_PEND_TLS   0x1u
#define FIBRA_PEND_PILA  0x2u

struct fibra_control {
    fibra_func      fn;
    void           *ctx;
    void           *pila;      /* pila de FIBRA_PILA_BYTES */
    void           *tls;       /* TP de la imagen TLS */
    void           *rsp;       /* contexto guardado por el primitivo */
    fibra_handle_t  handle;
    uint32_t        generacion;
    uint8_t         estado;
    uint8_t         despertar_pendiente;
    uint8_t         pendiente; /* bits FIBRA_PEND_* */
    uint8_t         agotado;
};

struct fibra_motor {
    struct fibra_callbacks cb;
    struct fibra_control   slots[FIBRA_SLOTS];
    void                  *raiz_rsp;      /* contexto raiz de la llamada actual */
    fibra_handle_t         actual;        /* 0 en raiz */
    unsigned               rr;            /* cursor round robin */
    int                    iniciado;
    int                    en_ejecutar;
    int                    notificando;
    fibra_observador_fn     observador;
    void                  *observador_ctx;
    struct fibra_estadisticas st;
};

static struct fibra_motor g_motor;
/* No permitir mutaciones sobre la pila origen con identidad destino publicada.
 * FX del callback no debe contaminar el estado que guardara el primitivo. */
static void fibra_notificar(fibra_handle_t destino){
    if(!g_motor.observador)return;
    uint8_t fx[512] __attribute__((aligned(16)));
    __asm__ volatile("fxsave64 %0":"=m"(fx)::"memory");
    g_motor.notificando=1;
    g_motor.observador(destino,g_motor.observador_ctx);
    g_motor.notificando=0;
    __asm__ volatile("fxrstor64 %0"::"m"(fx):"memory");
}

/* ------------------------------------------------------------------------- */
/* Proteccion de interrupciones                                              */
/* ------------------------------------------------------------------------- */
#ifdef TAEK_HOST_TEST
static inline unsigned long fibra_irq_desactivar(void) { return (unsigned long)RFLAGS_IF_BIT; }
static inline void fibra_irq_restaurar(unsigned long f) { (void)f; }
static inline void fibra_irq_recli(void) { }
#else
static inline unsigned long fibra_irq_desactivar(void) {
    unsigned long f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void fibra_irq_restaurar(unsigned long f) {
    if (f & RFLAGS_IF_BIT) __asm__ volatile("sti" ::: "memory");
}
static inline void fibra_irq_recli(void) { __asm__ volatile("cli" ::: "memory"); }
#endif

/* ------------------------------------------------------------------------- */
/* Handles y resolucion                                                      */
/* ------------------------------------------------------------------------- */
static fibra_handle_t fibra_handle_de(unsigned idx, uint32_t generacion) {
    return ((uint64_t)generacion << 32) | (uint64_t)(idx + 1u);
}

static int fibra_resolver(fibra_handle_t h, struct fibra_control **out) {
    if (h == FIBRA_HANDLE_NULO) return -1;
    uint32_t generacion = (uint32_t)(h >> 32);
    uint32_t idx = (uint32_t)(h & UINT64_C(0xffffffff));
    if (idx == 0 || idx > FIBRA_SLOTS) return -1;
    struct fibra_control *f = &g_motor.slots[idx - 1u];
    if (f->estado == FIBRA_LIBRE) return -1;
    if (f->handle != h) return -1; /* no aceptar handles nunca publicados */
    if (f->generacion != generacion) return -1;
    *out = f;
    return 0;
}

static int fibra_hay_recursos_vivos(void) {
    for (unsigned i = 0; i < FIBRA_SLOTS; i++)
        if (g_motor.slots[i].estado != FIBRA_LIBRE || g_motor.slots[i].pendiente != 0)
            return 1;
    return 0;
}

static int fibra_slot_libre(void) {
    for (unsigned i = 0; i < FIBRA_SLOTS; i++)
        if (g_motor.slots[i].estado == FIBRA_LIBRE && !g_motor.slots[i].agotado)
            return (int)i;
    return -1;
}

static int fibra_siguiente_lista(void) {
    for (unsigned k = 0; k < FIBRA_SLOTS; k++) {
        unsigned idx = (g_motor.rr + k) % FIBRA_SLOTS;
        uint8_t e = g_motor.slots[idx].estado;
        if (e == FIBRA_NUEVA || e == FIBRA_LISTA) {
            g_motor.rr = (idx + 1u) % FIBRA_SLOTS;
            return (int)idx;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Liberacion de una ranura (con reintento)                                  */
/* ------------------------------------------------------------------------- */
static void fibra_reiniciar_slot(struct fibra_control *f) {
    if (f->generacion >= UINT32_MAX) f->agotado = 1;   /* sin reutilizar: evita handle repetido */
    else f->generacion++;
    f->fn = NULL; f->ctx = NULL; f->pila = NULL; f->tls = NULL; f->rsp = NULL;
    f->handle = FIBRA_HANDLE_NULO;
    f->estado = FIBRA_LIBRE;
    f->despertar_pendiente = 0;
    f->pendiente = 0;
}

/* Intenta liberar TLS y pila. Devuelve FIBRA_OK solo si ambos quedaron libres;
 * si no, conserva la ranura con los bits pendientes para reintento. */
static int fibra_liberar_slot(struct fibra_control *f) {
    int pend = 0;
    if (f->tls) {
        if (g_motor.cb.tls_destruir(f->tls) == 0) {
            f->tls = NULL; g_motor.st.tls_liberados++;
        } else {
            pend |= FIBRA_PEND_TLS; g_motor.st.fallos_tls++;
        }
    }
    if (f->pila) {
        if (g_motor.cb.liberar(f->pila) == 0) {
            f->pila = NULL; g_motor.st.pilas_liberadas++;
        } else {
            pend |= FIBRA_PEND_PILA; g_motor.st.fallos_pila++;
        }
    }
    f->pendiente = (uint8_t)pend;
    if (pend) {
        /* Puede haber desaparecido TLS o pila. No volver a seleccionar una
         * NUEVA; conservar su handle publicado exclusivamente para retry. */
        f->estado = FIBRA_PARCIAL;
        return FIBRA_E_PENDIENTE;
    }
    if (f->handle != FIBRA_HANDLE_NULO) g_motor.st.liberadas++;
    fibra_reiniciar_slot(f);
    return FIBRA_OK;
}

/* Rollback de creacion. Si el backend falla al liberar, conserva la ranura como
 * PARCIAL (sin handle) con los bits pendientes para fibra_recolectar_parciales. */
static int fibra_rollback_creacion(struct fibra_control *f, void *pila, void *tls) {
    int pend = 0;
    if (tls) {
        if (g_motor.cb.tls_destruir(tls) == 0) g_motor.st.tls_liberados++;
        else pend |= FIBRA_PEND_TLS;
    }
    if (pila) {
        if (g_motor.cb.liberar(pila) == 0) g_motor.st.pilas_liberadas++;
        else pend |= FIBRA_PEND_PILA;
    }
    if (!pend) return FIBRA_OK;
    f->fn = NULL; f->ctx = NULL;
    f->tls = (pend & FIBRA_PEND_TLS) ? tls : NULL;
    f->pila = (pend & FIBRA_PEND_PILA) ? pila : NULL;
    f->rsp = NULL; f->handle = FIBRA_HANDLE_NULO;
    f->estado = FIBRA_PARCIAL;
    f->despertar_pendiente = 0;
    f->pendiente = (uint8_t)pend;
    return FIBRA_E_PENDIENTE;
}

/* ------------------------------------------------------------------------- */
/* Cambio de contexto: trampolin, entrada y salida                           */
/* ------------------------------------------------------------------------- */
static void fibra_terminar(struct fibra_control *f) __attribute__((noreturn, used));
static void fibra_iniciar(struct fibra_control *f) __attribute__((used));

static void fibra_iniciar(struct fibra_control *f) {
    fibra_func fn = f->fn;
    void *ctx = f->ctx;
    if (fn) fn(ctx);
    fibra_terminar(f);
}

/* Arranque de fibra nueva: i915_contexto_preparar deja r12 = puntero a la
 * ranura, y el primitivo salta aqui con RSP alineada a 16. */
__attribute__((naked, used)) static void fibra_trampoline(void) {
    __asm__ __volatile__(
        "movq %r12, %rdi\n\t"
        "call fibra_iniciar\n\t"
        "ud2\n\t"
    );
}

static void fibra_salir_a_raiz(struct fibra_control *f, unsigned long flags) {
    g_motor.actual = FIBRA_HANDLE_NULO;
    fibra_notificar(FIBRA_HANDLE_NULO);
    i915_cambiar_contexto(&f->rsp, g_motor.raiz_rsp, NULL, (uint64_t)flags);
}

static void fibra_terminar(struct fibra_control *f) {
    unsigned long flags = fibra_irq_desactivar();
    f->estado = FIBRA_MUERTA;
    g_motor.st.terminadas++;
    g_motor.actual = FIBRA_HANDLE_NULO;
    /* Se abandona esta pila: raiz jamas reanuda una fibra MUERTA. */
    fibra_notificar(FIBRA_HANDLE_NULO);
    i915_cambiar_contexto(&f->rsp, g_motor.raiz_rsp, NULL, (uint64_t)flags);
    __builtin_unreachable();
}

/* ------------------------------------------------------------------------- */
/* API publica                                                               */
/* ------------------------------------------------------------------------- */
int fibra_motor_iniciar(const struct fibra_callbacks *cb) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!cb) return FIBRA_E_ARGUMENTOS;
    if (cb->version != FIBRA_CALLBACKS_VERSION) return FIBRA_E_ARGUMENTOS;
    if (cb->bytes < FIBRA_CALLBACKS_BYTES) return FIBRA_E_ARGUMENTOS;
    if (!cb->asignar || !cb->liberar || !cb->tls_crear || !cb->tls_destruir ||
        !cb->permitido)
        return FIBRA_E_ARGUMENTOS;
    if (!cb->permitido()) return FIBRA_E_NO_PERMITIDO;
    if (fibra_hay_recursos_vivos()) return FIBRA_E_YA_INICIADO;

    unsigned long flags = fibra_irq_desactivar();
    /* Una raiz registrada tambien tiene ownership del contexto externo. */
    fibra_notificar(FIBRA_HANDLE_NULO);
    g_motor.observador=NULL;g_motor.observador_ctx=NULL;
    g_motor.cb = *cb;
    g_motor.raiz_rsp = NULL;
    g_motor.actual = FIBRA_HANDLE_NULO;
    g_motor.rr = 0;
    g_motor.en_ejecutar = 0;
    g_motor.iniciado = 1;
    fibra_irq_restaurar(flags);
    return FIBRA_OK;
}

int fibra_motor_observar(fibra_observador_fn fn,void *ctx){
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if(!g_motor.iniciado)return FIBRA_E_NO_INICIADO;
    if(!g_motor.cb.permitido())return FIBRA_E_NO_PERMITIDO;
    if(g_motor.actual || g_motor.en_ejecutar)return FIBRA_E_SOLO_RAIZ;
    if(fibra_hay_recursos_vivos())return FIBRA_E_OCUPADO;
    if(!fn && ctx)return FIBRA_E_ARGUMENTOS;
    unsigned long flags=fibra_irq_desactivar();
    fibra_notificar(FIBRA_HANDLE_NULO);
    g_motor.observador=fn;g_motor.observador_ctx=ctx;
    fibra_notificar(FIBRA_HANDLE_NULO);
    fibra_irq_restaurar(flags);return FIBRA_OK;
}

int fibra_crear(fibra_func fn, void *ctx, fibra_handle_t *out_handle) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!out_handle) return FIBRA_E_ARGUMENTOS;
    *out_handle = FIBRA_HANDLE_NULO;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!fn) return FIBRA_E_ARGUMENTOS;
    if (g_motor.actual != FIBRA_HANDLE_NULO) return FIBRA_E_SOLO_RAIZ;
    if (g_motor.en_ejecutar) return FIBRA_E_REENTRANTE;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;

    unsigned long flags = fibra_irq_desactivar();
    int idx = fibra_slot_libre();
    if (idx < 0) { g_motor.st.fallos++; fibra_irq_restaurar(flags); return FIBRA_E_AGOTADO; }

    struct fibra_control *f = &g_motor.slots[idx];
    void *pila = g_motor.cb.asignar(FIBRA_PILA_BYTES);
    if (!pila) { g_motor.st.fallos++; fibra_irq_restaurar(flags); return FIBRA_E_MEMORIA; }

    void *tls = g_motor.cb.tls_crear();
    if (!tls) {
        g_motor.st.fallos++;
        int rollback = fibra_rollback_creacion(f, pila, NULL);
        fibra_irq_restaurar(flags);
        return rollback == FIBRA_OK ? FIBRA_E_TLS : FIBRA_E_PENDIENTE;
    }

    struct i915_contexto_marco *marco =
        i915_contexto_preparar(pila, FIBRA_PILA_BYTES, fibra_trampoline,
                               (uint64_t)(uintptr_t)f);
    if (!marco) {
        g_motor.st.fallos++;
        int rollback = fibra_rollback_creacion(f, pila, tls);
        fibra_irq_restaurar(flags);
        return rollback == FIBRA_OK ? FIBRA_E_CONTEXTO : FIBRA_E_PENDIENTE;
    }

    /* TP de la fibra en fs_base; la fibra nueva arranca con IF=1. */
    marco->fs_base = (uint64_t)(uintptr_t)tls;
    marco->irq_flags = 0x202u;

    if (f->generacion == 0) f->generacion = 1;
    f->fn = fn; f->ctx = ctx; f->pila = pila; f->tls = tls; f->rsp = marco;
    f->handle = fibra_handle_de((unsigned)idx, f->generacion);
    f->estado = FIBRA_NUEVA;
    f->despertar_pendiente = 0;
    f->pendiente = 0;
    *out_handle = f->handle;
    g_motor.st.creadas++;
    fibra_irq_restaurar(flags);
    return FIBRA_OK;
}

int fibra_ejecutar(int pasos) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (g_motor.actual != FIBRA_HANDLE_NULO || g_motor.en_ejecutar)
        return FIBRA_E_REENTRANTE;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (pasos < 0) return FIBRA_E_ARGUMENTOS;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;

    unsigned long flags = fibra_irq_desactivar();
    g_motor.en_ejecutar = 1;
    g_motor.raiz_rsp = NULL;
    int cambios = 0;

    while (cambios < pasos) {
        int idx = fibra_siguiente_lista();
        if (idx < 0) break;
        struct fibra_control *f = &g_motor.slots[idx];
        f->estado = FIBRA_EJECUTANDO;
        g_motor.actual = f->handle;
        fibra_notificar(f->handle);
        i915_cambiar_contexto(&g_motor.raiz_rsp, f->rsp, NULL, (uint64_t)flags);
        fibra_irq_recli();
        g_motor.actual = FIBRA_HANDLE_NULO;
        cambios++;
        g_motor.st.cambios++;
        fibra_irq_recli();
    }

    g_motor.en_ejecutar = 0;
    g_motor.raiz_rsp = NULL;
    fibra_irq_restaurar(flags);
    return cambios;
}

fibra_handle_t fibra_actual(void) {
    return g_motor.iniciado && !g_motor.notificando && g_motor.cb.permitido() ? g_motor.actual : FIBRA_HANDLE_NULO;
}

int fibra_yield(void) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;
    fibra_handle_t h = g_motor.actual;
    if (h == FIBRA_HANDLE_NULO) return FIBRA_E_SOLO_FIBRA;
    struct fibra_control *f;
    if (fibra_resolver(h, &f)) return FIBRA_E_HANDLE;

    unsigned long flags = fibra_irq_desactivar();
    f->estado = FIBRA_LISTA;
    g_motor.st.yields++;
    fibra_salir_a_raiz(f, flags);
    return FIBRA_OK;
}

int fibra_bloquear(void) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;
    fibra_handle_t h = g_motor.actual;
    if (h == FIBRA_HANDLE_NULO) return FIBRA_E_SOLO_FIBRA;
    struct fibra_control *f;
    if (fibra_resolver(h, &f)) return FIBRA_E_HANDLE;

    unsigned long flags = fibra_irq_desactivar();
    if (f->despertar_pendiente) {
        f->despertar_pendiente = 0;   /* unico token coalescido: no se pierde el wake */
        fibra_irq_restaurar(flags);
        return FIBRA_OK;
    }
    f->estado = FIBRA_BLOQUEADA;
    g_motor.st.bloqueos++;
    fibra_salir_a_raiz(f, flags);
    return FIBRA_OK;
}

int fibra_despertar(fibra_handle_t h) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;
    struct fibra_control *f;
    if (fibra_resolver(h, &f)) return FIBRA_E_HANDLE;

    unsigned long flags = fibra_irq_desactivar();
    int r = FIBRA_OK;
    switch (f->estado) {
        case FIBRA_BLOQUEADA:
            f->estado = FIBRA_LISTA;
            break;
        case FIBRA_NUEVA:
        case FIBRA_LISTA:
        case FIBRA_EJECUTANDO:
            f->despertar_pendiente = 1;   /* wake previo al bloqueo */
            break;
        default:
            r = FIBRA_E_ESTADO;
            break;
    }
    if (r == FIBRA_OK) g_motor.st.despertares++;
    fibra_irq_restaurar(flags);
    return r;
}

int fibra_liberar(fibra_handle_t h) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;
    if (g_motor.actual != FIBRA_HANDLE_NULO) return FIBRA_E_SOLO_RAIZ;
    struct fibra_control *f;
    if (fibra_resolver(h, &f)) return FIBRA_E_HANDLE;
    if (!(f->estado == FIBRA_MUERTA || f->estado == FIBRA_NUEVA || f->estado == FIBRA_PARCIAL))
        return FIBRA_E_ESTADO;

    unsigned long flags = fibra_irq_desactivar();
    int r = fibra_liberar_slot(f);
    fibra_irq_restaurar(flags);
    return r;
}

int fibra_estado(fibra_handle_t h) {
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;
    struct fibra_control *f;
    if (fibra_resolver(h, &f)) return FIBRA_E_HANDLE;
    return (int)f->estado;
}

int fibra_recolectar_parciales(void) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;
    if (g_motor.actual != FIBRA_HANDLE_NULO || g_motor.en_ejecutar) return FIBRA_E_SOLO_RAIZ;
    unsigned long flags = fibra_irq_desactivar();
    int restantes = 0;
    for (unsigned i = 0; i < FIBRA_SLOTS; i++) {
        struct fibra_control *f = &g_motor.slots[i];
        if (f->estado == FIBRA_PARCIAL || f->pendiente != 0) {
            if (fibra_liberar_slot(f) != FIBRA_OK) restantes++;
        }
    }
    fibra_irq_restaurar(flags);
    return restantes;
}

int fibra_motor_detener(void) {
    if(g_motor.notificando)return FIBRA_E_REENTRANTE;
    if (!g_motor.iniciado) return FIBRA_E_NO_INICIADO;
    if (!g_motor.cb.permitido()) return FIBRA_E_NO_PERMITIDO;
    if (g_motor.actual != FIBRA_HANDLE_NULO || g_motor.en_ejecutar)
        return FIBRA_E_OCUPADO;

    unsigned long flags = fibra_irq_desactivar();
    for (unsigned i = 0; i < FIBRA_SLOTS; i++) {
        struct fibra_control *f = &g_motor.slots[i];
        if (f->estado == FIBRA_MUERTA || f->estado == FIBRA_NUEVA ||
            f->estado == FIBRA_PARCIAL)
            (void)fibra_liberar_slot(f);
    }
    for (unsigned i = 0; i < FIBRA_SLOTS; i++) {
        if (g_motor.slots[i].estado != FIBRA_LIBRE) {
            fibra_irq_restaurar(flags);
            return FIBRA_E_PENDIENTE;
        }
    }
    fibra_notificar(FIBRA_HANDLE_NULO);
    g_motor.observador=NULL;g_motor.observador_ctx=NULL;
    g_motor.iniciado = 0;
    fibra_irq_restaurar(flags);
    return FIBRA_OK;
}

void fibra_motor_estadisticas(struct fibra_estadisticas *out) {
    if (!out) return;
    *out = g_motor.st;
    out->slots_libres = 0;
    out->slots_ocupados = 0;
    out->slots_pendientes = 0;
    uint32_t gmax = 0;
    for (unsigned i = 0; i < FIBRA_SLOTS; i++) {
        struct fibra_control *f = &g_motor.slots[i];
        if (f->estado == FIBRA_LIBRE) out->slots_libres++;
        else out->slots_ocupados++;
        if (f->pendiente) out->slots_pendientes++;
        if (f->generacion > gmax) gmax = f->generacion;
    }
    out->generacion_maxima = gmax;
}

#ifdef TAEK_HOST_TEST
void fibra_prueba_forzar_generacion(unsigned slot, uint32_t generacion) {
    if (slot < FIBRA_SLOTS && g_motor.slots[slot].estado == FIBRA_LIBRE)
        g_motor.slots[slot].generacion = generacion;
}
#endif
