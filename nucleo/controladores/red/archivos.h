#ifndef RED_ARCHIVOS_H
#define RED_ARCHIVOS_H
#include <stdint.h>
void red_archivos_comando(const char *);
int red_archivos_preparar_reinicio(void);
/* Sólo devuelve bytes retenidos de una transferencia RAM completa/verificada. */
int red_archivos_ram_obtener(const void **,uint64_t *,uint8_t[32]);
#endif
