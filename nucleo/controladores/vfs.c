#include "fat_lector.h"
#include "teclado.h"
#include "xhci.h"
#include "audio_ac97.h"
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
static struct particiones g_particiones;
static struct particion g_particion_activa;
static int g_error_volumen,g_desconectado;
static char g_ruta_catalogo[4096];
static struct vfs_catalogo g_catalogo_vfs = {0};

// --- TABLA DE DESCRIPTORES DE ARCHIVO (Hito 68) ---
static struct vfs_descriptor_archivo g_descriptores[VFS_MAX_FDS] = {{0}};
static uint32_t g_generacion_descriptores = 1;
static int g_cancelacion_lectura;
static int servir_lectura(void){
    xhci_sondeo();audio_ac97_actualizar();
    if(consola_sondear_cancelacion())g_cancelacion_lectura=1;
    if(g_cancelacion_lectura)for(unsigned i=0;i<VFS_MAX_FDS;i++)if(g_descriptores[i].en_uso)g_descriptores[i].cancelado=1;
    return g_cancelacion_lectura;
}

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
    if(g_tipo_activo==VFS_FS_NINGUNO || g_desconectado)return 0;
    const struct usb_msc_dispositivo *d=usb_msc_obtener_dispositivo(g_unidad_activa);
    if(!d || !d->activo || !d->listo || d->generacion!=g_particion_activa.generacion_msc){vfs_notificar_desconexion(g_unidad_activa);return 0;}
    return 1;
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
    g_desconectado=0;g_ruta_catalogo[0]=0;
    g_unidad_activa = 0xFF;
    g_catalogo_vfs.total = 0;
    g_generacion_descriptores = vfs_generacion_siguiente(g_generacion_descriptores);
}

int vfs_montar(uint8_t unidad_msc) {
    return vfs_montar_particion(unidad_msc,0);
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
    while (nombre[i] && i < (int)sizeof(e->nombre)-1) {
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
    return vfs_listar_pagina(ruta,0);
}

int vfs_leer_archivo_texto(const char *ruta) {
    int fd;int r=vfs_abrir(ruta,&fd);if(r)return r;
    uint8_t buf[512];uint64_t total=0;int64_t n;
    while(total<65536 && (n=vfs_leer(fd,buf,sizeof(buf)))>0){
        for(int64_t i=0;i<n;i++){uint8_t c=buf[i];if(c=='\n')consola_escribir_caracter('\n');else if(c=='\t')consola_imprimir("    ");else if(c=='\r')continue;else if(c>=32 && c!=127)consola_escribir_caracter((char)c);else consola_escribir_caracter('.');}total+=(uint64_t)n;
    }
    if(total>=65536)consola_imprimir_linea("\n[Texto limitado a 64 KiB]");
    r=total<65536 && n<0?(int)n:0;vfs_cerrar(fd);return r;
}

int vfs_leer_archivo_binario(const char *ruta, void **buf_out, size_t *tam_out, int *es_dma_out) {
    if(!buf_out || !tam_out || !es_dma_out)return -1;*buf_out=0;*tam_out=0;*es_dma_out=0;
    int fd;int r=vfs_abrir(ruta,&fd);if(r)return r;uint64_t n=vfs_tamano_fd(fd);
    if(n>32u*1024*1024){vfs_cerrar(fd);return VOLUMEN_NO_SOPORTADO;}
    void *buf=asignar_memoria(n?(size_t)n:1);if(!buf){vfs_cerrar(fd);return -1;}
    uint64_t hechos=0;while(hechos<n){size_t k=n-hechos>65536?65536:(size_t)(n-hechos);int64_t leidos=vfs_leer(fd,(uint8_t *)buf+hechos,k);if(leidos!=(int64_t)k){liberar_memoria(buf);vfs_cerrar(fd);return leidos<0?(int)leidos:VOLUMEN_CORRUPTO;}hechos+=(uint64_t)leidos;}
    vfs_cerrar(fd);*buf_out=buf;*tam_out=(size_t)n;return 0;
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
    if(lba<g_particion_activa.inicio || (uint64_t)lba-g_particion_activa.inicio>g_particion_activa.sectores || sectores>g_particion_activa.sectores-((uint64_t)lba-g_particion_activa.inicio))return VOLUMEN_CORRUPTO;
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
    g_cancelacion_lectura=0;
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
    if (!desc || !buf) return -1;
    if(!cantidad)return 0;
    if (desc->cancelado) return g_desconectado?VOLUMEN_DESCONECTADO:VOLUMEN_CANCELADO;
    const struct usb_msc_dispositivo *dispositivo = usb_msc_obtener_dispositivo(desc->unidad_msc);
    if (!dispositivo || !dispositivo->activo || !dispositivo->listo) {
        desc->cancelado = 1;
        vfs_notificar_desconexion(desc->unidad_msc);
        return VOLUMEN_DESCONECTADO;
    }
    if (dispositivo->generacion != desc->generacion_msc) {
        desc->cancelado = 1;
        vfs_notificar_desconexion(desc->unidad_msc);
        return VOLUMEN_DESCONECTADO;
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
    if (!desc || !buf) return -1;
    if(!cantidad)return 0;
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
    (void)nombre;(void)contenido;
    consola_imprimir_linea("Volumen abierto en solo lectura; escritor experimental fuera de este montaje.");
    return VOLUMEN_SOLO_LECTURA;
}
int vfs_crear_directorio(const char *nombre) {
    (void)nombre;return VOLUMEN_SOLO_LECTURA;
}
const struct particiones *vfs_obtener_particiones(void){return &g_particiones;}
unsigned vfs_obtener_particion_activa(void){return g_particion_activa.numero;}
int vfs_ultimo_error(void){return g_error_volumen;}
const char *vfs_obtener_ruta_catalogo(void){return g_ruta_catalogo;}
void vfs_notificar_desconexion(uint8_t unidad){
    if(unidad!=g_unidad_activa)return;g_desconectado=1;g_catalogo_vfs.total=0;g_ruta_catalogo[0]=0;
    for(unsigned i=0;i<VFS_MAX_FDS;i++)if(g_descriptores[i].en_uso && g_descriptores[i].unidad_msc==unidad)g_descriptores[i].cancelado=1;
    /* No liberar buffers desde el callback de hotplug: el consumidor aún los posee. */
}
int vfs_montar_particion(uint8_t unidad,unsigned numero){
    g_cancelacion_lectura=0;particiones_configurar_servicio(servir_lectura);
    vfs_desmontar();g_unidad_activa=unidad;g_error_volumen=particiones_descubrir(unidad,&g_particiones);
    if(g_error_volumen)goto fallo;
    unsigned candidatas=0,elegida=0;
    for(unsigned i=0;i<g_particiones.total;i++){
        struct particion *p=&g_particiones.entradas[i];
        if(!p->resultado && (p->formato==VOLUMEN_FAT32 || p->formato==VOLUMEN_EXFAT)){
            struct fat_lector_volumen *prueba=asignar_memoria_cero(sizeof(*prueba));
            if(!prueba)p->resultado=VOLUMEN_NO_SOPORTADO;
            else {p->resultado=fat_lector_montar(prueba,p);fat_lector_desmontar(prueba);liberar_memoria(prueba);}
        }
        consola_imprimir("USB ");consola_imprimir_dec(unidad);consola_imprimir(" · Particion ");consola_imprimir_dec(p->numero);
        consola_imprimir(" · ");consola_imprimir(volumen_nombre(p->formato));consola_imprimir(" · ");consola_imprimir_linea(volumen_error(p->resultado));
        if(numero && p->numero==numero){candidatas=1;elegida=i;break;}
        if(!numero && p->formato!=VOLUMEN_DESCONOCIDO && !p->resultado){candidatas++;elegida=i;}
    }
    if(candidatas!=1){g_error_volumen=candidatas?VOLUMEN_ELEGIR:VOLUMEN_DESCONOCIDO_ERROR;
        if(!candidatas && g_particiones.total==1)g_error_volumen=g_particiones.entradas[0].resultado;goto fallo;}
    struct particion *p=&g_particiones.entradas[elegida];
    if(p->resultado){g_error_volumen=p->resultado;goto fallo;}
    switch(p->formato){
        case VOLUMEN_FAT32:g_error_volumen=fat32_montar_particion(p);break;
        case VOLUMEN_EXFAT:g_error_volumen=exfat_montar_particion(p);break;
        case VOLUMEN_NTFS:g_error_volumen=ntfs_montar_particion(p);break;
        case VOLUMEN_EXT4:g_error_volumen=ext4_montar_particion(p);break;
        default:g_error_volumen=VOLUMEN_DESCONOCIDO_ERROR;
    }
    p->resultado=g_error_volumen;if(g_error_volumen)goto fallo;
    g_particion_activa=*p;g_tipo_activo=(enum vfs_tipo_fs)p->formato;g_desconectado=0;
    consola_imprimir("USB ");consola_imprimir_dec(unidad);consola_imprimir(" · Particion ");consola_imprimir_dec(p->numero);consola_imprimir(" · ");consola_imprimir(vfs_obtener_nombre_fs());consola_imprimir_linea(" · Solo lectura");
    if(p->formato==VOLUMEN_EXFAT && fat_lector_exfat.respaldo)consola_imprimir_linea(fat_lector_exfat.respaldo==1?"Recuperacion de lectura: se usa el arranque de respaldo; disco sin reparar.":"Arranque principal valido; respaldo invalido, sin reparacion.");
    return 0;
fallo:
    g_catalogo_vfs.total=0;consola_imprimir_linea(volumen_error(g_error_volumen));return g_error_volumen;
}
int vfs_listar_pagina(const char *ruta,unsigned pagina){
    g_cancelacion_lectura=0;
    if(!vfs_esta_montado())return VOLUMEN_DESCONECTADO;
    char copia[sizeof(g_ruta_catalogo)];if(!ruta)ruta=g_ruta_catalogo;
    size_t len=0;while(ruta[len]){if(len+1==sizeof(copia))return VOLUMEN_NO_SOPORTADO;copia[len]=ruta[len];len++;}copia[len]=0;
    int r;
    switch(g_tipo_activo){
        case VFS_FS_FAT32:r=fat_lector_listar(&fat_lector_fat32,copia,pagina);break;
        case VFS_FS_EXFAT:r=fat_lector_listar(&fat_lector_exfat,copia,pagina);break;
        case VFS_FS_NTFS:r=pagina?VOLUMEN_NO_SOPORTADO:ntfs_listar_directorio(copia);break;
        case VFS_FS_EXT4:r=pagina?VOLUMEN_NO_SOPORTADO:ext4_listar_directorio(copia);break;
        default:r=VOLUMEN_NO_ENCONTRADO;
    }
    if(!r)memcpy(g_ruta_catalogo,copia,len+1);else vfs_limpiar_catalogo();return r;
}
enum vfs_tipo_archivo vfs_inspeccionar_archivo(const char *ruta,int *error){
    uint8_t prefijo[512];int fd;int r=vfs_abrir(ruta,&fd);if(r){if(error)*error=r;return VFS_TIPO_OTRO;}
    int64_t n=vfs_leer(fd,prefijo,sizeof(prefijo));vfs_cerrar(fd);if(error)*error=n<0?(int)n:0;if(n<0)return VFS_TIPO_OTRO;
    if(n>=8 && !memcmp(prefijo,"\x89PNG\r\n\x1a\n",8))return VFS_TIPO_IMAGEN_PNG;
    if(n>=3 && prefijo[0]==255 && prefijo[1]==216 && prefijo[2]==255)return VFS_TIPO_IMAGEN_JPEG;
    if(n>=2 && prefijo[0]=='B' && prefijo[1]=='M')return VFS_TIPO_IMAGEN_BMP;
    if(n>=12 && !memcmp(prefijo+4,"ftyp",4))return VFS_TIPO_MP4;
    if(n>=10 && !memcmp(prefijo,"ID3",3))return VFS_TIPO_MP3;
    if(n>=4 && prefijo[0]==255 && (prefijo[1]&0xe0)==0xe0 && (prefijo[1]&6)==2 && (prefijo[1]&0x18)!=8 && (prefijo[2]&0xf0)!=0 && (prefijo[2]&0xf0)!=0xf0 && (prefijo[2]&12)!=12)return VFS_TIPO_MP3;
    enum vfs_tipo_archivo t=vfs_detectar_tipo_archivo(ruta);
    if(t==VFS_TIPO_TEXTO){for(int64_t i=0;i<n;i++)if((prefijo[i]<32 && prefijo[i]!='\t' && prefijo[i]!='\r' && prefijo[i]!='\n') || prefijo[i]==127)return VFS_TIPO_OTRO;return t;}
    return VFS_TIPO_OTRO;
}
int vfs_consultar_ruta(const char *ruta,enum vfs_tipo_nodo *tipo){
    struct fat_lector_volumen *v=g_tipo_activo==VFS_FS_FAT32?&fat_lector_fat32:g_tipo_activo==VFS_FS_EXFAT?&fat_lector_exfat:0;
    if(!v || !tipo)return VOLUMEN_NO_SOPORTADO;
    g_cancelacion_lectura=0;struct fat_lector_nodo n;int r=fat_lector_resolver(v,ruta,&n,0);if(!r)*tipo=n.directorio?VFS_NODO_DIRECTORIO:VFS_NODO_ARCHIVO;return r;
}
