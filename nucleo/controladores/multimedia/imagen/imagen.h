#ifndef MULTIMEDIA_IMAGEN_H
#define MULTIMEDIA_IMAGEN_H
#include <stdint.h>
#include <stddef.h>
/* JPEG/PNG mediante callbacks VFS. Entrada <=32 MiB, dimensiones <=8192,
 * <=16 millones de píxeles y presupuesto total de asignaciones <=128 MiB.
 * La salida RGB opaca pertenece al consumidor hasta imagen_liberar().
 * No llamar concurrentemente; sólo coordinador con contexto de IRQ preservado. */
int imagen_decodificar_vfs(int,uint32_t **,unsigned *,unsigned *);
void imagen_liberar(void *);
size_t imagen_memoria_pico(void);
#endif
