#include "nv_os_interface.h"
#include "linux.h"
#include "../base/tiempo.h"
#include "../arquitectura/x86_64/pci.h"
#include "../arquitectura/x86_64/serial.h"

static uint64_t g_nv_paginas_dma = 0;
static uint64_t g_nv_mapeos_mmio = 0;

void *nv_os_alloc_pages(size_t size, uint64_t *dma_phys) {
    if (!dma_phys || size == 0) return NULL;
    dma_addr_t handle = 0;
    void *ptr = dma_alloc_coherent(NULL, size, &handle, __GFP_ZERO);
    if (ptr) {
        *dma_phys = (uint64_t)handle;
        g_nv_paginas_dma += (size + 4095) / 4096;
    }
    return ptr;
}

void nv_os_free_pages(void *cpu_addr, uint64_t dma_phys, size_t size) {
    if (!cpu_addr || size == 0) return;
    dma_free_coherent(NULL, size, cpu_addr, (dma_addr_t)dma_phys);
    size_t pags = (size + 4095) / 4096;
    if (g_nv_paginas_dma >= pags) {
        g_nv_paginas_dma -= pags;
    }
}

void *nv_os_map_mmio(uint64_t phys_addr, size_t size) {
    void *ptr = ioremap_nocache(phys_addr, size);
    if (ptr) {
        g_nv_mapeos_mmio++;
    }
    return ptr;
}

void nv_os_unmap_mmio(void *virt_addr) {
    if (!virt_addr) return;
    iounmap(virt_addr);
    if (g_nv_mapeos_mmio > 0) {
        g_nv_mapeos_mmio--;
    }
}

int nv_os_read_pci_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t *val) {
    if (!val) return -1;
    *val = pci_leer_config_32(bus, slot, func, offset);
    return 0;
}

int nv_os_write_pci_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val) {
    pci_escribir_config_32(bus, slot, func, offset, val);
    return 0;
}

void nv_os_delay_us(uint32_t us) {
    uint32_t ms = (us + 999) / 1000;
    if (ms == 0) ms = 1;
    esperar_milisegundos(ms);
}

void nv_os_delay_ms(uint32_t ms) {
    esperar_milisegundos(ms);
}

void nv_os_spinlock_init(nv_spinlock_t *lock) {
    if (lock) lock->lock = 0;
}

void nv_os_spinlock_acquire(nv_spinlock_t *lock) {
    if (!lock) return;
    while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
}

void nv_os_spinlock_release(nv_spinlock_t *lock) {
    if (!lock) return;
    __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
}

void nv_os_log(const char *msg) {
    if (!msg) return;
    serial_imprimir("[NV-OS-IF] ");
    serial_imprimir_linea(msg);
    pr_info("%s", msg);
}

void nv_os_obtener_metricas(uint64_t *paginas_dma_activas, uint64_t *mapeos_mmio_activos) {
    if (paginas_dma_activas) *paginas_dma_activas = g_nv_paginas_dma;
    if (mapeos_mmio_activos) *mapeos_mmio_activos = g_nv_mapeos_mmio;
}
