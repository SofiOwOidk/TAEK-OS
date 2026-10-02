#define _GNU_SOURCE
#include "tareas.h"

// ----------------------------------------------------------------------------
// Asignación de memoria del shim: heap del núcleo en TAEK, malloc en host.
// ----------------------------------------------------------------------------
#ifndef TAEK_HOST_TEST
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#define i915_kmalloc(_n) asignar_memoria(_n)
#define i915_kfree(_p)   liberar_memoria(_p)
#else
#include <stdlib.h>
#include <unistd.h>
#define i915_kmalloc(_n) malloc(_n)
#define i915_kfree(_p)   free(_p)
#endif

#define MAX_TAREAS_I915 32

static struct task_struct g_tabla_tareas[MAX_TAREAS_I915];
static struct i915_cpu    g_cpu[I915_MAX_CPU];
static bool               g_sched_iniciado = false;
static atomic_t           g_tareas_vivas = ATOMIC_INIT(0);

/* Selector de CPU lógica del planificador. En host es por hilo (TLS): cada
 * pthread tiene el suyo. En el núcleo es por APIC local, con registro explícito
 * slot<->apic; así no hay un único selector global compartido entre CPUs. */
#ifdef TAEK_HOST_TEST
static __thread unsigned g_cpu_sel = 0;
static unsigned cpu_sel_leer(void) { return g_cpu_sel; }
static void     cpu_sel_escribir(unsigned c) { if (c < I915_MAX_CPU) g_cpu_sel = c; }
void i915_scheduler_registrar_cpu(unsigned slot, unsigned apic_id) { (void)slot; (void)apic_id; }
#else
static unsigned g_slot_por_apic[256];
static unsigned i915_apic_local(void) {
    unsigned a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (b >> 24) & 0xFF;
}
static unsigned cpu_sel_leer(void) {
    unsigned s = g_slot_por_apic[i915_apic_local()];
    return s < I915_MAX_CPU ? s : 0;
}
static void cpu_sel_escribir(unsigned c) {
    if (c < I915_MAX_CPU) g_slot_por_apic[i915_apic_local()] = c;
}
void i915_scheduler_registrar_cpu(unsigned slot, unsigned apic_id) {
    if (apic_id < 256 && slot < I915_MAX_CPU) g_slot_por_apic[apic_id] = slot;
}
#endif

// ----------------------------------------------------------------------------
// CAMBIO DE CONTEXTO REAL (x86-64 System V)
// Guarda los registros callee-saved en la pila actual, publica el rsp en
// *desde y carga el rsp de la tarea destino; al volver, la tarea destino
// continúa donde quedó. No usa SIMD ni estado flotante.
// ----------------------------------------------------------------------------
__attribute__((naked)) static void i915_cambiar_contexto(
    void **desde __attribute__((unused)), void *hacia __attribute__((unused))) {
    __asm__ __volatile__(
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "movq %rsp, (%rdi)\n\t"
        "movq %rsi, %rsp\n\t"
        "popq %r15\n\t"
        "popq %r14\n\t"
        "popq %r13\n\t"
        "popq %r12\n\t"
        "popq %rbx\n\t"
        "popq %rbp\n\t"
        "retq\n\t"
    );
}

void i915_tarea_terminar(void);
void i915_tarea_iniciar_ejecucion(struct task_struct *t);

/* Punto de entrada de una tarea nueva: r12 lleva su puntero (ver creación). */
__attribute__((naked, used)) static void i915_trampoline(void) {
    __asm__ __volatile__(
        "movq %r12, %rdi\n\t"
        "call i915_tarea_iniciar_ejecucion\n\t"
        "ud2\n\t"
    );
}

void i915_tarea_iniciar_ejecucion(struct task_struct *t) {
    if (t && t->fn) t->fn(t->data);
    i915_tarea_terminar();
}

// ----------------------------------------------------------------------------
// ESTADO POR CPU
// ----------------------------------------------------------------------------
struct i915_cpu *i915_cpu_estado(unsigned cpu) {
    if (cpu >= I915_MAX_CPU) return NULL;
    return &g_cpu[cpu];
}

void i915_scheduler_cpu(unsigned cpu) { cpu_sel_escribir(cpu); }

unsigned i915_scheduler_cpu_actual(void) { return cpu_sel_leer(); }

struct task_struct *i915_cpu_actual(unsigned cpu) {
    return cpu < I915_MAX_CPU ? g_cpu[cpu].actual : NULL;
}

void i915_tareas_iniciar(void) {
    if (g_sched_iniciado) return;
    for (unsigned c = 0; c < I915_MAX_CPU; c++) {
        spin_lock_init(&g_cpu[c].lock);
        INIT_LIST_HEAD(&g_cpu[c].runq);
        g_cpu[c].actual = NULL;
        g_cpu[c].id     = c;
    }
    for (int i = 0; i < MAX_TAREAS_I915; i++) {
        g_tabla_tareas[i].id         = i;
        g_tabla_tareas[i].state      = TASK_DEAD;
        g_tabla_tareas[i].stack_base = NULL;
        g_tabla_tareas[i].stack_size = 0;
        g_tabla_tareas[i].rsp        = NULL;
        g_tabla_tareas[i].fn         = NULL;
        g_tabla_tareas[i].data       = NULL;
        g_tabla_tareas[i].name       = NULL;
        g_tabla_tareas[i].cpu_id     = 0;
        INIT_LIST_HEAD(&g_tabla_tareas[i].node);
        INIT_LIST_HEAD(&g_tabla_tareas[i].wait_node);
    }
    // Tarea 0: contexto que ya está ejecutándose (BSP o hilo de prueba).
    // NO se encola: es la tarea actual, no una candidata.
    g_tabla_tareas[0].state  = TASK_RUNNING;
    g_tabla_tareas[0].name   = "i915_main";
    g_tabla_tareas[0].cpu_id = 0;
    INIT_LIST_HEAD(&g_tabla_tareas[0].node);
    g_cpu[0].actual = &g_tabla_tareas[0];
    atomic_set(&g_tareas_vivas, 1);
    g_sched_iniciado = true;
}

static struct task_struct *tomar_libre(void) {
    for (int i = 1; i < MAX_TAREAS_I915; i++) {
        if (g_tabla_tareas[i].state == TASK_DEAD && g_tabla_tareas[i].stack_base == NULL) {
            return &g_tabla_tareas[i];
        }
    }
    return NULL;
}

struct task_struct *i915_tarea_actual(void) {
    if (!g_sched_iniciado) i915_tareas_iniciar();
    return g_cpu[cpu_sel_leer()].actual;
}

struct task_struct *i915_tarea_crear(void (*fn)(void *), void *data,
                                     const char *name, unsigned cpu) {
    if (!g_sched_iniciado) i915_tareas_iniciar();
    if (!fn || cpu >= I915_MAX_CPU) return NULL;

    unsigned long flags;
    spin_lock_irqsave(&g_cpu[cpu].lock, flags);
    struct task_struct *t = tomar_libre();
    if (!t) {
        spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
        return NULL;
    }
    void *stack = i915_kmalloc(TASK_STACK_SIZE_BYTES);
    if (!stack) {
        spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
        return NULL;
    }

    t->state      = TASK_RUNNING;
    t->stack_base = stack;
    t->stack_size = TASK_STACK_SIZE_BYTES;
    t->fn         = fn;
    t->data       = data;
    t->name       = name ? name : "i915_tarea";
    t->cpu_id     = cpu;
    INIT_LIST_HEAD(&t->node);
    INIT_LIST_HEAD(&t->wait_node);

    // Marco inicial: i915_cambiar_contexto hace pop de r15..rbp y retq.
    uintptr_t tope = ((uintptr_t)stack + TASK_STACK_SIZE_BYTES) & ~(uintptr_t)0xF;
    uint64_t *sp = (uint64_t *)tope;
    *--sp = (uint64_t)(uintptr_t)i915_trampoline; // retq
    *--sp = 0;                                    // rbp
    *--sp = 0;                                    // rbx
    *--sp = (uint64_t)(uintptr_t)t;               // r12 = tarea
    *--sp = 0;                                    // r13
    *--sp = 0;                                    // r14
    *--sp = 0;                                    // r15
    t->rsp = sp;

    list_add_tail(&t->node, &g_cpu[cpu].runq);
    atomic_inc(&g_tareas_vivas);
    spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
    return t;
}

void i915_tarea_destruir(struct task_struct *t) {
    if (!t || t->id == 0) return;
    unsigned cpu = t->cpu_id;
    unsigned long flags;
    spin_lock_irqsave(&g_cpu[cpu].lock, flags);
    if (t->state == TASK_RUNNING && g_cpu[cpu].actual == t) {
        // No se libera una pila en uso; marcar y dejar que termine.
        spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
        return;
    }
    if (!list_empty(&t->node)) list_del_init(&t->node);
    if (!list_empty(&t->wait_node)) list_del_init(&t->wait_node);
    if (t->stack_base) {
        i915_kfree(t->stack_base);
        t->stack_base = NULL;
    }
    t->state  = TASK_DEAD;
    t->fn     = NULL;
    t->data   = NULL;
    t->rsp    = NULL;
    spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
}

void i915_tarea_dormir(void) {
    struct task_struct *curr = i915_tarea_actual();
    if (!curr) return;
    curr->state = TASK_UNINTERRUPTIBLE;
    i915_schedule();
}

void i915_tarea_despertar(struct task_struct *t) {
#ifdef I915_PRUEBA_SIN_WAKEUP
    /* Mutante de prueba: el despertar se elimina a propósito. */
    (void)t;
    return;
#endif
    if (!t || t->state == TASK_DEAD) return;
    unsigned cpu = t->cpu_id;
    unsigned long flags;
    spin_lock_irqsave(&g_cpu[cpu].lock, flags);
    if (t->state != TASK_RUNNING) {
        t->state = TASK_RUNNING;
        if (t != g_cpu[cpu].actual && list_empty(&t->node)) {
            list_add_tail(&t->node, &g_cpu[cpu].runq);
        }
    }
    spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
}

void i915_tarea_ceder(void) { i915_schedule(); }

void i915_schedule(void) {
    if (!g_sched_iniciado) i915_tareas_iniciar();
    unsigned cpu = cpu_sel_leer();
    unsigned long flags;
    spin_lock_irqsave(&g_cpu[cpu].lock, flags);

    for (;;) {
        struct task_struct *prev = g_cpu[cpu].actual;
        struct task_struct *next = NULL;

        if (!list_empty(&g_cpu[cpu].runq)) {
            next = list_first_entry(&g_cpu[cpu].runq, struct task_struct, node);
            list_del_init(&next->node);
        }

        if (next && next != prev) {
            if (prev && prev->state == TASK_RUNNING && list_empty(&prev->node)) {
                list_add_tail(&prev->node, &g_cpu[cpu].runq);
            }
            g_cpu[cpu].actual = next;
            spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
            i915_cambiar_contexto(&prev->rsp, next->rsp);
            return;
        }

        // next == NULL o next == prev
        if (prev && prev->state == TASK_RUNNING) {
            // Tarea ejecutable: no durmió o fue despertada en la ventana previa al sueño
            spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
            return;
        }

        // prev->state != TASK_RUNNING y runq vacía: suspensión sin tareas disponibles
        // La CPU debe esperar en reposo (idle) a que una interrupción o evento despierte una tarea
#ifdef TAEK_HOST_TEST
        spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
        usleep(50);
        spin_lock_irqsave(&g_cpu[cpu].lock, flags);
#else
        spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
        __asm__ volatile ("sti; hlt" ::: "memory");
        spin_lock_irqsave(&g_cpu[cpu].lock, flags);
#endif
    }
}

void i915_tarea_terminar(void) {
    struct task_struct *curr = i915_tarea_actual();
    if (curr) curr->state = TASK_DEAD;
    i915_schedule();     /* nunca debería volver a una tarea muerta */
    for (;;) __asm__ __volatile__("pause");
}

// ============================================================================
// MUTEX BLOQUEANTE
// ============================================================================
void i915_mutex_init(struct mutex *lock) {
    if (!lock) return;
    atomic_set(&lock->count, 1);
    spin_lock_init(&lock->wait_lock);
    lock->owner = NULL;
    INIT_LIST_HEAD(&lock->wait_list);
}

int i915_mutex_trylock(struct mutex *lock) {
    if (!lock) return 0;
    unsigned long flags;
    spin_lock_irqsave(&lock->wait_lock, flags);
    if (atomic_read(&lock->count) == 1) {
        atomic_set(&lock->count, 0);
        lock->owner = i915_tarea_actual();
        spin_unlock_irqrestore(&lock->wait_lock, flags);
        return 1;
    }
    spin_unlock_irqrestore(&lock->wait_lock, flags);
    return 0;
}

void i915_mutex_lock(struct mutex *lock) {
    if (!lock) return;
    struct task_struct *curr = i915_tarea_actual();
    for (;;) {
        unsigned long flags;
        spin_lock_irqsave(&lock->wait_lock, flags);
        if (atomic_read(&lock->count) == 1) {
            atomic_set(&lock->count, 0);
            lock->owner = curr;
            spin_unlock_irqrestore(&lock->wait_lock, flags);
            return;
        }
        if (curr) {
            curr->state = TASK_UNINTERRUPTIBLE;
            if (list_empty(&curr->wait_node)) list_add_tail(&curr->wait_node, &lock->wait_list);
        }
        spin_unlock_irqrestore(&lock->wait_lock, flags);
        i915_schedule();
        if (curr) {
            spin_lock_irqsave(&lock->wait_lock, flags);
            if (!list_empty(&curr->wait_node)) list_del_init(&curr->wait_node);
            spin_unlock_irqrestore(&lock->wait_lock, flags);
        }
    }
}

void i915_mutex_unlock(struct mutex *lock) {
    if (!lock) return;
    unsigned long flags;
    spin_lock_irqsave(&lock->wait_lock, flags);
    atomic_set(&lock->count, 1);
    lock->owner = NULL;
    if (!list_empty(&lock->wait_list)) {
        struct task_struct *t = list_first_entry(&lock->wait_list, struct task_struct, wait_node);
        list_del_init(&t->wait_node);
        i915_tarea_despertar(t);
    }
    spin_unlock_irqrestore(&lock->wait_lock, flags);
}

int i915_mutex_is_locked(struct mutex *lock) {
    return lock ? (atomic_read(&lock->count) <= 0) : 0;
}

// ============================================================================
// WAITQUEUES
// ============================================================================
void i915_init_waitqueue_head(wait_queue_head_t *q) {
    if (!q) return;
    spin_lock_init(&q->lock);
    INIT_LIST_HEAD(&q->head);
}

void i915_prepare_to_wait(wait_queue_head_t *q, wait_queue_entry_t *entry, int state) {
    if (!q || !entry) return;
    unsigned long flags;
    spin_lock_irqsave(&q->lock, flags);
    entry->flags = 0;
    if (list_empty(&entry->entry)) list_add_tail(&entry->entry, &q->head);
    if (entry->task) entry->task->state = state;
    spin_unlock_irqrestore(&q->lock, flags);
}

void i915_finish_wait(wait_queue_head_t *q, wait_queue_entry_t *entry) {
    if (!q || !entry) return;
    unsigned long flags;
    spin_lock_irqsave(&q->lock, flags);
    if (!list_empty(&entry->entry)) list_del_init(&entry->entry);
    if (entry->task) entry->task->state = TASK_RUNNING;
    spin_unlock_irqrestore(&q->lock, flags);
}

void i915_wake_up(wait_queue_head_t *q) {
    if (!q) return;
    unsigned long flags;
    spin_lock_irqsave(&q->lock, flags);
    if (!list_empty(&q->head)) {
        wait_queue_entry_t *e = list_first_entry(&q->head, wait_queue_entry_t, entry);
        list_del_init(&e->entry);
        if (e->task) i915_tarea_despertar(e->task);
    }
    spin_unlock_irqrestore(&q->lock, flags);
}

void i915_wake_up_all(wait_queue_head_t *q) {
    if (!q) return;
    unsigned long flags;
    spin_lock_irqsave(&q->lock, flags);
    wait_queue_entry_t *pos, *n;
    list_for_each_entry_safe(pos, n, &q->head, entry) {
        list_del_init(&pos->entry);
        if (pos->task) i915_tarea_despertar(pos->task);
    }
    spin_unlock_irqrestore(&q->lock, flags);
}

// ============================================================================
// WORKQUEUES
// ============================================================================
static void worker_thread_fn(void *data) {
    struct workqueue_struct *wq = (struct workqueue_struct *)data;
    if (!wq) return;
    for (;;) {
        struct work_struct *work = NULL;
        unsigned long flags;
        spin_lock_irqsave(&wq->lock, flags);
        for (;;) {
            if (!list_empty(&wq->work_list)) {
                work = list_first_entry(&wq->work_list, struct work_struct, entry);
                list_del_init(&work->entry);
                atomic_set(&work->data, (1 << WORK_STRUCT_RUNNING_BIT));
                atomic_dec(&wq->pending);
                break;
            }
            if (!wq->running) {
                spin_unlock_irqrestore(&wq->lock, flags);
                i915_tarea_terminar();   /* no retorna */
            }
            /* Dormir SIN ventana: estado y comprobación bajo el MISMO cerrojo
             * que usa queue_work para encolar y despertar. Si el encolado llega
             * después, despertar cambia el estado; si llega antes, la
             * comprobación posterior lo ve. */
            struct task_struct *curr = i915_tarea_actual();
            if (curr) curr->state = TASK_UNINTERRUPTIBLE;
            if (!list_empty(&wq->work_list) || !wq->running) {
                if (curr) curr->state = TASK_RUNNING;
                continue;
            }
            spin_unlock_irqrestore(&wq->lock, flags);
            i915_schedule();
            spin_lock_irqsave(&wq->lock, flags);
        }
        spin_unlock_irqrestore(&wq->lock, flags);

        if (work->func) work->func(work);

        spin_lock_irqsave(&wq->lock, flags);
        int cur_st = atomic_read(&work->data);
        /* Retirar exclusivamente el bit RUNNING, preservando PENDING si fue reencolado en vuelo */
        atomic_set(&work->data, cur_st & ~(1 << WORK_STRUCT_RUNNING_BIT));
        atomic_dec(&wq->active_jobs);
        spin_unlock_irqrestore(&wq->lock, flags);
        i915_wake_up_all(&wq->wait_idle);
    }
}

struct workqueue_struct *i915_create_singlethread_workqueue(const char *name) {
    struct workqueue_struct *wq = i915_kmalloc(sizeof(struct workqueue_struct));
    if (!wq) return NULL;
    spin_lock_init(&wq->lock);
    INIT_LIST_HEAD(&wq->work_list);
    i915_init_waitqueue_head(&wq->wait_idle);
    atomic_set(&wq->active_jobs, 0);
    atomic_set(&wq->pending, 0);
    wq->running    = true;
    wq->destruida  = false;
    wq->name       = name ? name : "i915_wq";
    wq->worker_task = i915_tarea_crear(worker_thread_fn, wq, wq->name, i915_scheduler_cpu_actual());
    if (!wq->worker_task) {
        i915_kfree(wq);
        return NULL;
    }
    return wq;
}

bool i915_queue_work(struct workqueue_struct *wq, struct work_struct *work) {
    if (!wq || !work) return false;
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    if (!wq->running || wq->destruida) {
        spin_unlock_irqrestore(&wq->lock, flags);
        return false;
    }
    int data = atomic_read(&work->data);
    if (data & (1 << WORK_STRUCT_PENDING_BIT)) {
        spin_unlock_irqrestore(&wq->lock, flags);
        return false;   // ya encolado
    }
    atomic_set(&work->data, data | (1 << WORK_STRUCT_PENDING_BIT));
    work->wq = wq;
    list_add_tail(&work->entry, &wq->work_list);
    atomic_inc(&wq->pending);
    atomic_inc(&wq->active_jobs);
    i915_tarea_despertar(wq->worker_task);
    spin_unlock_irqrestore(&wq->lock, flags);
    return true;
}

bool i915_cancel_work_sync(struct work_struct *work) {
    if (!work || !work->wq) return false;
    struct workqueue_struct *wq = work->wq;
    unsigned long flags;
    bool cancelled_pending = false;

    spin_lock_irqsave(&wq->lock, flags);
    int data = atomic_read(&work->data);
    if (data & (1 << WORK_STRUCT_PENDING_BIT)) {
        list_del_init(&work->entry);
        // Desactivar SOLAMENTE el bit PENDING, preservando RUNNING si estaba activo
        atomic_set(&work->data, data & ~(1 << WORK_STRUCT_PENDING_BIT));
        atomic_dec(&wq->pending);
        atomic_dec(&wq->active_jobs);
        cancelled_pending = true;
    }
    spin_unlock_irqrestore(&wq->lock, flags);

    if (cancelled_pending) {
        i915_wake_up_all(&wq->wait_idle);
    }

    // Si se está ejecutando (RUNNING activo), esperar síncronamente su terminación.
    while (atomic_read(&work->data) & (1 << WORK_STRUCT_RUNNING_BIT)) {
        i915_tarea_ceder();
    }
    return cancelled_pending;
}

void i915_flush_workqueue(struct workqueue_struct *wq) {
    if (!wq) return;
    while (atomic_read(&wq->active_jobs) > 0) {
        i915_tarea_ceder();
    }
}

void i915_destroy_workqueue(struct workqueue_struct *wq) {
    if (!wq) return;
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    wq->running = false;
    spin_unlock_irqrestore(&wq->lock, flags);
    i915_tarea_despertar(wq->worker_task);

    // Drenar trabajos en vuelo y esperar a que el worker termine.
    i915_flush_workqueue(wq);
    while (wq->worker_task && wq->worker_task->state != TASK_DEAD) {
        i915_tarea_ceder();
    }
    struct task_struct *w = wq->worker_task;
    wq->destruida = true;
    wq->worker_task = NULL;
    i915_tarea_destruir(w);
    i915_kfree(wq);
}
