#include "serial.h"
#include "puertos.h"

static int  g_serial_listo = 0;
static char g_kernel_log[KERNEL_LOG_TAMANO];
static uint32_t g_log_cursor = 0;
static uint32_t g_log_total_bytes = 0;

int serial_iniciar(void) {
    escribir_puerto_b(PUERTO_COM1 + 1, 0x00);    // Desactivar todas las interrupciones
    escribir_puerto_b(PUERTO_COM1 + 3, 0x80);    // Activar DLAB (divisor de velocidad)
    escribir_puerto_b(PUERTO_COM1 + 0, 0x01);    // Divisor 1 (115200 baudios) byte bajo
    escribir_puerto_b(PUERTO_COM1 + 1, 0x00);    // byte alto
    escribir_puerto_b(PUERTO_COM1 + 3, 0x03);    // 8 bits, sin paridad, un bit de parada (8N1)
    escribir_puerto_b(PUERTO_COM1 + 2, 0xC7);    // Activar colas FIFO, limpiar búferes
    escribir_puerto_b(PUERTO_COM1 + 4, 0x0B);    // Interrupciones activadas, RTS/DSR listos
    escribir_puerto_b(PUERTO_COM1 + 4, 0x1E);    // Modo bucle invertido para autoprueba
    escribir_puerto_b(PUERTO_COM1 + 0, 0xAE);    // Byte de prueba

    // Verificar si el chip UART responde correctamente
    if (leer_puerto_b(PUERTO_COM1 + 0) != 0xAE) {
        g_serial_listo = 0;
        return 1;
    }

    // Configurar en modo de operación normal
    escribir_puerto_b(PUERTO_COM1 + 4, 0x0F);
    g_serial_listo = 1;
    return 0;
}

int serial_esta_activo(void) {
    return g_serial_listo;
}

static inline int serial_transmisor_vacio(void) {
    return leer_puerto_b(PUERTO_COM1 + 5) & 0x20;
}

int serial_hay_datos(void) {
    if (!g_serial_listo) return 0;
    return leer_puerto_b(PUERTO_COM1 + 5) & 0x01;
}

char serial_leer_caracter(void) {
    if (!g_serial_listo) return 0;
    while (serial_hay_datos() == 0);
    return (char)leer_puerto_b(PUERTO_COM1);
}

void serial_escribir_caracter(char c) {
    // 1. Guardar siempre en el búfer circular de log en memoria (dmesg)
    if (g_log_cursor < KERNEL_LOG_TAMANO - 1) {
        g_kernel_log[g_log_cursor++] = c;
        g_kernel_log[g_log_cursor] = '\0';
    } else {
        // Avance circular si se llena
        g_kernel_log[g_log_cursor] = c;
        g_log_cursor = (g_log_cursor + 1) % (KERNEL_LOG_TAMANO - 1);
    }
    g_log_total_bytes++;

    // 2. Transmisión física por UART COM1 con límite de tiempo para no colgar el CPU en placas sin chip serial
    uint32_t timeout = 50000;
    while ((serial_transmisor_vacio() == 0) && (--timeout > 0));
    if (timeout > 0) {
        escribir_puerto_b(PUERTO_COM1, (uint8_t)c);
    }
}

void serial_imprimir(const char *texto) {
    if (!texto) return;
    for (int i = 0; texto[i] != '\0'; i++) {
        if (texto[i] == '\n') {
            serial_escribir_caracter('\r');
        }
        serial_escribir_caracter(texto[i]);
    }
}

void serial_imprimir_linea(const char *texto) {
    serial_imprimir(texto);
    serial_imprimir("\n");
}

void serial_imprimir_hex(uint64_t valor) {
    const char hex[] = "0123456789ABCDEF";
    serial_imprimir("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t cuarteto = (valor >> i) & 0xF;
        serial_escribir_caracter(hex[cuarteto]);
    }
}

void serial_imprimir_dec(uint64_t valor) {
    if (valor == 0) {
        serial_escribir_caracter('0');
        return;
    }
    char buf[24];
    int idx = 0;
    while (valor > 0) {
        buf[idx++] = '0' + (valor % 10);
        valor /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) {
        serial_escribir_caracter(buf[i]);
    }
}

const char *serial_obtener_log_buffer(uint32_t *tamano_out, uint32_t *cursor_out) {
    if (tamano_out) *tamano_out = g_log_total_bytes > KERNEL_LOG_TAMANO ? KERNEL_LOG_TAMANO : g_log_cursor;
    if (cursor_out) *cursor_out = g_log_cursor;
    return g_kernel_log;
}
