#include "ext4.h"
#include "usb_msc.h"
#include "consola.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/memoria.h"

// ============================================================================
// TAEK OS - CONTROLADOR DE SISTEMA DE ARCHIVOS EXT4 (Hito 51)
// Arquitectura Anillo 0 en Español (Lectura y Escritura)
// ============================================================================

static struct ext4_volumen g_volumen = {0};
static int g_ext4_inicializado = 0;

// Búferes estáticos en BSS para evitar desbordar la pila del kernel
static uint8_t g_bloque_buf[4096];
static uint8_t g_bloque_aux[4096];
static uint8_t g_dir_buf[4096];

#define EXT4_MAX_ITEMS_DIR 128
struct ext4_item {
    char     nombre[256];
    uint8_t  es_directorio;
    uint32_t inodo;
    uint32_t tamano;
};

static struct ext4_item g_items_actuales[EXT4_MAX_ITEMS_DIR];

// Métricas para el comando tree
static uint32_t g_arbol_directorios = 0;
static uint32_t g_arbol_archivos = 0;
static uint64_t g_arbol_bytes_totales = 0;

void ext4_iniciar(void) {
    if (g_ext4_inicializado) return;
    g_volumen.montado = 0;
    g_ext4_inicializado = 1;
    serial_imprimir_linea("[EXT4] Subsistema de archivos Linux ext4 inicializado en Ring 0.");
}

int ext4_esta_montado(void) {
    return g_volumen.montado;
}

const struct ext4_volumen *ext4_obtener_volumen(void) {
    if (!g_volumen.montado) return NULL;
    return &g_volumen;
}

void ext4_desmontar(void) {
    if (g_volumen.montado) {
        serial_imprimir("[EXT4] Volumen '");
        serial_imprimir(g_volumen.etiqueta);
        serial_imprimir_linea("' desmontado.");
    }
    g_volumen.montado = 0;
}

// Lee un bloque lógico ext4 del volumen activo
static int ext4_leer_bloque(uint32_t bloque, void *destino) {
    if (!g_volumen.montado) return -1;
    uint32_t lba = g_volumen.lba_inicio_particion + (bloque * g_volumen.sectores_por_bloque);
    return usb_msc_leer_sectores(g_volumen.unidad_msc, lba, (uint16_t)g_volumen.sectores_por_bloque, destino);
}

// Escribe un bloque lógico ext4 en el volumen activo
static int ext4_escribir_bloque(uint32_t bloque, const void *origen) {
    if (!g_volumen.montado) return -1;
    uint32_t lba = g_volumen.lba_inicio_particion + (bloque * g_volumen.sectores_por_bloque);
    return usb_msc_escribir_sectores(g_volumen.unidad_msc, lba, (uint16_t)g_volumen.sectores_por_bloque, origen);
}

// Lee el descriptor del grupo de bloques especificado
static int ext4_leer_descriptor_grupo(uint32_t grupo, struct ext4_grupo_descriptor *gd) {
    if (!g_volumen.montado || !gd) return -1;
    if (grupo >= g_volumen.total_grupos) return -2;

    uint32_t offset_bytes = grupo * g_volumen.tamano_descriptor_grupo;
    uint32_t bloque_gdt = (offset_bytes / g_volumen.tamano_bloque);
    uint32_t offset_en_bloque = offset_bytes % g_volumen.tamano_bloque;

    uint32_t lba_gdt = g_volumen.lba_bloque_descriptores + (bloque_gdt * g_volumen.sectores_por_bloque);
    int res = usb_msc_leer_sectores(g_volumen.unidad_msc, lba_gdt, (uint16_t)g_volumen.sectores_por_bloque, g_bloque_aux);
    if (res != 0) return -3;

    const uint8_t *src = &g_bloque_aux[offset_en_bloque];
    uint8_t *dst = (uint8_t *)gd;
    uint32_t tam = g_volumen.tamano_descriptor_grupo;
    if (tam > sizeof(struct ext4_grupo_descriptor)) tam = sizeof(struct ext4_grupo_descriptor);
    for (uint32_t i = 0; i < tam; i++) dst[i] = src[i];

    return 0;
}

// Escribe el descriptor de grupo actualizado a disco
static int ext4_escribir_descriptor_grupo(uint32_t grupo, const struct ext4_grupo_descriptor *gd) {
    if (!g_volumen.montado || !gd) return -1;
    if (grupo >= g_volumen.total_grupos) return -2;

    uint32_t offset_bytes = grupo * g_volumen.tamano_descriptor_grupo;
    uint32_t bloque_gdt = (offset_bytes / g_volumen.tamano_bloque);
    uint32_t offset_en_bloque = offset_bytes % g_volumen.tamano_bloque;

    uint32_t lba_gdt = g_volumen.lba_bloque_descriptores + (bloque_gdt * g_volumen.sectores_por_bloque);
    int res = usb_msc_leer_sectores(g_volumen.unidad_msc, lba_gdt, (uint16_t)g_volumen.sectores_por_bloque, g_bloque_aux);
    if (res != 0) return -3;

    uint8_t *dst = &g_bloque_aux[offset_en_bloque];
    const uint8_t *src = (const uint8_t *)gd;
    uint32_t tam = g_volumen.tamano_descriptor_grupo;
    if (tam > sizeof(struct ext4_grupo_descriptor)) tam = sizeof(struct ext4_grupo_descriptor);
    for (uint32_t i = 0; i < tam; i++) dst[i] = src[i];

    return usb_msc_escribir_sectores(g_volumen.unidad_msc, lba_gdt, (uint16_t)g_volumen.sectores_por_bloque, g_bloque_aux);
}

// Lee un inodo por su número (1-indexado)
static int ext4_leer_inodo(uint32_t inodo_num, struct ext4_inodo *inodo_out) {
    if (!g_volumen.montado || inodo_num == 0 || !inodo_out) return -1;

    uint32_t grupo = (inodo_num - 1) / g_volumen.inodos_por_grupo;
    uint32_t indice = (inodo_num - 1) % g_volumen.inodos_por_grupo;

    struct ext4_grupo_descriptor gd;
    if (ext4_leer_descriptor_grupo(grupo, &gd) != 0) return -2;

    uint64_t tabla_inodos_bloque = (uint64_t)gd.bg_inode_table_lo;
    if (g_volumen.tamano_descriptor_grupo >= 64) {
        tabla_inodos_bloque |= ((uint64_t)gd.bg_inode_table_hi << 32);
    }

    uint32_t offset_bytes = indice * g_volumen.tamano_inodo;
    uint32_t bloque_inodo = (uint32_t)tabla_inodos_bloque + (offset_bytes / g_volumen.tamano_bloque);
    uint32_t offset_en_bloque = offset_bytes % g_volumen.tamano_bloque;

    if (ext4_leer_bloque(bloque_inodo, g_bloque_buf) != 0) return -3;

    const uint8_t *src = &g_bloque_buf[offset_en_bloque];
    uint8_t *dst = (uint8_t *)inodo_out;
    uint32_t tam = sizeof(struct ext4_inodo);
    if (tam > g_volumen.tamano_inodo) tam = g_volumen.tamano_inodo;
    for (uint32_t i = 0; i < tam; i++) dst[i] = src[i];

    return 0;
}

// Escribe un inodo actualizado a disco
static int ext4_escribir_inodo(uint32_t inodo_num, const struct ext4_inodo *inodo_in) {
    if (!g_volumen.montado || inodo_num == 0 || !inodo_in) return -1;

    uint32_t grupo = (inodo_num - 1) / g_volumen.inodos_por_grupo;
    uint32_t indice = (inodo_num - 1) % g_volumen.inodos_por_grupo;

    struct ext4_grupo_descriptor gd;
    if (ext4_leer_descriptor_grupo(grupo, &gd) != 0) return -2;

    uint64_t tabla_inodos_bloque = (uint64_t)gd.bg_inode_table_lo;
    if (g_volumen.tamano_descriptor_grupo >= 64) {
        tabla_inodos_bloque |= ((uint64_t)gd.bg_inode_table_hi << 32);
    }

    uint32_t offset_bytes = indice * g_volumen.tamano_inodo;
    uint32_t bloque_inodo = (uint32_t)tabla_inodos_bloque + (offset_bytes / g_volumen.tamano_bloque);
    uint32_t offset_en_bloque = offset_bytes % g_volumen.tamano_bloque;

    if (ext4_leer_bloque(bloque_inodo, g_bloque_buf) != 0) return -3;

    uint8_t *dst = &g_bloque_buf[offset_en_bloque];
    const uint8_t *src = (const uint8_t *)inodo_in;
    uint32_t tam = sizeof(struct ext4_inodo);
    if (tam > g_volumen.tamano_inodo) tam = g_volumen.tamano_inodo;
    for (uint32_t i = 0; i < tam; i++) dst[i] = src[i];

    return ext4_escribir_bloque(bloque_inodo, g_bloque_buf);
}

// Mapea un bloque lógico de un inodo a su bloque físico real en disco usando extents
static int ext4_mapear_bloque_logico(const struct ext4_inodo *inodo, uint32_t bloque_logico, uint32_t *bloque_fisico_out) {
    if (!inodo || !bloque_fisico_out) return -1;

    // Verificar si el inodo usa extents
    if (inodo->i_flags & EXT4_EXTENTS_FL) {
        const struct ext4_extent_cabecera *eh = (const struct ext4_extent_cabecera *)&inodo->i_block[0];
        if (eh->eh_magic != EXT4_EXTENTS_MAGIC) {
            return -2;
        }

        if (eh->eh_depth == 0) {
            // Hojas directas en i_block
            const struct ext4_extent *ex = (const struct ext4_extent *)(&inodo->i_block[3]);
            for (uint16_t i = 0; i < eh->eh_entries; i++) {
                uint32_t inicio = ex[i].ee_block;
                uint32_t fin = inicio + (uint32_t)ex[i].ee_len;
                if (bloque_logico >= inicio && bloque_logico < fin) {
                    uint32_t delta = bloque_logico - inicio;
                    *bloque_fisico_out = ex[i].ee_start_lo + delta;
                    return 0;
                }
            }
            return -3; // No mapeado (agujero o fuera de rango)
        } else {
            // Nivel 1 de árbol de extents (índices apuntan a bloques con hojas)
            const uint32_t *idx_ptr = &inodo->i_block[3];
            for (uint16_t i = 0; i < eh->eh_entries; i++) {
                uint32_t ei_block = idx_ptr[i * 3 + 0];
                uint32_t ei_leaf_lo = idx_ptr[i * 3 + 1];
                // Leer el bloque de hojas
                if (ext4_leer_bloque(ei_leaf_lo, g_bloque_aux) == 0) {
                    const struct ext4_extent_cabecera *leh = (const struct ext4_extent_cabecera *)g_bloque_aux;
                    if (leh->eh_magic == EXT4_EXTENTS_MAGIC && leh->eh_depth == 0) {
                        const struct ext4_extent *lex = (const struct ext4_extent *)&g_bloque_aux[sizeof(struct ext4_extent_cabecera)];
                        for (uint16_t j = 0; j < leh->eh_entries; j++) {
                            uint32_t inicio = lex[j].ee_block;
                            uint32_t fin = inicio + (uint32_t)lex[j].ee_len;
                            if (bloque_logico >= inicio && bloque_logico < fin) {
                                uint32_t delta = bloque_logico - inicio;
                                *bloque_fisico_out = lex[j].ee_start_lo + delta;
                                return 0;
                            }
                        }
                    }
                }
                (void)ei_block;
            }
            return -4;
        }
    } else {
        // Bloques directos clásicos estilo ext2/ext3 (primeros 12 bloques directos)
        if (bloque_logico < 12) {
            *bloque_fisico_out = inodo->i_block[bloque_logico];
            return (*bloque_fisico_out != 0) ? 0 : -5;
        }
        return -6;
    }
}

// Monta el sistema de archivos ext4 escaneando MBR o Superfloppy
int ext4_montar(uint8_t unidad_msc) {
    if (!g_ext4_inicializado) ext4_iniciar();

    if (unidad_msc >= USB_MSC_MAX_DISPOSITIVOS) return -1;
    const struct usb_msc_dispositivo *dev = usb_msc_obtener_dispositivo(unidad_msc);
    if (!dev || !dev->activo || !dev->listo) return -2;

    if (g_volumen.montado && g_volumen.unidad_msc == unidad_msc) return 0;

    // 1. Leer LBA 0 para detectar MBR
    uint8_t mbr[512];
    int res = usb_msc_leer_sectores(unidad_msc, 0, 1, mbr);
    if (res != 0) return -3;

    uint32_t lba_particion = 0;
    int particion_encontrada = 0;

    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        for (int p = 0; p < 4; p++) {
            uint32_t p_off = 446 + (p * 16);
            uint8_t tipo = mbr[p_off + 4];
            uint32_t inicio_lba = (uint32_t)mbr[p_off + 8] |
                                 ((uint32_t)mbr[p_off + 9] << 8) |
                                 ((uint32_t)mbr[p_off + 10] << 16) |
                                 ((uint32_t)mbr[p_off + 11] << 24);
            uint32_t cant_sec = (uint32_t)mbr[p_off + 12] |
                                ((uint32_t)mbr[p_off + 13] << 8) |
                                ((uint32_t)mbr[p_off + 14] << 16) |
                                ((uint32_t)mbr[p_off + 15] << 24);

            if (cant_sec > 0 && inicio_lba > 0) {
                if (tipo == 0x83 || tipo == 0xEE) {
                    lba_particion = inicio_lba;
                    particion_encontrada = 1;
                    break;
                }
            }
        }
    }

    // Probar el Superbloque ext4 en offset 1024 bytes (LBA particion + 2)
    uint32_t lba_sb = (particion_encontrada ? lba_particion : 0) + 2;
    res = usb_msc_leer_sectores(unidad_msc, lba_sb, 2, g_bloque_buf);
    if (res != 0) return -4;

    const struct ext4_superbloque *sb = (const struct ext4_superbloque *)g_bloque_buf;
    if (sb->s_magic != EXT4_SUPER_MAGIC) {
        // Si no se encontró en la partición MBR, probar directamente en LBA 2 (Superfloppy)
        if (particion_encontrada) {
            lba_sb = 2;
            res = usb_msc_leer_sectores(unidad_msc, lba_sb, 2, g_bloque_buf);
            if (res != 0 || sb->s_magic != EXT4_SUPER_MAGIC) {
                return -5; // No es ext4
            }
            lba_particion = 0;
        } else {
            return -5;
        }
    }

    // Configurar descriptor de volumen
    g_volumen.montado = 1;
    g_volumen.unidad_msc = unidad_msc;
    g_volumen.lba_inicio_particion = lba_particion;
    g_volumen.tamano_bloque = 1024U << sb->s_log_block_size;
    if (g_volumen.tamano_bloque < 1024 || g_volumen.tamano_bloque > 4096) {
        g_volumen.tamano_bloque = 4096;
    }
    g_volumen.sectores_por_bloque = g_volumen.tamano_bloque / 512;
    g_volumen.bloques_por_grupo = sb->s_blocks_per_group;
    g_volumen.inodos_por_grupo = sb->s_inodes_per_group;
    g_volumen.tamano_inodo = sb->s_inode_size ? sb->s_inode_size : 256;

    if ((sb->s_feature_incompat & 0x80) && sb->s_desc_size >= 64) {
        g_volumen.tamano_descriptor_grupo = sb->s_desc_size;
    } else {
        g_volumen.tamano_descriptor_grupo = 32;
    }

    g_volumen.total_grupos = (sb->s_blocks_count_lo + sb->s_blocks_per_group - 1) / sb->s_blocks_per_group;

    // Cálculo del LBA de inicio de la tabla de descriptores de grupo (GDT):
    // Si tamano_bloque == 1024, bloque 0 = boot, bloque 1 = superblock, bloque 2 = GDT.
    // Si tamano_bloque > 1024, bloque 0 = boot + superblock, bloque 1 = GDT.
    uint32_t bloque_inicio_gdt = (g_volumen.tamano_bloque == 1024) ? 2 : 1;
    g_volumen.lba_bloque_descriptores = lba_particion + (bloque_inicio_gdt * g_volumen.sectores_por_bloque);

    for (int i = 0; i < 16; i++) {
        char c = sb->s_volume_name[i];
        g_volumen.etiqueta[i] = (c >= 32 && c <= 126) ? c : ' ';
    }
    g_volumen.etiqueta[16] = '\0';

    serial_imprimir("  [EXT4] Volumen '");
    serial_imprimir(g_volumen.etiqueta);
    serial_imprimir("' montado [OK]. Bloque: ");
    serial_imprimir_dec(g_volumen.tamano_bloque);
    serial_imprimir(" bytes | Grupos: ");
    serial_imprimir_dec(g_volumen.total_grupos);
    serial_imprimir(" | Inodos/Grupo: ");
    serial_imprimir_dec(g_volumen.inodos_por_grupo);
    serial_imprimir_linea("");

    return 0;
}

// Lee las entradas de un directorio (dado su número de inodo)
static int ext4_leer_entradas_directorio(uint32_t inodo_dir, int *num_items_out) {
    if (!num_items_out) return -1;
    *num_items_out = 0;

    struct ext4_inodo inodo;
    if (ext4_leer_inodo(inodo_dir, &inodo) != 0) return -2;

    if ((inodo.i_mode & 0xF000) != EXT4_S_IFDIR) return -3;

    uint32_t dir_size = inodo.i_size_lo;
    uint32_t cant_bloques = (dir_size + g_volumen.tamano_bloque - 1) / g_volumen.tamano_bloque;
    if (cant_bloques == 0) cant_bloques = 1;

    int total_items = 0;

    for (uint32_t b = 0; b < cant_bloques && total_items < EXT4_MAX_ITEMS_DIR; b++) {
        uint32_t bloque_fisico = 0;
        if (ext4_mapear_bloque_logico(&inodo, b, &bloque_fisico) != 0) continue;

        if (ext4_leer_bloque(bloque_fisico, g_dir_buf) != 0) continue;

        uint32_t offset = 0;
        while (offset + sizeof(struct ext4_dir_entry_cabecera) <= g_volumen.tamano_bloque && total_items < EXT4_MAX_ITEMS_DIR) {
            const struct ext4_dir_entry_cabecera *deh = (const struct ext4_dir_entry_cabecera *)&g_dir_buf[offset];
            if (deh->rec_len == 0) break;

            if (deh->inodo != 0 && deh->name_len > 0) {
                const char *nombre_raw = (const char *)&g_dir_buf[offset + sizeof(struct ext4_dir_entry_cabecera)];

                // Filtrar '.' y '..'
                int es_punto = (deh->name_len == 1 && nombre_raw[0] == '.');
                int es_dos_puntos = (deh->name_len == 2 && nombre_raw[0] == '.' && nombre_raw[1] == '.');

                if (!es_punto && !es_dos_puntos) {
                    struct ext4_item *it = &g_items_actuales[total_items];
                    it->inodo = deh->inodo;
                    it->es_directorio = (deh->file_type == EXT4_FT_DIR);

                    int len = deh->name_len;
                    if (len > 255) len = 255;
                    for (int k = 0; k < len; k++) it->nombre[k] = nombre_raw[k];
                    it->nombre[len] = '\0';

                    // Leer tamaño del inodo hijo
                    struct ext4_inodo hijo_ino;
                    if (ext4_leer_inodo(deh->inodo, &hijo_ino) == 0) {
                        it->tamano = hijo_ino.i_size_lo;
                        if ((hijo_ino.i_mode & 0xF000) == EXT4_S_IFDIR) {
                            it->es_directorio = 1;
                        }
                    } else {
                        it->tamano = 0;
                    }

                    total_items++;
                }
            }

            offset += deh->rec_len;
        }
    }

    *num_items_out = total_items;
    return 0;
}

// Búsqueda de inodo por ruta (ej. "/leeme.txt" o "leeme.txt")
static uint32_t ext4_buscar_inodo_por_ruta(const char *ruta) {
    if (!ruta || !g_volumen.montado) return 0;
    while (*ruta == '/' || *ruta == '\\') ruta++;
    if (*ruta == '\0') return EXT4_ROOT_INO;

    uint32_t inodo_actual = EXT4_ROOT_INO;
    char componente[128];

    while (*ruta != '\0') {
        int idx = 0;
        while (*ruta != '\0' && *ruta != '/' && *ruta != '\\' && idx < 127) {
            componente[idx++] = *ruta++;
        }
        componente[idx] = '\0';
        while (*ruta == '/' || *ruta == '\\') ruta++;

        int num_items = 0;
        if (ext4_leer_entradas_directorio(inodo_actual, &num_items) != 0) return 0;

        uint32_t siguiente_inodo = 0;
        for (int i = 0; i < num_items; i++) {
            // Comparación simple sensible / insensible a mayúsculas
            int match = 1;
            int p = 0;
            while (componente[p] && g_items_actuales[i].nombre[p]) {
                char c1 = componente[p];
                char c2 = g_items_actuales[i].nombre[p];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) { match = 0; break; }
                p++;
            }
            if (match && componente[p] == '\0' && g_items_actuales[i].nombre[p] == '\0') {
                siguiente_inodo = g_items_actuales[i].inodo;
                break;
            }
        }

        if (siguiente_inodo == 0) return 0;
        inodo_actual = siguiente_inodo;
    }

    return inodo_actual;
}

static void ext4_imprimir_tamano(uint32_t bytes) {
    if (bytes < 1024) {
        consola_imprimir_dec(bytes);
        consola_imprimir(" B");
    } else if (bytes < 1024 * 1024) {
        consola_imprimir_dec(bytes / 1024);
        consola_imprimir(" KiB");
    } else {
        consola_imprimir_dec(bytes / (1024 * 1024));
        consola_imprimir(" MiB");
    }
}

// Recorrido recursivo DFS para el comando tree
static void ext4_tree_recursivo(uint32_t inodo_dir, const char *prefijo, int nivel) {
    if (nivel > 10) return;

    struct ext4_item items_locales[32];
    int num_items = 0;

    ext4_leer_entradas_directorio(inodo_dir, &num_items);
    int items_a_procesar = (num_items > 32) ? 32 : num_items;

    for (int i = 0; i < items_a_procesar; i++) {
        items_locales[i] = g_items_actuales[i];
    }

    for (int i = 0; i < items_a_procesar; i++) {
        int es_ultimo = (i == (items_a_procesar - 1));
        const struct ext4_item *it = &items_locales[i];

        consola_imprimir_color(prefijo, COLOR_PROMPT_DEFAULT);
        if (es_ultimo) {
            consola_imprimir_color("\\-- ", COLOR_PROMPT_DEFAULT);
        } else {
            consola_imprimir_color("+-- ", COLOR_PROMPT_DEFAULT);
        }

        if (it->es_directorio) {
            g_arbol_directorios++;
            consola_imprimir_linea_color(it->nombre, COLOR_USUARIO_DEFAULT);

            char nuevo_prefijo[128];
            int p = 0;
            while (prefijo[p] && p < 100) {
                nuevo_prefijo[p] = prefijo[p];
                p++;
            }
            if (es_ultimo) {
                nuevo_prefijo[p++] = ' '; nuevo_prefijo[p++] = ' '; nuevo_prefijo[p++] = ' '; nuevo_prefijo[p++] = ' ';
            } else {
                nuevo_prefijo[p++] = '|'; nuevo_prefijo[p++] = ' '; nuevo_prefijo[p++] = ' '; nuevo_prefijo[p++] = ' ';
            }
            nuevo_prefijo[p] = '\0';

            ext4_tree_recursivo(it->inodo, nuevo_prefijo, nivel + 1);
        } else {
            g_arbol_archivos++;
            g_arbol_bytes_totales += it->tamano;

            consola_imprimir(it->nombre);
            consola_imprimir(" (");
            ext4_imprimir_tamano(it->tamano);
            consola_imprimir_linea(")");
        }
    }
}

int ext4_ejecutar_tree(const char *ruta_inicial) {
    if (!g_volumen.montado) {
        if (ext4_montar(0) != 0) {
            consola_imprimir_linea_color("  [!] Error: No hay partición ext4 montada.", COLOR_ERROR_DEFAULT);
            return -1;
        }
    }

    uint32_t inodo_raiz = EXT4_ROOT_INO;
    if (ruta_inicial && *ruta_inicial != '\0') {
        inodo_raiz = ext4_buscar_inodo_por_ruta(ruta_inicial);
        if (inodo_raiz == 0) {
            consola_imprimir_linea_color("  [!] Ruta no encontrada en volumen ext4.", COLOR_ERROR_DEFAULT);
            return -2;
        }
    }

    g_arbol_directorios = 0;
    g_arbol_archivos = 0;
    g_arbol_bytes_totales = 0;

    consola_imprimir_color(".", COLOR_USUARIO_DEFAULT);
    consola_imprimir(" [ext4: ");
    consola_imprimir(g_volumen.etiqueta);
    consola_imprimir_linea("]");

    ext4_tree_recursivo(inodo_raiz, "", 1);

    consola_imprimir_linea("");
    consola_imprimir_dec(g_arbol_directorios);
    consola_imprimir(" directorios, ");
    consola_imprimir_dec(g_arbol_archivos);
    consola_imprimir(" archivos (Total: ");
    ext4_imprimir_tamano((uint32_t)g_arbol_bytes_totales);
    consola_imprimir_linea(")");

    return 0;
}

int ext4_listar_directorio(const char *ruta) {
    if (!g_volumen.montado) {
        if (ext4_montar(0) != 0) {
            consola_imprimir_linea_color("  [!] Error: No hay partición ext4 montada.", COLOR_ERROR_DEFAULT);
            return -1;
        }
    }

    uint32_t inodo_dir = EXT4_ROOT_INO;
    if (ruta && *ruta != '\0') {
        inodo_dir = ext4_buscar_inodo_por_ruta(ruta);
        if (inodo_dir == 0) {
            consola_imprimir_linea_color("  [!] Directorio no encontrado.", COLOR_ERROR_DEFAULT);
            return -2;
        }
    }

    int num_items = 0;
    ext4_leer_entradas_directorio(inodo_dir, &num_items);

    consola_imprimir_linea_color("==================== LISTADO DE DIRECTORIO EXT4 ====================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("TIPO    TAMAÑO        INODO    NOMBRE", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("----------------------------------------------------------------------");

    for (int i = 0; i < num_items; i++) {
        const struct ext4_item *it = &g_items_actuales[i];
        if (it->es_directorio) {
            consola_imprimir_color("<DIR>   ", COLOR_USUARIO_DEFAULT);
            consola_imprimir("     ---      ");
        } else {
            consola_imprimir("FILE    ");
            ext4_imprimir_tamano(it->tamano);
            consola_imprimir("      ");
        }
        consola_imprimir_dec(it->inodo);
        consola_imprimir("   ");
        if (it->es_directorio) {
            consola_imprimir_color(it->nombre, COLOR_USUARIO_DEFAULT);
            consola_imprimir_linea("/");
        } else {
            consola_imprimir_linea(it->nombre);
        }
    }

    consola_imprimir_linea_color("======================================================================", COLOR_AVISO_DEFAULT);
    return 0;
}

int ext4_leer_archivo_texto(const char *ruta) {
    if (!ruta || *ruta == '\0') {
        consola_imprimir_linea_color("Uso: cat <archivo> o leer <archivo>", COLOR_ERROR_DEFAULT);
        return -1;
    }

    if (!g_volumen.montado) {
        if (ext4_montar(0) != 0) {
            consola_imprimir_linea_color("  [!] Error: No hay partición ext4 montada.", COLOR_ERROR_DEFAULT);
            return -2;
        }
    }

    uint32_t inodo_num = ext4_buscar_inodo_por_ruta(ruta);
    if (inodo_num == 0) {
        consola_imprimir("  [!] Archivo no encontrado: '");
        consola_imprimir(ruta);
        consola_imprimir_linea("'");
        return -3;
    }

    struct ext4_inodo inodo;
    if (ext4_leer_inodo(inodo_num, &inodo) != 0) {
        consola_imprimir_linea_color("  [!] Error al leer inodo de archivo.", COLOR_ERROR_DEFAULT);
        return -4;
    }

    if ((inodo.i_mode & 0xF000) == EXT4_S_IFDIR) {
        consola_imprimir_linea_color("  [!] El elemento especificado es un directorio.", COLOR_ERROR_DEFAULT);
        return -5;
    }

    consola_imprimir("==> Mostrando contenido de '");
    consola_imprimir(ruta);
    consola_imprimir("' (");
    ext4_imprimir_tamano(inodo.i_size_lo);
    consola_imprimir_linea("):");
    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);

    uint32_t bytes_restantes = inodo.i_size_lo;
    uint32_t cant_bloques = (bytes_restantes + g_volumen.tamano_bloque - 1) / g_volumen.tamano_bloque;

    for (uint32_t b = 0; b < cant_bloques && bytes_restantes > 0; b++) {
        uint32_t bloque_fisico = 0;
        if (ext4_mapear_bloque_logico(&inodo, b, &bloque_fisico) != 0) break;

        if (ext4_leer_bloque(bloque_fisico, g_bloque_buf) != 0) {
            consola_imprimir_linea_color("  [!] Error leyendo bloque de datos.", COLOR_ERROR_DEFAULT);
            break;
        }

        uint32_t bytes_a_imprimir = (bytes_restantes < g_volumen.tamano_bloque) ? bytes_restantes : g_volumen.tamano_bloque;
        for (uint32_t i = 0; i < bytes_a_imprimir; i++) {
            char ch = (char)g_bloque_buf[i];
            if (ch == '\r') continue;
            consola_escribir_caracter(ch);
        }
        bytes_restantes -= bytes_a_imprimir;
    }

    consola_imprimir_linea("");
    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);
    return 0;
}

// Asignador atómico de inodo libre en Grupo 0
static uint32_t ext4_asignar_inodo_libre(void) {
    struct ext4_grupo_descriptor gd;
    if (ext4_leer_descriptor_grupo(0, &gd) != 0) return 0;

    uint32_t bloque_bitmap = gd.bg_inode_bitmap_lo;
    if (ext4_leer_bloque(bloque_bitmap, g_bloque_buf) != 0) return 0;

    // Inodos 1..10 están reservados en ext4. Buscar a partir del bit 11 (índice 10)
    uint32_t inodo_asignado = 0;
    for (uint32_t bit = 11; bit < g_volumen.inodos_por_grupo; bit++) {
        uint32_t byte_idx = bit / 8;
        uint8_t mascara = 1U << (bit % 8);
        if (!(g_bloque_buf[byte_idx] & mascara)) {
            // Bit libre encontrado
            g_bloque_buf[byte_idx] |= mascara;
            inodo_asignado = bit + 1; // 1-indexado
            break;
        }
    }

    if (inodo_asignado == 0) return 0;

    // Guardar bitmap actualizado
    if (ext4_escribir_bloque(bloque_bitmap, g_bloque_buf) != 0) return 0;

    // Actualizar descriptores
    if (gd.bg_free_inodes_count_lo > 0) gd.bg_free_inodes_count_lo--;
    ext4_escribir_descriptor_grupo(0, &gd);

    return inodo_asignado;
}

// Asignador atómico de bloque libre en Grupo 0
static uint32_t ext4_asignar_bloque_libre(void) {
    struct ext4_grupo_descriptor gd;
    if (ext4_leer_descriptor_grupo(0, &gd) != 0) return 0;

    uint32_t bloque_bitmap = gd.bg_block_bitmap_lo;
    if (ext4_leer_bloque(bloque_bitmap, g_bloque_buf) != 0) return 0;

    uint32_t bloque_asignado = 0;
    for (uint32_t bit = 0; bit < g_volumen.bloques_por_grupo; bit++) {
        uint32_t byte_idx = bit / 8;
        uint8_t mascara = 1U << (bit % 8);
        if (!(g_bloque_buf[byte_idx] & mascara)) {
            g_bloque_buf[byte_idx] |= mascara;
            bloque_asignado = bit;
            break;
        }
    }

    if (bloque_asignado == 0) return 0;

    if (ext4_escribir_bloque(bloque_bitmap, g_bloque_buf) != 0) return 0;

    if (gd.bg_free_blocks_count_lo > 0) gd.bg_free_blocks_count_lo--;
    ext4_escribir_descriptor_grupo(0, &gd);

    return bloque_asignado;
}

// Inserta una entrada de directorio en el bloque del directorio raíz
static int ext4_insertar_entrada_directorio(uint32_t inodo_dir, uint32_t inodo_nuevo, const char *nombre, uint8_t file_type) {
    struct ext4_inodo dir_ino;
    if (ext4_leer_inodo(inodo_dir, &dir_ino) != 0) return -1;

    uint32_t bloque_fisico = 0;
    if (ext4_mapear_bloque_logico(&dir_ino, 0, &bloque_fisico) != 0) return -2;

    if (ext4_leer_bloque(bloque_fisico, g_dir_buf) != 0) return -3;

    // Calcular longitud requerida para la nueva entrada (alineada a 4 bytes)
    int name_len = 0;
    while (nombre[name_len]) name_len++;
    uint16_t nueva_len = (8 + name_len + 3) & ~3;

    // Recorrer hasta la última entrada
    uint32_t offset = 0;
    struct ext4_dir_entry_cabecera *deh_prev = NULL;

    while (offset < g_volumen.tamano_bloque) {
        struct ext4_dir_entry_cabecera *deh = (struct ext4_dir_entry_cabecera *)&g_dir_buf[offset];
        if (deh->rec_len == 0) break;

        uint16_t min_len_actual = (8 + deh->name_len + 3) & ~3;
        if (deh->rec_len > min_len_actual + nueva_len) {
            // ¡Espacio disponible en esta entrada para dividirla!
            uint16_t rec_len_original = deh->rec_len;
            deh->rec_len = min_len_actual;

            uint32_t offset_nueva = offset + min_len_actual;
            struct ext4_dir_entry_cabecera *deh_nuevo = (struct ext4_dir_entry_cabecera *)&g_dir_buf[offset_nueva];
            deh_nuevo->inodo = inodo_nuevo;
            deh_nuevo->rec_len = rec_len_original - min_len_actual;
            deh_nuevo->name_len = (uint8_t)name_len;
            deh_nuevo->file_type = file_type;

            char *nom_dst = (char *)&g_dir_buf[offset_nueva + sizeof(struct ext4_dir_entry_cabecera)];
            for (int k = 0; k < name_len; k++) nom_dst[k] = nombre[k];

            // Escribir bloque de directorio actualizado
            return ext4_escribir_bloque(bloque_fisico, g_dir_buf);
        }

        deh_prev = deh;
        offset += deh->rec_len;
    }

    (void)deh_prev;
    return -4; // No hay espacio en este bloque
}

int ext4_crear_archivo(const char *nombre, const uint8_t *datos, uint32_t tamano) {
    if (!nombre || *nombre == '\0') return -1;
    if (!g_volumen.montado) {
        if (ext4_montar(0) != 0) return -2;
    }

    // Verificar si ya existe
    if (ext4_buscar_inodo_por_ruta(nombre) != 0) {
        consola_imprimir_linea_color("  [!] Ya existe un archivo o carpeta con ese nombre.", COLOR_ERROR_DEFAULT);
        return -3;
    }

    // 1. Asignar inodo
    uint32_t nuevo_inodo = ext4_asignar_inodo_libre();
    if (nuevo_inodo == 0) {
        consola_imprimir_linea_color("  [!] Error: No hay inodos libres en la partición ext4.", COLOR_ERROR_DEFAULT);
        return -4;
    }

    // 2. Asignar bloque de datos si tamano > 0
    uint32_t bloque_datos = 0;
    if (tamano > 0) {
        bloque_datos = ext4_asignar_bloque_libre();
        if (bloque_datos == 0) {
            consola_imprimir_linea_color("  [!] Error: No hay bloques de datos libres en ext4.", COLOR_ERROR_DEFAULT);
            return -5;
        }

        // Limpiar y escribir datos en el nuevo bloque
        for (uint32_t i = 0; i < g_volumen.tamano_bloque; i++) g_bloque_buf[i] = 0;
        uint32_t a_copiar = (tamano < g_volumen.tamano_bloque) ? tamano : g_volumen.tamano_bloque;
        if (datos) {
            for (uint32_t i = 0; i < a_copiar; i++) g_bloque_buf[i] = datos[i];
        }
        ext4_escribir_bloque(bloque_datos, g_bloque_buf);
    }

    // 3. Crear estructura de inodo tipo archivo regular
    struct ext4_inodo inodo;
    for (uint32_t i = 0; i < sizeof(struct ext4_inodo); i++) ((uint8_t *)&inodo)[i] = 0;

    inodo.i_mode = EXT4_S_IFREG | 0644; // Archivo regular rw-r--r--
    inodo.i_links_count = 1;
    inodo.i_size_lo = tamano;
    inodo.i_blocks_lo = (tamano > 0) ? g_volumen.sectores_por_bloque : 0;
    inodo.i_flags = EXT4_EXTENTS_FL;

    // Configurar extent directo
    struct ext4_extent_cabecera *eh = (struct ext4_extent_cabecera *)&inodo.i_block[0];
    eh->eh_magic = EXT4_EXTENTS_MAGIC;
    eh->eh_entries = (tamano > 0) ? 1 : 0;
    eh->eh_max = 4;
    eh->eh_depth = 0;

    if (tamano > 0) {
        struct ext4_extent *ex = (struct ext4_extent *)&inodo.i_block[3];
        ex->ee_block = 0;
        ex->ee_len = 1;
        ex->ee_start_hi = 0;
        ex->ee_start_lo = bloque_datos;
    }

    // Escribir inodo en disco
    if (ext4_escribir_inodo(nuevo_inodo, &inodo) != 0) return -6;

    // 4. Insertar entrada en directorio raíz
    if (ext4_insertar_entrada_directorio(EXT4_ROOT_INO, nuevo_inodo, nombre, EXT4_FT_REG_FILE) != 0) {
        return -7;
    }

    consola_imprimir("==> [EXT4] Archivo creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Inodo: ");
    consola_imprimir_dec(nuevo_inodo);
    consola_imprimir_linea(")");
    return 0;
}

int ext4_crear_directorio(const char *nombre) {
    if (!nombre || *nombre == '\0') return -1;
    if (!g_volumen.montado) {
        if (ext4_montar(0) != 0) return -2;
    }

    if (ext4_buscar_inodo_por_ruta(nombre) != 0) {
        consola_imprimir_linea_color("  [!] Ya existe un archivo o carpeta con ese nombre.", COLOR_ERROR_DEFAULT);
        return -3;
    }

    uint32_t nuevo_inodo = ext4_asignar_inodo_libre();
    if (nuevo_inodo == 0) return -4;

    uint32_t bloque_dir = ext4_asignar_bloque_libre();
    if (bloque_dir == 0) return -5;

    // Limpiar bloque y crear '.' y '..'
    for (uint32_t i = 0; i < g_volumen.tamano_bloque; i++) g_bloque_buf[i] = 0;

    // Entrada '.'
    struct ext4_dir_entry_cabecera *d_punto = (struct ext4_dir_entry_cabecera *)&g_bloque_buf[0];
    d_punto->inodo = nuevo_inodo;
    d_punto->rec_len = 12;
    d_punto->name_len = 1;
    d_punto->file_type = EXT4_FT_DIR;
    g_bloque_buf[8] = '.';

    // Entrada '..'
    struct ext4_dir_entry_cabecera *d_dospuntos = (struct ext4_dir_entry_cabecera *)&g_bloque_buf[12];
    d_dospuntos->inodo = EXT4_ROOT_INO;
    d_dospuntos->rec_len = (uint16_t)(g_volumen.tamano_bloque - 12);
    d_dospuntos->name_len = 2;
    d_dospuntos->file_type = EXT4_FT_DIR;
    g_bloque_buf[20] = '.';
    g_bloque_buf[21] = '.';

    ext4_escribir_bloque(bloque_dir, g_bloque_buf);

    // Inodo de directorio
    struct ext4_inodo inodo;
    for (uint32_t i = 0; i < sizeof(struct ext4_inodo); i++) ((uint8_t *)&inodo)[i] = 0;

    inodo.i_mode = EXT4_S_IFDIR | 0755;
    inodo.i_links_count = 2; // '.' y el padre
    inodo.i_size_lo = g_volumen.tamano_bloque;
    inodo.i_blocks_lo = g_volumen.sectores_por_bloque;
    inodo.i_flags = EXT4_EXTENTS_FL;

    struct ext4_extent_cabecera *eh = (struct ext4_extent_cabecera *)&inodo.i_block[0];
    eh->eh_magic = EXT4_EXTENTS_MAGIC;
    eh->eh_entries = 1;
    eh->eh_max = 4;
    eh->eh_depth = 0;

    struct ext4_extent *ex = (struct ext4_extent *)&inodo.i_block[3];
    ex->ee_block = 0;
    ex->ee_len = 1;
    ex->ee_start_hi = 0;
    ex->ee_start_lo = bloque_dir;

    ext4_escribir_inodo(nuevo_inodo, &inodo);

    // Insertar en directorio padre (raíz)
    ext4_insertar_entrada_directorio(EXT4_ROOT_INO, nuevo_inodo, nombre, EXT4_FT_DIR);

    consola_imprimir("==> [EXT4] Directorio creado con éxito: '");
    consola_imprimir(nombre);
    consola_imprimir("' (Inodo: ");
    consola_imprimir_dec(nuevo_inodo);
    consola_imprimir_linea(")");
    return 0;
}
