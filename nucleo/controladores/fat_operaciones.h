#ifndef TAEK_FAT_OPERACIONES_H
#define TAEK_FAT_OPERACIONES_H
#include "particiones.h"
enum fat_operacion { FAT_OPERACION_COPIAR, FAT_OPERACION_MOVER, FAT_OPERACION_ELIMINAR };
/* Opera sobre archivos regulares de una particion FAT32/exFAT.
 * No sobreescribe destinos. Verifica la copia antes de publicar metadatos.
 * Undo de sectores en RAM ante errores I/O; Dirty conserva un fallo ambiguo.
 * No promete atomicidad ante perdida de energia. */
int fat_operar_archivo(const struct particion *,const char *,const char *,enum fat_operacion);
int fat_operar_entre(const struct particion *,const char *,const struct particion *,const char *,int mover);
/* H147: carpeta vacia (carpeta=1, sin datos) o archivo con n bytes de 'datos'. Un archivo existente con ese nombre se
 * reemplaza en la misma operacion; una carpeta existente da VOLUMEN_EXISTE. Mismas garantias que fat_operar_archivo. */
int fat_crear(const struct particion *,const char *ruta,int carpeta,const void *datos,uint64_t n);
#endif
