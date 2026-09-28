#ifndef CONTROLADORES_FAT_LECTOR_H
#define CONTROLADORES_FAT_LECTOR_H
#include "particiones.h"
#include <stddef.h>
#define FAT_LECTOR_NOMBRE 1024
#define FAT_LECTOR_PUNTOS 32
struct fat_lector_nodo {
    uint64_t longitud, valida;
    uint32_t cluster;
    uint8_t contiguo, directorio;
    uint16_t nombre16[255], unidades;
    char nombre[FAT_LECTOR_NOMBRE];
};
struct fat_lector_punto {uint32_t indice,cluster,tortuga,potencia,pasos;};
struct fat_lector_cursor {
    struct fat_lector_nodo nodo;
    uint32_t indice,cluster,tortuga,potencia,pasos,total_pasos;
    unsigned puntos, proximo;
    struct fat_lector_punto punto[FAT_LECTOR_PUNTOS];
};
struct fat_lector_volumen {
    struct particion particion;
    uint64_t sectores, fat, datos, longitud_bitmap;
    uint32_t clusters, raiz, sectores_cluster, bytes_cluster, sectores_fat, cluster_bitmap;
    uint16_t *upcase;
    int montado, exfat, respaldo, error;
    struct fat_lector_cursor bitmap;
    uint64_t bitmap_pagina;
    uint8_t bitmap_cache[512];
};
struct fat_lector_iterador {
    struct fat_lector_cursor cursor;
    uint64_t posicion, sector_cache;
    uint8_t cache[512];
    uint16_t lfn[260];
    unsigned lfn_esperado,lfn_total;
    uint8_t lfn_checksum;
    int terminado;
};
struct vfs_descriptor_archivo;
extern struct fat_lector_volumen fat_lector_fat32, fat_lector_exfat;
int fat_lector_montar(struct fat_lector_volumen *,const struct particion *);
void fat_lector_desmontar(struct fat_lector_volumen *);
int fat_lector_resolver(struct fat_lector_volumen *,const char *,struct fat_lector_nodo *,const volatile uint8_t *);
int fat_lector_iterar(struct fat_lector_volumen *,const struct fat_lector_nodo *,struct fat_lector_iterador *);
int fat_lector_siguiente(struct fat_lector_volumen *,struct fat_lector_iterador *,struct fat_lector_nodo *,const volatile uint8_t *);
void fat_lector_cursor_iniciar(struct fat_lector_cursor *,const struct fat_lector_nodo *);
int64_t fat_lector_leer(struct fat_lector_volumen *,struct fat_lector_cursor *,uint64_t,void *,size_t,const volatile uint8_t *);
int fat_lector_abrir_stream(struct fat_lector_volumen *,const char *,void *);
int64_t fat_lector_leer_stream(struct fat_lector_volumen *,void *,void *,size_t);
int fat_lector_listar(struct fat_lector_volumen *,const char *,unsigned);
int fat_lector_tree(struct fat_lector_volumen *,const char *);
#endif
