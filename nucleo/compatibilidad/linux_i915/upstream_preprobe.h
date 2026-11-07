#ifndef TAEK_UPSTREAM_PREPROBE_H
#define TAEK_UPSTREAM_PREPROBE_H
/* Frontera opaca; los layouts PCI/DRM/i915 se compilan con cabeceras Linux. */
struct taek_upstream_preprobe_resultado {
    unsigned checks,fallos,admitido;
    unsigned plataforma,graphics,media,display,gt,dma_bits,ppgtt_bits,ppgtt_tipo;
    unsigned paginas,motores,logical_contexts,llc,gmd_id,ult;
    unsigned long long privado_bytes,pci_bytes;
    char nombre[32];
};
void taek_preprobe_frontera_reiniciar(void);
unsigned taek_preprobe_frontera_fallos(void);
int taek_upstream_preprobe(unsigned vendor,unsigned device,unsigned revision,
    unsigned subvendor,unsigned subdevice,struct taek_upstream_preprobe_resultado *out);
#endif
