/* Perfil exclusivo de preprobe 5917. MMIO/log de error no está admitido.
 * Las funciones son fronteras de rechazo instrumentadas: cualquier llamada
 * invalida el resultado de la etapa. No son servicios MMIO para i915 activo. */
#include <linux/pci.h>
#include <drm/drm_print.h>
#include "../upstream_preprobe.h"
extern void taek_preprobe_rechazado(const char *operacion);
static unsigned rechazados;
static void rechazar(const char *op){rechazados++;taek_preprobe_rechazado(op);}
void taek_preprobe_frontera_reiniciar(void){rechazados=0;}
unsigned taek_preprobe_frontera_fallos(void){return rechazados;}
void __iomem *pci_iomap_range(struct pci_dev *dev,int bar,unsigned long off,unsigned long maxlen){
    (void)dev;(void)bar;(void)off;(void)maxlen;rechazar("pci_iomap_range");return NULL;
}
void pci_iounmap(struct pci_dev *dev,void __iomem *addr){
    (void)dev;(void)addr;rechazar("pci_iounmap");
}
unsigned int ioread32(const void __iomem *addr){(void)addr;rechazar("ioread32");return ~0u;}
const char *dev_driver_string(const struct device *dev){
    (void)dev;rechazar("dev_driver_string");return "preprobe-unbound";
}
void _dev_err(const struct device *dev,const char *fmt,...){
    (void)dev;(void)fmt;rechazar("_dev_err");
}
void __drm_dev_dbg(struct _ddebug *desc,const struct device *dev,enum drm_debug_category category,const char *fmt,...){
    (void)desc;(void)dev;(void)category;(void)fmt;rechazar("__drm_dev_dbg");
}
