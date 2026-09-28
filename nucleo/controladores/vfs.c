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

// --- TABLA DE DESCRIPTORES DE ARCHIVO (Hito 68) ---
static struct vfs_descriptor_archivo g_descriptores[VFS_MAX_FDS] = {{0}};
static uint32_t g_generacion_descriptores = 1;

static uint32_t vfs_generacion_siguiente(uint32_t actual) {
    const uint32_t maximo = (uint32_t)INT32_MAX >> 3;
    actual++;
    return (actual == 0 || actual > maximo) ? 1 : actual;
}

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
    // Cerrar todos los descriptores abiertos antes de desmontar (Hito 68)
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (g_descriptores[i].en_uso) {
            int handle = (int)((g_descriptores[i].generacion << 3) | (uint32_t)i);
            vfs_cerrar(handle);
        }
    }
    if (g_tipo_activo == VFS_FS_FAT32) fat32_desmontar();
    else if (g_tipo_activo == VFS_FS_EXFAT) exfat_desmontar();
    else if (g_tipo_activo == VFS_FS_NTFS) ntfs_desmontar();
    else if (g_tipo_activo == VFS_FS_EXT4) ext4_desmontar();
    g_tipo_activo = VFS_FS_NINGUNO;
    g_unidad_activa = 0xFF;
    g_catalogo_vfs.total = 0;
    g_generacion_descriptores = vfs_generacion_siguiente(g_generacion_descriptores);
}

int vfs_montar(uint8_t unidad_msc) {
    vfs_desmontar();
    g_unidad_activa = unidad_msc;

    // 1. Probar NTFS
    if (ntfs_montar(unidad_msc) == 0) {
        g_tipo_activo = VFS_FS_NTFS;
        return 0;
    }

    // 2. Probar exFAT. Un -2 significa que se identifico exFAT pero
    // fallo una validacion; no se debe reinterpretar esa misma unidad como FAT32.
    int resultado_exfat = exfat_montar(unidad_msc);
    if (resultado_exfat == 0) {
        g_tipo_activo = VFS_FS_EXFAT;
        return 0;
    }
    if (resultado_exfat == -2) {
        consola_imprimir_linea_color(
            "Error: se identifico exFAT, pero su geometria o metadatos no son compatibles.",
            0x00FFAA00);
        return -2;
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
    if (vfs_str_igual_sin_caso(e, "jpg") || vfs_str_igual_sin_caso(e, "jpeg")) {
        return VFS_TIPO_IMAGEN_JPEG;
    }
    if (vfs_str_igual_sin_caso(e, "png")) {
        return VFS_TIPO_IMAGEN_PNG;
    }
    if (vfs_str_igual_sin_caso(e, "mp3")) {
        return VFS_TIPO_MP3;
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

struct vfs_descriptor_archivo *vfs_obtener_descriptor(int fd) {
    if (fd <= 0) return NULL;
    uint32_t token = (uint32_t)fd;
    uint32_t indice = token & (VFS_MAX_FDS - 1);
    uint32_t generacion = token >> 3;
    if (indice >= VFS_MAX_FDS || generacion != g_generacion_descriptores) return NULL;
    if (!g_descriptores[indice].en_uso ||
        g_descriptores[indice].generacion != generacion) return NULL;
    return &g_descriptores[indice];
}

int vfs_usb_leer_sectores(struct vfs_descriptor_archivo *fd, uint32_t lba,
                          uint32_t sectores, void *destino) {
    if (!fd || !fd->en_uso || !destino) return -1;
    uint8_t *p = (uint8_t *)destino;
    while (sectores) {
        if (fd->cancelado) return -2;
        const struct usb_msc_dispositivo *dev = usb_msc_obtener_dispositivo(fd->unidad_msc);
        if (!dev || !dev->activo || !dev->listo || dev->generacion != fd->generacion_msc)
            return -3;
        uint16_t bloque = (sectores > 128) ? 128 : (uint16_t)sectores;
        if ((uint64_t)lba + bloque > UINT32_MAX) return -4;
        if (usb_msc_leer_sectores(fd->unidad_msc, lba, bloque, p) != 0) return -5;
        lba += bloque;
        p += (size_t)bloque * 512;
        sectores -= bloque;
    }
    return 0;
}

int vfs_abrir(const char *ruta, int *fd_out) {
    if (!ruta || !fd_out) return -1;
    *fd_out = -1;

    if (!vfs_esta_montado()) {
        if (vfs_montar(g_unidad_activa) != 0) return -2;
    }

    // Buscar descriptor libre
    int fd_idx = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!g_descriptores[i].en_uso) { fd_idx = i; break; }
    }
    if (fd_idx < 0) return -3; // No hay descriptores disponibles

    // Inicializar descriptor
    struct vfs_descriptor_archivo *fd = &g_descriptores[fd_idx];
    // Limpiar toda la estructura
    uint8_t *p = (uint8_t *)fd;
    for (size_t i = 0; i < sizeof(struct vfs_descriptor_archivo); i++) p[i] = 0;

    fd->fd_id = fd_idx;
    fd->generacion = g_generacion_descriptores;
    fd->fs_tipo = g_tipo_activo;
    fd->unidad_msc = g_unidad_activa;
    const struct usb_msc_dispositivo *medio = usb_msc_obtener_dispositivo(g_unidad_activa);
    if (!medio || !medio->activo || !medio->listo) {
        fd->en_uso = 0;
        return -5;
    }
    fd->generacion_msc = medio->generacion;
    fd->bufer_unidad_logica = 0xFFFFFFFF; // Nada en cache

    int res = -10;
    switch (g_tipo_activo) {
        case VFS_FS_FAT32: res = fat32_abrir_stream(ruta, fd); break;
        case VFS_FS_EXFAT: res = exfat_abrir_stream(ruta, fd); break;
        case VFS_FS_NTFS:  res = ntfs_abrir_stream(ruta, fd);  break;
        case VFS_FS_EXT4:  res = ext4_abrir_stream(ruta, fd);  break;
        default: return -4;
    }

    if (res != 0) {
        // Limpiar en caso de error
        if (fd->bufer_cache) { liberar_memoria(fd->bufer_cache); fd->bufer_cache = NULL; }
        fd->en_uso = 0;
        return res;
    }

    fd->en_uso = 1;
    *fd_out = (int)((g_generacion_descriptores << 3) | (uint32_t)fd_idx);
    return 0;
}

int64_t vfs_leer(int fd, void *buf, size_t cantidad) {
    struct vfs_descriptor_archivo *desc = vfs_obtener_descriptor(fd);
    if (!desc || !buf || cantidad == 0) return -1;
    if (desc->cancelado) return -3;
    const struct usb_msc_dispositivo *dispositivo = usb_msc_obtener_dispositivo(desc->unidad_msc);
    if (!dispositivo || !dispositivo->activo || !dispositivo->listo) {
        desc->cancelado = 1;
        return -4;
    }
    if (dispositivo->generacion != desc->generacion_msc) {
        desc->cancelado = 1;
        return -4;
    }

    int64_t leidos;
    switch (desc->fs_tipo) {
        case VFS_FS_FAT32: leidos = fat32_leer_stream(desc, buf, cantidad); break;
        case VFS_FS_EXFAT: leidos = exfat_leer_stream(desc, buf, cantidad); break;
        case VFS_FS_NTFS:  leidos = ntfs_leer_stream(desc, buf, cantidad); break;
        case VFS_FS_EXT4:  leidos = ext4_leer_stream(desc, buf, cantidad); break;
        default: return -2;
    }
    if (leidos < 0) {
        dispositivo = usb_msc_obtener_dispositivo(desc->unidad_msc);
        if (!dispositivo || !dispositivo->activo || !dispositivo->listo ||
            dispositivo->generacion != desc->generacion_msc) desc->cancelado = 1;
    }
    return leidos;
}

int64_t vfs_leer_en(int fd, uint64_t offset, void *buf, size_t cantidad) {
    struct vfs_descriptor_archivo *desc = vfs_obtener_descriptor(fd);
    if (!desc || !buf || !cantidad) return -1;
    if (offset > desc->tamano) return 0;
    uint64_t posicion_anterior = desc->posicion;
    desc->posicion = offset;
    int64_t leidos = vfs_leer(fd, buf, cantidad);
    /* Un error de medio deja el descriptor cancelado; aun así restauramos el cursor. */
    desc->posicion = posicion_anterior;
    return leidos;
}

int vfs_cancelar(int fd) {
    struct vfs_descriptor_archivo *desc = vfs_obtener_descriptor(fd);
    if (!desc) return -1;
    desc->cancelado = 1;
    return 0;
}

int64_t vfs_buscar(int fd, int64_t offset, int origen) {
    struct vfs_descriptor_archivo *desc = vfs_obtener_descriptor(fd);
    if (!desc) return -1;
    uint64_t base;
    if (origen == VFS_SEEK_SET) base = 0;
    else if (origen == VFS_SEEK_CUR) base = desc->posicion;
    else if (origen == VFS_SEEK_END) base = desc->tamano;
    else return -1;

    uint64_t destino;
    if (offset < 0) {
        uint64_t retroceso = (uint64_t)(-(offset + 1)) + 1;
        if (retroceso > base) return -1;
        destino = base - retroceso;
    } else {
        if (base > desc->tamano || (uint64_t)offset > desc->tamano - base) return -1;
        destino = base + (uint64_t)offset;
    }
    if (destino > desc->tamano || destino > INT64_MAX) return -1;
    desc->posicion = destino;
    desc->bufer_unidad_logica = 0xFFFFFFFF;
    return (int64_t)destino;
}

uint64_t vfs_tamano_fd(int fd) {
    struct vfs_descriptor_archivo *desc = vfs_obtener_descriptor(fd);
    if (!desc) return 0;
    return desc->tamano;
}

uint64_t vfs_posicion_fd(int fd) {
    struct vfs_descriptor_archivo *desc = vfs_obtener_descriptor(fd);
    if (!desc) return 0;
    return desc->posicion;
}

void vfs_cerrar(int fd) {
    struct vfs_descriptor_archivo *desc = vfs_obtener_descriptor(fd);
    if (!desc) return;

    switch (desc->fs_tipo) {
        case VFS_FS_FAT32: fat32_cerrar_stream(desc); break;
        case VFS_FS_EXFAT: exfat_cerrar_stream(desc); break;
        case VFS_FS_NTFS:  ntfs_cerrar_stream(desc);  break;
        case VFS_FS_EXT4:  ext4_cerrar_stream(desc);  break;
        default:
            if (desc->bufer_cache) { liberar_memoria(desc->bufer_cache); desc->bufer_cache = NULL; }
            break;
    }
    desc->bufer_tamano = 0;
    desc->bufer_bytes_validos = 0;
    desc->bufer_unidad_logica = 0xFFFFFFFF;
    desc->en_uso = 0;
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
