#include "pantalla.h"

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
