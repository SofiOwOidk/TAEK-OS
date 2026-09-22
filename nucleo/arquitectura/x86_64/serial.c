#include "serial.h"
#include "puertos.h"

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
        return 1;
    }

    // Configurar en modo de operación normal
    escribir_puerto_b(PUERTO_COM1 + 4, 0x0F);
    return 0;
}

static inline int serial_transmisor_vacio(void) {
    return leer_puerto_b(PUERTO_COM1 + 5) & 0x20;
}

int serial_hay_datos(void) {
    return leer_puerto_b(PUERTO_COM1 + 5) & 0x01;
}

char serial_leer_caracter(void) {
    while (serial_hay_datos() == 0);
    return (char)leer_puerto_b(PUERTO_COM1);
}

void serial_escribir_caracter(char c) {
    while (serial_transmisor_vacio() == 0);
    escribir_puerto_b(PUERTO_COM1, (uint8_t)c);
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
    char buf[32];
    int idx = 0;
    while (valor > 0) {
        buf[idx++] = '0' + (valor % 10);
        valor /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) {
        serial_escribir_caracter(buf[i]);
    }
}
