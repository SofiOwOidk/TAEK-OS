#include "hilos.h"
#include "memoria.h"
#include "tiempo.h"
#include "trabajos.h"
#include "paginacion.h"
#include "../arquitectura/x86_64/apic.h"
#include "../arquitectura/x86_64/idt.h"
#include "../arquitectura/x86_64/tls.h"
#include "../arquitectura/x86_64/gdt.h"

/* El cambio de contexto de compatibilidad/linux_i915/tareas.c, sin traer aqui los tipos del shim de Linux. El marco es
 * el mismo (tareas.c comprueba 608 bytes, rsp en 512, fs_base en 520 e IF en 528). */
struct i915_contexto_marco {
    uint8_t fpu[512] __attribute__((aligned(16)));
    void *registros_rsp;
    uint64_t fs_base;
    uint64_t irq_flags, reservado;
    void *traza;
    uint64_t r15, r14, r13, r12, rbx, rbp;
    void (*entrada)(void);
};
_Static_assert(sizeof(struct i915_contexto_marco) == 608, "marco de tareas.c");
_Static_assert(__builtin_offsetof(struct i915_contexto_marco, fs_base) == 520, "fs_base de tareas.c");
_Static_assert(__builtin_offsetof(struct i915_contexto_marco, irq_flags) == 528, "irq_flags de tareas.c");
void i915_cambiar_contexto(void **desde, void *hacia, void *traza, uint64_t irq_flags);
struct i915_contexto_marco *i915_contexto_preparar(void *stack, size_t bytes, void (*entrada)(void), uint64_t r12);

#define HILOS_CPUS CERROJO_CPUS
#define VECTOR_TICTAC 48
#define LAPIC_LVT_TIMER 0x320u
#define LAPIC_TIMER_INICIAL 0x380u
#define LAPIC_TIMER_ACTUAL 0x390u
#define LAPIC_TIMER_DIVISOR 0x3E0u
#define LVT_ENMASCARADO 0x10000u
#define LVT_PERIODICO 0x20000u
#define PILA_POR_OMISION (64u * 1024u)

struct cpu_hilos {
    cerrojo_t cerrojo;             /* Colas de esta CPU; nunca se tiene a traves de un cambio de contexto */
    hilo_t *actual;                /* NULL: corre la raiz */
    void *raiz_rsp;
    hilo_t *listos[HILO_PRIORIDADES], *listos_cola[HILO_PRIORIDADES];
    hilo_t *dormidos;
    uint32_t hilos;                /* Hilos vivos asignados a esta CPU */
    volatile uint32_t cambio_pedido;
    int tictac, lista;
    uint64_t apic_por_ms, tsc_ultimo_tic, desvio_max, tics, cambios, desalojos, creados;
};
static struct cpu_hilos g_cpu[HILOS_CPUS];
static uint32_t g_siguiente_id = 1;
static int g_vector_listo;

static inline uint64_t tsc(void) { uint32_t a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return ((uint64_t)d << 32) | a; }
static inline uint64_t apagar_irq(void) { uint64_t f; __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory"); return f; }
static inline void restaurar_irq(uint64_t f) { if (f & 0x200u) __asm__ volatile("sti" ::: "memory"); }

/* ------------------------------------------------------------------ colas (con el cerrojo de la CPU tomado) */

static void encolar(struct cpu_hilos *c, hilo_t *h) {
    unsigned p = h->prioridad < HILO_PRIORIDADES ? h->prioridad : HILO_FONDO;
    h->siguiente = 0;
    if (c->listos_cola[p]) c->listos_cola[p]->siguiente = h; else c->listos[p] = h;
    c->listos_cola[p] = h;
}
static hilo_t *sacar(struct cpu_hilos *c) {
    for (unsigned p = 0; p < HILO_PRIORIDADES; p++) {
        hilo_t *h = c->listos[p];
        if (!h) continue;
        c->listos[p] = h->siguiente;
        if (!c->listos[p]) c->listos_cola[p] = 0;
        h->siguiente = 0;
        return h;
    }
    return 0;
}
static int hay_mas_urgente(struct cpu_hilos *c, unsigned prioridad) {
    for (unsigned p = 0; p < prioridad && p < HILO_PRIORIDADES; p++) if (c->listos[p]) return 1;
    return 0;
}

/* ------------------------------------------------------------------ tictac */

static void tictac(struct cpu_hilos *c, int encender) {
    if (encender == c->tictac || !c->apic_por_ms) return;
    c->tictac = encender;
    if (encender) {
        c->tsc_ultimo_tic = 0;
        apic_escribir(LAPIC_TIMER_DIVISOR, 3);
        apic_escribir(LAPIC_LVT_TIMER, VECTOR_TICTAC | LVT_PERIODICO);
        apic_escribir(LAPIC_TIMER_INICIAL, (uint32_t)c->apic_por_ms);
    } else {
        apic_escribir(LAPIC_TIMER_INICIAL, 0);
        apic_escribir(LAPIC_LVT_TIMER, LVT_ENMASCARADO);
    }
}

/* Interrupcion del tictac, en la CPU que la recibe. Despierta a los dormidos que ya tocan y pide el cambio si el hilo
 * en curso agoto su turno (1 tictac) o hay uno mas urgente. El cambio se hace en hilos_tras_irq. */
static void tic(struct marco_interrupcion *m) {
    (void)m;
    unsigned yo = cpu_actual();
    if (!yo || yo >= HILOS_CPUS) return;
    struct cpu_hilos *c = &g_cpu[yo];
    uint64_t ahora = tsc();
    if (c->tsc_ultimo_tic) {
        uint64_t d = ahora - c->tsc_ultimo_tic, nominal = trabajos_ticks_por_ms(yo);
        uint64_t desvio = d > nominal ? d - nominal : nominal - d;
        if (nominal && desvio > c->desvio_max) c->desvio_max = desvio;
    }
    c->tsc_ultimo_tic = ahora;
    c->tics++;
    uint64_t f = cerrojo_tomar_irq(&c->cerrojo);
    hilo_t **p = &c->dormidos;
    while (*p) {
        hilo_t *h = *p;
        if (ahora >= h->despertar_tsc) { *p = h->siguiente; h->estado = HILO_LISTO; encolar(c, h); }
        else p = &h->siguiente;
    }
    if (c->actual) {
        int otro = 0;
        for (unsigned q = 0; q < HILO_PRIORIDADES; q++) if (c->listos[q]) otro = 1;
        if (otro) c->cambio_pedido = 1;
    }
    cerrojo_soltar_irq(&c->cerrojo, f);
}

void hilos_cpu_iniciar(unsigned cpu) {
    if (!cpu || cpu >= HILOS_CPUS) return;
    struct cpu_hilos *c = &g_cpu[cpu];
    c->cerrojo.nombre = "hilos";
    /* Calibrar el temporizador APIC de esta CPU contra su TSC: divisor 16, 10 ms */
    apic_escribir(LAPIC_TIMER_DIVISOR, 3);
    apic_escribir(LAPIC_LVT_TIMER, LVT_ENMASCARADO);
    apic_escribir(LAPIC_TIMER_INICIAL, 0xFFFFFFFFu);
    uint64_t por_ms = trabajos_ticks_por_ms(cpu), t0 = tsc();
    if (!por_ms) por_ms = tiempo_ciclos_por_ms();
    while (tsc() - t0 < por_ms * 10u) __asm__ volatile("pause");
    uint32_t actual = apic_leer(LAPIC_TIMER_ACTUAL);
    apic_escribir(LAPIC_TIMER_INICIAL, 0);
    c->apic_por_ms = (0xFFFFFFFFu - actual) / 10u;
    c->lista = c->apic_por_ms != 0;
    if (!__atomic_exchange_n(&g_vector_listo, 1, __ATOMIC_ACQ_REL)) idt_registrar_manejador(VECTOR_TICTAC, tic);
}

/* ------------------------------------------------------------------ cambio de contexto */

hilo_t *hilo_actual(void) {
    unsigned yo = cpu_actual();
    return yo && yo < HILOS_CPUS ? g_cpu[yo].actual : 0;
}

/* Del hilo en curso a la raiz de su CPU. Con las interrupciones apagadas; vuelve cuando la raiz lo elija otra vez. */
static void a_la_raiz(struct cpu_hilos *c, hilo_t *h) {
    i915_cambiar_contexto(&h->rsp, c->raiz_rsp, 0, 0);
}

static void liberar(hilo_t *h) {
    if (h->tls) taek_tls_destruir(h->tls);
    liberar_memoria(h->pila);
    liberar_memoria(h);
}

int hilos_raiz(unsigned cpu) {
    if (!cpu || cpu >= HILOS_CPUS) return 0;
    struct cpu_hilos *c = &g_cpu[cpu];
    cerrojo_tomar(&c->cerrojo);
    hilo_t *h = sacar(c);
    if (!h) {
        /* H207: sin nada listo ni dormido no hace falta tictac, aunque haya hilos bloqueados (el de lotes casi
         * siempre lo esta): los despierta una senal con su IPI, no el reloj */
        if (!c->dormidos) tictac(c, 0);
        cerrojo_soltar(&c->cerrojo);
        return 0;
    }
    tictac(c, 1);
    c->actual = h;
    c->cambio_pedido = 0;
    c->cambios++;
    h->estado = HILO_EJECUTANDO;
    h->turnos++;
    cerrojo_soltar(&c->cerrojo);
    /* H213: un hilo de programa recibe sus interrupciones y llamadas desde Anillo 3 en su propia pila */
    if (h->rsp0) gdt_fijar_rsp0(cpu, h->rsp0);

    i915_cambiar_contexto(&c->raiz_rsp, h->rsp, 0, 0);

    /* El hilo volvio: lo dejo como corresponde a su estado */
    cerrojo_tomar(&c->cerrojo);
    c->actual = 0;
    int muerto = 0;
    if (h->estado == HILO_CEDIDO) { h->estado = HILO_LISTO; encolar(c, h); }
    else if (h->estado == HILO_MUERTO) { muerto = 1; if (c->hilos) c->hilos--; }
    /* DORMIDO y BLOQUEADO ya estan en su lista; LISTO ya lo encolo quien lo desperto */
    cerrojo_soltar(&c->cerrojo);
    if (muerto) liberar(h);
    return 1;
}

void hilos_tras_irq(int puede_cambiar) {
    unsigned yo = cpu_actual();
    if (!yo || yo >= HILOS_CPUS || !puede_cambiar) return;
    struct cpu_hilos *c = &g_cpu[yo];
    hilo_t *h = c->actual;
    if (!h) return;
    /* Un lote de video publicado manda: la raiz lo atiende antes de seguir con los hilos */
    if (!c->cambio_pedido && !trabajos_pendiente_en(yo)) return;
    if (cerrojo_en_mano(yo) || idt_en_interrupcion()) return;
    c->cambio_pedido = 0;
    c->desalojos++;
    h->desalojos++;
    h->estado = HILO_CEDIDO;
    a_la_raiz(c, h);
}

/* ------------------------------------------------------------------ crear y terminar */

__attribute__((naked, used)) static void trampolin(void) {
    __asm__ volatile("movq %r12, %rdi\n\tcall hilo_arrancar\n\tud2\n\t");
}
__attribute__((used, noreturn)) void hilo_arrancar(hilo_t *h) {
    h->fn(h->arg);
    hilo_salir();
}

hilo_t *hilo_crear(void (*fn)(void *), void *arg, const char *nombre, unsigned cpu, unsigned prioridad, size_t pila_bytes) {
    if (!fn || !cpu || cpu >= HILOS_CPUS || cpu >= trabajos_cpu_arrancadas() || !g_cpu[cpu].lista) return 0;
    if (!pila_bytes) pila_bytes = PILA_POR_OMISION;
    hilo_t *h = asignar_memoria_cero(sizeof(*h));
    void *pila = h ? asignar_memoria(pila_bytes) : 0;
    void *tls = pila ? taek_tls_crear() : 0;
    struct i915_contexto_marco *m = tls ? i915_contexto_preparar(pila, pila_bytes, trampolin, (uint64_t)(uintptr_t)h) : 0;
    if (!m) {
        if (tls) taek_tls_destruir(tls);
        if (pila) liberar_memoria(pila);
        if (h) liberar_memoria(h);
        return 0;
    }
    m->irq_flags = 0x202;          /* Un hilo nuevo empieza con las interrupciones encendidas */
    m->fs_base = (uint64_t)(uintptr_t)tls;
    h->rsp = m; h->pila = pila; h->pila_bytes = pila_bytes; h->tls = tls;
    h->fn = fn; h->arg = arg; h->nombre = nombre;
    h->id = __atomic_fetch_add(&g_siguiente_id, 1, __ATOMIC_RELAXED);
    h->cpu = (uint8_t)cpu; h->prioridad = (uint8_t)(prioridad < HILO_PRIORIDADES ? prioridad : HILO_NORMAL);
    struct cpu_hilos *c = &g_cpu[cpu];
    uint64_t f = cerrojo_tomar_irq(&c->cerrojo);
    c->hilos++; c->creados++;
    h->estado = HILO_LISTO;
    encolar(c, h);
    if (c->actual && hay_mas_urgente(c, c->actual->prioridad)) c->cambio_pedido = 1;
    cerrojo_soltar_irq(&c->cerrojo, f);
    trabajos_ipi_cpu(cpu);
    return h;
}

void hilo_salir(void) {
    hilo_t *h = hilo_actual();
    if (!h) for (;;) __asm__ volatile("cli; hlt");
    apagar_irq();
    h->estado = HILO_MUERTO;
    a_la_raiz(&g_cpu[h->cpu], h);
    for (;;) __asm__ volatile("cli; hlt");   /* La raiz no vuelve a un hilo muerto */
}

void hilo_ceder(void) {
    hilo_t *h = hilo_actual();
    if (!h) return;
    uint64_t f = apagar_irq();
    h->estado = HILO_CEDIDO;
    a_la_raiz(&g_cpu[h->cpu], h);
    restaurar_irq(f);
}

int hilo_dormir_ms(uint32_t ms) {
    hilo_t *h = hilo_actual();
    if (!h) return -1;
    struct cpu_hilos *c = &g_cpu[h->cpu];
    uint64_t f = apagar_irq();
    cerrojo_tomar(&c->cerrojo);
    h->despertar_tsc = tsc() + (uint64_t)ms * trabajos_ticks_por_ms(h->cpu);
    h->estado = HILO_DORMIDO;
    h->siguiente = c->dormidos;
    c->dormidos = h;
    cerrojo_soltar(&c->cerrojo);
    a_la_raiz(c, h);
    restaurar_irq(f);
    return 0;
}

/* ------------------------------------------------------------------ eventos y mutex */

static void despertar(hilo_t *h) {
    struct cpu_hilos *c = &g_cpu[h->cpu];
    uint64_t f = cerrojo_tomar_irq(&c->cerrojo);
    h->estado = HILO_LISTO;
    encolar(c, h);
    if (c->actual && hay_mas_urgente(c, c->actual->prioridad)) c->cambio_pedido = 1;
    cerrojo_soltar_irq(&c->cerrojo, f);
    if (cpu_actual() != h->cpu) trabajos_ipi_cpu(h->cpu);
}

int hilo_esperar(evento_t *e) {
    hilo_t *h = hilo_actual();
    if (!h || !e) return -1;
    uint64_t f = apagar_irq();
    cerrojo_tomar(&e->cerrojo);
    if (e->senales) { e->senales--; cerrojo_soltar(&e->cerrojo); restaurar_irq(f); return 0; }
    h->estado = HILO_BLOQUEADO;
    h->siguiente = 0;
    hilo_t **p = &e->esperando;
    while (*p) p = &(*p)->siguiente;
    *p = h;
    cerrojo_soltar(&e->cerrojo);
    /* Si alguien senala entre soltar el cerrojo y cambiar, el hilo queda LISTO y encolado; la raiz de esta CPU no lo
     * elige hasta que este cambio termine, porque es la unica que saca de esa cola. */
    a_la_raiz(&g_cpu[h->cpu], h);
    restaurar_irq(f);
    return 0;
}

void evento_senalar(evento_t *e) {
    if (!e) return;
    uint64_t f = cerrojo_tomar_irq(&e->cerrojo);
    hilo_t *h = e->esperando;
    if (h) e->esperando = h->siguiente; else e->senales++;
    cerrojo_soltar_irq(&e->cerrojo, f);
    if (h) despertar(h);
}

int mutex_tomar(mutex_t *m) {
    hilo_t *h = hilo_actual();
    if (!h || !m) return -1;
    for (;;) {
        uint64_t f = cerrojo_tomar_irq(&m->evento.cerrojo);
        if (!m->dueno) { m->dueno = h; m->evento.senales = 0; cerrojo_soltar_irq(&m->evento.cerrojo, f); return 0; }
        cerrojo_soltar_irq(&m->evento.cerrojo, f);
        hilo_esperar(&m->evento);
    }
}

void mutex_soltar(mutex_t *m) {
    if (!m) return;
    uint64_t f = cerrojo_tomar_irq(&m->evento.cerrojo);
    m->dueno = 0;
    cerrojo_soltar_irq(&m->evento.cerrojo, f);
    evento_senalar(&m->evento);
}

/* ------------------------------------------------------------------ estadisticas */

void hilos_estadisticas(unsigned cpu, struct hilos_estadisticas *e) {
    if (!e || cpu >= HILOS_CPUS) return;
    struct cpu_hilos *c = &g_cpu[cpu];
    e->tics = c->tics; e->cambios = c->cambios; e->desalojos = c->desalojos; e->vivos = c->hilos; e->creados = c->creados;
    e->tic_desvio_max_ciclos = c->desvio_max; e->apic_por_ms = c->apic_por_ms; e->tictac = c->tictac;
}
void hilos_cero(void) {
    for (unsigned i = 0; i < HILOS_CPUS; i++) {
        g_cpu[i].tics = g_cpu[i].cambios = g_cpu[i].desalojos = g_cpu[i].desvio_max = 0;
    }
}
