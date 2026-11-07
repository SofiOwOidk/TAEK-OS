#include <linux/pci.h>
#include "i915_drv.h"
#include "intel_device_info.h"
#include "../upstream_preprobe.h"
extern const struct pci_device_id *taek_i915_match_original(struct pci_dev *pdev);
static struct pci_dev pci;
static struct drm_i915_private i915;
static void comprobar(struct taek_upstream_preprobe_resultado *r,int ok,unsigned bit){
    r->checks++;if(!ok)r->fallos|=bit;
}
int taek_upstream_preprobe(unsigned vendor,unsigned device,unsigned revision,
    unsigned subvendor,unsigned subdevice,struct taek_upstream_preprobe_resultado *r){
    if(!r)return -EINVAL;
    memset(r,0,sizeof(*r));
    taek_preprobe_frontera_reiniciar();
    if(vendor!=PCI_VENDOR_ID_INTEL || device!=0x5917 || revision>255 ||
       subvendor>65535 || subdevice>65535)return -ENODEV;
    memset(&pci,0,sizeof(pci));memset(&i915,0,sizeof(i915));
    pci.vendor=vendor;pci.device=device;pci.revision=revision;
    pci.subsystem_vendor=subvendor;pci.subsystem_device=subdevice;pci.class=PCI_CLASS_DISPLAY_VGA<<8;
    const struct pci_device_id *match=taek_i915_match_original(&pci);
    comprobar(r,match && match->driver_data,1);
    if(!match || !match->driver_data)return -ENODEV;
    const struct intel_device_info *info=(const struct intel_device_info *)match->driver_data;
    /* Entrada restringida a 5917 y tabla original. GMD_ID no se admite aquí. */
    comprobar(r,info->platform==INTEL_KABYLAKE && !info->has_gmd_id,2);
    if(info->platform!=INTEL_KABYLAKE || info->has_gmd_id)return -ENODEV;
    i915.drm.dev=&pci.dev;
    intel_device_info_driver_create(&i915,device,info);
    intel_device_info_runtime_init_early(&i915);
    const struct intel_runtime_info *runtime=RUNTIME_INFO(&i915);
    r->admitido=1;r->plataforma=info->platform;
    r->graphics=runtime->graphics.ip.ver;r->media=runtime->media.ip.ver;
    r->display=DISPLAY_RUNTIME_INFO(&i915)->ip.ver;r->gt=info->gt;
    r->dma_bits=info->dma_mask_size;r->ppgtt_bits=runtime->ppgtt_size;r->ppgtt_tipo=runtime->ppgtt_type;
    r->paginas=runtime->page_sizes;r->motores=info->platform_engine_mask;
    r->logical_contexts=info->has_logical_ring_contexts;r->llc=info->has_llc;
    r->gmd_id=info->has_gmd_id;r->ult=IS_KABYLAKE_ULT(&i915);
    r->privado_bytes=sizeof(i915);r->pci_bytes=sizeof(pci);
    const char *nombre=intel_platform_name(info->platform);
    for(unsigned j=0;j+1<sizeof(r->nombre) && nombre[j];j++)r->nombre[j]=nombre[j];
    comprobar(r,r->graphics==9 && r->media==9 && r->display==9,4);
    comprobar(r,r->gt==2 && r->dma_bits==39 && r->ppgtt_bits==48 && r->ppgtt_tipo==INTEL_PPGTT_FULL,8);
    comprobar(r,r->paginas==(I915_GTT_PAGE_SIZE_4K|I915_GTT_PAGE_SIZE_64K),16);
    comprobar(r,r->motores==(BIT(RCS0)|BIT(VCS0)|BIT(BCS0)|BIT(VECS0)),32);
    /* El PCI ID 5917 está en KBL GT2 genérico, no en la lista ULT fijada. */
    comprobar(r,r->logical_contexts && r->llc && !r->gmd_id && !r->ult,64);
    comprobar(r,HAS_DISPLAY(&i915) && DISPLAY_RUNTIME_INFO(&i915)->pipe_mask==7,128);
    comprobar(r,taek_preprobe_frontera_fallos()==0,256);
    return r->fallos?-EIO:0;
}
