#ifndef BASE_ENERGIA_H
#define BASE_ENERGIA_H

#include <stdint.h>

void apagar_equipo(void) __attribute__((noreturn));
void detener_cpu(void) __attribute__((noreturn));

#endif // BASE_ENERGIA_H
