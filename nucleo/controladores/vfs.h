#ifndef CONTROLADORES_VFS_H
#define CONTROLADORES_VFS_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// TAEK OS - CAPA DE SISTEMA DE ARCHIVOS VIRTUAL (VFS) (Hito 50)
// Selector Dinámico de Sistemas de Archivos: FAT32, exFAT y NTFS
// ============================================================================

enum vfs_tipo_fs {
    VFS_FS_NINGUNO = 0,
    VFS_FS_FAT32   = 1,
    VFS_FS_EXFAT   = 2,
    VFS_FS_NTFS    = 3
};

// Inicializa el subsistema VFS
void vfs_iniciar(void);

// Detecta el formato (FAT32, exFAT o NTFS) y monta el controlador correspondiente
int  vfs_montar(uint8_t unidad_msc);

// Desmonta el volumen actualmente activo
void vfs_desmontar(void);

// Consulta si hay un sistema de archivos montado
int  vfs_esta_montado(void);

// Retorna el tipo de sistema de archivos activo
enum vfs_tipo_fs vfs_obtener_tipo_fs(void);

// Retorna el nombre legible del sistema de archivos ("FAT32", "exFAT", "NTFS", "Ninguno")
const char *vfs_obtener_nombre_fs(void);

// Ejecuta la exploración jerárquica 'tree' sobre el sistema de archivos activo
int  vfs_ejecutar_tree(const char *ruta_inicial);

// Lista los contenidos de un directorio ('ls' / 'dir')
int  vfs_listar_directorio(const char *ruta);

// Lee y despliega el contenido de un archivo de texto ('cat' / 'leer')
int  vfs_leer_archivo_texto(const char *ruta);

#endif // CONTROLADORES_VFS_H
