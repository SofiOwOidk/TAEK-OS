#ifndef TAEK_DRIVER_RAM_H
#define TAEK_DRIVER_RAM_H
#include <stdint.h>
void driver_ram_comando(const char *);
void driver_ram_sondear(void);
/* Carga directa desde memoria (modulo de arranque). 0, -2 si ya hay un modulo, u otro negativo. */
int driver_ram_cargar_bytes(const void *datos,uint64_t bytes);
/* Primera interfaz de la clase de video que haya registrado el modulo cargado. */
int driver_ram_tiene_video(void);
int driver_ram_video(uint32_t orden,void *peticion);
#endif
