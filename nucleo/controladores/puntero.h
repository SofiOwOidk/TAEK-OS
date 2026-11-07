#ifndef TAEK_PUNTERO_H
#define TAEK_PUNTERO_H
#include <stdint.h>
struct puntero_estado {uint64_t secuencia;int32_t x,y;uint32_t botones,dispositivo;uint64_t clicks;int32_t click_x,click_y;};
int puntero_publicar(int32_t,int32_t,uint32_t,uint32_t);
void puntero_retirar(void);
const struct puntero_estado *puntero_obtener_estado(void);
void puntero_comando(const char *);
#endif
