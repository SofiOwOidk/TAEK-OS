#ifndef TAEK_DRIVER_CONTEXTO_PRUEBA_H
#define TAEK_DRIVER_CONTEXTO_PRUEBA_H
#include <stdint.h>
/* Diagnostico residente de lectura: el propietario mantiene en_callback=1.
 * No invoca codigo de modulo, heap ni mutadores del motor desde AP/IRQ.
 * Requiere BSP ordinario y AP real; no acredita GPU ni contexto NMI. */
typedef int (*driver_contexto_lectura_fn)(void);
struct driver_contexto_resultado {
    unsigned cpus,entradas,fallos,admision,heap_limpio;
    int antes,despues,lote[4],irq;
    unsigned irq_recibidas,vector_retenido,if_antes,if_despues;
};
int driver_contexto_probar(driver_contexto_lectura_fn,struct driver_contexto_resultado *);
#endif
