#ifndef TAEK_CACHE_FUENTE_H
#define TAEK_CACHE_FUENTE_H
#include "../mp4/mp4.h"

/* Debe cubrir el tamano maximo de READ(10) seleccionable (256 KiB). */
#define CACHE_FUENTE_MAX 262144
/* Ranuras de datos: dos flujos intercalados (video y audio) sin expulsarse. */
#define CACHE_FUENTE_RANURAS 2
/* Pistas con estado de secuencialidad independiente. */
#define CACHE_FUENTE_PISTAS 4
/* Lecturas logicas contiguas necesarias antes de adelantar. */
#define CACHE_FUENTE_UMBRAL_SECUENCIAL 3

/* Estado minimo de lectura adelantada por pista. */
typedef struct {
    int      activo;
    uint64_t offset_esperado;   /* fin de la ultima lectura logica de la pista */
    size_t   ventana;           /* bytes a adelantar una vez secuencial */
    unsigned consecutivas;      /* lecturas logicas contiguas consecutivas */
    int      secuencial;        /* 1 tras CACHE_FUENTE_UMBRAL_SECUENCIAL */
} cache_readahead;

typedef struct {
    mp4_lectura_posicional leer;
    void *contexto;
    int (*validar)(void *);
    void *usuario_validar;
    uint64_t tamano;
    size_t   ventana;           /* adelanto comun; se copia a cada pista */
    unsigned reemplazo;

    /* Telemetria de desacople logico/fisico. */
    uint64_t lecturas;          /* peticiones del demux (logicas) */
    uint64_t aciertos;          /* servidas sin tocar almacenamiento */
    uint64_t fallos;            /* lecturas al almacenamiento */
    uint64_t adelantos;         /* fallos resueltos con read-ahead */

    uint64_t offset_ranura[CACHE_FUENTE_RANURAS];
    size_t   validos[CACHE_FUENTE_RANURAS];
    cache_readahead ra[CACHE_FUENTE_PISTAS];
    uint8_t datos[CACHE_FUENTE_RANURAS][CACHE_FUENTE_MAX];
} cache_fuente;

int64_t cache_fuente_leer(void *,int,uint64_t,void *,size_t);
#endif
