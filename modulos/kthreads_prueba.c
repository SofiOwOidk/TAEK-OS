#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_tasks.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_kthreads.h"
extern int taek_kthread_fixture_probar(void);
extern int taek_kthread_fixture_retirar(void);
extern int taek_kthread_fixture_reap_en_join(void);
static const struct taek_kernel_api *k;
static unsigned tasks,waits,threads,observing,bound,done,visits;
static uint64_t h,id,mask,root_fs,child_fs;
static int result;
static uint64_t flags(void){uint64_t f;__asm__ volatile("pushfq; pop %0":"=r"(f)::"memory");return f;}
static void irq(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static uint64_t fs(void){uint32_t a,d;__asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(0xc0000100));return ((uint64_t)d<<32)|a;}
static void *alloc(unsigned long n){return k->asignar(n);}
static int release(void *p){return k->liberar(p);}
static int block(void){return k->fibra_bloquear();}
static int yield(void){return k->fibra_yield();}
static int wake(unsigned long v){return k->fibra_despertar(v);}
static unsigned long actual(void){return k->fibra_actual();}
static int state(unsigned long v){return k->fibra_estado(v);}
static int context(void){return k->fibra_contexto();}
static unsigned long clock_ms(void){return k->obtener_ms();}
static void message(const char *s){k->imprimir("[KTHREAD WARN] ");k->imprimir_linea(s);mask|=1;}
static void warning(const char *s,unsigned l,unsigned f){(void)s;(void)l;(void)f;mask|=2;}
static void fatal(int n){k->imprimir("[KTHREAD FATAL] ");k->imprimir_dec((unsigned)n);k->imprimir_linea(" backing conservado");for(;;)__asm__ volatile("cli; hlt");}
static int execute(int n){
 if(taek_kthreads_servir()<0)fatal(201);int rc=k->fibra_ejecutar(n);
 if(taek_kthreads_servir()<0)fatal(202);
 if(taek_kthread_fixture_reap_en_join())fatal(203);return rc;
}
static int create(void (*fn)(void *),void *ctx,unsigned long *out){return k->fibra_crear(fn,ctx,out);}
static int retire(unsigned long fh){return k->fibra_liberar(fh);}
static const struct taek_tasks_ops task_ops={sizeof(task_ops),TAEK_TASKS_VERSION,alloc,release,block,yield,wake,actual,state,context,message,warning,fatal};
static const struct taek_tasks_wait_ops wait_ops={sizeof(wait_ops),TAEK_TASKS_WAIT_VERSION,clock_ms,execute};
static const struct taek_kthreads_ops thread_ops={sizeof(thread_ops),TAEK_KTHREADS_VERSION,alloc,release,create,retire,state,fatal};
static void observer(uint64_t dest,void *arg){
 if(arg!=&visits || context()!=2 || (flags()&512) || taek_tasks_seleccionar(dest))mask|=4;
 visits++;__asm__ volatile("pxor %%xmm15,%%xmm15":::"memory");
}
static void supervisor(void *arg){
 if(arg!=&result || taek_tasks_actual()!=id || actual()!=h || !(flags()&512))mask|=8;
 child_fs=fs();uint64_t self;__asm__ volatile("movq %%fs:0,%0":"=r"(self));
 if(!child_fs || child_fs==root_fs || child_fs!=self)mask|=16;
 uint16_t cw=0x27f;uint32_t mx=0x3f80;unsigned char pattern[16] __attribute__((aligned(16)))={17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
 __asm__ volatile("fninit; fldcw %0; ldmxcsr %1; movdqu %2,%%xmm15"::"m"(cw),"m"(mx),"m"(pattern):"memory");
 result=taek_kthread_fixture_probar();
 unsigned char fx[512] __attribute__((aligned(16)));__asm__ volatile("fxsave64 %0":"=m"(fx)::"memory");
 if(((unsigned)fx[0]|((unsigned)fx[1]<<8))!=cw || ((unsigned)fx[24]|((unsigned)fx[25]<<8)|((unsigned)fx[26]<<16)|((unsigned)fx[27]<<24))!=mx)mask|=32;
 for(unsigned j=0;j<16;j++)if(fx[400+j]!=pattern[j])mask|=32;
 if(taek_tasks_actual()!=id || actual()!=h || fs()!=child_fs || !(flags()&512))mask|=64;
 done=1;
}
static int retire_supervisor(void){
 if(h){if(state(h)!=-9 && k->fibra_liberar(h))return -1;
  if(bound){if(taek_tasks_desvincular(id))return -1;bound=0;}h=0;}
 if(id){if(taek_tasks_destruir(id))return -1;id=0;}return 0;
}
static int init(const struct taek_kernel_api *api){
 if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_CONTEXTO_BYTES)return -1;
 k=api;if(context()!=1 || taek_tasks_iniciar(&task_ops))return -2;tasks=1;
 if(taek_tasks_configurar_esperas(&wait_ops))return -3;waits=1;
 if(taek_kthreads_iniciar(&thread_ops))return -4;threads=1;
 if(k->fibra_observar(observer,&visits))return -5;observing=1;return 0;
}
static int equal(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static int command(const char *arg){
 if(!threads || !equal(arg,"probar") || h || id)return -1;
 mask=0;done=0;result=-99;root_fs=fs();uint64_t previous=flags();irq(512);
 if(taek_tasks_crear(&id)){irq(previous);return -2;}
 int rc=k->fibra_crear(supervisor,&result,&h);if(!rc)rc=taek_tasks_vincular(id,h);
 if(rc){retire_supervisor();irq(previous);return -3;}bound=1;
 uint64_t begin=clock_ms();unsigned steps=0;
 while(state(h)!=5 && clock_ms()-begin<5000 && steps<10000){
  int n=taek_tasks_ejecutar(1);if(n<0){mask|=128;break;}steps+=(unsigned)n;
 }
 if(!done || state(h)!=5 || result)mask|=256;
 if(done && state(h)==5 && !result){if(taek_kthread_fixture_retirar() || retire_supervisor())mask|=512;}
 struct taek_kthreads_stats st;taek_kthreads_estadisticas(&st);
 if(st.vivos || st.pendientes || st.joins || st.retenidos || st.fallos || h || id || fs()!=root_fs || taek_tasks_actual())mask|=1024;
 irq(previous);if((flags()&512)!=(previous&512))mask|=2048;
 k->imprimir("[KTHREAD] RESULTADO=");k->imprimir(mask?"FAIL":"OK");k->imprimir(" mask=");k->imprimir_dec(mask);
 k->imprimir(" fixture=");k->imprimir_dec((uint64_t)(int64_t)result);k->imprimir(" steps=");k->imprimir_dec(steps);
 k->imprimir_linea(" original worker/queue/flush/stop; constructor/exit TaekOS current TLS/IF/FX");
 return mask?-4:0;
}
static int stop(void){
 if(threads){if(h && state(h)!=5 && state(h)!=1)return -1;
  if(taek_kthread_fixture_retirar())return -2;
  if(retire_supervisor() || taek_kthreads_parar())return -3;threads=0;}
 if(waits){if(taek_tasks_configurar_esperas(0))return -4;waits=0;}
 if(observing){if(k->fibra_observar(0,0))return -5;observing=0;}
 if(tasks){if(taek_tasks_parar())return -6;tasks=0;}return 0;
}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
 TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"kthreads-prueba",init,command,stop};
