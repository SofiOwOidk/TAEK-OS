#ifndef CONTROLADORES_CONSOLA_H
#define CONTROLADORES_CONSOLA_H

#include <stdint.h>

#define COLOR_FONDO_DEFAULT   0x00000000 // Negro puro
#define COLOR_TEXTO_DEFAULT   0x00E0E0E0 // Blanco plata
#define COLOR_PROMPT_DEFAULT  0x0000FF66 // Verde terminal brillante
#define COLOR_USUARIO_DEFAULT 0x0000E5FF // Cyan eléctrico
#define COLOR_ERROR_DEFAULT   0x00FF4444 // Rojo advertencia
#define COLOR_AVISO_DEFAULT   0x00FFD700 // Oro / Ámbar
#define COLOR_EXITO_DEFAULT   0x0000FF00 // Verde puro

void consola_iniciar(void);
void consola_limpiar(void);

void consola_establecer_color_texto(uint32_t fg);
void consola_establecer_color_fondo(uint32_t bg);
uint32_t consola_obtener_color_texto(void);

void consola_escribir_caracter(char c);
void consola_imprimir(const char *texto);
void consola_imprimir_color(const char *texto, uint32_t fg);
void consola_imprimir_linea(const char *texto);
void consola_imprimir_linea_color(const char *texto, uint32_t fg);

void consola_imprimir_dec(uint64_t valor);
void consola_imprimir_hex(uint64_t valor);

char consola_leer_caracter(void);
int  consola_leer_linea(char *buffer, int max_len);

#endif // CONTROLADORES_CONSOLA_H
