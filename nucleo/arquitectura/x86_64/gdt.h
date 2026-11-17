#ifndef ARQUITECTURA_X86_64_GDT_H
#define ARQUITECTURA_X86_64_GDT_H

#include <stdint.h>

struct entrada_gdt {
    uint16_t limite_bajo;
    uint16_t base_baja;
    uint8_t  base_media;
    uint8_t  acceso;
    uint8_t  granularidad;
    uint8_t  base_alta;
} __attribute__((packed));

struct puntero_gdt {
    uint16_t limite;
    uint64_t base;
} __attribute__((packed));

void gdt_iniciar(void);
void gdt_destruir_tss_e_ist(void);
/* H213, Fase D: pila de nucleo (RSP0 del TSS) a la que entra una interrupcion o una llamada desde Anillo 3 en una CPU
 * auxiliar (1 a 3). Cada hilo de programa tiene la suya; 0 vuelve a la pila propia de esa CPU. */
void gdt_fijar_rsp0(unsigned cpu, uint64_t rsp0);

#endif // ARQUITECTURA_X86_64_GDT_H
