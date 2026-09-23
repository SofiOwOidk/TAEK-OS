#ifndef ARQUITECTURA_X86_64_SERIAL_H
#define ARQUITECTURA_X86_64_SERIAL_H

#include <stdint.h>

#define PUERTO_COM1 0x3F8
#define KERNEL_LOG_TAMANO (64 * 1024) // 64 KiB de búfer de log en memoria

int         serial_iniciar(void);
int         serial_esta_activo(void);
int         serial_hay_datos(void);
char        serial_leer_caracter(void);
void        serial_escribir_caracter(char c);
void        serial_imprimir(const char *texto);
void        serial_imprimir_linea(const char *texto);
void        serial_imprimir_hex(uint64_t valor);
void        serial_imprimir_dec(uint64_t valor);
const char *serial_obtener_log_buffer(uint32_t *tamano_out, uint32_t *cursor_out);

#endif // ARQUITECTURA_X86_64_SERIAL_H
