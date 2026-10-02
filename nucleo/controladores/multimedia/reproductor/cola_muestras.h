#ifndef TAEK_COLA_MUESTRAS_H
#define TAEK_COLA_MUESTRAS_H
#include "../mp4/mp4.h"
#define COLA_MUESTRAS_MAX 256
typedef struct {size_t inicio, bytes; int64_t pts; uint32_t duracion;} muestra_lista;
/* Un solo productor/consumidor BSP. Los punteros entregados viven hasta la
 * siguiente recarga: jamás recargar mientras el decodificador los utiliza. */
typedef struct {
    mp4_contenedor cursor;
    uint8_t *datos;
    size_t capacidad, usados;
    unsigned cabeza, cantidad;
    int pista, eof, error;
    uint64_t bytes_leidos, muestras_leidas, faltantes;
    muestra_lista muestras[COLA_MUESTRAS_MAX];
    void (*observar_indice)(void *,int,unsigned,int64_t,uint64_t,uint64_t);
    void *usuario_observador;
} cola_muestras;
void cola_muestras_iniciar(cola_muestras *,const mp4_contenedor *,int,uint8_t *,size_t);
int cola_muestras_recargar(cola_muestras *,unsigned,int (*)(void *),void *);
int cola_muestras_tomar(cola_muestras *,const uint8_t **,size_t *,int64_t *,uint32_t *);
#endif
