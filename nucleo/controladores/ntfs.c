#include "ntfs.h"
#include "usb_msc.h"
#include "consola.h"
#include "../base/memoria.h"

// ============================================================================
// TAEK OS - IMPLEMENTACIÓN DEL CONTROLADOR NTFS (Hito 50)
// Solo Lectura en Anillo 0 - Sin dependencias externas
// ============================================================================

#define COLOR_DIR_NTFS      0x0055FFFF // Cian brillante
#define COLOR_FILE_NTFS     0x00FFFFFF // Blanco
#define COLOR_BIN_NTFS      0x0055FF55 // Verde brillante
#define COLOR_TXT_NTFS      0x00FFFF55 // Amarillo brillante
#define COLOR_AVISO_NTFS    0x00FFAA00 // Naranja

static struct ntfs_volumen g_vol_ntfs = {0};

// Búferes estáticos en BSS
static uint8_t g_ntfs_sector_buf[512] __attribute__((aligned(16)));
static uint8_t g_ntfs_record_buf[1024] __attribute__((aligned(16)));
static uint8_t g_ntfs_cluster_buf[4096] __attribute__((aligned(16)));

// Estructura para almacenar información de archivos descubiertos en la MFT
struct ntfs_nodo {
    uint32_t mft_idx;
    uint32_t padre_mft_idx;
    char     nombre[256];
    uint8_t  es_directorio;
    uint8_t  es_sistema;
    uint64_t tamano_bytes;
};

#define MAX_NODOS_NTFS 256
static struct ntfs_nodo g_ntfs_nodos[MAX_NODOS_NTFS];
static int g_total_nodos_ntfs = 0;

// Estadísticas de 'tree'
static uint32_t g_tree_ntfs_directorios = 0;
static uint32_t g_tree_ntfs_archivos = 0;
static uint64_t g_tree_ntfs_bytes_totales = 0;

static int ntfs_str_igual_sin_caso(const char *s1, const char *s2) {
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

static size_t ntfs_strlen(const char *s) {
    size_t len = 0;
    while (s && s[len]) len++;
    return len;
}

void ntfs_iniciar(void) {
    memset(&g_vol_ntfs, 0, sizeof(g_vol_ntfs));
    g_total_nodos_ntfs = 0;
}

int ntfs_esta_montado(void) {
    return g_vol_ntfs.montado;
}

const struct ntfs_volumen *ntfs_obtener_volumen(void) {
    return &g_vol_ntfs;
}

void ntfs_desmontar(void) {
    g_vol_ntfs.montado = 0;
    g_total_nodos_ntfs = 0;
}

// Aplica la secuencia de actualización (USA / Fixups) a un registro MFT de 1024 bytes
static int ntfs_aplicar_fixups(uint8_t *buffer, uint32_t tamano) {
    struct ntfs_registro_mft *reg = (struct ntfs_registro_mft *)buffer;
    if (reg->magic != NTFS_MAGIC_FILE) return -1;

    uint16_t usa_offset = reg->usa_offset;
    uint16_t usa_count = reg->usa_count;

    if (usa_count < 2 || usa_offset + usa_count * 2 > tamano) return -2;

    uint16_t usa_num = *(uint16_t *)&buffer[usa_offset];
    uint16_t *usa_array = (uint16_t *)&buffer[usa_offset + 2];

    uint32_t num_sectores = tamano / 512;
    for (uint32_t s = 0; s < num_sectores && s < (uint32_t)(usa_count - 1); s++) {
        uint32_t sector_end_offset = (s + 1) * 512 - 2;
        uint16_t sector_sig = *(uint16_t *)&buffer[sector_end_offset];
        if (sector_sig != usa_num) {
            // Error de integridad en sector
            return -3;
        }
        *(uint16_t *)&buffer[sector_end_offset] = usa_array[s];
    }
    return 0;
}

static int ntfs_leer_registro_mft(uint32_t idx_mft, uint8_t *destino) {
    if (!g_vol_ntfs.montado) return -1;

    uint32_t lba_mft_base = g_vol_ntfs.lba_inicio_particion + (uint32_t)(g_vol_ntfs.lcn_mft * g_vol_ntfs.sectores_por_cluster);
    uint32_t lba_registro = lba_mft_base + idx_mft * g_vol_ntfs.sectores_por_registro_mft;

    if (usb_msc_leer_sectores(g_vol_ntfs.unidad_msc, lba_registro, g_vol_ntfs.sectores_por_registro_mft, destino) != 0) {
        return -2;
    }

    return ntfs_aplicar_fixups(destino, g_vol_ntfs.tamano_registro_mft);
}

// Escanea los registros MFT iniciales para poblar la tabla de nodos
static void ntfs_poblar_catalogo(void) {
    g_total_nodos_ntfs = 0;

    for (uint32_t idx = 0; idx < 128 && g_total_nodos_ntfs < MAX_NODOS_NTFS; idx++) {
        if (ntfs_leer_registro_mft(idx, g_ntfs_record_buf) != 0) continue;

        struct ntfs_registro_mft *reg = (struct ntfs_registro_mft *)g_ntfs_record_buf;
        if (!(reg->banderas & NTFS_BANDERA_EN_USO)) continue;

        int es_dir = (reg->banderas & NTFS_BANDERA_DIR) ? 1 : 0;
        uint32_t offset = reg->primer_atributo_offset;
        uint32_t tam_usado = reg->tamano_usado;

        char mejor_nombre[256];
        mejor_nombre[0] = '\0';
        uint32_t padre_idx = 0;
        uint64_t tam_real = 0;
        int tiene_nombre = 0;
        uint8_t mejor_namespace = 0xFF;

        while (offset + sizeof(struct ntfs_atributo_cabecera) <= tam_usado && offset < g_vol_ntfs.tamano_registro_mft) {
            struct ntfs_atributo_cabecera *attr = (struct ntfs_atributo_cabecera *)&g_ntfs_record_buf[offset];
            if (attr->tipo == NTFS_ATTR_END || attr->longitud == 0) break;

            if (attr->tipo == NTFS_ATTR_FILE_NAME && attr->no_residente == 0) {
                struct ntfs_atributo_residente *res = (struct ntfs_atributo_residente *)attr;
                if (offset + res->offset_valor + sizeof(struct ntfs_attr_file_name) <= tam_usado) {
                    struct ntfs_attr_file_name *fn = (struct ntfs_attr_file_name *)&g_ntfs_record_buf[offset + res->offset_valor];
                    uint8_t ns = fn->namespace_tipo;

                    // Preferir Win32 o POSIX sobre DOS 8.3
                    if (!tiene_nombre || ns == 1 || ns == 3 || (mejor_namespace == 2 && ns != 2)) {
                        mejor_namespace = ns;
                        padre_idx = (uint32_t)(fn->referencia_directorio_padre & 0x0000FFFFFFFFFFFFULL);
                        tam_real = fn->tamano_real;

                        int nlen = fn->longitud_nombre;
                        int nout = 0;
                        for (int k = 0; k < nlen && nout < 250; k++) {
                            uint16_t ch = fn->nombre_utf16[k];
                            mejor_nombre[nout++] = (ch < 128) ? (char)ch : '_';
                        }
                        mejor_nombre[nout] = '\0';
                        tiene_nombre = 1;
                    }
                }
            } else if (attr->tipo == NTFS_ATTR_DATA) {
                if (attr->no_residente == 0) {
                    struct ntfs_atributo_residente *res = (struct ntfs_atributo_residente *)attr;
                    tam_real = res->longitud_valor;
                } else {
                    struct ntfs_atributo_no_residente *no_res = (struct ntfs_atributo_no_residente *)attr;
                    tam_real = no_res->tamano_real;
                }
            }

            offset += attr->longitud;
        }

        if (tiene_nombre && mejor_nombre[0] != '\0') {
            // Ignorar archivos de metadatos del sistema NTFS ($MFT, $LogFile, etc.)
            int es_sistema = 0;
            if (idx < 16 || mejor_nombre[0] == '$') {
                es_sistema = 1;
            }

            g_ntfs_nodos[g_total_nodos_ntfs].mft_idx = idx;
            g_ntfs_nodos[g_total_nodos_ntfs].padre_mft_idx = padre_idx;
            memcpy(g_ntfs_nodos[g_total_nodos_ntfs].nombre, mejor_nombre, ntfs_strlen(mejor_nombre) + 1);
            g_ntfs_nodos[g_total_nodos_ntfs].es_directorio = es_dir;
            g_ntfs_nodos[g_total_nodos_ntfs].es_sistema = es_sistema;
            g_ntfs_nodos[g_total_nodos_ntfs].tamano_bytes = tam_real;
            g_total_nodos_ntfs++;
        }
    }
}

int ntfs_montar(uint8_t unidad_msc) {
    g_vol_ntfs.montado = 0;
    g_vol_ntfs.unidad_msc = unidad_msc;

    // 1. Leer LBA 0 (buscar MBR o Superfloppy)
    if (usb_msc_leer_sectores(unidad_msc, 0, 1, g_ntfs_sector_buf) != 0) {
        return -1;
    }

    uint32_t lba_particion = 0;
    int encontrado = 0;

    // Caso A: Superfloppy (VBR en LBA 0)
    if (memcmp(&g_ntfs_sector_buf[3], "NTFS    ", 8) == 0) {
        lba_particion = 0;
        encontrado = 1;
    }

    // Caso B: Partición MBR
    if (!encontrado && g_ntfs_sector_buf[510] == 0x55 && g_ntfs_sector_buf[511] == 0xAA) {
        for (int p = 0; p < 4; p++) {
            uint32_t off = 446 + p * 16;
            uint8_t tipo = g_ntfs_sector_buf[off + 4];
            uint32_t inicio_lba = *(uint32_t *)&g_ntfs_sector_buf[off + 8];

            if (tipo == 0x07 && inicio_lba != 0) { // Tipo 0x07 NTFS
                static uint8_t sector_prueba[512] __attribute__((aligned(16)));
                if (usb_msc_leer_sectores(unidad_msc, inicio_lba, 1, sector_prueba) == 0) {
                    if (memcmp(&sector_prueba[3], "NTFS    ", 8) == 0) {
                        lba_particion = inicio_lba;
                        memcpy(g_ntfs_sector_buf, sector_prueba, 512);
                        encontrado = 1;
                        break;
                    }
                }
            }
        }
    }

    if (!encontrado) return -1;

    struct ntfs_vbr *vbr = (struct ntfs_vbr *)g_ntfs_sector_buf;

    g_vol_ntfs.lba_inicio_particion = lba_particion;
    g_vol_ntfs.bytes_por_sector = vbr->bytes_por_sector;
    g_vol_ntfs.sectores_por_cluster = vbr->sectores_por_cluster;
    g_vol_ntfs.bytes_por_cluster = g_vol_ntfs.bytes_por_sector * g_vol_ntfs.sectores_por_cluster;
    g_vol_ntfs.lcn_mft = vbr->lcn_mft;
    g_vol_ntfs.total_sectores = vbr->total_sectores;

    if (vbr->clusters_por_registro_mft < 0) {
        g_vol_ntfs.tamano_registro_mft = 1U << (-vbr->clusters_por_registro_mft);
    } else {
        g_vol_ntfs.tamano_registro_mft = (uint32_t)vbr->clusters_por_registro_mft * g_vol_ntfs.bytes_por_cluster;
    }

    if (g_vol_ntfs.bytes_por_sector == 0) return -2;
    g_vol_ntfs.sectores_por_registro_mft = g_vol_ntfs.tamano_registro_mft / g_vol_ntfs.bytes_por_sector;

    memcpy(g_vol_ntfs.etiqueta, "NTFS_VOL", 9);

    g_vol_ntfs.montado = 1;
    ntfs_poblar_catalogo();
    return 0;
}

static void ntfs_tree_recursivo(uint32_t padre_idx, const char *prefijo, int profundidad) {
    if (profundidad > 5) return;

    // Contar cuántos hijos tiene este padre
    int total_hijos = 0;
    for (int i = 0; i < g_total_nodos_ntfs; i++) {
        if (!g_ntfs_nodos[i].es_sistema && g_ntfs_nodos[i].padre_mft_idx == padre_idx && g_ntfs_nodos[i].mft_idx != padre_idx) {
            total_hijos++;
        }
    }

    int contador = 0;
    for (int i = 0; i < g_total_nodos_ntfs; i++) {
        if (g_ntfs_nodos[i].es_sistema || g_ntfs_nodos[i].padre_mft_idx != padre_idx || g_ntfs_nodos[i].mft_idx == padre_idx) {
            continue;
        }

        contador++;
        int es_ultimo = (contador == total_hijos);

        consola_imprimir(prefijo);
        if (es_ultimo) {
            consola_imprimir("└── ");
        } else {
            consola_imprimir("├── ");
        }

        if (g_ntfs_nodos[i].es_directorio) {
            g_tree_ntfs_directorios++;
            consola_imprimir_color(g_ntfs_nodos[i].nombre, COLOR_DIR_NTFS);
            consola_imprimir_linea_color("/", COLOR_DIR_NTFS);

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

            ntfs_tree_recursivo(g_ntfs_nodos[i].mft_idx, nuevo_prefijo, profundidad + 1);
        } else {
            g_tree_ntfs_archivos++;
            g_tree_ntfs_bytes_totales += g_ntfs_nodos[i].tamano_bytes;

            uint32_t color = COLOR_FILE_NTFS;
            const char *nombre = g_ntfs_nodos[i].nombre;
            int nlen = 0;
            while (nombre[nlen]) nlen++;

            if ((nlen > 4 && memcmp(&nombre[nlen - 4], ".txt", 4) == 0) ||
                (nlen > 4 && memcmp(&nombre[nlen - 4], ".cfg", 4) == 0)) {
                color = COLOR_TXT_NTFS;
            } else if ((nlen > 4 && memcmp(&nombre[nlen - 4], ".efi", 4) == 0) ||
                       (nlen > 4 && memcmp(&nombre[nlen - 4], ".bin", 4) == 0)) {
                color = COLOR_BIN_NTFS;
            }

            consola_imprimir_color(nombre, color);
            consola_imprimir("  (");
            if (g_ntfs_nodos[i].tamano_bytes < 1024) {
                consola_imprimir_dec((uint32_t)g_ntfs_nodos[i].tamano_bytes);
                consola_imprimir(" B)");
            } else if (g_ntfs_nodos[i].tamano_bytes < 1024 * 1024) {
                consola_imprimir_dec((uint32_t)(g_ntfs_nodos[i].tamano_bytes / 1024));
                consola_imprimir(" KB)");
            } else {
                consola_imprimir_dec((uint32_t)(g_ntfs_nodos[i].tamano_bytes / (1024 * 1024)));
                consola_imprimir(" MB)");
            }
            consola_imprimir_linea("");
        }
    }
}

int ntfs_ejecutar_tree(const char *ruta_inicial) {
    (void)ruta_inicial;
    if (!g_vol_ntfs.montado) {
        if (ntfs_montar(0) != 0) {
            consola_imprimir_linea_color("Error: No se encontró volumen NTFS montable en la unidad USB.", COLOR_AVISO_NTFS);
            return -1;
        }
    }

    consola_imprimir_linea_color("================== ÁRBOL DE ARCHIVOS NTFS (USB MSC) ==================", COLOR_AVISO_NTFS);
    consola_imprimir("Unidad USB: ");
    consola_imprimir_color(g_vol_ntfs.etiqueta, COLOR_DIR_NTFS);
    consola_imprimir("  (LCN $MFT: ");
    consola_imprimir_dec((uint32_t)g_vol_ntfs.lcn_mft);
    consola_imprimir_linea(")");

    consola_imprimir_linea_color(".", COLOR_DIR_NTFS);

    g_tree_ntfs_directorios = 0;
    g_tree_ntfs_archivos = 0;
    g_tree_ntfs_bytes_totales = 0;

    // Directorio raíz en NTFS es el registro MFT 5
    ntfs_tree_recursivo(5, "", 1);

    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_AVISO_NTFS);
    consola_imprimir("Resumen: ");
    consola_imprimir_dec(g_tree_ntfs_directorios);
    consola_imprimir(" directorios, ");
    consola_imprimir_dec(g_tree_ntfs_archivos);
    consola_imprimir(" archivos (Total: ");
    if (g_tree_ntfs_bytes_totales < 1024) {
        consola_imprimir_dec((uint32_t)g_tree_ntfs_bytes_totales);
        consola_imprimir(" B)");
    } else if (g_tree_ntfs_bytes_totales < 1024 * 1024) {
        consola_imprimir_dec((uint32_t)(g_tree_ntfs_bytes_totales / 1024));
        consola_imprimir(" KB)");
    } else {
        consola_imprimir_dec((uint32_t)(g_tree_ntfs_bytes_totales / (1024 * 1024)));
        consola_imprimir(" MB)");
    }
    consola_imprimir_linea("");
    consola_imprimir_linea_color("======================================================================", COLOR_AVISO_NTFS);
    return 0;
}

int ntfs_listar_directorio(const char *ruta) {
    (void)ruta;
    if (!g_vol_ntfs.montado) {
        if (ntfs_montar(0) != 0) {
            consola_imprimir_linea_color("Error: No se encontró volumen NTFS montable.", COLOR_AVISO_NTFS);
            return -1;
        }
    }

    consola_imprimir_linea_color("TIPO    TAMAÑO     MFT#    NOMBRE", COLOR_AVISO_NTFS);
    consola_imprimir_linea_color("----    ------     ----    ------", COLOR_AVISO_NTFS);

    for (int i = 0; i < g_total_nodos_ntfs; i++) {
        if (g_ntfs_nodos[i].es_sistema || g_ntfs_nodos[i].padre_mft_idx != 5) continue;

        if (g_ntfs_nodos[i].es_directorio) {
            consola_imprimir_color("[DIR]   ", COLOR_DIR_NTFS);
            consola_imprimir("   -       ");
        } else {
            consola_imprimir_color("[ARCH]  ", COLOR_FILE_NTFS);
            consola_imprimir_dec((uint32_t)g_ntfs_nodos[i].tamano_bytes);
            consola_imprimir(" B    ");
        }
        consola_imprimir_dec(g_ntfs_nodos[i].mft_idx);
        consola_imprimir("       ");
        consola_imprimir_linea(g_ntfs_nodos[i].nombre);
    }
    return 0;
}

int ntfs_leer_archivo_texto(const char *ruta) {
    if (!ruta || ruta[0] == '\0') {
        consola_imprimir_linea_color("Uso: cat <archivo> (ej. cat notas.txt)", COLOR_AVISO_NTFS);
        return -1;
    }

    if (!g_vol_ntfs.montado) {
        if (ntfs_montar(0) != 0) {
            consola_imprimir_linea_color("Error: No hay volumen NTFS montado.", COLOR_AVISO_NTFS);
            return -1;
        }
    }

    int idx_nodo = -1;
    for (int i = 0; i < g_total_nodos_ntfs; i++) {
        if (!g_ntfs_nodos[i].es_directorio && ntfs_str_igual_sin_caso(g_ntfs_nodos[i].nombre, ruta)) {
            idx_nodo = i;
            break;
        }
    }

    if (idx_nodo < 0) {
        consola_imprimir_color("Archivo no encontrado en NTFS: ", COLOR_AVISO_NTFS);
        consola_imprimir_linea(ruta);
        return -2;
    }

    uint32_t mft_idx = g_ntfs_nodos[idx_nodo].mft_idx;
    if (ntfs_leer_registro_mft(mft_idx, g_ntfs_record_buf) != 0) {
        consola_imprimir_linea_color("Error al leer registro MFT del archivo.", COLOR_AVISO_NTFS);
        return -3;
    }

    struct ntfs_registro_mft *reg = (struct ntfs_registro_mft *)g_ntfs_record_buf;
    uint32_t offset = reg->primer_atributo_offset;
    uint32_t tam_usado = reg->tamano_usado;

    consola_imprimir("==> [ NTFS: ");
    consola_imprimir(g_ntfs_nodos[idx_nodo].nombre);
    consola_imprimir(" (");
    consola_imprimir_dec((uint32_t)g_ntfs_nodos[idx_nodo].tamano_bytes);
    consola_imprimir_linea(" bytes) ] <==");

    while (offset + sizeof(struct ntfs_atributo_cabecera) <= tam_usado && offset < g_vol_ntfs.tamano_registro_mft) {
        struct ntfs_atributo_cabecera *attr = (struct ntfs_atributo_cabecera *)&g_ntfs_record_buf[offset];
        if (attr->tipo == NTFS_ATTR_END || attr->longitud == 0) break;

        if (attr->tipo == NTFS_ATTR_DATA) {
            if (attr->no_residente == 0) {
                // Datos residentes: contenidos directamente en el MFT
                struct ntfs_atributo_residente *res = (struct ntfs_atributo_residente *)attr;
                uint8_t *datos = &g_ntfs_record_buf[offset + res->offset_valor];
                uint32_t len = res->longitud_valor;

                for (uint32_t b = 0; b < len; b++) {
                    char c = (char)datos[b];
                    if (c == '\r') continue;
                    consola_escribir_caracter(c);
                }
                consola_imprimir_linea("");
                return 0;
            } else {
                // Datos no residentes: decodificar Data Runs
                struct ntfs_atributo_no_residente *no_res = (struct ntfs_atributo_no_residente *)attr;
                uint8_t *run_ptr = &g_ntfs_record_buf[offset + no_res->offset_data_runs];
                uint64_t bytes_restantes = no_res->tamano_real;
                int64_t prev_lcn = 0;

                while (*run_ptr != 0 && bytes_restantes > 0) {
                    uint8_t header = *run_ptr++;
                    uint8_t len_size = header & 0x0F;
                    uint8_t off_size = (header >> 4) & 0x0F;

                    if (len_size == 0 || len_size > 8 || off_size > 8) break;

                    uint64_t run_len = 0;
                    for (int k = 0; k < len_size; k++) {
                        run_len |= ((uint64_t)*run_ptr++) << (k * 8);
                    }

                    int64_t lcn_delta = 0;
                    if (off_size > 0) {
                        for (int k = 0; k < off_size; k++) {
                            lcn_delta |= ((int64_t)*run_ptr++) << (k * 8);
                        }
                        // Extensión de signo
                        if (*(run_ptr - 1) & 0x80) {
                            for (int k = off_size; k < 8; k++) {
                                lcn_delta |= ((int64_t)0xFF) << (k * 8);
                            }
                        }
                    }

                    int64_t curr_lcn = prev_lcn + lcn_delta;
                    prev_lcn = curr_lcn;

                    // Leer clusters correspondientes a este run
                    for (uint64_t c = 0; c < run_len && bytes_restantes > 0; c++) {
                        uint32_t cluster_lba = g_vol_ntfs.lba_inicio_particion + (uint32_t)((curr_lcn + c) * g_vol_ntfs.sectores_por_cluster);
                        if (usb_msc_leer_sectores(g_vol_ntfs.unidad_msc, cluster_lba, g_vol_ntfs.sectores_por_cluster, g_ntfs_cluster_buf) != 0) {
                            consola_imprimir_linea_color("[Error I/O leyendo cluster NTFS]", COLOR_AVISO_NTFS);
                            break;
                        }

                        uint32_t a_leer = (bytes_restantes > g_vol_ntfs.bytes_por_cluster) ? g_vol_ntfs.bytes_por_cluster : (uint32_t)bytes_restantes;
                        for (uint32_t b = 0; b < a_leer; b++) {
                            char ch = (char)g_ntfs_cluster_buf[b];
                            if (ch == '\r') continue;
                            consola_escribir_caracter(ch);
                        }
                        bytes_restantes -= a_leer;
                    }
                }
                consola_imprimir_linea("");
                return 0;
            }
        }

        offset += attr->longitud;
    }

    consola_imprimir_linea_color("[Archivo sin flujo de datos $DATA]", COLOR_AVISO_NTFS);
    return 0;
}
