#include "atencion.h"
#include "tiempo.h"
#include "trabajos.h"

struct atencion_medida atencion_usb = { .max_sys_ini = ATENCION_SIN_SYS, .max_sys_fin = ATENCION_SIN_SYS,
                                        .ultimo_sys = ATENCION_SIN_SYS, .que = ATENCION_USB };
struct atencion_medida atencion_red = { .max_sys_ini = ATENCION_SIN_SYS, .max_sys_fin = ATENCION_SIN_SYS,
                                        .ultimo_sys = ATENCION_SIN_SYS, .que = ATENCION_RED };
volatile uint32_t atencion_sys_actual = ATENCION_SIN_SYS;
uint32_t atencion_sys_max_us[ATENCION_SYS_MAX];
struct atencion_hueco atencion_huecos[ATENCION_HUECOS];
uint32_t atencion_huecos_total;

void atencion_marcar(struct atencion_medida *m) {
    uint64_t ahora = tiempo_obtener_milisegundos();
    uint32_t sys = atencion_sys_actual;
    if (m->ultimo_ms && ahora > m->ultimo_ms) {
        uint64_t hueco = ahora - m->ultimo_ms;
        uint32_t ms = hueco > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)hueco;
        if (ms > m->max_ms) {
            m->max_ms = ms;
            m->max_sys_ini = (uint16_t)m->ultimo_sys;
            m->max_sys_fin = (uint16_t)sys;
            m->max_cuando_ms = ahora;
        }
        if (ms >= ATENCION_HUECO_MIN_MS) {
            struct atencion_hueco *h = &atencion_huecos[atencion_huecos_total % ATENCION_HUECOS];
            h->fin_ms = ahora; h->ms = ms; h->que = (uint8_t)m->que;
            h->sys_ini = (uint8_t)m->ultimo_sys; h->sys_fin = (uint8_t)sys; h->reservado = 0;
            atencion_huecos_total++;
        }
    }
    m->ultimo_ms = ahora;
    m->ultimo_sys = sys;
    m->pasadas++;
}

unsigned atencion_huecos_copiar(struct atencion_hueco *out, unsigned max) {
    unsigned n = atencion_huecos_total < ATENCION_HUECOS ? atencion_huecos_total : ATENCION_HUECOS;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; i++) out[i] = atencion_huecos[(atencion_huecos_total - n + i) % ATENCION_HUECOS];
    return n;
}

void atencion_sys_terminar(uint32_t numero, uint64_t ciclos) {
    uint64_t por_ms = tiempo_ciclos_por_ms();
    if (numero >= ATENCION_SYS_MAX || !por_ms) return;
    uint64_t us = ciclos * 1000u / por_ms;
    if (us > 0xFFFFFFFFu) us = 0xFFFFFFFFu;
    if (us > atencion_sys_max_us[numero]) atencion_sys_max_us[numero] = (uint32_t)us;
}

static void medida_cero(struct atencion_medida *m) {
    m->pasadas = 0; m->max_ms = 0; m->max_cuando_ms = 0;
    m->max_sys_ini = ATENCION_SIN_SYS; m->max_sys_fin = ATENCION_SIN_SYS;
}

/* H215, Fase E1 */
volatile uint64_t atencion_cpu0_espera;
uint32_t atencion_espera_anidada;
static struct { uint64_t tsc, espera, ocupado[4]; } g_base;

static void carga_base(void) {
    g_base.tsc = atencion_tsc();
    g_base.espera = atencion_cpu0_espera;
    for (unsigned i = 1; i < 4; i++) g_base.ocupado[i] = trabajos_ocupado_ciclos(i);
}

void atencion_carga(struct atencion_carga *c) {
    if (!g_base.tsc) carga_base();   /* Sin cero previo: desde la primera consulta */
    uint64_t dt = atencion_tsc() - g_base.tsc, por_ms = tiempo_ciclos_por_ms();
    c->cpus = trabajos_cpu_arrancadas();
    c->ms = por_ms ? dt / por_ms : 0;
    uint64_t esp = atencion_cpu0_espera - g_base.espera;
    if (esp > dt) esp = dt;
    c->cpu0_espera = dt ? (uint32_t)(esp * 1000u / dt) : 0;
    c->cpu0_trabajo = 1000u - c->cpu0_espera;
    c->ocupado[0] = c->cpu0_trabajo;
    for (unsigned i = 1; i < 4; i++) {
        uint64_t o = i < c->cpus ? trabajos_ocupado_ciclos(i) - g_base.ocupado[i] : 0;
        if (o > dt) o = dt;
        c->ocupado[i] = dt ? (uint32_t)(o * 1000u / dt) : 0;
    }
}

uint64_t atencion_cero_ms;
void atencion_cero(void) {
    medida_cero(&atencion_usb);
    medida_cero(&atencion_red);
    for (unsigned i = 0; i < ATENCION_SYS_MAX; i++) atencion_sys_max_us[i] = 0;
    atencion_cero_ms = tiempo_obtener_milisegundos();
    carga_base();
}

/* El orden es el de arquitectura/x86_64/anillo3_abi.h (SYS_SALIR = 0 ... SYS_FUTEX = 27) */
const char *atencion_sys_nombre(uint32_t numero) {
    static const char *const n[] = { "SALIR", "ESCRIBIR", "TIEMPO", "CEDER", "SUPERFICIE", "PRESENTAR", "MOVER",
        "EVENTO", "PANTALLA", "ESPERAR", "INFO", "CAPTURA", "EQUIPO", "ELEVAR", "WEB", "MEMORIA", "ARCHIVOS",
        "LANZAR", "ARGUMENTO", "AUDIO", "TAMANO", "SENSORES", "ORDEN", "RED", "VIDEO", "FLUJO", "HILO", "FUTEX" };
    if (numero == ATENCION_SIN_SYS) return "sin SYS";
    return numero < sizeof(n) / sizeof(n[0]) ? n[numero] : "?";
}
