#ifndef CONTROLADORES_COMPOSITOR_H
#define CONTROLADORES_COMPOSITOR_H
#include <stdint.h>

/* Cada operacion pasa por una capa. Una capa puede rechazar (distinto de 0) y entonces la hace la CPU:
 * asi se agregan el motor de copia y el 3D de la GPU sin que los programas lo noten. */
struct comp_capa {
    const char *nombre;
    int (*rellenar)(uint32_t *d, uint32_t paso_d, uint32_t ancho, uint32_t alto, uint32_t color);
    int (*copiar)(uint32_t *d, uint32_t paso_d, const uint32_t *o, uint32_t paso_o, uint32_t ancho, uint32_t alto);
    int (*mezclar)(uint32_t *d, uint32_t paso_d, const uint32_t *o, uint32_t paso_o, uint32_t ancho, uint32_t alto);
    int (*presentar)(const uint32_t *o, uint32_t paso_o, uint32_t x, uint32_t y, uint32_t ancho, uint32_t alto);
};

#define COMP_FONDO 0x00202030u

int compositor_abierto(void);
void compositor_recomponer(void);
int compositor_abrir(void);
void compositor_cerrar(void);
int compositor_crear(uint64_t pixeles, uint32_t ancho, uint32_t alto, int32_t x, int32_t y, uint32_t banderas);
int compositor_presentar(uint32_t id, int64_t x, int64_t y, int64_t ancho, int64_t alto);
int compositor_mover(uint32_t id, int32_t x, int32_t y);
int compositor_elevar(uint32_t id);
int compositor_tamano(uint32_t id, int32_t x, int32_t y, int64_t ancho, int64_t alto);
void compositor_puntero(int32_t x, int32_t y, int visible);

/* Diagnostico y pruebas */
const char *compositor_capa_nombre(void);
void compositor_forzar_rechazo(int activo);
uint64_t compositor_rechazos(void);
void compositor_estadisticas(uint64_t *composiciones, uint64_t *ciclos_componer, uint64_t *ciclos_presentar);
uint32_t compositor_leer_pantalla(uint32_t x, uint32_t y);

#endif
