#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_tasks.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_kthreads.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_wq.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_early.h"
#include "../nucleo/compatibilidad/linux_i915/media/taek_video_abi.h"
/* Probe original de i915 en RAM, hasta donde pida cada orden: desde early_probe sin registros hasta
 * i915_gem_init con trabajos reales en los motores (gem-gt). Lo que no tiene servicio es una trampa
 * fatal con nombre. El dispositivo no se desmonta en gem-gt: se sale reiniciando. */
extern int taek_kthreads_autorrecoger(void);
static const struct taek_kernel_api *k;
static unsigned listos;
static uint64_t h, id, mask;
static int result, simular, soltar;
static volatile unsigned done;
#define LINUX_DIRECTO UINT64_C(0xffff888000000000)
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
static void message(const char *s){k->imprimir("[I915] ");k->imprimir_linea(s);}
/* Sin marcos de pila: candidatos a retorno dentro del modulo, para simbolizar quien provoco el aviso. */
static void pila_modulo(void){
 uint64_t *p;__asm__ volatile("mov %%rsp,%0":"=r"(p));
 const uint64_t base=UINT64_C(0x400000000000),fin=base+(24u<<20);unsigned vistos=0;
 for(unsigned i=0;i<512 && vistos<24;i++)if(p[i]>=base && p[i]<fin){
  char t[32]="[I915 PILA] 0x";unsigned n=14;
  for(int d=44;d>=0;d-=4){unsigned v=(unsigned)(p[i]>>d)&15;t[n++]=(char)(v<10?'0'+v:'A'+v-10);}
  t[n]=0;k->imprimir_linea(t);vistos++;
 }
}
static void warning(const char *s,unsigned l,unsigned f){(void)f;k->imprimir("[I915 WARN] ");k->imprimir(s);k->imprimir(":");k->imprimir_dec(l);k->imprimir_linea("");if(!(mask&2))pila_modulo();mask|=2;}
static void fatal(int n){k->imprimir("[I915 FATAL] ");k->imprimir_dec((unsigned)n);k->imprimir_linea("");k->colapsar("Frontera i915 detenida");for(;;)__asm__ volatile("cli; hlt");}
static void servir(void){
 if(taek_kthreads_servir()<0)fatal(201);
 if(taek_kthreads_autorrecoger()<0)fatal(202);
 if(taek_wq_bombear()<0)fatal(203);
 taek_early_bombear();
}
static int execute(int n){servir();int rc=k->fibra_ejecutar(n);servir();return rc;}
static int create(void (*fn)(void *),void *ctx,unsigned long *out){return k->fibra_crear(fn,ctx,out);}
static int retire(unsigned long fh){return k->fibra_liberar(fh);}
static void observer(uint64_t dest,void *arg){(void)arg;if(taek_tasks_seleccionar(dest))mask|=4;}
static unsigned long pag(unsigned n,unsigned long al){return k->paginas_asignar(n,al);}
static int pag_l(unsigned long f,unsigned n){return k->paginas_liberar(f,n);}
static unsigned long pag_o(unsigned n){return k->paginas_gpu_asignar(n);}
static unsigned pci(unsigned char b,unsigned char r,unsigned char f,unsigned char o,unsigned char n){return k->pci_leer(b,r,f,o,n);}
static void colapso(const char *m){k->colapsar(m);}
static const struct taek_tasks_ops task_ops={sizeof(task_ops),TAEK_TASKS_VERSION,alloc,release,block,yield,wake,actual,state,context,message,warning,fatal};
static const struct taek_tasks_wait_ops wait_ops={sizeof(wait_ops),TAEK_TASKS_WAIT_VERSION,clock_ms,execute};
static const struct taek_kthreads_ops thread_ops={sizeof(thread_ops),TAEK_KTHREADS_VERSION,alloc,release,create,retire,state,fatal};
static struct taek_wq_ops wq_ops={sizeof(wq_ops),TAEK_WQ_VERSION,alloc,release,clock_ms,message,fatal,0};
static int barra(unsigned char b,unsigned char r,unsigned char f,unsigned n,unsigned long *base,unsigned long *tam){
 uint64_t x=0,y=0;int rc=k->pci_barra(b,r,f,n,&x,&y);*base=x;*tam=y;return rc;
}
static unsigned long mmio_m(unsigned long f,unsigned long n,int w){return k->mmio_mapear(f,n,w);}
static int mmio_d(unsigned long v,unsigned long n){return k->mmio_desmapear(v,n);}
static unsigned long ciclos(void){return k->ciclos_por_ms();}
static int dma_dir(void){return k->dma_directo_gpu();}
static unsigned long fb_f(void){return k->fb_fisica();}
static const struct taek_early_ops early_ops={sizeof(early_ops),TAEK_EARLY_VERSION,pag,pag_l,pci,message,colapso,barra,mmio_m,mmio_d,ciclos,dma_dir,fb_f,pag_o};
static unsigned long fallo_n;
static int modo;
static void supervisor(void *arg){(void)arg;result=taek_early_ejecutar(modo,simular,soltar,fallo_n);done=1;}
static int retire_supervisor(void){
 if(h){if(state(h)!=-9 && k->fibra_liberar(h))return -1;if(taek_tasks_desvincular(id))return -1;h=0;}
 if(id){if(taek_tasks_destruir(id))return -1;id=0;}return 0;
}
static unsigned pasos_ultimo;
static uint64_t ms_ultimo;
static int gt_listo,media_listo,video_muerto;
/* Ejecuta modo en una fibra hasta que termine o caduque. 0 si la fibra termino y se retiro. */
static int correr(unsigned long limite_ms){
 mask=0;done=0;result=-99;uint64_t previous=flags();irq(512);
 if(taek_tasks_crear(&id)){irq(previous);return -2;}
 int rc=k->fibra_crear(supervisor,0,&h);if(!rc)rc=taek_tasks_vincular(id,h);
 if(rc){retire_supervisor();irq(previous);return -3;}
 uint64_t begin=clock_ms();unsigned steps=0,sin_progreso=0;
 while(!(done && state(h)==5) && clock_ms()-begin<limite_ms){
  int n=taek_tasks_ejecutar(1);if(n<0){mask|=128;break;}steps+=(unsigned)n;
  if(!n){
   if(++sin_progreso>40000)break;
   uint64_t t=clock_ms();while(clock_ms()==t)__asm__ volatile("pause");
  }else sin_progreso=0;
 }
 pasos_ultimo=steps;ms_ultimo=clock_ms()-begin;
 if(!done || state(h)!=5)mask|=256;
 if(done && state(h)==5 && retire_supervisor())mask|=512;
 irq(previous);
 return 0;
}
/* Interfaz del reproductor. Sin recuperacion del dispositivo: si una orden no termina, la GPU queda fuera hasta reiniciar y el
 * reproductor sigue por software. */
static int preparar_gpu(void){
 fallo_n=0;simular=0;soltar=0;
 if(!gt_listo){
  modo=TAEK_EARLY_GT;
  if(correr(60000) || mask || result)return TAEK_VIDEO_FALLO;
  gt_listo=1;
 }
 if(!media_listo){
  modo=TAEK_EARLY_MEDIA_INIT;
  if(correr(60000) || mask || result)return TAEK_VIDEO_FALLO;
  media_listo=1;
 }
 return TAEK_VIDEO_OK;
}
static int video_control(uint32_t orden,uint64_t arg){
 struct taek_video_peticion *p=(struct taek_video_peticion *)(uintptr_t)arg;
 if(!listos || video_muerto || h || id){video_muerto=1;return TAEK_VIDEO_FALLO;}
 if(orden==TAEK_VIDEO_PREPARAR){int r=preparar_gpu();if(r)video_muerto=1;return r;}
 if(!media_listo || !p)return TAEK_VIDEO_FALLO;
 fallo_n=0;simular=0;soltar=0;modo=TAEK_EARLY_MEDIA_OP;
 taek_early_media_peticion(p);
 /* Abrir y cerrar reservan y sueltan superficies; un fotograma tarda milisegundos. */
 int rc=correr(orden==TAEK_VIDEO_ABRIR || orden==TAEK_VIDEO_CERRAR?30000:5000);
 if(rc || mask){video_muerto=1;return TAEK_VIDEO_FALLO;}
 return result;
}
static int init(const struct taek_kernel_api *api){
 if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_GPU_BYTES)return -1;
 k=api;if(context()!=1 || taek_tasks_iniciar(&task_ops))return -2;
 if(taek_tasks_configurar_esperas(&wait_ops))return -3;
 if(taek_kthreads_iniciar(&thread_ops))return -4;
 if(k->fibra_observar(observer,0))return -5;
 int a=k->mapa_lineal_alias(LINUX_DIRECTO);
 if(!a)a=k->mapa_lineal_alias(0x0000600000000000ull);   /* Memoria "de usuario" de Media Driver, desde H130. */
 if(a){k->imprimir("[I915] Alias lineal rechazado: ");k->imprimir_dec((unsigned)-a);k->imprimir_linea("");return -7;}
 wq_ops.delta_lineal=LINUX_DIRECTO-k->hhdm_offset();
 if(taek_wq_frontera_iniciar(&wq_ops))return -6;
 if(taek_early_frontera_iniciar(&early_ops))return -8;
 listos=1;
 /* Un kernel anterior no conoce la clase de video: el modulo sigue sirviendo sus ordenes. */
 static const struct taek_driver_ops video_ops={sizeof(video_ops),TAEK_DRIVER_ABI,0,0,0,video_control};
 k->registrar("h264-gpu",TAEK_DRIVER_VIDEO,&video_ops);
 return 0;
}
static int equal(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static int prefijo(const char *a,const char *p){while(*p)if(*a++!=*p++)return 0;return 1;}
static void dec(const char *n,uint64_t v){k->imprimir(" ");k->imprimir(n);k->imprimir("=");k->imprimir_dec(v);}
static int command(const char *arg){
 /* Prueba de la red de seguridad: un colapso debe enviar evidencia por LAN y reiniciar. */
 if(equal(arg,"colapso-prueba")){k->colapsar("Colapso de prueba solicitado");return -1;}
 if(equal(arg,"colapso-pf")){*(volatile uint64_t *)(uintptr_t)(0x10+flags()%8*0)=1;return -1;}
 if(!listos || h || id)return -1;
 fallo_n=0;modo=TAEK_EARLY_SOLO_EARLY;simular=0;soltar=1;
 if(equal(arg,"probar")){}
 else if(equal(arg,"probar-simulada"))simular=1;
 else if(equal(arg,"retener"))soltar=0;
 else if(equal(arg,"mmio-lectura"))modo=TAEK_EARLY_MMIO_LECTURA;
 else if(equal(arg,"mmio-lectura-simulada")){modo=TAEK_EARLY_MMIO_LECTURA;simular=1;}
 else if(equal(arg,"forcewake"))modo=TAEK_EARLY_MMIO_FW;
 else if(equal(arg,"forcewake-simulada")){modo=TAEK_EARLY_MMIO_FW;simular=1;}
 else if(equal(arg,"mmio-info"))modo=TAEK_EARLY_MMIO_INFO;
 else if(equal(arg,"mmio-info-simulada")){modo=TAEK_EARLY_MMIO_INFO;simular=1;}
 else if(equal(arg,"reset"))modo=TAEK_EARLY_MMIO_RESET;
 else if(equal(arg,"ggtt-lectura"))modo=TAEK_EARLY_GGTT_LECTURA;
 else if(equal(arg,"ggtt-probe"))modo=TAEK_EARLY_GGTT_PROBE;
 else if(equal(arg,"hw-probe"))modo=TAEK_EARLY_HW_PROBE;
 else if(equal(arg,"gem-ggtt"))modo=TAEK_EARLY_GEM_GGTT;
 else if(equal(arg,"gem-gt")){modo=TAEK_EARLY_GT;soltar=0;}
 else if(equal(arg,"gt-hola")){modo=TAEK_EARLY_GT_HOLA;soltar=0;}
 else if(equal(arg,"gt-mal")){modo=TAEK_EARLY_GT_MAL;soltar=0;}
 else if(equal(arg,"gt-guarda")){modo=TAEK_EARLY_GT_GUARDA;soltar=0;}
 else if(equal(arg,"media-rt")){modo=TAEK_EARLY_MEDIA_RT;soltar=0;}
 else if(equal(arg,"media-init")){modo=TAEK_EARLY_MEDIA_INIT;soltar=0;}
 else if(equal(arg,"media-caps")){modo=TAEK_EARLY_MEDIA_CAPS;soltar=0;}
 else if(equal(arg,"media-idr")){modo=TAEK_EARLY_MEDIA_IDR;soltar=0;}
 else if(equal(arg,"media-mal")){modo=TAEK_EARLY_MEDIA_MAL;soltar=0;}
 else if(equal(arg,"media-video")){modo=TAEK_EARLY_MEDIA_VIDEO;soltar=0;}
 else if(equal(arg,"gt-carga")){modo=TAEK_EARLY_GT_CARGA;soltar=0;}
 else if(equal(arg,"gt-lote")){modo=TAEK_EARLY_GT_LOTE;soltar=0;}
 else if(equal(arg,"gt-cuelgue")){modo=TAEK_EARLY_GT_CUELGUE;soltar=0;}
 else if(equal(arg,"gt-cerrar")){modo=TAEK_EARLY_GT_CERRAR;soltar=0;}
 else if(equal(arg,"hw-probe-simulada-directo")){modo=TAEK_EARLY_HW_PROBE;simular=1;taek_early_dma_simulada(1);}
 else if(equal(arg,"ggtt-probe-simulada")){modo=TAEK_EARLY_GGTT_PROBE;simular=1;taek_early_dma_simulada(0);}
 else if(equal(arg,"ggtt-probe-simulada-directo")){modo=TAEK_EARLY_GGTT_PROBE;simular=1;taek_early_dma_simulada(1);}
 else if(prefijo(arg,"fallo ") || prefijo(arg,"fallo-simulada ")){
  simular=arg[5]=='-';soltar=1;
  const char *p=arg+(simular?15:6);if(*p<'0' || *p>'9')return -1;
  while(*p>='0' && *p<='9')fallo_n=fallo_n*10+(unsigned long)(*p++-'0');
  if(*p || !fallo_n)return -1;
 }
 else return -1;
 int corrio=correr(60000);if(corrio)return corrio;
 unsigned steps=pasos_ultimo;uint64_t ms=ms_ultimo;
 if(!mask && !result){if(modo==TAEK_EARLY_GT)gt_listo=1;if(modo==TAEK_EARLY_MEDIA_INIT)media_listo=1;}
 struct taek_early_resultado r;taek_early_resultado_leer(&r);
 struct taek_wq_stats s;taek_wq_estadisticas(&s);
 k->imprimir("[I915] RESULTADO=");k->imprimir(mask||result?"FAIL":"OK");
 dec("mask",mask);dec("fase",(uint64_t)r.fase);k->imprimir(" rc=");if(r.rc<0)k->imprimir("-");k->imprimir_dec(r.rc<0?(uint64_t)-r.rc:(uint64_t)r.rc);
 dec("steps",steps);dec("ms",ms);k->imprimir_linea("");
 k->imprimir("[I915] identidad");dec("vendor",r.vendor);dec("device",r.device);dec("rev",r.revision);dec("simulada",r.simulada);k->imprimir_linea("");
 k->imprimir("[I915] linux");dec("plataforma",r.platform);dec("sub",r.subplatform);dec("stepgt",r.step_gt);dec("pch",r.pch_tipo);dec("pchid",r.pch_id);dec("gts",r.gt_count);dec("ttm",r.mem_regiones_ok);k->imprimir_linea("");
 k->imprimir("[I915] frontera");dec("heap_bloques",s.heap_bloques);dec("heap_bytes",s.heap_bytes);dec("heap_pico",s.heap_pico_bytes);dec("heap_fallos",s.heap_fallos);dec("paginas",r.paginas);dec("pico_pag",r.paginas_pico);dec("devres",r.devres);dec("registros",r.registros);dec("rcu_pend",s.rcu_pendientes);dec("timers_pend",s.timers_pendientes);dec("inyectados",taek_wq_fallos_inyectados());k->imprimir_linea("");
 k->imprimir("[I915] mmio");dec("modo",r.modo);dec("fallos",r.mmio_fallos);dec("mapeos",r.mapeos);dec("hrtimers",r.hrtimers);dec("dma_mapeados",r.dma_mapeados);dec("dma_rechazados",r.dma_rechazados);k->imprimir_linea("");
 k->imprimir_linea("[I915] probe original segun el modo; esto no es decodificacion.");
 return mask||result?-4:0;
}
static int stop(void){return listos?-1:0;}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
 TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"i915-early",init,command,stop};
