#include "linux.h"
#include "../base/memoria.h"
#include "../base/dma.h"
#include "../base/paginacion.h"
#include "../base/tiempo.h"
#include "../base/huevo.h"
#include "../arquitectura/x86_64/pci.h"
#include "../arquitectura/x86_64/serial.h"
#include "../controladores/consola.h"

#define MAX_LINUX_PCI_DEVS 32
#define MAX_MAPEOS_IOREMAP 64

struct registro_ioremap {
    uint64_t dir_virtual;
    uint64_t dir_fisica;
    uint64_t tamano;
    int      activo;
};

static uint64_t g_mem_dma_asignada = 0;
static uint64_t g_conteo_mapeos_ioremap = 0;
static uint64_t g_cursor_ioremap_virtual = LINUX_SHIM_IOREMAP_BASE;
static struct registro_ioremap g_tabla_ioremap[MAX_MAPEOS_IOREMAP];

static struct pci_dev g_tabla_linux_pci[MAX_LINUX_PCI_DEVS];
static uint32_t       g_conteo_linux_pci = 0;
static int            g_linux_shim_iniciado = 0;

static void sincronizar_dispositivos_pci(void) {
    int total_pci = pci_obtener_conteo();
    g_conteo_linux_pci = 0;

    for (int i = 0; i < total_pci && g_conteo_linux_pci < MAX_LINUX_PCI_DEVS; i++) {
        const struct dispositivo_pci *pci = pci_obtener_dispositivo(i);
        if (!pci) continue;

        struct pci_dev *ldev = &g_tabla_linux_pci[g_conteo_linux_pci++];
        ldev->bus              = pci->bus;
        ldev->slot             = pci->ranura;
        ldev->func             = pci->funcion;
        ldev->devfn            = (pci->ranura << 3) | (pci->funcion & 0x07);
        ldev->vendor           = pci->id_proveedor;
        ldev->device           = pci->id_dispositivo;
        ldev->subsystem_vendor = pci->sub_proveedor;
        ldev->subsystem_device = pci->sub_dispositivo;
        ldev->class            = (pci->clase << 16) | (pci->subclase << 8) | pci->prog_if;
        ldev->irq              = pci->linea_irq;
        ldev->sysdata          = (void *)pci;

        for (int b = 0; b < 6; b++) {
            if (pci->barras[b].valida) {
                ldev->resource[b].start = pci->barras[b].dir_base;
                ldev->resource[b].end   = pci->barras[b].dir_base + pci->barras[b].tamano - 1;
                ldev->resource[b].flags = pci->barras[b].es_io ? IORESOURCE_IO : IORESOURCE_MEM;
                if (pci->barras[b].predecible) {
                    ldev->resource[b].flags |= IORESOURCE_PREFETCH;
                }
            } else {
                ldev->resource[b].start = 0;
                ldev->resource[b].end   = 0;
                ldev->resource[b].flags = 0;
            }
        }
    }
}

int linux_shim_iniciar(void) {
    if (g_linux_shim_iniciado) return 0;

    for (int i = 0; i < MAX_MAPEOS_IOREMAP; i++) {
        g_tabla_ioremap[i].activo = 0;
    }

    sincronizar_dispositivos_pci();
    g_linux_shim_iniciado = 1;
    return 0;
}

void linux_shim_obtener_estadisticas(uint64_t *mem_dma_asignada,
                                     uint64_t *mapeos_ioremap,
                                     uint32_t *dispositivos_pci) {
    if (mem_dma_asignada) *mem_dma_asignada = g_mem_dma_asignada;
    if (mapeos_ioremap)    *mapeos_ioremap    = g_conteo_mapeos_ioremap;
    if (dispositivos_pci)  *dispositivos_pci  = g_conteo_linux_pci;
}

void *dma_alloc_coherent(void *dev, size_t size, dma_addr_t *dma_handle, unsigned int flag) {
    (void)dev;
    if (size == 0 || !dma_handle) return NULL;

    uint64_t alineacion = 4096;
    if (size >= 65536) {
        alineacion = 65536; // Alineación estricta de 64 KiB requerida por hardware GPU/GSP/WPR
    }

    uint64_t phys = 0;
    void *virt = dma_asignar_bufer_contiguo(size, alineacion, &phys);

    if (!virt) {
        // Fallback al PMM si la arena DMA no está disponible o está colmada
        size_t num_paginas = (size + 4095) / 4096;
        phys = pmm_asignar_pagina_fisica();
        if (phys == 0) return NULL;
        for (size_t i = 1; i < num_paginas; i++) {
            pmm_asignar_pagina_fisica();
        }
        uint64_t hhdm = memoria_obtener_hhdm_offset();
        virt = (void *)(hhdm + phys);
    }

    *dma_handle = (dma_addr_t)phys;

    if (flag & __GFP_ZERO) {
        memset(virt, 0, size);
    }

    size_t num_paginas = (size + 4095) / 4096;
    g_mem_dma_asignada += num_paginas * 4096;
    return virt;
}

void dma_free_coherent(void *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle) {
    (void)dev;
    if (size == 0) return;

    dma_liberar_bufer_contiguo(cpu_addr, (uint64_t)dma_handle, size);

    size_t num_paginas = (size + 4095) / 4096;
    if (g_mem_dma_asignada >= num_paginas * 4096) {
        g_mem_dma_asignada -= num_paginas * 4096;
    }
}

static void *ioremap_interno(phys_addr_t offset, size_t size, uint64_t atributos) {
    if (size == 0) return NULL;

    uint64_t phys_inicio = offset & ~0xFFFULL;
    uint64_t off_dentro  = offset & 0xFFFULL;
    uint64_t tam_alineado = ((size + off_dentro + 4095) / 4096) * 4096;

    uint64_t virt_inicio = g_cursor_ioremap_virtual;
    g_cursor_ioremap_virtual += tam_alineado + 4096; // 4 KiB guard page

    for (uint64_t p = 0; p < tam_alineado; p += 4096) {
        int res = paginacion_mapear(virt_inicio + p, phys_inicio + p, atributos);
        if (res != 0) {
            return NULL;
        }
    }

    for (int i = 0; i < MAX_MAPEOS_IOREMAP; i++) {
        if (!g_tabla_ioremap[i].activo) {
            g_tabla_ioremap[i].dir_virtual = virt_inicio;
            g_tabla_ioremap[i].dir_fisica  = phys_inicio;
            g_tabla_ioremap[i].tamano      = tam_alineado;
            g_tabla_ioremap[i].activo      = 1;
            break;
        }
    }

    g_conteo_mapeos_ioremap++;
    return (void *)(virt_inicio + off_dentro);
}

void *ioremap(phys_addr_t offset, size_t size) {
    return ioremap_interno(offset, size, PAGINA_ATRIBUTOS_MMIO);
}

void *ioremap_nocache(phys_addr_t offset, size_t size) {
    return ioremap_interno(offset, size, PAGINA_ATRIBUTOS_MMIO);
}

void *ioremap_wc(phys_addr_t offset, size_t size) {
    return ioremap_interno(offset, size, PAGINA_PRESENTE | PAGINA_ESCRITURA | PAGINA_ESCRITURA_DIR);
}

void iounmap(void *addr) {
    if (!addr) return;
    uint64_t virt = (uint64_t)addr & ~0xFFFULL;

    for (int i = 0; i < MAX_MAPEOS_IOREMAP; i++) {
        if (g_tabla_ioremap[i].activo && g_tabla_ioremap[i].dir_virtual == virt) {
            for (uint64_t p = 0; p < g_tabla_ioremap[i].tamano; p += 4096) {
                paginacion_desmapear(virt + p);
            }
            g_tabla_ioremap[i].activo = 0;
            if (g_conteo_mapeos_ioremap > 0) g_conteo_mapeos_ioremap--;
            return;
        }
    }
}

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from) {
    if (!g_linux_shim_iniciado) linux_shim_iniciar();

    int start_idx = 0;
    if (from != NULL) {
        for (uint32_t i = 0; i < g_conteo_linux_pci; i++) {
            if (&g_tabla_linux_pci[i] == from) {
                start_idx = i + 1;
                break;
            }
        }
    }

    for (uint32_t i = start_idx; i < g_conteo_linux_pci; i++) {
        if (g_tabla_linux_pci[i].vendor == (unsigned short)vendor &&
            g_tabla_linux_pci[i].device == (unsigned short)device) {
            return &g_tabla_linux_pci[i];
        }
    }
    return NULL;
}

struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from) {
    if (!g_linux_shim_iniciado) linux_shim_iniciar();

    int start_idx = 0;
    if (from != NULL) {
        for (uint32_t i = 0; i < g_conteo_linux_pci; i++) {
            if (&g_tabla_linux_pci[i] == from) {
                start_idx = i + 1;
                break;
            }
        }
    }

    for (uint32_t i = start_idx; i < g_conteo_linux_pci; i++) {
        if ((g_tabla_linux_pci[i].class >> 8) == (class >> 8)) {
            return &g_tabla_linux_pci[i];
        }
    }
    return NULL;
}

int pci_enable_device(struct pci_dev *dev) {
    if (!dev) return -EINVAL;
    u16 cmd = 0;
    pci_read_config_word(dev, 0x04, &cmd);
    cmd |= (1 << 0) | (1 << 1); // IO Enable + Memory Enable
    pci_write_config_word(dev, 0x04, cmd);
    return 0;
}

void pci_set_master(struct pci_dev *dev) {
    if (!dev) return;
    u16 cmd = 0;
    pci_read_config_word(dev, 0x04, &cmd);
    cmd |= (1 << 2); // Bus Master Enable
    pci_write_config_word(dev, 0x04, cmd);
}

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val) {
    if (!dev || !val) return -EINVAL;
    *val = pci_leer_config_8(dev->bus, dev->slot, dev->func, where);
    return 0;
}

int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val) {
    if (!dev || !val) return -EINVAL;
    *val = pci_leer_config_16(dev->bus, dev->slot, dev->func, where);
    return 0;
}

int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val) {
    if (!dev || !val) return -EINVAL;
    *val = pci_leer_config_32(dev->bus, dev->slot, dev->func, where);
    return 0;
}

int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val) {
    if (!dev) return -EINVAL;
    pci_escribir_config_8(dev->bus, dev->slot, dev->func, where, val);
    return 0;
}

int pci_write_config_word(const struct pci_dev *dev, int where, u16 val) {
    if (!dev) return -EINVAL;
    pci_escribir_config_16(dev->bus, dev->slot, dev->func, where, val);
    return 0;
}

int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val) {
    if (!dev) return -EINVAL;
    pci_escribir_config_32(dev->bus, dev->slot, dev->func, where, val);
    return 0;
}

resource_size_t pci_resource_start(struct pci_dev *dev, int bar) {
    if (!dev || bar < 0 || bar >= 6) return 0;
    return dev->resource[bar].start;
}

resource_size_t pci_resource_len(struct pci_dev *dev, int bar) {
    if (!dev || bar < 0 || bar >= 6) return 0;
    if (dev->resource[bar].end < dev->resource[bar].start) return 0;
    return dev->resource[bar].end - dev->resource[bar].start + 1;
}

unsigned long pci_resource_flags(struct pci_dev *dev, int bar) {
    if (!dev || bar < 0 || bar >= 6) return 0;
    return dev->resource[bar].flags;
}

int printk(const char *fmt, ...) {
    if (!fmt) return 0;

    const char *p = fmt;
    if (p[0] == '<' && p[1] >= '0' && p[1] <= '7' && p[2] == '>') {
        p += 3;
    }

    char buf[512];
    int idx = 0;

    va_list args;
    va_start(args, fmt);

    while (*p && idx < (int)sizeof(buf) - 1) {
        if (*p != '%') {
            buf[idx++] = *p++;
            continue;
        }

        p++; // Saltar '%'
        if (*p == '%') {
            buf[idx++] = '%';
            p++;
        } else if (*p == 's') {
            const char *str = va_arg(args, const char *);
            if (!str) str = "(null)";
            while (*str && idx < (int)sizeof(buf) - 1) buf[idx++] = *str++;
            p++;
        } else if (*p == 'd' || *p == 'i') {
            int val = va_arg(args, int);
            if (val < 0) {
                if (idx < (int)sizeof(buf) - 1) buf[idx++] = '-';
                val = -val;
            }
            char num_buf[16];
            int n_idx = 0;
            if (val == 0) num_buf[n_idx++] = '0';
            else {
                while (val > 0 && n_idx < 15) {
                    num_buf[n_idx++] = '0' + (val % 10);
                    val /= 10;
                }
            }
            while (n_idx > 0 && idx < (int)sizeof(buf) - 1) {
                buf[idx++] = num_buf[--n_idx];
            }
            p++;
        } else if (*p == 'u') {
            unsigned int val = va_arg(args, unsigned int);
            char num_buf[16];
            int n_idx = 0;
            if (val == 0) num_buf[n_idx++] = '0';
            else {
                while (val > 0 && n_idx < 15) {
                    num_buf[n_idx++] = '0' + (val % 10);
                    val /= 10;
                }
            }
            while (n_idx > 0 && idx < (int)sizeof(buf) - 1) {
                buf[idx++] = num_buf[--n_idx];
            }
            p++;
        } else if (*p == 'x' || *p == 'p') {
            uint64_t val = (*p == 'p') ? (uint64_t)va_arg(args, void *) : (uint64_t)va_arg(args, unsigned int);
            if (*p == 'p') {
                if (idx < (int)sizeof(buf) - 2) {
                    buf[idx++] = '0';
                    buf[idx++] = 'x';
                }
            }
            char hex_buf[20];
            const char hex_digits[] = "0123456789abcdef";
            int h_idx = 0;
            if (val == 0) hex_buf[h_idx++] = '0';
            else {
                while (val > 0 && h_idx < 19) {
                    hex_buf[h_idx++] = hex_digits[val & 0x0F];
                    val >>= 4;
                }
            }
            while (h_idx > 0 && idx < (int)sizeof(buf) - 1) {
                buf[idx++] = hex_buf[--h_idx];
            }
            p++;
        } else {
            buf[idx++] = *p++;
        }
    }

    va_end(args);
    buf[idx] = '\0';

    serial_imprimir("[LINUX-SHIM] ");
    serial_imprimir_linea(buf);
    consola_imprimir_linea_color(buf, COLOR_PROMPT_DEFAULT);

    return idx;
}

int linux_shim_ejecutar_autodiagnostico(void) {
    if (!g_linux_shim_iniciado) linux_shim_iniciar();

    // 1. Probar Spinlock y Atomics
    spinlock_t lock = SPIN_LOCK_UNLOCKED;
    spin_lock(&lock);
    atomic_t cont = ATOMIC_INIT(42);
    atomic_inc(&cont);
    atomic_dec(&cont);
    if (atomic_read(&cont) != 42) {
        spin_unlock(&lock);
        return 1;
    }
    spin_unlock(&lock);

    // 2. Probar Asignación DMA Coherente (búfer continuo)
    dma_addr_t dma_handle = 0;
    void *cpu_addr = dma_alloc_coherent(NULL, 8192, &dma_handle, __GFP_ZERO);
    if (!cpu_addr || dma_handle == 0) return 2;

    uint32_t *dma_ptr = (uint32_t *)cpu_addr;
    dma_ptr[0] = 0x14900BEE; // Firma i9-14900HX
    dma_ptr[1] = 0x50700000; // Firma RTX 5070 Ti

    mb(); // Barrera de memoria

    if (dma_ptr[0] != 0x14900BEE || dma_ptr[1] != 0x50700000) {
        dma_free_coherent(NULL, 8192, cpu_addr, dma_handle);
        return 3;
    }
    dma_free_coherent(NULL, 8192, cpu_addr, dma_handle);

    // 3. Probar ioremap_nocache
    void *mmio_virt = ioremap_nocache(0x00000000, 4096);
    if (!mmio_virt) return 4;
    iounmap(mmio_virt);

    // 4. Probar enumeración pci_get_class
    struct pci_dev *gpu_dev = pci_get_class(0x030000, NULL);
    (void)gpu_dev; // Puede ser nulo si no hay display controller registrado

    return 0;
}
