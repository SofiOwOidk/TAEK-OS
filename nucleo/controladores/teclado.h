#ifndef CONTROLADORES_TECLADO_H
#define CONTROLADORES_TECLADO_H

#include <stdint.h>

void teclado_iniciar(void);
int  teclado_hay_datos(void);
char teclado_leer_caracter(void);
int  teclado_esta_presente(void);
int  teclado_es_modo_nativo(void);
void teclado_fijar_modo_nativo(int nativo);

#endif // CONTROLADORES_TECLADO_H
