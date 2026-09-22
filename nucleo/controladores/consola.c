#include "consola.h"
#include "pantalla.h"
#include "teclado.h"
#include "../base/utf8.h"
#include "../base/tiempo.h"
#include "../arquitectura/x86_64/serial.h"

static int g_columnas = 160;
static int g_filas    = 50;

static int g_cursor_x = 0;
static int g_cursor_y = 0;

static uint32_t g_color_fg = COLOR_TEXTO_DEFAULT;
static uint32_t g_color_bg = COLOR_FONDO_DEFAULT;

// Estado del decodificador UTF-8 para cadenas continuas
static uint32_t g_utf8_codepunto = 0;
static int      g_utf8_restantes = 0;

void consola_iniciar(void) {
    uint64_t ancho = pantalla_obtener_ancho();
    uint64_t alto  = pantalla_obtener_alto();

    if (ancho > 0 && alto > 0) {
        g_columnas = (int)(ancho / 8);
        g_filas    = (int)(alto / 16);
    }

    g_cursor_x = 0;
    g_cursor_y = 0;
    g_color_fg = COLOR_TEXTO_DEFAULT;
    g_color_bg = COLOR_FONDO_DEFAULT;

    teclado_iniciar();
}

void consola_limpiar(void) {
    pantalla_limpiar(g_color_bg);
    g_cursor_x = 0;
    g_cursor_y = 0;
}

void consola_establecer_color_texto(uint32_t fg) {
    g_color_fg = fg;
}

void consola_establecer_color_fondo(uint32_t bg) {
    g_color_bg = bg;
}

uint32_t consola_obtener_color_texto(void) {
    return g_color_fg;
}

static void consola_avanzar_linea(void) {
    g_cursor_x = 0;
    g_cursor_y++;
    if (g_cursor_y >= g_filas) {
        pantalla_desplazar_arriba(1, g_color_bg);
        g_cursor_y = g_filas - 1;
    }
}

void consola_escribir_caracter(char c) {
    // Espejo al puerto serial
    serial_escribir_caracter(c);

    // Caracteres de control
    if (c == '\r') {
        g_cursor_x = 0;
        return;
    }
    if (c == '\n') {
        consola_avanzar_linea();
        return;
    }
    if (c == '\b') {
        if (g_cursor_x > 0) {
            g_cursor_x--;
            pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, ' ', g_color_fg, g_color_bg);
        }
        return;
    }
    if (c == '\t') {
        int espacios = 4 - (g_cursor_x % 4);
        for (int i = 0; i < espacios; i++) {
            consola_escribir_caracter(' ');
        }
        return;
    }

    // Decodificación de secuencias UTF-8 multibyte
    uint8_t byte = (uint8_t)c;
    uint8_t glifo = 0;

    if (g_utf8_restantes == 0) {
        if ((byte & 0x80) == 0) {
            glifo = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            g_utf8_codepunto = (byte & 0x1F);
            g_utf8_restantes = 1;
            return;
        } else if ((byte & 0xF0) == 0xE0) {
            g_utf8_codepunto = (byte & 0x0F);
            g_utf8_restantes = 2;
            return;
        } else if ((byte & 0xF8) == 0xF0) {
            g_utf8_codepunto = (byte & 0x07);
            g_utf8_restantes = 3;
            return;
        } else {
            glifo = '?';
        }
    } else {
        g_utf8_codepunto = (g_utf8_codepunto << 6) | (byte & 0x3F);
        g_utf8_restantes--;
        if (g_utf8_restantes > 0) {
            return;
        }
        glifo = unicode_a_cp437(g_utf8_codepunto);
    }

    // Dibujar el caracter en el Framebuffer
    pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, glifo, g_color_fg, g_color_bg);
    g_cursor_x++;

    if (g_cursor_x >= g_columnas) {
        consola_avanzar_linea();
    }
}

void consola_imprimir(const char *texto) {
    if (!texto) return;
    for (int i = 0; texto[i] != '\0'; i++) {
        consola_escribir_caracter(texto[i]);
    }
}

void consola_imprimir_color(const char *texto, uint32_t fg) {
    uint32_t anterior = g_color_fg;
    g_color_fg = fg;
    consola_imprimir(texto);
    g_color_fg = anterior;
}

void consola_imprimir_linea(const char *texto) {
    consola_imprimir(texto);
    consola_escribir_caracter('\n');
}

void consola_imprimir_linea_color(const char *texto, uint32_t fg) {
    consola_imprimir_color(texto, fg);
    consola_escribir_caracter('\n');
}

void consola_imprimir_dec(uint64_t valor) {
    if (valor == 0) {
        consola_escribir_caracter('0');
        return;
    }
    char buf[32];
    int idx = 0;
    while (valor > 0) {
        buf[idx++] = '0' + (valor % 10);
        valor /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) {
        consola_escribir_caracter(buf[i]);
    }
}

void consola_imprimir_hex(uint64_t valor) {
    const char hex[] = "0123456789ABCDEF";
    consola_imprimir("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (valor >> i) & 0xF;
        consola_escribir_caracter(hex[nibble]);
    }
}

char consola_leer_caracter(void) {
    // 1. Revisar teclado PS/2
    char c = teclado_leer_caracter();
    if (c != 0) return c;

    // 2. Revisar puerto serial COM1
    if (serial_hay_datos()) {
        char cs = serial_leer_caracter();
        if (cs == '\r') cs = '\n'; // Normalizar enter de terminal
        return cs;
    }

    return 0;
}

int consola_leer_linea(char *buffer, int max_len) {
    if (!buffer || max_len <= 1) return 0;

    int idx = 0;
    buffer[0] = '\0';

    int ciclo_cursor = 0;
    int estado_cursor = 1;

    for (;;) {
        // Dibujar cursor parpadeante de bloque
        ciclo_cursor++;
        if (ciclo_cursor >= 50000) {
            ciclo_cursor = 0;
            estado_cursor = !estado_cursor;
            uint8_t c_cursor = estado_cursor ? 0xDB : ' '; // 0xDB es el bloque sólido en CP437
            pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, c_cursor, COLOR_PROMPT_DEFAULT, g_color_bg);
        }

        char c = consola_leer_caracter();
        if (c == 0) {
            continue;
        }

        // Limpiar el cursor en la posición actual antes de procesar
        pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, ' ', g_color_fg, g_color_bg);

        if (c == '\n' || c == '\r') {
            buffer[idx] = '\0';
            consola_escribir_caracter('\n');
            return idx;
        }

        if (c == '\b' || (uint8_t)c == 127) { // Backspace o Delete
            if (idx > 0) {
                idx--;
                buffer[idx] = '\0';
                if (g_cursor_x > 0) {
                    g_cursor_x--;
                    pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, ' ', g_color_fg, g_color_bg);
                    serial_escribir_caracter('\b');
                    serial_escribir_caracter(' ');
                    serial_escribir_caracter('\b');
                }
            }
            continue;
        }

        if (c >= 32 && idx < max_len - 1) {
            buffer[idx++] = c;
            buffer[idx] = '\0';
            consola_escribir_caracter(c);
        }
    }
}
