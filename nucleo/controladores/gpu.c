#include "gpu.h"
#include "../arquitectura/x86_64/pci.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/paginacion.h"
#include "../base/tiempo.h"
#include "../base/huevo.h"

static struct estado_gpu g_gpu;
static int g_gpu_iniciada = 0;

static void str_copiar(char *dest, const char *src, int max) {
    if (!dest || max <= 0) return;
    int i = 0;
    if (src) {
        while (src[i] && i < max - 1) {
            dest[i] = src[i];
            i++;
        }
    }
    dest[i] = '\0';
}

const char *gpu_nombre_arquitectura_nvidia(uint32_t boot0) {
    uint32_t chip_id = (boot0 >> 20) & 0x1FF;

    if (chip_id >= 0x190 && chip_id <= 0x19F) return "NVIDIA Blackwell (RTX 5000 / GB20x)";
    if (chip_id >= 0x170 && chip_id <= 0x17F) return "NVIDIA Ada Lovelace (RTX 4000 / AD10x)";
    if (chip_id >= 0x160 && chip_id <= 0x16F) return "NVIDIA Ampere (RTX 3000 / GA10x)";
    if (chip_id >= 0x140 && chip_id <= 0x14F) return "NVIDIA Turing (RTX 2000 / TU10x)";
    if (chip_id >= 0x130 && chip_id <= 0x13F) return "NVIDIA Volta (GV100)";
    if (chip_id >= 0x120 && chip_id <= 0x12F) return "NVIDIA Pascal (GTX 1000 / GP10x)";
    if (chip_id >= 0x110 && chip_id <= 0x11F) return "NVIDIA Maxwell (GTX 900 / GM20x)";
    if (chip_id >= 0x100 && chip_id <= 0x10F) return "NVIDIA Kepler (GTX 600/700 / GK10x)";

    return "NVIDIA Silicio Desconocido";
}

int gpu_iniciar(void) {
    if (g_gpu_iniciada) return 0;

    uint8_t *p = (uint8_t *)&g_gpu;
    for (size_t i = 0; i < sizeof(struct estado_gpu); i++) {
        p[i] = 0;
    }

    const struct dispositivo_pci *dev = pci_obtener_gpu_primaria();
    if (!dev) {
        g_gpu.gpu_detectada = 0;
        return 1;
    }

    g_gpu.gpu_detectada   = 1;
    g_gpu.bus             = dev->bus;
    g_gpu.ranura          = dev->ranura;
    g_gpu.funcion         = dev->funcion;
    g_gpu.id_proveedor    = dev->id_proveedor;
    g_gpu.id_dispositivo  = dev->id_dispositivo;
    str_copiar(g_gpu.nombre_proveedor, pci_nombre_proveedor(dev->id_proveedor), sizeof(g_gpu.nombre_proveedor));
    str_copiar(g_gpu.nombre_clase, pci_nombre_clase(dev->clase, dev->subclase), sizeof(g_gpu.nombre_clase));

    int idx_bar_mmio = -1;
    int idx_bar_vram = -1;

    if (dev->id_proveedor == 0x10DE) {
        idx_bar_mmio = 0;
        idx_bar_vram = 1;
    } else if (dev->id_proveedor == 0x1234) {
        idx_bar_mmio = 2;
        idx_bar_vram = 0;
    } else {
        for (int b = 0; b < 6; b++) {
            if (!dev->barras[b].valida) continue;
            if (dev->barras[b].es_io) continue;

            if (dev->barras[b].predecible && idx_bar_vram == -1) {
                idx_bar_vram = b;
            } else if (!dev->barras[b].predecible && idx_bar_mmio == -1) {
                idx_bar_mmio = b;
            }
        }
        if (idx_bar_mmio == -1) idx_bar_mmio = 0;
    }

    if (idx_bar_mmio >= 0 && dev->barras[idx_bar_mmio].valida) {
        g_gpu.dir_fisica_mmio = dev->barras[idx_bar_mmio].dir_base;
        g_gpu.tamano_mmio     = dev->barras[idx_bar_mmio].tamano;
    }

    if (idx_bar_vram >= 0 && dev->barras[idx_bar_vram].valida) {
        g_gpu.dir_fisica_vram = dev->barras[idx_bar_vram].dir_base;
        g_gpu.tamano_vram     = dev->barras[idx_bar_vram].tamano;
        g_gpu.vram_detectada  = 1;
    }

    if (g_gpu.dir_fisica_mmio != 0) {
        g_gpu.dir_virtual_mmio = GPU_MMIO_VIRTUAL_BASE;

        uint64_t limite_mapeo = g_gpu.tamano_mmio;
        if (limite_mapeo == 0 || limite_mapeo > 16ULL * 1024ULL * 1024ULL) {
            limite_mapeo = 16ULL * 1024ULL * 1024ULL;
        }

        for (uint64_t offset = 0; offset < limite_mapeo; offset += 4096) {
            int ret = paginacion_mapear(g_gpu.dir_virtual_mmio + offset,
                                       g_gpu.dir_fisica_mmio + offset,
                                       PAGINA_ATRIBUTOS_MMIO);
            if (ret != 0) {
                g_gpu.mapeo_mmio_activo = 0;
                return -2;
            }
        }
        g_gpu.mapeo_mmio_activo = 1;

        pci_activar_bus_master(dev);

        g_gpu.firma_silicio_boot0 = gpu_leer_mmio_32(0x00000000);
        g_gpu.silicio_leido = 1;

        if (dev->id_proveedor == 0x10DE) {
            str_copiar(g_gpu.arquitectura_nombre,
                       gpu_nombre_arquitectura_nvidia(g_gpu.firma_silicio_boot0),
                       sizeof(g_gpu.arquitectura_nombre));
        } else if (dev->id_proveedor == 0x8086) {
            str_copiar(g_gpu.arquitectura_nombre, "Intel Graphics Display Engine", sizeof(g_gpu.arquitectura_nombre));
        } else if (dev->id_proveedor == 0x1234) {
            str_copiar(g_gpu.arquitectura_nombre, "Bochs / QEMU Extended VGA", sizeof(g_gpu.arquitectura_nombre));
        } else if (dev->id_proveedor == 0x1AF4) {
            str_copiar(g_gpu.arquitectura_nombre, "VirtIO GPU Accelerator", sizeof(g_gpu.arquitectura_nombre));
        } else {
            str_copiar(g_gpu.arquitectura_nombre, "Controlador Gráfico PCI Genérico", sizeof(g_gpu.arquitectura_nombre));
        }
    }

    g_gpu_iniciada = 1;
    return 0;
}

const struct estado_gpu *gpu_obtener_estado(void) {
    if (!g_gpu_iniciada) gpu_iniciar();
    return &g_gpu;
}

uint32_t gpu_leer_mmio_32(uint32_t offset) {
    if (!g_gpu.mapeo_mmio_activo || g_gpu.dir_virtual_mmio == 0) {
        return 0xFFFFFFFF;
    }
    volatile uint32_t *reg = (volatile uint32_t *)(g_gpu.dir_virtual_mmio + offset);
    return *reg;
}

void gpu_escribir_mmio_32(uint32_t offset, uint32_t valor) {
    if (!g_gpu.mapeo_mmio_activo || g_gpu.dir_virtual_mmio == 0) {
        return;
    }
    volatile uint32_t *reg = (volatile uint32_t *)(g_gpu.dir_virtual_mmio + offset);
    *reg = valor;
}

int gpu_ejecutar_autodiagnostico(void) {
    if (!g_gpu_iniciada) {
        if (gpu_iniciar() != 0) return 1;
    }

    if (!g_gpu.gpu_detectada) return 2;
    if (!g_gpu.mapeo_mmio_activo) return 3;

    uint64_t tsc_inicio = rdtsc();
    uint32_t val1 = gpu_leer_mmio_32(0x00000000);
    uint32_t val2 = gpu_leer_mmio_32(0x00000000);
    uint64_t tsc_fin = rdtsc();

    (void)val1;
    (void)val2;
    (void)(tsc_fin - tsc_inicio);

    return 0;
}
