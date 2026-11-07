/* Se incluye DESPUES del kthread.c original íntegro. Así se ve su struct
 * kthread privado sin duplicar layout. Los constructores/exit originales no
 * son roots; estos servicios explícitos son el backend adaptado de TaekOS. */
#include "../upstream_kthreads.h"
#include "../upstream_tasks.h"
struct taek_kthread_request {
    struct list_head node;
    struct completion done;
    int (*fn)(void *);
    void *data;
    const char *name;
    unsigned long id;
    int rc;
};
enum taek_kthread_phase { KT_EMPTY,KT_BUILD,KT_READY,KT_EXITED,KT_FAILED,KT_REAP };
struct taek_kthread_record {
    unsigned long id,fiber;
    struct task_struct *task;
    struct kthread *payload;
    char *name;
    struct taek_kthread_request *request;
    unsigned phase,bound,joining,joined,started,autoreap;
};
static struct {
    struct taek_kthreads_ops ops;
    struct taek_kthread_record records[TAEK_TASKS_SLOTS];
    struct list_head requests;
    struct taek_kthreads_stats stats;
    unsigned active,serving;
} kt;
static int kt_root(void){return kt.active && taek_tasks_contexto()==1 && !taek_tasks_actual();}
static int kt_child(void){
    unsigned long flags;asm volatile("pushfq; pop %0":"=r"(flags)::"memory");
    return kt.active && taek_tasks_contexto()==1 && taek_tasks_actual() && (flags&512);
}
static void kt_fatal(int n){kt.stats.fallos++;kt.ops.fatal(n);__builtin_trap();}
static void kt_zero(void *p,unsigned long n){volatile unsigned char *b=p;while(n--)*b++=0;}
static struct taek_kthread_record *kt_find(unsigned long id){
    if(!id)return NULL;
    for(unsigned i=0;i<TAEK_TASKS_SLOTS;i++)if(kt.records[i].phase && kt.records[i].id==id)return &kt.records[i];
    return NULL;
}
/* Once the callback returns its data may already have been freed by an
 * original worker. Only payload/result/owned record are accessed afterwards. */
static void kt_entry(void *arg){
    struct taek_kthread_record *r=arg;
    if(!kt_child() || r->phase!=KT_BUILD || current!=r->task ||
       taek_tasks_actual()!=r->id || r->task->worker_private!=r->payload)kt_fatal(101);
    set_current_state(TASK_UNINTERRUPTIBLE);r->phase=KT_READY;
    schedule();
    r->started=1;
    int result=-EINTR;
    if(!kthread_should_stop())result=r->payload->threadfn(r->payload->data);
    __set_current_state(TASK_RUNNING);
    r->payload->result=result;
    complete(&r->payload->exited);
    r->phase=KT_EXITED;kt.stats.terminados++;
}
/* Stage confirmation matters: a Task or TLS free can fail and must not cause
 * a second successful free of an earlier stage on the next attempt. */
static int kt_reap(struct taek_kthread_record *r){
    if(r->joining || r->request)return -EBUSY;
    if(r->task && taek_tasks_refs(r->id)>1)return -EBUSY;
    if(r->fiber){
        int state=kt.ops.estado(r->fiber);
        if(state!=-9 && state!=1 && state!=5 && state!=6)return -EBUSY;
        if(state!=-9 && kt.ops.retirar(r->fiber))return -EAGAIN;
        if(r->bound){if(taek_tasks_desvincular(r->id))return -EAGAIN;r->bound=0;}
        r->fiber=0;
    }
    if(r->bound)return -EAGAIN;
    if(r->name){if(kt.ops.liberar(r->name))return -EAGAIN;r->name=NULL;if(r->payload)r->payload->full_name=NULL;}
    if(r->payload){
        if(kt.ops.liberar(r->payload))return -EAGAIN;
        r->payload=NULL;if(r->task){r->task->worker_private=NULL;r->task->vfork_done=NULL;}
    }
    if(r->task){if(taek_tasks_destruir(r->id))return -EAGAIN;r->task=NULL;}
    r->id=0;r->phase=KT_EMPTY;r->joined=r->started=0;kt.stats.recolectados++;return 0;
}
int taek_kthreads_iniciar(const struct taek_kthreads_ops *ops){
    if(kt.active)return -EBUSY;
    if(taek_tasks_contexto()!=1 || taek_tasks_actual() || !ops || ops->bytes<sizeof(*ops) ||
       ops->version!=TAEK_KTHREADS_VERSION || !ops->asignar || !ops->liberar ||
       !ops->crear || !ops->retirar || !ops->estado || !ops->fatal)return -EINVAL;
    kt.ops=*ops;INIT_LIST_HEAD(&kt.requests);kt_zero(&kt.stats,sizeof(kt.stats));kt.active=1;return 0;
}
int taek_kthreads_parar(void){
    if(!kt_root() || kt.serving)return -EINVAL;
    if(!list_empty(&kt.requests))return -EBUSY;
    for(unsigned i=0;i<TAEK_TASKS_SLOTS;i++)if(kt.records[i].phase)return -EBUSY;
    kt.active=0;kt_zero(&kt.ops,sizeof(kt.ops));return 0;
}
int taek_kthread_crear(int (*fn)(void *),void *data,const char *name,unsigned long *id){
    if(!kt_child() || !fn || !name || !id)return -EINVAL;
    unsigned len=0;while(len<64 && name[len])len++;
    if(!len || len==64)return -EINVAL;
    struct taek_kthread_request request;
    request.fn=fn;request.data=data;request.name=name;request.id=0;request.rc=-EINPROGRESS;
    init_completion(&request.done);INIT_LIST_HEAD(&request.node);
    list_add_tail(&request.node,&kt.requests);
    wait_for_completion(&request.done);
    if(!list_empty(&request.node) || request.rc==-EINPROGRESS)kt_fatal(102);
    if(!request.rc)*id=request.id;
    return request.rc;
}
static void kt_reply(struct taek_kthread_request *q,int rc,unsigned long id){
    list_del_init(&q->node);q->rc=rc;q->id=id;complete(&q->done);
}
static int kt_build(struct taek_kthread_record *r,struct taek_kthread_request *q){
    r->phase=KT_BUILD;
    if(taek_tasks_crear(&r->id))return -ENOMEM;
    r->task=taek_tasks_objeto(r->id);if(!r->task)kt_fatal(103);
    r->payload=kt.ops.asignar(sizeof(*r->payload));if(!r->payload)return -ENOMEM;
    kt_zero(r->payload,sizeof(*r->payload));init_completion(&r->payload->exited);init_completion(&r->payload->parked);
    r->task->flags|=PF_KTHREAD;r->task->worker_private=r->payload;r->task->vfork_done=&r->payload->exited;
    unsigned n=0;while(q->name[n])n++;
    r->name=kt.ops.asignar(n+1);if(!r->name)return -ENOMEM;
    for(unsigned j=0;j<=n;j++)r->name[j]=q->name[j];r->payload->full_name=r->name;
    for(unsigned j=0;j<TASK_COMM_LEN;j++)r->task->comm[j]=j<n?q->name[j]:0;
    r->task->comm[TASK_COMM_LEN-1]=0;r->payload->threadfn=q->fn;r->payload->data=q->data;
    if(kt.ops.crear(kt_entry,r,&r->fiber))return -ENOMEM;
    if(taek_tasks_vincular(r->id,r->fiber))return -EINVAL;r->bound=1;
    r->request=q;kt.stats.creados++;return 0;
}
int taek_kthreads_servir(void){
    if(!kt_root() || kt.serving)return -EINVAL;
    kt.serving=1;int completed=0;
    for(unsigned i=0;i<TAEK_TASKS_SLOTS;i++){
        struct taek_kthread_record *r=&kt.records[i];
        if(r->phase==KT_READY && r->request){
            if(kt.ops.estado(r->fiber)!=4 || READ_ONCE(r->task->__state)!=TASK_UNINTERRUPTIBLE)kt_fatal(104);
            struct taek_kthread_request *q=r->request;r->request=NULL;kt_reply(q,0,r->id);completed++;
        }
        if(r->phase==KT_FAILED){int rc=kt_reap(r);if(rc && rc!=-EAGAIN && rc!=-EBUSY)kt_fatal(105);}
    }
    struct taek_kthread_request *q,*next;
    list_for_each_entry_safe(q,next,&kt.requests,node){
        /* BUILD requests stay on the list until the initial stopped handshake. */
        int owned=0;for(unsigned i=0;i<TAEK_TASKS_SLOTS;i++)if(kt.records[i].request==q)owned=1;
        if(owned)continue;
        struct taek_kthread_record *r=NULL;
        for(unsigned i=0;i<TAEK_TASKS_SLOTS;i++)if(!kt.records[i].phase){r=&kt.records[i];break;}
        if(!r){kt_reply(q,-ENOMEM,0);completed++;continue;}
        int rc=kt_build(r,q);
        if(rc){r->request=NULL;r->phase=KT_FAILED;kt.stats.fallos++;kt_reap(r);kt_reply(q,rc,0);completed++;}
    }
    kt.serving=0;return completed;
}
int taek_kthread_despertar(unsigned long id){
    if(!kt.active || taek_tasks_contexto()!=1)return -EINVAL;
    struct taek_kthread_record *r=kt_find(id);
    if(!r || r->request || r->phase!=KT_READY || !r->bound)return -EINVAL;
    return wake_up_process(r->task);
}
int taek_kthread_detener(unsigned long id,int *result){
    if(!kt_child() || !result)return -EINVAL;
    struct taek_kthread_record *r=kt_find(id);
    if(!r || current==r->task || r->request || r->joining ||
       (r->phase!=KT_READY && r->phase!=KT_EXITED))return -EINVAL;
    if(r->joined)return -EALREADY;
    r->joining=1;int rc=kthread_stop(r->task);r->joining=0;
    /* The original wait consumes exited.done. Persist the completed join in
     * our ledger rather than treating the completion token as an exit flag. */
    if(r->phase!=KT_EXITED)kt_fatal(106);
    r->joined=1;
    *result=rc;return 0;
}
int taek_kthread_recolectar(unsigned long id){
    if(!kt_root() || kt.serving)return -EINVAL;
    struct taek_kthread_record *r=kt_find(id);
    if(!r || (r->phase!=KT_EXITED && r->phase!=KT_FAILED && r->phase!=KT_REAP))return -EBUSY;
    /* Refusal must leave EXITED unchanged while a stop waiter still needs it.
     * Enter the staged reaper only after all non-mutating admission checks. */
    if(r->joining || r->request || (r->task && taek_tasks_refs(r->id)>1))return -EBUSY;
    if(r->fiber){int state=kt.ops.estado(r->fiber);
        if(state!=-9 && state!=1 && state!=5 && state!=6)return -EBUSY;}
    r->phase=KT_REAP;return kt_reap(r);
}
void taek_kthreads_estadisticas(struct taek_kthreads_stats *out){
    if(!out)return;*out=kt.stats;out->vivos=out->pendientes=out->detenidos=out->salidos=out->joins=out->retenidos=0;
    for(unsigned i=0;i<TAEK_TASKS_SLOTS;i++){
        struct taek_kthread_record *r=&kt.records[i];if(!r->phase)continue;
        out->vivos++;out->pendientes+=!!r->request;out->detenidos+=r->phase==KT_READY && !r->started;
        out->salidos+=r->phase==KT_EXITED;out->joins+=!!r->joining;
        out->retenidos+=r->phase==KT_FAILED || r->phase==KT_REAP;
    }
}
/* PREEMPT_NONE: original worker reaches this safe point without held locks.
 * This yields the actual current fiber; it does not fabricate progress. */
int __cond_resched(void){if(taek_tasks_yield())kt_fatal(107);return 1;}
/* UP cooperativo: una fibra fuera de CPU con estado coincidente ya está
 * inactiva. No implementa afinidad ni admite PARKED del perfil Linux. */
unsigned long wait_task_inactive(struct task_struct *t,unsigned int match_state){
    if(!kt.active || taek_tasks_contexto()!=1)return 0;
    if(match_state!=TASK_INTERRUPTIBLE && match_state!=TASK_UNINTERRUPTIBLE && match_state!=TASK_IDLE)return 0;
    for(unsigned i=0;i<TAEK_TASKS_SLOTS;i++){
        struct taek_kthread_record *r=&kt.records[i];
        if(r->task!=t || !r->bound || current==t)continue;
        int state=kt.ops.estado(r->fiber);
        if(state!=4 || READ_ONCE(t->__state)!=match_state)return 0;
        return READ_ONCE(t->nvcsw)|(1ul<<(BITS_PER_LONG-1));
    }
    return 0;
}
void *memset(void *p,int c,size_t n){volatile unsigned char *b=p;while(n--)*b++=(unsigned char)c;return p;}
