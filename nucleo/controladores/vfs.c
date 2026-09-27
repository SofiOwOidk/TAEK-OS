#include "vfs.h"
#include "fat32.h"
#include "exfat.h"
#include "ntfs.h"
#include "ext4.h"
#include "usb_msc.h"
#include "consola.h"
#include "../base/memoria.h"
#include "../base/dma.h"

// ============================================================================
// TAEK OS - IMPLEMENTACIÓN DEL SUBSISTEMA VFS (Hito 67)
// Selector Dinámico de Sistemas de Archivos: FAT32, exFAT, NTFS y ext4
// Catálogo Indexado de Archivos y Lectura Binaria para Multimedia
// ============================================================================

static enum vfs_tipo_fs g_tipo_activo = VFS_FS_NINGUNO;
static uint8_t g_unidad_activa = 0;
static struct vfs_catalogo g_catalogo_vfs = {0};

void vfs_iniciar(void) {
    g_tipo_activo = VFS_FS_NINGUNO;
    g_catalogo_vfs.total = 0;
    fat32_iniciar();
    exfat_iniciar();
    ntfs_iniciar();
    ext4_iniciar();
}

int vfs_esta_montado(void) {
    return (g_tipo_activo != VFS_FS_NINGUNO);
}

uint8_t vfs_obtener_unidad_activa(void) {
    return g_unidad_activa;
}

enum vfs_tipo_fs vfs_obtener_tipo_fs(void) {
    return g_tipo_activo;
}

const char *vfs_obtener_nombre_fs(void) {
    switch (g_tipo_activo) {
        case VFS_FS_FAT32: return "FAT32";
        case VFS_FS_EXFAT: return "exFAT";
        case VFS_FS_NTFS:  return "NTFS";
        case VFS_FS_EXT4:  return "ext4";
        default:           return "Ninguno";
    }
}

void vfs_desmontar(void) {
    if (g_tipo_activo == VFS_FS_FAT32) fat32_desmontar();
    else if (g_tipo_activo == VFS_FS_EXFAT) exfat_desmontar();
    else if (g_tipo_activo == VFS_FS_NTFS) ntfs_desmontar();
    else if (g_tipo_activo == VFS_FS_EXT4) ext4_desmontar();
    g_tipo_activo = VFS_FS_NINGUNO;
    g_unidad_activa = 0xFF;
    g_catalogo_vfs.total = 0;
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

    // 3. Probar ext4 (Linux)
    if (ext4_montar(unidad_msc) == 0) {
        g_tipo_activo = VFS_FS_EXT4;
        return 0;
    }

    // 4. Probar FAT32
    if (fat32_montar(unidad_msc) == 0) {
        g_tipo_activo = VFS_FS_FAT32;
        return 0;
    }

    return -1;
}

// --- GESTIÓN DEL CATÁLOGO INDEXADO ---

const struct vfs_catalogo *vfs_obtener_catalogo(void) {
    return &g_catalogo_vfs;
}

const struct vfs_entrada *vfs_obtener_entrada_catalogo(int indice_1based) {
    if (indice_1based < 1 || indice_1based > g_catalogo_vfs.total) return NULL;
    return &g_catalogo_vfs.entradas[indice_1based - 1];
}

static int vfs_str_igual_sin_caso(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    while (*s1 && *s2) {
        char c1 = *s1++;
        char c2 = *s2++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
    }
    return (*s1 == '\0' && *s2 == '\0');
}

const struct vfs_entrada *vfs_buscar_entrada_catalogo(const char *nombre) {
    if (!nombre || *nombre == '\0') return NULL;
    for (int i = 0; i < g_catalogo_vfs.total; i++) {
        if (vfs_str_igual_sin_caso(g_catalogo_vfs.entradas[i].nombre, nombre)) {
            return &g_catalogo_vfs.entradas[i];
        }
    }
    return NULL;
}

void vfs_limpiar_catalogo(void) {
    g_catalogo_vfs.total = 0;
}

enum vfs_tipo_archivo vfs_detectar_tipo_archivo(const char *nombre) {
    if (!nombre) return VFS_TIPO_OTRO;
    int len = 0;
    while (nombre[len]) len++;
    if (len < 4) return VFS_TIPO_OTRO;

    int punto = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (nombre[i] == '.') { punto = i; break; }
    }
    if (punto < 0) return VFS_TIPO_OTRO;

    const char *ext = &nombre[punto + 1];
    int ext_len = len - (punto + 1);
    if (ext_len >= 8) return VFS_TIPO_OTRO;

    char e[8];
    for (int i = 0; i < ext_len; i++) {
        char c = ext[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        e[i] = c;
    }
    e[ext_len] = '\0';

    if (vfs_str_igual_sin_caso(e, "mp4") || vfs_str_igual_sin_caso(e, "m4v") ||
        vfs_str_igual_sin_caso(e, "h264") || vfs_str_igual_sin_caso(e, "264")) {
        return VFS_TIPO_MP4;
    }
    if (vfs_str_igual_sin_caso(e, "bmp") || vfs_str_igual_sin_caso(e, "dib")) {
        return VFS_TIPO_IMAGEN_BMP;
    }
    if (vfs_str_igual_sin_caso(e, "txt") || vfs_str_igual_sin_caso(e, "cfg") ||
        vfs_str_igual_sin_caso(e, "log") || vfs_str_igual_sin_caso(e, "ini") ||
        vfs_str_igual_sin_caso(e, "inf") || vfs_str_igual_sin_caso(e, "c") ||
        vfs_str_igual_sin_caso(e, "h") || vfs_str_igual_sin_caso(e, "md") ||
        vfs_str_igual_sin_caso(e, "sh") || vfs_str_igual_sin_caso(e, "bat") ||
        vfs_str_igual_sin_caso(e, "json") || vfs_str_igual_sin_caso(e, "xml")) {
        return VFS_TIPO_TEXTO;
    }
    return VFS_TIPO_OTRO;
}

void vfs_agregar_entrada_catalogo(const char *nombre, uint64_t tamano, enum vfs_tipo_nodo tipo_nodo) {
    if (!nombre || g_catalogo_vfs.total >= VFS_MAX_CATALOGO) return;

    // Omitir directorios relativos '.' y '..'
    if (nombre[0] == '.' && (nombre[1] == '\0' || (nombre[1] == '.' && nombre[2] == '\0'))) {
        return;
    }

    struct vfs_entrada *e = &g_catalogo_vfs.entradas[g_catalogo_vfs.total++];
    int i = 0;
    while (nombre[i] && i < 255) {
        e->nombre[i] = nombre[i];
        i++;
    }
    e->nombre[i] = '\0';
    e->tamano = tamano;
    e->tipo_nodo = tipo_nodo;
    e->tipo_archivo = (tipo_nodo == VFS_NODO_DIRECTORIO) ? VFS_TIPO_DIR : vfs_detectar_tipo_archivo(nombre);
}

int vfs_ejecutar_tree(const char *ruta_inicial) {
    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) {
            consola_imprimir_linea_color("Error: No se encontró ningún sistema de archivos soportado en la unidad.", 0x00FFAA00);
            return -1;
        }
    }

    switch (g_tipo_activo) {
        case VFS_FS_NTFS:  return ntfs_ejecutar_tree(ruta_inicial);
        case VFS_FS_EXFAT: return exfat_ejecutar_tree(ruta_inicial);
        case VFS_FS_FAT32: return fat32_ejecutar_tree(ruta_inicial);
        case VFS_FS_EXT4:  return ext4_ejecutar_tree(ruta_inicial);
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
        case VFS_FS_EXT4:  return ext4_listar_directorio(ruta);
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
        case VFS_FS_EXT4:  return ext4_leer_archivo_texto(ruta);
        default:           return -1;
    }
}

int vfs_leer_archivo_binario(const char *ruta, void **buf_out, size_t *tam_out, int *es_dma_out) {
    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) {
            consola_imprimir_linea_color("Error: No hay sistema de archivos montado.", 0x00FFAA00);
            return -1;
        }
    }

    switch (g_tipo_activo) {
        case VFS_FS_NTFS:  return ntfs_leer_archivo_binario(ruta, buf_out, tam_out, es_dma_out);
        case VFS_FS_EXFAT: return exfat_leer_archivo_binario(ruta, buf_out, tam_out, es_dma_out);
        case VFS_FS_FAT32: return fat32_leer_archivo_binario(ruta, buf_out, tam_out, es_dma_out);
        case VFS_FS_EXT4:  return ext4_leer_archivo_binario(ruta, buf_out, tam_out, es_dma_out);
        default:           return -1;
    }
}

void vfs_liberar_archivo_binario(void *buf, size_t tam, int es_dma) {
    if (!buf) return;
    if (es_dma) {
        uint64_t hhdm = memoria_obtener_hhdm_offset();
        uint64_t phys = (uint64_t)(uintptr_t)buf - hhdm;
        dma_liberar_bufer_contiguo(buf, phys, tam);
    } else {
        liberar_memoria(buf);
    }
}

int vfs_crear_archivo(const char *nombre, const char *contenido) {
    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) {
            consola_imprimir_linea_color("Error: No hay sistema de archivos montado para escribir.", 0x00FFAA00);
            return -1;
        }
    }

    uint32_t len = 0;
    if (contenido) {
        while (contenido[len]) len++;
    }

    switch (g_tipo_activo) {
        case VFS_FS_EXT4:
            return ext4_crear_archivo(nombre, (const uint8_t *)contenido, len);
        case VFS_FS_FAT32:
            return fat32_crear_archivo(nombre, (const uint8_t *)contenido, len);
        case VFS_FS_EXFAT:
            return exfat_crear_archivo(nombre, (const uint8_t *)contenido, len);
        case VFS_FS_NTFS:
            consola_imprimir_linea_color("  [!] NTFS montado en modo SOLO LECTURA. No es posible escribir en esta versión.", 0x00FFAA00);
            return -2;
        default:
            return -3;
    }
}

int vfs_crear_directorio(const char *nombre) {
    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) {
            consola_imprimir_linea_color("Error: No hay sistema de archivos montado para crear directorio.", 0x00FFAA00);
            return -1;
        }
    }

    switch (g_tipo_activo) {
        case VFS_FS_EXT4:
            return ext4_crear_directorio(nombre);
        case VFS_FS_FAT32:
            return fat32_crear_directorio(nombre);
        case VFS_FS_EXFAT:
            return exfat_crear_directorio(nombre);
        case VFS_FS_NTFS:
            consola_imprimir_linea_color("  [!] NTFS montado en modo SOLO LECTURA.", 0x00FFAA00);
            return -2;
        default:
            return -3;
    }
}
