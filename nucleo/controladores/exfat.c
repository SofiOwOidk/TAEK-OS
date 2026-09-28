#include "fat_lector.h"
#include "exfat.h"
#include "vfs.h"
#include "usb_msc.h"
#include "consola.h"
#include "../base/memoria.h"
#include "../base/dma.h"

// ============================================================================
// TAEK OS - IMPLEMENTACIÓN DEL CONTROLADOR exFAT (Hito 50)
// Lectura y creación experimental en Anillo 0 - Sin dependencias externas
// ============================================================================

#define COLOR_DIR_EXFAT      0x0055FFFF // Cian brillante
#define COLOR_FILE_EXFAT     0x00FFFFFF // Blanco
#define COLOR_BIN_EXFAT      0x0055FF55 // Verde brillante
#define COLOR_TXT_EXFAT      0x00FFFF55 // Amarillo brillante
#define COLOR_AVISO_EXFAT    0x00FFAA00 // Naranja

static struct exfat_volumen g_vol_exfat = {0};

// Búfer estático en BSS para evitar desbordamiento de pila (hasta 32 KiB)
static uint8_t g_exfat_cluster_buf[32768] __attribute__((aligned(16)));
static uint8_t g_exfat_bitmap_buf[32768] __attribute__((aligned(16)));
static uint8_t g_exfat_sector_buf[512] __attribute__((aligned(16)));

// Estructura para almacenar entradas de un directorio durante la exploración
struct exfat_nodo {
    char     nombre[256];
    uint8_t  es_directorio;
    uint32_t cluster_inicio;
    uint64_t tamano_bytes;
    uint8_t  sin_cadena_fat;
};

#define MAX_NODOS_POR_DIR 128
static struct exfat_nodo g_exfat_nodos[MAX_NODOS_POR_DIR];

// Estadísticas de 'tree'
static uint32_t g_tree_exfat_directorios = 0;
static uint32_t g_tree_exfat_archivos = 0;
static uint64_t g_tree_exfat_bytes_totales = 0;

static int exfat_str_igual_sin_caso(const char *s1, const char *s2) {
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

void exfat_iniciar(void) {
    memset(&g_vol_exfat, 0, sizeof(g_vol_exfat));
}

int exfat_esta_montado(void) {
    return g_vol_exfat.montado;
}

const struct exfat_volumen *exfat_obtener_volumen(void) {
    return &g_vol_exfat;
}

void exfat_desmontar(void) {
    g_vol_exfat.montado = 0;
    fat_lector_desmontar(&fat_lector_exfat);
}

static uint32_t exfat_cluster_a_lba(uint32_t cluster) {
    if (cluster < 2 || cluster - 2 >= g_vol_exfat.total_clusters) return 0;
    uint64_t lba = (uint64_t)g_vol_exfat.lba_heap +
                   (uint64_t)(cluster - 2) * g_vol_exfat.sectores_por_cluster;
    return lba <= UINT32_MAX ? (uint32_t)lba : 0;
}

static uint32_t exfat_siguiente_cluster(uint32_t cluster) {
    if (cluster < 2 || cluster >= g_vol_exfat.total_clusters + 2) return 0xFFFFFFFF;

    uint32_t offset_bytes = cluster * 4;
    uint32_t sector_fat = g_vol_exfat.lba_fat + (offset_bytes / g_vol_exfat.bytes_por_sector);
    uint32_t offset_en_sector = offset_bytes % g_vol_exfat.bytes_por_sector;

    if (usb_msc_leer_sectores(g_vol_exfat.unidad_msc, sector_fat, 1, g_exfat_sector_buf) != 0) {
        return 0xFFFFFFFF;
    }

    uint32_t sig = *(uint32_t *)&g_exfat_sector_buf[offset_en_sector];
    if (sig >= 0xFFFFFFF8) return 0xFFFFFFFF; // Fin de cadena (EOC)
    return sig;
}

static int exfat_leer_cluster(uint32_t cluster, uint8_t *destino) {
    uint32_t lba = exfat_cluster_a_lba(cluster);
    if (lba == 0) return -1;
    return usb_msc_leer_sectores(g_vol_exfat.unidad_msc, lba, g_vol_exfat.sectores_por_cluster, destino);
}

int exfat_montar(uint8_t unidad_msc) {
    struct particiones *ps=asignar_memoria(sizeof(*ps));if(!ps)return VOLUMEN_NO_SOPORTADO;
    int r=particiones_descubrir(unidad_msc,ps);unsigned candidatos=0,elegida=0;
    if(!r)for(unsigned i=0;i<ps->total;i++)if(ps->entradas[i].formato==VOLUMEN_EXFAT){elegida=i;candidatos++;}
    if(!r)r=candidatos==1?exfat_montar_particion(&ps->entradas[elegida]):candidatos?VOLUMEN_ELEGIR:VOLUMEN_DESCONOCIDO_ERROR;
    liberar_memoria(ps);return r;
}

static int exfat_leer_entradas_directorio(uint32_t cluster_dir, uint8_t sin_fat,
                                          uint64_t longitud, struct exfat_nodo *nodos, int max_nodos) {
    int total = 0;
    uint32_t curr_cluster = cluster_dir;
    uint32_t visitados = 0;
    uint64_t limite = sin_fat ? (longitud / g_vol_exfat.bytes_por_cluster +
                                 (longitud % g_vol_exfat.bytes_por_cluster != 0)) :
                                 g_vol_exfat.total_clusters;
    if (limite > g_vol_exfat.total_clusters) limite = g_vol_exfat.total_clusters;

    while (curr_cluster != 0xFFFFFFFF && curr_cluster >= 2 &&
           total < max_nodos && visitados++ < limite) {
        if (exfat_leer_cluster(curr_cluster, g_exfat_cluster_buf) != 0) break;

        uint32_t num_entradas = g_vol_exfat.bytes_por_cluster / 32;
        for (uint32_t i = 0; i < num_entradas && total < max_nodos; i++) {
            uint8_t *entrada = &g_exfat_cluster_buf[i * 32];
            uint8_t tipo = entrada[0];

            if (tipo == 0x00) { // Fin de entradas
                return total;
            }

            if (!(tipo & 0x80)) { // Entrada no crítica eliminada
                continue;
            }

            if (tipo == EXFAT_TIPO_ETIQUETA) {
                uint8_t len = entrada[1];
                if (len > 0 && len <= 11) {
                    uint16_t *label_utf16 = (uint16_t *)&entrada[2];
                    for (int k = 0; k < len; k++) {
                        g_vol_exfat.etiqueta[k] = (char)(label_utf16[k] & 0x7F);
                    }
                    g_vol_exfat.etiqueta[len] = '\0';
                }
                continue;
            }

            // Entrada primaria de archivo / directorio
            if (tipo == EXFAT_TIPO_ARCHIVO) {
                struct exfat_entrada_archivo *file_ent = (struct exfat_entrada_archivo *)entrada;
                uint8_t sec_count = file_ent->conteo_secundarias;
                uint16_t attrs = file_ent->atributos;

                if (sec_count < 2 || (i + sec_count) >= num_entradas) {
                    i += sec_count;
                    continue;
                }

                // Entrada secundaria 1: Flujo de datos (0xC0)
                uint8_t *sec_flujo = &g_exfat_cluster_buf[(i + 1) * 32];
                if (sec_flujo[0] != EXFAT_TIPO_FLUJO) {
                    i += sec_count;
                    continue;
                }
                struct exfat_entrada_flujo *stream_ent = (struct exfat_entrada_flujo *)sec_flujo;

                uint8_t sin_fat = (stream_ent->banderas & EXFAT_FLUJO_NO_FAT) ? 1 : 0;
                uint32_t pcluster = stream_ent->primer_cluster;
                uint64_t tam = stream_ent->longitud_datos;
                uint8_t len_nombre = stream_ent->longitud_nombre;

                // Entradas de nombre (0xC1)
                char nombre[256];
                int nombre_idx = 0;

                for (uint8_t s = 2; s <= sec_count && nombre_idx < 250; s++) {
                    uint8_t *sec_nom = &g_exfat_cluster_buf[(i + s) * 32];
                    if (sec_nom[0] == EXFAT_TIPO_NOMBRE) {
                        struct exfat_entrada_nombre *nom_ent = (struct exfat_entrada_nombre *)sec_nom;
                        for (int ch = 0; ch < 15 && nombre_idx < len_nombre; ch++) {
                            uint16_t u = nom_ent->nombre_utf16[ch];
                            if (u == 0) break;
                            nombre[nombre_idx++] = (u < 128) ? (char)u : '_';
                        }
                    }
                }
                nombre[nombre_idx] = '\0';

                // Guardar nodo si tiene nombre válido
                if (nombre_idx > 0) {
                    memcpy(nodos[total].nombre, nombre, nombre_idx + 1);
                    nodos[total].es_directorio = (attrs & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
                    nodos[total].cluster_inicio = pcluster;
                    nodos[total].tamano_bytes = tam;
                    nodos[total].sin_cadena_fat = sin_fat;
                    total++;
                }

                i += sec_count;
            }
        }

        curr_cluster = sin_fat ? curr_cluster + 1 : exfat_siguiente_cluster(curr_cluster);
    }

    return total;
}

static void exfat_tree_recursivo(uint32_t cluster_dir, uint8_t sin_fat, uint64_t longitud,
                                const char *prefijo, int profundidad) {
    if (profundidad > 5) return;

    struct exfat_nodo *nodos = g_exfat_nodos;
    int total = exfat_leer_entradas_directorio(cluster_dir, sin_fat, longitud, nodos, MAX_NODOS_POR_DIR);

    for (int i = 0; i < total; i++) {
        int es_ultimo = (i == total - 1);

        consola_imprimir(prefijo);
        if (es_ultimo) {
            consola_imprimir("└── ");
        } else {
            consola_imprimir("├── ");
        }

        if (nodos[i].es_directorio) {
            g_tree_exfat_directorios++;
            consola_imprimir_color(nodos[i].nombre, COLOR_DIR_EXFAT);
            consola_imprimir_linea_color("/", COLOR_DIR_EXFAT);

            char nuevo_prefijo[128];
            int p_len = 0;
            while (prefijo[p_len] && p_len < 100) {
                nuevo_prefijo[p_len] = prefijo[p_len];
                p_len++;
            }
            if (es_ultimo) {
                nuevo_prefijo[p_len++] = ' ';
                nuevo_prefijo[p_len++] = ' ';
                nuevo_prefijo[p_len++] = ' ';
                nuevo_prefijo[p_len++] = ' ';
            } else {
                nuevo_prefijo[p_len++] = 0xE2; // '│' UTF-8 (0xE2 0x94 0x82)
                nuevo_prefijo[p_len++] = 0x94;
                nuevo_prefijo[p_len++] = 0x82;
                nuevo_prefijo[p_len++] = ' ';
            }
            nuevo_prefijo[p_len] = '\0';

            // Para la recursión copiamos el nodo actual en la pila
            struct exfat_nodo dir_actual = nodos[i];
            exfat_tree_recursivo(dir_actual.cluster_inicio, dir_actual.sin_cadena_fat,
                                dir_actual.tamano_bytes, nuevo_prefijo, profundidad + 1);
            // Restaurar entradas
            exfat_leer_entradas_directorio(cluster_dir, sin_fat, longitud, nodos, MAX_NODOS_POR_DIR);
        } else {
            g_tree_exfat_archivos++;
            g_tree_exfat_bytes_totales += nodos[i].tamano_bytes;

            uint32_t color = COLOR_FILE_EXFAT;
            const char *nombre = nodos[i].nombre;
            int nlen = 0;
            while (nombre[nlen]) nlen++;

            if ((nlen > 4 && memcmp(&nombre[nlen - 4], ".txt", 4) == 0) ||
                (nlen > 4 && memcmp(&nombre[nlen - 4], ".cfg", 4) == 0)) {
                color = COLOR_TXT_EXFAT;
            } else if ((nlen > 4 && memcmp(&nombre[nlen - 4], ".efi", 4) == 0) ||
                       (nlen > 4 && memcmp(&nombre[nlen - 4], ".bin", 4) == 0)) {
                color = COLOR_BIN_EXFAT;
            }

            consola_imprimir_color(nombre, color);
            consola_imprimir("  (");
            if (nodos[i].tamano_bytes < 1024) {
                consola_imprimir_dec((uint32_t)nodos[i].tamano_bytes);
                consola_imprimir(" B)");
            } else if (nodos[i].tamano_bytes < 1024 * 1024) {
                consola_imprimir_dec((uint32_t)(nodos[i].tamano_bytes / 1024));
                consola_imprimir(" KB)");
            } else {
                consola_imprimir_dec((uint32_t)(nodos[i].tamano_bytes / (1024 * 1024)));
                consola_imprimir(" MB)");
            }
            consola_imprimir_linea("");
        }
    }
}

int exfat_ejecutar_tree(const char *ruta_inicial) {
    return fat_lector_tree(&fat_lector_exfat,ruta_inicial);
}

int exfat_listar_directorio(const char *ruta) {
    return fat_lector_listar(&fat_lector_exfat,ruta,0);
}

int exfat_leer_archivo_texto(const char *ruta) {
    return vfs_leer_archivo_texto(ruta);
}

// Lee un archivo binario completo en memoria desde exFAT
int exfat_leer_archivo_binario(const char *ruta, void **buf_out, size_t *tam_out, int *es_dma_out) {
    return vfs_leer_archivo_binario(ruta,buf_out,tam_out,es_dma_out);
}

// Escribe un cluster completo en disco
static int exfat_escribir_cluster(uint32_t cluster, const uint8_t *origen) {
    uint32_t lba = exfat_cluster_a_lba(cluster);
    if (lba == 0) return -1;
    return usb_msc_escribir_sectores(g_vol_exfat.unidad_msc, lba, g_vol_exfat.sectores_por_cluster, origen);
}

// Calcula el hash de nombre exFAT en mayúsculas
static uint16_t exfat_hash_nombre_str(const char *nombre) {
    uint16_t hash = 0;
    for (int i = 0; nombre[i]; i++) {
        char c = nombre[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        uint16_t u = (uint16_t)(uint8_t)c;
        hash = ((hash << 15) | (hash >> 1)) + (u & 0xFF);
        hash = ((hash << 15) | (hash >> 1)) + ((u >> 8) & 0xFF);
    }
    return hash;
}

// Calcula el checksum de un conjunto de entradas exFAT (32 * conteo bytes)
static uint16_t exfat_calcular_checksum_set(const uint8_t *datos, int conteo_entradas) {
    uint16_t csum = 0;
    int total = conteo_entradas * 32;
    for (int i = 0; i < total; i++) {
        if (i == 2 || i == 3) continue; // Saltar campo checksum en la entrada 0x85
        csum = ((csum << 15) | (csum >> 1)) + datos[i];
    }
    return csum;
}

static int exfat_nombre_valido(const char *nombre) {
    if (!nombre) return 0;
    uint32_t n = 0;
    for (; nombre[n] && n <= 15; n++) {
        unsigned char c = (unsigned char)nombre[n];
        if (c < 32 || c > 126 || c == '"' || c == '*' || c == '/' || c == ':' ||
            c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') return 0;
    }
    return n > 0 && n <= 15 && nombre[n - 1] != ' ' && nombre[n - 1] != '.';
}

// Escritor limitado a raíz de un cluster: buscar todos los sets, no sólo los
// primeros MAX_NODOS_POR_DIR que presenta el comando de listado.
static int exfat_nombre_ya_existe(const char *nombre) {
    if (exfat_leer_cluster(g_vol_exfat.cluster_raiz, g_exfat_cluster_buf) != 0) return -1;
    uint32_t entradas = g_vol_exfat.bytes_por_cluster / 32;
    uint32_t longitud = 0;
    while (nombre[longitud]) longitud++;
    for (uint32_t i = 0; i < entradas; i++) {
        uint8_t *e = &g_exfat_cluster_buf[i * 32];
        if (e[0] == 0) return 0;
        if (e[0] != EXFAT_TIPO_ARCHIVO) continue;
        uint32_t secundarias = e[1];
        if (secundarias < 2 || i + secundarias >= entradas) return -1;
        struct exfat_entrada_flujo *flujo =
            (struct exfat_entrada_flujo *)&g_exfat_cluster_buf[(i + 1) * 32];
        if (flujo->tipo_entrada != EXFAT_TIPO_FLUJO) return -1;
        if (flujo->longitud_nombre == longitud) {
            uint32_t pos = 0;
            int igual = 1;
            for (uint32_t s = 2; s <= secundarias && pos < longitud; s++) {
                struct exfat_entrada_nombre *ne =
                    (struct exfat_entrada_nombre *)&g_exfat_cluster_buf[(i + s) * 32];
                if (ne->tipo_entrada != EXFAT_TIPO_NOMBRE) { igual = 0; break; }
                for (uint32_t k = 0; k < 15 && pos < longitud; k++, pos++) {
                    uint16_t u = ne->nombre_utf16[k];
                    unsigned char c = (unsigned char)nombre[pos];
                    if (u >= 'A' && u <= 'Z') u += 32;
                    if (c >= 'A' && c <= 'Z') c += 32;
                    if (u != c) igual = 0;
                }
            }
            if (igual && pos == longitud) return 1;
        }
        i += secundarias;
    }
    return 0;
}

// Asigna un cluster libre en la partición exFAT
static uint32_t exfat_asignar_clusters_contiguos(uint32_t cantidad) {
    if (!g_vol_exfat.montado) return 0;
    if (!cantidad || cantidad > g_vol_exfat.total_clusters ||
        exfat_leer_cluster(g_vol_exfat.cluster_bitmap, g_exfat_bitmap_buf) != 0) return 0;
    uint32_t inicio = 0, libres = 0;
    for (uint32_t bit = 0; bit < g_vol_exfat.total_clusters; bit++) {
        if (g_exfat_bitmap_buf[bit / 8] & (1U << (bit % 8))) {
            libres = 0;
        } else {
            if (!libres) inicio = bit;
            if (++libres == cantidad) {
                for (uint32_t j = 0; j < cantidad; j++)
                    g_exfat_bitmap_buf[(inicio + j) / 8] |= 1U << ((inicio + j) % 8);
                if (exfat_escribir_cluster(g_vol_exfat.cluster_bitmap, g_exfat_bitmap_buf) != 0) return 0;
                return inicio + 2;
            }
        }
    }
    return 0;
}

static uint32_t exfat_asignar_cluster_libre(void) {
    return exfat_asignar_clusters_contiguos(1);
}

// VolumeFlags está excluido del checksum de la región de arranque exFAT.
// Dejar Dirty=1 si cualquier fase falla: fsck podrá detectar la operación incompleta.
static int exfat_marcar_sucio(int sucio) {
    static uint8_t boot_principal[512], boot_respaldo[512];
    uint32_t principal = g_vol_exfat.lba_inicio_particion;
    uint32_t respaldo = principal + 12;
    if (usb_msc_leer_sectores(g_vol_exfat.unidad_msc, principal, 1, boot_principal) != 0 ||
        usb_msc_leer_sectores(g_vol_exfat.unidad_msc, respaldo, 1, boot_respaldo) != 0 ||
        memcmp(boot_principal + 3, "EXFAT   ", 8) != 0 ||
        memcmp(boot_respaldo + 3, "EXFAT   ", 8) != 0) return -1;
    struct exfat_vbr *a = (struct exfat_vbr *)boot_principal;
    struct exfat_vbr *b = (struct exfat_vbr *)boot_respaldo;
    if (sucio) {
        a->banderas_volumen |= 0x0002;
        b->banderas_volumen |= 0x0002;
        if (usb_msc_escribir_sectores(g_vol_exfat.unidad_msc, principal, 1, boot_principal) != 0 ||
            usb_msc_escribir_sectores(g_vol_exfat.unidad_msc, respaldo, 1, boot_respaldo) != 0) return -1;
    } else {
        a->banderas_volumen &= (uint16_t)~0x0002;
        b->banderas_volumen &= (uint16_t)~0x0002;
        if (usb_msc_escribir_sectores(g_vol_exfat.unidad_msc, respaldo, 1, boot_respaldo) != 0 ||
            usb_msc_escribir_sectores(g_vol_exfat.unidad_msc, principal, 1, boot_principal) != 0) return -1;
    }
    return 0;
}

// Inserta un conjunto de entradas (0x85, 0xC0, 0xC1) en el directorio raíz
static int exfat_insertar_entrada_directorio(uint32_t cluster_dir, const char *nombre, uint8_t es_dir, uint32_t cluster_inicio, uint32_t tamano) {
    if (exfat_leer_cluster(cluster_dir, g_exfat_cluster_buf) != 0) return -1;

    uint32_t entradas_por_cluster = g_vol_exfat.bytes_por_cluster / 32;
    int ranura_inicio = -1;

    // Buscar 3 ranuras consecutivas disponibles (< 0x80 o 0x00)
    for (uint32_t i = 0; i + 2 < entradas_por_cluster; i++) {
        uint8_t t0 = g_exfat_cluster_buf[i * 32];
        uint8_t t1 = g_exfat_cluster_buf[(i + 1) * 32];
        uint8_t t2 = g_exfat_cluster_buf[(i + 2) * 32];

        if ((t0 < 0x80 || t0 == 0x00) && (t1 < 0x80 || t1 == 0x00) && (t2 < 0x80 || t2 == 0x00)) {
            ranura_inicio = (int)i;
            break;
        }
    }

    if (ranura_inicio < 0) return -2; // No hay espacio suficiente en el cluster raíz

    uint8_t *set_ptr = &g_exfat_cluster_buf[ranura_inicio * 32];
    for (int k = 0; k < 96; k++) set_ptr[k] = 0;

    int nom_len = 0;
    while (nombre[nom_len] && nom_len < 15) nom_len++;

    // 1. Entrada 0x85 (File/Directory)
    struct exfat_entrada_archivo *e_file = (struct exfat_entrada_archivo *)&set_ptr[0];
    e_file->tipo_entrada = EXFAT_TIPO_ARCHIVO;
    e_file->conteo_secundarias = 2;
    e_file->atributos = es_dir ? EXFAT_ATTR_DIRECTORY : EXFAT_ATTR_ARCHIVE;

    // 2. Entrada 0xC0 (Stream Extension)
    struct exfat_entrada_flujo *e_stream = (struct exfat_entrada_flujo *)&set_ptr[32];
    e_stream->tipo_entrada = EXFAT_TIPO_FLUJO;
    e_stream->banderas = 1u | (tamano?EXFAT_FLUJO_NO_FAT:0u); // AllocationPossible; vacío sin extensión contigua.
    e_stream->longitud_nombre = (uint8_t)nom_len;
    e_stream->hash_nombre = exfat_hash_nombre_str(nombre);
    e_stream->primer_cluster = cluster_inicio;
    e_stream->longitud_valida = tamano;
    e_stream->longitud_datos = tamano;

    // 3. Entrada 0xC1 (File Name)
    struct exfat_entrada_nombre *e_name = (struct exfat_entrada_nombre *)&set_ptr[64];
    e_name->tipo_entrada = EXFAT_TIPO_NOMBRE;
    for (int k = 0; k < nom_len; k++) {
        e_name->nombre_utf16[k] = (uint16_t)(uint8_t)nombre[k];
    }

    // Calcular y guardar Checksum
    e_file->checksum = exfat_calcular_checksum_set(set_ptr, 3);

    // Escribir cluster de directorio modificado a disco
    return exfat_escribir_cluster(cluster_dir, g_exfat_cluster_buf);
}

int exfat_crear_archivo(const char *nombre, const uint8_t *datos, uint32_t tamano) {
    if (!exfat_nombre_valido(nombre)) return -1;
    if (!g_vol_exfat.montado) {
        if (exfat_montar(0) != 0) return -2;
    }
    if (!g_vol_exfat.escritura_habilitada) return -6;
    if (tamano && !datos) return -1;
    int existente = exfat_nombre_ya_existe(nombre);
    if (existente != 0) return existente > 0 ? -8 : -2;
    if (exfat_marcar_sucio(1) != 0) {
        g_vol_exfat.escritura_habilitada = 0;
        return -7;
    }

    // 1. Asignar cluster de datos si tamano > 0
    uint32_t cluster_datos = 0;
    uint32_t cantidad = 0;
    if (tamano > 0) {
        cantidad = (uint32_t)(((uint64_t)tamano + g_vol_exfat.bytes_por_cluster - 1) /
                              g_vol_exfat.bytes_por_cluster);
        cluster_datos = exfat_asignar_clusters_contiguos(cantidad);
        if (cluster_datos == 0) {
            consola_imprimir_linea_color("  [!] Error: No hay clusters libres en exFAT.", COLOR_AVISO_EXFAT);
            return -3;
        }

        for (uint32_t n = 0; n < cantidad; n++) {
            memset(g_exfat_cluster_buf, 0, g_vol_exfat.bytes_por_cluster);
            uint32_t offset = n * g_vol_exfat.bytes_por_cluster;
            uint32_t a_copiar = tamano - offset;
            if (a_copiar > g_vol_exfat.bytes_por_cluster) a_copiar = g_vol_exfat.bytes_por_cluster;
            memcpy(g_exfat_cluster_buf, datos + offset, a_copiar);
            if (exfat_escribir_cluster(cluster_datos + n, g_exfat_cluster_buf) != 0) {
                g_vol_exfat.escritura_habilitada = 0;
                return -4;
            }
        }
    }

    // 2. Insertar entradas en directorio raíz
    if (exfat_insertar_entrada_directorio(g_vol_exfat.cluster_raiz, nombre, 0, cluster_datos, tamano) != 0) {
        g_vol_exfat.escritura_habilitada = 0;
        // El estado de una escritura fallida al directorio es ambiguo. Conservar
        // la reserva evita que un archivo parcialmente publicado apunte a espacio libre.
        return -5;
    }
    if (exfat_marcar_sucio(0) != 0) {
        g_vol_exfat.escritura_habilitada = 0;
        return -7;
    }

    consola_imprimir("==> [exFAT] Archivo creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Cluster: ");
    consola_imprimir_dec(cluster_datos);
    consola_imprimir_linea(")");
    return 0;
}
int exfat_crear_directorio(const char *nombre) {
    if (!exfat_nombre_valido(nombre)) return -1;
    if (!g_vol_exfat.montado) {
        if (exfat_montar(0) != 0) return -2;
    }
    if (!g_vol_exfat.escritura_habilitada) return -6;
    int existente = exfat_nombre_ya_existe(nombre);
    if (existente != 0) return existente > 0 ? -8 : -2;
    if (exfat_marcar_sucio(1) != 0) {
        g_vol_exfat.escritura_habilitada = 0;
        return -7;
    }

    uint32_t cluster_dir = exfat_asignar_cluster_libre();
    if (cluster_dir == 0) return -3;

    // Limpiar cluster del nuevo directorio
    for (uint32_t i = 0; i < sizeof(g_exfat_cluster_buf); i++) g_exfat_cluster_buf[i] = 0;
    if (exfat_escribir_cluster(cluster_dir, g_exfat_cluster_buf) != 0) {
        g_vol_exfat.escritura_habilitada = 0;
        return -4;
    }

    if (exfat_insertar_entrada_directorio(g_vol_exfat.cluster_raiz, nombre, 1, cluster_dir,
                                         g_vol_exfat.bytes_por_cluster) != 0) {
        g_vol_exfat.escritura_habilitada = 0;
        return -5;
    }
    if (exfat_marcar_sucio(0) != 0) {
        g_vol_exfat.escritura_habilitada = 0;
        return -7;
    }

    consola_imprimir("==> [exFAT] Directorio creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Cluster: ");
    consola_imprimir_dec(cluster_dir);
    consola_imprimir_linea(")");
    return 0;
}

static uint32_t exfat_sig_cluster_stream(struct exfat_cursor_archivo *cur) {
    if (cur->sin_cadena_fat) return cur->cluster_actual + 1;
    uint8_t fat_buf[512];
    uint32_t fat_offset = cur->cluster_actual * 4;
    uint32_t fat_lba = cur->lba_fat + (fat_offset / cur->bytes_por_sector);
    uint32_t off = fat_offset % cur->bytes_por_sector;
    if (usb_msc_leer_sectores(cur->unidad_msc, fat_lba, 1, fat_buf) != 0) return 0xFFFFFFFF;
    uint32_t val = *(uint32_t *)&fat_buf[off];
    if (val >= 0xFFFFFFF8U || val < 2) return 0xFFFFFFFF;
    return val;
}

int exfat_abrir_stream(const char *ruta, void *fd_generico) {
    return fat_lector_abrir_stream(&fat_lector_exfat,ruta,fd_generico);
}

int64_t exfat_leer_stream(void *fd_generico, void *buf, size_t cantidad) {
    return fat_lector_leer_stream(&fat_lector_exfat,fd_generico,buf,cantidad);
}

int64_t exfat_buscar_stream(void *fd_generico, int64_t offset, int origen) {
    struct vfs_descriptor_archivo *fd = (struct vfs_descriptor_archivo *)fd_generico;
    int64_t nueva_pos = 0;

    if (origen == VFS_SEEK_SET) {
        nueva_pos = offset;
    } else if (origen == VFS_SEEK_CUR) {
        nueva_pos = (int64_t)fd->posicion + offset;
    } else if (origen == VFS_SEEK_END) {
        nueva_pos = (int64_t)fd->tamano + offset;
    } else {
        return -1;
    }

    if (nueva_pos < 0) nueva_pos = 0;
    if ((uint64_t)nueva_pos > fd->tamano) nueva_pos = fd->tamano;

    fd->posicion = (uint64_t)nueva_pos;
    return nueva_pos;
}

void exfat_cerrar_stream(void *fd_generico) {
    struct vfs_descriptor_archivo *fd = (struct vfs_descriptor_archivo *)fd_generico;
    if (fd->bufer_cache) {
        liberar_memoria(fd->bufer_cache);
        fd->bufer_cache = NULL;
    }
    fd->bufer_tamano = 0;
    fd->bufer_bytes_validos = 0;
    fd->bufer_unidad_logica = 0xFFFFFFFF;
    fd->en_uso = 0;
}

int exfat_habilitar_escritura_experimental(void){
    if(!g_vol_exfat.montado || fat_lector_exfat.respaldo || g_vol_exfat.bytes_por_cluster>sizeof(g_exfat_cluster_buf) ||
       g_vol_exfat.longitud_bitmap>g_vol_exfat.bytes_por_cluster)return VOLUMEN_NO_SOPORTADO;
    uint8_t s[512];int r=particion_leer(&fat_lector_exfat.particion,0,1,s,0);if(r)return r;
    if((s[106]&6) || exfat_siguiente_cluster(g_vol_exfat.cluster_raiz)!=0xffffffff)return VOLUMEN_NO_SOPORTADO;
    g_vol_exfat.escritura_habilitada=1;return 0;
}
int exfat_montar_particion(const struct particion *p){
    exfat_desmontar();int r=fat_lector_montar(&fat_lector_exfat,p);if(r)return r;
    struct fat_lector_volumen *v=&fat_lector_exfat;memset(&g_vol_exfat,0,sizeof(g_vol_exfat));
    g_vol_exfat.montado=1;g_vol_exfat.unidad_msc=p->unidad;g_vol_exfat.lba_inicio_particion=(uint32_t)p->inicio;
    g_vol_exfat.bytes_por_sector=512;g_vol_exfat.sectores_por_cluster=v->sectores_cluster;g_vol_exfat.bytes_por_cluster=v->bytes_cluster;
    g_vol_exfat.lba_fat=(uint32_t)(p->inicio+v->fat);g_vol_exfat.lba_heap=(uint32_t)(p->inicio+v->datos);
    g_vol_exfat.cluster_raiz=v->raiz;g_vol_exfat.total_clusters=v->clusters;g_vol_exfat.cluster_bitmap=v->cluster_bitmap;g_vol_exfat.longitud_bitmap=v->longitud_bitmap;
    memcpy(g_vol_exfat.etiqueta,"EXFAT_VOL",10);return 0;
}
