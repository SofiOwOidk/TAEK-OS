#include "interno.h"

uint32_t h264_bits_leer(h264_bits *b, unsigned n) {
    if (b->error || n > 32 || b->bytes > SIZE_MAX / 8 ||
        b->posicion > b->bytes * 8 || n > b->bytes * 8 - b->posicion) {
        b->error = 1;
        return 0;
    }
    uint32_t v = 0;
    while (n--) {
        v = (v << 1) | ((b->datos[b->posicion >> 3] >>
                        (7 - (b->posicion & 7))) & 1);
        ++b->posicion;
    }
    return v;
}

uint32_t h264_ue(h264_bits *b) {
    unsigned ceros = 0;
    while (!b->error && !h264_bits_leer(b, 1)) {
        if (++ceros > 31) { b->error = 1; return 0; }
    }
    if (b->error) return 0;
    return ((UINT32_C(1) << ceros) - 1) + h264_bits_leer(b, ceros);
}

int32_t h264_se(h264_bits *b) {
    uint32_t v = h264_ue(b);
    if (v == UINT32_MAX) { b->error = 1; return 0; }
    return (v & 1) ? (int32_t)(v / 2 + 1) : -(int32_t)(v / 2);
}

int h264_mas_rbsp(const h264_bits *b) {
    if (b->error || b->bytes > SIZE_MAX / 8 || b->posicion >= b->bytes * 8)
        return 0;
    size_t fin = b->bytes * 8;
    if (fin - b->posicion > 8) return 1;
    /* Únicamente un 1 seguido de ceros es rbsp_trailing_bits. */
    for (size_t i = b->posicion; i < fin; ++i) {
        int bit = (b->datos[i >> 3] >> (7 - (i & 7))) & 1;
        if (bit != (i == b->posicion)) return 1;
    }
    return 0;
}

int h264_fin_rbsp(h264_bits *b) {
    if (h264_bits_leer(b, 1) != 1) b->error = 1;
    while (!b->error && (b->posicion & 7))
        if (h264_bits_leer(b, 1)) b->error = 1;
    return !b->error && b->posicion == b->bytes * 8;
}
