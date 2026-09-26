#include "exfat.h"
#include "usb_msc.h"
#include "consola.h"
#include "../base/memoria.h"

// ============================================================================
// TAEK OS - IMPLEMENTACIÓN DEL CONTROLADOR exFAT (Hito 50)
// Solo Lectura en Anillo 0 - Sin dependencias externas
// ============================================================================

#define COLOR_DIR_EXFAT      0x0055FFFF // Cian brillante
#define COLOR_FILE_EXFAT     0x00FFFFFF // Blanco
#define COLOR_BIN_EXFAT      0x0055FF55 // Verde brillante
#define COLOR_TXT_EXFAT      0x00FFFF55 // Amarillo brillante
#define COLOR_AVISO_EXFAT    0x00FFAA00 // Naranja

static struct exfat_volumen g_vol_exfat = {0};

// Búfer estático en BSS para evitar desbordamiento de pila (hasta 32 KiB)
static uint8_t g_exfat_cluster_buf[32768] __attribute__((aligned(16)));
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
}

static uint32_t exfat_cluster_a_lba(uint32_t cluster) {
    if (cluster < 2) return 0;
    return g_vol_exfat.lba_heap + (cluster - 2) * g_vol_exfat.sectores_por_cluster;
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
    g_vol_exfat.montado = 0;
    g_vol_exfat.unidad_msc = unidad_msc;

    // 1. Leer LBA 0 (buscar MBR o Superfloppy)
    if (usb_msc_leer_sectores(unidad_msc, 0, 1, g_exfat_sector_buf) != 0) {
        return -1;
    }

    uint32_t lba_particion = 0;
    int encontrado = 0;

    // Caso A: Superfloppy (VBR en LBA 0)
    if (memcmp(&g_exfat_sector_buf[3], "EXFAT   ", 8) == 0) {
        lba_particion = 0;
        encontrado = 1;
    }

    // Caso B: Partición MBR
    if (!encontrado && g_exfat_sector_buf[510] == 0x55 && g_exfat_sector_buf[511] == 0xAA) {
        for (int p = 0; p < 4; p++) {
            uint32_t off = 446 + p * 16;
            uint8_t tipo = g_exfat_sector_buf[off + 4];
            uint32_t inicio_lba = *(uint32_t *)&g_exfat_sector_buf[off + 8];

            if (tipo == 0x07 && inicio_lba != 0) { // Tipo 0x07 puede ser exFAT o NTFS
                static uint8_t sector_prueba[512] __attribute__((aligned(16)));
                if (usb_msc_leer_sectores(unidad_msc, inicio_lba, 1, sector_prueba) == 0) {
                    if (memcmp(&sector_prueba[3], "EXFAT   ", 8) == 0) {
                        lba_particion = inicio_lba;
                        memcpy(g_exfat_sector_buf, sector_prueba, 512);
                        encontrado = 1;
                        break;
                    }
                }
            }
        }
    }

    if (!encontrado) return -1;

    struct exfat_vbr *vbr = (struct exfat_vbr *)g_exfat_sector_buf;

    g_vol_exfat.lba_inicio_particion = lba_particion;
    g_vol_exfat.bytes_por_sector = 1U << vbr->shift_bytes_por_sector;
    g_vol_exfat.sectores_por_cluster = 1U << vbr->shift_sectores_por_cluster;
    g_vol_exfat.bytes_por_cluster = g_vol_exfat.bytes_por_sector * g_vol_exfat.sectores_por_cluster;
    g_vol_exfat.lba_fat = lba_particion + vbr->desplazamiento_fat;
    g_vol_exfat.lba_heap = lba_particion + vbr->desplazamiento_heap;
    g_vol_exfat.cluster_raiz = vbr->cluster_raiz;
    g_vol_exfat.total_clusters = vbr->conteo_clusters;
    memcpy(g_vol_exfat.etiqueta, "EXFAT_VOL", 10);

    // Validación básica de coherencia
    if (g_vol_exfat.bytes_por_sector == 0 || g_vol_exfat.bytes_por_cluster > sizeof(g_exfat_cluster_buf)) {
        return -2;
    }

    g_vol_exfat.montado = 1;
    return 0;
}

static int exfat_leer_entradas_directorio(uint32_t cluster_dir, struct exfat_nodo *nodos, int max_nodos) {
    int total = 0;
    uint32_t curr_cluster = cluster_dir;

    while (curr_cluster != 0xFFFFFFFF && curr_cluster >= 2 && total < max_nodos) {
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

        curr_cluster = exfat_siguiente_cluster(curr_cluster);
    }

    return total;
}

static void exfat_tree_recursivo(uint32_t cluster_dir, const char *prefijo, int profundidad) {
    if (profundidad > 5) return;

    struct exfat_nodo *nodos = g_exfat_nodos;
    int total = exfat_leer_entradas_directorio(cluster_dir, nodos, MAX_NODOS_POR_DIR);

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
            exfat_tree_recursivo(dir_actual.cluster_inicio, nuevo_prefijo, profundidad + 1);
            // Restaurar entradas
            exfat_leer_entradas_directorio(cluster_dir, nodos, MAX_NODOS_POR_DIR);
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
    (void)ruta_inicial;
    if (!g_vol_exfat.montado) {
        if (exfat_montar(0) != 0) {
            consola_imprimir_linea_color("Error: No se encontró volumen exFAT montable en la unidad USB.", COLOR_AVISO_EXFAT);
            return -1;
        }
    }

    consola_imprimir_linea_color("================== ÁRBOL DE ARCHIVOS exFAT (USB MSC) ==================", COLOR_AVISO_EXFAT);
    consola_imprimir("Unidad USB: ");
    consola_imprimir_color(g_vol_exfat.etiqueta, COLOR_DIR_EXFAT);
    consola_imprimir("  (Cluster Raíz: ");
    consola_imprimir_dec(g_vol_exfat.cluster_raiz);
    consola_imprimir_linea(")");

    consola_imprimir_linea_color(".", COLOR_DIR_EXFAT);

    g_tree_exfat_directorios = 0;
    g_tree_exfat_archivos = 0;
    g_tree_exfat_bytes_totales = 0;

    exfat_tree_recursivo(g_vol_exfat.cluster_raiz, "", 1);

    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_AVISO_EXFAT);
    consola_imprimir("Resumen: ");
    consola_imprimir_dec(g_tree_exfat_directorios);
    consola_imprimir(" directorios, ");
    consola_imprimir_dec(g_tree_exfat_archivos);
    consola_imprimir(" archivos (Total: ");
    if (g_tree_exfat_bytes_totales < 1024) {
        consola_imprimir_dec((uint32_t)g_tree_exfat_bytes_totales);
        consola_imprimir(" B)");
    } else if (g_tree_exfat_bytes_totales < 1024 * 1024) {
        consola_imprimir_dec((uint32_t)(g_tree_exfat_bytes_totales / 1024));
        consola_imprimir(" KB)");
    } else {
        consola_imprimir_dec((uint32_t)(g_tree_exfat_bytes_totales / (1024 * 1024)));
        consola_imprimir(" MB)");
    }
    consola_imprimir_linea("");
    consola_imprimir_linea_color("======================================================================", COLOR_AVISO_EXFAT);
    return 0;
}

int exfat_listar_directorio(const char *ruta) {
    (void)ruta;
    if (!g_vol_exfat.montado) {
        if (exfat_montar(0) != 0) {
            consola_imprimir_linea_color("Error: No se encontró volumen exFAT montable.", COLOR_AVISO_EXFAT);
            return -1;
        }
    }

    struct exfat_nodo *nodos = g_exfat_nodos;
    int total = exfat_leer_entradas_directorio(g_vol_exfat.cluster_raiz, nodos, MAX_NODOS_POR_DIR);

    consola_imprimir_linea_color("TIPO    TAMAÑO     CLUSTER    NOMBRE", COLOR_AVISO_EXFAT);
    consola_imprimir_linea_color("----    ------     -------    ------", COLOR_AVISO_EXFAT);

    for (int i = 0; i < total; i++) {
        if (nodos[i].es_directorio) {
            consola_imprimir_color("[DIR]   ", COLOR_DIR_EXFAT);
            consola_imprimir("   -       ");
        } else {
            consola_imprimir_color("[ARCH]  ", COLOR_FILE_EXFAT);
            consola_imprimir_dec((uint32_t)nodos[i].tamano_bytes);
            consola_imprimir(" B    ");
        }
        consola_imprimir_dec(nodos[i].cluster_inicio);
        consola_imprimir("        ");
        consola_imprimir_linea(nodos[i].nombre);
    }
    return 0;
}

int exfat_leer_archivo_texto(const char *ruta) {
    if (!ruta || ruta[0] == '\0') {
        consola_imprimir_linea_color("Uso: cat <archivo> (ej. cat notas.txt)", COLOR_AVISO_EXFAT);
        return -1;
    }

    if (!g_vol_exfat.montado) {
        if (exfat_montar(0) != 0) {
            consola_imprimir_linea_color("Error: No hay volumen exFAT montado.", COLOR_AVISO_EXFAT);
            return -1;
        }
    }

    struct exfat_nodo *nodos = g_exfat_nodos;
    int total = exfat_leer_entradas_directorio(g_vol_exfat.cluster_raiz, nodos, MAX_NODOS_POR_DIR);

    int idx = -1;
    for (int i = 0; i < total; i++) {
        if (!nodos[i].es_directorio && exfat_str_igual_sin_caso(nodos[i].nombre, ruta)) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        consola_imprimir_color("Archivo no encontrado en exFAT: ", COLOR_AVISO_EXFAT);
        consola_imprimir_linea(ruta);
        return -2;
    }

    consola_imprimir("==> [ exFAT: ");
    consola_imprimir(nodos[idx].nombre);
    consola_imprimir(" (");
    consola_imprimir_dec((uint32_t)nodos[idx].tamano_bytes);
    consola_imprimir_linea(" bytes) ] <==");

    uint32_t curr_cluster = nodos[idx].cluster_inicio;
    uint64_t restante = nodos[idx].tamano_bytes;
    uint8_t sin_fat = nodos[idx].sin_cadena_fat;

    while (restante > 0 && curr_cluster != 0xFFFFFFFF && curr_cluster >= 2) {
        if (exfat_leer_cluster(curr_cluster, g_exfat_cluster_buf) != 0) {
            consola_imprimir_linea_color("[Error de lectura I/O de cluster en exFAT]", COLOR_AVISO_EXFAT);
            break;
        }

        uint32_t a_leer = (restante > g_vol_exfat.bytes_por_cluster) ? g_vol_exfat.bytes_por_cluster : (uint32_t)restante;
        for (uint32_t b = 0; b < a_leer; b++) {
            char c = (char)g_exfat_cluster_buf[b];
            if (c == '\r') continue;
            consola_escribir_caracter(c);
        }
        restante -= a_leer;

        if (sin_fat) {
            curr_cluster++;
        } else {
            curr_cluster = exfat_siguiente_cluster(curr_cluster);
        }
    }
    consola_imprimir_linea("");
    return 0;
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

// Asigna un cluster libre en la partición exFAT
static uint32_t exfat_asignar_cluster_libre(void) {
    if (!g_vol_exfat.montado) return 0;

    // Escanear la FAT de exFAT si existe
    uint32_t lba_fat_inicio = g_vol_exfat.lba_inicio_particion + g_vol_exfat.lba_fat;
    for (uint32_t s = 0; s < 64; s++) {
        uint32_t lba_sec = lba_fat_inicio + s;
        if (usb_msc_leer_sectores(g_vol_exfat.unidad_msc, lba_sec, 1, g_exfat_sector_buf) != 0) {
            return 0;
        }

        uint32_t *entradas = (uint32_t *)g_exfat_sector_buf;
        for (int i = 0; i < 128; i++) {
            uint32_t clus = (s * 128) + (uint32_t)i;
            if (clus < 2) continue;

            if (entradas[i] == 0) {
                entradas[i] = 0xFFFFFFFF; // EOC
                if (usb_msc_escribir_sectores(g_vol_exfat.unidad_msc, lba_sec, 1, g_exfat_sector_buf) != 0) {
                    return 0;
                }
                return clus;
            }
        }
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
    e_stream->banderas = EXFAT_FLUJO_NO_FAT;
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
    if (!nombre || *nombre == '\0') return -1;
    if (!g_vol_exfat.montado) {
        if (exfat_montar(0) != 0) return -2;
    }

    // 1. Asignar cluster de datos si tamano > 0
    uint32_t cluster_datos = 0;
    if (tamano > 0) {
        cluster_datos = exfat_asignar_cluster_libre();
        if (cluster_datos == 0) {
            consola_imprimir_linea_color("  [!] Error: No hay clusters libres en exFAT.", COLOR_AVISO_EXFAT);
            return -3;
        }

        for (uint32_t i = 0; i < sizeof(g_exfat_cluster_buf); i++) g_exfat_cluster_buf[i] = 0;
        uint32_t a_copiar = (tamano < g_vol_exfat.bytes_por_cluster) ? tamano : g_vol_exfat.bytes_por_cluster;
        if (datos) {
            for (uint32_t i = 0; i < a_copiar; i++) g_exfat_cluster_buf[i] = datos[i];
        }

        if (exfat_escribir_cluster(cluster_datos, g_exfat_cluster_buf) != 0) return -4;
    }

    // 2. Insertar entradas en directorio raíz
    if (exfat_insertar_entrada_directorio(g_vol_exfat.cluster_raiz, nombre, 0, cluster_datos, tamano) != 0) {
        return -5;
    }

    consola_imprimir("==> [exFAT] Archivo creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Cluster: ");
    consola_imprimir_dec(cluster_datos);
    consola_imprimir_linea(")");
    return 0;
}

int exfat_crear_directorio(const char *nombre) {
    if (!nombre || *nombre == '\0') return -1;
    if (!g_vol_exfat.montado) {
        if (exfat_montar(0) != 0) return -2;
    }

    uint32_t cluster_dir = exfat_asignar_cluster_libre();
    if (cluster_dir == 0) return -3;

    // Limpiar cluster del nuevo directorio
    for (uint32_t i = 0; i < sizeof(g_exfat_cluster_buf); i++) g_exfat_cluster_buf[i] = 0;
    if (exfat_escribir_cluster(cluster_dir, g_exfat_cluster_buf) != 0) return -4;

    if (exfat_insertar_entrada_directorio(g_vol_exfat.cluster_raiz, nombre, 1, cluster_dir, 0) != 0) {
        return -5;
    }

    consola_imprimir("==> [exFAT] Directorio creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Cluster: ");
    consola_imprimir_dec(cluster_dir);
    consola_imprimir_linea(")");
    return 0;
}

