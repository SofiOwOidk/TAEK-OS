/* Paneles translucidos sobre un degradado, solo para ver y medir el compositor. Esc termina antes de tiempo. */
#include "taek.h"

TAEK_PROGRAMA

static uint32_t premultiplicar(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
}

long principal(uint64_t segundos, uint64_t sin_uso) {
    (void)sin_uso;
    struct taek_pantalla pant;
    if (taek_pantalla(&pant)) return -1;
    uint32_t W = pant.ancho, H = pant.alto;

    struct taek_superficie fondo = { .ancho = W, .alto = H };
    if (taek_superficie(&fondo) < 0) return -2;
    uint32_t *f = (uint32_t *)fondo.pixeles;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        uint32_t t = y * 255 / H, u = x * 255 / W;
        f[(uint64_t)y * W + x] = 0xFF000000u | (40 + t / 3) << 16 | (90 + u / 3) << 8 | (160 + t / 4);
    }
    if (taek_presentar(fondo.id, 0, 0, W, H)) return -3;

    enum { NP = 3 };
    static const uint32_t tonos[NP][4] = { { 150, 255, 255, 255 }, { 170, 230, 40, 60 }, { 140, 40, 200, 255 } };
    struct taek_superficie pan[NP];
    int x[NP], y[NP], dx[NP], dy[NP];
    for (int i = 0; i < NP; i++) {
        uint32_t an = W / 4 + (uint32_t)i * 40, al = H / 4 + (uint32_t)i * 20;
        x[i] = 60 + i * 180; y[i] = 60 + i * 120; dx[i] = 3 + i; dy[i] = 2 + i;
        pan[i] = (struct taek_superficie){ .ancho = an, .alto = al, .x = x[i], .y = y[i], .banderas = TAEK_SUP_ALFA };
        if (taek_superficie(&pan[i]) < 0) return -4;
        uint32_t *p = (uint32_t *)pan[i].pixeles;
        const uint32_t *t = tonos[i];
        for (uint32_t b = 0; b < al; b++) for (uint32_t a = 0; a < an; a++) {
            int borde = a < 3 || b < 3 || a >= an - 3 || b >= al - 3;
            uint32_t alfa = borde ? 255 : t[0] - (t[0] / 3) * b / al;
            p[(uint64_t)b * an + a] = premultiplicar(alfa, t[1], t[2], t[3]);
        }
        if (taek_presentar(pan[i].id, 0, 0, an, al)) return -5;
    }

    long fin = (long)segundos * 1000, cuadros = 0;
    struct taek_evento e;
    while (taek_ms() < fin) {
        if (taek_evento(&e) < 0) return -6;
        if (e.tecla == 27) break;
        for (int i = 0; i < NP; i++) {
            x[i] += dx[i]; y[i] += dy[i];
            if (x[i] < 0 || x[i] + (int)pan[i].ancho > (int)W) { dx[i] = -dx[i]; x[i] += 2 * dx[i]; }
            if (y[i] < 0 || y[i] + (int)pan[i].alto > (int)H) { dy[i] = -dy[i]; y[i] += 2 * dy[i]; }
            if (taek_mover(pan[i].id, x[i], y[i])) return -7;
        }
        cuadros++;
    }
    return cuadros;
}
