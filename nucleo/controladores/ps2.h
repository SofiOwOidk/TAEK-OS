#ifndef CONTROLADORES_PS2_H
#define CONTROLADORES_PS2_H
/* H142: controlador PS/2 (8042) junto a xHCI, como hace Linux. En las laptops el teclado interno y, a menudo, el trackpad
 * cuelgan del controlador embebido y salen por el 8042 aunque el USB este en manos del sistema. Se lee por sondeo. */
#include <stdint.h>

#define PS2_DISPOSITIVO 0x5052u   /* Identificador del mouse PS/2 ante puntero_publicar ('PR') */

/* Habilita el teclado y el puerto AUX y reinicia el mouse. 0 si hay mouse, 1 si solo hay teclado, negativo si no hay
 * 8042. Todas las esperas tienen tope. verbose: imprimir cada paso. */
int ps2_iniciar(int verbose);
int ps2_activo(void);          /* El 8042 se inicio y se lee junto a xHCI */
int ps2_raton_activo(void);    /* El mouse contesto al reinicio */
/* Vacia el 8042: teclas a su cola, bytes del puerto AUX al mouse. Barato si no hay nada. */
void ps2_sondear(void);
char ps2_leer_caracter(void);  /* 0 si no hay tecla */
/* Un byte del puerto AUX (tambien desde teclado.c en modo=ps2). Arma paquetes de 3 bytes y mueve el puntero. */
void ps2_raton_byte(uint8_t b);
void ps2_comando(const char *arg);

#endif
