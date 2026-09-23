#ifndef COMPATIBILIDAD_NV_OS_INTERFACE_H
#define COMPATIBILIDAD_NV_OS_INTERFACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// TAEK OS - CAPA OS-INTERFACE PARA NVIDIA CORE (DESACOPLAMIENTO TOTAL)
// ============================================================================

typedef struct {
    volatile int lock;
} nv_spinlock_t;

void *nv_os_alloc_pages(size_t size, uint64_t *dma_phys);
void  nv_os_free_pages(void *cpu_addr, uint64_t dma_phys, size_t size);

void *nv_os_map_mmio(uint64_t phys_addr, size_t size);
void  nv_os_unmap_mmio(void *virt_addr);

int   nv_os_read_pci_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t *val);
int   nv_os_write_pci_32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val);

void  nv_os_delay_us(uint32_t us);
void  nv_os_delay_ms(uint32_t ms);

void  nv_os_spinlock_init(nv_spinlock_t *lock);
void  nv_os_spinlock_acquire(nv_spinlock_t *lock);
void  nv_os_spinlock_release(nv_spinlock_t *lock);

void  nv_os_log(const char *msg);
void  nv_os_obtener_metricas(uint64_t *paginas_dma_activas, uint64_t *mapeos_mmio_activos);

#endif // COMPATIBILIDAD_NV_OS_INTERFACE_H
