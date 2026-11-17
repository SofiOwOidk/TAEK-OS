#include "cerrojo.h"
#include "paginacion.h"

/* ------------------------------------------------------------------ identidad de la CPU */

/* IA32_TSC_AUX (0xC0000103) guarda el indice de cada CPU y rdtscp lo devuelve en ECX sin tocar memoria ni
 * serializar. Sin RDTSCP (CPUID 8000_0001h EDX bit 27) se busca el APIC ID inicial (CPUID 1 EBX 31:24) en una tabla. */
static int g_rdtscp = -1;
static uint8_t g_apic_de[CERROJO_CPUS];
static uint8_t g_apic_puesto[CERROJO_CPUS];

static uint8_t apic_inicial(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (uint8_t)(b >> 24);
}

void cpu_identidad_iniciar(unsigned indice) {
    if (indice >= CERROJO_CPUS) return;
    if (g_rdtscp < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
        int hay = 0;
        if (a >= 0x80000001u) {
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u), "c"(0));
            hay = (d >> 27) & 1;
        }
        g_rdtscp = hay;
    }
    g_apic_de[indice] = apic_inicial();
    g_apic_puesto[indice] = 1;
    if (g_rdtscp > 0)
        __asm__ volatile("wrmsr" :: "c"(0xC0000103u), "a"((uint32_t)indice), "d"(0u) : "memory");
}

unsigned cpu_actual(void) {
    if (g_rdtscp > 0) {
        uint32_t a, d, c;
        __asm__ volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));
        return c < CERROJO_CPUS ? c : 0;
    }
    if (g_rdtscp < 0) return 0;   /* Arranque muy temprano: solo existe el BSP */
    uint8_t yo = apic_inicial();
    for (unsigned i = 0; i < CERROJO_CPUS; i++) if (g_apic_puesto[i] && g_apic_de[i] == yo) return i;
    return 0;
}

/* ------------------------------------------------------------------ cerrojo */

static inline uint64_t tsc(void) {
    uint32_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}
static inline uint64_t apagar_irq(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void restaurar_irq(uint64_t f) {
    if (f & 0x200u) __asm__ volatile("sti" ::: "memory");
}

static cerrojo_t *g_lista;
static volatile uint8_t g_lista_ocupada;
static void registrar(cerrojo_t *c) {
    while (__atomic_test_and_set(&g_lista_ocupada, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    if (!c->registrado) { c->registrado = 1; c->siguiente = g_lista; g_lista = c; }
    __atomic_clear(&g_lista_ocupada, __ATOMIC_RELEASE);
}
const cerrojo_t *cerrojo_primero(void) { return g_lista; }
void cerrojo_cero(void) {
    for (cerrojo_t *c = g_lista; c; c = c->siguiente) { c->max_ciclos = 0; c->max_desde = 0; c->tomas = 0; c->disputas = 0; }
}

/* Cuantos cerrojos tiene tomados cada CPU (niveles exteriores). El dueno de un cerrojo es la CPU, no el hilo: por
 * eso el planificador (Fase A) no puede cambiar de hilo en una CPU que tiene alguno en la mano. */
static volatile uint32_t g_en_mano[CERROJO_CPUS];
unsigned cerrojo_en_mano(unsigned cpu) { return cpu < CERROJO_CPUS ? g_en_mano[cpu] : 0; }

/* Con las interrupciones apagadas. Devuelve 1 si lo tomo o ya era suyo. */
static int intentar(cerrojo_t *c, uint32_t yo, uint64_t desde) {
    if (__atomic_load_n(&c->dueno, __ATOMIC_RELAXED) == yo) { c->profundidad++; return 1; }
    uint32_t libre = 0;
    if (!__atomic_compare_exchange_n(&c->dueno, &libre, yo, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    c->profundidad = 1;
    c->tomas++;
    c->t_tomado = tsc();
    c->desde = desde;
    g_en_mano[yo - 1]++;
    return 1;
}
/* Con las interrupciones apagadas */
static void dejar(cerrojo_t *c) {
    if (--c->profundidad) return;
    uint64_t d = tsc() - c->t_tomado;
    if (d > c->max_ciclos) { c->max_ciclos = d; c->max_desde = c->desde; }
    uint32_t yo = c->dueno;
    if (yo && yo <= CERROJO_CPUS && g_en_mano[yo - 1]) g_en_mano[yo - 1]--;
    __atomic_store_n(&c->dueno, 0, __ATOMIC_RELEASE);
}

uint64_t cerrojo_tomar_irq(cerrojo_t *c) {
    uint64_t f = apagar_irq();
    uint32_t yo = cpu_actual() + 1;
    uint64_t desde = (uint64_t)(uintptr_t)__builtin_return_address(0);
    int espero = 0;
    while (!intentar(c, yo, desde)) {
        espero = 1;
        paginacion_tlb_atender();   /* Con IF apagado no llega la IPI: se atiende a mano */
        __asm__ volatile("pause");
    }
    if (espero) c->disputas++;
    if (!c->registrado) registrar(c);
    return f;
}
void cerrojo_soltar_irq(cerrojo_t *c, uint64_t rflags) {
    dejar(c);
    restaurar_irq(rflags);
}

/* Sin tocar IF fuera de la toma: se apaga solo el instante de tomar o soltar, para que una interrupcion de esta
 * misma CPU nunca vea el cerrojo a medio tomar. Mientras espera a otra CPU, las interrupciones siguen entrando. */
void cerrojo_tomar(cerrojo_t *c) {
    int espero = 0;
    uint64_t desde = (uint64_t)(uintptr_t)__builtin_return_address(0);
    for (;;) {
        uint64_t f = apagar_irq();
        int ok = intentar(c, cpu_actual() + 1, desde);
        if (ok && espero) c->disputas++;
        if (ok && !c->registrado) registrar(c);
        restaurar_irq(f);
        if (ok) return;
        espero = 1;
        paginacion_tlb_atender();
        __asm__ volatile("pause");
    }
}
/* H211: solo para el informe de un colapso (el huevo), que termina en reinicio. Espera hasta ms a que otra CPU lo
 * suelte; si no, se lo queda igual. El dueno anterior no se entera: despues de esto no hay vuelta atras. */
void cerrojo_forzar(cerrojo_t *c, uint32_t ms) {
    uint32_t yo = cpu_actual() + 1;
    const uint64_t por_ms = 2000000u;   /* Sin calibrar a proposito: es un plazo de cortesia en pleno colapso */
    uint64_t t0 = tsc();
    for (;;) {
        uint32_t libre = 0;
        if (__atomic_load_n(&c->dueno, __ATOMIC_ACQUIRE) == yo) return;
        if (__atomic_compare_exchange_n(&c->dueno, &libre, yo, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { c->profundidad = 1; return; }
        if (tsc() - t0 > (uint64_t)ms * por_ms) break;
        __asm__ volatile("pause");
    }
    __atomic_store_n(&c->dueno, yo, __ATOMIC_RELEASE);
    c->profundidad = 1;
}

/* H208: tomarlo solo si esta libre (o ya es de esta CPU). Para quien prefiere volver mas tarde que esperar. */
int cerrojo_intentar(cerrojo_t *c) {
    uint64_t f = apagar_irq();
    int ok = intentar(c, cpu_actual() + 1, (uint64_t)(uintptr_t)__builtin_return_address(0));
    if (ok && !c->registrado) registrar(c);
    restaurar_irq(f);
    return ok;
}
void cerrojo_soltar(cerrojo_t *c) {
    uint64_t f = apagar_irq();
    dejar(c);
    restaurar_irq(f);
}
