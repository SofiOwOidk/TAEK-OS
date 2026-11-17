#ifndef TAEK_PUNTERO_H
#define TAEK_PUNTERO_H
#include <stdint.h>
struct puntero_estado {uint64_t secuencia;int32_t x,y;uint32_t botones,dispositivo;uint64_t clicks;int32_t click_x,click_y;uint32_t pulsados;};
int puntero_publicar(int32_t,int32_t,uint32_t,uint32_t);
void puntero_retirar(void);
/* H141: botones pulsados desde la ultima llamada, aunque ya se hayan soltado. Un clic rapido cabe entre dos cuadros. */
uint32_t puntero_tomar_pulsados(void);
/* Aceleracion de trackpad, compartida por I2C y PS/2 */
int32_t puntero_acelerar(int32_t d,int32_t v,int32_t *resto);
const struct puntero_estado *puntero_obtener_estado(void);
void puntero_comando(const char *);
#endif
