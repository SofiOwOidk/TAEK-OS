#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_tasks.h"
/* Consumidores mutex/wake_q/refcount originales sobre tareas adaptadas TaekOS.
 * Ensayo CPU/heap, no copy_process/kthread ni PCI/MMIO/DMA/GPU. */
static const struct taek_kernel_api *k;
static unsigned activo,observando,armado,parcial,free_fail,ciclos[2],avisos,visitas;
static uint64_t h[2],tp[2],id[2],raiz,mask,sentinel;
static unsigned bound[2];
static unsigned char patron[2][16] __attribute__((aligned(16)))={
    {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
    {31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46}};
static uint64_t flags(void){uint64_t r;__asm__ volatile("pushfq; pop %0":"=r"(r));return r;}
static void irq(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static uint64_t fs(void){uint32_t a,d;__asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(0xc0000100));return ((uint64_t)d<<32)|a;}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void *alloc(unsigned long n){return k->asignar(n);}
static int release(void *p){if(free_fail){free_fail=0;return -1;}return k->liberar(p);}
static int block(void){return k->fibra_bloquear();}
static int yield(void){return k->fibra_yield();}
static int wake(unsigned long a){return k->fibra_despertar(a);}
static unsigned long actual(void){return k->fibra_actual();}
static int estado(unsigned long a){return k->fibra_estado(a);}
static int contexto(void){return k->fibra_contexto();}
static void mensaje(const char *s){k->imprimir("[TASKS WARN] ");k->imprimir_linea(s);}
static void warning(const char *s,unsigned line,unsigned f){
    avisos++;k->imprimir("[TASKS WARN SITE] ");k->imprimir(s);k->imprimir(" linea=");k->imprimir_dec(line);
    k->imprimir(" flags=");k->imprimir_dec(f);k->imprimir_linea("");
}
static void fatal(int n){
    k->imprimir("[TASKS FATAL] ");k->imprimir_dec((unsigned)n);k->imprimir_linea(" backing conservado; contrato invalido.");
    for(;;)__asm__ volatile("cli; hlt");
}
static const struct taek_tasks_ops ops={sizeof(ops),TAEK_TASKS_VERSION,alloc,release,
    block,yield,wake,actual,estado,contexto,mensaje,warning,fatal};
static void observer(uint64_t handle,void *arg){
    if(arg!=&visitas || k->fibra_contexto()!=2 || (flags()&512) || taek_tasks_seleccionar(handle))mask|=1;
    sentinel=UINT64_C(0x123456789abcdef0);
    if(taek_tasks_crear(&sentinel)>=0 || sentinel!=UINT64_C(0x123456789abcdef0) ||
       taek_tasks_mutex_lock()!=-4 || taek_tasks_mutex_unlock()!=-4 || taek_tasks_schedule()!=-4 ||
       taek_tasks_parar()>=0 || taek_tasks_wakeq(id[0],1)>=0)mask|=2;
    visitas++;__asm__ volatile("pxor %%xmm15,%%xmm15":::"memory");
}
static void fijar(unsigned n){
    uint16_t cw=n?0x37f:0x27f;uint32_t mx=n?0x3f80:0x1f80;
    __asm__ volatile("fninit; fldcw %0; ldmxcsr %1; movdqu %2,%%xmm15"::"m"(cw),"m"(mx),"m"(patron[n]):"memory");
}
static void verificar(unsigned n,uint64_t f){
    uint8_t fx[512] __attribute__((aligned(16)));__asm__ volatile("fxsave64 %0":"=m"(fx)::"memory");
    uint16_t cw=n?0x37f:0x27f;uint32_t mx=n?0x3f80:0x1f80;
    if(((uint16_t)fx[0]|((uint16_t)fx[1]<<8))!=cw ||
       ((uint32_t)fx[24]|((uint32_t)fx[25]<<8)|((uint32_t)fx[26]<<16)|((uint32_t)fx[27]<<24))!=mx)mask|=4;
    for(unsigned j=0;j<16;j++)if(fx[160+15*16+j]!=patron[n][j])mask|=4;
    if(fs()!=tp[n] || (flags()&512)!=(f&512) || k->fibra_actual()!=h[n] ||
       taek_tasks_actual()!=id[n] || taek_tasks_estado(id[n]) || k->fibra_contexto()!=1)mask|=8;
}
static void hijo(void *arg){
    unsigned n=arg==&ciclos[1];uint64_t self;tp[n]=fs();__asm__ volatile("movq %%fs:0,%0":"=r"(self));
    if(!tp[n] || tp[n]==raiz || self!=tp[n] || !(flags()&512) || taek_tasks_actual()!=id[n])mask|=16;
    fijar(n);
    for(unsigned i=0;i<64;i++){
        uint64_t f=(i&1)?512:0;irq(f);
        if(taek_tasks_mutex_lock())mask|=32;
        verificar(n,f);
        if(!n){if(taek_tasks_yield())mask|=64;verificar(n,f);}
        if(taek_tasks_mutex_unlock())mask|=128;
        ciclos[n]++;if(taek_tasks_yield())mask|=256;verificar(n,f);
    }
    if(n){
        if(taek_tasks_publicar_espera() || taek_tasks_wakeq(id[1],1) || taek_tasks_schedule())mask|=512;
        if(taek_tasks_publicar_espera() || taek_tasks_wakeq(id[1],1) || taek_tasks_publicar_espera())mask|=1024;
        fijar(1);if(taek_tasks_schedule())mask|=2048;verificar(1,512);
        if(taek_tasks_refs(id[1])!=1 || taek_tasks_publicar_espera() || taek_tasks_wakeq(id[1],1))mask|=4096;
    }
    irq(512);
}
static void espera(void *arg){
    (void)arg;if(taek_tasks_mutex_lock() || taek_tasks_publicar_espera() || taek_tasks_schedule() || taek_tasks_mutex_unlock())mask|=8192;
    armado=0;
}
static int retirar(unsigned n){
    /* Conservar cada etapa hasta confirmarla: liberar la fibra puede preceder
     * a un fallo al liberar task_struct; el retry no debe usar el handle viejo. */
    if(h[n]){
        if(k->fibra_estado(h[n])!=-9 && k->fibra_liberar(h[n]))return -1;
        if(bound[n]){if(taek_tasks_desvincular(id[n]))return -1;bound[n]=0;}
        h[n]=0;
    }
    if(bound[n])return -1;
    if(id[n]){if(taek_tasks_destruir(id[n]))return -1;id[n]=0;}
    return 0;
}
static int crear(unsigned n,void (*fn)(void *),void *arg){
    if(h[n] || id[n] || bound[n])return -1;
    if(taek_tasks_crear(&id[n]))return -1;
    if(k->fibra_crear(fn,arg,&h[n]) || taek_tasks_vincular(id[n],h[n])){
        retirar(n);return -1;
    }
    bound[n]=1;return 0;
}
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_CONTEXTO_BYTES)return -1;
    k=api;if(k->fibra_contexto()!=1 || taek_tasks_iniciar(&ops))return -2;activo=1;
    if(k->fibra_observar(observer,&visitas))return -3;observando=1;return 0;
}
static int comando(const char *arg){
    if(!activo || parcial || free_fail)return -1;
    if(igual(arg,"probar")){
        if(armado || h[0] || h[1] || id[0] || id[1])return -1;mask=0;ciclos[0]=ciclos[1]=0;tp[0]=tp[1]=0;raiz=fs();
        struct taek_tasks_stats antes,s;taek_tasks_stats(&antes);
        if(crear(0,hijo,&ciclos[0]) || crear(1,hijo,&ciclos[1])){retirar(0);retirar(1);return -2;}
        uint64_t previo=flags();irq(0);
        if(k->fibra_ejecutar(258)!=258 || (flags()&512) || fs()!=raiz || k->fibra_actual() ||
           taek_tasks_actual() || k->fibra_estado(h[0])!=5 || k->fibra_estado(h[1])!=4 || taek_tasks_estado(id[1])!=2)mask|=16384;
        taek_tasks_stats(&s);
        if(s.vivos!=3 || s.vinculados!=2 || s.referencias!=3 || s.esperando ||
           s.bloqueos-antes.bloqueos!=65 || s.prewakes-antes.prewakes!=2 || s.wakes-antes.wakes!=66 ||
           taek_tasks_mutex_locked() || taek_tasks_mutex_unlock()>=0 || taek_tasks_destruir(id[1])>=0)mask|=32768;
        if(taek_tasks_wakeq(id[1],1) || taek_tasks_wakeq(id[1],1))mask|=65536;
        irq(512);if(k->fibra_ejecutar(1)!=1 || !(flags()&512) || fs()!=raiz || taek_tasks_actual())mask|=131072;
        irq(previo);
        if(ciclos[0]!=64 || ciclos[1]!=64 || tp[0]==tp[1] || retirar(0) || retirar(1))mask|=262144;
        taek_tasks_stats(&s);if(s.vivos!=1 || s.referencias!=1 || s.vinculados || s.esperando || s.fallos || avisos)mask|=524288;
        k->imprimir("[TASKS] RESULTADO=");k->imprimir(mask?"FAIL":"OK");k->imprimir(" ciclos=");k->imprimir_dec(ciclos[0]+ciclos[1]);
        k->imprimir(" mask=");k->imprimir_dec(mask);k->imprimir_linea(" mutex-block/wakeq/refs current TLS/IF/FX");return mask?-4:0;
    }
    if(igual(arg,"armar")){
        if(armado || h[0] || h[1] || id[0] || id[1])return -1;mask=0;
        if(crear(0,espera,0))return -2;
        armado=1;if(k->fibra_ejecutar(1)!=1)return -2;
        k->imprimir_linea("[TASKS] Suspendida con mutex; task/stack/refs retenidos.");return mask?-3:0;
    }
    if(igual(arg,"reanudar")){
        if(!armado || taek_tasks_wakeq(id[0],1) || k->fibra_ejecutar(1)!=1 || armado || retirar(0) || mask)return -1;
        k->imprimir_linea("[TASKS] Reanudada; mutex/task/stack drenados.");return 0;
    }
    if(igual(arg,"free-fallo")){
        if(armado || h[0] || h[1] || id[0] || id[1])return -1;free_fail=1;
        k->imprimir_linea("[TASKS] Proxima liberacion de raiz fallara una vez.");return 0;
    }
    return -1;
}
static int detener(void){
    if(!activo)return 0;
    if(armado)return -1;
    for(unsigned n=0;n<2;n++)if(retirar(n))return -1;
    struct taek_tasks_stats s;taek_tasks_stats(&s);
    if(s.vinculados || s.esperando || s.vivos!=1)return -1;
    if(observando){if(k->fibra_observar(0,0))return -2;observando=0;}
    int rc=taek_tasks_parar();if(rc){parcial=1;k->imprimir_linea("[TASKS] Raiz retenida para retry; observer separado.");return rc;}
    activo=parcial=0;k->imprimir_linea("[TASKS] Tareas liberadas; callbacks separados.");return 0;
}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
    TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"tasks-prueba",iniciar,comando,detener};
