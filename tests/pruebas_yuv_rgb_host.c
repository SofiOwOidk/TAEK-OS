#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Definición de h264_imagen para el test
typedef struct {
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    unsigned paso_y;
    unsigned paso_c;
    unsigned ancho;
    unsigned alto;
    int rango_completo;
    int matriz_color;
} h264_imagen;

static inline uint32_t canal_ref(int x) {
    return (uint32_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

// Implementación original de referencia
void h264_convertir_rgb_ref(const h264_imagen *im, uint32_t *rgb, unsigned w, unsigned h) {
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
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) {
        unsigned sx = (unsigned)((uint64_t)x * im->ancho / w);
        unsigned sy = (unsigned)((uint64_t)y * im->alto / h);
        int yy = yr * ((int)im->y[(size_t)sy * im->paso_y + sx] - offset);
        int u = (int)im->u[(size_t)(sy / 2) * im->paso_c + sx / 2] - 128;
        int v = (int)im->v[(size_t)(sy / 2) * im->paso_c + sx / 2] - 128;
        rgb[(size_t)y * w + x] = (canal_ref((yy + rv * v + 128) >> 8) << 16) |
                                 (canal_ref((yy - gu * u - gv * v + 128) >> 8) << 8) |
                                  canal_ref((yy + bu * u + 128) >> 8);
    }
}

static inline uint32_t canal_opt(int x) {
    if ((unsigned)x <= 255) return (uint32_t)x;
    return (x < 0) ? 0 : 255;
}

// Implementación escalar optimizada (Encargo G)
void h264_convertir_rgb_opt(const h264_imagen *im, uint32_t *rgb, unsigned w, unsigned h) {
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
        // Camino ultrarrápido 1:1 (sin división 64-bit por píxel, reutilización de croma 2:1)
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
                prgb[x] = (canal_opt((yy0 + r_off) >> 8) << 16) |
                          (canal_opt((yy0 + g_off) >> 8) << 8) |
                           canal_opt((yy0 + b_off) >> 8);

                int yy1 = yr * ((int)py[x + 1] - offset);
                prgb[x + 1] = (canal_opt((yy1 + r_off) >> 8) << 16) |
                              (canal_opt((yy1 + g_off) >> 8) << 8) |
                               canal_opt((yy1 + b_off) >> 8);
            }
            if (x < w) {
                int u = (int)pu[x >> 1] - 128;
                int v = (int)pv[x >> 1] - 128;
                int yy = yr * ((int)py[x] - offset);
                prgb[x] = (canal_opt((yy + rv * v + 128) >> 8) << 16) |
                          (canal_opt((yy - gu * u - gv * v + 128) >> 8) << 8) |
                           canal_opt((yy + bu * u + 128) >> 8);
            }
        }
    } else {
        // Camino escalado con precomputación de coordenadas por columna
        unsigned *sx_lut = (unsigned *)malloc(w * sizeof(unsigned));
        for (unsigned x = 0; x < w; x++) {
            sx_lut[x] = (unsigned)((uint64_t)x * im->ancho / w);
        }

        for (unsigned y = 0; y < h; y++) {
            unsigned sy = (unsigned)((uint64_t)y * im->alto / h);
            const uint8_t *py = im->y + (size_t)sy * im->paso_y;
            const uint8_t *pu = im->u + (size_t)(sy >> 1) * im->paso_c;
            const uint8_t *pv = im->v + (size_t)(sy >> 1) * im->paso_c;
            uint32_t *prgb = rgb + (size_t)y * w;

            for (unsigned x = 0; x < w; x++) {
                unsigned sx = sx_lut[x];
                int yy = yr * ((int)py[sx] - offset);
                int u = (int)pu[sx >> 1] - 128;
                int v = (int)pv[sx >> 1] - 128;
                prgb[x] = (canal_opt((yy + rv * v + 128) >> 8) << 16) |
                          (canal_opt((yy - gu * u - gv * v + 128) >> 8) << 8) |
                           canal_opt((yy + bu * u + 128) >> 8);
            }
        }
        free(sx_lut);
    }
}

int main(void) {
    printf("=== TEST DE REGRESIÓN EXACTA Y RENDIMIENTO YUV -> RGB ===\n");
    unsigned w = 640, h = 360;
    size_t tam_y = (size_t)w * h;
    size_t tam_c = (size_t)(w / 2) * (h / 2);

    uint8_t *buf_y = malloc(tam_y);
    uint8_t *buf_u = malloc(tam_c);
    uint8_t *buf_v = malloc(tam_c);
    uint32_t *rgb_ref = malloc(w * h * sizeof(uint32_t));
    uint32_t *rgb_opt = malloc(w * h * sizeof(uint32_t));

    // Llenar con datos pseudo-aleatorios deterministas
    srand(12345);
    for (size_t i = 0; i < tam_y; i++) buf_y[i] = (uint8_t)(rand() % 256);
    for (size_t i = 0; i < tam_c; i++) {
        buf_u[i] = (uint8_t)(rand() % 256);
        buf_v[i] = (uint8_t)(rand() % 256);
    }

    h264_imagen im = {
        .y = buf_y,
        .u = buf_u,
        .v = buf_v,
        .paso_y = w,
        .paso_c = w / 2,
        .ancho = w,
        .alto = h,
        .rango_completo = 0,
        .matriz_color = 0
    };

    // 1. Verificación de equivalencia exacta 1:1 en todas las configuraciones
    int modos_rango[] = {0, 1};
    int modos_matriz[] = {0, 1, 9};

    for (int r = 0; r < 2; r++) {
        for (int m = 0; m < 3; m++) {
            im.rango_completo = modos_rango[r];
            im.matriz_color = modos_matriz[m];

            memset(rgb_ref, 0, w * h * sizeof(uint32_t));
            memset(rgb_opt, 0, w * h * sizeof(uint32_t));

            h264_convertir_rgb_ref(&im, rgb_ref, w, h);
            h264_convertir_rgb_opt(&im, rgb_opt, w, h);

            int diffs = 0;
            for (size_t i = 0; i < (size_t)w * h; i++) {
                if (rgb_ref[i] != rgb_opt[i]) {
                    diffs++;
                }
            }

            printf("Config (rango=%d, matriz=%d): %d discrepancias en %u pixeles -> %s\n",
                   im.rango_completo, im.matriz_color, diffs, w * h,
                   diffs == 0 ? "COINCIDENCIA EXACTA (100% OK)" : "FALLO");

            if (diffs != 0) {
                printf("ERROR CRÍTICO: Discrepancia detectada en salida RGB!\n");
                return 1;
            }
        }
    }

    // 2. Medición de rendimiento comparativo en 500 fotogramas
    int frames = 500;
    clock_t t0 = clock();
    for (int i = 0; i < frames; i++) {
        h264_convertir_rgb_ref(&im, rgb_ref, w, h);
    }
    clock_t t1 = clock();
    double ms_ref = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;

    clock_t t2 = clock();
    for (int i = 0; i < frames; i++) {
        h264_convertir_rgb_opt(&im, rgb_opt, w, h);
    }
    clock_t t3 = clock();
    double ms_opt = (double)(t3 - t2) * 1000.0 / CLOCKS_PER_SEC;

    printf("\n--- RESULTADOS DE RENDIMIENTO (500 fotogramas 640x360) ---\n");
    printf("  Tiempo Referencia : %.2f ms (%.3f ms/cuadro)\n", ms_ref, ms_ref / frames);
    printf("  Tiempo Optimizado : %.2f ms (%.3f ms/cuadro)\n", ms_opt, ms_opt / frames);
    printf("  Aceleración       : %.2fx más rápido!\n", ms_ref / ms_opt);

    free(buf_y); free(buf_u); free(buf_v);
    free(rgb_ref); free(rgb_opt);
    printf("\n[OK] Todas las pruebas superadas con éxito.\n");
    return 0;
}
