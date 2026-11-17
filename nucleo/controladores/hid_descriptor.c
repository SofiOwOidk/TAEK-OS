/* Ver hid_descriptor.h. Cada elemento del descriptor es un prefijo (tamano, tipo y etiqueta) y 0, 1, 2 o 4 bytes.
 * Los globales (pagina de uso, tamano y cantidad, ID) valen hasta que cambian; los locales (usos) solo hasta el
 * siguiente elemento principal (Input, Output, Feature, Collection). */
#include "hid_descriptor.h"

#define MAX_USOS 16
#define MAX_PILA 4
#define MAX_PROF 8

struct globales { uint16_t pagina; int32_t min_logico; uint8_t tam; uint8_t cantidad; uint8_t id; };

static uint32_t valor(const uint8_t *p, unsigned tam) {
    uint32_t v = 0;
    for (unsigned i = 0; i < tam; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static int32_t con_signo(uint32_t v, unsigned tam) {
    if (tam == 1) return (int8_t)v;
    if (tam == 2) return (int16_t)v;
    return (int32_t)v;
}

int hid_buscar_raton(const uint8_t *d, unsigned n, struct hid_raton *r) {
    struct globales g = {0, 0, 0, 0, 0}, pila[MAX_PILA];
    unsigned pila_n = 0;
    uint32_t usos[MAX_USOS];          /* Pagina en los 16 bits altos, uso en los bajos */
    unsigned usos_n = 0;
    uint32_t uso_min = 0, uso_max = 0;
    int hay_rango = 0;
    uint16_t bits_por_id[256] = {0};
    int prof = 0, prof_raton = -1;     /* Profundidad de la coleccion Raton abierta, o -1 */
    struct hid_raton res = {0};
    int encontrado = 0;

    for (unsigned i = 0; i < n;) {
        uint8_t pre = d[i];
        if (pre == 0xFE) {                       /* Elemento largo: se salta entero */
            if (i + 2 >= n) return -1;
            i += 3u + d[i + 1];
            continue;
        }
        unsigned tam = pre & 3; if (tam == 3) tam = 4;
        unsigned tipo = (pre >> 2) & 3, etq = pre >> 4;
        if (i + 1 + tam > n) return -1;
        uint32_t v = valor(d + i + 1, tam);
        i += 1 + tam;

        if (tipo == 1) {                          /* Global */
            switch (etq) {
            case 0: g.pagina = (uint16_t)v; break;
            case 1: g.min_logico = con_signo(v, tam); break;
            case 7: g.tam = (uint8_t)v; break;
            case 8: g.id = (uint8_t)v; break;
            case 9: g.cantidad = (uint8_t)v; break;
            case 10: if (pila_n < MAX_PILA) pila[pila_n++] = g; break;
            case 11: if (pila_n) g = pila[--pila_n]; break;
            default: break;
            }
        } else if (tipo == 2) {                   /* Local */
            uint32_t u = tam == 4 ? v : ((uint32_t)g.pagina << 16 | (v & 0xFFFF));
            if (etq == 0 && usos_n < MAX_USOS) usos[usos_n++] = u;
            else if (etq == 1) { uso_min = u; hay_rango = 1; }
            else if (etq == 2) { uso_max = u; hay_rango = 1; }
        } else if (tipo == 0) {                   /* Principal */
            if (etq == 10) {                      /* Collection */
                uint32_t u = usos_n ? usos[0] : 0;
                if (v == 1 && u == 0x00010002 && prof_raton < 0 && !encontrado) prof_raton = prof;
                if (prof < MAX_PROF) prof++;
            } else if (etq == 12) {               /* End Collection */
                if (prof) prof--;
                if (prof_raton >= 0 && prof == prof_raton) {
                    prof_raton = -1;
                    if (res.x.presente && res.y.presente) encontrado = 1;
                    else res = (struct hid_raton){0};
                }
            } else if (etq == 8) {                /* Input */
                unsigned total = (unsigned)g.tam * g.cantidad;
                uint16_t base = bits_por_id[g.id];
                if (prof_raton >= 0 && !encontrado && !(v & 1) && g.tam && g.tam <= 32) {
                    for (unsigned k = 0; k < g.cantidad; k++) {
                        uint32_t u;
                        if (k < usos_n) u = usos[k];
                        else if (hay_rango) { u = uso_min + k; if (u > uso_max) u = uso_max; }
                        else if (usos_n) u = usos[usos_n - 1];
                        else continue;
                        struct hid_campo c = { (uint16_t)(base + k * g.tam), g.tam, g.min_logico < 0, 1 };
                        if ((u >> 16) == 9) {                         /* Botones */
                            if (!res.botones.presente) { res.botones = c; res.id = g.id; }
                            if (res.n_botones < 32) res.n_botones++;
                        } else if (u == 0x00010030 && !res.x.presente) { res.x = c; res.relativo = !!(v & 4); res.id = g.id; }
                        else if (u == 0x00010031 && !res.y.presente) res.y = c;
                        else if (u == 0x00010038 && !res.rueda.presente) res.rueda = c;
                    }
                }
                if (base + total > 0xFFFF) return -1;
                bits_por_id[g.id] = (uint16_t)(base + total);
            }
            usos_n = 0; hay_rango = 0; uso_min = uso_max = 0;
        }
    }
    if (!encontrado) return res.x.presente && res.y.presente ? -1 : -2;
    res.bits = bits_por_id[res.id];
    *r = res;
    return 0;
}

static uint32_t bits_de(const uint8_t *p, unsigned n, unsigned bit, unsigned tam) {
    uint32_t v = 0;
    for (unsigned k = 0; k < tam; k++) {
        unsigned b = bit + k;
        if (b / 8 >= n) break;
        v |= (uint32_t)((p[b / 8] >> (b % 8)) & 1) << k;
    }
    return v;
}
static int32_t campo(const struct hid_campo *c, const uint8_t *p, unsigned n) {
    if (!c->presente) return 0;
    uint32_t v = bits_de(p, n, c->bit, c->tam);
    if (c->con_signo && c->tam < 32 && (v >> (c->tam - 1)) & 1) v |= ~0u << c->tam;
    return (int32_t)v;
}

int hid_decodificar_raton(const struct hid_raton *r, const uint8_t *rep, unsigned n,
                          int32_t *dx, int32_t *dy, uint32_t *botones, int32_t *rueda) {
    if (r->id) {
        if (n < 1) return -1;
        if (rep[0] != r->id) return -2;
        rep++; n--;
    }
    if (n * 8 < r->y.bit + r->y.tam || n * 8 < r->x.bit + r->x.tam) return -1;
    *dx = campo(&r->x, rep, n);
    *dy = campo(&r->y, rep, n);
    *rueda = campo(&r->rueda, rep, n);
    uint32_t b = 0;
    for (unsigned k = 0; k < r->n_botones && k < 3; k++) b |= bits_de(rep, n, r->botones.bit + k, 1) << k;
    *botones = b;
    return 0;
}
