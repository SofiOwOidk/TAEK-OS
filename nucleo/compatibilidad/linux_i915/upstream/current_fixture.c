/* Layouts/accessores originales Linux congelados. Es una prueba de current y
 * mutex fastpath, NO alloc_task_struct/kthread ni un task/DRM operativo. */
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/percpu.h>
#include <asm/current.h>
#include "../upstream_current.h"
DEFINE_PER_CPU_ALIGNED(struct pcpu_hot,pcpu_hot);
static struct task_struct identidades[3] __attribute__((aligned(64)));
static struct mutex lock;
int taek_original_mutex_try_fast(struct mutex *);
int taek_original_mutex_unlock_fast(struct mutex *);
int taek_current_fixture_iniciar(void){
    /* Toda fixture nace en BSS. Reiniciar solo cuando no hay propietario. */
    if(atomic_long_read(&lock.owner))return -1;
    for(unsigned i=0;i<3;i++){
        identidades[i].pid=1000+(int)i;WRITE_ONCE(identidades[i].__state,TASK_RUNNING);
        identidades[i].thread_info.flags=0;
    }
    __mutex_init(&lock,"current-fixture",NULL);
    WRITE_ONCE(pcpu_hot.current_task,&identidades[0]);return 0;
}
int taek_current_fixture_seleccionar(unsigned i){
    if(i>=3)return -1;WRITE_ONCE(pcpu_hot.current_task,&identidades[i]);return 0;
}
__attribute__((__noinline__)) int taek_current_fixture_verificar(unsigned i,int pid,unsigned long estado){
    if(i>=3)return -1;
    struct task_struct *t=current;
    return t==&identidades[i] && task_pid_nr(t)==pid && READ_ONCE(t->__state)==estado &&
        task_is_running(t)==(estado==TASK_RUNNING) && task_thread_info(t)==&t->thread_info &&
        READ_ONCE(task_thread_info(t)->flags)==0?0:-1;
}
int taek_current_fixture_estado(unsigned long estado){
    if(estado!=TASK_RUNNING && estado!=TASK_INTERRUPTIBLE)return -1;
    set_current_state(estado);return 0;
}
int taek_current_fixture_trylock(void){return taek_original_mutex_try_fast(&lock);}
int taek_current_fixture_unlock(void){return taek_original_mutex_unlock_fast(&lock);}
int taek_current_fixture_locked(void){return mutex_is_locked(&lock);}
unsigned long taek_current_fixture_owner(void){return atomic_long_read(&lock.owner);}
unsigned long taek_current_fixture_identidad(unsigned i){return i<3?(unsigned long)&identidades[i]:0;}
unsigned long taek_current_fixture_task_bytes(void){return sizeof(struct task_struct);}
unsigned long taek_current_fixture_task_align(void){return __alignof__(struct task_struct);}
