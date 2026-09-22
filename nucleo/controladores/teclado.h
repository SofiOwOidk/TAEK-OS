#ifndef CONTROLADORES_TECLADO_H
#define CONTROLADORES_TECLADO_H

#include <stdint.h>

void teclado_iniciar(void);
int  teclado_hay_datos(void);
char teclado_leer_caracter(void);

#endif // CONTROLADORES_TECLADO_H
