#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_tasks.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_completion.h"
/* Ensayo BSP de consumidores completion/swait originales, no workers/GPU. */
static const struct taek_kernel_api *k;
static unsigned active,observing,events,wait_backend,armed,bound[2],which[2]={0,1},mode,visits,warnings;
static uint64_t h[2],id[2],root_fs,child_fs[2],mask;
static long results[2];
static unsigned wait_order[2],wait_sequence;
static unsigned char patterns[2][16] __attribute__((aligned(16)))={
 {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16},
 {31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46}};
static uint64_t flags(void){uint64_t r;__asm__ volatile("pushfq; pop %0":"=r"(r)::"memory");return r;}
static void irq(uint64_t f){if(f&512)__asm__ volatile("sti":::"memory");else __asm__ volatile("cli":::"memory");}
static uint64_t fs(void){uint32_t a,d;__asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(0xc0000100));return ((uint64_t)d<<32)|a;}
static int equal(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void *alloc(unsigned long n){return k->asignar(n);}
static int release(void *p){return k->liberar(p);}
static int block(void){return k->fibra_bloquear();}
static int yield(void){return k->fibra_yield();}
static int wake(unsigned long v){return k->fibra_despertar(v);}
static unsigned long actual(void){return k->fibra_actual();}
static int state(unsigned long v){return k->fibra_estado(v);}
static int context(void){return k->fibra_contexto();}
static unsigned long clock_ms(void){return k->obtener_ms();}
static int execute(int n){return k->fibra_ejecutar(n);}
static void message(const char *s){k->imprimir("[COMPLETION WARN] ");k->imprimir_linea(s);}
static void warning(const char *s,unsigned line,unsigned f){
 warnings++;k->imprimir("[COMPLETION WARN SITE] ");k->imprimir(s);k->imprimir(" linea=");k->imprimir_dec(line);
 k->imprimir(" flags=");k->imprimir_dec(f);k->imprimir_linea("");
}
static void fatal(int n){k->imprimir("[COMPLETION FATAL] ");k->imprimir_dec((unsigned)n);k->imprimir_linea(" backing conservado.");for(;;)__asm__ volatile("cli; hlt");}
static const struct taek_tasks_ops ops={sizeof(ops),TAEK_TASKS_VERSION,alloc,release,block,yield,wake,actual,state,context,message,warning,fatal};
static const struct taek_tasks_wait_ops wait_ops={sizeof(wait_ops),TAEK_TASKS_WAIT_VERSION,clock_ms,execute};
static void observer(uint64_t dest,void *arg){
 if(arg!=&visits || context()!=2 || (flags()&512) || taek_tasks_seleccionar(dest))mask|=1;
 if(taek_completion_reset(0)!=-4 || taek_completion_complete(0,0)!=-4 || taek_completion_try(0)!=-4 ||
    taek_tasks_configurar_esperas(0)>=0 || taek_tasks_ejecutar(0)>=0)mask|=2;
 visits++;__asm__ volatile("pxor %%xmm15,%%xmm15":::"memory");
}
static void set_fx(unsigned n){uint16_t cw=n?0x37f:0x27f;uint32_t mx=n?0x3f80:0x1f80;
 __asm__ volatile("fninit; fldcw %0; ldmxcsr %1; movdqu %2,%%xmm15"::"m"(cw),"m"(mx),"m"(patterns[n]):"memory");}
static void check_fx(unsigned n){
 unsigned char fx[512] __attribute__((aligned(16)));__asm__ volatile("fxsave64 %0":"=m"(fx)::"memory");
 uint16_t cw=n?0x37f:0x27f;uint32_t mx=n?0x3f80:0x1f80;
 if(((unsigned)fx[0]|((unsigned)fx[1]<<8))!=cw ||
    ((unsigned)fx[24]|((unsigned)fx[25]<<8)|((unsigned)fx[26]<<16)|((unsigned)fx[27]<<24))!=mx)mask|=4;
 for(unsigned j=0;j<16;j++)if(fx[160+15*16+j]!=patterns[n][j])mask|=4;
 if(fs()!=child_fs[n] || !(flags()&512) || actual()!=h[n] || taek_tasks_actual()!=id[n] || taek_tasks_estado(id[n]))mask|=8;
}
static void child(void *arg){
 unsigned n=*(unsigned *)arg;child_fs[n]=fs();uint64_t self;__asm__ volatile("movq %%fs:0,%0":"=r"(self));
 if(n>1 || !child_fs[n] || self!=child_fs[n] || child_fs[n]==root_fs || !(flags()&512) || taek_tasks_actual()!=id[n])mask|=16;
 set_fx(n);
 if(mode==0){irq(0);if(taek_completion_wait(0,0)!=-4)mask|=32;irq(512);wait_order[n]=wait_sequence++;results[n]=taek_completion_wait(0,-1);}
 else if(mode==1)results[n]=taek_completion_wait(n?2:1,n?100:8);
 else results[n]=taek_completion_wait(0,10000);
 check_fx(n);
 if(mode==2)armed=0;
}
static int retire(unsigned n){
 if(h[n]){
  if(state(h[n])!=-9 && k->fibra_liberar(h[n]))return -1;
  if(bound[n]){if(taek_tasks_desvincular(id[n]))return -1;bound[n]=0;}
  h[n]=0;
 }
 if(bound[n])return -1;
 if(id[n]){if(taek_tasks_destruir(id[n]))return -1;id[n]=0;}return 0;
}
static int create(unsigned n){
 if(h[n] || id[n] || bound[n]){k->imprimir("[COMPLETION CREATE] retained n=");k->imprimir_dec(n);k->imprimir(" h=");k->imprimir_dec(h[n]);k->imprimir(" id=");k->imprimir_dec(id[n]);k->imprimir(" bound=");k->imprimir_dec(bound[n]);k->imprimir_linea("");return -1;}
 int rc=taek_tasks_crear(&id[n]);
 if(rc){k->imprimir("[COMPLETION CREATE] task error=");k->imprimir_dec((uint64_t)(int64_t)rc);k->imprimir_linea("");return -1;}
 rc=k->fibra_crear(child,&which[n],&h[n]);
 if(!rc)rc=taek_tasks_vincular(id[n],h[n]);
 if(rc){k->imprimir("[COMPLETION CREATE] fiber/bind error=");k->imprimir_dec((uint64_t)(int64_t)rc);k->imprimir(" handle=");k->imprimir_dec(h[n]);k->imprimir_linea("");retire(n);return -1;}
 bound[n]=1;return 0;
}
static int empty(void){return !h[0] && !h[1] && !id[0] && !id[1] && !armed;}
static int iniciar(const struct taek_kernel_api *api){
 if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_CONTEXTO_BYTES)return -1;
 k=api;if(context()!=1 || taek_tasks_iniciar(&ops))return -2;active=1;
 if(taek_tasks_configurar_esperas(&wait_ops))return -3;wait_backend=1;
 if(taek_completion_iniciar())return -4;events=1;
 if(k->fibra_observar(observer,&visits))return -5;observing=1;return 0;
}
static int command(const char *arg){
 if(!active || !events || !wait_backend)return -1;
 if(equal(arg,"probar")){
  if(!empty())return -1;mask=warnings=0;root_fs=fs();uint64_t previous=flags();
  struct taek_tasks_timer_stats before,after;taek_tasks_timer_stats(&before);
  for(unsigned n=0;n<3;n++)if(taek_completion_reset(n))mask|=64;
  if(taek_completion_try(0) || taek_completion_done(0) || taek_completion_complete(0,0) || taek_completion_complete(0,0) ||
     taek_completion_count(0)!=2 || taek_completion_try(0)!=1 || taek_completion_try(0)!=1 || taek_completion_try(0) ||
     taek_completion_complete(0,1) || taek_completion_count(0)!=UINT32_MAX || taek_completion_try(0)!=1 ||
     taek_completion_try(0)!=1 || taek_completion_count(0)!=UINT32_MAX || taek_completion_reset(0))mask|=128;
  mode=0;wait_sequence=0;wait_order[0]=wait_order[1]=99;results[0]=results[1]=-99;
  if(create(0) || create(1)){retire(0);retire(1);return -2;}
  irq(512);
  if(taek_tasks_ejecutar(2)!=2 || state(h[0])!=4 || state(h[1])!=4 || taek_completion_waiters(0)!=2 ||
     taek_completion_reset(0)!=-15 || taek_completion_parar()!=-15 || wait_sequence!=2 || wait_order[0]==wait_order[1])mask|=256;
  /* El RR conserva su cursor entre rondas: FIFO es orden de encolado,
   * independientemente del orden en que se crearon las fibras. */
  unsigned first=wait_order[0]<wait_order[1]?0:1,last=first^1;
  if(taek_completion_complete(0,0) || state(h[first])!=2 || state(h[last])!=4 || taek_tasks_ejecutar(1)!=1 ||
     results[first]!=1 || taek_completion_waiters(0)!=1 || state(h[first])!=5)mask|=512;
  if(taek_completion_complete(0,1) || taek_completion_reset(0)!=-15 || taek_tasks_ejecutar(1)!=1 ||
     results[last]!=1 || taek_completion_waiters(0) || taek_completion_reset(0) || retire(0) || retire(1))mask|=1024;
  mode=1;results[0]=results[1]=-99;
  if(create(0) || create(1)){irq(previous);retire(0);retire(1);return -2;}
  if(taek_tasks_ejecutar(2)!=2 || state(h[0])!=4 || state(h[1])!=4)mask|=2048;
  taek_tasks_timer_stats(&after);if(after.activos!=2 || taek_tasks_configurar_esperas(0)>=0)mask|=4096;
  if(taek_completion_complete(2,0) || taek_tasks_ejecutar(1)!=1 || results[1]<=0 || results[1]>100 || state(h[1])!=5)mask|=8192;
  uint64_t begin=clock_ms();unsigned steps=0;
  while(state(h[0])!=5 && clock_ms()-begin<2000){int r=taek_tasks_ejecutar(1);if(r<0){mask|=16384;break;}steps+=(unsigned)r;}
  if(state(h[0])!=5 || results[0] || steps!=1 || taek_completion_done(1) || taek_completion_waiters(1) || taek_completion_waiters(2))mask|=32768;
  taek_tasks_timer_stats(&after);
  if(after.activos || after.registrados-before.registrados!=2 || after.vencidos-before.vencidos!=1 || after.cancelados-before.cancelados!=1)mask|=65536;
  if(retire(0) || retire(1) || fs()!=root_fs || taek_tasks_actual() || child_fs[0]==child_fs[1])mask|=131072;
  irq(previous);struct taek_tasks_stats s;taek_tasks_stats(&s);
  if(s.vivos!=1 || s.vinculados || s.esperando || s.referencias!=1 || s.fallos || warnings || (flags()&512)!=(previous&512))mask|=262144;
  k->imprimir("[COMPLETION] RESULTADO=");k->imprimir(mask?"FAIL":"OK");k->imprimir(" mask=");k->imprimir_dec(mask);
  k->imprimir_linea(" FIFO/all/consume wait/timeout/prewake current TLS/IF/FX");
  k->imprimir("[COMPLETION] FIFO first=");k->imprimir_dec(first);k->imprimir(" arrivals=");k->imprimir_dec(wait_order[0]);
  k->imprimir("/");k->imprimir_dec(wait_order[1]);k->imprimir_linea("");return mask?-4:0;
 }
 if(equal(arg,"armar")){
  if(!empty() || taek_completion_reset(0))return -1;mask=0;mode=2;root_fs=fs();
  if(create(0))return -2;armed=1;uint64_t previous=flags();irq(512);int r=taek_tasks_ejecutar(1);irq(previous);
  if(r!=1 || state(h[0])!=4)return -3;k->imprimir_linea("[COMPLETION] Suspendida con timer/espera; backing retenido.");return 0;
 }
 if(equal(arg,"reanudar")){
  if(!armed || taek_completion_complete(0,0))return -1;uint64_t previous=flags();irq(512);int r=taek_tasks_ejecutar(1);irq(previous);
  if(r!=1 || armed || results[0]<=0 || retire(0) || mask)return -2;
  struct taek_tasks_timer_stats s;taek_tasks_timer_stats(&s);if(s.activos)return -3;
  k->imprimir_linea("[COMPLETION] Reanudada; timer/espera/task drenados.");return 0;
 }
 return -1;
}
static int stop(void){
 if(!active)return 0;if(armed)return -1;
 for(unsigned n=0;n<2;n++)if(retire(n))return -1;
 if(events){if(taek_completion_parar())return -2;events=0;}
 if(wait_backend){if(taek_tasks_configurar_esperas(0))return -3;wait_backend=0;}
 if(observing){if(k->fibra_observar(0,0))return -4;observing=0;}
 if(taek_tasks_parar())return -5;active=0;k->imprimir_linea("[COMPLETION] Esperas/timers/tasks liberados.");return 0;
}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
 TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"completion-prueba",iniciar,command,stop};
