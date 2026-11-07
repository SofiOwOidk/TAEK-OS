#define _GNU_SOURCE
#include "tareas.h"

// ----------------------------------------------------------------------------
// Asignación de memoria del shim: heap del núcleo en TAEK, malloc en host.
// ----------------------------------------------------------------------------
#ifndef TAEK_HOST_TEST
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#include "arquitectura/x86_64/fpu.h"
#include "arquitectura/x86_64/tls.h"
#include "base/trabajos.h"
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
static bool               g_descriptor_ocupado[MAX_TAREAS_I915];
static struct i915_cpu    g_cpu[I915_MAX_CPU];
static struct task_struct g_raiz[I915_MAX_CPU];
static unsigned           g_sched_iniciado;
static spinlock_t         g_tabla_lock=SPIN_LOCK_UNLOCKED;
static spinlock_t         g_registro_lock=SPIN_LOCK_UNLOCKED;
static unsigned           g_cpu_registrada[I915_MAX_CPU],g_apic_por_slot[I915_MAX_CPU];
static atomic_t           g_tareas_vivas = ATOMIC_INIT(0);

/* Selector de CPU lógica del planificador. En host es por hilo (TLS): cada
 * pthread tiene el suyo. En el núcleo es por APIC local, con registro explícito
 * slot<->apic; así no hay un único selector global compartido entre CPUs. */
#ifdef TAEK_HOST_TEST
static __thread unsigned g_cpu_sel = 0;
static unsigned cpu_sel_leer(void) { return g_cpu_sel; }
static int cpu_sel_escribir(unsigned c) {
    g_cpu_sel=c<I915_MAX_CPU?c:I915_CPU_INVALIDA;return c<I915_MAX_CPU?0:-1;
}
#else
static unsigned i915_apic_local(void) {
    unsigned a,b,c,d,max;
    __asm__ volatile("cpuid":"=a"(max),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));
    if(max>=11){
        __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(11),"c"(0));
        if(b)return d; /* ID x2APIC completo, sin truncarlo a ocho bits. */
    }
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    return b>>24;
}
static unsigned cpu_sel_leer(void) {
    unsigned id=i915_apic_local();
    for(unsigned s=0;s<I915_MAX_CPU;s++)
        if(__atomic_load_n(&g_cpu_registrada[s],__ATOMIC_ACQUIRE) && g_apic_por_slot[s]==id)return s;
    return I915_CPU_INVALIDA;
}
static int cpu_sel_escribir(unsigned c) {
    /* La afinidad nativa se registra durante el arranque; no renombrar CPU
     * en ejecucion mediante el selector que emula CPUs en pruebas host. */
    return c<I915_MAX_CPU && c==cpu_sel_leer()?0:-1;
}
#endif

// ----------------------------------------------------------------------------
// CAMBIO DE CONTEXTO REAL (x86-64 System V)
// Guarda los registros callee-saved en la pila actual, publica el rsp en
// *desde y carga el rsp de la tarea destino; al volver, la tarea destino
// continúa donde quedó, con x87/MMX/MXCSR/XMM0..15 propios.
// ----------------------------------------------------------------------------
_Static_assert(sizeof(struct i915_contexto_marco)==608,"context frame assembly layout");
_Static_assert(offsetof(struct i915_contexto_marco,registros_rsp)==512,"saved stack pointer");
_Static_assert(offsetof(struct i915_contexto_marco,traza)==544,"integer frame offset");
_Static_assert(offsetof(struct i915_contexto_marco,fs_base)==520,"native TLS frame offset");
_Static_assert(offsetof(struct i915_contexto_marco,irq_flags)==528,"IRQ frame offset");
_Static_assert(sizeof(struct i915_contexto_traza)==64,"context trace assembly layout");
#ifndef I915_TEST_OMIT_RESTORE
#define I915_TEST_OMIT_RESTORE 0
#endif
__attribute__((naked)) void i915_cambiar_contexto(
    void **desde __attribute__((unused)), void *hacia __attribute__((unused)),
    struct i915_contexto_traza *traza __attribute__((unused)),
    uint64_t irq_flags __attribute__((unused))) {
    __asm__ __volatile__(
        "pushfq\n\t"
        "popq %r9\n\t"
        "testq %rcx, %rcx\n\t"
        "cmovnzq %rcx, %r9\n\t"
#ifndef TAEK_HOST_TEST
        "cli\n\t"
#endif
        "testq %rdx, %rdx\n\t"
        "jz 1f\n\t"
        "movq %rsp, (%rdx)\n\t"
        "1:\n\t"
        "pushq %rbp\n\t"
        "pushq %rbx\n\t"
        "pushq %r12\n\t"
        "pushq %r13\n\t"
        "pushq %r14\n\t"
        "pushq %r15\n\t"
        "pushq %rdx\n\t"
        "movq %rsp, %r10\n\t"
        "subq $544, %rsp\n\t"
        "andq $-16, %rsp\n\t"
        "movq %r10, 512(%rsp)\n\t"
        "movq %r9, 528(%rsp)\n\t"
#ifndef TAEK_HOST_TEST
        "movl $0xc0000100, %ecx\n\t"
        "rdmsr\n\t"
        "shlq $32, %rdx\n\t"
        "orq %rdx, %rax\n\t"
        "movq %rax, 520(%rsp)\n\t"
#endif
#ifdef I915_TEST_SKIP_FXSAVE
        /* Valid empty state makes omitted-save sensitivity measurable rather
         * than faulting on arbitrary MXCSR bytes in an uninitialized stack. */
        "movq %rdi, %r8\n\t"
        "movq %rsp, %rdi\n\t"
        "xorl %eax, %eax\n\t"
        "movl $64, %ecx\n\t"
        "rep stosq\n\t"
        "movq %r8, %rdi\n\t"
        "movw $0x37f, (%rsp)\n\t"
        "movl $0x1f80, 24(%rsp)\n\t"
#else
        "fxsave64 (%rsp)\n\t"
#endif
        "movq %rsp, (%rdi)\n\t"
        "movq %rsi, %rsp\n\t"
#if !defined(TAEK_HOST_TEST) && !defined(I915_TEST_SKIP_TLS_RESTORE)
        "movq 520(%rsp), %rax\n\t"
        "movq %rax, %rdx\n\t"
        "shrq $32, %rdx\n\t"
        "movl $0xc0000100, %ecx\n\t"
        "wrmsr\n\t"
#endif
#ifndef I915_TEST_SKIP_FXRSTOR
        "fxrstor64 (%rsp)\n\t"
#endif
#ifdef I915_TEST_CORRUPT_XMM15
        "pxor %xmm15, %xmm15\n\t"
#endif
#ifdef I915_TEST_CORRUPT_X87
        "fninit\n\t"
#endif
#ifdef I915_TEST_CORRUPT_MXCSR
        "subq $16, %rsp\n\t"
        "movl $0x1f80, (%rsp)\n\t"
        "ldmxcsr (%rsp)\n\t"
        "addq $16, %rsp\n\t"
#endif
        "movq 528(%rsp), %r9\n\t"
        "movq 512(%rsp), %rsp\n\t"
        "popq %r11\n\t"
#if I915_TEST_OMIT_RESTORE & 32
        "addq $8, %rsp\n\t"
#else
        "popq %r15\n\t"
#endif
#if I915_TEST_OMIT_RESTORE & 16
        "addq $8, %rsp\n\t"
#else
        "popq %r14\n\t"
#endif
#if I915_TEST_OMIT_RESTORE & 8
        "addq $8, %rsp\n\t"
#else
        "popq %r13\n\t"
#endif
#if I915_TEST_OMIT_RESTORE & 4
        "addq $8, %rsp\n\t"
#else
        "popq %r12\n\t"
#endif
#if I915_TEST_OMIT_RESTORE & 2
        "addq $8, %rsp\n\t"
#else
        "popq %rbx\n\t"
#endif
#if I915_TEST_OMIT_RESTORE & 1
        "addq $8, %rsp\n\t"
#else
        "popq %rbp\n\t"
#endif
        "testq %r11, %r11\n\t"
        "jz 2f\n\t"
        "movq %rsp, 8(%r11)\n\t"
        "movq %rbp, 16(%r11)\n\t"
        "movq %rbx, 24(%r11)\n\t"
        "movq %r12, 32(%r11)\n\t"
        "movq %r13, 40(%r11)\n\t"
        "movq %r14, 48(%r11)\n\t"
        "movq %r15, 56(%r11)\n\t"
        "2:\n\t"
#if !defined(TAEK_HOST_TEST) && !defined(I915_TEST_SKIP_IRQ_RESTORE)
        "testq $512, %r9\n\t"
        "jz 3f\n\t"
        /* STI shadow covers RET; no maskable IRQ sees half-restored state. */
        "sti\n\t"
        "3:\n\t"
#endif
        "retq\n\t"
    );
}

int i915_contexto_sse2_listo(void) {
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    if((d&((1u<<24)|(1u<<25)|(1u<<26)))!=((1u<<24)|(1u<<25)|(1u<<26)))return 0;
#ifndef TAEK_HOST_TEST
    uint64_t cr0,cr4;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    __asm__ volatile("mov %%cr4,%0":"=r"(cr4));
    if((cr0&12) || (cr4&((1u<<9)|(1u<<10)))!=((1u<<9)|(1u<<10)))return 0;
    if(x86_64_fpu_capacidades_cpu().ymm_habilitado)return 0;
#endif
    return 1;
}
#ifdef I915_TEST_BAD_INITIAL_ALIGNMENT
__attribute__((naked,used)) static void i915_entrada_desalineada(void){
    __asm__ volatile("retq\n\t");
}
#endif

struct i915_contexto_marco *i915_contexto_preparar(void *stack,size_t bytes,
                                                void (*entrada)(void),uint64_t r12) {
    if(!i915_contexto_sse2_listo() || !stack || !entrada || bytes<sizeof(struct i915_contexto_marco)+32 ||
       (uintptr_t)stack>UINTPTR_MAX-bytes)return NULL;
    uintptr_t top=((uintptr_t)stack+bytes)&~(uintptr_t)15;
#ifdef I915_TEST_BAD_INITIAL_ALIGNMENT
    /* Keep FXSAVE storage aligned; an extra real return shifts the initial
     * trampoline stack by 8. The probe detects the ABI fault without #GP. */
    top-=16;
#endif
    struct i915_contexto_marco *frame=(void *)(top-sizeof(*frame));
    *frame=(struct i915_contexto_marco){.r12=r12,.entrada=entrada};
    frame->registros_rsp=&frame->traza;
#ifndef TAEK_HOST_TEST
    frame->fs_base=taek_tls_base_actual();
#endif
    __asm__ volatile("pushfq; popq %0":"=r"(frame->irq_flags)::"memory");
    frame->fpu[0]=0x7f;frame->fpu[1]=0x03; /* FCW: masked, round-nearest */
    frame->fpu[24]=0x80;frame->fpu[25]=0x1f; /* MXCSR default */
#ifdef I915_TEST_BAD_INITIAL_ALIGNMENT
    *(void (**)(void))(frame+1)=entrada;
    frame->entrada=i915_entrada_desalineada;
#endif
    return frame;
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

int i915_scheduler_cpu(unsigned cpu) { return cpu_sel_escribir(cpu); }

unsigned i915_scheduler_cpu_actual(void) { return cpu_sel_leer(); }

struct task_struct *i915_cpu_actual(unsigned cpu) {
    return cpu < I915_MAX_CPU ? g_cpu[cpu].actual : NULL;
}

void i915_tareas_iniciar(void) {
    if(__atomic_load_n(&g_sched_iniciado,__ATOMIC_ACQUIRE)==2)return;
#ifndef TAEK_HOST_TEST
    if(!trabajos_es_coordinador())return;
#endif
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&g_sched_iniciado,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)){
        while(__atomic_load_n(&g_sched_iniciado,__ATOMIC_ACQUIRE)==1)__asm__ volatile("pause");
        return;
    }
    for (unsigned c = 0; c < I915_MAX_CPU; c++) {
        spin_lock_init(&g_cpu[c].lock);
        INIT_LIST_HEAD(&g_cpu[c].runq);
        g_cpu[c].actual = NULL;
        g_cpu[c].id     = c;
        g_raiz[c]=(struct task_struct){.id=-(int)c,.state=TASK_RUNNING,
                                      .name="i915_raiz_cpu",.cpu_id=c};
        INIT_LIST_HEAD(&g_raiz[c].node);INIT_LIST_HEAD(&g_raiz[c].wait_node);
    }
    for (int i = 0; i < MAX_TAREAS_I915; i++) {
        g_tabla_tareas[i].id         = i;
        g_tabla_tareas[i].state      = TASK_DEAD;
        g_tabla_tareas[i].stack_base = NULL;
        g_tabla_tareas[i].stack_size = 0;
        g_tabla_tareas[i].rsp        = NULL;
        g_tabla_tareas[i].tls_base   = NULL;
        g_tabla_tareas[i].fn         = NULL;
        g_tabla_tareas[i].data       = NULL;
        g_tabla_tareas[i].name       = NULL;
        g_tabla_tareas[i].cpu_id     = 0;
        INIT_LIST_HEAD(&g_tabla_tareas[i].node);
        INIT_LIST_HEAD(&g_tabla_tareas[i].wait_node);
    }
    // Contexto raiz del BSP/hilo host. Los AP registran su propia raiz antes
    // de planificar; la tabla dinamica no puede reservar estos contextos.
#ifndef TAEK_HOST_TEST
    g_raiz[0].tls_base=(void *)taek_tls_base_actual();
    g_apic_por_slot[0]=i915_apic_local();
#endif
    g_cpu[0].actual=&g_raiz[0];
    __atomic_store_n(&g_cpu_registrada[0],1,__ATOMIC_RELEASE);
    atomic_set(&g_tareas_vivas, 1);
    __atomic_store_n(&g_sched_iniciado,2,__ATOMIC_RELEASE);
}

int i915_scheduler_registrar_cpu(unsigned slot,unsigned apic_id){
    if(slot>=I915_MAX_CPU || __atomic_load_n(&g_sched_iniciado,__ATOMIC_ACQUIRE)!=2)return -1;
#ifndef TAEK_HOST_TEST
    if(apic_id!=i915_apic_local())return -1;
#else
    if(slot!=cpu_sel_leer())return -1;
#endif
    unsigned long flags;spin_lock_irqsave(&g_registro_lock,flags);
    if(g_cpu_registrada[slot]){
        int result=g_apic_por_slot[slot]==apic_id?0:-1;
        spin_unlock_irqrestore(&g_registro_lock,flags);return result;
    }
    for(unsigned i=0;i<I915_MAX_CPU;i++)if(g_cpu_registrada[i] && g_apic_por_slot[i]==apic_id){
        spin_unlock_irqrestore(&g_registro_lock,flags);return -1;
    }
    unsigned long cpu_flags;spin_lock_irqsave(&g_cpu[slot].lock,cpu_flags);
#ifndef TAEK_HOST_TEST
    g_raiz[slot].tls_base=(void *)taek_tls_base_actual();
#endif
    g_cpu[slot].actual=&g_raiz[slot];g_apic_por_slot[slot]=apic_id;
    __atomic_store_n(&g_cpu_registrada[slot],1,__ATOMIC_RELEASE);
    atomic_inc(&g_tareas_vivas);
    spin_unlock_irqrestore(&g_cpu[slot].lock,cpu_flags);
    spin_unlock_irqrestore(&g_registro_lock,flags);return 0;
}

static struct task_struct *tomar_libre(void) {
    for (int i = 1; i < MAX_TAREAS_I915; i++) {
        if (!g_descriptor_ocupado[i]) {
#ifndef I915_TEST_NO_GLOBAL_RESERVE
            g_descriptor_ocupado[i]=true;
            g_tabla_tareas[i].state=TASK_RESERVADA;
#endif
            return &g_tabla_tareas[i];
        }
    }
    return NULL;
}

struct task_struct *i915_tarea_actual(void) {
    i915_tareas_iniciar();unsigned cpu=cpu_sel_leer();
    return cpu<I915_MAX_CPU?g_cpu[cpu].actual:NULL;
}

struct task_struct *i915_tarea_crear(void (*fn)(void *), void *data,
                                     const char *name, unsigned cpu) {
    i915_tareas_iniciar();
    if (!fn || cpu >= I915_MAX_CPU || cpu_sel_leer()>=I915_MAX_CPU ||
        __atomic_load_n(&g_sched_iniciado,__ATOMIC_ACQUIRE)!=2)return NULL;
#ifndef TAEK_HOST_TEST
    if(!__atomic_load_n(&g_cpu_registrada[cpu],__ATOMIC_ACQUIRE) || !trabajos_es_coordinador())return NULL;
#endif

    unsigned long flags;
    spin_lock_irqsave(&g_tabla_lock, flags);
    struct task_struct *t = tomar_libre();
    spin_unlock_irqrestore(&g_tabla_lock,flags);
    if(!t)return NULL;
    void *stack = i915_kmalloc(TASK_STACK_SIZE_BYTES);
    if(!stack)goto fallo_reserva;

    t->stack_base = stack;
    t->stack_size = TASK_STACK_SIZE_BYTES;
    t->fn         = fn;
    t->data       = data;
    t->name       = name ? name : "i915_tarea";
    t->cpu_id     = cpu;
    INIT_LIST_HEAD(&t->node);
    INIT_LIST_HEAD(&t->wait_node);

    // The naked trampoline starts with RSP % 16 == 0 before its C call.
    t->rsp=i915_contexto_preparar(stack,TASK_STACK_SIZE_BYTES,i915_trampoline,(uint64_t)(uintptr_t)t);
    if(!t->rsp)goto fallo_pila;
    ((struct i915_contexto_marco *)t->rsp)->irq_flags=0x202; /* Tarea nueva: IF=1. */
#ifndef TAEK_HOST_TEST
    t->tls_base=taek_tls_crear();
    if(!t->tls_base)goto fallo_pila;
    ((struct i915_contexto_marco *)t->rsp)->fs_base=(uintptr_t)t->tls_base;
#endif

    spin_lock_irqsave(&g_cpu[cpu].lock,flags);
    t->state=TASK_RUNNING;list_add_tail(&t->node, &g_cpu[cpu].runq);
    atomic_inc(&g_tareas_vivas);
    spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
    return t;
fallo_pila:
    i915_kfree(stack);t->stack_base=NULL;t->rsp=NULL;t->fn=NULL;t->data=NULL;
fallo_reserva:
    spin_lock_irqsave(&g_tabla_lock,flags);t->state=TASK_DEAD;g_descriptor_ocupado[t->id]=false;
    spin_unlock_irqrestore(&g_tabla_lock,flags);return NULL;
}

void i915_tarea_destruir(struct task_struct *t) {
    if (!t || t->id <= 0) return;
#ifndef TAEK_HOST_TEST
    if(!trabajos_es_coordinador() || cpu_sel_leer()>=I915_MAX_CPU)return;
#endif
    unsigned cpu = t->cpu_id;
    if(cpu>=I915_MAX_CPU)return;
    unsigned long flags,table_flags;spin_lock_irqsave(&g_tabla_lock,table_flags);
    spin_lock_irqsave(&g_cpu[cpu].lock, flags);
    if (t->state==TASK_RESERVADA || g_cpu[cpu].actual == t) {
        // No se libera una pila en uso; marcar y dejar que termine.
        spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
        spin_unlock_irqrestore(&g_tabla_lock,table_flags);
        return;
    }
#ifndef TAEK_HOST_TEST
    if(taek_tls_destruir(t->tls_base)){
        spin_unlock_irqrestore(&g_cpu[cpu].lock,flags);
        spin_unlock_irqrestore(&g_tabla_lock,table_flags);return;
    }
#endif
    t->tls_base=NULL;
    if (!list_empty(&t->node)) list_del_init(&t->node);
    if (!list_empty(&t->wait_node)) list_del_init(&t->wait_node);
    if (t->stack_base) {
        atomic_dec(&g_tareas_vivas);
        i915_kfree(t->stack_base);
        t->stack_base = NULL;
    }
    t->state  = TASK_DEAD;
    t->fn     = NULL;
    t->data   = NULL;
    t->rsp    = NULL;
    g_descriptor_ocupado[t->id]=false;
    spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
    spin_unlock_irqrestore(&g_tabla_lock,table_flags);
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
    if (!t) return;
    unsigned cpu = t->cpu_id;
    if(cpu>=I915_MAX_CPU)return;
    unsigned long flags;
    spin_lock_irqsave(&g_cpu[cpu].lock, flags);
    if (t->state == TASK_INTERRUPTIBLE || t->state == TASK_UNINTERRUPTIBLE) {
        t->state = TASK_RUNNING;
        if (t != g_cpu[cpu].actual && list_empty(&t->node)) {
            list_add_tail(&t->node, &g_cpu[cpu].runq);
        }
    }
    spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
}

void i915_tarea_ceder(void) { i915_schedule(); }

void i915_schedule(void) {
    i915_tareas_iniciar();
    unsigned cpu = cpu_sel_leer();
    if(cpu>=I915_MAX_CPU)return;
    unsigned long flags;
    spin_lock_irqsave(&g_cpu[cpu].lock, flags);
    const unsigned long flags_entrada=flags;

    for (;;) {
        struct task_struct *prev = g_cpu[cpu].actual;
        /* Sin raiz no hay pila donde guardar el retorno. No consumir una
         * tarea de la cola ni modificar actual en una CPU sin contexto. */
        if(!prev){spin_unlock_irqrestore(&g_cpu[cpu].lock,flags_entrada);return;}
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
            spin_unlock(&g_cpu[cpu].lock);
            i915_cambiar_contexto(&prev->rsp, next->rsp, NULL,flags_entrada);
            return;
        }

        // next == NULL o next == prev
        if (prev && prev->state == TASK_RUNNING) {
            // Tarea ejecutable: no durmió o fue despertada en la ventana previa al sueño
            spin_unlock_irqrestore(&g_cpu[cpu].lock, flags_entrada);
            return;
        }

        // prev->state != TASK_RUNNING y runq vacía: suspensión sin tareas disponibles
        // La CPU debe esperar en reposo (idle) a que una interrupción o evento despierte una tarea
#ifdef TAEK_HOST_TEST
        spin_unlock_irqrestore(&g_cpu[cpu].lock, flags);
        usleep(50);
        spin_lock_irqsave(&g_cpu[cpu].lock, flags);
#else
        spin_unlock(&g_cpu[cpu].lock);
        __asm__ volatile ("sti; hlt; cli" ::: "memory");
        spin_lock(&g_cpu[cpu].lock);
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
    entry->flags &= ~WQ_FLAG_EXCLUSIVE;
    /* No exclusivos delante: wake_up llega a todos antes del primer exclusivo. */
    if (list_empty(&entry->entry)) list_add(&entry->entry, &q->head);
    if (entry->task) entry->task->state = state;
    spin_unlock_irqrestore(&q->lock, flags);
}

void i915_prepare_to_wait_exclusive(wait_queue_head_t *q, wait_queue_entry_t *entry, int state) {
    if (!q || !entry) return;
    unsigned long flags;
    spin_lock_irqsave(&q->lock, flags);
    entry->flags |= WQ_FLAG_EXCLUSIVE;
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
    wait_queue_entry_t *pos, *n;
    list_for_each_entry_safe(pos, n, &q->head, entry) {
        int exclusivo = pos->flags & WQ_FLAG_EXCLUSIVE;
        list_del_init(&pos->entry);
        if (pos->task) i915_tarea_despertar(pos->task);
        if (exclusivo) break;
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
                /* PENDING se limpia antes del callback: permite reencolar en vuelo. */
                atomic_set(&work->data, atomic_read(&work->data) & ~(1 << WORK_STRUCT_PENDING_BIT));
                __atomic_store_n(&wq->current_work, work, __ATOMIC_RELEASE);
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

        /* El callback pudo liberar work: a partir de aquí no se desreferencia. */
        spin_lock_irqsave(&wq->lock, flags);
        __atomic_store_n(&wq->current_work, NULL, __ATOMIC_RELEASE);
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
    wq->current_work = NULL;
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
    if (data & ((1 << WORK_STRUCT_PENDING_BIT) | (1 << WORK_STRUCT_CANCELING_BIT))) {
        spin_unlock_irqrestore(&wq->lock, flags);
        return false;   // ya encolado, o cancelación en curso
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

    /* Un solo cancelador activo por trabajo; el siguiente espera su turno. */
    for (;;) {
        spin_lock_irqsave(&wq->lock, flags);
        if (!(atomic_read(&work->data) & (1 << WORK_STRUCT_CANCELING_BIT))) break;
        spin_unlock_irqrestore(&wq->lock, flags);
        i915_tarea_ceder();
    }
    int data = atomic_read(&work->data) | (1 << WORK_STRUCT_CANCELING_BIT);
    if (data & (1 << WORK_STRUCT_PENDING_BIT)) {
        list_del_init(&work->entry);
        data &= ~(1 << WORK_STRUCT_PENDING_BIT);
        atomic_dec(&wq->pending);
        atomic_dec(&wq->active_jobs);
        cancelled_pending = true;
    }
    /* CANCELING impide que un callback en vuelo se reencole durante la espera. */
    atomic_set(&work->data, data);
    spin_unlock_irqrestore(&wq->lock, flags);

    if (cancelled_pending) {
        i915_wake_up_all(&wq->wait_idle);
    }

    /* Desde el propio callback no se puede esperar: se bloquearía a sí mismo. */
    if (i915_tarea_actual() != wq->worker_task) {
        while (__atomic_load_n(&wq->current_work, __ATOMIC_ACQUIRE) == work) {
            i915_tarea_ceder();
        }
    }

    spin_lock_irqsave(&wq->lock, flags);
    atomic_set(&work->data, atomic_read(&work->data) & ~(1 << WORK_STRUCT_CANCELING_BIT));
    spin_unlock_irqrestore(&wq->lock, flags);
    return cancelled_pending;
}

/* Barrera FIFO, como wq_barrier de Linux: flush espera lo encolado hasta ahora y
 * no lo que un callback vuelva a encolar después, así que un autorrearme no lo
 * deja colgado. Vive en la pila: el worker no la toca tras llamar al callback. */
struct i915_barrera {
    struct work_struct work;
    volatile int       hecho;
};

static void i915_barrera_fn(struct work_struct *work) {
    struct i915_barrera *b = container_of(work, struct i915_barrera, work);
    __atomic_store_n(&b->hecho, 1, __ATOMIC_RELEASE);
}

void i915_flush_workqueue(struct workqueue_struct *wq) {
    if (!wq) return;
    /* Hacer flush desde el propio worker es un interbloqueo (Linux lo avisa). */
    if (i915_tarea_actual() == wq->worker_task) return;
    struct i915_barrera barrera;
    INIT_WORK(&barrera.work, i915_barrera_fn);
    barrera.hecho = 0;
    unsigned long flags;
    spin_lock_irqsave(&wq->lock, flags);
    if (!wq->worker_task || wq->destruida) {
        spin_unlock_irqrestore(&wq->lock, flags);
        return;
    }
    atomic_set(&barrera.work.data, 1 << WORK_STRUCT_PENDING_BIT);
    barrera.work.wq = wq;
    list_add_tail(&barrera.work.entry, &wq->work_list);
    atomic_inc(&wq->pending);
    atomic_inc(&wq->active_jobs);
    i915_tarea_despertar(wq->worker_task);
    spin_unlock_irqrestore(&wq->lock, flags);
    while (!__atomic_load_n(&barrera.hecho, __ATOMIC_ACQUIRE)) {
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

    // running=false rechaza encolar: el worker drena lo pendiente y termina.
    while (wq->worker_task && wq->worker_task->state != TASK_DEAD) {
        i915_tarea_ceder();
    }
    struct task_struct *w = wq->worker_task;
    wq->destruida = true;
    wq->worker_task = NULL;
    i915_tarea_destruir(w);
    i915_kfree(wq);
}
