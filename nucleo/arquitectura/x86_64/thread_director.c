/* H215, CONGELADO: Intel Thread Director / HFI. Ver thread_director.h.
 *
 * Fuentes del formato: Intel SDM vol. 3, "Hardware Feedback Interface and Intel Thread Director", y como lo lee Linux
 * (drivers/thermal/intel/intel_hfi.c): cabecera de 8 bytes (marca de tiempo) + por clase 2 bytes de "cambio",
 * redondeado a 8; despues una fila por CPU logica con 2 bytes por clase (rendimiento, eficiencia), tambien a 8. */
#include "thread_director.h"
#include "../../base/hilos.h"
#include "../../base/trabajos.h"
#include "../../base/tiempo.h"
#include "../../base/dma.h"
#include "../../controladores/consola.h"

#define MSR_HW_FEEDBACK_PTR     0x17D0u
#define MSR_HW_FEEDBACK_CONFIG  0x17D1u
#define MSR_PACKAGE_THERM_STATUS 0x1B1u
#define THERM_HFI_ACTUALIZADA   (UINT64_C(1) << 26)
#define THERM_CLEAR_PKG_MASK    ((1u << 1) | (1u << 3) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 11) | (1u << 26))

static struct td_estado g_td;
static uint64_t g_td_actualizaciones;

static inline void cpuid(uint32_t hoja, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(hoja), "c"(sub));
}
static inline uint64_t leer_msr(uint32_t m) { uint32_t a, d; __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(m)); return ((uint64_t)d << 32) | a; }
static inline void escribir_msr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory"); }

/* Lo que cada CPU dice de si misma */
static void leer_esta_cpu(struct td_cpu *o) {
    uint32_t a, b, c, d, max;
    cpuid(0, 0, &max, &b, &c, &d);
    o->tipo = 0; o->fila = 0;
    if (max >= 6) { cpuid(6, 0, &a, &b, &c, &d); o->fila = (uint16_t)(d >> 16); }
    if (max >= 0x1A) { cpuid(0x1A, 0, &a, &b, &c, &d); o->tipo = (uint8_t)(a >> 24); }
    __atomic_store_n(&o->visto, 1, __ATOMIC_RELEASE);
}
static void hilo_cpuid(void *p) { leer_esta_cpu((struct td_cpu *)p); }

const struct td_estado *thread_director_detectar(void) {
    uint32_t a, b, c, d;
    cpuid(0, 0, &g_td.cpuid_max, &b, &c, &d);
    g_td.hibrido = g_td.hfi = g_td.itd = 0; g_td.clases = 1; g_td.capacidades = 0; g_td.paginas = 0;
    if (g_td.cpuid_max >= 7) { cpuid(7, 0, &a, &b, &c, &d); g_td.hibrido = (d >> 15) & 1; }
    if (g_td.cpuid_max >= 6) {
        cpuid(6, 0, &a, &b, &c, &d);
        g_td.hfi = (a >> 19) & 1;
        g_td.itd = (a >> 23) & 1;
        if (g_td.itd) g_td.clases = (c >> 8) & 0xFF;
        if (!g_td.clases) g_td.clases = 1;
        g_td.capacidades = (uint8_t)d;
        g_td.paginas = ((d >> 8) & 0xF) + 1;
    }
    /* Cada CPU auxiliar contesta desde un hilo corto propio: el tipo de nucleo y la fila son de cada una */
    unsigned n = trabajos_cpu_arrancadas();
    if (n > TD_CPUS) n = TD_CPUS;
    for (unsigned i = 0; i < TD_CPUS; i++) g_td.cpu[i].visto = 0;
    leer_esta_cpu(&g_td.cpu[0]);
    for (unsigned i = 1; i < n; i++) hilo_crear(hilo_cpuid, &g_td.cpu[i], "td_cpuid", i, HILO_NORMAL, 0);
    uint64_t t0 = tiempo_obtener_milisegundos();
    for (unsigned i = 1; i < n; i++)
        while (!__atomic_load_n(&g_td.cpu[i].visto, __ATOMIC_ACQUIRE) && tiempo_obtener_milisegundos() - t0 < 200)
            __asm__ volatile("pause");
    g_td.detectado = 1;
    return &g_td;
}

static unsigned redondear8(unsigned x) { return (x + 7u) & ~7u; }

void thread_director_leer_fila(const uint8_t *tabla, unsigned clases, unsigned fila, unsigned clase,
                               uint8_t *rendimiento, uint8_t *eficiencia) {
    unsigned cabecera = 8u + redondear8(clases * 2u), paso = redondear8(clases * 2u);
    const volatile uint8_t *p = tabla + cabecera + (uint64_t)fila * paso + clase * 2u;
    if (rendimiento) *rendimiento = p[0];
    if (eficiencia) *eficiencia = p[1];
}

int thread_director_encender(void) {
    if (!g_td.detectado) thread_director_detectar();
    if (!g_td.hfi) return -1;
    if (g_td.activo) return 0;
    if (!g_td.tabla) {
        uint64_t fisica = 0;
        void *v = dma_asignar_bufer_contiguo((uint64_t)g_td.paginas * 4096u, 4096u, &fisica);
        if (!v || !fisica) return -2;
        for (unsigned i = 0; i < g_td.paginas * 4096u; i++) ((volatile uint8_t *)v)[i] = 0;
        g_td.tabla = v; g_td.tabla_fisica = fisica;   /* No se devuelve nunca: el hardware puede seguir escribiendo */
    }
    escribir_msr(MSR_HW_FEEDBACK_PTR, g_td.tabla_fisica | 1u);
    escribir_msr(MSR_HW_FEEDBACK_CONFIG, leer_msr(MSR_HW_FEEDBACK_CONFIG) | 1u);   /* Solo HFI; sin clasificar hilos */
    g_td.activo = 1;
    return 0;
}

void thread_director_apagar(void) {
    if (!g_td.activo) return;
    escribir_msr(MSR_HW_FEEDBACK_CONFIG, leer_msr(MSR_HW_FEEDBACK_CONFIG) & ~UINT64_C(3));
    g_td.activo = 0;
}

/* El hardware avisa con el bit 26 de IA32_PACKAGE_THERM_STATUS que reescribio la tabla; se borra como Linux */
static void atender_actualizacion(void) {
    if (!g_td.activo) return;
    uint64_t v = leer_msr(MSR_PACKAGE_THERM_STATUS);
    if (!(v & THERM_HFI_ACTUALIZADA)) return;
    g_td_actualizaciones++;
    escribir_msr(MSR_PACKAGE_THERM_STATUS, v & THERM_CLEAR_PKG_MASK & ~THERM_HFI_ACTUALIZADA);
}

int thread_director_rendimiento(uint8_t *perf, unsigned n) {
    if (!g_td.activo || !g_td.tabla) return 0;
    atender_actualizacion();
    for (unsigned i = 0; i < n && i < TD_CPUS; i++)
        thread_director_leer_fila(g_td.tabla, g_td.clases, g_td.cpu[i].fila, 0, &perf[i], 0);
    return 1;
}

unsigned thread_director_elegir(const uint8_t *perf, const unsigned *hilos, unsigned n) {
    unsigned mejor = 0, puntos_mejor = 0;
    for (unsigned i = 1; i < n && i < TD_CPUS; i++) {
        unsigned puntos = (unsigned)perf[i] * 16u / (1u + hilos[i]);   /* Rendimiento 0 = no usar esa CPU */
        if (puntos > puntos_mejor) { puntos_mejor = puntos; mejor = i; }
    }
    return mejor;
}

/* Prueba sin hardware: una tabla armada a mano con el formato de Linux. Comprueba la lectura y la eleccion. */
static int prueba(void) {
    static uint8_t t[4096];
    int fallos = 0;
    for (unsigned clases = 1; clases <= 4; clases += 3) {
        for (unsigned i = 0; i < sizeof(t); i++) t[i] = 0xEE;   /* Basura: si se lee donde no es, se nota */
        unsigned cab = 8u + redondear8(clases * 2u), paso = redondear8(clases * 2u);
        static const uint8_t r[4] = { 100, 255, 128, 64 }, e[4] = { 10, 20, 30, 40 };
        for (unsigned f = 0; f < 4; f++) for (unsigned k = 0; k < clases; k++) {
            t[cab + f * paso + k * 2u] = (uint8_t)(r[f] - k); t[cab + f * paso + k * 2u + 1] = (uint8_t)(e[f] + k);
        }
        for (unsigned f = 0; f < 4; f++) for (unsigned k = 0; k < clases; k++) {
            uint8_t rr, ee; thread_director_leer_fila(t, clases, f, k, &rr, &ee);
            if (rr != (uint8_t)(r[f] - k) || ee != (uint8_t)(e[f] + k)) fallos++;
        }
    }
    static const uint8_t perf[4] = { 100, 255, 128, 64 };
    unsigned h0[4] = { 0, 0, 0, 0 }, h1[4] = { 0, 3, 0, 0 }, h2[4] = { 0, 0, 0, 0 };
    static const uint8_t perf_cero[4] = { 255, 0, 0, 0 };
    if (thread_director_elegir(perf, h0, 4) != 1) fallos++;        /* La de mas rendimiento */
    if (thread_director_elegir(perf, h1, 4) != 2) fallos++;        /* 255/4 < 128: reparte */
    if (thread_director_elegir(perf_cero, h2, 4) != 0) fallos++;   /* Ninguna auxiliar sirve: 0 = sin eleccion */
    return fallos;
}

void thread_director_comando(const char *arg) {
    while (arg && *arg == ' ') arg++;
    if (arg && arg[0] == 'p') {   /* prueba */
        int f = prueba();
        consola_imprimir("[TD] Prueba sin hardware (tabla HFI armada a mano, formato de Linux): ");
        consola_imprimir_dec((uint64_t)f); consola_imprimir_linea(f ? " fallos. RESULTADO=FALLO" : " fallos. RESULTADO=PASS");
        return;
    }
    const struct td_estado *e = thread_director_detectar();
    if (arg && arg[0] == 'e') {
        int r = thread_director_encender();
        consola_imprimir_linea(r == -1 ? "[TD] Este procesador no tiene HFI (CPUID.06H:EAX[19] = 0): nada que encender."
                             : r ? "[TD] Sin memoria para la tabla HFI." : "[TD] HFI encendida (CONGELADO: sin probar en hardware).");
    } else if (arg && arg[0] == 'a') {
        thread_director_apagar();
        consola_imprimir_linea("[TD] Apagado.");
    }
    consola_imprimir_linea("--- INTEL THREAD DIRECTOR (H215, CONGELADO) ---");
    consola_imprimir("  Hibrido (CPUID.07H:EDX[15]) "); consola_imprimir(e->hibrido ? "si" : "no");
    consola_imprimir(" | HFI (CPUID.06H:EAX[19]) "); consola_imprimir(e->hfi ? "si" : "no");
    consola_imprimir(" | Thread Director (EAX[23]) "); consola_imprimir(e->itd ? "si" : "no");
    consola_imprimir(" | CPUID max "); consola_imprimir_hex(e->cpuid_max); consola_imprimir_linea("");
    if (e->hfi) {
        consola_imprimir("  Clases "); consola_imprimir_dec(e->clases);
        consola_imprimir(" | capacidades "); consola_imprimir_dec(e->capacidades);
        consola_imprimir(" | tabla de "); consola_imprimir_dec(e->paginas);
        consola_imprimir(" pagina(s) | "); consola_imprimir_linea(e->activo ? "ENCENDIDA" : "apagada");
    }
    unsigned n = trabajos_cpu_arrancadas();
    uint8_t perf[TD_CPUS] = { 0 };
    int con_tabla = thread_director_rendimiento(perf, n < TD_CPUS ? n : TD_CPUS);
    for (unsigned i = 0; i < n && i < TD_CPUS; i++) {
        consola_imprimir("  CPU "); consola_imprimir_dec(i); consola_imprimir(": ");
        if (!e->cpu[i].visto) { consola_imprimir_linea("no contesto"); continue; }
        consola_imprimir(e->cpu[i].tipo == 0x40 ? "nucleo P" : e->cpu[i].tipo == 0x20 ? "nucleo E" :
                         e->cpu[i].tipo ? "tipo desconocido" : "sin tipo (no hibrido)");
        if (e->hfi) { consola_imprimir(" | fila HFI "); consola_imprimir_dec(e->cpu[i].fila); }
        if (con_tabla) { consola_imprimir(" | rendimiento "); consola_imprimir_dec(perf[i]); }
        consola_imprimir_linea("");
    }
    if (!e->hfi) consola_imprimir_linea("  No disponible en este procesador: los hilos de programa siguen repartidos por turno.");
    else if (g_td_actualizaciones) { consola_imprimir("  Actualizaciones de la tabla: "); consola_imprimir_dec(g_td_actualizaciones); consola_imprimir_linea(""); }
}
