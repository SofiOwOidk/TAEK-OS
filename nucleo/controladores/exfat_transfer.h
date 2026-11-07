#ifndef EXFAT_TRANSFER_H
#define EXFAT_TRANSFER_H
#include "particiones.h"
int exfat_transfer_destino(const struct particion *);
uint64_t exfat_transfer_libres(void);
int exfat_transfer_iniciar(const char *,uint64_t);
int exfat_transfer_escribir(uint64_t,const void *,unsigned);
int exfat_transfer_publicar(const uint8_t hash[32]);
int exfat_transfer_cancelar(void);
int exfat_transfer_activo(void);
const char *exfat_transfer_temporal(void);
const struct particion *exfat_transfer_particion(void);
#endif
