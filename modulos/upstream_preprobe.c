#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_preprobe.h"
static const struct taek_kernel_api *kernel;
static unsigned avisos,rondas,activo,selector_fallos;
void *memset(void *dest,int valor,size_t n){volatile unsigned char *p=dest;for(size_t i=0;i<n;i++)p[i]=(unsigned char)valor;return dest;}
void *memcpy(void *dest,const void *src,size_t n){volatile unsigned char *p=dest;const volatile unsigned char *s=src;for(size_t i=0;i<n;i++)p[i]=s[i];return dest;}
void __warn_printk(const char *fmt,...){kernel->imprimir("[PREPROBE WARN] ");kernel->imprimir_linea(fmt);}
void taek_linux_warn_site(const char *file,unsigned line,unsigned flags){
    avisos++;kernel->imprimir("[PREPROBE WARN SITE] ");kernel->imprimir(file);
    kernel->imprimir(" line=");kernel->imprimir_dec(line);kernel->imprimir(" flags=");kernel->imprimir_dec(flags);kernel->imprimir_linea("");
}
void taek_preprobe_rechazado(const char *op){kernel->imprimir("[PREPROBE RECHAZO] ");kernel->imprimir_linea(op);}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static unsigned long flags(void){unsigned long f;__asm__ volatile("pushfq; popq %0":"=r"(f)::"memory");return f;}
static unsigned in32(unsigned short port){unsigned value;__asm__ volatile("inl %1,%0":"=a"(value):"Nd"(port));return value;}
static void out32(unsigned short port,unsigned value){__asm__ volatile("outl %0,%1"::"a"(value),"Nd"(port):"memory");}
static unsigned pci_lectura(unsigned offset){
    /* Solo config read de 00:02.0. CF8 es selector, nunca escribir CFC.
     * El callback está serializado en BSP por ABI 1; preservar IF y selector. */
    unsigned long f=flags();__asm__ volatile("cli":::"memory");
    unsigned selector=in32(0xcf8);out32(0xcf8,0x80001000u|(offset&0xfcu));
    unsigned value=in32(0xcfc);out32(0xcf8,selector);
    if(in32(0xcf8)!=selector)selector_fallos++;
    if(f&0x200)__asm__ volatile("sti":::"memory");
    return value;
}
static int probar(unsigned vendor,unsigned device,unsigned rev,unsigned subvendor,unsigned subdevice){
    struct taek_upstream_preprobe_resultado r={0};unsigned antes=avisos;
    int code=taek_upstream_preprobe(vendor,device,rev,subvendor,subdevice,&r);
    unsigned frontera=taek_preprobe_frontera_fallos();
    if(r.checks!=9 || r.fallos || !r.admitido || avisos!=antes || frontera)code=-1;
    rondas++;
    kernel->imprimir("[PREPROBE RAM] RESULTADO=");kernel->imprimir(code?"FAIL":"OK");
    kernel->imprimir(" checks=");kernel->imprimir_dec(r.checks);kernel->imprimir(" mask=");kernel->imprimir_dec(r.fallos);
    kernel->imprimir(" frontera=");kernel->imprimir_dec(frontera);kernel->imprimir(" avisos=");kernel->imprimir_dec(avisos-antes);
    kernel->imprimir(" ronda=");kernel->imprimir_dec(rondas);kernel->imprimir_linea("");
    kernel->imprimir("[PREPROBE RAM] plataforma=");kernel->imprimir(r.nombre);
    kernel->imprimir(" graphics=");kernel->imprimir_dec(r.graphics);kernel->imprimir(" media=");kernel->imprimir_dec(r.media);
    kernel->imprimir(" display=");kernel->imprimir_dec(r.display);kernel->imprimir(" GT=");kernel->imprimir_dec(r.gt);
    kernel->imprimir(" DMA_bits=");kernel->imprimir_dec(r.dma_bits);kernel->imprimir(" PPGTT_bits=");kernel->imprimir_dec(r.ppgtt_bits);
    kernel->imprimir(" motores=");kernel->imprimir_dec(r.motores);kernel->imprimir_linea("");
    kernel->imprimir_linea("[PREPROBE RAM] Estructuras/ruta temprana originales; MMIO/DMA/submission/3D/video pendientes.");return code;
}
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_BASE_BYTES)return -1;
    kernel=api;activo=1;avisos=rondas=selector_fallos=0;
    kernel->imprimir_linea("[PREPROBE RAM] i915 PCI/display/device_info originales listos, sin init hardware.");return 0;
}
static int comando(const char *arg){
    if(!activo)return -1;
    if(igual(arg,"probar"))return probar(0x8086,0x5917,7,0x1028,0x0816);
    if(igual(arg,"capturar")){
        unsigned long f=flags();unsigned id=pci_lectura(0);
        if(id!=0x59178086u){kernel->imprimir_linea("[PREPROBE RAM] PCI 00:02.0 fuera del perfil 8086:5917; rechazado.");return -19;}
        unsigned cfg=pci_lectura(4),clase=pci_lectura(8),subs=pci_lectura(0x2c);
        unsigned bar0=pci_lectura(0x10),bar2=pci_lectura(0x18);
        int code=probar(id&0xffff,id>>16,clase&255,subs&0xffff,subs>>16);
        unsigned cfg_despues=pci_lectura(4);
        unsigned unchanged=(cfg&0xffff)==(cfg_despues&0xffff) && bar0==pci_lectura(0x10) && bar2==pci_lectura(0x18);
        unsigned if_igual=(f&0x200)==(flags()&0x200);
        kernel->imprimir("[PREPROBE PCI] config_igual=");kernel->imprimir_dec(unchanged);
        kernel->imprimir(" IF_igual=");kernel->imprimir_dec(if_igual);
        kernel->imprimir(" selector_fallos=");kernel->imprimir_dec(selector_fallos);kernel->imprimir_linea("");
        if(!unchanged || !if_igual || selector_fallos || (clase>>24)!=3)return -1;
        return code;
    }
    kernel->imprimir_linea("[PREPROBE RAM] Comandos: probar | capturar (PCI 00:02.0 solo lectura)");return 0;
}
static int detener(void){
    /* La etapa no registra dispositivos/callbacks, no mapea ni entrega memoria al GPU. */
    activo=0;if(kernel)kernel->imprimir_linea("[PREPROBE RAM] Detenido, sin recursos externos.");kernel=0;return 0;
}
const struct taek_driver taek_driver={TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,
    "upstream-preprobe",iniciar,comando,detener};
