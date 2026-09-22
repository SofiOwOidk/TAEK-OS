#ifndef CONTROLADORES_PANTALLA_H
#define CONTROLADORES_PANTALLA_H

#include <stdint.h>
#include <stddef.h>
#include "../../boot/limine/limine.h"

int  pantalla_iniciar(struct limine_framebuffer *fb);
void pantalla_limpiar(uint32_t color);
void pantalla_dibujar_pixel(int x, int y, uint32_t color);
void pantalla_dibujar_rectangulo(int x, int y, int ancho, int alto, uint32_t color);
void pantalla_dibujar_imagen_centrada(int ancho, int alto, const uint32_t *pixeles);

void pantalla_dibujar_caracter(int col, int fila, uint8_t c, uint32_t fg, uint32_t bg);
void pantalla_desplazar_arriba(int lineas, uint32_t color_fondo);

uint64_t pantalla_obtener_ancho(void);
uint64_t pantalla_obtener_alto(void);

#endif // CONTROLADORES_PANTALLA_H
