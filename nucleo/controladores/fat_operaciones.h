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
#endif
