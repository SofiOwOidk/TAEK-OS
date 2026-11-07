#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_tasks.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_kthreads.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_wq.h"
/* Workqueue original de Linux en RAM. Sus workers globales no tienen desmontaje en
 * Linux (system_wq vive para siempre), asi que este modulo rechaza la descarga: se
 * sale de el reiniciando, lo que el actualizador por LAN hace sin intervencion. */
extern int taek_kthreads_autorrecoger(void);
static const struct taek_kernel_api *k;
static unsigned listos;
static uint64_t h, id, mask;
static int result;
static volatile unsigned done;
static uint64_t flags(void){uint64_t f;__asm__ volatile("pushfq; pop %0":"=r"(f)::"memory");return f;}
static void irq(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static void *alloc(unsigned long n){return k->asignar(n);}
static int release(void *p){return k->liberar(p);}
static int block(void){return k->fibra_bloquear();}
static int yield(void){return k->fibra_yield();}
static int wake(unsigned long v){return k->fibra_despertar(v);}
static unsigned long actual(void){return k->fibra_actual();}
static int state(unsigned long v){return k->fibra_estado(v);}
static int context(void){return k->fibra_contexto();}
static unsigned long clock_ms(void){return k->obtener_ms();}
static void message(const char *s){k->imprimir("[WQ] ");k->imprimir_linea(s);}
static void warning(const char *s,unsigned l,unsigned f){(void)f;k->imprimir("[WQ WARN] ");k->imprimir(s);k->imprimir(":");k->imprimir_dec(l);k->imprimir_linea("");mask|=2;}
static void fatal(int n){k->imprimir("[WQ FATAL] ");k->imprimir_dec((unsigned)n);k->imprimir_linea(" backing conservado");for(;;)__asm__ volatile("cli; hlt");}
static void servir(void){
 if(taek_kthreads_servir()<0)fatal(201);
 if(taek_kthreads_autorrecoger()<0)fatal(202);
 if(taek_wq_bombear()<0)fatal(203);
}
static int execute(int n){servir();int rc=k->fibra_ejecutar(n);servir();return rc;}
static int create(void (*fn)(void *),void *ctx,unsigned long *out){return k->fibra_crear(fn,ctx,out);}
static int retire(unsigned long fh){return k->fibra_liberar(fh);}
static void observer(uint64_t dest,void *arg){(void)arg;if(taek_tasks_seleccionar(dest))mask|=4;}
static const struct taek_tasks_ops task_ops={sizeof(task_ops),TAEK_TASKS_VERSION,alloc,release,block,yield,wake,actual,state,context,message,warning,fatal};
static const struct taek_tasks_wait_ops wait_ops={sizeof(wait_ops),TAEK_TASKS_WAIT_VERSION,clock_ms,execute};
static const struct taek_kthreads_ops thread_ops={sizeof(thread_ops),TAEK_KTHREADS_VERSION,alloc,release,create,retire,state,fatal};
static const struct taek_wq_ops wq_ops={sizeof(wq_ops),TAEK_WQ_VERSION,alloc,release,clock_ms,message,fatal,0};
static void supervisor(void *arg){(void)arg;result=taek_wq_fixture_probar();done=1;}
static int retire_supervisor(void){
 if(h){if(state(h)!=-9 && k->fibra_liberar(h))return -1;if(taek_tasks_desvincular(id))return -1;h=0;}
 if(id){if(taek_tasks_destruir(id))return -1;id=0;}return 0;
}
static int init(const struct taek_kernel_api *api){
 if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_CONTEXTO_BYTES)return -1;
 k=api;if(context()!=1 || taek_tasks_iniciar(&task_ops))return -2;
 if(taek_tasks_configurar_esperas(&wait_ops))return -3;
 if(taek_kthreads_iniciar(&thread_ops))return -4;
 if(k->fibra_observar(observer,0))return -5;
 if(taek_wq_frontera_iniciar(&wq_ops))return -6;
 listos=1;return 0;
}
static int equal(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void dec(const char *n,uint64_t v){k->imprimir(" ");k->imprimir(n);k->imprimir("=");k->imprimir_dec(v);}
static int command(const char *arg){
 if(!listos || !equal(arg,"probar") || h || id)return -1;
 mask=0;done=0;result=-99;uint64_t previous=flags();irq(512);
 if(taek_tasks_crear(&id)){irq(previous);return -2;}
 int rc=k->fibra_crear(supervisor,0,&h);if(!rc)rc=taek_tasks_vincular(id,h);
 if(rc){retire_supervisor();irq(previous);return -3;}
 uint64_t begin=clock_ms();unsigned steps=0,sin_progreso=0;
 while(!(done && state(h)==5) && clock_ms()-begin<30000){
  int n=taek_tasks_ejecutar(1);if(n<0){mask|=128;break;}steps+=(unsigned)n;
  /* Sin ejecutables se espera al reloj, no se gira: los timers son los que despiertan. */
  if(!n){
   if(++sin_progreso>20000)break;
   uint64_t t=clock_ms();while(clock_ms()==t)__asm__ volatile("pause");
  }else sin_progreso=0;
 }
 uint64_t ms=clock_ms()-begin;
 if(!done || state(h)!=5 || result)mask|=256;
 if(done && state(h)==5 && retire_supervisor())mask|=512;
 irq(previous);
 struct taek_wq_resultado r;taek_wq_fixture_resultado(&r);
 struct taek_wq_stats s;taek_wq_estadisticas(&s);
 struct taek_kthreads_stats t;taek_kthreads_estadisticas(&t);
 k->imprimir("[WQ] RESULTADO=");k->imprimir(mask||r.mascara?"FAIL":"OK");
 dec("mask",mask);dec("fixture",r.mascara);dec("checks",r.checks);dec("rondas",r.rondas);dec("ejec",r.ejecuciones);
 dec("steps",steps);dec("ms",ms);k->imprimir_linea("");
 k->imprimir("[WQ] kthreads");dec("vivos",t.vivos);dec("creados",t.creados);dec("recogidos",t.recolectados);
 dec("heap_bloques",s.heap_bloques);dec("heap_bytes",s.heap_bytes);dec("heap_pico",s.heap_pico_bytes);
 dec("timers",s.timers_disparados);dec("timers_pend",s.timers_pendientes);dec("rcu",s.rcu_ejecutados);dec("rcu_pend",s.rcu_pendientes);k->imprimir_linea("");
 k->imprimir_linea("[WQ] workqueue/idr/xarray/wait originales; timers, RCU y slab TaekOS; sin GPU.");
 return mask||r.mascara?-4:0;
}
/* Los workers globales siguen vivos a proposito: la descarga se rechaza, sin simular. */
static int stop(void){return listos?-1:0;}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
 TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"workqueue-prueba",init,command,stop};
