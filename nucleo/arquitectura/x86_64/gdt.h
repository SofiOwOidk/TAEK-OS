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

#endif // ARQUITECTURA_X86_64_GDT_H
