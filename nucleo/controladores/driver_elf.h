#ifndef TAEK_DRIVER_ELF_H
#define TAEK_DRIVER_ELF_H
#include <stddef.h>
#include <stdint.h>
#define DRIVER_ELF_SECCIONES 128
#define DRIVER_ELF_MAX (24u*1024u*1024u)
struct driver_elf_seccion {
    uint32_t nombre,tipo;
    uint64_t flags,direccion,offset,tamano;
    uint32_t enlace,info;
    uint64_t alineacion,entrada;
};
struct driver_elf_plan {
    struct driver_elf_seccion secciones[DRIVER_ELF_SECCIONES];
    uint64_t posiciones[DRIVER_ELF_SECCIONES];
    uint32_t total,bytes,descriptor_seccion;
    uint64_t descriptor_offset;
};
/* Pure, bounded validation and relocation; no kernel execution or allocation. */
int driver_elf_planificar(const void *,size_t,struct driver_elf_plan *);
int driver_elf_materializar(const void *,size_t,const struct driver_elf_plan *,void *,size_t,uint64_t);
int driver_elf_rango(const struct driver_elf_plan *,uint64_t,uint64_t,size_t,int);
#endif
