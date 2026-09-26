#ifndef CONTROLADORES_EXT4_H
#define CONTROLADORES_EXT4_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// TAEK OS - CONTROLADOR DE SISTEMA DE ARCHIVOS EXT4 (Hito 51)
// Arquitectura Anillo 0 en Español (Lectura y Escritura)
// ============================================================================

#define EXT4_SUPER_MAGIC     0xEF53
#define EXT4_EXTENTS_MAGIC   0xF30A

#define EXT4_ROOT_INO        2

#define EXT4_S_IFDIR         0x4000
#define EXT4_S_IFREG         0x8000

#define EXT4_FT_UNKNOWN      0
#define EXT4_FT_REG_FILE     1
#define EXT4_FT_DIR          2

#define EXT4_EXTENTS_FL      0x00080000

// Superbloque ext4 (offset 1024 bytes)
struct __attribute__((packed)) ext4_superbloque {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count_lo;
    uint32_t s_r_blocks_count_lo;
    uint32_t s_free_blocks_count_lo;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;        // Tamaño de bloque = 1024 << s_log_block_size
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;                 // 0xEF53
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;            // Usualmente 256 bytes
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;             // 32 o 64 bytes
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
};

// Descriptor de grupo de bloques
struct __attribute__((packed)) ext4_grupo_descriptor {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
};

// Inodo ext4 (mínimo 128 bytes, extendido a 256)
struct __attribute__((packed)) ext4_inodo {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];             // 60 bytes: Árbol de extents
    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;
    uint32_t i_obso_faddr;
    uint16_t i_osd2[12];
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
};

// Cabecera del árbol de extents (12 bytes)
struct __attribute__((packed)) ext4_extent_cabecera {
    uint16_t eh_magic;                // 0xF30A
    uint16_t eh_entries;              // Cantidad de extents válidos
    uint16_t eh_max;                  // Capacidad máxima en el bloque
    uint16_t eh_depth;                // 0 = hojas (apuntan a bloques físicos de datos)
    uint32_t eh_generation;
};

// Entrada de extent (12 bytes)
struct __attribute__((packed)) ext4_extent {
    uint32_t ee_block;                // Primer bloque lógico que cubre este extent
    uint16_t ee_len;                  // Cantidad de bloques que abarca
    uint16_t ee_start_hi;             // Parte alta de 16 bits del bloque físico
    uint32_t ee_start_lo;             // Parte baja de 32 bits del bloque físico
};

// Entrada de directorio ext4
struct __attribute__((packed)) ext4_dir_entry_cabecera {
    uint32_t inodo;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
};

// Descriptor del volumen ext4 montado
struct ext4_volumen {
    int      montado;
    uint8_t  unidad_msc;
    uint32_t lba_inicio_particion;
    uint32_t tamano_bloque;           // Típicamente 4096 o 1024 bytes
    uint32_t sectores_por_bloque;     // tamano_bloque / 512
    uint32_t bloques_por_grupo;
    uint32_t inodos_por_grupo;
    uint16_t tamano_inodo;            // Típicamente 256 bytes
    uint16_t tamano_descriptor_grupo; // 32 o 64 bytes
    uint32_t total_grupos;
    uint32_t lba_bloque_descriptores;
    char     etiqueta[17];
};

// --- API PÚBLICA DEL CONTROLADOR EXT4 ---
void ext4_iniciar(void);
int  ext4_montar(uint8_t unidad_msc);
void ext4_desmontar(void);
int  ext4_esta_montado(void);
const struct ext4_volumen *ext4_obtener_volumen(void);

int  ext4_ejecutar_tree(const char *ruta_inicial);
int  ext4_listar_directorio(const char *ruta);
int  ext4_leer_archivo_texto(const char *ruta);

// Escritura en ext4
int  ext4_crear_archivo(const char *nombre, const uint8_t *datos, uint32_t tamano);
int  ext4_crear_directorio(const char *nombre);

#endif // CONTROLADORES_EXT4_H
