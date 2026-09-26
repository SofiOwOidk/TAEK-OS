#include "cabac.h"
#include "tablas_cabac.h"

int h264_cabac_iniciar(h264_cabac *c, h264_bits *b, int qp, unsigned modo) {
    if (modo > 3 || qp < 0 || qp > 51) return 0;
    c->bits = b;
    for (unsigned i = 0; i < 460; i++) {
        int inicial = h264_recortar(((cabac_mn[modo][i][0] * qp) >> 4) +
                                     cabac_mn[modo][i][1], 1, 126);
        c->contexto[i] = (uint8_t)(inicial <= 63 ? (63-inicial)*2 : (inicial-64)*2+1);
    }
    c->rango = 510;
    c->valor = h264_bits_leer(b,9);
    if (c->valor >= 510) b->error = 1;
    return !b->error;
}

static void renormalizar(h264_cabac *c) {
    while (c->rango < 256) {
        c->rango <<= 1;
        c->valor = (c->valor << 1) | h264_bits_leer(c->bits,1);
    }
    if (c->valor >= c->rango) c->bits->error = 1;
}

unsigned h264_cabac_bin(h264_cabac *c, unsigned ctx) {
    if (ctx >= 460 || c->bits->error) { c->bits->error = 1; return 0; }
    unsigned estado = c->contexto[ctx] >> 1, mps = c->contexto[ctx] & 1;
    unsigned lps = cabac_rango_lps[estado][(c->rango>>6)&3], resultado = mps;
    c->rango -= lps;
    if (c->valor >= c->rango) {
        resultado ^= 1;
        c->valor -= c->rango;
        c->rango = lps;
        if (!estado) mps ^= 1;
        estado = cabac_trans_lps[estado];
    } else if (estado < 62) estado++;
    c->contexto[ctx] = (uint8_t)(estado*2+mps);
    renormalizar(c);
    return resultado;
}

unsigned h264_cabac_bypass(h264_cabac *c) {
    if (c->bits->error) return 0;
    c->valor = (c->valor<<1) | h264_bits_leer(c->bits,1);
    if (c->valor >= c->rango) { c->valor -= c->rango; return 1; }
    return 0;
}

unsigned h264_cabac_terminar(h264_cabac *c) {
    c->rango -= 2;
    if (c->valor >= c->rango) return 1;
    renormalizar(c);
    return 0;
}
