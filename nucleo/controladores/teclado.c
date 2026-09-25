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

static int g_teclado_iniciado = 0;
static int g_teclado_presente = 0;
static int g_modo_nativo = 0;

int teclado_es_modo_nativo(void) {
    return g_modo_nativo;
}

void teclado_fijar_modo_nativo(int nativo) {
    g_modo_nativo = nativo;
}

static void esperar_buffer_entrada_vacio(void) {
    int timeout = 10000;
    while ((leer_puerto_b(PUERTO_ESTADO_TECLADO) & 0x02) && timeout > 0) {
        timeout--;
    }
}

static void configurar_controlador_8042(void) {
    // 1. Activar el primer puerto PS/2 (Teclado) en el controlador 8042
    esperar_buffer_entrada_vacio();
    escribir_puerto_b(PUERTO_ESTADO_TECLADO, 0xAE); // Enable Keyboard Interface

    // 2. Drenar búfer de salida
    int timeout = 1000;
    while ((leer_puerto_b(PUERTO_ESTADO_TECLADO) & 0x01) && timeout > 0) {
        leer_puerto_b(PUERTO_DATOS_TECLADO);
        timeout--;
    }

    // 3. Activar escaneo en el teclado (Comando 0xF4 a puerto 0x60)
    esperar_buffer_entrada_vacio();
    escribir_puerto_b(PUERTO_DATOS_TECLADO, 0xF4);

    // Drenar el ACK (0xFA) si llega
    timeout = 1000;
    while ((leer_puerto_b(PUERTO_ESTADO_TECLADO) & 0x01) && timeout > 0) {
        uint8_t ack = leer_puerto_b(PUERTO_DATOS_TECLADO);
        if (ack == 0xFA) break;
        timeout--;
    }
}

void teclado_iniciar(void) {
    if (!g_modo_nativo) {
        return; // No tocar puertos 0x60 / 0x64 si estamos en modo xHCI puro
    }
    if (g_teclado_iniciado) return;
    g_teclado_iniciado = 1;

    uint8_t estado = leer_puerto_b(PUERTO_ESTADO_TECLADO);
    // Si el puerto 0x64 devuelve 0xFF, el bus está flotando (no hay chip 8042 PS/2 en la placa)
    if (estado == 0xFF) {
        g_teclado_presente = 0;
        return;
    }

    g_teclado_presente = 1;
    configurar_controlador_8042();
}

int teclado_esta_presente(void) {
    if (!g_modo_nativo) {
        return 0;
    }
    if (!g_teclado_iniciado) {
        teclado_iniciar();
    }
    return g_teclado_presente;
}

int teclado_hay_datos(void) {
    if (!g_modo_nativo) {
        return 0;
    }
    if (!g_teclado_iniciado) {
        teclado_iniciar();
    }
    uint8_t estado = leer_puerto_b(PUERTO_ESTADO_TECLADO);
    if (estado == 0xFF) {
        g_teclado_presente = 0;
        return 0;
    }

    // Si antes no estaba presente y ahora el puerto responde (despertó SMM o emulación USB)
    if (!g_teclado_presente) {
        g_teclado_presente = 1;
        configurar_controlador_8042();
    }

    return (estado & 0x01) != 0;
}

char teclado_leer_caracter(void) {
    if (!g_modo_nativo) {
        return 0;
    }
    if (!teclado_hay_datos()) {
        return 0;
    }

    uint8_t estado = leer_puerto_b(PUERTO_ESTADO_TECLADO);
    // Si el bit 5 (0x20) está activo, el byte en 0x60 pertenece al ratón/touchpad (puerto AUX).
    // Lo leemos y descartamos para no interpretar coordenadas de ratón como teclas.
    if (estado & 0x20) {
        leer_puerto_b(PUERTO_DATOS_TECLADO);
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
