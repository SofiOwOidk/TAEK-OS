#ifndef CONTROLADORES_PARTICIONES_H
#define CONTROLADORES_PARTICIONES_H
#include <stdint.h>
#define PARTICIONES_MAX 128
enum volumen_formato { VOLUMEN_DESCONOCIDO, VOLUMEN_FAT32, VOLUMEN_EXFAT, VOLUMEN_NTFS, VOLUMEN_EXT4 };
enum volumen_resultado {
    VOLUMEN_OK=0, VOLUMEN_DESCONOCIDO_ERROR=-20, VOLUMEN_NO_SOPORTADO=-21,
    VOLUMEN_CORRUPTO=-22, VOLUMEN_TRANSPORTE=-23, VOLUMEN_DESCONECTADO=-24,
    VOLUMEN_CANCELADO=-25, VOLUMEN_NO_ENCONTRADO=-26, VOLUMEN_ELEGIR=-27,
    VOLUMEN_SECTOR_NO_SOPORTADO=-28, VOLUMEN_SOLO_LECTURA=-29,
    VOLUMEN_TEXFAT_NO_SOPORTADO=-30
};
struct particion {
    uint64_t inicio, sectores, generacion_msc;
    uint32_t numero;
    uint8_t unidad, esquema; /* 0 volumen directo, 1 MBR, 2 GPT */
    enum volumen_formato formato;
    int resultado, recuperado;
};
struct particiones { unsigned total; struct particion entradas[PARTICIONES_MAX]; int recuperado; };
int particiones_descubrir(uint8_t,struct particiones *);
int particion_leer(const struct particion *,uint64_t,unsigned,void *,const volatile uint8_t *);
int particion_identificar(struct particion *);
void particiones_configurar_servicio(int (*)(void));
const char *volumen_nombre(enum volumen_formato);
const char *volumen_error(int);
#endif
