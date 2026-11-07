/* Tareas kernel adaptadas TaekOS. Mantiene task_struct/refs/wake_q originales;
 * consumidores mutex, wake_q y esperas kernel con estados limitados sobre BSP.
 * No crea copy_process/kthread, stack Linux, FPU Linux, señales, PID namespaces
 * ni workers. El backend es cooperativo; no admitir llamadas desde AP/IRQ/NMI.
 */
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/wake_q.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <linux/bug.h>
#include <linux/jiffies.h>
#include <asm/current.h>
#include "../upstream_tasks.h"
_Static_assert(!IS_ENABLED(CONFIG_SMP),"Solo perfil UP");
_Static_assert(!IS_ENABLED(CONFIG_PREEMPT_COUNT),"Perfil PREEMPT_NONE sin contador");
_Static_assert(__alignof__(struct task_struct)<=64,"Revisar alineacion de arquitectura");
_Static_assert(sizeof(struct taek_tasks_ops)==96,"Conservar ABI H124");
_Static_assert(sizeof(struct taek_tasks_wait_ops)==24,"ABI wait_ops x86-64");
_Static_assert(HZ==250,"Conversion monotona milisegundos a jiffies HZ250");
DEFINE_PER_CPU_ALIGNED(struct pcpu_hot,pcpu_hot);
/* sched/core.c avisa a la workqueue original alrededor de cada sueño real: sin esto,
 * un callback bloqueado deja nr_running en 1 y su pool no arranca a quien lo resuelve. */
#ifdef TAEK_TASKS_WQ_HOOKS
#include <linux/sched/signal.h>
extern void wq_worker_sleeping(struct task_struct *);
extern void wq_worker_running(struct task_struct *);
#define WQ_ANTES_DE_DORMIR(t) do{if((t)->flags&PF_WQ_WORKER)wq_worker_sleeping(t);}while(0)
#define WQ_AL_VOLVER(t) do{if((t)->flags&PF_WQ_WORKER)wq_worker_running(t);}while(0)
#else
#define WQ_ANTES_DE_DORMIR(t) do{}while(0)
#define WQ_AL_VOLVER(t) do{}while(0)
#endif
struct registro {
    struct task_struct *task;
    void *raw;
    unsigned long id,fibra;
    unsigned generacion,agotado,esperando,held,retirando,token;
    unsigned timer_activo,timer_vencido;
    unsigned long timer_inicio,timer_plazo;
};
static struct {
    struct taek_tasks_ops ops;
    struct registro registros[TAEK_TASKS_SLOTS+1];
    struct mutex mutex;
    struct taek_tasks_stats st;
    struct taek_tasks_wait_ops waits;
    struct taek_tasks_timer_stats ts;
    unsigned wait_configurado,reloj_valido,en_bombeo,en_reloj;
    unsigned long reloj_ms;
    unsigned activo,pid;
} g;
static void cero(void *p,unsigned long n){volatile unsigned char *b=p;while(n--)*b++=0;}
static struct registro *por_id(unsigned long id){
    if(!id)return g.registros[0].task?&g.registros[0]:NULL;
    unsigned n=(unsigned)id;if(!n || n>TAEK_TASKS_SLOTS)return NULL;
    struct registro *r=&g.registros[n];return r->task && r->id==id?r:NULL;
}
static struct registro *por_task(struct task_struct *t){
    for(unsigned i=0;i<=TAEK_TASKS_SLOTS;i++)if(g.registros[i].task==t && t)return &g.registros[i];
    return NULL;
}
static void fatal(int n){g.st.fallos++;g.ops.fatal(n);__builtin_trap();}
static struct registro *en_cpu(void){
    struct registro *r=por_task(current);
    if(!g.activo || g.en_reloj || g.ops.contexto()!=1 || !r || r->retirando || r->fibra!=g.ops.actual())fatal(1);
    return r;
}
static int ordinario(void){return g.activo && !g.en_reloj && g.ops.contexto()==1;}
static int raiz(void){return ordinario() && !g.ops.actual() && current==g.registros[0].task && !g.registros[0].retirando;}
static unsigned timers_vivos(void){
    unsigned n=0;for(unsigned i=0;i<=TAEK_TASKS_SLOTS;i++)n+=!!g.registros[i].timer_activo;return n;
}
int taek_tasks_contexto(void){return g.activo && !g.en_reloj?g.ops.contexto():0;}
int taek_tasks_configurar_esperas(const struct taek_tasks_wait_ops *ops){
    if(!raiz())return -1;
    if(g.en_bombeo || g.en_reloj || timers_vivos())return -15;
    if(ops && (ops->bytes<sizeof(*ops) || ops->version!=TAEK_TASKS_WAIT_VERSION || !ops->clock_ms || !ops->ejecutar))return -1;
    if(ops)g.waits=*ops;else cero(&g.waits,sizeof(g.waits));
    g.wait_configurado=!!ops;g.reloj_valido=0;g.reloj_ms=0;
    return 0;
}
static unsigned long reloj_ms(void){
    if(!g.wait_configurado || !g.waits.clock_ms || g.en_reloj)fatal(18);
    g.en_reloj=1;unsigned long now=g.waits.clock_ms();g.en_reloj=0;
    if(g.reloj_valido && now<g.reloj_ms)fatal(19);
    g.reloj_ms=now;g.reloj_valido=1;return now;
}
static int reservar(unsigned n){
    struct registro *r=&g.registros[n];
    void *raw=g.ops.asignar(sizeof(struct task_struct)+63);if(!raw)return -6;
    struct task_struct *t=(void *)ALIGN((unsigned long)raw,64);
    cero(t,sizeof(*t));refcount_set(&t->usage,1);WRITE_ONCE(t->__state,TASK_RUNNING);
    t->wake_q.next=NULL;t->worker_private=NULL;t->thread_info.flags=0;
    raw_spin_lock_init(&t->pi_lock);spin_lock_init(&t->alloc_lock);
    t->pid=(int)g.pid++;t->comm[0]='t';t->comm[1]='a';t->comm[2]='e';t->comm[3]='k';
    r->raw=raw;r->task=t;r->fibra=0;r->held=r->esperando=r->retirando=r->token=0;
    r->timer_activo=r->timer_vencido=0;r->timer_inicio=r->timer_plazo=0;
    if(n){r->generacion++;r->id=((unsigned long)r->generacion<<32)|n;}
    else r->id=0;
    g.st.creadas++;return 0;
}
int taek_tasks_iniciar(const struct taek_tasks_ops *ops){
    if(g.activo)return -3;
    if(!ops || ops->bytes<sizeof(*ops) || ops->version!=TAEK_TASKS_VERSION ||
       !ops->asignar || !ops->liberar || !ops->bloquear || !ops->yield || !ops->despertar ||
       !ops->actual || !ops->estado || !ops->contexto || !ops->mensaje || !ops->warning || !ops->fatal ||
       ops->contexto()!=1 || ops->actual())return -1;
    g.ops=*ops;g.pid=1000;cero(&g.st,sizeof(g.st));cero(&g.ts,sizeof(g.ts));
    if(reservar(0)){cero(&g.ops,sizeof(g.ops));return -6;}
    __mutex_init(&g.mutex,"taek-mutex",NULL);WRITE_ONCE(pcpu_hot.current_task,g.registros[0].task);
    g.activo=1;return 0;
}
int taek_tasks_crear(unsigned long *id){
    if(!raiz() || !id)return -1;
    for(unsigned i=1;i<=TAEK_TASKS_SLOTS;i++){
        struct registro *r=&g.registros[i];if(r->task || r->agotado)continue;
        if(r->generacion==~0u){r->agotado=1;continue;}
        int rc=reservar(i);if(!rc)*id=r->id;return rc;
    }return -5;
}
int taek_tasks_vincular(unsigned long id,unsigned long fibra){
    struct registro *r=por_id(id);
    if(!raiz() || !id || !r || r->retirando || r->fibra || !fibra || g.ops.estado(fibra)!=1)return -1;
    for(unsigned i=1;i<=TAEK_TASKS_SLOTS;i++)if(g.registros[i].task && g.registros[i].fibra==fibra)return -1;
    r->fibra=fibra;return 0;
}
int taek_tasks_desvincular(unsigned long id){
    struct registro *r=por_id(id);
    if(!raiz() || !id || !r || r->retirando || !r->fibra || g.ops.estado(r->fibra)!=-9 ||
       r->esperando || r->held || r->timer_activo || READ_ONCE(r->task->__state)!=TASK_RUNNING)return -1;
    r->fibra=0;r->token=0;return 0; /* motor ya retiro el token con el handle */
}
int taek_tasks_seleccionar(unsigned long fibra){
    if(!g.activo)return -2;if(g.ops.contexto()!=2)return -4;
    if(!fibra){WRITE_ONCE(pcpu_hot.current_task,g.registros[0].task);return 0;}
    for(unsigned i=1;i<=TAEK_TASKS_SLOTS;i++){
        struct registro *r=&g.registros[i];if(r->task && r->fibra==fibra && !r->retirando){WRITE_ONCE(pcpu_hot.current_task,r->task);return 0;}
    }return -9; /* El observer consumidor debe tratar esto como fatal. */
}
unsigned long taek_tasks_actual(void){return ordinario()?en_cpu()->id:0;}
int taek_tasks_pid(unsigned long id){struct registro *r=por_id(id);return r?task_pid_nr(r->task):-1;}
int taek_tasks_refs(unsigned long id){struct registro *r=por_id(id);return r?refcount_read(&r->task->usage):-1;}
int taek_tasks_estado(unsigned long id){struct registro *r=por_id(id);return r?(int)READ_ONCE(r->task->__state):-1;}
void *taek_tasks_objeto(unsigned long id){
    if(!ordinario())return NULL;
    struct registro *caller=por_task(current);
    if(!caller || caller->retirando || caller->fibra!=g.ops.actual())return NULL;
    struct registro *r=por_id(id);
    return r && !r->retirando?r->task:NULL;
}
/* Destructor de la frontera: se libera el backing real solo tras retirar
 * binding, esperas, lock y referencia base. No sustituye el teardown Linux. */
void __put_task_struct(struct task_struct *t){
    struct registro *r=por_task(t);
    if(!r || !r->retirando || r->fibra || r->esperando || r->held || r->token || r->timer_activo ||
       refcount_read(&t->usage) || t->wake_q.next || current==t)fatal(2);
    if(g.ops.liberar(r->raw)){g.st.fallos++;return;} /* conservar para retry */
    r->task=NULL;r->raw=NULL;r->id=0;r->retirando=0;g.st.destruidas++;
}
int taek_tasks_destruir(unsigned long id){
    struct registro *r=por_id(id);
    if(!raiz() || !id || !r || r->fibra || r->esperando || r->held || r->token || r->timer_activo || r->task->wake_q.next)return -1;
    if(r->retirando){__put_task_struct(r->task);return r->task?-6:0;}
    if(refcount_read(&r->task->usage)!=1)return -15;
    r->retirando=1;put_task_struct(r->task);return r->task?-6:0;
}
int taek_tasks_parar(void){
    if(!ordinario() || g.ops.actual() || current!=g.registros[0].task)return -1;
    if(g.en_bombeo || g.en_reloj || timers_vivos())return -15;
    if(mutex_is_locked(&g.mutex) || !list_empty(&g.mutex.wait_list))return -15;
    for(unsigned i=1;i<=TAEK_TASKS_SLOTS;i++)if(g.registros[i].task)return -15;
    struct registro *r=&g.registros[0];
    if(r->held || r->esperando || r->token || r->task->wake_q.next ||
       (!r->retirando && refcount_read(&r->task->usage)!=1))return -15;
    WRITE_ONCE(pcpu_hot.current_task,NULL);
    if(r->retirando)__put_task_struct(r->task);
    else{r->retirando=1;put_task_struct(r->task);}
    if(r->task){WRITE_ONCE(pcpu_hot.current_task,r->task);return -6;}
    g.activo=0;cero(&g.ops,sizeof(g.ops));cero(&g.waits,sizeof(g.waits));
    g.wait_configurado=g.reloj_valido=0;g.reloj_ms=0;return 0;
}
static int consumir_token(struct registro *r){
    if(!r->token)return 0;
    if(g.ops.bloquear() || en_cpu()!=r)fatal(5);
    r->token=0;g.st.prewakes++;return 1;
}
/* schedule_preempt_disabled conserva su wrapper original. schedule es la
 * frontera cooperativa; wake previo consume exactamente el token del motor. */
static int estado_admitido(unsigned state){
    /* TASK_KILLABLE: sin senales nadie puede matar la espera; duerme como UNINTERRUPTIBLE. */
    return state==TASK_RUNNING || state==TASK_INTERRUPTIBLE ||
           state==TASK_UNINTERRUPTIBLE || state==TASK_IDLE || state==TASK_KILLABLE;
}
void schedule(void){
    struct registro *r=en_cpu();if(!r->fibra)fatal(3);
    unsigned state=READ_ONCE(r->task->__state);
    if(!estado_admitido(state))fatal(4);
    int prewake=consumir_token(r);
    if(state==TASK_RUNNING){if(!prewake){if(g.ops.yield())fatal(6);g.st.yields++;}}
    else{WQ_ANTES_DE_DORMIR(r->task);g.st.bloqueos++;if(g.ops.bloquear())fatal(7);}
    if(en_cpu()!=r || READ_ONCE(r->task->__state)!=TASK_RUNNING)fatal(8);
    WQ_AL_VOLVER(r->task);
}
int try_to_wake_up(struct task_struct *t,unsigned int state_mask,int wake_flags){
    /* Esta frontera aun no despierta desde IRQ/AP ni implementa WF_*.
     * Rechazar antes de publicar state o token, incluso para RUNNING. */
    if(!ordinario() || !state_mask || (state_mask&~(TASK_NORMAL|TASK_NOLOAD)) || wake_flags)fatal(20);
    en_cpu();
    struct registro *r=por_task(t);
    if(!g.activo || !r || r->retirando || !r->fibra)fatal(9);
    smp_mb();unsigned state=READ_ONCE(t->__state);
    if(state==TASK_RUNNING)return 0;
    if(!estado_admitido(state))fatal(10);
    if(!(state&state_mask))return 0;
    int fs=g.ops.estado(r->fibra);if(fs!=2 && fs!=3 && fs!=4)fatal(11);
    WRITE_ONCE(t->__state,TASK_RUNNING);smp_mb();
    if(g.ops.despertar(r->fibra))fatal(12);
    if(fs!=4)r->token=1;
    g.st.wakes++;return 1;
}
int wake_up_process(struct task_struct *t){return try_to_wake_up(t,TASK_NORMAL,0);}
int wake_up_state(struct task_struct *t,unsigned int state){return try_to_wake_up(t,state,0);}
static unsigned bombear_timers(void){
    unsigned long now=reloj_ms();g.ts.bombeos++;unsigned despertados=0;
    for(unsigned i=1;i<=TAEK_TASKS_SLOTS;i++){
        struct registro *r=&g.registros[i];
        if(!r->timer_activo || r->timer_vencido)continue;
        if(!r->task || r->retirando || !r->fibra || now<r->timer_inicio)fatal(21);
        if((now-r->timer_inicio)/4<r->timer_plazo)continue;
        r->timer_vencido=1;g.ts.vencidos++;
        despertados+=(unsigned)try_to_wake_up(r->task,TASK_NORMAL,0);
    }
    return despertados;
}
int taek_tasks_ejecutar(int pasos){
    if(!raiz() || pasos<0 || (unsigned)pasos>TAEK_TASKS_MAX_PASOS)return -1;
    if(!g.wait_configurado)return -2;
    if(g.en_bombeo || g.en_reloj)return -13;
    g.en_bombeo=1;int total=0,rc=0;
    if(!pasos)bombear_timers();
    for(int n=0;n<pasos;n++){
        bombear_timers();rc=g.waits.ejecutar(1);
        if(!raiz() || rc>1)fatal(22);
        unsigned despertados=bombear_timers();
        if(rc<0 || (!rc && !despertados))break;
        total+=rc;
    }
    g.en_bombeo=0;return rc<0?rc:total;
}
void taek_tasks_timer_stats(struct taek_tasks_timer_stats *out){
    if(!out)return;*out=g.ts;out->configurado=g.wait_configurado;
    out->activos=timers_vivos();out->bombeando=g.en_bombeo;out->reloj_ms=g.reloj_ms;
}
/* Solo perfil sin block I/O: las tareas se crean plug=NULL y no exponen
 * setters de plug. Un consumidor que requiera un plug debe integrar storage. */
int io_schedule_prepare(void){
    struct registro *r=en_cpu();if(r->task->plug)fatal(13);
    int old=r->task->in_iowait;r->task->in_iowait=1;return old;
}
void io_schedule_finish(int old){if(old!=0 && old!=1)fatal(14);en_cpu()->task->in_iowait=old;}
long schedule_timeout(long timeout){
    struct registro *r=en_cpu();
    if(timeout<0){
        g.ops.mensaje("schedule_timeout: timeout negativo; se devuelve 0.");
        g.st.avisos++;g.ops.warning(__FILE__,__LINE__,0);
        __set_current_state(TASK_RUNNING);return 0;
    }
    if(!timeout){__set_current_state(TASK_RUNNING);return 0;}
    if(!r->fibra || r->timer_activo)fatal(23);
    unsigned state=READ_ONCE(r->task->__state);
    if(!estado_admitido(state))fatal(4);
    if(timeout==MAX_SCHEDULE_TIMEOUT){schedule();__set_current_state(TASK_RUNNING);return timeout;}
    /* No registrar ni bloquear si falta un reloj monotono/pump operativo. */
    unsigned long inicio=reloj_ms();
    r->timer_inicio=inicio;r->timer_plazo=(unsigned long)timeout;r->timer_vencido=0;r->timer_activo=1;g.ts.registrados++;
    schedule();
    unsigned long now=reloj_ms();unsigned long elapsed=(now-inicio)/4;
    long restante=elapsed>=(unsigned long)timeout?0:timeout-(long)elapsed;
    if(!r->timer_vencido)g.ts.cancelados++;
    r->timer_activo=r->timer_vencido=0;r->timer_inicio=r->timer_plazo=0;
    __set_current_state(TASK_RUNNING);return restante;
}
long io_schedule_timeout(long timeout){
    int old=io_schedule_prepare();long restante=schedule_timeout(timeout);io_schedule_finish(old);return restante;
}
long schedule_timeout_interruptible(long timeout){
    en_cpu();__set_current_state(TASK_INTERRUPTIBLE);return schedule_timeout(timeout);
}
void __warn_printk(const char *fmt,...){if(!g.activo)__builtin_trap();g.ops.mensaje(fmt);}
void taek_linux_warn_site(const char *file,unsigned line,unsigned flags){g.st.avisos++;g.ops.warning(file,line,flags);}
int taek_tasks_mutex_lock(void){
    if(!g.activo)return -2;
    if(!ordinario())return -4;
    struct registro *r=en_cpu();if(r->held)return -1;
    if(!r->fibra && mutex_is_locked(&g.mutex))return -15;
    consumir_token(r);
    r->esperando=1;mutex_lock(&g.mutex);r->esperando=0;r->held=1;return 0;
}
int taek_tasks_mutex_unlock(void){
    if(!g.activo)return -2;
    if(!ordinario())return -4;
    struct registro *r=en_cpu();if(!r->held)return -1;
    mutex_unlock(&g.mutex);r->held=0;return 0;
}
int taek_tasks_mutex_locked(void){return g.activo?mutex_is_locked(&g.mutex):-1;}
int taek_tasks_yield(void){
    if(!g.activo)return -2;
    if(!ordinario())return -4;
    struct registro *r=en_cpu();if(!r->fibra || READ_ONCE(r->task->__state)!=TASK_RUNNING)return -1;
    consumir_token(r);
    schedule();return 0;
}
int taek_tasks_wakeq(unsigned long id,int duplicate){
    struct registro *r=por_id(id);if(!ordinario() || !r || r->retirando || !r->fibra || (duplicate!=0 && duplicate!=1))return -1;
    en_cpu();
    DEFINE_WAKE_Q(head);int refs=refcount_read(&r->task->usage);
    wake_q_add(&head,r->task);if(refcount_read(&r->task->usage)!=refs+1)fatal(15);
    if(duplicate){wake_q_add(&head,r->task);if(refcount_read(&r->task->usage)!=refs+1)fatal(16);}
    wake_up_q(&head);
    if(refcount_read(&r->task->usage)!=refs || r->task->wake_q.next)fatal(17);
    return 0;
}
int taek_tasks_publicar_espera_estado(unsigned state){
    if(!g.activo)return -2;if(!ordinario())return -4;
    if(!estado_admitido(state) || state==TASK_RUNNING)return -1;
    struct registro *r=en_cpu();if(!r->fibra)return -1;consumir_token(r);set_current_state(state);return 0;
}
int taek_tasks_publicar_espera(void){return taek_tasks_publicar_espera_estado(TASK_UNINTERRUPTIBLE);}
int taek_tasks_schedule(void){if(!g.activo)return -2;if(!ordinario())return -4;if(!en_cpu()->fibra)return -1;schedule();return 0;}
void taek_tasks_stats(struct taek_tasks_stats *out){
    if(!out)return;*out=g.st;out->vivos=out->vinculados=out->esperando=out->referencias=0;
    for(unsigned i=0;i<=TAEK_TASKS_SLOTS;i++){
        struct registro *r=&g.registros[i];if(!r->task)continue;
        out->vivos++;out->vinculados+=!!r->fibra;out->esperando+=!!r->esperando;
        unsigned refs=refcount_read(&r->task->usage);
        out->referencias=refs>~0u-out->referencias?~0u:out->referencias+refs;
    }
}
void *memcpy(void *dest,const void *src,size_t n){
    volatile unsigned char *d=dest;const unsigned char *s=src;while(n--)*d++=*s++;return dest;
}
