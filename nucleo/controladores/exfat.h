#ifndef CONTROLADORES_EXFAT_H
#define CONTROLADORES_EXFAT_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// TAEK OS - CONTROLADOR DE SISTEMA DE ARCHIVOS exFAT (Hito 50)
// Arquitectura Anillo 0 en Español (Solo Lectura)
// ============================================================================

#define EXFAT_ATTR_READ_ONLY 0x0001
#define EXFAT_ATTR_HIDDEN    0x0002
#define EXFAT_ATTR_SYSTEM    0x0004
#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_ATTR_ARCHIVE   0x0020

#define EXFAT_TIPO_BITMAP      0x81
#define EXFAT_TIPO_UPCASE      0x82
#define EXFAT_TIPO_ETIQUETA    0x83
#define EXFAT_TIPO_ARCHIVO     0x85
#define EXFAT_TIPO_FLUJO       0xC0
#define EXFAT_TIPO_NOMBRE      0xC1

#define EXFAT_FLUJO_NO_FAT     0x02 // Si está activo, los clusters son contiguos

// Sector de arranque principal de exFAT (VBR - 512 bytes)
struct __attribute__((packed)) exfat_vbr {
    uint8_t  jmp_boot[3];
    char     fs_name[8];              // "EXFAT   "
    uint8_t  must_be_zero[53];
    uint64_t desplazamiento_particion;// En sectores
    uint64_t longitud_volumen;        // En sectores
    uint32_t desplazamiento_fat;      // En sectores desde el inicio de la partición
    uint32_t longitud_fat;            // En sectores
    uint32_t desplazamiento_heap;     // En sectores (inicio de clusters de datos)
    uint32_t conteo_clusters;
    uint32_t cluster_raiz;            // Primer cluster del directorio raíz
    uint32_t serial_volumen;
    uint16_t revision_fs;
    uint16_t banderas_volumen;
    uint8_t  shift_bytes_por_sector;  // 9 = 512 bytes
    uint8_t  shift_sectores_por_cluster; // ej: 3 = 8 sectores (4096 bytes)
    uint8_t  num_fats;                // Usualmente 1
    uint8_t  unidad_bios;
    uint8_t  porcentaje_uso;
    uint8_t  reservado[7];
    uint8_t  codigo_arranque[390];
    uint16_t firma_arranque;          // 0xAA55
};

// Entrada primaria de archivo (Tipo 0x85 - 32 bytes)
struct __attribute__((packed)) exfat_entrada_archivo {
    uint8_t  tipo_entrada;            // 0x85
    uint8_t  conteo_secundarias;      // Cantidad de entradas 0xC0 y 0xC1
    uint16_t checksum;
    uint16_t atributos;
    uint16_t reservado1;
    uint32_t timestamp_creacion;
    uint32_t timestamp_modificacion;
    uint32_t timestamp_acceso;
    uint8_t  ms_creacion;
    uint8_t  ms_modificacion;
    uint8_t  tz_creacion;
    uint8_t  tz_modificacion;
    uint8_t  tz_acceso;
    uint8_t  reservado2[7];
};

// Entrada secundaria de extensión de flujo de datos (Tipo 0xC0 - 32 bytes)
struct __attribute__((packed)) exfat_entrada_flujo {
    uint8_t  tipo_entrada;            // 0xC0
    uint8_t  banderas;                // Bit 1: 1 = No FAT chain (contiguo)
    uint8_t  reservado1;
    uint8_t  longitud_nombre;         // En caracteres UTF-16
    uint16_t hash_nombre;
    uint16_t reservado2;
    uint64_t longitud_valida;
    uint32_t reservado3;
    uint32_t primer_cluster;          // Cluster inicial de datos
    uint64_t longitud_datos;          // Tamaño real del archivo en bytes
};

// Entrada secundaria de fragmento de nombre de archivo (Tipo 0xC1 - 32 bytes)
struct __attribute__((packed)) exfat_entrada_nombre {
    uint8_t  tipo_entrada;            // 0xC1
    uint8_t  banderas;
    uint16_t nombre_utf16[15];        // 15 caracteres UCS-2 / UTF-16LE
};

// Descriptor del volumen exFAT montado
struct exfat_volumen {
    int      montado;
    uint8_t  unidad_msc;
    uint32_t lba_inicio_particion;
    uint32_t lba_fat;
    uint32_t lba_heap;
    uint32_t bytes_por_sector;
    uint32_t sectores_por_cluster;
    uint32_t bytes_por_cluster;
    uint32_t cluster_raiz;
    uint32_t total_clusters;
    char     etiqueta[32];
};

// --- API PÚBLICA DEL CONTROLADOR exFAT ---
void exfat_iniciar(void);
int  exfat_montar(uint8_t unidad_msc);
void exfat_desmontar(void);
int  exfat_esta_montado(void);
const struct exfat_volumen *exfat_obtener_volumen(void);

int  exfat_ejecutar_tree(const char *ruta_inicial);
int  exfat_listar_directorio(const char *ruta);
int  exfat_leer_archivo_texto(const char *ruta);

#endif // CONTROLADORES_EXFAT_H
