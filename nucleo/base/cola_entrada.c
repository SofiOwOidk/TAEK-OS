#include "cola_entrada.h"
#include "cerrojo.h"

static cerrojo_t g_cerrojo = CERROJO("cola_entrada");
static struct { char c; uint8_t fuente; } g_cola[COLA_ENTRADA_TAM];
static uint32_t g_inicio, g_total;
static uint64_t g_perdidas, g_encoladas[ENTRADA_FUENTES];

int cola_entrada_encolar(uint8_t fuente, char c) {
    int r = 0;
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo);
    if (g_total == COLA_ENTRADA_TAM) { g_perdidas++; r = -1; }
    else {
        uint32_t i = (g_inicio + g_total) % COLA_ENTRADA_TAM;
        g_cola[i].c = c; g_cola[i].fuente = fuente;
        g_total++;
        if (fuente < ENTRADA_FUENTES) g_encoladas[fuente]++;
    }
    cerrojo_soltar_irq(&g_cerrojo, f);
    return r;
}

char cola_entrada_tomar(uint8_t *fuente) {
    char c = 0;
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo);
    if (g_total) {
        c = g_cola[g_inicio].c;
        if (fuente) *fuente = g_cola[g_inicio].fuente;
        g_inicio = (g_inicio + 1) % COLA_ENTRADA_TAM;
        g_total--;
    }
    cerrojo_soltar_irq(&g_cerrojo, f);
    return c;
}

unsigned cola_entrada_pendientes(void) { return __atomic_load_n(&g_total, __ATOMIC_RELAXED); }
uint64_t cola_entrada_perdidas(void) { return g_perdidas; }
uint64_t cola_entrada_encoladas(uint8_t fuente) { return fuente < ENTRADA_FUENTES ? g_encoladas[fuente] : 0; }
