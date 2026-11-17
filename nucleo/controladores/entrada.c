#include "entrada.h"
#include "ps2.h"
#include "hid_i2c.h"
#include "../base/hilos.h"

cerrojo_t g_cerrojo_entrada = CERROJO("entrada");

static evento_t g_ev = EVENTO("entrada_hilo");
static hilo_t *g_hilo;
static volatile int g_modo;
static uint64_t g_pasadas;

int entrada_cede_el_sondeo(void) {
    return __atomic_load_n(&g_modo, __ATOMIC_ACQUIRE) && g_hilo && hilo_actual() != g_hilo;
}

static void hilo_entrada(void *u) {
    (void)u;
    for (;;) {
        if (!__atomic_load_n(&g_modo, __ATOMIC_ACQUIRE)) { hilo_esperar(&g_ev); continue; }
        /* Si CPU 0 tiene el cerrojo (una orden de la consola que usa el 8042 o el I2C), se vuelve en la proxima vuelta */
        if (cerrojo_intentar(&g_cerrojo_entrada)) {
            if (ps2_activo()) ps2_sondear();
            if (tactil_listo()) (void)tactil_sondear();
            g_pasadas++;
            cerrojo_soltar(&g_cerrojo_entrada);
        }
        hilo_dormir_ms(4);
    }
}

int entrada_hilo(int encender) {
    if (encender && !g_hilo) {
        /* En CPU 2, no en la 1: mientras lee el trackpad tiene el cerrojo de entrada (una lectura I2C espera hasta varios ms)
         * y con un cerrojo en la mano su CPU no desaloja; en CPU 1 eso retrasaba al hilo de eventos USB (11 ms en H210) */
        g_hilo = hilo_crear(hilo_entrada, 0, "entrada", 2, HILO_NORMAL, 0);
        if (!g_hilo) return -1;
    }
    int antes = g_modo;
    __atomic_store_n(&g_modo, encender ? 1 : 0, __ATOMIC_RELEASE);
    if (encender && !antes) evento_senalar(&g_ev);
    return g_modo;
}

uint64_t entrada_hilo_pasadas(void) { return g_pasadas; }
