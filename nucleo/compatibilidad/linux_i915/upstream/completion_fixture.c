/* Objetos y consumidores completion originales. La fachada solo admite el
 * ensayo cooperativo BSP; no ofrece señales, complete IRQ/AP ni workers. */
#include <linux/completion.h>
#include <linux/sched.h>
#include "../upstream_tasks.h"
#include "../upstream_completion.h"
static struct completion events[TAEK_COMPLETION_SLOTS];
static unsigned active,waiters[TAEK_COMPLETION_SLOTS];
static int ordinary(void){return taek_tasks_contexto()==1;}
static int valid(unsigned n){return ordinary() && active && n<TAEK_COMPLETION_SLOTS;}
static unsigned long flags(void){unsigned long f;asm volatile("pushfq; pop %0":"=r"(f)::"memory");return f;}
int taek_completion_iniciar(void){
    if(!ordinary() || taek_tasks_actual() || active)return -1;
    for(unsigned n=0;n<TAEK_COMPLETION_SLOTS;n++){init_completion(&events[n]);waiters[n]=0;}
    active=1;return 0;
}
int taek_completion_parar(void){
    if(!ordinary() || taek_tasks_actual() || !active)return -1;
    for(unsigned n=0;n<TAEK_COMPLETION_SLOTS;n++)if(waiters[n] || !list_empty(&events[n].wait.task_list))return -15;
    active=0;return 0;
}
int taek_completion_reset(unsigned n){
    if(!valid(n))return -4;
    if(waiters[n] || !list_empty(&events[n].wait.task_list))return -15;
    reinit_completion(&events[n]);return 0;
}
int taek_completion_complete(unsigned n,int all){
    if(!valid(n) || (all!=0 && all!=1))return -4;
    if(all)complete_all(&events[n]);else complete(&events[n]);return 0;
}
long taek_completion_wait(unsigned n,long timeout){
    if(!valid(n) || !taek_tasks_actual() || !(flags()&512) || timeout< -1)return -4;
    waiters[n]++;
    long r;if(timeout==-1){wait_for_completion(&events[n]);r=1;}
    else r=wait_for_completion_timeout(&events[n],(unsigned long)timeout);
    waiters[n]--;return r;
}
int taek_completion_try(unsigned n){return valid(n)?try_wait_for_completion(&events[n]):-4;}
int taek_completion_done(unsigned n){return valid(n)?completion_done(&events[n]):-4;}
int taek_completion_waiters(unsigned n){return valid(n)?(int)waiters[n]:-4;}
unsigned long taek_completion_count(unsigned n){return valid(n)?READ_ONCE(events[n].done):~0ul;}
