#ifndef CONTROLADORES_FAT32_H
#define CONTROLADORES_FAT32_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// TAEK OS - CONTROLADOR DE SISTEMA DE ARCHIVOS FAT32 (Hito 49)
// Arquitectura Anillo 0 en Español
// ============================================================================

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F // (READ_ONLY | HIDDEN | SYSTEM | VOLUME_ID)

// Estructura oficial del BIOS Parameter Block (BPB) FAT32 (Sector de Arranque VBR)
struct __attribute__((packed)) fat32_bpb {
    uint8_t  jmp_boot[3];
    char     oem_name[8];
    uint16_t bytes_por_sector;        // Comúnmente 512
    uint8_t  sectores_por_cluster;     // 1, 2, 4, 8, 16, 32, 64
    uint16_t sectores_reservados;      // Desplazamiento a la FAT1
    uint8_t  num_fats;                 // Usualmente 2
    uint16_t root_entries;             // 0 en FAT32
    uint16_t total_sectores_16;        // 0 en FAT32
    uint8_t  media_type;               // 0xF8 para discos fijos / flash
    uint16_t sectores_por_fat_16;      // 0 en FAT32
    uint16_t sectores_por_pista;
    uint16_t num_cabezas;
    uint32_t sectores_ocultos;         // LBA de inicio de la partición si está particionado
    uint32_t total_sectores_32;        // Total de sectores del volumen
    uint32_t sectores_por_fat_32;      // Tamaño de cada tabla FAT en sectores
    uint16_t banderas_ext;
    uint16_t version_fs;
    uint32_t cluster_raiz;             // Comúnmente cluster 2
    uint16_t sector_fs_info;
    uint16_t sector_backup_boot;
    uint8_t  reservado[12];
    uint8_t  numero_unidad;
    uint8_t  reservado1;
    uint8_t  firma_arranque;           // 0x29
    uint32_t id_volumen;
    char     etiqueta_volumen[11];
    char     tipo_fs[8];               // "FAT32   "
};

// Entrada de directorio estándar 8.3 (32 bytes)
struct __attribute__((packed)) fat32_entrada_dir {
    char     nombre[11];               // 8 nombre + 3 extensión
    uint8_t  atributos;                // Máscara de bits FAT32_ATTR_*
    uint8_t  nt_reservado;
    uint8_t  creacion_decimas;
    uint16_t hora_creacion;
    uint16_t fecha_creacion;
    uint16_t fecha_ultimo_acceso;
    uint16_t cluster_alto;             // Bits 31:16 del cluster inicial
    uint16_t hora_modificacion;
    uint16_t fecha_modificacion;
    uint16_t cluster_bajo;             // Bits 15:0 del cluster inicial
    uint32_t tamano_archivo;           // Tamaño en bytes
};

// Entrada de Nombre Largo de Archivo (LFN - 32 bytes)
struct __attribute__((packed)) fat32_entrada_lfn {
    uint8_t  orden;                    // Secuencia (bit 6 = último fragmento 0x40)
    uint16_t nombre1[5];               // Caracteres 1-5 (UTF-16LE)
    uint8_t  atributos;                // Siempre 0x0F
    uint8_t  tipo;                     // 0x00
    uint8_t  checksum;                 // Checksum del nombre corto 8.3
    uint16_t nombre2[6];               // Caracteres 6-11 (UTF-16LE)
    uint16_t cluster_cero;             // Siempre 0
    uint16_t nombre3[2];               // Caracteres 12-13 (UTF-16LE)
};

// Estructura de volumen montado en memoria
struct fat32_volumen {
    int      montado;
    uint8_t  unidad_msc;
    uint32_t lba_inicio_particion;
    uint32_t lba_fat;
    uint32_t lba_datos;
    uint16_t bytes_por_sector;
    uint8_t  sectores_por_cluster;
    uint32_t bytes_por_cluster;
    uint32_t sectores_por_fat;
    uint32_t cluster_raiz;
    char     etiqueta[12];
};

// --- API PÚBLICA DEL CONTROLADOR FAT32 ---

// Inicializa estructuras internas del subsistema de archivos
void fat32_iniciar(void);

// Monta el sistema de archivos FAT32 de la unidad USB especificada (escanea MBR y VBR)
int  fat32_montar(uint8_t unidad_msc);

// Desmonta el volumen actual
void fat32_desmontar(void);

// Consulta si hay un volumen FAT32 montado
int  fat32_esta_montado(void);

// Retorna el descriptor del volumen montado actual
const struct fat32_volumen *fat32_obtener_volumen(void);

// Dibuja el árbol de directorios y archivos estilo 'tree' de Linux
int  fat32_ejecutar_tree(const char *ruta_inicial);

// Lista los contenidos de un directorio específico ('ls' / 'dir')
int  fat32_listar_directorio(const char *ruta);

// Lee y muestra en pantalla un archivo de texto plano ('cat')
int  fat32_leer_archivo_texto(const char *ruta);

#endif // CONTROLADORES_FAT32_H
