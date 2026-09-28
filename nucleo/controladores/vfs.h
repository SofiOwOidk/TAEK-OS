#ifndef CONTROLADORES_VFS_H
#define CONTROLADORES_VFS_H

#include <stdint.h>
#include <stddef.h>
#include "particiones.h"

// ============================================================================
// TAEK OS - CAPA DE SISTEMA DE ARCHIVOS VIRTUAL (VFS) (Hito 67)
// Selector Dinámico de Sistemas de Archivos: FAT32, exFAT, NTFS y ext4
// Catálogo Indexado de Archivos y Soporte Binario para Reproductor y Visor
// ============================================================================

enum vfs_tipo_fs {
    VFS_FS_NINGUNO = 0,
    VFS_FS_FAT32   = 1,
    VFS_FS_EXFAT   = 2,
    VFS_FS_NTFS    = 3,
    VFS_FS_EXT4    = 4
};

enum vfs_tipo_nodo {
    VFS_NODO_ARCHIVO    = 0,
    VFS_NODO_DIRECTORIO = 1
};

enum vfs_tipo_archivo {
    VFS_TIPO_OTRO       = 0,
    VFS_TIPO_MP4        = 1,
    VFS_TIPO_IMAGEN_BMP = 2,
    VFS_TIPO_TEXTO      = 3,
    VFS_TIPO_DIR        = 4,
    VFS_TIPO_MP3        = 5,
    VFS_TIPO_IMAGEN_JPEG = 6,
    VFS_TIPO_IMAGEN_PNG  = 7
};

// --- DESCRIPTOR DE ARCHIVO VFS PARA LECTURA POR POSICIÓN (Hito 68) ---

#define VFS_MAX_FDS 8
#define VFS_STREAM_CACHE_MAX (1024U * 1024U)

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

// Tamaño del cursor específico de FS más grande (NTFS con extents)
// ntfs_cursor_archivo: ~1024 + 64*24 + varios campos ≈ 2700 bytes
#define VFS_CURSOR_TAMANO_MAX 3072

struct vfs_descriptor_archivo {
    int               en_uso;           // 1 = descriptor activo
    int               fd_id;            // Identificador 0..VFS_MAX_FDS-1
    uint32_t          generacion;        // Invalida handles después de desmontar el volumen
    volatile uint8_t  cancelado;         // Cancelación cooperativa entre bloques
    enum vfs_tipo_fs  fs_tipo;          // Sistema de archivos del descriptor
    uint64_t          posicion;         // Posición actual del cursor en bytes
    uint64_t          tamano;           // Tamaño total del archivo en bytes
    uint8_t           unidad_msc;       // Unidad USB MSC asociada
    uint64_t          generacion_msc;   // Identidad del medio al abrir
    // Búfer de lectura por descriptor (evita colisiones con búferes globales)
    uint8_t          *bufer_cache;      // Búfer dinámico para cluster/bloque actual
    uint32_t          bufer_tamano;     // Tamaño del búfer asignado
    uint32_t          bufer_unidad_logica; // Índice lógico de cluster/bloque cacheado
    uint32_t          bufer_bytes_validos; // Bytes válidos en el búfer
    // Cursor específico del sistema de archivos (unión opaca)
    uint8_t           cursor[VFS_CURSOR_TAMANO_MAX] __attribute__((aligned(8)));
};

#define VFS_MAX_CATALOGO 128

struct vfs_entrada {
    char                  nombre[1024];
    uint64_t              tamano;
    enum vfs_tipo_nodo    tipo_nodo;
    enum vfs_tipo_archivo tipo_archivo;
};

struct vfs_catalogo {
    struct vfs_entrada entradas[VFS_MAX_CATALOGO];
    int                total;
};

// Inicializa el subsistema VFS
void vfs_iniciar(void);

// Detecta el formato (FAT32, exFAT, NTFS o ext4) y monta el controlador correspondiente
int  vfs_montar(uint8_t unidad_msc);
int vfs_montar_particion(uint8_t unidad_msc,unsigned numero);
const struct particiones *vfs_obtener_particiones(void);
unsigned vfs_obtener_particion_activa(void);
int vfs_ultimo_error(void);
void vfs_notificar_desconexion(uint8_t);
int vfs_listar_pagina(const char *,unsigned);
const char *vfs_obtener_ruta_catalogo(void);
enum vfs_tipo_archivo vfs_inspeccionar_archivo(const char *,int *);
int vfs_consultar_ruta(const char *,enum vfs_tipo_nodo *);

// Desmonta el volumen actualmente activo
void vfs_desmontar(void);

// Consulta si hay un sistema de archivos montado
int  vfs_esta_montado(void);

// Retorna la unidad USB MSC actualmente activa en VFS
uint8_t vfs_obtener_unidad_activa(void);

// Retorna el tipo de sistema de archivos activo
enum vfs_tipo_fs vfs_obtener_tipo_fs(void);

// Retorna el nombre legible del sistema de archivos ("FAT32", "exFAT", "NTFS", "ext4", "Ninguno")
const char *vfs_obtener_nombre_fs(void);

// Catálogo indexado de archivos en Ring 0
const struct vfs_catalogo *vfs_obtener_catalogo(void);
const struct vfs_entrada  *vfs_obtener_entrada_catalogo(int indice_1based);
const struct vfs_entrada  *vfs_buscar_entrada_catalogo(const char *nombre);
void                       vfs_limpiar_catalogo(void);
void                       vfs_agregar_entrada_catalogo(const char *nombre, uint64_t tamano, enum vfs_tipo_nodo tipo_nodo);
enum vfs_tipo_archivo      vfs_detectar_tipo_archivo(const char *nombre);

// --- API DE DESCRIPTORES DE ARCHIVO PARA LECTURA POR POSICIÓN (Hito 68) ---
// Permite leer archivos desde disco sin cargar el contenido completo en RAM.
// Cada descriptor mantiene un cursor con posición y búfer de cluster/bloque.

// Abre un archivo y retorna un handle con generación. Retorna 0 en éxito, negativo en error.
int      vfs_abrir(const char *ruta, int *fd_out);

// Lee hasta 'cantidad' bytes desde la posición actual del descriptor.
// Retorna los bytes efectivamente leídos, 0 en EOF, negativo en error.
int64_t  vfs_leer(int fd, void *buf, size_t cantidad);

// Lee desde un offset de 64 bits sin cambiar la posición secuencial del descriptor.
int64_t  vfs_leer_en(int fd, uint64_t offset, void *buf, size_t cantidad);

// Solicita detener una lectura/reproducción en curso. El lector comprueba el flag por bloque.
int      vfs_cancelar(int fd);

// Mueve el cursor del descriptor. origen: VFS_SEEK_SET, VFS_SEEK_CUR, VFS_SEEK_END.
// Retorna la nueva posición absoluta, o negativo en error.
int64_t  vfs_buscar(int fd, int64_t offset, int origen);

// Retorna el tamaño total del archivo asociado al descriptor.
uint64_t vfs_tamano_fd(int fd);

// Retorna la posición actual del cursor del descriptor.
uint64_t vfs_posicion_fd(int fd);

// Cierra el descriptor y libera los recursos asociados.
void     vfs_cerrar(int fd);

// Obtiene un puntero al descriptor (para uso interno de los controladores FS).
struct vfs_descriptor_archivo *vfs_obtener_descriptor(int fd);

// Divide las lecturas de datos en transacciones compatibles con el pool DMA MSC.
int      vfs_usb_leer_sectores(struct vfs_descriptor_archivo *fd, uint32_t lba,
                               uint32_t sectores, void *destino);

// Ejecuta la exploración jerárquica 'tree' sobre el sistema de archivos activo
int  vfs_ejecutar_tree(const char *ruta_inicial);

// Lista los contenidos de un directorio ('ls' / 'dir') y actualiza el catálogo indexado
int  vfs_listar_directorio(const char *ruta);

// Lee y despliega el contenido de un archivo de texto ('cat' / 'leer')
int  vfs_leer_archivo_texto(const char *ruta);

// Lee un archivo binario completo en memoria desde la unidad activa
int  vfs_leer_archivo_binario(const char *ruta, void **buf_out, size_t *tam_out, int *es_dma_out);

// Libera la memoria asignada por vfs_leer_archivo_binario
void vfs_liberar_archivo_binario(void *buf, size_t tam, int es_dma);

// Crea un nuevo archivo con el contenido especificado ('touch' / 'escribir')
int  vfs_crear_archivo(const char *nombre, const char *contenido);

// Crea un nuevo directorio ('mkdir')
int  vfs_crear_directorio(const char *nombre);

#endif // CONTROLADORES_VFS_H
