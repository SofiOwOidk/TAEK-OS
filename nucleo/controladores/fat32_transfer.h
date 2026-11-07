#ifndef FAT32_TRANSFER_H
#define FAT32_TRANSFER_H
#include "particiones.h"
int fat32_transfer_destino(const struct particion *);
uint64_t fat32_transfer_libres(void);
int fat32_transfer_iniciar(const char *,uint64_t);
int fat32_transfer_escribir(uint64_t,const void *,unsigned);
int fat32_transfer_publicar(const uint8_t hash[32]);
int fat32_transfer_cancelar(void);
int fat32_transfer_activo(void);
const char *fat32_transfer_temporal(void);
const struct particion *fat32_transfer_particion(void);
#endif
