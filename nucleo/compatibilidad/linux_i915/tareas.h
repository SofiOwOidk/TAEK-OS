#ifndef LINUX_I915_TAREAS_H
#define LINUX_I915_TAREAS_H

#include "linux_types.h"

// ----------------------------------------------------------------------------
// ESTADOS DE TAREA (semántica Linux: RUNNING = ejecutable o ejecutándose)
// ----------------------------------------------------------------------------
#define TASK_RUNNING          0
#define TASK_INTERRUPTIBLE    1
#define TASK_UNINTERRUPTIBLE  2
#define TASK_DEAD             3

#define I915_MAX_CPU          4
#define TASK_STACK_SIZE_BYTES (64 * 1024) // 64 KiB de pila real por tarea

struct task_struct {
    int                 id;
    volatile int        state;
    void               *stack_base;
    size_t              stack_size;
    void               *rsp;        /* contexto guardado (sólo el planificador) */
    void              (*fn)(void *);
    void               *data;
    const char         *name;
    unsigned            cpu_id;     /* afinidad: CPU lógica propietaria */
    struct list_head    node;       /* enlace en la runqueue de su CPU */
    struct list_head    wait_node;  /* enlace en una lista de espera (mutex/wq) */
};

struct i915_cpu {
    spinlock_t          lock;
    struct list_head    runq;
    struct task_struct *actual;
    unsigned            id;
};

// ----------------------------------------------------------------------------
// PLANIFICADOR COOPERATIVO CON CAMBIO DE CONTEXTO REAL
// ----------------------------------------------------------------------------
void                i915_tareas_iniciar(void);
struct task_struct *i915_tarea_actual(void);
struct task_struct *i915_tarea_crear(void (*fn)(void *), void *data,
                                     const char *name, unsigned cpu);
void                i915_tarea_destruir(struct task_struct *t);
void                i915_tarea_dormir(void);
void                i915_tarea_despertar(struct task_struct *t);
void                i915_tarea_ceder(void);
void                i915_schedule(void);
void                i915_tarea_terminar(void);

/* Selección de CPU lógica para el planificador que llama (BSP/AP o test). */
void                i915_scheduler_cpu(unsigned cpu);
unsigned            i915_scheduler_cpu_actual(void);
/* Núcleo: asocia un slot lógico (0..3) a un APIC id para que cada CPU tenga su
 * propio estado; en host es no-op (el selector es TLS por hilo). */
void                i915_scheduler_registrar_cpu(unsigned slot, unsigned apic_id);
struct task_struct *i915_cpu_actual(unsigned cpu);
struct i915_cpu   *i915_cpu_estado(unsigned cpu);

/* ------------------------------------------------------------------------- */
/* MUTEX BLOQUEANTE (suspende la tarea; no consume CPU en espera)            */
/* ------------------------------------------------------------------------- */
struct mutex {
    atomic_t            count;
    spinlock_t          wait_lock;
    struct task_struct *owner;
    struct list_head    wait_list;
};

#define DEFINE_MUTEX(mutexname) \
    struct mutex mutexname = { ATOMIC_INIT(1), SPIN_LOCK_UNLOCKED, NULL, \
                               { &(mutexname).wait_list, &(mutexname).wait_list } }

#define mutex_init          i915_mutex_init
#define mutex_lock          i915_mutex_lock
#define mutex_trylock       i915_mutex_trylock
#define mutex_unlock        i915_mutex_unlock
#define mutex_is_locked     i915_mutex_is_locked

void i915_mutex_init(struct mutex *lock);
void i915_mutex_lock(struct mutex *lock);
int  i915_mutex_trylock(struct mutex *lock);
void i915_mutex_unlock(struct mutex *lock);
int  i915_mutex_is_locked(struct mutex *lock);

/* ------------------------------------------------------------------------- */
/* WAITQUEUES (registro previo; el llamador mantiene vivo el entry)          */
/* ------------------------------------------------------------------------- */
typedef struct wait_queue_entry {
    struct task_struct *task;
    struct list_head    entry;
    int                 flags;
} wait_queue_entry_t;

typedef struct wait_queue_head {
    spinlock_t          lock;
    struct list_head    head;
} wait_queue_head_t;

#define DECLARE_WAIT_QUEUE_HEAD(name) \
    wait_queue_head_t name = { SPIN_LOCK_UNLOCKED, { &(name).head, &(name).head } }

#define init_waitqueue_head i915_init_waitqueue_head
#define prepare_to_wait     i915_prepare_to_wait
#define finish_wait         i915_finish_wait
#define wake_up             i915_wake_up
#define wake_up_all         i915_wake_up_all

void i915_init_waitqueue_head(wait_queue_head_t *q);
void i915_prepare_to_wait(wait_queue_head_t *q, wait_queue_entry_t *entry, int state);
void i915_finish_wait(wait_queue_head_t *q, wait_queue_entry_t *entry);
void i915_wake_up(wait_queue_head_t *q);
void i915_wake_up_all(wait_queue_head_t *q);

/* ------------------------------------------------------------------------- */
/* WORKQUEUES (ciclo de vida PENDING/RUNNING, cancelación y flush)           */
/* ------------------------------------------------------------------------- */
#define WORK_STRUCT_PENDING_BIT 0
#define WORK_STRUCT_RUNNING_BIT 1

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
    atomic_t                 data;   /* bit 0 PENDING, bit 1 RUNNING */
    struct list_head         entry;
    work_func_t              func;
    struct workqueue_struct *wq;
};

#define INIT_WORK(_work, _func) \
    do { \
        atomic_set(&(_work)->data, 0); \
        INIT_LIST_HEAD(&(_work)->entry); \
        (_work)->func = (_func); \
        (_work)->wq = NULL; \
    } while (0)

struct workqueue_struct {
    spinlock_t          lock;
    struct list_head    work_list;
    struct task_struct *worker_task;
    wait_queue_head_t   wait_idle;
    atomic_t            active_jobs;   /* en cola o ejecutándose */
    atomic_t            pending;       /* encolados sin ejecutar */
    bool                running;
    bool                destruida;
    const char         *name;
};

#define create_singlethread_workqueue i915_create_singlethread_workqueue
#define destroy_workqueue             i915_destroy_workqueue
#define queue_work                    i915_queue_work
#define cancel_work_sync              i915_cancel_work_sync
#define flush_workqueue               i915_flush_workqueue

struct workqueue_struct *i915_create_singlethread_workqueue(const char *name);
void                     i915_destroy_workqueue(struct workqueue_struct *wq);
bool                     i915_queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool                     i915_cancel_work_sync(struct work_struct *work);
void                     i915_flush_workqueue(struct workqueue_struct *wq);

#endif // LINUX_I915_TAREAS_H
