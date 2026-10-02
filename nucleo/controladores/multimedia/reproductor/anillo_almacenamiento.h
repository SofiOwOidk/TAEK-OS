#ifndef TAEK_ANILLO_ALMACENAMIENTO_H
#define TAEK_ANILLO_ALMACENAMIENTO_H
#include <stdint.h>
#include <stddef.h>
#include "../mp4/mp4.h"

// ============================================================================
// TAEK OS - ANILLO DE ALMACENAMIENTO (PRODUCTOR / CONSUMIDOR)
// Desacopla el almacenamiento (productor) del demux MP4 (consumidor): el
// consumidor lee bloques ya prefetched y no espera al USB mientras haya datos.
// Un solo productor/consumidor (BSP); el transporte BOT sigue siendo sincrono.
// ============================================================================

#define ANILLO_BLOQUES       8
#define ANILLO_BLOQUE_BYTES  (128u * 1024u)
#define ANILLO_PISTAS        4

enum anillo_estado {
    ANILLO_VACIO = 0,
    ANILLO_LLENANDO,
    ANILLO_LISTO,
    ANILLO_CONSUMIENDO
};

typedef struct {
    uint64_t offset;   // offset del bloque en el archivo
    size_t   bytes;    // bytes validos
    int      estado;
} anillo_bloque;

typedef struct {
    int64_t (*leer)(void *contexto, int pista, uint64_t offset, void *dst, size_t n);
    void *contexto;
    int (*validar)(void *usuario);
    void *usuario_validar;
    uint64_t tamano;
    size_t   bloque_bytes;

    anillo_bloque bloques[ANILLO_BLOQUES];
    uint8_t datos[ANILLO_BLOQUES][ANILLO_BLOQUE_BYTES];

    uint64_t productor_off;            // siguiente offset a producir
    unsigned cola;                     // proxima ranura a escribir
    unsigned cabeza;                   // bloque mas antiguo retenido
    unsigned cantidad;                 // bloques LISTO/CONSUMIENDO

    uint64_t leido[ANILLO_PISTAS];     // maximo offset leido por pista
    int      pista_activa[ANILLO_PISTAS];

    // Telemetria del desacople.
    uint64_t lecturas, aciertos, fallos, reposiciones,
             bloques_leidos, bytes_leidos, evicciones;
    uint64_t starvations;              // consumidor no encontro dato prefetched
    uint64_t productor_paradas;        // productor no pudo llenar (lleno/protegido)
    uint64_t bytes_inutiles;           // bloques evictados sin haberse consumido
    uint64_t bytes_utiles;             // bloques evictados ya consumidos
    uint64_t margen_min, margen_max, margen_suma, margen_muestras;
    uint8_t  tocado[ANILLO_BLOQUES];
} anillo_almacenamiento;

void    anillo_iniciar(anillo_almacenamiento *a,
                       int64_t (*leer)(void *, int, uint64_t, void *, size_t),
                       void *contexto,
                       int (*validar)(void *), void *usuario_validar,
                       uint64_t tamano);

// Bombea el productor: rellena hasta `max_bloques` bloques vacios, sin pisar
// datos que alguna pista aun no haya leido. Devuelve 1 con progreso/lleno,
// 0 en EOF, <0 error.
int     anillo_rellenar(anillo_almacenamiento *a, unsigned max_bloques);

// Consumidor: firma de `mp4_lectura_posicional`.
int64_t anillo_leer(void *contexto, int pista, uint64_t off, void *dst, size_t n);

#endif // TAEK_ANILLO_ALMACENAMIENTO_H
