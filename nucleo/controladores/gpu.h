#ifndef CONTROLADORES_GPU_H
#define CONTROLADORES_GPU_H

#include <stdint.h>
#include <stddef.h>

// Dirección virtual asignada en el Kernel de TAEK OS para el mapeo MMIO de la GPU
#define GPU_MMIO_VIRTUAL_BASE 0xFFFFFE0000000000ULL

struct estado_gpu {
    uint8_t  gpu_detectada;
    uint8_t  bus;
    uint8_t  ranura;
    uint8_t  funcion;
    uint16_t id_proveedor;
    uint16_t id_dispositivo;
    char     nombre_proveedor[32];
    char     nombre_clase[48];

    // Espacio de Registros de Control MMIO (Memory-Mapped I/O)
    uint64_t dir_fisica_mmio;
    uint64_t dir_virtual_mmio;
    uint64_t tamano_mmio;
    uint8_t  mapeo_mmio_activo;

    // Espacio de Memoria de Video (VRAM Aperture)
    uint64_t dir_fisica_vram;
    uint64_t tamano_vram;
    uint8_t  vram_detectada;

    // Identificación del Silicio
    uint32_t firma_silicio_boot0;
    char     arquitectura_nombre[40];
    uint8_t  silicio_leido;
};

int  gpu_iniciar(void);
const struct estado_gpu *gpu_obtener_estado(void);
uint32_t gpu_leer_mmio_32(uint32_t offset);
void     gpu_escribir_mmio_32(uint32_t offset, uint32_t valor);
int  gpu_ejecutar_autodiagnostico(void);
const char *gpu_nombre_arquitectura_nvidia(uint32_t boot0);

#endif // CONTROLADORES_GPU_H
