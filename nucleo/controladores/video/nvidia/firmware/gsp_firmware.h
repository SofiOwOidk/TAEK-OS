#ifndef GSP_FIRMWARE_H
#define GSP_FIRMWARE_H

#include "../inc/nvtypes.h"
#include "../inc/nvstatus.h"
#include "../inc/nv_gsp.h"

// ============================================================================
// NVIDIA GSP FIRMWARE LOADER & DESCRIPTOR (HITO 18)
// Carga, desempaquetado de ELF y preparación de región protegida WPR en DMA
// ============================================================================

#define GSP_FIRMWARE_VERSION_MAJ 570
#define GSP_FIRMWARE_VERSION_MIN 86
#define GSP_FIRMWARE_NOMBRE "gsp_gb20x.bin (NVIDIA Blackwell Microcode)"

// Descriptor del Microcódigo GSP
typedef struct {
    NvU64 magic;                    // GSP_FW_SIGNATURE (0x00505347ULL "GSP\0")
    NvU32 version_major;
    NvU32 version_minor;
    char  nombre_firmware[64];
    NvU64 tamano_total;
    NvU64 tamano_bootloader;
    NvU64 tamano_imagen_rm;
    NvU64 tamano_wpr_heap;          // Tamaño requerido de la región WPR (16 MiB o 32 MiB)
    NvBool firmas_autenticadas;     // 1 si el Boot ROM / Falcon validó la firma
    NvBool cargado_en_dma;          // 1 si está residente en memoria DMA contigua
} gsp_firmware_descriptor_t;

// Argumentos de arranque pasados al coprocesador GSP (Falcon / RISC-V)
typedef struct {
    NvU64 wpr_base_phys;            // Dirección física base de la región protegida WPR
    NvU64 wpr_size;                 // Tamaño de la región WPR
    NvU64 cmd_queue_phys;           // Dirección física de la cola de comandos RPC
    NvU64 stat_queue_phys;          // Dirección física de la cola de estado RPC
    NvU32 shared_mem_size;          // Tamaño del búfer de colas compartidas
    NvU32 boot_flags;               // Banderas de arranque (Debug, Verbose, FastBoot)
    NvU32 status_init;              // Código de estado retornado por el Bootloader
    NvU32 canario_verificacion;     // Canario de seguridad de TAEK OS
} __attribute__((aligned(64))) gsp_boot_args_t;

struct nvidia_dispositivo; // Declaración adelantada

// Carga y prepara el microcódigo GSP en memoria DMA contigua y configura argumentos WPR
NV_STATUS gsp_firmware_cargar(struct nvidia_dispositivo *dev);

// Descarga el firmware y libera la memoria WPR
NV_STATUS gsp_firmware_descargar(struct nvidia_dispositivo *dev);

// Obtiene el descriptor con la información del firmware actual
const gsp_firmware_descriptor_t *gsp_firmware_obtener_info(void);

// Obtiene la estructura de argumentos de arranque del GSP
const gsp_boot_args_t *gsp_firmware_obtener_boot_args(void);

// Autodiagnóstico del cargador de firmware (Hito 18)
int gsp_firmware_autodiagnostico(struct nvidia_dispositivo *dev);

#endif // GSP_FIRMWARE_H
