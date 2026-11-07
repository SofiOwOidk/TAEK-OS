/* Servicios TaekOS bajo el probe original de i915 de Linux 6.6.78.
 *
 * Adaptacion explicita, no codigo Linux: memmap/struct page sobre el PMM de TaekOS, modelo de
 * dispositivo minimo, inodo anonimo y shmem sin VFS, PCI de solo lectura, ioremap sin cache, DMA con
 * VT-d inactivo comprobado, hrtimer, tasklets, irq_work e IRQ por sondeo en raiz, y un registro visible
 * de lo que Linux publicaria en sysfs o chrdev. Lo no implementado esta en early_trampas.c, generado:
 * cada simbolo se detiene con su nombre y nunca devuelve exito. */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/shrinker.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/rcupdate.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/rcupdate_wait.h>
#include <linux/completion.h>
#include <linux/panic.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/notifier.h>
#include <asm/msr.h>
#include <asm/pci.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/ioport.h>
#include <linux/irq_work.h>
#include <linux/pagevec.h>
#include <linux/smp.h>
#include <linux/page-flags.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <asm/processor.h>
#include <asm/pgtable.h>
#include <asm/cpufeature.h>
#include "../upstream_early.h"
#include "../upstream_wq.h"

static struct {
    struct taek_early_ops ops;
    unsigned activo;
    struct taek_early_resultado r;
    u64 tsc_inicio;
} e;

struct taek_early_resultado *taek_early_resultado_interno(void) { return &e.r; }

void taek_early_trampa(const char *simbolo) {
    static char t[96];
    e.r.trampas++;
    snprintf(t, sizeof(t), "[EARLY TRAMPA] %s sin servicio en esta etapa", simbolo);
    if (e.ops.mensaje) e.ops.mensaje(t);
    if (e.ops.colapsar) e.ops.colapsar(t);
    __builtin_trap();
}
#define TRAMPA(s) taek_early_trampa(s)

/* ------------------------------------------------------------- datos del nucleo */
unsigned int tsc_khz;
struct cpuinfo_x86 boot_cpu_data __read_mostly;
pteval_t __default_kernel_pte_mask __read_mostly = ~0;
pteval_t __supported_pte_mask __read_mostly = ~0;
unsigned long phys_base;
int oops_in_progress;
int overflowuid = 65534;
bool static_key_initialized __read_mostly = true;
DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_INIT_ON_FREE_DEFAULT_ON, init_on_free);
struct bus_type pci_bus_type = {.name = "pci"};

static void cpuid_cuenta(u32 hoja, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    asm volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(hoja), "c"(sub));
}
static void cpu_identificar(void) {
    struct cpuinfo_x86 *c = &boot_cpu_data;
    u32 a, b, x, d, max;
    cpuid_cuenta(0, 0, &max, &b, &x, &d);
    c->cpuid_level = max;
    c->x86_vendor = (b == 0x756e6547) ? X86_VENDOR_INTEL : X86_VENDOR_UNKNOWN;
    cpuid_cuenta(1, 0, &a, &b, &x, &d);
    c->x86 = (a >> 8) & 15;
    c->x86_model = (a >> 4) & 15;
    if (c->x86 == 15) c->x86 += (a >> 20) & 255;
    if (c->x86 >= 6) c->x86_model += ((a >> 16) & 15) << 4;
    c->x86_stepping = a & 15;
    c->x86_capability[CPUID_1_EDX] = d;
    c->x86_capability[CPUID_1_ECX] = x;
    c->x86_clflush_size = ((b >> 8) & 255) * 8;
    c->x86_cache_alignment = c->x86_clflush_size;
    if (max >= 7) {
        u32 a7, b7, c7, d7;
        cpuid_cuenta(7, 0, &a7, &b7, &c7, &d7);
        c->x86_capability[CPUID_7_0_EBX] = b7;
        c->x86_capability[CPUID_7_ECX] = c7;
    }
    cpuid_cuenta(0x80000000u, 0, &a, &b, &x, &d);
    if (a >= 0x80000001u) {
        u32 a8, b8, c8, d8;
        cpuid_cuenta(0x80000001u, 0, &a8, &b8, &c8, &d8);
        c->x86_capability[CPUID_8000_0001_EDX] = d8;
        c->x86_capability[CPUID_8000_0001_ECX] = c8;
    }
    c->x86_phys_bits = 36;
    c->x86_virt_bits = 48;
    if (a >= 0x80000008u) {
        u32 a8, b8, c8, d8;
        cpuid_cuenta(0x80000008u, 0, &a8, &b8, &c8, &d8);
        c->x86_phys_bits = a8 & 255;
        c->x86_virt_bits = (a8 >> 8) & 255;
    }
}

/* ---------------------------------------------------------- tiempo y esperas */
/* Tiempo desde el TSC con los ciclos por ms que calibro TaekOS; las esperas de registros lo usan. */
static u64 tiempo_ns(void) {
    u64 t = rdtsc_ordered(), c = e.ops.ciclos_por_ms();
    return t / c * NSEC_PER_MSEC + (t % c) * NSEC_PER_MSEC / c;
}
ktime_t ktime_get(void) { taek_wq_bombear_jiffies(); return ns_to_ktime(tiempo_ns()); }
ktime_t ktime_get_raw(void) { return ktime_get(); }
u64 ktime_get_raw_fast_ns(void) { return ktime_to_ns(ktime_get()); }
ktime_t ktime_get_with_offset(enum tk_offsets offs) { (void)offs; return ktime_get(); }
time64_t ktime_get_real_seconds(void) { return ktime_divns(ktime_get(), NSEC_PER_SEC); }
u64 local_clock(void) { return ktime_to_ns(ktime_get()); }
unsigned int jiffies_to_msecs(const unsigned long j) { return j * 4; }
unsigned int jiffies_to_usecs(const unsigned long j) { return j * 4000; }
unsigned long __msecs_to_jiffies(const unsigned int m) {
    if ((int)m < 0) return MAX_JIFFY_OFFSET;
    return DIV_ROUND_UP(m, 4);
}
unsigned long nsecs_to_jiffies(u64 n) { return div_u64(n + 4 * NSEC_PER_MSEC - 1, 4 * NSEC_PER_MSEC); }
u64 nsecs_to_jiffies64(u64 n) { return nsecs_to_jiffies(n); }
unsigned long round_jiffies_up(unsigned long j) { return roundup(j, HZ); }
unsigned long round_jiffies_up_relative(unsigned long j) { return round_jiffies_up(j + jiffies) - jiffies; }
struct timespec64 ns_to_timespec64(s64 ns) {
    struct timespec64 t = {};
    s32 r;
    if (!ns) return t;
    t.tv_sec = div_s64_rem(ns, NSEC_PER_SEC, &r);
    if (r < 0) { t.tv_sec--; r += NSEC_PER_SEC; }
    t.tv_nsec = r;
    return t;
}
void msleep(unsigned int ms) {
    unsigned long t = msecs_to_jiffies(ms) + 1;
    while (t) { __set_current_state(TASK_UNINTERRUPTIBLE); t = schedule_timeout(t); }
}
unsigned long msleep_interruptible(unsigned int ms) { msleep(ms); return 0; }
void usleep_range_state(unsigned long min, unsigned long max, unsigned int state) {
    (void)max;
    unsigned long t = usecs_to_jiffies(min) + 1;
    __set_current_state(state);
    schedule_timeout(t);
}
/* xloops es segundos en coma fija 2^-32 (udelay usa n*0x10c7): se convierte con el TSC calibrado. */
void __const_udelay(unsigned long xloops) {
    u64 ciclos = ((u64)xloops * (e.ops.ciclos_por_ms() * 1000)) >> 32, ini = rdtsc_ordered();
    while (rdtsc_ordered() - ini < ciclos) cpu_relax();
}
void __udelay(unsigned long us) { __const_udelay(us * 0x10c7); }
void __ndelay(unsigned long ns) { __const_udelay(ns * 5); }
int __cond_resched_lock(spinlock_t *lock) { (void)lock; return 0; }

/* RCU de TINY_RCU: en UP sin preemption, poder dormir ya implica un periodo de gracia. */
static unsigned long rcu_gp;
void synchronize_rcu(void) { might_sleep(); rcu_gp += 2; }
unsigned long get_state_synchronize_rcu(void) { return rcu_gp + 2; }
struct barrera_rcu { struct rcu_head h; unsigned hecho; };
static void barrera_cb(struct rcu_head *h) { WRITE_ONCE(container_of(h, struct barrera_rcu, h)->hecho, 1); }
void rcu_barrier(void) {
    struct barrera_rcu b = {.hecho = 0};
    call_rcu(&b.h, barrera_cb);
    while (!READ_ONCE(b.hecho)) { __set_current_state(TASK_UNINTERRUPTIBLE); schedule_timeout(1); }
}

/* Los aleatorios no protegen nada en esta etapa: rdtsc mezclado, sin pretension criptografica. */
u64 get_random_u64(void) {
    u64 x = rdtsc_ordered() ^ 0x9e3779b97f4a7c15ull;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    return x;
}

unsigned long __usecs_to_jiffies(const unsigned int u) { return DIV_ROUND_UP(u, 4000); }
void wakeme_after_rcu(struct rcu_head *h) { complete(&container_of(h, struct rcu_synchronize, head)->completion); }
int timer_shutdown_sync(struct timer_list *t) { int r = timer_delete_sync(t); t->function = NULL; return r; }

/* Nadie mas corta interrupciones: las tareas diferidas se atienden en raiz, entre fibras. */
void __local_bh_enable_ip(unsigned long ip, unsigned int cnt) { (void)ip; __preempt_count_sub(cnt); }

static unsigned long taints;
void add_taint(unsigned int flag, enum lockdep_ok ok) { (void)ok; set_bit(flag, &taints); }
int test_taint(unsigned flag) { return test_bit(flag, &taints); }

/* Tasklets: mismos estados que kernel/softirq.c; se ejecutan en raiz con bombear_tareas. */
static struct tasklet_struct *tasklets, **tasklets_cola = &tasklets;
void tasklet_setup(struct tasklet_struct *t, void (*cb)(struct tasklet_struct *)) {
    t->next = NULL;
    t->state = 0;
    atomic_set(&t->count, 0);
    t->callback = cb;
    t->use_callback = true;
    t->data = 0;
}
static void tasklet_encolar(struct tasklet_struct *t) {
    t->next = NULL;
    *tasklets_cola = t;
    tasklets_cola = &t->next;
}
void __tasklet_schedule(struct tasklet_struct *t) { tasklet_encolar(t); }
void __tasklet_hi_schedule(struct tasklet_struct *t) { tasklet_encolar(t); }
static void hrtimers_bombear(void);
extern void taek_early_irq_bombear(void);
void taek_early_bombear_tareas(void) {
    taek_early_irq_bombear();
    hrtimers_bombear();
    struct tasklet_struct *lista = tasklets;
    tasklets = NULL;
    tasklets_cola = &tasklets;
    while (lista) {
        struct tasklet_struct *t = lista;
        lista = t->next;
        if (tasklet_trylock(t)) {
            if (!atomic_read(&t->count)) {
                if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state)) TRAMPA("tasklet sin SCHED");
                if (t->use_callback) t->callback(t);
                else t->func(t->data);
                tasklet_unlock(t);
                continue;
            }
            tasklet_unlock(t);
        }
        tasklet_encolar(t);   /* Deshabilitado: sigue pendiente, como en Linux. */
    }
}
void tasklet_kill(struct tasklet_struct *t) {
    /* En raiz nunca hay un callback a medias; solo se espera a que salga de la cola. */
    while (test_bit(TASKLET_STATE_SCHED, &t->state)) { __set_current_state(TASK_UNINTERRUPTIBLE); schedule_timeout(1); }
}

/* hrtimer: mismos estados que Linux sobre una tabla; caducan en raiz, nunca en contexto de IRQ.
 * El callback no corre a medias visto desde una fibra, asi que cancelar no necesita esperar. */
#define HRTIMERS_MAX 128
static struct hrtimer *hrs[HRTIMERS_MAX];
static struct hrtimer_cpu_base hr_cpu;
static struct hrtimer_clock_base hr_base = {.cpu_base = &hr_cpu};
void hrtimer_init(struct hrtimer *t, clockid_t clk, enum hrtimer_mode m) {
    (void)clk; (void)m;
    memset(t, 0, sizeof(*t));
    t->base = &hr_base;
}
static int hr_quitar(struct hrtimer *t) {
    for (unsigned i = 0; i < HRTIMERS_MAX; i++) if (hrs[i] == t) {
        hrs[i] = NULL;
        t->state = HRTIMER_STATE_INACTIVE;
        e.r.hrtimers--;
        return 1;
    }
    return 0;
}
static void hr_encolar(struct hrtimer *t) {
    for (unsigned i = 0; i < HRTIMERS_MAX; i++) if (!hrs[i]) {
        hrs[i] = t;
        t->state = HRTIMER_STATE_ENQUEUED;
        e.r.hrtimers++;
        return;
    }
    TRAMPA("hrtimer: tabla llena");
}
void hrtimer_start_range_ns(struct hrtimer *t, ktime_t tim, u64 delta, const enum hrtimer_mode m) {
    (void)delta;
    hr_quitar(t);
    ktime_t exp = (m & HRTIMER_MODE_REL) ? ktime_add(ktime_get(), tim) : tim;
    t->_softexpires = exp;
    t->node.expires = exp;
    hr_encolar(t);
}
bool hrtimer_active(const struct hrtimer *t) { return t->state != HRTIMER_STATE_INACTIVE || hr_base.running == t; }
int hrtimer_try_to_cancel(struct hrtimer *t) { return hr_quitar(t); }
int hrtimer_cancel(struct hrtimer *t) { return hr_quitar(t); }
u64 hrtimer_forward(struct hrtimer *t, ktime_t ahora, ktime_t intervalo) {
    u64 n = 0;
    while (ktime_compare(t->node.expires, ahora) <= 0) {
        t->node.expires = ktime_add(t->node.expires, intervalo);
        n++;
    }
    t->_softexpires = t->node.expires;
    return n;
}
static void hrtimers_bombear(void) {
    ktime_t ahora = ktime_get();
    for (unsigned i = 0; i < HRTIMERS_MAX; i++) {
        struct hrtimer *t = hrs[i];
        if (!t || ktime_compare(t->node.expires, ahora) > 0) continue;
        hr_quitar(t);
        hr_base.running = t;
        enum hrtimer_restart r = t->function(t);
        hr_base.running = NULL;
        if (r == HRTIMER_RESTART && !hrtimer_is_queued(t)) hr_encolar(t);
    }
}

/* IOSF solo arbitra el bus PMIC de Baytrail/Cherrytrail, ausente aqui; se conserva la exclusion. */
static DEFINE_MUTEX(iosf_mutex);
void iosf_mbi_punit_acquire(void) { mutex_lock(&iosf_mutex); }
void iosf_mbi_punit_release(void) { mutex_unlock(&iosf_mutex); }
void iosf_mbi_assert_punit_acquired(void) { WARN_ON(!mutex_is_locked(&iosf_mutex)); }
int iosf_mbi_register_pmic_bus_access_notifier(struct notifier_block *nb) { (void)nb; return 0; }
int iosf_mbi_unregister_pmic_bus_access_notifier_unlocked(struct notifier_block *nb) { (void)nb; return 0; }

/* ---------------------------------------------------- memmap y paginas fisicas */
/* Secciones de struct page creadas a demanda; viven hasta reiniciar, como en Linux. */
#ifdef CONFIG_SPARSEMEM_EXTREME
struct mem_section **mem_section;
#else
#error "Perfil SPARSEMEM_EXTREME esperado"
#endif
static unsigned long secciones_vivas;

static int seccion_asegurar(unsigned long pfn) {
    unsigned long nr = pfn_to_section_nr(pfn), raiz = SECTION_NR_TO_ROOT(nr);
    if (raiz >= NR_SECTION_ROOTS) return -EINVAL;
    if (!mem_section[raiz]) {
        mem_section[raiz] = kzalloc(sizeof(struct mem_section) * SECTIONS_PER_ROOT, GFP_KERNEL);
        if (!mem_section[raiz]) return -ENOMEM;
    }
    struct mem_section *s = __nr_to_section(nr);
    if (s->section_mem_map) return 0;
    unsigned paginas = PAGE_ALIGN(sizeof(struct page) * PAGES_PER_SECTION) >> PAGE_SHIFT;
    unsigned long fisica = e.ops.paginas_asignar(paginas, PAGE_SIZE);
    if (!fisica) return -ENOMEM;
    struct page *map = __va(fisica);
    memset(map, 0, paginas << PAGE_SHIFT);
    for (unsigned long i = 0; i < PAGES_PER_SECTION; i++) {
        set_page_section(&map[i], nr);
        INIT_LIST_HEAD(&map[i].lru);
    }
    s->section_mem_map = (unsigned long)(map - section_nr_to_pfn(nr)) | SECTION_HAS_MEM_MAP | SECTION_MARKED_PRESENT;
    secciones_vivas++;
    return 0;
}

struct page *__alloc_pages(gfp_t gfp, unsigned int order, int nid, nodemask_t *nodes) {
    (void)nid; (void)nodes;
    if (order && (gfp & __GFP_COMP)) {
        /* Las compuestas exigen metadatos de folio que aun no se usan aqui: error real. */
        pr_err("__alloc_pages: compuesta orden %u no soportada\n", order);
        return NULL;
    }
    unsigned n = 1u << order;
    unsigned long fisica = e.ops.paginas_asignar(n, PAGE_SIZE << order);
    if (!fisica) return NULL;
    if ((gfp & __GFP_DMA32) && fisica + (n << PAGE_SHIFT) > (1ull << 32)) {
        e.ops.paginas_liberar(fisica, n);
        pr_err("__alloc_pages: sin memoria bajo 4 GiB\n");
        return NULL;
    }
    unsigned long pfn = fisica >> PAGE_SHIFT;
    for (unsigned long p = pfn; p < pfn + n; p += PAGES_PER_SECTION - (p & (PAGES_PER_SECTION - 1))) {
        if (seccion_asegurar(p)) { e.ops.paginas_liberar(fisica, n); return NULL; }
    }
    if (seccion_asegurar(pfn + n - 1)) { e.ops.paginas_liberar(fisica, n); return NULL; }
    struct page *pg = pfn_to_page(pfn);
    set_page_count(pg, 1);
    if (gfp & __GFP_ZERO) memset(__va(fisica), 0, (size_t)n << PAGE_SHIFT);
    e.r.paginas += n;
    if (e.r.paginas > e.r.paginas_pico) e.r.paginas_pico = e.r.paginas;
    return pg;
}
void __free_pages(struct page *pg, unsigned int order) {
    if (!pg) return;
    if (!put_page_testzero(pg)) return;
    unsigned n = 1u << order;
    if (e.ops.paginas_liberar(page_to_pfn(pg) << PAGE_SHIFT, n)) TRAMPA("__free_pages: pagina ajena");
    e.r.paginas -= n;
}
unsigned long __get_free_pages(gfp_t gfp, unsigned int order) {
    struct page *pg = alloc_pages(gfp, order);
    return pg ? (unsigned long)page_address(pg) : 0;
}
void free_pages(unsigned long a, unsigned int order) { if (a) __free_pages(virt_to_page((void *)a), order); }
/* Linux dimensiona limites de TTM con esto; se declara lo que el modulo puede pedir. */
void si_meminfo(struct sysinfo *v) {
    memset(v, 0, sizeof(*v));
    v->totalram = v->freeram = (1ul << 30) >> PAGE_SHIFT;
    v->mem_unit = PAGE_SIZE;
}
bool is_vmalloc_addr(const void *p) { (void)p; return false; }

/* Sin reclamo de memoria en TaekOS: se conservan registrados y nadie los invoca. */
static LIST_HEAD(shrinkers);
int __printf(2, 3) register_shrinker(struct shrinker *s, const char *fmt, ...) {
    (void)fmt;
    list_add_tail(&s->list, &shrinkers);
    return 0;
}
void unregister_shrinker(struct shrinker *s) { if (s->list.next) { list_del(&s->list); s->list.next = NULL; } }
void synchronize_shrinkers(void) {}

/* ---------------------------------------------------- utilidades mm/util.c */
void *kmalloc_large(size_t n, gfp_t g) { return __kmalloc(n, g); }
void *kmalloc_large_node(size_t n, gfp_t g, int nid) { (void)nid; return __kmalloc(n, g); }
void kmem_cache_destroy(struct kmem_cache *c) { kfree(c); }
ssize_t strscpy_pad(char *d, const char *s, size_t n) {
    ssize_t w = strscpy(d, s, n);
    size_t usados = w < 0 ? n : (size_t)w + 1;
    if (usados < n) memset(d + usados, 0, n - usados);
    return w;
}
void *__kmalloc_node_track_caller(size_t n, gfp_t g, int nid, unsigned long c) { (void)nid; (void)c; return __kmalloc(n, g); }
void *kvmalloc_node(size_t n, gfp_t g, int nid) { (void)nid; return __kmalloc(n, g); }
void kvfree(const void *p) { kfree(p); }
size_t ksize(const void *p) {
    if (ZERO_OR_NULL_PTR(p)) return 0;
    return taek_wq_bloque_bytes(p);
}
size_t kmalloc_size_roundup(size_t n) { return n; }
void *krealloc(const void *p, size_t n, gfp_t g) {
    if (!n) { kfree(p); return ZERO_SIZE_PTR; }
    void *q = __kmalloc(n, g);
    if (q && !ZERO_OR_NULL_PTR(p)) { size_t v = ksize(p); memcpy(q, p, min(v, n)); kfree(p); }
    return q;
}
void *kmemdup(const void *s, size_t n, gfp_t g) { void *p = __kmalloc(n, g); if (p) memcpy(p, s, n); return p; }
char *kstrdup(const char *s, gfp_t g) { return s ? kmemdup(s, strlen(s) + 1, g) : NULL; }
const char *kstrdup_const(const char *s, gfp_t g) { return kstrdup(s, g); }
void kfree_const(const void *p) { kfree(p); }
char *kvasprintf(gfp_t g, const char *f, va_list ap) {
    va_list aq;
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, f, aq);
    va_end(aq);
    char *p = __kmalloc(n + 1, g);
    if (p) vsnprintf(p, n + 1, f, ap);
    return p;
}
char *kasprintf(gfp_t g, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    char *p = kvasprintf(g, f, ap);
    va_end(ap);
    return p;
}
int sprintf(char *b, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    int r = vsnprintf(b, INT_MAX, f, ap);
    va_end(ap);
    return r;
}
int _printk_deferred(const char *f, ...) {
    char t[160];
    va_list ap;
    va_start(ap, f);
    vsnprintf(t, sizeof(t), f, ap);
    va_end(ap);
    return _printk("%s", t);
}

/* ------------------------------------------------------- modelo de dispositivo */
static void texto_dev(const char *nivel, const struct device *d, const char *f, va_list ap) {
    char t[140];
    vsnprintf(t, sizeof(t), f, ap);
    _printk("%s %s: %s", nivel, d ? dev_name(d) : "(sin dev)", t);
}
#define DEV_IMPRIME(nombre, nivel) \
    void nombre(const struct device *d, const char *f, ...) { va_list ap; va_start(ap, f); texto_dev(nivel, d, f, ap); va_end(ap); }
DEV_IMPRIME(_dev_err, "[err]")
DEV_IMPRIME(_dev_warn, "[warn]")
DEV_IMPRIME(_dev_notice, "[note]")
DEV_IMPRIME(_dev_info, "[info]")
void _dev_printk(const char *nivel, const struct device *d, const char *f, ...) {
    (void)nivel;
    va_list ap; va_start(ap, f); texto_dev("[dev]", d, f, ap); va_end(ap);
}
const char *dev_driver_string(const struct device *d) {
    struct device_driver *drv = READ_ONCE(d->driver);
    return drv ? drv->name : "";
}

void device_initialize(struct device *d) {
    kref_init(&d->kobj.kref);
    INIT_LIST_HEAD(&d->dma_pools);
    mutex_init(&d->mutex);
    spin_lock_init(&d->devres_lock);
    INIT_LIST_HEAD(&d->devres_head);
}
int dev_set_name(struct device *d, const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    char *n = kvasprintf(GFP_KERNEL, f, ap);
    va_end(ap);
    if (!n) return -ENOMEM;
    kfree_const(d->kobj.name);
    d->kobj.name = n;
    return 0;
}
struct device *get_device(struct device *d) { if (d) kref_get(&d->kobj.kref); return d; }
static void device_soltar(struct kref *k) {
    struct device *d = container_of(k, struct device, kobj.kref);
    const char *n = d->kobj.name;
    d->kobj.name = NULL;
    kfree_const(n);
    if (d->release) d->release(d);
    else if (d->type && d->type->release) d->type->release(d);
    else if (d->class && d->class->dev_release) d->class->dev_release(d);
}
void put_device(struct device *d) {
    /* drm_minor_alloc de 6.6.78 deja kdev=ERR_PTR si falla la asignacion y su release lo pasa aqui;
     * en Linux eso seria un oops bajo falta de memoria. Hallado inyectando fallos: se avisa y sigue. */
    if (IS_ERR(d)) { pr_err("put_device con ERR_PTR(%ld)\n", PTR_ERR(d)); return; }
    if (d) kref_put(&d->kobj.kref, device_soltar);
}

struct accion_devres { struct list_head l; void (*fn)(void *); void *dato; };
int __devm_add_action(struct device *d, void (*fn)(void *), void *dato, const char *nombre) {
    (void)nombre;
    struct accion_devres *a = kmalloc(sizeof(*a), GFP_KERNEL);
    if (!a) return -ENOMEM;
    a->fn = fn; a->dato = dato;
    list_add_tail(&a->l, &d->devres_head);
    e.r.devres++;
    return 0;
}
void taek_early_devres_liberar(struct device *d) {
    while (!list_empty(&d->devres_head)) {
        struct accion_devres *a = list_last_entry(&d->devres_head, struct accion_devres, l);
        list_del(&a->l);
        e.r.devres--;
        a->fn(a->dato);
        kfree(a);
    }
}

/* Lo que Linux publicaria en sysfs/chrdev queda en un registro: sin VFS nadie puede abrirlo. */
#define REG_MAX 64
static struct { const char *tipo; const char *nombre; const void *ref; } reg[REG_MAX];
static int registrar(const char *tipo, const char *nombre, const void *ref) {
    for (unsigned i = 0; i < REG_MAX; i++) if (!reg[i].tipo) {
        reg[i].tipo = tipo; reg[i].nombre = nombre; reg[i].ref = ref;
        e.r.registros++;
        return 0;
    }
    return -ENOSPC;
}
static void desregistrar(const void *ref) {
    for (unsigned i = 0; i < REG_MAX; i++) if (reg[i].tipo && reg[i].ref == ref) {
        reg[i].tipo = NULL;
        e.r.registros--;
    }
}
struct class *class_create(const char *nombre) {
    struct class *c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) return ERR_PTR(-ENOMEM);
    c->name = nombre;
    int r = registrar("clase", nombre, c);
    if (r) { kfree(c); return ERR_PTR(r); }
    return c;
}
void class_destroy(const struct class *c) { if (!IS_ERR_OR_NULL(c)) { desregistrar(c); kfree(c); } }
int class_create_file_ns(const struct class *c, const struct class_attribute *a, const void *ns) {
    (void)c; (void)ns;
    return registrar("atributo", a->attr.name, a);
}
void class_remove_file_ns(const struct class *c, const struct class_attribute *a, const void *ns) {
    (void)c; (void)ns;
    desregistrar(a);
}
int __register_chrdev(unsigned int mayor, unsigned int base, unsigned int n, const char *nombre, const struct file_operations *fops) {
    (void)mayor; (void)base; (void)n;
    return registrar("chrdev", nombre, fops);
}
void __unregister_chrdev(unsigned int mayor, unsigned int base, unsigned int n, const char *nombre) {
    (void)mayor; (void)base; (void)n;
    for (unsigned i = 0; i < REG_MAX; i++) if (reg[i].tipo && reg[i].nombre == nombre) { reg[i].tipo = NULL; e.r.registros--; }
}

/* ----------------------------------------------------------- inodo sin VFS */
/* i915 solo monta gemfs con hugepages, apagado en este perfil: llega con NULL y no hay nada que soltar. */
void kern_unmount(struct vfsmount *m) { if (m && !IS_ERR(m)) TRAMPA("kern_unmount: montaje real inesperado"); }
int simple_pin_fs(struct file_system_type *fs, struct vfsmount **m, int *cuenta) {
    (void)fs;
    if (!*m) {
        struct vfsmount *v = kzalloc(sizeof(*v), GFP_KERNEL);
        if (!v) return -ENOMEM;
        v->mnt_sb = kzalloc(sizeof(*v->mnt_sb), GFP_KERNEL);
        if (!v->mnt_sb) { kfree(v); return -ENOMEM; }
        *m = v;
    }
    ++*cuenta;
    return 0;
}
void simple_release_fs(struct vfsmount **m, int *cuenta) {
    if (--*cuenta) return;
    kfree((*m)->mnt_sb);
    kfree(*m);
    *m = NULL;
}
struct inode *alloc_anon_inode(struct super_block *sb) {
    struct inode *i = kzalloc(sizeof(*i), GFP_KERNEL);
    if (!i) return ERR_PTR(-ENOMEM);
    i->i_sb = sb;
    i->i_mapping = &i->i_data;
    i->i_data.host = i;
    atomic_set(&i->i_count, 1);
    init_rwsem(&i->i_data.i_mmap_rwsem);
    return i;
}
void iput(struct inode *i) { if (i && atomic_dec_and_test(&i->i_count)) kfree(i); }

/* ------------------------------------------------------------ PCI, bus 0, lectura */
#define PCI_DEVS 64
static struct pci_sysdata bus0_sys;
static struct pci_bus bus0 = {.sysdata = &bus0_sys};
static struct pci_dev *pcis[PCI_DEVS];
static struct pci_dev *pci_obtener(unsigned devfn) {
    for (unsigned i = 0; i < PCI_DEVS; i++) if (pcis[i] && pcis[i]->devfn == devfn) return pcis[i];
    return NULL;
}
static int pci_gmch_simulado;
void taek_early_pci_simular_gmch(int v) { pci_gmch_simulado = v; }
static unsigned cfg(const struct pci_bus *b, unsigned devfn, unsigned off, unsigned n) {
    /* Solo identidad simulada: QEMU no tiene los campos de memoria robada ni de tabla GGTT. */
    if (pci_gmch_simulado && devfn == PCI_DEVFN(2, 0) && off == 0x50) return 0x1c1;
    return e.ops.pci_leer(b ? b->number : 0, PCI_SLOT(devfn), PCI_FUNC(devfn), off, n);
}
int pci_bus_read_config_byte(struct pci_bus *b, unsigned devfn, int w, u8 *v) { *v = cfg(b, devfn, w, 1); return 0; }
int pci_bus_read_config_word(struct pci_bus *b, unsigned devfn, int w, u16 *v) { *v = cfg(b, devfn, w, 2); return 0; }
int pci_bus_read_config_dword(struct pci_bus *b, unsigned devfn, int w, u32 *v) { *v = cfg(b, devfn, w, 4); return 0; }
int pci_read_config_byte(const struct pci_dev *d, int w, u8 *v) { *v = cfg(d->bus, d->devfn, w, 1); return 0; }
int pci_read_config_word(const struct pci_dev *d, int w, u16 *v) { *v = cfg(d->bus, d->devfn, w, 2); return 0; }
int pci_read_config_dword(const struct pci_dev *d, int w, u32 *v) { *v = cfg(d->bus, d->devfn, w, 4); return 0; }

/* Crea la identidad desde el espacio de configuracion real. No enciende ni mapea nada. */
struct pci_dev *taek_early_pci_crear(unsigned devfn) {
    struct pci_dev *ya = pci_obtener(devfn);
    if (ya) return ya;
    u32 id = cfg(&bus0, devfn, PCI_VENDOR_ID, 4);
    if (id == 0xffffffffu || !id) return NULL;
    unsigned libre = PCI_DEVS;
    for (unsigned i = 0; i < PCI_DEVS; i++) if (!pcis[i]) { libre = i; break; }
    if (libre == PCI_DEVS) return NULL;
    struct pci_dev *d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d) return NULL;
    d->bus = &bus0;
    d->devfn = devfn;
    d->vendor = id & 0xffff;
    d->device = id >> 16;
    u32 clase = cfg(&bus0, devfn, PCI_CLASS_REVISION, 4);
    d->class = clase >> 8;
    d->revision = clase & 0xff;
    d->subsystem_vendor = cfg(&bus0, devfn, PCI_SUBSYSTEM_VENDOR_ID, 2);
    d->subsystem_device = cfg(&bus0, devfn, PCI_SUBSYSTEM_ID, 2);
    d->hdr_type = cfg(&bus0, devfn, PCI_HEADER_TYPE, 1);
    d->dev.bus = &pci_bus_type;
    d->dev.parent = NULL;
    d->dma_mask = DMA_BIT_MASK(32);
    d->dev.dma_mask = &d->dma_mask;
    d->dev.coherent_dma_mask = DMA_BIT_MASK(32);
    d->dev.dma_parms = &d->dma_parms;
    device_initialize(&d->dev);
    get_device(&d->dev);   /* Referencia del bus: el pci_dev dura hasta reiniciar. */
    dev_set_name(&d->dev, "0000:00:%02x.%d", PCI_SLOT(devfn), PCI_FUNC(devfn));
    for (unsigned b = 0; b < 6; b++) {
        unsigned long base = 0, tam = 0;
        int t = e.ops.pci_barra(0, PCI_SLOT(devfn), PCI_FUNC(devfn), b, &base, &tam);
        if (t < 0 || !tam) continue;
        d->resource[b].start = base;
        d->resource[b].end = base + tam - 1;
        d->resource[b].flags = t ? IORESOURCE_IO : IORESOURCE_MEM;
    }
    pcis[libre] = d;
    return d;
}
struct pci_dev *pci_get_class(unsigned int clase, struct pci_dev *desde) {
    unsigned inicio = desde ? desde->devfn + 1 : 0;
    if (desde) put_device(&desde->dev);
    for (unsigned devfn = inicio; devfn < 256; devfn++) {
        if (cfg(&bus0, devfn, PCI_VENDOR_ID, 2) == 0xffff) continue;
        if ((cfg(&bus0, devfn, PCI_CLASS_REVISION, 4) >> 8) != clase) continue;
        struct pci_dev *d = taek_early_pci_crear(devfn);
        if (d) return (struct pci_dev *)get_device(&d->dev) ? d : NULL;
    }
    return NULL;
}
void pci_dev_put(struct pci_dev *d) { if (d) put_device(&d->dev); }
/* Misma condicion que drivers/pci/pci.h:pci_match_one_device. */
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *d) {
    if (!ids) return NULL;
    for (; ids->vendor || ids->subvendor || ids->class_mask; ids++) {
        if ((ids->vendor == PCI_ANY_ID || ids->vendor == d->vendor) &&
            (ids->device == PCI_ANY_ID || ids->device == d->device) &&
            (ids->subvendor == PCI_ANY_ID || ids->subvendor == d->subsystem_vendor) &&
            (ids->subdevice == PCI_ANY_ID || ids->subdevice == d->subsystem_device) &&
            !((ids->class ^ d->class) & ids->class_mask))
            return ids;
    }
    return NULL;
}

struct pci_dev *pci_get_domain_bus_and_slot(int dominio, unsigned int bus, unsigned int devfn) {
    if (dominio || bus) return NULL;
    struct pci_dev *d = taek_early_pci_crear(devfn);
    return d && get_device(&d->dev) ? d : NULL;
}

/* -------------------------------------------------------------------- MMIO */
/* En modo simulado no hay GPU: los registros son RAM a cero y nada responde a forcewake, asi que
 * ese camino solo puede acabar en error. Sirve para ejercitar el codigo, no el hardware. */
#define MAPEOS_MAX 16
static struct { unsigned long virt, base, bytes, sim_fisica; } mapeos[MAPEOS_MAX];
static int mmio_escritura, mmio_simulado;
void taek_early_mmio_modo(int escritura, int simulado) { mmio_escritura = escritura; mmio_simulado = simulado; }

static void __iomem *mapear_con(resource_size_t fisica, unsigned long bytes, int escritura) {
    unsigned long base = fisica & PAGE_MASK, desp = fisica - base;
    unsigned long total = PAGE_ALIGN(bytes + desp), v = 0, sim = 0;
    unsigned s = MAPEOS_MAX;
    for (unsigned i = 0; i < MAPEOS_MAX; i++) if (!mapeos[i].virt) { s = i; break; }
    if (s == MAPEOS_MAX) return NULL;
    if (mmio_simulado) {
        /* La apertura simulada no se lee nunca: basta un trozo para no consumir 256 MiB de QEMU. */
        if (total > (16ul << 20)) total = 16ul << 20;
        sim = e.ops.paginas_asignar(total >> PAGE_SHIFT, PAGE_SIZE);
        if (!sim) return NULL;
        v = (unsigned long)__va(sim);
        memset((void *)v, 0, total);
    } else {
        v = e.ops.mmio_mapear(base, total, escritura);
        if (!v) { pr_err("mmio_mapear rechazado: %lx+%lx\n", base, total); return NULL; }
    }
    mapeos[s].virt = v; mapeos[s].base = base; mapeos[s].bytes = total; mapeos[s].sim_fisica = sim;
    e.r.mapeos++;
    return (void __iomem *)(v + desp);
}
static void __iomem *mapear(resource_size_t fisica, unsigned long bytes) { return mapear_con(fisica, bytes, mmio_escritura); }
void __iomem *ioremap(resource_size_t fisica, unsigned long bytes) { return mapear(fisica, bytes); }
/* Solo la tabla de la GGTT usa _wc en este perfil. Se mapea sin cache (mas estricto que WC) y, mientras
 * ggtt_escritura sea 0, sin permiso de escritura: tocar una PTE provocaria un #PF con su direccion. */
static int ggtt_escritura;
void taek_early_ggtt_escritura(int v) { ggtt_escritura = v; }
void __iomem *ioremap_wc(resource_size_t fisica, unsigned long bytes) { return mapear_con(fisica, bytes, ggtt_escritura); }
void iounmap(volatile void __iomem *a) {
    unsigned long v = (unsigned long)a;
    for (unsigned i = 0; i < MAPEOS_MAX; i++) if (mapeos[i].virt && v >= mapeos[i].virt && v - mapeos[i].virt < mapeos[i].bytes) {
        int r = mapeos[i].sim_fisica ? e.ops.paginas_liberar(mapeos[i].sim_fisica, mapeos[i].bytes >> PAGE_SHIFT)
                                     : e.ops.mmio_desmapear(mapeos[i].virt, mapeos[i].bytes);
        if (r) TRAMPA("iounmap: el kernel rechazo el desmapeo");
        mapeos[i].virt = 0;
        e.r.mapeos--;
        return;
    }
    TRAMPA("iounmap: direccion desconocida");
}
void __iomem *pci_iomap_range(struct pci_dev *d, int bar, unsigned long desp, unsigned long max) {
    resource_size_t inicio = pci_resource_start(d, bar), lon = pci_resource_len(d, bar);
    if (!lon || desp >= lon) return NULL;
    lon -= desp;
    if (max && lon > max) lon = max;
    return mapear(inicio + desp, lon);
}
void pci_iounmap(struct pci_dev *d, void __iomem *a) { (void)d; iounmap(a); }
unsigned int ioread32(const void __iomem *a) { return readl(a); }
void iowrite32(u32 v, void __iomem *a) { writel(v, a); }

/* --------------------------------------------------------------------- DMA */
/* Con VT-d inactivo la GPU ve direcciones fisicas. Se comprueba al mapear y nunca se presume: sin
 * esa observacion el mapeo falla. Esto no demuestra coherencia ni que la GPU llegue al buffer. */
static int dma_sim_directo;
void taek_early_dma_simulada(int directo) { dma_sim_directo = directo; }
static int dma_directo(void) {
    int r = mmio_simulado ? (dma_sim_directo ? 1 : -1) : e.ops.dma_directo_gpu();
    if (r != 1) {
        e.r.dma_rechazados++;
        pr_err("[DMA] ruta %s: mapeo rechazado\n", r ? "desconocida" : "traducida");
    }
    return r == 1;
}
int dma_set_mask(struct device *d, u64 m) { *d->dma_mask = m; return 0; }
int dma_set_coherent_mask(struct device *d, u64 m) { d->coherent_dma_mask = m; return 0; }
size_t dma_max_mapping_size(struct device *d) { (void)d; return SIZE_MAX; }
unsigned int dma_map_sg_attrs(struct device *d, struct scatterlist *sgl, int nents, enum dma_data_direction dir, unsigned long attrs) {
    (void)dir; (void)attrs;
    if (!dma_directo()) return 0;
    struct scatterlist *s;
    int i;
    for_each_sg(sgl, s, nents, i) {
        phys_addr_t f = sg_phys(s);
        if (f + s->length - 1 > *d->dma_mask) { e.r.dma_rechazados++; pr_err("[DMA] fuera de mascara: %llx\n", (u64)f); return 0; }
        s->dma_address = f;
        sg_dma_len(s) = s->length;
    }
    e.r.dma_mapeados += nents;
    return nents;
}
void dma_unmap_sg_attrs(struct device *d, struct scatterlist *sgl, int nents, enum dma_data_direction dir, unsigned long attrs) {
    (void)d; (void)sgl; (void)dir; (void)attrs;
    e.r.dma_mapeados -= nents;
}
/* Sin DRM fuera de i915 que publicar: no hay controlador que expulsar. La consola de TaekOS usa el
 * framebuffer del firmware y se conserva a proposito: esta etapa no toma el display. */
int aperture_remove_conflicting_pci_devices(struct pci_dev *p, const char *nombre) { (void)p; (void)nombre; return 0; }

/* El PAT de TaekOS no tiene entrada WC: se pide sin cache, mas estricto. Solo se guarda en io_mapping. */
pgprot_t pgprot_writecombine(pgprot_t p) { return __pgprot(pgprot_val(p) | _PAGE_PCD | _PAGE_PWT); }
unsigned long cachemode2protval(enum page_cache_mode m) { return m == _PAGE_CACHE_MODE_WB ? 0 : _PAGE_PCD | _PAGE_PWT; }
/* ------------------------------------------------- IRQ por sondeo e irq_work */
/* Los modulos no reciben interrupciones: el manejador original se llama en raiz entre fibras. Lee el
 * mismo registro maestro que con una IRQ real; sin fuente activa no hace nada y devuelve IRQ_NONE. */
static irq_handler_t irq_fn;
static void *irq_dato;
static int irq_num;
int request_threaded_irq(unsigned int irq, irq_handler_t h, irq_handler_t hilo, unsigned long fl, const char *nombre, void *dato) {
    (void)fl; (void)nombre;
    if (hilo || irq_fn) return -EBUSY;
    irq_fn = h; irq_dato = dato; irq_num = irq;
    return 0;
}
const void *free_irq(unsigned int irq, void *dato) { (void)irq; irq_fn = NULL; return dato; }
void synchronize_irq(unsigned int irq) { (void)irq; }
bool synchronize_hardirq(unsigned int irq) { (void)irq; return true; }
#define IRQ_WORK_MAX 64
static struct irq_work *iwork[IRQ_WORK_MAX];
bool irq_work_queue(struct irq_work *w) {
    if (atomic_fetch_or(IRQ_WORK_CLAIMED, &w->node.a_flags) & IRQ_WORK_PENDING) return false;
    for (unsigned i = 0; i < IRQ_WORK_MAX; i++) if (!iwork[i]) { iwork[i] = w; return true; }
    TRAMPA("irq_work: tabla llena");
    return false;
}
static void irq_work_bombear(void) {
    for (unsigned i = 0; i < IRQ_WORK_MAX; i++) {
        struct irq_work *w = iwork[i];
        if (!w) continue;
        iwork[i] = NULL;
        atomic_and(~IRQ_WORK_PENDING, &w->node.a_flags);
        w->func(w);
        atomic_and(~IRQ_WORK_BUSY, &w->node.a_flags);
    }
}
void irq_work_sync(struct irq_work *w) {
    while (atomic_read(&w->node.a_flags) & IRQ_WORK_BUSY) { __set_current_state(TASK_UNINTERRUPTIBLE); schedule_timeout(1); }
}
void taek_early_irq_bombear(void) {
    if (irq_fn) {
        unsigned long f;
        local_irq_save(f);
        irq_fn(irq_num, irq_dato);
        local_irq_restore(f);
    }
    irq_work_bombear();
}
/* Un solo CPU: ejecutar aqui es ejecutar en todos. */
void on_each_cpu_cond_mask(smp_cond_func_t cond, smp_call_func_t f, void *info, bool wait, const struct cpumask *m) {
    (void)wait; (void)m;
    if (!cond || cond(0, info)) f(info);
}

/* ------------------------------------------------------------- copias y FPU */
void memcpy_fromio(void *d, const volatile void __iomem *s, size_t n) { memcpy(d, (const void __force *)s, n); }
void memcpy_toio(volatile void __iomem *d, const void *s, size_t n) { memcpy((void __force *)d, s, n); }
void memset_io(volatile void __iomem *d, int v, size_t n) { memset((void __force *)d, v, n); }
void clear_page_orig(void *p) { memset(p, 0, PAGE_SIZE); }
void copy_page(void *d, void *s) { memcpy(d, s, PAGE_SIZE); }
/* i915_memcpy_from_wc usa SSE. Las fibras conservan el estado FX; aqui se guarda el de quien estaba. */
static u8 fpu_area[512] __aligned(16);
static unsigned fpu_anidado;
void kernel_fpu_begin_mask(unsigned int m) {
    (void)m;
    if (fpu_anidado++) TRAMPA("kernel_fpu_begin anidado");
    asm volatile("fxsave64 %0" : "=m"(fpu_area));
}
void kernel_fpu_end(void) {
    asm volatile("fxrstor64 %0" : : "m"(fpu_area));
    fpu_anidado--;
}

/* ----------------------------------------------------------- shmem minimo */
/* Sin VFS ni intercambio: cada objeto es un bloque fisico contiguo creado al primer uso. Las paginas
 * tienen una referencia de la propia "fichero" y se devuelven al soltarlo. vmap se apoya en que son
 * contiguas: entonces el mapa lineal ya las cubre y no hace falta tocar tablas de paginas. */
struct shmem_bloque { unsigned long fisica; unsigned paginas; };
static int shmem_rellenar(struct address_space *m) {
    struct shmem_bloque *b = m->private_data;
    if (b->fisica) return 0;
    unsigned long f = e.ops.paginas_objeto_asignar(b->paginas);
    if (!f) f = e.ops.paginas_asignar(b->paginas, PAGE_SIZE);
    if (!f) return -ENOMEM;
    unsigned long pfn = f >> PAGE_SHIFT;
    for (unsigned long p = pfn; p < pfn + b->paginas; p += PAGES_PER_SECTION - (p & (PAGES_PER_SECTION - 1)))
        if (seccion_asegurar(p)) { e.ops.paginas_liberar(f, b->paginas); return -ENOMEM; }
    if (seccion_asegurar(pfn + b->paginas - 1)) { e.ops.paginas_liberar(f, b->paginas); return -ENOMEM; }
    for (unsigned i = 0; i < b->paginas; i++) set_page_count(pfn_to_page(pfn + i), 1);
    memset(__va(f), 0, (size_t)b->paginas << PAGE_SHIFT);
    b->fisica = f;
    e.r.paginas += b->paginas;
    if (e.r.paginas > e.r.paginas_pico) e.r.paginas_pico = e.r.paginas;
    return 0;
}
struct file *shmem_file_setup(const char *nombre, loff_t tam, unsigned long fl) {
    (void)nombre; (void)fl;
    struct file *f = kzalloc(sizeof(*f), GFP_KERNEL);
    struct inode *i = kzalloc(sizeof(*i), GFP_KERNEL);
    struct shmem_bloque *b = kzalloc(sizeof(*b), GFP_KERNEL);
    if (!f || !i || !b) { kfree(f); kfree(i); kfree(b); return ERR_PTR(-ENOMEM); }
    b->paginas = DIV_ROUND_UP(tam, PAGE_SIZE);
    i->i_mapping = &i->i_data;
    i->i_data.host = i;
    i->i_data.private_data = b;
    i->i_size = tam;
    atomic_set(&i->i_count, 1);
    f->f_inode = i;
    f->f_mapping = &i->i_data;
    atomic_long_set(&f->f_count, 1);
    return f;
}
static struct folio *shmem_folio(struct address_space *m, pgoff_t idx) {
    struct shmem_bloque *b = m->private_data;
    if (idx >= b->paginas) return ERR_PTR(-EINVAL);
    int r = shmem_rellenar(m);
    if (r) return ERR_PTR(r);
    struct page *p = pfn_to_page((b->fisica >> PAGE_SHIFT) + idx);
    get_page(p);
    return page_folio(p);
}
struct folio *shmem_read_folio_gfp(struct address_space *m, pgoff_t idx, gfp_t g) { (void)g; return shmem_folio(m, idx); }
struct page *shmem_read_mapping_page_gfp(struct address_space *m, pgoff_t idx, gfp_t g) {
    (void)g;
    struct folio *f = shmem_folio(m, idx);
    return IS_ERR(f) ? ERR_CAST(f) : &f->page;
}
/* Descartar el contenido: el objeto queda a cero y conserva su bloque hasta soltar el fichero. */
void shmem_truncate_range(struct inode *i, loff_t ini, loff_t fin) {
    struct shmem_bloque *b = i->i_data.private_data;
    (void)ini; (void)fin;
    if (b->fisica) memset(__va(b->fisica), 0, (size_t)b->paginas << PAGE_SHIFT);
}
void fput(struct file *f) {
    if (!atomic_long_dec_and_test(&f->f_count)) return;
    struct inode *i = f->f_inode;
    struct shmem_bloque *b = i->i_data.private_data;
    if (b->fisica) {
        if (e.ops.paginas_liberar(b->fisica, b->paginas)) TRAMPA("fput: bloque ajeno");
        e.r.paginas -= b->paginas;
    }
    kfree(b);
    kfree(i);
    kfree(f);
}
/* Sin LRU ni intercambio: no hay lista en la que mover paginas. */
void check_move_unevictable_folios(struct folio_batch *b) { (void)b; }
bool folio_mark_dirty(struct folio *f) { return !test_and_set_bit(PG_dirty, &f->flags); }
void folio_mark_accessed(struct folio *f) { (void)f; }
void mark_page_accessed(struct page *p) { (void)p; }
bool set_page_dirty(struct page *p) { return !test_and_set_bit(PG_dirty, &p->flags); }
void __folio_batch_release(struct folio_batch *b) {
    for (unsigned i = 0; i < b->nr; i++) folio_put(b->folios[i]);
    b->nr = 0;
}
/* Siempre queda la referencia del fichero o la de __alloc_pages, que se suelta con __free_pages. */
void __folio_put(struct folio *f) { (void)f; TRAMPA("__folio_put: pagina sin dueno"); }
void *vmap(struct page **p, unsigned int n, unsigned long fl, pgprot_t prot) {
    (void)fl; (void)prot;
    for (unsigned i = 1; i < n; i++) if (page_to_pfn(p[i]) != page_to_pfn(p[0]) + i) {
        pr_err("vmap: paginas no contiguas\n");
        return NULL;
    }
    return page_address(p[0]);
}
void vunmap(const void *a) { (void)a; }

/* Sin MTRR ni PAT programado para WC: la apertura queda sin cache, mas lenta pero correcta. */
int arch_phys_wc_add(unsigned long base, unsigned long tam) { (void)base; (void)tam; return 0; }
void arch_phys_wc_del(int h) { (void)h; }
/* Sin mapeo por ahora de la OpRegion: Linux lo trata como ausencia y sigue sin datos del panel. */
void *memremap(resource_size_t f, size_t n, unsigned long fl) {
    (void)fl;
    pr_info("[GGTT] memremap %llx+%zx sin servicio: se devuelve error\n", (u64)f, n);
    return NULL;
}
void memunmap(void *a) { (void)a; }
/* El bus master lo dejo el firmware (PCI_COMMAND=0x0007). La API de PCI es de solo lectura: si no
 * estuviera puesto no se puede fingir que se ha activado. */
void pci_set_master(struct pci_dev *d) {
    u16 c;
    pci_read_config_word(d, PCI_COMMAND, &c);
    if (!(c & PCI_COMMAND_MASTER) && !mmio_simulado) TRAMPA("pci_set_master: exigiria escribir PCI_COMMAND");
    d->is_busmaster = 1;
}
/* TaekOS no tiene arbol de recursos: la unica reserva es la memoria robada, y no hay con quien chocar. */
struct resource iomem_resource = {.name = "PCI mem", .start = 0, .end = -1, .flags = IORESOURCE_MEM};
atomic_long_t _totalram_pages = ATOMIC_LONG_INIT((1ul << 30) >> PAGE_SHIFT);
static void region_soltar(void *r) { kfree(r); }
struct resource *__devm_request_region(struct device *d, struct resource *padre, resource_size_t ini, resource_size_t n, const char *nombre) {
    struct resource *r = kzalloc(sizeof(*r), GFP_KERNEL);
    if (!r) return NULL;
    r->name = nombre; r->start = ini; r->end = ini + n - 1; r->flags = padre->flags | IORESOURCE_BUSY; r->parent = padre;
    if (__devm_add_action(d, region_soltar, r, "region")) { kfree(r); return NULL; }
    return r;
}

/* Igual que arch/x86/kernel/early-quirks.c en Gen9: BDSM y campo GMS de GMCH, en unidades de 32 MiB. */
struct resource intel_graphics_stolen_res = DEFINE_RES_MEM(0, 0);
void taek_early_robada_detectar(struct pci_dev *d) {
    u32 gmch, bdsm;
    pci_read_config_dword(d, 0x50, &gmch);
    pci_read_config_dword(d, 0x5c, &bdsm);
    u32 gms = (gmch >> 8) & 0xff;
    intel_graphics_stolen_res.start = bdsm & ~0xfffffu;
    intel_graphics_stolen_res.end = gms ? intel_graphics_stolen_res.start + gms * (32ull << 20) - 1 : 0;
}

unsigned long taek_early_fb_fisica(void) { return e.ops.fb_fisica(); }

/* ------------------------------------------------------------------ arranque */
int taek_early_frontera_iniciar(const struct taek_early_ops *ops) {
    if (e.activo) return -EBUSY;
    if (!ops || ops->bytes < sizeof(*ops) || ops->version != TAEK_EARLY_VERSION || !ops->paginas_asignar ||
        !ops->paginas_liberar || !ops->pci_leer || !ops->mensaje || !ops->colapsar || !ops->pci_barra ||
        !ops->mmio_mapear || !ops->mmio_desmapear || !ops->ciclos_por_ms || !ops->ciclos_por_ms() ||
        !ops->dma_directo_gpu || !ops->fb_fisica || !ops->paginas_objeto_asignar) return -EINVAL;
    e.ops = *ops;
    e.activo = 1;
    tsc_khz = ops->ciclos_por_ms();
    cpu_identificar();
    mem_section = kzalloc(sizeof(*mem_section) * NR_SECTION_ROOTS, GFP_KERNEL);
    if (!mem_section) return -ENOMEM;
    return 0;
}
