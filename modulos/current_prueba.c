#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_current.h"
/* Ensayo de accesores Linux originales sobre tres fixtures, sin crear tareas
 * Linux ni invocar el slowpath de mutex. No toca PCI/MMIO/DMA. */
static const struct taek_kernel_api *k;
static uint64_t handles[2],tp[2],raiz,mask;
static unsigned ciclos[2],armado;
static unsigned long estados[3];
struct observador {unsigned visitas;void *raw;};
static struct observador *obs;
static void *reserva;
static struct observador candidato;
static uint64_t sentinel;
static uint8_t patron[2][16] __attribute__((aligned(16)))={
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
    {31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46}};
static uint64_t flags(void){uint64_t r;__asm__ volatile("pushfq; pop %0":"=r"(r));return r;}
static void irq(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static uint64_t fs(void){uint32_t a,d;__asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(0xc0000100));return ((uint64_t)d<<32)|a;}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void vacio(void *arg){(void)arg;}
static void observar(uint64_t h,void *arg){
    struct observador *o=arg;unsigned n=0;
    if(h){if(h==handles[0])n=1;else if(h==handles[1])n=2;else mask|=1;}
    if(flags()&512)mask|=2;
    if(taek_current_fixture_seleccionar(n) || taek_current_fixture_verificar(n,1000+(int)n,estados[n]))mask|=4;
    sentinel=UINT64_C(0x123456789abcdef0);
    if(k->fibra_actual() || k->fibra_crear(vacio,0,&sentinel)>=0 || sentinel!=UINT64_C(0x123456789abcdef0) ||
       k->fibra_ejecutar(0)>=0 || k->fibra_yield()>=0 || k->fibra_bloquear()>=0 ||
       k->fibra_despertar(h)>=0 || k->fibra_liberar(h)>=0 || k->fibra_observar(0,0)>=0 ||
       k->asignar(1) || k->liberar(o->raw)>=0)mask|=8;
    o->visitas++;
    /* Adversario FX: el motor debe proteger al emisor y al destino. */
    __asm__ volatile("pxor %%xmm15,%%xmm15":::"memory");
}
static int identidad(unsigned n){return !taek_current_fixture_verificar(n+1,1001+(int)n,estados[n+1]);}
static void hijo(void *arg){
    unsigned n=arg==&ciclos[1];uint64_t self;
    tp[n]=fs();__asm__ volatile("movq %%fs:0,%0":"=r"(self));
    if(!tp[n] || self!=tp[n] || tp[n]==raiz || !(flags()&512) || !identidad(n))mask|=16;
    if(n==0){if(!taek_current_fixture_trylock() || taek_current_fixture_owner()!=taek_current_fixture_identidad(1))mask|=32;}
    else if(taek_current_fixture_trylock() || taek_current_fixture_unlock())mask|=64;
    uint16_t cw=n?0x37f:0x27f;uint32_t mx=n?0x3f80:0x1f80;
    __asm__ volatile("fninit; fldcw %0; ldmxcsr %1; movdqu %2,%%xmm15"::"m"(cw),"m"(mx),"m"(patron[n]):"memory");
    for(unsigned i=0;i<64;i++){
        uint64_t esperado=(i&1)?512:0;irq(esperado);
        if(k->fibra_yield())mask|=128;
        uint8_t fx[512] __attribute__((aligned(16)));__asm__ volatile("fxsave64 %0":"=m"(fx)::"memory");
        if(((uint16_t)fx[0]|((uint16_t)fx[1]<<8))!=cw ||
           ((uint32_t)fx[24]|((uint32_t)fx[25]<<8)|((uint32_t)fx[26]<<16)|((uint32_t)fx[27]<<24))!=mx)mask|=256;
        for(unsigned j=0;j<16;j++)if(fx[160+15*16+j]!=patron[n][j])mask|=256;
        if((flags()&512)!=esperado || fs()!=tp[n] || k->fibra_actual()!=handles[n] || !identidad(n))mask|=512;
        ciclos[n]++;
    }
    if(n==0){
        if(taek_current_fixture_estado(1))mask|=1024;estados[1]=1;
        if(!identidad(0) || k->fibra_bloquear() || !identidad(0))mask|=1024;
        uint8_t fx[512] __attribute__((aligned(16)));__asm__ volatile("fxsave64 %0":"=m"(fx)::"memory");
        if(!(flags()&512) || fs()!=tp[0] || k->fibra_actual()!=handles[0] ||
           ((uint16_t)fx[0]|((uint16_t)fx[1]<<8))!=cw ||
           ((uint32_t)fx[24]|((uint32_t)fx[25]<<8)|((uint32_t)fx[26]<<16)|((uint32_t)fx[27]<<24))!=mx)mask|=524288;
        for(unsigned j=0;j<16;j++)if(fx[160+15*16+j]!=patron[0][j])mask|=524288;
        if(taek_current_fixture_estado(0))mask|=1024;estados[1]=0;
        if(!taek_current_fixture_unlock())mask|=2048;
    }else{
        /* Espera cooperativa del ensayo; no sustituye mutex_lock Linux. */
        while(!taek_current_fixture_trylock())if(k->fibra_yield() || !identidad(1)){mask|=4096;break;}
        if(taek_current_fixture_owner()!=taek_current_fixture_identidad(2) || !taek_current_fixture_unlock())mask|=4096;
    }
    irq(512);
}
static void espera(void *arg){(void)arg;if(k->fibra_bloquear())mask|=8192;armado=0;}
static int adjuntar(void){
    void *viejo=k->asignar(64);reserva=k->asignar(64);
    if(!viejo || !reserva)return -1;
    struct observador *anterior=(struct observador *)((uint8_t *)viejo+16);
    obs=(struct observador *)((uint8_t *)reserva+16);anterior->visitas=obs->visitas=0;
    anterior->raw=viejo;obs->raw=reserva;
    if(k->fibra_observar(observar,anterior) || k->liberar(viejo)!=-1 ||
       k->fibra_observar(observar,obs) || k->liberar(viejo) || k->liberar(reserva)!=-1)mask|=16384;
    return 0;
}
static int separar(void){
    if(k->fibra_observar(0,0) || taek_current_fixture_verificar(0,1000,0))return -1;
    int r=k->liberar(reserva);if(!r){reserva=0;obs=0;}return r;
}
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_OBSERVADOR_BYTES)return -1;
    k=api;return taek_current_fixture_iniciar();
}
static int comando(const char *arg){
    if(igual(arg,"probar")){
        if(armado || reserva)return -1;
        mask=0;ciclos[0]=ciclos[1]=0;estados[0]=estados[1]=estados[2]=0;raiz=fs();
        if(taek_current_fixture_task_bytes()!=6208 || taek_current_fixture_task_align()!=64 || adjuntar())return -2;
        if(k->fibra_crear(hijo,&ciclos[0],&handles[0]) || k->fibra_crear(hijo,&ciclos[1],&handles[1]))return -3;
        uint64_t previo=flags();irq(0);
        if(k->fibra_ejecutar(130)!=130 || (flags()&512) || fs()!=raiz || k->fibra_actual() ||
           taek_current_fixture_verificar(0,1000,0) || !taek_current_fixture_locked() ||
           taek_current_fixture_unlock() || taek_current_fixture_owner()!=taek_current_fixture_identidad(1))mask|=32768;
        if(k->fibra_estado(handles[0])!=4 || k->fibra_estado(handles[1])!=2 ||
           k->fibra_observar(observar,&candidato)>=0 || k->liberar(reserva)!=-1)mask|=65536;
        unsigned visitas=obs->visitas;
        if(k->fibra_despertar(handles[0]))mask|=131072;
        irq(512);
        if(k->fibra_ejecutar(2)!=2 || !(flags()&512) || fs()!=raiz ||
           taek_current_fixture_verificar(0,1000,0) || taek_current_fixture_locked() || obs->visitas!=visitas+4)mask|=131072;
        irq(previo);
        if(ciclos[0]!=64 || ciclos[1]!=64 || tp[0]==tp[1] ||
           k->fibra_liberar(handles[0]) || k->fibra_liberar(handles[1]) || separar())mask|=262144;
        k->imprimir("[CURRENT] RESULTADO=");k->imprimir(mask?"FAIL":"OK");
        k->imprimir(" ciclos=");k->imprimir_dec(ciclos[0]+ciclos[1]);k->imprimir(" mask=");k->imprimir_dec(mask);
        k->imprimir_linea(" current/PID/state/mutex-fast TLS/IF/FX pin");return mask?-4:0;
    }
    if(igual(arg,"armar")){
        if(armado || reserva)return -1;mask=0;estados[0]=estados[1]=estados[2]=0;armado=1;
        if(adjuntar() || k->fibra_crear(espera,0,&handles[0]) || k->fibra_ejecutar(1)!=1)return -2;
        if(k->fibra_observar(observar,&candidato)>=0 || k->liberar(reserva)!=-1 || mask)return -3;
        k->imprimir_linea("[CURRENT] Suspendida; observer y ctx retenidos.");return 0;
    }
    if(igual(arg,"reanudar")){
        if(!armado || !reserva || k->liberar(reserva)!=-1 || k->fibra_despertar(handles[0]) ||
           k->fibra_ejecutar(1)!=1 || armado || k->fibra_liberar(handles[0]) || separar() || mask)return -1;
        k->imprimir_linea("[CURRENT] Reanudada; observer separado y ctx liberado.");return 0;
    }
    return -1;
}
static int detener(void){return 0; /* Adversario del cierre: el motor debe retener. */}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
    TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"current-prueba",iniciar,comando,detener};
