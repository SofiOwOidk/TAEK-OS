#ifndef CONTROLADORES_TECLADO_H
#define CONTROLADORES_TECLADO_H

#include <stdint.h>
#define TECLA_ARRIBA 17
#define TECLA_ABAJO 18
#define TECLA_CTRL_ARRIBA 19
#define TECLA_CTRL_ABAJO 20
/* H146: flechas laterales, para los visores (anterior / siguiente). Codigos de control libres: Ctrl+\ y Ctrl+] */
#define TECLA_IZQUIERDA 28
#define TECLA_DERECHA 29
/* H147: Supr y F2, para el explorador (a la papelera, renombrar). Ctrl+^ y Ctrl+_ libres */
#define TECLA_SUPRIMIR 30
#define TECLA_F2 31

void teclado_iniciar(void);
int  teclado_hay_datos(void);
char teclado_leer_caracter(void);
int  teclado_esta_presente(void);
int  teclado_es_modo_nativo(void);
void teclado_fijar_modo_nativo(int nativo);
char teclado_traducir(uint8_t scancode);

#endif // CONTROLADORES_TECLADO_H
