#ifndef CONTROLADORES_VFS_H
#define CONTROLADORES_VFS_H

#include <stdint.h>
#include <stddef.h>

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
    VFS_TIPO_DIR        = 4
};

#define VFS_MAX_CATALOGO 128

struct vfs_entrada {
    char                  nombre[256];
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
