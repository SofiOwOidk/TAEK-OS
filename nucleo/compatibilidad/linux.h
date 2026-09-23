#ifndef COMPATIBILIDAD_LINUX_H
#define COMPATIBILIDAD_LINUX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "../base/memoria.h"

// ============================================================================
// TAEK OS - CAPA DE COMPATIBILIDAD LINUX KERNEL SHIM (Ring 0)
// Emulación y adaptación directa de las interfaces del núcleo de Linux
// para habilitar controladores de video (NVIDIA Open, VirtIO, DRM).
// ============================================================================

#define LINUX_SHIM_IOREMAP_BASE 0xFFFFFD0000000000ULL

// Tipos enteros de Linux
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;

typedef int8_t    s8;
typedef int16_t   s16;
typedef int32_t   s32;
typedef int64_t   s64;

typedef uint64_t  dma_addr_t;
typedef uint64_t  phys_addr_t;
typedef uint64_t  resource_size_t;
typedef int64_t   ssize_t;

// Códigos de error estándar (errno.h)
#define EPERM        1
#define ENOENT       2
#define EIO          5
#define ENXIO        6
#define ENOMEM      12
#define EACCES      13
#define EFAULT      14
#define EBUSY       16
#define ENODEV      19
#define EINVAL      22
#define ENOSPC      28
#define ETIMEDOUT  110

// Niveles de registro printk
#define KERN_EMERG   "<0>"
#define KERN_ALERT   "<1>"
#define KERN_CRIT    "<2>"
#define KERN_ERR     "<3>"
#define KERN_WARNING "<4>"
#define KERN_NOTICE  "<5>"
#define KERN_INFO    "<6>"
#define KERN_DEBUG   "<7>"

// Spinlocks y Atomics
typedef struct {
    volatile int bloqueado;
} spinlock_t;

#define SPIN_LOCK_UNLOCKED (spinlock_t){ .bloqueado = 0 }
#define DEFINE_SPINLOCK(x) spinlock_t x = SPIN_LOCK_UNLOCKED

static inline void spin_lock_init(spinlock_t *lock) {
    if (lock) lock->bloqueado = 0;
}

static inline void spin_lock(spinlock_t *lock) {
    if (!lock) return;
    while (__atomic_test_and_set(&lock->bloqueado, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
}

static inline void spin_unlock(spinlock_t *lock) {
    if (!lock) return;
    __atomic_clear(&lock->bloqueado, __ATOMIC_RELEASE);
}

static inline unsigned long spin_lock_irqsave(spinlock_t *lock, unsigned long flags) {
    (void)flags;
    spin_lock(lock);
    return 0;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    (void)flags;
    spin_unlock(lock);
}

typedef struct {
    volatile int contador;
} atomic_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v) {
    return __atomic_load_n(&v->contador, __ATOMIC_RELAXED);
}

static inline void atomic_set(atomic_t *v, int i) {
    __atomic_store_n(&v->contador, i, __ATOMIC_RELAXED);
}

static inline void atomic_inc(atomic_t *v) {
    __atomic_fetch_add(&v->contador, 1, __ATOMIC_SEQ_CST);
}

static inline void atomic_dec(atomic_t *v) {
    __atomic_fetch_sub(&v->contador, 1, __ATOMIC_SEQ_CST);
}

// Barreras de memoria
static inline void mb(void)      { __asm__ volatile ("mfence" ::: "memory"); }
static inline void rmb(void)     { __asm__ volatile ("lfence" ::: "memory"); }
static inline void wmb(void)     { __asm__ volatile ("sfence" ::: "memory"); }
static inline void barrier(void) { __asm__ volatile ("" ::: "memory"); }

// Accesos MMIO
static inline u8  readb(const volatile void *addr) { return *(const volatile u8 *)addr; }
static inline u16 readw(const volatile void *addr) { return *(const volatile u16 *)addr; }
static inline u32 readl(const volatile void *addr) { return *(const volatile u32 *)addr; }
static inline u64 readq(const volatile void *addr) { return *(const volatile u64 *)addr; }

static inline void writeb(u8 val, volatile void *addr)  { *(volatile u8 *)addr = val; }
static inline void writew(u16 val, volatile void *addr) { *(volatile u16 *)addr = val; }
static inline void writel(u32 val, volatile void *addr) { *(volatile u32 *)addr = val; }
static inline void writeq(u64 val, volatile void *addr) { *(volatile u64 *)addr = val; }

// Recursos PCI
#define IORESOURCE_IO         0x00000100
#define IORESOURCE_MEM        0x00000200
#define IORESOURCE_PREFETCH   0x00002000

struct resource {
    resource_size_t start;
    resource_size_t end;
    const char     *name;
    unsigned long   flags;
};

struct pci_dev {
    unsigned int    bus;
    unsigned int    slot;
    unsigned int    func;
    unsigned int    devfn;
    unsigned short  vendor;
    unsigned short  device;
    unsigned short  subsystem_vendor;
    unsigned short  subsystem_device;
    unsigned int    class;
    unsigned int    irq;
    struct resource resource[6];
    void           *sysdata;
};

// Funciones del Linux Shim
void *dma_alloc_coherent(void *dev, size_t size, dma_addr_t *dma_handle, unsigned int flag);
void  dma_free_coherent(void *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle);

void *ioremap(phys_addr_t offset, size_t size);
void *ioremap_nocache(phys_addr_t offset, size_t size);
void *ioremap_wc(phys_addr_t offset, size_t size);
void  iounmap(void *addr);

struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from);
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from);
int  pci_enable_device(struct pci_dev *dev);
void pci_set_master(struct pci_dev *dev);

int  pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
int  pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
int  pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
int  pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
int  pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
int  pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);

resource_size_t pci_resource_start(struct pci_dev *dev, int bar);
resource_size_t pci_resource_len(struct pci_dev *dev, int bar);
unsigned long   pci_resource_flags(struct pci_dev *dev, int bar);

int printk(const char *fmt, ...);
#define pr_info(fmt, ...) printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)  printk(KERN_ERR fmt, ##__VA_ARGS__)

int  linux_shim_iniciar(void);
int  linux_shim_ejecutar_autodiagnostico(void);
void linux_shim_obtener_estadisticas(uint64_t *mem_dma_asignada,
                                     uint64_t *mapeos_ioremap,
                                     uint32_t *dispositivos_pci);

#endif // COMPATIBILIDAD_LINUX_H
