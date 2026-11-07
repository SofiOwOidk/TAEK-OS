#ifndef CONTROLADORES_ARCHIVO_LECTURA_H
#define CONTROLADORES_ARCHIVO_LECTURA_H
#include "particiones.h"
// Lector de archivos normal, volumen/caches nuevos, sin operaciones de escritura.
int archivo_calcular_sha256(const struct particion *,const char *,uint8_t digest[32],uint64_t *bytes);
#endif
