#ifndef ARQUITECTURA_X86_64_SERIAL_H
#define ARQUITECTURA_X86_64_SERIAL_H

#include <stdint.h>

#define PUERTO_COM1 0x3F8

int  serial_iniciar(void);
void serial_escribir_caracter(char c);
void serial_imprimir(const char *texto);
void serial_imprimir_linea(const char *texto);
void serial_imprimir_hex(uint64_t valor);
void serial_imprimir_dec(uint64_t valor);

#endif // ARQUITECTURA_X86_64_SERIAL_H
