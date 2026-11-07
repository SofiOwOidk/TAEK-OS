/* Consumidor de ensayo: worker/queue/flush/stop originales, constructor
 * cooperativo explícito. No workqueue de pools ni probe Intel. */
#include <linux/kthread.h>
#include <linux/sched/task.h>
#include <linux/completion.h>
#include "../upstream_kthreads.h"
#include "../upstream_tasks.h"
static struct kthread_worker worker;
static struct kthread_work work_a,work_b,work_c;
static struct completion arrived,release,simple_executed;
static unsigned events,mask;
static unsigned long target,never_started,finished;
static int simple_calls;
static unsigned long joining_target;
static unsigned reap_join_checks;
static int stop_target(unsigned long id,int *result){
    joining_target=id;int rc=taek_kthread_detener(id,result);joining_target=0;return rc;
}
int taek_kthread_fixture_reap_en_join(void){
    if(taek_tasks_contexto()!=1 || taek_tasks_actual())return -1;
    if(!joining_target)return 0;
    struct taek_kthreads_stats st;taek_kthreads_estadisticas(&st);
    if(st.joins && st.salidos){
        if(taek_kthread_recolectar(joining_target)!=-EBUSY)return -2;
        reap_join_checks++;
    }
    return 0;
}
static void callback_a(struct kthread_work *work){
    if(work!=&work_a || !taek_tasks_actual() || kthread_data(current)!=&worker || !(current->flags&PF_KTHREAD))mask|=1;
    events++;complete(&arrived);wait_for_completion(&release);events++;
}
static void callback_b(struct kthread_work *work){if(work!=&work_b)mask|=2;events++;}
static void callback_c(struct kthread_work *work){if(work!=&work_c)mask|=4;events++;}
static int simple(void *p){if(p!=&simple_calls)mask|=8;simple_calls++;complete(&simple_executed);return 37;}
/* A live extra ref pins Task/payload until explicitly dropped by the root. */
static struct task_struct *extra;
int taek_kthread_fixture_probar(void){
    if(taek_tasks_contexto()!=1 || !taek_tasks_actual() || target || never_started || finished || extra)return -1;
    mask=events=0;simple_calls=0;reap_join_checks=0;init_completion(&arrived);init_completion(&release);init_completion(&simple_executed);
    kthread_init_worker(&worker);kthread_init_work(&work_a,callback_a);kthread_init_work(&work_b,callback_b);kthread_init_work(&work_c,callback_c);
    if(taek_kthread_crear(kthread_worker_fn,&worker,"taek-original-worker",&target))return -2;
    struct task_struct *t=taek_tasks_objeto(target);
    if(!t || t->worker_private==NULL || READ_ONCE(t->__state)!=TASK_UNINTERRUPTIBLE || kthread_data(t)!=&worker || worker.task)return -3;
    if(taek_kthread_despertar(target)!=1)return -4;
    if(!kthread_queue_work(&worker,&work_a) || kthread_queue_work(&worker,&work_a) || !kthread_queue_work(&worker,&work_b))mask|=16;
    wait_for_completion(&arrived);
    if(events!=1 || READ_ONCE(t->__state)!=TASK_UNINTERRUPTIBLE || worker.current_work!=&work_a || list_empty(&worker.work_list))mask|=32;
    complete(&release);kthread_flush_work(&work_a);kthread_flush_worker(&worker);
    if(events!=3 || !list_empty(&worker.work_list))mask|=64;
    if(!kthread_queue_work(&worker,&work_c))mask|=128;kthread_flush_work(&work_c);
    if(events!=4)mask|=256;
    int result=-99;
    if(stop_target(target,&result) || result || worker.task || !list_empty(&worker.work_list))mask|=512;
    if(taek_kthread_detener(target,&result)!=-EALREADY)mask|=16384;
    extra=t;get_task_struct(extra);
    if(taek_kthread_crear(simple,&simple_calls,"taek-stop-before-start",&never_started) ||
       stop_target(never_started,&result) || result!=-EINTR || simple_calls)mask|=1024;
    if(taek_kthread_crear(simple,&simple_calls,"taek-result-after-exit",&finished) || taek_kthread_despertar(finished)!=1)mask|=2048;
    /* Wait for actual callback progress rather than assuming a particular
     * round-robin activation follows a yield. The callback then returns. */
    wait_for_completion(&simple_executed);
    if(simple_calls!=1 || stop_target(finished,&result) || result!=37)mask|=8192;
    if(!reap_join_checks)mask|=32768;
    return (int)mask;
}
int taek_kthread_fixture_retirar(void){
    if(taek_tasks_contexto()!=1 || taek_tasks_actual())return -1;
    if(extra){
        if(taek_kthread_recolectar(target)!=-EBUSY)return -2;
        put_task_struct(extra);extra=NULL;
    }
    unsigned long *ids[3]={&target,&never_started,&finished};
    for(unsigned i=0;i<3;i++)if(*ids[i]){if(taek_kthread_recolectar(*ids[i]))return -3;*ids[i]=0;}
    return 0;
}
