#include "../nucleo/controladores/driver_abi.h"
/* Modulo de ensayo Ring 0: solo CPU/heap, sin PCI/MMIO/DMA. */
static const struct taek_kernel_api *k;
static uint64_t handles[2],tp[2],raiz;
static unsigned ciclos[2],mask,armado;
static const uint64_t solo_lectura=UINT64_C(0x1122334455667788);
static uint8_t patron[2][16] __attribute__((aligned(16)))={
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
    {31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46}};
static uint64_t flags(void){uint64_t r;__asm__ volatile("pushfq; pop %0":"=r"(r));return r;}
static void irq(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static uint64_t fs(void){uint32_t a,d;__asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(0xc0000100));return ((uint64_t)d<<32)|a;}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void hijo(void *arg){
    unsigned n=arg==&ciclos[1];uint64_t self;
    tp[n]=fs();__asm__ volatile("movq %%fs:0,%0":"=r"(self));
    if(!tp[n] || self!=tp[n] || tp[n]==raiz || !(flags()&512))mask|=1;
    if(k->fibra_actual()!=handles[n])mask|=2;
    /* El modulo no puede liberar la reserva privada de TLS. */
    void *raw=((void **)(uintptr_t)tp[n])[1];
    if(k->liberar(raw)!=-1)mask|=4;
    if(k->fibra_ejecutar(1)>=0 || k->fibra_liberar(handles[n])>=0)mask|=8;
    uint16_t cw=n?0x37f:0x27f;uint32_t mx=n?0x3f80:0x1f80;
    __asm__ volatile("fninit; fldcw %0; ldmxcsr %1; movdqu %2,%%xmm15"::"m"(cw),"m"(mx),"m"(patron[n]):"memory");
    for(unsigned i=0;i<64;i++){
        uint64_t esperado=(i&1)?512:0;irq(esperado);
        if(k->fibra_yield())mask|=16;
        uint8_t fx[512] __attribute__((aligned(16)));
        __asm__ volatile("fxsave64 %0":"=m"(fx)::"memory");
        uint16_t gotcw=(uint16_t)fx[0]|((uint16_t)fx[1]<<8);
        uint32_t gotmx=(uint32_t)fx[24]|((uint32_t)fx[25]<<8)|((uint32_t)fx[26]<<16)|((uint32_t)fx[27]<<24);
        if(gotcw!=cw || gotmx!=mx)mask|=32;
        for(unsigned j=0;j<16;j++)if(fx[160+15*16+j]!=patron[n][j])mask|=32;
        if((flags()&512)!=esperado || fs()!=tp[n] || k->fibra_actual()!=handles[n])mask|=64;
        ciclos[n]++;
    }
    irq(512);
}
static void espera(void *arg){(void)arg;if(k->fibra_bloquear())mask|=128;armado=0;}
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_FIBRAS_BYTES)return -1;
    k=api;return 0;
}
static int comando(const char *arg){
    if(igual(arg,"probar")){
        if(armado)return -1;mask=0;ciclos[0]=ciclos[1]=0;tp[0]=tp[1]=0;raiz=fs();
        if(k->fibra_crear(hijo,&ciclos[0],(uint64_t *)(uintptr_t)&solo_lectura)>=0 ||
           solo_lectura!=UINT64_C(0x1122334455667788))mask|=2048;
        if(k->fibra_crear(hijo,&ciclos[0],&handles[0]) || k->fibra_crear(hijo,&ciclos[1],&handles[1]))return -2;
        uint64_t previo=flags();irq(0);
        int r=k->fibra_ejecutar(130);
        if(r!=130 || (flags()&512) || fs()!=raiz || k->fibra_actual())mask|=256;
        irq(previo);
        if(ciclos[0]!=64 || ciclos[1]!=64 || tp[0]==tp[1])mask|=512;
        uint64_t viejo=handles[0];
        if(k->fibra_liberar(handles[0]) || k->fibra_liberar(handles[1]) || k->fibra_despertar(viejo)>=0)mask|=1024;
        k->imprimir("[FIBRAS] RESULTADO=");k->imprimir(mask?"FAIL":"OK");
        k->imprimir(" ciclos=");k->imprimir_dec(ciclos[0]+ciclos[1]);
        k->imprimir(" mask=");k->imprimir_dec(mask);k->imprimir_linea(" TLS/IF/FX");
        return mask?-3:0;
    }
    if(igual(arg,"armar")){
        if(armado)return -1;mask=0;armado=1;
        if(k->fibra_crear(espera,0,&handles[0]) || k->fibra_ejecutar(1)!=1)return -2;
        k->imprimir_linea("[FIBRAS] Suspendida; detener devolvera cero para probar retencion residente.");return 0;
    }
    if(igual(arg,"reanudar")){
        if(!armado || k->fibra_despertar(handles[0]) || k->fibra_ejecutar(1)!=1 || armado || k->fibra_liberar(handles[0]))return -1;
        k->imprimir_linea("[FIBRAS] Reanudada y liberada.");return 0;
    }
    return -1;
}
static int detener(void){return 0; /* Adversario: no basta confiar en este cero. */}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
    TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"fibras-prueba",iniciar,comando,detener};
