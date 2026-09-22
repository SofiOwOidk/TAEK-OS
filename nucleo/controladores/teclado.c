#include "teclado.h"
#include "../arquitectura/x86_64/puertos.h"

#define PUERTO_DATOS_TECLADO  0x60
#define PUERTO_ESTADO_TECLADO 0x64

static int g_shift = 0;
static int g_bloq_mayus = 0;

// Tabla Scancode Set 1 normal
static const char g_mapa_normal[128] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

// Tabla Scancode Set 1 con Shift
static const char g_mapa_shift[128] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '\"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

void teclado_iniciar(void) {
    while (leer_puerto_b(PUERTO_ESTADO_TECLADO) & 0x01) {
        leer_puerto_b(PUERTO_DATOS_TECLADO);
    }
}

int teclado_hay_datos(void) {
    return (leer_puerto_b(PUERTO_ESTADO_TECLADO) & 0x01) != 0;
}

char teclado_leer_caracter(void) {
    if (!teclado_hay_datos()) {
        return 0;
    }

    uint8_t scancode = leer_puerto_b(PUERTO_DATOS_TECLADO);

    // Liberación de tecla
    if (scancode & 0x80) {
        uint8_t soltada = scancode & 0x7F;
        if (soltada == 0x2A || soltada == 0x36) {
            g_shift = 0;
        }
        return 0;
    }

    // Tecla pulsada
    if (scancode == 0x2A || scancode == 0x36) {
        g_shift = 1;
        return 0;
    }
    if (scancode == 0x3A) {
        g_bloq_mayus = !g_bloq_mayus;
        return 0;
    }

    if (scancode >= 128) {
        return 0;
    }

    char c = 0;
    if (g_shift) {
        c = g_mapa_shift[scancode];
    } else {
        c = g_mapa_normal[scancode];
    }

    if (g_bloq_mayus && c >= 'a' && c <= 'z' && !g_shift) {
        c = c - 'a' + 'A';
    } else if (g_bloq_mayus && c >= 'A' && c <= 'Z' && g_shift) {
        c = c - 'A' + 'a';
    }

    return c;
}
