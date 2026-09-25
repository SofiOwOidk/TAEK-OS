#ifndef CONTROLADORES_NTFS_H
#define CONTROLADORES_NTFS_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// TAEK OS - CONTROLADOR DE SISTEMA DE ARCHIVOS NTFS (Hito 50)
// Arquitectura Anillo 0 en Español (Solo Lectura)
// ============================================================================

#define NTFS_MAGIC_FILE       0x454C4946 // "FILE" en Little Endian
#define NTFS_MAGIC_BAAD       0x44414142 // "BAAD" (registro corrupto)

#define NTFS_ATTR_STD_INFO    0x10
#define NTFS_ATTR_ATTR_LIST   0x20
#define NTFS_ATTR_FILE_NAME   0x30
#define NTFS_ATTR_OBJECT_ID   0x40
#define NTFS_ATTR_SECURITY    0x50
#define NTFS_ATTR_VOL_NAME    0x60
#define NTFS_ATTR_VOL_INFO    0x70
#define NTFS_ATTR_DATA        0x80
#define NTFS_ATTR_INDEX_ROOT  0x90
#define NTFS_ATTR_INDEX_ALLOC 0xA0
#define NTFS_ATTR_BITMAP      0xB0
#define NTFS_ATTR_END         0xFFFFFFFF

#define NTFS_BANDERA_EN_USO   0x0001
#define NTFS_BANDERA_DIR      0x0002

#define NTFS_FILE_ATTR_DIR    0x10000000
#define NTFS_FILE_ATTR_HIDDEN 0x00000002
#define NTFS_FILE_ATTR_SYSTEM 0x00000004

// Sector de arranque de partición NTFS (VBR - 512 bytes)
struct __attribute__((packed)) ntfs_vbr {
    uint8_t  jmp_boot[3];
    char     oem_id[8];               // "NTFS    "
    uint16_t bytes_por_sector;        // Usualmente 512
    uint8_t  sectores_por_cluster;     // 1, 2, 4, 8 (4096 bytes con 8)
    uint16_t sectores_reservados;      // Siempre 0 en NTFS
    uint8_t  ceros1[3];
    uint16_t no_usado1;
    uint8_t  descriptor_medio;         // 0xF8
    uint16_t no_usado2;
    uint16_t sectores_por_pista;
    uint16_t num_cabezas;
    uint32_t sectores_ocultos;
    uint32_t no_usado3;
    uint32_t no_usado4;
    uint64_t total_sectores;          // Sectores totales del volumen
    int64_t  lcn_mft;                 // Cluster de inicio de $MFT
    int64_t  lcn_mft_mirr;            // Cluster de inicio de $MFTMirr
    int8_t   clusters_por_registro_mft;// Si negativo, tamaño = 1 << (-val) (ej -10 -> 1024 B)
    uint8_t  ceros2[3];
    int8_t   clusters_por_bloque_indice;
    uint8_t  ceros3[3];
    uint64_t serial_volumen;
    uint32_t checksum;
    uint8_t  bootstrap[426];
    uint16_t firma_arranque;          // 0xAA55
};

// Encabezado de un registro MFT (normalmente 1024 bytes)
struct __attribute__((packed)) ntfs_registro_mft {
    uint32_t magic;                   // "FILE" (0x454C4946)
    uint16_t usa_offset;              // Desplazamiento a la tabla de Fixups (USA)
    uint16_t usa_count;               // Cantidad de palabras en la tabla de Fixups
    uint64_t lsn;                     // Log Sequence Number
    uint16_t numero_secuencia;
    uint16_t conteo_enlaces;
    uint16_t primer_atributo_offset;  // Usualmente 0x30 o 0x38
    uint16_t banderas;                // 0x01 = En uso, 0x02 = Es directorio
    uint32_t tamano_usado;            // Bytes utilizados del registro
    uint32_t tamano_asignado;         // Bytes asignados (1024)
    uint64_t mft_base;                // 0 si es registro primario
    uint16_t proximo_id_attr;
    uint16_t alineacion;
    uint32_t numero_registro_mft;     // Índice dentro de la $MFT
};

// Encabezado genérico de atributo NTFS
struct __attribute__((packed)) ntfs_atributo_cabecera {
    uint32_t tipo;                    // 0x10, 0x30, 0x80, etc.
    uint32_t longitud;                // Longitud total del atributo en bytes
    uint8_t  no_residente;            // 0 = Residente, 1 = No residente
    uint8_t  longitud_nombre;
    uint16_t offset_nombre;
    uint16_t banderas;
    uint16_t id_atributo;
};

// Atributo residente
struct __attribute__((packed)) ntfs_atributo_residente {
    struct ntfs_atributo_cabecera cabecera;
    uint32_t longitud_valor;
    uint16_t offset_valor;
    uint8_t  banderas_indexadas;
    uint8_t  reservado;
};

// Atributo no residente
struct __attribute__((packed)) ntfs_atributo_no_residente {
    struct ntfs_atributo_cabecera cabecera;
    uint64_t primer_vcn;
    uint64_t ultimo_vcn;
    uint16_t offset_data_runs;
    uint16_t unidad_compresion;
    uint32_t padding;
    uint64_t tamano_asignado;
    uint64_t tamano_real;             // Tamaño real del archivo en bytes
    uint64_t tamano_inicializado;
};

// Contenido del atributo $FILE_NAME (0x30)
struct __attribute__((packed)) ntfs_attr_file_name {
    uint64_t referencia_directorio_padre; // Los 48 bits bajos son el índice MFT padre (5 = raíz)
    uint64_t tiempo_creacion;
    uint64_t tiempo_modificacion;
    uint64_t tiempo_mft;
    uint64_t tiempo_acceso;
    uint64_t tamano_asignado;
    uint64_t tamano_real;
    uint32_t banderas_archivo;        // 0x10000000 = Directorio
    uint32_t ea_reparse;
    uint8_t  longitud_nombre;         // En caracteres UTF-16
    uint8_t  namespace_tipo;          // 0=POSIX, 1=Win32, 2=DOS, 3=Win32&DOS
    uint16_t nombre_utf16[1];         // Caracteres UTF-16LE
};

// Estructura de volumen NTFS montado en memoria
struct ntfs_volumen {
    int      montado;
    uint8_t  unidad_msc;
    uint32_t lba_inicio_particion;
    uint16_t bytes_por_sector;
    uint8_t  sectores_por_cluster;
    uint32_t bytes_por_cluster;
    uint32_t tamano_registro_mft;     // Usualmente 1024 bytes
    uint32_t sectores_por_registro_mft;
    int64_t  lcn_mft;                 // Cluster de inicio de $MFT
    uint64_t total_sectores;
    char     etiqueta[32];
};

// --- API PÚBLICA DEL CONTROLADOR NTFS ---
void ntfs_iniciar(void);
int  ntfs_montar(uint8_t unidad_msc);
void ntfs_desmontar(void);
int  ntfs_esta_montado(void);
const struct ntfs_volumen *ntfs_obtener_volumen(void);

int  ntfs_ejecutar_tree(const char *ruta_inicial);
int  ntfs_listar_directorio(const char *ruta);
int  ntfs_leer_archivo_texto(const char *ruta);

#endif // CONTROLADORES_NTFS_H
