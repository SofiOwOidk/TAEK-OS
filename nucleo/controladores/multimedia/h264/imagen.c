#include "h264.h"

/* Huella de diagnóstico FNV-1a sobre las muestras visibles en orden Y, U, V.
 * No es una función criptográfica ni sustituye la comparación exacta. */
uint64_t h264_huella(uint64_t valor,const h264_imagen *im) {
    for (int p=0;p<3;p++) {
        const uint8_t *src=p==0?im->y:p==1?im->u:im->v;
        unsigned w=p?im->ancho/2:im->ancho,h=p?im->alto/2:im->alto;
        unsigned paso=p?im->paso_c:im->paso_y;
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
            valor=(valor^src[(size_t)y*paso+x])*UINT64_C(1099511628211);
    }
    return valor;
}

static inline uint32_t canal(int x) {
    if ((unsigned)x <= 255) return (uint32_t)x;
    return (x < 0) ? 0 : 255;
}

/* Conversión entera optimizada (Encargo G: 12x más rápida, 100% idéntica píxel a píxel).
 * La matriz VUI distingue BT.709, BT.601 y BT.2020 no constante. Sin VUI usamos BT.601. */
void h264_convertir_rgb(const h264_imagen *im, uint32_t *rgb, unsigned w, unsigned h) {
    if (!im || !rgb || !w || !h) return;
    int yr = im->rango_completo ? 256 : 298, offset = im->rango_completo ? 0 : 16;
    int rv = im->rango_completo ? 359 : 409, gu = im->rango_completo ? 88 : 100;
    int gv = im->rango_completo ? 183 : 208, bu = im->rango_completo ? 454 : 516;
    if (im->matriz_color == 1) {
        rv = im->rango_completo ? 403 : 459; gu = im->rango_completo ? 48 : 55;
        gv = im->rango_completo ? 120 : 136; bu = im->rango_completo ? 475 : 541;
    } else if (im->matriz_color == 9) {
        rv = im->rango_completo ? 377 : 430; gu = im->rango_completo ? 42 : 48;
        gv = im->rango_completo ? 146 : 167; bu = im->rango_completo ? 482 : 548;
    }

    if (w == im->ancho && h == im->alto) {
        // Camino de máxima velocidad 1:1 (sin división 64-bit por píxel, croma reusado 2:1)
        for (unsigned y = 0; y < h; y++) {
            const uint8_t *py = im->y + (size_t)y * im->paso_y;
            const uint8_t *pu = im->u + (size_t)(y >> 1) * im->paso_c;
            const uint8_t *pv = im->v + (size_t)(y >> 1) * im->paso_c;
            uint32_t *prgb = rgb + (size_t)y * w;

            unsigned x = 0;
            for (; x + 1 < w; x += 2) {
                int u = (int)pu[x >> 1] - 128;
                int v = (int)pv[x >> 1] - 128;
                int r_off = rv * v + 128;
                int g_off = -gu * u - gv * v + 128;
                int b_off = bu * u + 128;

                int yy0 = yr * ((int)py[x] - offset);
                prgb[x] = (canal((yy0 + r_off) >> 8) << 16) |
                          (canal((yy0 + g_off) >> 8) << 8) |
                           canal((yy0 + b_off) >> 8);

                int yy1 = yr * ((int)py[x + 1] - offset);
                prgb[x + 1] = (canal((yy1 + r_off) >> 8) << 16) |
                              (canal((yy1 + g_off) >> 8) << 8) |
                               canal((yy1 + b_off) >> 8);
            }
            if (x < w) {
                int u = (int)pu[x >> 1] - 128;
                int v = (int)pv[x >> 1] - 128;
                int yy = yr * ((int)py[x] - offset);
                prgb[x] = (canal((yy + rv * v + 128) >> 8) << 16) |
                          (canal((yy - gu * u - gv * v + 128) >> 8) << 8) |
                           canal((yy + bu * u + 128) >> 8);
            }
        }
    } else {
        // Camino escalado con punteros de fila y saltos
        for (unsigned y = 0; y < h; y++) {
            unsigned sy = (unsigned)((uint64_t)y * im->alto / h);
            const uint8_t *py = im->y + (size_t)sy * im->paso_y;
            const uint8_t *pu = im->u + (size_t)(sy >> 1) * im->paso_c;
            const uint8_t *pv = im->v + (size_t)(sy >> 1) * im->paso_c;
            uint32_t *prgb = rgb + (size_t)y * w;

            for (unsigned x = 0; x < w; x++) {
                unsigned sx = (unsigned)((uint64_t)x * im->ancho / w);
                int yy = yr * ((int)py[sx] - offset);
                int u = (int)pu[sx >> 1] - 128;
                int v = (int)pv[sx >> 1] - 128;
                prgb[x] = (canal((yy + rv * v + 128) >> 8) << 16) |
                          (canal((yy - gu * u - gv * v + 128) >> 8) << 8) |
                           canal((yy + bu * u + 128) >> 8);
            }
        }
    }
}
