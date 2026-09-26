#include "fat32.h"
#include "usb_msc.h"
#include "consola.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/memoria.h"

// ============================================================================
// TAEK OS - CONTROLADOR DE SISTEMA DE ARCHIVOS FAT32 (Hito 49)
// Arquitectura Anillo 0 en Español
// ============================================================================

static struct fat32_volumen g_volumen = {0};
static int g_fat32_inicializado = 0;

// Búferes estáticos en BSS para evitar consumir la pila del kernel
static uint8_t g_sector_buf[512];
static uint8_t g_fat_sector_buf[512];
static uint8_t g_cluster_buf[32768]; // Soporta clusters de hasta 32 KiB (64 sectores)

// Estructura interna para almacenar temporalmente los elementos de un directorio
#define FAT32_MAX_ITEMS_DIR 128
struct fat32_item {
    char     nombre[256];
    uint8_t  es_directorio;
    uint32_t cluster_inicio;
    uint32_t tamano;
};

static struct fat32_item g_items_actuales[FAT32_MAX_ITEMS_DIR];

// Métricas de árbol
static uint32_t g_arbol_directorios = 0;
static uint32_t g_arbol_archivos = 0;
static uint64_t g_arbol_bytes_totales = 0;

// Estado del acumulador de Nombres Largos (LFN)
static char g_lfn_buffer[256];
static int  g_lfn_activo = 0;

void fat32_iniciar(void) {
    if (g_fat32_inicializado) return;
    g_volumen.montado = 0;
    g_fat32_inicializado = 1;
    serial_imprimir_linea("[FAT32] Subsistema de archivos FAT32 inicializado en Ring 0.");
}

int fat32_esta_montado(void) {
    return g_volumen.montado;
}

const struct fat32_volumen *fat32_obtener_volumen(void) {
    if (!g_volumen.montado) return NULL;
    return &g_volumen;
}

void fat32_desmontar(void) {
    if (g_volumen.montado) {
        serial_imprimir("[FAT32] Volumen '");
        serial_imprimir(g_volumen.etiqueta);
        serial_imprimir_linea("' desmontado.");
    }
    g_volumen.montado = 0;
}

// Convierte un número de cluster FAT32 a su LBA físico de inicio en el disco
static uint32_t fat32_cluster_a_lba(const struct fat32_volumen *vol, uint32_t cluster) {
    if (cluster < 2) return 0;
    return vol->lba_datos + ((cluster - 2) * (uint32_t)vol->sectores_por_cluster);
}

// Consulta la tabla FAT para obtener el siguiente cluster en la cadena
static uint32_t fat32_siguiente_cluster(const struct fat32_volumen *vol, uint32_t cluster_actual) {
    if (!vol || !vol->montado || cluster_actual < 2) return 0;

    uint32_t fat_offset_bytes = cluster_actual * 4;
    uint32_t fat_sector_lba = vol->lba_fat + (fat_offset_bytes / vol->bytes_por_sector);
    uint32_t offset_en_sector = fat_offset_bytes % vol->bytes_por_sector;

    int res = usb_msc_leer_sectores(vol->unidad_msc, fat_sector_lba, 1, g_fat_sector_buf);
    if (res != 0) {
        serial_imprimir("[FAT32 ERROR] No se pudo leer sector de FAT: ");
        serial_imprimir_dec(fat_sector_lba);
        serial_imprimir_linea("");
        return 0;
    }

    uint32_t raw_val = *(const uint32_t *)&g_fat_sector_buf[offset_en_sector];
    uint32_t sig_cluster = raw_val & 0x0FFFFFFFU; // FAT32 usa los 28 bits bajos

    // Fin de cadena de clusters (EOC: End Of Clusterchain >= 0x0FFFFFF8)
    if (sig_cluster >= 0x0FFFFFF8U || sig_cluster == 0) {
        return 0; // Fin de cadena alcanzado
    }

    // Cluster defectuoso
    if (sig_cluster == 0x0FFFFFF7U) {
        serial_imprimir_linea("[FAT32 AVISO] Se detectó cluster defectuoso (Bad Cluster).");
        return 0;
    }

    return sig_cluster;
}

// Monta el sistema de archivos FAT32 escaneando MBR o Superfloppy
int fat32_montar(uint8_t unidad_msc) {
    if (!g_fat32_inicializado) fat32_iniciar();

    if (unidad_msc >= USB_MSC_MAX_DISPOSITIVOS) return -1;
    const struct usb_msc_dispositivo *dev = usb_msc_obtener_dispositivo(unidad_msc);
    if (!dev || !dev->activo || !dev->listo) {
        return -2;
    }

    // Si ya está montada esta misma unidad, retornar éxito
    if (g_volumen.montado && g_volumen.unidad_msc == unidad_msc) {
        return 0;
    }

    // 1. Leer LBA 0 (Sector MBR o VBR)
    int res = usb_msc_leer_sectores(unidad_msc, 0, 1, g_sector_buf);
    if (res != 0) {
        serial_imprimir_linea("[FAT32 ERROR] Falló lectura de LBA 0.");
        return -3;
    }

    // Comprobar firma mágica de arranque 0x55AA al final del sector
    uint16_t firma = (uint16_t)g_sector_buf[510] | ((uint16_t)g_sector_buf[511] << 8);
    if (firma != 0xAA55) {
        serial_imprimir_linea("[FAT32 ERROR] LBA 0 no posee firma de arranque 0x55AA.");
        return -4;
    }

    uint32_t lba_particion = 0;
    int particion_encontrada = 0;

    // Verificar si LBA 0 es directamente un VBR FAT32 (Superfloppy format)
    if (g_sector_buf[82] == 'F' && g_sector_buf[83] == 'A' && g_sector_buf[84] == 'T' &&
        g_sector_buf[85] == '3' && g_sector_buf[86] == '2') {
        lba_particion = 0;
        particion_encontrada = 1;
        serial_imprimir_linea("  [FAT32] Formato Superfloppy detectado directamente en LBA 0.");
    } else {
        // Escanear la tabla de particiones MBR (offset 446 a 509)
        for (int p = 0; p < 4; p++) {
            uint32_t p_off = 446 + (p * 16);
            uint8_t tipo = g_sector_buf[p_off + 4];
            uint32_t inicio_lba = (uint32_t)g_sector_buf[p_off + 8] |
                                 ((uint32_t)g_sector_buf[p_off + 9] << 8) |
                                 ((uint32_t)g_sector_buf[p_off + 10] << 16) |
                                 ((uint32_t)g_sector_buf[p_off + 11] << 24);
            uint32_t cant_sec = (uint32_t)g_sector_buf[p_off + 12] |
                                ((uint32_t)g_sector_buf[p_off + 13] << 8) |
                                ((uint32_t)g_sector_buf[p_off + 14] << 16) |
                                ((uint32_t)g_sector_buf[p_off + 15] << 24);

            if (cant_sec > 0 && inicio_lba > 0) {
                // Particiones comunes de FAT32: 0x0B (FAT32 CHS), 0x0C (FAT32 LBA)
                // O probar cualquier partición que contenga cabecera FAT32 en su VBR
                if (tipo == 0x0B || tipo == 0x0C || tipo == 0x07 || tipo == 0xEE || tipo == 0x83) {
                    lba_particion = inicio_lba;
                    particion_encontrada = 1;
                    break;
                }
            }
        }
    }

    if (!particion_encontrada) {
        serial_imprimir_linea("[FAT32 ERROR] No se localizó ninguna partición compatible en el MBR.");
        return -5;
    }

    // 2. Si no era Superfloppy, leer el sector VBR en lba_particion
    if (lba_particion != 0) {
        res = usb_msc_leer_sectores(unidad_msc, lba_particion, 1, g_sector_buf);
        if (res != 0) {
            serial_imprimir_linea("[FAT32 ERROR] Falló lectura del VBR en inicio de partición.");
            return -6;
        }
    }

    // 3. Parsear el BIOS Parameter Block (BPB) FAT32
    const struct fat32_bpb *bpb = (const struct fat32_bpb *)g_sector_buf;

    uint16_t bytes_sec = bpb->bytes_por_sector;
    if (bytes_sec == 0 || bytes_sec > 4096) bytes_sec = 512;

    uint8_t sec_per_clus = bpb->sectores_por_cluster;
    if (sec_per_clus == 0) {
        serial_imprimir_linea("[FAT32 ERROR] Sectores por cluster inválido (0).");
        return -7;
    }

    uint16_t sec_resv = bpb->sectores_reservados;
    uint8_t num_fats = bpb->num_fats;
    uint32_t sec_fat = bpb->sectores_por_fat_32;
    uint32_t root_clus = bpb->cluster_raiz;

    if (num_fats == 0 || sec_fat == 0 || root_clus < 2) {
        serial_imprimir_linea("[FAT32 ERROR] Parámetros BPB incompatibles con especificación FAT32.");
        return -8;
    }

    // Guardar descriptor del volumen montado
    g_volumen.montado = 1;
    g_volumen.unidad_msc = unidad_msc;
    g_volumen.lba_inicio_particion = lba_particion;
    g_volumen.bytes_por_sector = bytes_sec;
    g_volumen.sectores_por_cluster = sec_per_clus;
    g_volumen.bytes_por_cluster = (uint32_t)sec_per_clus * (uint32_t)bytes_sec;
    g_volumen.sectores_por_fat = sec_fat;
    g_volumen.cluster_raiz = root_clus;

    g_volumen.lba_fat = lba_particion + (uint32_t)sec_resv;
    g_volumen.lba_datos = g_volumen.lba_fat + ((uint32_t)num_fats * sec_fat);

    for (int i = 0; i < 11; i++) {
        char c = bpb->etiqueta_volumen[i];
        g_volumen.etiqueta[i] = (c >= 32 && c <= 126) ? c : ' ';
    }
    g_volumen.etiqueta[11] = '\0';

    serial_imprimir("  [FAT32] Volumen '");
    serial_imprimir(g_volumen.etiqueta);
    serial_imprimir("' montado [OK]. Cluster raíz: ");
    serial_imprimir_dec(g_volumen.cluster_raiz);
    serial_imprimir(" | LBA Datos: ");
    serial_imprimir_dec(g_volumen.lba_datos);
    serial_imprimir(" | Bytes/Cluster: ");
    serial_imprimir_dec(g_volumen.bytes_por_cluster);
    serial_imprimir_linea("");

    return 0;
}

// Procesa una entrada LFN extrayendo los caracteres a g_lfn_buffer
static void fat32_procesar_entrada_lfn(const struct fat32_entrada_lfn *lfn) {
    uint8_t orden = lfn->orden;
    int seq = (orden & 0x1F); // 1..31
    if (seq < 1 || seq > 20) return;

    if (orden & 0x40) {
        // Último fragmento (inicio del nombre en lectura inversa)
        for (int i = 0; i < 256; i++) g_lfn_buffer[i] = '\0';
        g_lfn_activo = 1;
    }

    if (!g_lfn_activo) return;

    int base_idx = (seq - 1) * 13;

    // Caracteres 1 a 5
    for (int i = 0; i < 5; i++) {
        uint16_t wc = lfn->nombre1[i];
        if (wc == 0x0000 || wc == 0xFFFF) break;
        if (base_idx + i < 255) g_lfn_buffer[base_idx + i] = (char)(wc & 0xFF);
    }
    // Caracteres 6 a 11
    for (int i = 0; i < 6; i++) {
        uint16_t wc = lfn->nombre2[i];
        if (wc == 0x0000 || wc == 0xFFFF) break;
        if (base_idx + 5 + i < 255) g_lfn_buffer[base_idx + 5 + i] = (char)(wc & 0xFF);
    }
    // Caracteres 12 a 13
    for (int i = 0; i < 2; i++) {
        uint16_t wc = lfn->nombre3[i];
        if (wc == 0x0000 || wc == 0xFFFF) break;
        if (base_idx + 11 + i < 255) g_lfn_buffer[base_idx + 11 + i] = (char)(wc & 0xFF);
    }
}

// Limpia y formatea un nombre 8.3 clásico si no hay LFN disponible
static void fat32_formatear_nombre_83(const char raw[11], char *dest, int es_dir) {
    int pos = 0;

    // Nombre (primeros 8 caracteres)
    for (int i = 0; i < 8; i++) {
        if (raw[i] != ' ') {
            char c = raw[i];
            if (c >= 'A' && c <= 'Z') c += 32; // Normalizar a minúsculas
            dest[pos++] = c;
        }
    }

    // Extensión (últimos 3 caracteres si no es directorio)
    if (!es_dir) {
        int tiene_ext = 0;
        for (int i = 8; i < 11; i++) {
            if (raw[i] != ' ') {
                tiene_ext = 1;
                break;
            }
        }
        if (tiene_ext) {
            dest[pos++] = '.';
            for (int i = 8; i < 11; i++) {
                if (raw[i] != ' ') {
                    char c = raw[i];
                    if (c >= 'A' && c <= 'Z') c += 32;
                    dest[pos++] = c;
                }
            }
        }
    }

    dest[pos] = '\0';
}

// Carga las entradas de un directorio en g_items_actuales
static int fat32_leer_entradas_directorio(uint32_t cluster_dir, int *num_items) {
    *num_items = 0;
    if (!g_volumen.montado || cluster_dir < 2) return -1;

    uint32_t cluster_actual = cluster_dir;
    g_lfn_activo = 0;
    for (int i = 0; i < 256; i++) g_lfn_buffer[i] = '\0';

    while (cluster_actual >= 2 && *num_items < FAT32_MAX_ITEMS_DIR) {
        uint32_t lba = fat32_cluster_a_lba(&g_volumen, cluster_actual);
        uint16_t sec_cant = g_volumen.sectores_por_cluster;
        if (sec_cant > 64) sec_cant = 64;

        int res = usb_msc_leer_sectores(g_volumen.unidad_msc, lba, sec_cant, g_cluster_buf);
        if (res != 0) {
            serial_imprimir_linea("[FAT32 ERROR] Error leyendo cluster de directorio.");
            break;
        }

        uint32_t total_entradas = g_volumen.bytes_por_cluster / 32;
        int fin_directorio = 0;

        for (uint32_t e = 0; e < total_entradas; e++) {
            const uint8_t *raw_entry = &g_cluster_buf[e * 32];
            uint8_t primer_byte = raw_entry[0];

            if (primer_byte == 0x00) {
                // Entrada no asignada y fin de las entradas de este directorio
                fin_directorio = 1;
                break;
            }

            if (primer_byte == 0xE5) {
                // Entrada eliminada, reiniciar acumulador LFN
                g_lfn_activo = 0;
                continue;
            }

            uint8_t attr = raw_entry[11];

            // ¿Es entrada de Nombre Largo (LFN)?
            if (attr == FAT32_ATTR_LFN) {
                fat32_procesar_entrada_lfn((const struct fat32_entrada_lfn *)raw_entry);
                continue;
            }

            // Ignorar etiqueta de volumen o entradas ocultas del sistema que no sean directorios
            if ((attr & FAT32_ATTR_VOLUME_ID) && !(attr & FAT32_ATTR_DIRECTORY)) {
                g_lfn_activo = 0;
                continue;
            }

            const struct fat32_entrada_dir *entry = (const struct fat32_entrada_dir *)raw_entry;
            int es_dir = (attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;

            // Ignorar '.' y '..' para no generar ciclos recursivos en el árbol
            if (entry->nombre[0] == '.') {
                g_lfn_activo = 0;
                continue;
            }

            struct fat32_item *item = &g_items_actuales[*num_items];
            item->es_directorio = es_dir;
            item->cluster_inicio = ((uint32_t)entry->cluster_alto << 16) | (uint32_t)entry->cluster_bajo;
            item->tamano = entry->tamano_archivo;

            // Asignar nombre (LFN o 8.3)
            if (g_lfn_activo && g_lfn_buffer[0] != '\0') {
                int p = 0;
                while (g_lfn_buffer[p] && p < 255) {
                    item->nombre[p] = g_lfn_buffer[p];
                    p++;
                }
                item->nombre[p] = '\0';
            } else {
                fat32_formatear_nombre_83(entry->nombre, item->nombre, es_dir);
            }

            g_lfn_activo = 0;
            for (int i = 0; i < 256; i++) g_lfn_buffer[i] = '\0';

            (*num_items)++;
            if (*num_items >= FAT32_MAX_ITEMS_DIR) break;
        }

        if (fin_directorio) break;
        cluster_actual = fat32_siguiente_cluster(&g_volumen, cluster_actual);
    }

    return 0;
}

// Imprime el tamaño formateado en B, KB o MB
static void fat32_imprimir_tamano(uint32_t bytes) {
    if (bytes >= 1024 * 1024) {
        consola_imprimir_dec(bytes / (1024 * 1024));
        consola_imprimir(".");
        consola_imprimir_dec((bytes % (1024 * 1024)) / 100000);
        consola_imprimir(" MB");
    } else if (bytes >= 1024) {
        consola_imprimir_dec(bytes / 1024);
        consola_imprimir(" KB");
    } else {
        consola_imprimir_dec(bytes);
        consola_imprimir(" B");
    }
}

// Función recursiva para dibujar las ramas del árbol estilo 'tree'
static void fat32_dibujar_ramas_recursivo(uint32_t cluster_dir, int profundidad, uint32_t mascara_ultimos) {
    if (profundidad >= 5) return; // Límite de seguridad para evitar exceso de profundidad

    int num_items = 0;
    // Guardar los items en una copia local porque la función es recursiva
    struct fat32_item items_locales[64];
    fat32_leer_entradas_directorio(cluster_dir, &num_items);

    int cant_a_copiar = (num_items < 64) ? num_items : 64;
    for (int i = 0; i < cant_a_copiar; i++) {
        items_locales[i] = g_items_actuales[i];
    }

    for (int i = 0; i < cant_a_copiar; i++) {
        const struct fat32_item *it = &items_locales[i];
        int es_ultimo = (i == cant_a_copiar - 1);

        // Imprimir líneas de conexión según los ancestros
        for (int p = 0; p < profundidad; p++) {
            if (mascara_ultimos & (1 << p)) {
                consola_imprimir("    ");
            } else {
                consola_imprimir_color("│   ", COLOR_AVISO_DEFAULT);
            }
        }

        // Conector de la rama actual (├── o └──)
        if (es_ultimo) {
            consola_imprimir_color("└── ", COLOR_AVISO_DEFAULT);
        } else {
            consola_imprimir_color("├── ", COLOR_AVISO_DEFAULT);
        }

        // Renderizado del elemento según si es carpeta o archivo
        if (it->es_directorio) {
            g_arbol_directorios++;
            consola_imprimir_color(it->nombre, COLOR_USUARIO_DEFAULT);
            consola_imprimir_linea_color("/", COLOR_USUARIO_DEFAULT);

            uint32_t nueva_mascara = mascara_ultimos;
            if (es_ultimo) nueva_mascara |= (1 << profundidad);

            fat32_dibujar_ramas_recursivo(it->cluster_inicio, profundidad + 1, nueva_mascara);
        } else {
            g_arbol_archivos++;
            g_arbol_bytes_totales += it->tamano;

            // Colorear archivos según su extensión
            int len = 0;
            while (it->nombre[len]) len++;
            uint32_t color_archivo = COLOR_TEXTO_DEFAULT;

            if (len >= 4) {
                const char *ext = &it->nombre[len - 4];
                if (ext[0] == '.') {
                    if ((ext[1] == 'e' || ext[1] == 'E') && (ext[2] == 'f' || ext[2] == 'F') && (ext[3] == 'i' || ext[3] == 'I')) {
                        color_archivo = COLOR_EXITO_DEFAULT; // Binarios UEFI
                    } else if ((ext[1] == 'b' || ext[1] == 'B') && (ext[2] == 'i' || ext[2] == 'I') && (ext[3] == 'n' || ext[3] == 'N')) {
                        color_archivo = COLOR_EXITO_DEFAULT; // Binarios firmware
                    } else if ((ext[1] == 't' || ext[1] == 'T') && (ext[2] == 'x' || ext[2] == 'X') && (ext[3] == 't' || ext[3] == 'T')) {
                        color_archivo = COLOR_PROMPT_DEFAULT; // Texto
                    } else if ((ext[1] == 'c' || ext[1] == 'C') && (ext[2] == 'f' || ext[2] == 'F') && (ext[3] == 'g' || ext[3] == 'G')) {
                        color_archivo = COLOR_PROMPT_DEFAULT; // Config
                    }
                }
            }

            consola_imprimir_color(it->nombre, color_archivo);
            consola_imprimir("  (");
            fat32_imprimir_tamano(it->tamano);
            consola_imprimir_linea(")");
        }
    }
}

// Ejecuta el comando 'tree' / 'arbol' sobre el sistema de archivos del pendrive
int fat32_ejecutar_tree(const char *ruta_inicial) {
    (void)ruta_inicial;

    // Verificar o montar automáticamente la primera unidad USB MSC
    if (!g_volumen.montado) {
        int m_res = fat32_montar(0);
        if (m_res != 0) {
            consola_imprimir_linea_color("  [!] No se pudo montar el sistema de archivos FAT32 en la memoria USB.", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea("      Verifica que el pendrive esté conectado ('usb') y formateado en FAT32.");
            return -1;
        }
    }

    g_arbol_directorios = 0;
    g_arbol_archivos = 0;
    g_arbol_bytes_totales = 0;

    consola_imprimir_linea_color("================== ÁRBOL DE ARCHIVOS FAT32 (USB MSC) ==================", COLOR_AVISO_DEFAULT);
    consola_imprimir_color("Unidad USB: ", COLOR_PROMPT_DEFAULT);
    consola_imprimir_color(g_volumen.etiqueta, COLOR_EXITO_DEFAULT);
    consola_imprimir(" (Cluster Raíz: ");
    consola_imprimir_dec(g_volumen.cluster_raiz);
    consola_imprimir_linea(")");
    consola_imprimir_linea_color(".", COLOR_USUARIO_DEFAULT);

    fat32_dibujar_ramas_recursivo(g_volumen.cluster_raiz, 0, 0);

    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);
    consola_imprimir_color("Resumen: ", COLOR_PROMPT_DEFAULT);
    consola_imprimir_dec(g_arbol_directorios);
    consola_imprimir(" directorios, ");
    consola_imprimir_dec(g_arbol_archivos);
    consola_imprimir(" archivos (Total: ");
    fat32_imprimir_tamano((uint32_t)g_arbol_bytes_totales);
    consola_imprimir_linea(")");
    consola_imprimir_linea_color("======================================================================", COLOR_AVISO_DEFAULT);

    return 0;
}

// Lista los contenidos del directorio raíz ('ls' / 'dir')
int fat32_listar_directorio(const char *ruta) {
    (void)ruta;

    if (!g_volumen.montado) {
        int m_res = fat32_montar(0);
        if (m_res != 0) {
            consola_imprimir_linea_color("  [!] No se pudo montar el sistema de archivos FAT32.", COLOR_ERROR_DEFAULT);
            return -1;
        }
    }

    int num_items = 0;
    fat32_leer_entradas_directorio(g_volumen.cluster_raiz, &num_items);

    consola_imprimir_linea_color("==================== LISTADO DE DIRECTORIO FAT32 ====================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("TIPO    TAMAÑO        NOMBRE", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("----------------------------------------------------------------------");

    for (int i = 0; i < num_items; i++) {
        const struct fat32_item *it = &g_items_actuales[i];
        if (it->es_directorio) {
            consola_imprimir_color("<DIR>   ", COLOR_USUARIO_DEFAULT);
            consola_imprimir("     ---      ");
            consola_imprimir_color(it->nombre, COLOR_USUARIO_DEFAULT);
            consola_imprimir_linea("/");
        } else {
            consola_imprimir("FILE    ");
            fat32_imprimir_tamano(it->tamano);
            // Espaciado
            consola_imprimir("      ");
            consola_imprimir_linea(it->nombre);
        }
    }

    consola_imprimir_linea_color("======================================================================", COLOR_AVISO_DEFAULT);
    return 0;
}

// Lee un archivo de texto plano de la raíz y lo muestra en la consola ('cat' / 'leer')
int fat32_leer_archivo_texto(const char *nombre_buscado) {
    if (!nombre_buscado || *nombre_buscado == '\0') {
        consola_imprimir_linea_color("Uso: cat <nombre_archivo> o leer <nombre_archivo>", COLOR_ERROR_DEFAULT);
        return -1;
    }

    if (!g_volumen.montado) {
        int m_res = fat32_montar(0);
        if (m_res != 0) {
            consola_imprimir_linea_color("  [!] Memoria USB no montada.", COLOR_ERROR_DEFAULT);
            return -2;
        }
    }

    int num_items = 0;
    fat32_leer_entradas_directorio(g_volumen.cluster_raiz, &num_items);

    const struct fat32_item *objetivo = NULL;
    for (int i = 0; i < num_items; i++) {
        // Comparación simple sin distinción de mayúsculas/minúsculas
        int match = 1;
        int p = 0;
        while (nombre_buscado[p] && g_items_actuales[i].nombre[p]) {
            char c1 = nombre_buscado[p];
            char c2 = g_items_actuales[i].nombre[p];
            if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
            if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
            if (c1 != c2) {
                match = 0;
                break;
            }
            p++;
        }
        if (match && nombre_buscado[p] == '\0' && g_items_actuales[i].nombre[p] == '\0') {
            objetivo = &g_items_actuales[i];
            break;
        }
    }

    if (!objetivo) {
        consola_imprimir("  [!] Archivo no encontrado: '");
        consola_imprimir(nombre_buscado);
        consola_imprimir_linea("'");
        return -3;
    }

    if (objetivo->es_directorio) {
        consola_imprimir_linea_color("  [!] El elemento especificado es un directorio, no un archivo.", COLOR_ERROR_DEFAULT);
        return -4;
    }

    consola_imprimir("==> Mostrando contenido de '");
    consola_imprimir(objetivo->nombre);
    consola_imprimir("' (");
    fat32_imprimir_tamano(objetivo->tamano);
    consola_imprimir_linea("):");
    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);

    uint32_t clus = objetivo->cluster_inicio;
    uint32_t bytes_restantes = objetivo->tamano;

    while (clus >= 2 && bytes_restantes > 0) {
        uint32_t lba = fat32_cluster_a_lba(&g_volumen, clus);
        uint16_t sec_cant = g_volumen.sectores_por_cluster;
        if (sec_cant > 64) sec_cant = 64;

        int res = usb_msc_leer_sectores(g_volumen.unidad_msc, lba, sec_cant, g_cluster_buf);
        if (res != 0) {
            consola_imprimir_linea_color("  [!] Error leyendo cluster de datos.", COLOR_ERROR_DEFAULT);
            break;
        }

        uint32_t bytes_a_imprimir = (bytes_restantes < g_volumen.bytes_por_cluster) ? bytes_restantes : g_volumen.bytes_por_cluster;
        for (uint32_t b = 0; b < bytes_a_imprimir && b < 32768; b++) {
            char ch = (char)g_cluster_buf[b];
            if (ch == '\r') continue;
            consola_escribir_caracter(ch);
        }

        bytes_restantes -= bytes_a_imprimir;
        clus = fat32_siguiente_cluster(&g_volumen, clus);
    }

    consola_imprimir_linea("");
    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);
    return 0;
}

// Convierte un nombre de archivo estándar a formato 8.3 en mayúsculas (11 bytes sin punto)
static void fat32_convertir_a_raw_83(const char *origen, char destino[11]) {
    for (int i = 0; i < 11; i++) destino[i] = ' ';
    if (!origen) return;

    int pos_punto = -1;
    for (int i = 0; origen[i]; i++) {
        if (origen[i] == '.') {
            pos_punto = i;
            break;
        }
    }

    // Parte del nombre (hasta 8 caracteres)
    int fin_nombre = (pos_punto >= 0) ? pos_punto : 8;
    for (int i = 0; i < fin_nombre && origen[i]; i++) {
        if (i >= 8) break;
        char c = origen[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        destino[i] = c;
    }

    // Extensión (hasta 3 caracteres)
    if (pos_punto >= 0) {
        int idx_ext = pos_punto + 1;
        for (int i = 0; i < 3 && origen[idx_ext + i]; i++) {
            char c = origen[idx_ext + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            destino[8 + i] = c;
        }
    }
}

// Asigna un cluster libre escaneando la FAT y marcándolo con EOC (0x0FFFFFFF)
static uint32_t fat32_asignar_cluster_libre(void) {
    if (!g_volumen.montado) return 0;

    for (uint32_t s = 0; s < g_volumen.sectores_por_fat && s < 256; s++) {
        uint32_t lba_fat_sec = g_volumen.lba_fat + s;
        if (usb_msc_leer_sectores(g_volumen.unidad_msc, lba_fat_sec, 1, g_fat_sector_buf) != 0) {
            return 0;
        }

        uint32_t *entradas = (uint32_t *)g_fat_sector_buf;
        for (int i = 0; i < 128; i++) {
            uint32_t cluster_idx = (s * 128) + (uint32_t)i;
            if (cluster_idx < 2) continue; // Reservados

            if ((entradas[i] & 0x0FFFFFFFU) == 0) {
                // ¡Cluster libre encontrado!
                entradas[i] = 0x0FFFFFFFU; // Marcar Fin de Cadena (EOC)

                // Escribir sector de vuelta en FAT1
                if (usb_msc_escribir_sectores(g_volumen.unidad_msc, lba_fat_sec, 1, g_fat_sector_buf) != 0) {
                    return 0;
                }
                // Si hay FAT2 de respaldo, sincronizarla también
                uint32_t lba_fat2_sec = lba_fat_sec + g_volumen.sectores_por_fat;
                usb_msc_escribir_sectores(g_volumen.unidad_msc, lba_fat2_sec, 1, g_fat_sector_buf);

                return cluster_idx;
            }
        }
    }

    return 0; // Disco lleno
}

// Inserta una entrada de directorio de 32 bytes en el cluster de directorio dado
static int fat32_insertar_entrada_directorio(uint32_t cluster_dir, const char nombre_83[11], uint8_t atributos, uint32_t cluster_inicio, uint32_t tamano) {
    uint32_t clus = cluster_dir;

    while (clus >= 2) {
        uint32_t lba_base = fat32_cluster_a_lba(&g_volumen, clus);

        for (uint8_t s = 0; s < g_volumen.sectores_por_cluster; s++) {
            uint32_t lba_sec = lba_base + s;
            if (usb_msc_leer_sectores(g_volumen.unidad_msc, lba_sec, 1, g_sector_buf) != 0) {
                return -1;
            }

            for (int e = 0; e < 16; e++) {
                uint32_t off = e * 32;
                uint8_t primer_byte = g_sector_buf[off];

                if (primer_byte == 0x00 || primer_byte == 0xE5) {
                    // Ranura libre encontrada
                    struct fat32_entrada_dir *entry = (struct fat32_entrada_dir *)&g_sector_buf[off];
                    for (int k = 0; k < 32; k++) ((uint8_t *)entry)[k] = 0;

                    for (int k = 0; k < 11; k++) entry->nombre[k] = nombre_83[k];
                    entry->atributos = atributos;
                    entry->cluster_alto = (uint16_t)((cluster_inicio >> 16) & 0xFFFF);
                    entry->cluster_bajo = (uint16_t)(cluster_inicio & 0xFFFF);
                    entry->tamano_archivo = tamano;

                    // Escribir sector modificado a disco
                    return usb_msc_escribir_sectores(g_volumen.unidad_msc, lba_sec, 1, g_sector_buf);
                }
            }
        }

        clus = fat32_siguiente_cluster(&g_volumen, clus);
    }

    return -2; // No hay ranuras libres
}

int fat32_crear_archivo(const char *nombre, const uint8_t *datos, uint32_t tamano) {
    if (!nombre || *nombre == '\0') return -1;
    if (!g_volumen.montado) {
        if (fat32_montar(0) != 0) return -2;
    }

    char nombre_83[11];
    fat32_convertir_a_raw_83(nombre, nombre_83);

    // 1. Asignar cluster de datos si tamano > 0
    uint32_t cluster_datos = 0;
    if (tamano > 0) {
        cluster_datos = fat32_asignar_cluster_libre();
        if (cluster_datos == 0) {
            consola_imprimir_linea_color("  [!] Error: No hay clusters libres en FAT32.", COLOR_ERROR_DEFAULT);
            return -3;
        }

        // Limpiar búfer y escribir datos al LBA del cluster
        for (uint32_t i = 0; i < sizeof(g_cluster_buf); i++) g_cluster_buf[i] = 0;
        uint32_t a_copiar = (tamano < g_volumen.bytes_por_cluster) ? tamano : g_volumen.bytes_por_cluster;
        if (datos) {
            for (uint32_t i = 0; i < a_copiar; i++) g_cluster_buf[i] = datos[i];
        }

        uint32_t lba = fat32_cluster_a_lba(&g_volumen, cluster_datos);
        if (usb_msc_escribir_sectores(g_volumen.unidad_msc, lba, g_volumen.sectores_por_cluster, g_cluster_buf) != 0) {
            return -4;
        }
    }

    // 2. Insertar entrada en el directorio raíz
    int res = fat32_insertar_entrada_directorio(g_volumen.cluster_raiz, nombre_83, FAT32_ATTR_ARCHIVE, cluster_datos, tamano);
    if (res != 0) return -5;

    consola_imprimir("==> [FAT32] Archivo creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Cluster: ");
    consola_imprimir_dec(cluster_datos);
    consola_imprimir_linea(")");
    return 0;
}

int fat32_crear_directorio(const char *nombre) {
    if (!nombre || *nombre == '\0') return -1;
    if (!g_volumen.montado) {
        if (fat32_montar(0) != 0) return -2;
    }

    char nombre_83[11];
    fat32_convertir_a_raw_83(nombre, nombre_83);

    // 1. Asignar cluster para el nuevo directorio
    uint32_t cluster_dir = fat32_asignar_cluster_libre();
    if (cluster_dir == 0) return -3;

    // 2. Limpiar cluster y configurar '.' y '..'
    for (uint32_t i = 0; i < sizeof(g_cluster_buf); i++) g_cluster_buf[i] = 0;

    // '.'
    struct fat32_entrada_dir *e_punto = (struct fat32_entrada_dir *)&g_cluster_buf[0];
    for (int i = 0; i < 11; i++) e_punto->nombre[i] = ' ';
    e_punto->nombre[0] = '.';
    e_punto->atributos = FAT32_ATTR_DIRECTORY;
    e_punto->cluster_alto = (uint16_t)((cluster_dir >> 16) & 0xFFFF);
    e_punto->cluster_bajo = (uint16_t)(cluster_dir & 0xFFFF);

    // '..'
    struct fat32_entrada_dir *e_dospuntos = (struct fat32_entrada_dir *)&g_cluster_buf[32];
    for (int i = 0; i < 11; i++) e_dospuntos->nombre[i] = ' ';
    e_dospuntos->nombre[0] = '.';
    e_dospuntos->nombre[1] = '.';
    e_dospuntos->atributos = FAT32_ATTR_DIRECTORY;
    // Apunta al directorio raíz
    e_dospuntos->cluster_alto = (uint16_t)((g_volumen.cluster_raiz >> 16) & 0xFFFF);
    e_dospuntos->cluster_bajo = (uint16_t)(g_volumen.cluster_raiz & 0xFFFF);

    // Escribir cluster de directorio en disco
    uint32_t lba = fat32_cluster_a_lba(&g_volumen, cluster_dir);
    if (usb_msc_escribir_sectores(g_volumen.unidad_msc, lba, g_volumen.sectores_por_cluster, g_cluster_buf) != 0) {
        return -4;
    }

    // 3. Insertar entrada en el directorio raíz
    int res = fat32_insertar_entrada_directorio(g_volumen.cluster_raiz, nombre_83, FAT32_ATTR_DIRECTORY, cluster_dir, 0);
    if (res != 0) return -5;

    consola_imprimir("==> [FAT32] Directorio creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Cluster: ");
    consola_imprimir_dec(cluster_dir);
    consola_imprimir_linea(")");
    return 0;
}

