#ifndef BASE_TIEMPO_H
#define BASE_TIEMPO_H

#include <stdint.h>

void     tiempo_iniciar(void);
void     esperar_milisegundos(uint32_t ms);
void     esperar_microsegundos(uint32_t us);
uint64_t tiempo_obtener_milisegundos(void);
uint64_t rdtsc(void);

#endif // BASE_TIEMPO_H
