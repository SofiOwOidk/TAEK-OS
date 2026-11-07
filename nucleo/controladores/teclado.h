#ifndef CONTROLADORES_TECLADO_H
#define CONTROLADORES_TECLADO_H

#include <stdint.h>
#define TECLA_ARRIBA 17
#define TECLA_ABAJO 18
#define TECLA_CTRL_ARRIBA 19
#define TECLA_CTRL_ABAJO 20

void teclado_iniciar(void);
int  teclado_hay_datos(void);
char teclado_leer_caracter(void);
int  teclado_esta_presente(void);
int  teclado_es_modo_nativo(void);
void teclado_fijar_modo_nativo(int nativo);

#endif // CONTROLADORES_TECLADO_H
