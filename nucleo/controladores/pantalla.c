#include "pantalla.h"
#include "fuente8x16.h"

static void    *g_fb_base = NULL;
static uint64_t g_ancho   = 0;
static uint64_t g_alto    = 0;
static uint64_t g_pitch   = 0;
static uint16_t g_bpp     = 0;

int pantalla_iniciar(struct limine_framebuffer *fb) {
    if (!fb || !fb->address) {
        return 1;
    }
    g_fb_base = fb->address;
    g_ancho   = fb->width;
    g_alto    = fb->height;
    g_pitch   = fb->pitch;
    g_bpp     = fb->bpp;
    return 0;
}

uint64_t pantalla_obtener_ancho(void) {
    return g_ancho;
}

uint64_t pantalla_obtener_alto(void) {
    return g_alto;
}

void pantalla_dibujar_pixel(int x, int y, uint32_t color) {
    if (!g_fb_base || x < 0 || x >= (int)g_ancho || y < 0 || y >= (int)g_alto) {
        return;
    }
    uint32_t *fila = (uint32_t *)((uint8_t *)g_fb_base + y * g_pitch);
    fila[x] = color;
}

void pantalla_limpiar(uint32_t color) {
    if (!g_fb_base) return;
    for (uint64_t y = 0; y < g_alto; y++) {
        uint32_t *fila = (uint32_t *)((uint8_t *)g_fb_base + y * g_pitch);
        for (uint64_t x = 0; x < g_ancho; x++) {
            fila[x] = color;
        }
    }
}

void pantalla_dibujar_rectangulo(int x, int y, int ancho, int alto, uint32_t color) {
    for (int i = 0; i < alto; i++) {
        for (int j = 0; j < ancho; j++) {
            pantalla_dibujar_pixel(x + j, y + i, color);
        }
    }
}

void pantalla_dibujar_imagen_centrada(int ancho, int alto, const uint32_t *pixeles) {
    if (!g_fb_base || !pixeles) return;

    int inicio_x = ((int)g_ancho - ancho) / 2;
    int inicio_y = ((int)g_alto - alto) / 2;
    if (inicio_x < 0) inicio_x = 0;
    if (inicio_y < 0) inicio_y = 0;

    for (int y = 0; y < alto; y++) {
        int dest_y = inicio_y + y;
        if (dest_y >= (int)g_alto) break;

        uint32_t *dest = (uint32_t *)((uint8_t *)g_fb_base + dest_y * g_pitch + inicio_x * 4);
        const uint32_t *src = &pixeles[y * ancho];

        int copia_ancho = (inicio_x + ancho > (int)g_ancho) ? ((int)g_ancho - inicio_x) : ancho;
        for (int x = 0; x < copia_ancho; x++) {
            dest[x] = src[x];
        }
    }
}

void pantalla_dibujar_caracter(int col, int fila, uint8_t c, uint32_t fg, uint32_t bg) {
    if (!g_fb_base) return;
    int inicio_x = col * 8;
    int inicio_y = fila * 16;
    if (inicio_x + 8 > (int)g_ancho || inicio_y + 16 > (int)g_alto) return;

    const uint8_t *glifo = g_fuente_8x16[c];
    for (int y = 0; y < 16; y++) {
        uint8_t bits = glifo[y];
        uint32_t *dest = (uint32_t *)((uint8_t *)g_fb_base + (inicio_y + y) * g_pitch + inicio_x * 4);
        for (int x = 0; x < 8; x++) {
            dest[x] = (bits & (0x80 >> x)) ? fg : bg;
        }
    }
}

void pantalla_desplazar_arriba(int lineas, uint32_t color_fondo) {
    if (!g_fb_base || lineas <= 0) return;
    int pixeles_alto = lineas * 16;
    if (pixeles_alto >= (int)g_alto) {
        pantalla_limpiar(color_fondo);
        return;
    }

    uint64_t bytes_a_copiar = (g_alto - pixeles_alto) * g_pitch;
    uint64_t *dest = (uint64_t *)g_fb_base;
    const uint64_t *src = (const uint64_t *)((const uint8_t *)g_fb_base + pixeles_alto * g_pitch);

    uint64_t total_u64 = bytes_a_copiar / 8;
    for (uint64_t i = 0; i < total_u64; i++) {
        dest[i] = src[i];
    }

    // Limpiar las filas inferiores que quedaron libres
    for (int y = (int)g_alto - pixeles_alto; y < (int)g_alto; y++) {
        uint32_t *fila = (uint32_t *)((uint8_t *)g_fb_base + y * g_pitch);
        for (uint64_t x = 0; x < g_ancho; x++) {
            fila[x] = color_fondo;
        }
    }
}
