#include "vfs.h"
#include "fat32.h"
#include "exfat.h"
#include "ntfs.h"
#include "usb_msc.h"
#include "consola.h"

// ============================================================================
// TAEK OS - IMPLEMENTACIÓN DEL SUBSISTEMA VFS (Hito 50)
// ============================================================================

static enum vfs_tipo_fs g_tipo_activo = VFS_FS_NINGUNO;
static uint8_t g_unidad_activa = 0;

void vfs_iniciar(void) {
    g_tipo_activo = VFS_FS_NINGUNO;
    fat32_iniciar();
    exfat_iniciar();
    ntfs_iniciar();
}

int vfs_esta_montado(void) {
    return (g_tipo_activo != VFS_FS_NINGUNO);
}

enum vfs_tipo_fs vfs_obtener_tipo_fs(void) {
    return g_tipo_activo;
}

const char *vfs_obtener_nombre_fs(void) {
    switch (g_tipo_activo) {
        case VFS_FS_FAT32: return "FAT32";
        case VFS_FS_EXFAT: return "exFAT";
        case VFS_FS_NTFS:  return "NTFS";
        default:           return "Ninguno";
    }
}

void vfs_desmontar(void) {
    if (g_tipo_activo == VFS_FS_FAT32) fat32_desmontar();
    else if (g_tipo_activo == VFS_FS_EXFAT) exfat_desmontar();
    else if (g_tipo_activo == VFS_FS_NTFS) ntfs_desmontar();
    g_tipo_activo = VFS_FS_NINGUNO;
}

int vfs_montar(uint8_t unidad_msc) {
    vfs_desmontar();
    g_unidad_activa = unidad_msc;

    // 1. Probar NTFS
    if (ntfs_montar(unidad_msc) == 0) {
        g_tipo_activo = VFS_FS_NTFS;
        return 0;
    }

    // 2. Probar exFAT
    if (exfat_montar(unidad_msc) == 0) {
        g_tipo_activo = VFS_FS_EXFAT;
        return 0;
    }

    // 3. Probar FAT32
    if (fat32_montar(unidad_msc) == 0) {
        g_tipo_activo = VFS_FS_FAT32;
        return 0;
    }

    return -1;
}

int vfs_ejecutar_tree(const char *ruta_inicial) {
    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) {
            consola_imprimir_linea_color("Error: No se encontró ningún sistema de archivos soportado (FAT32, exFAT o NTFS) en la unidad.", 0x00FFAA00);
            return -1;
        }
    }

    switch (g_tipo_activo) {
        case VFS_FS_NTFS:  return ntfs_ejecutar_tree(ruta_inicial);
        case VFS_FS_EXFAT: return exfat_ejecutar_tree(ruta_inicial);
        case VFS_FS_FAT32: return fat32_ejecutar_tree(ruta_inicial);
        default:           return -1;
    }
}

int vfs_listar_directorio(const char *ruta) {
    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) {
            consola_imprimir_linea_color("Error: No hay sistema de archivos montado.", 0x00FFAA00);
            return -1;
        }
    }

    switch (g_tipo_activo) {
        case VFS_FS_NTFS:  return ntfs_listar_directorio(ruta);
        case VFS_FS_EXFAT: return exfat_listar_directorio(ruta);
        case VFS_FS_FAT32: return fat32_listar_directorio(ruta);
        default:           return -1;
    }
}

int vfs_leer_archivo_texto(const char *ruta) {
    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) {
            consola_imprimir_linea_color("Error: No hay sistema de archivos montado.", 0x00FFAA00);
            return -1;
        }
    }

    switch (g_tipo_activo) {
        case VFS_FS_NTFS:  return ntfs_leer_archivo_texto(ruta);
        case VFS_FS_EXFAT: return exfat_leer_archivo_texto(ruta);
        case VFS_FS_FAT32: return fat32_leer_archivo_texto(ruta);
        default:           return -1;
    }
}
