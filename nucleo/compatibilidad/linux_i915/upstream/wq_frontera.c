/* Servicios TaekOS que necesita la workqueue original de Linux 6.6.78.
 *
 * Adaptacion explicita, no codigo Linux: timer_list, RCU diferido, slab/kmalloc sobre
 * el heap nativo, percpu UP, mascaras CPU0 y formato de texto. Perfil congelado:
 * CONFIG_SMP=n, PREEMPT_NONE, HZ=250, cooperativo y solo BSP ordinario.
 * Lo inalcanzable en este perfil (sysfs, device model) detiene el sistema: nunca
 * devuelve exito. Una operacion de formato no soportada se ve en la salida. */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/rcupdate.h>
#include <linux/rcuwait.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/cpumask.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/cpuhotplug.h>
#include <linux/radix-tree.h>
#include <linux/delay.h>
#include <linux/wait.h>
#include "../upstream_wq.h"

extern int try_to_wake_up(struct task_struct *, unsigned int, int);

_Static_assert(!IS_ENABLED(CONFIG_SMP), "Solo perfil UP");
_Static_assert(!IS_ENABLED(CONFIG_PREEMPT_COUNT), "RCU cooperativo exige PREEMPT_NONE");
_Static_assert(HZ == 250, "Conversion ms a jiffies de este perfil");

static struct {
    struct taek_wq_ops ops;
    struct hlist_head timers;
    struct rcu_head *rcu_primero, **rcu_cola;
    struct taek_wq_stats st;
    unsigned activo, bombeando;
} w;

static void wq_fatal(int n) { w.ops.fatal(n); __builtin_trap(); }

/* ------------------------------------------------------------------ jiffies */
unsigned long volatile __cacheline_aligned_in_smp jiffies;
unsigned long loops_per_jiffy = (1 << 12);

static void jiffies_actualizar(void) {
    if (w.activo) WRITE_ONCE(jiffies, w.ops.clock_ms() / 4);
}
void taek_wq_bombear_jiffies(void) { jiffies_actualizar(); }

/* ---------------------------------------------------------------- memoria */
#define BLOQUE_MAGIA 0x54414b57u
struct bloque {
    void *raw;
    unsigned long bytes;
    unsigned magia, percpu;
    unsigned long relleno;
};
_Static_assert(sizeof(struct bloque) == 32, "El puntero de usuario sigue alineado a 16");

/* Control negativo: la asignacion numero n desde ahora falla, como en una falta real de memoria. */
static unsigned long fallo_en, fallos_inyectados;
void taek_wq_fallo_en(unsigned long n) { fallo_en = n; if (n) fallos_inyectados = 0; }
unsigned long taek_wq_fallos_inyectados(void) { return fallos_inyectados; }
void taek_wq_fallo_limpiar(void) { fallos_inyectados = 0; }

static void *reservar(size_t size, size_t align, gfp_t gfp, unsigned percpu) {
    if (!w.activo) wq_fatal(301);
    if (fallo_en && !--fallo_en) { fallos_inyectados++; return NULL; }
    if (!size || size > 16ul * 1024 * 1024 || !is_power_of_2(align)) {
        w.st.heap_fallos++;
        pr_err("reservar rechazada: size=%lu align=%lu\n", (unsigned long)size, (unsigned long)align);
        return NULL;
    }
    if (align < 16) align = 16;
    unsigned long total = size + align + sizeof(struct bloque);
    void *raw = w.ops.asignar(total);
    if (!raw) {
        w.st.heap_fallos++;
        pr_err("heap sin memoria: total=%lu vivos=%lu bytes=%lu\n", total, w.st.heap_bloques, w.st.heap_bytes);
        return NULL;
    }
    unsigned long p = ALIGN((unsigned long)raw + sizeof(struct bloque), align);
    /* Con delta, todo el acceso posterior usa el alias lineal de la misma memoria. */
    p += w.ops.delta_lineal;
    struct bloque *b = (struct bloque *)p - 1;
    b->raw = raw; b->bytes = total; b->magia = BLOQUE_MAGIA; b->percpu = percpu; b->relleno = size;
    w.st.heap_bloques++; w.st.heap_bytes += total;
    if (w.st.heap_bytes > w.st.heap_pico_bytes) w.st.heap_pico_bytes = w.st.heap_bytes;
    if (gfp & __GFP_ZERO) memset((void *)p, 0, size);
    return (void *)p;
}
unsigned long taek_wq_bloque_bytes(const void *p) {
    const struct bloque *b = (const struct bloque *)p - 1;
    if (b->magia != BLOQUE_MAGIA) wq_fatal(306);
    return b->relleno;
}

static void soltar(const void *p, unsigned percpu) {
    struct bloque *b = (struct bloque *)p - 1;
    if (b->magia != BLOQUE_MAGIA || b->percpu != percpu) wq_fatal(302);
    unsigned long total = b->bytes;
    void *raw = b->raw;
    b->magia = 0;
    if (w.ops.liberar(raw)) wq_fatal(303);   /* Propietario inconsistente: no seguir. */
    w.st.heap_bloques--; w.st.heap_bytes -= total;
}

/* kmalloc garantiza alineacion natural para tamanos potencia de dos. */
static size_t alineacion_kmalloc(size_t size) {
    size_t a = size & -size;
    if (a < 16) a = 16;
    if (a > 64 * 1024) a = 64 * 1024;
    return a;
}

struct kmem_cache {
    const char *name;
    unsigned int size, align;
    void (*ctor)(void *);
};
struct kmem_cache *kmalloc_caches[NR_KMALLOC_TYPES][KMALLOC_SHIFT_HIGH + 1];

void *__kmalloc(size_t size, gfp_t gfp) {
    if (!size) return ZERO_SIZE_PTR;
    return reservar(size, alineacion_kmalloc(size), gfp, 0);
}
void *__kmalloc_node(size_t size, gfp_t gfp, int node) { (void)node; return __kmalloc(size, gfp); }
void *kmalloc_trace(struct kmem_cache *s, gfp_t gfp, size_t size) { (void)s; return __kmalloc(size, gfp); }
void *kmalloc_node_trace(struct kmem_cache *s, gfp_t gfp, int node, size_t size) { (void)s; (void)node; return __kmalloc(size, gfp); }
void kfree(const void *p) {
    if (ZERO_OR_NULL_PTR(p)) return;
    soltar(p, 0);
}

struct kmem_cache *kmem_cache_create(const char *name, unsigned int size, unsigned int align,
                                     slab_flags_t flags, void (*ctor)(void *)) {
    (void)flags;
    struct kmem_cache *c = __kmalloc(sizeof(*c), GFP_KERNEL | __GFP_ZERO);
    if (!c) return NULL;
    c->name = name; c->size = size; c->align = align; c->ctor = ctor;
    w.st.caches++;
    return c;
}
void *kmem_cache_alloc(struct kmem_cache *c, gfp_t gfp) {
    void *p = reservar(c->size, max_t(size_t, c->align, 16), gfp, 0);
    /* Sin reciclado de objetos: el constructor corre sobre cada objeto nuevo. */
    if (p && c->ctor) c->ctor(p);
    return p;
}
void *kmem_cache_alloc_node(struct kmem_cache *c, gfp_t gfp, int node) { (void)node; return kmem_cache_alloc(c, gfp); }
void *kmem_cache_alloc_lru(struct kmem_cache *c, struct list_lru *lru, gfp_t gfp) { (void)lru; return kmem_cache_alloc(c, gfp); }
void kmem_cache_free(struct kmem_cache *c, void *p) { (void)c; if (p) soltar(p, 0); }

void __percpu *__alloc_percpu(size_t size, size_t align) {
    void *p = reservar(size, align, GFP_KERNEL | __GFP_ZERO, 1);
    if (p) w.st.percpu_bloques++;
    return (void __force __percpu *)p;
}
void free_percpu(void __percpu *p) {
    if (!p) return;
    soltar((void __force *)p, 1);
    w.st.percpu_bloques--;
}

/* ---------------------------------------------------------------- mascaras */
#define MASK_DECLARE_1(x) [x + 1][0] = (1UL << (x))
#define MASK_DECLARE_2(x) MASK_DECLARE_1(x), MASK_DECLARE_1(x + 1)
#define MASK_DECLARE_4(x) MASK_DECLARE_2(x), MASK_DECLARE_2(x + 2)
#define MASK_DECLARE_8(x) MASK_DECLARE_4(x), MASK_DECLARE_4(x + 4)
const unsigned long cpu_bit_bitmap[BITS_PER_LONG + 1][BITS_TO_LONGS(NR_CPUS)] = {
    MASK_DECLARE_8(0), MASK_DECLARE_8(8), MASK_DECLARE_8(16), MASK_DECLARE_8(24),
    MASK_DECLARE_8(32), MASK_DECLARE_8(40), MASK_DECLARE_8(48), MASK_DECLARE_8(56),
};
struct cpumask __cpu_possible_mask = {.bits = {1}};
struct cpumask __cpu_online_mask = {.bits = {1}};
struct cpumask __cpu_active_mask = {.bits = {1}};

/* Un solo CPU que nunca sale de linea: registrar sin ejecutar es el contrato de _nocalls. */
int __cpuhp_setup_state(enum cpuhp_state state, const char *name, bool invoke,
                        int (*startup)(unsigned int), int (*teardown)(unsigned int), bool multi) {
    (void)state; (void)name; (void)teardown; (void)multi;
    if (invoke && startup) return startup(0);
    return 0;
}

/* ------------------------------------------------------------------ timers */
void init_timer_key(struct timer_list *t, void (*fn)(struct timer_list *), unsigned int flags,
                    const char *name, struct lock_class_key *key) {
    (void)name; (void)key;
    t->entry.pprev = NULL;
    t->function = fn;
    t->flags = flags;
}
static int timer_quitar(struct timer_list *t) {
    if (hlist_unhashed(&t->entry)) return 0;
    hlist_del(&t->entry);
    t->entry.pprev = NULL;
    w.st.timers_pendientes--;
    return 1;
}
int mod_timer(struct timer_list *t, unsigned long expires) {
    /* Como __mod_timer de Linux: un timer ya apagado (timer_shutdown) no se vuelve a armar. */
    if (WARN_ON_ONCE(!t->function)) return 0;
    int estaba = timer_quitar(t);
    t->expires = expires;
    hlist_add_head(&t->entry, &w.timers);
    w.st.timers_pendientes++;
    jiffies_actualizar();
    return estaba;
}
void add_timer(struct timer_list *t) {
    if (!hlist_unhashed(&t->entry)) wq_fatal(304);
    mod_timer(t, t->expires);
}
void add_timer_on(struct timer_list *t, int cpu) {
    if (cpu) wq_fatal(305);
    add_timer(t);
}
int timer_delete(struct timer_list *t) { return timer_quitar(t); }
/* Los callbacks corren en raiz y no pueden dormir: visto desde una fibra, ninguno
 * esta a medias. Por eso borrar basta para el contrato de la variante _sync. */
int timer_delete_sync(struct timer_list *t) { return timer_quitar(t); }

/* --------------------------------------------------------------------- RCU */
void call_rcu(struct rcu_head *h, rcu_callback_t fn) {
    h->func = fn;
    h->next = NULL;
    *w.rcu_cola = h;
    w.rcu_cola = &h->next;
    w.st.rcu_pendientes++;
}
int rcuwait_wake_up(struct rcuwait *wa) {
    struct task_struct *t = rcu_dereference(wa->task);
    return t ? wake_up_process(t) : 0;
}
void finish_rcuwait(struct rcuwait *wa) {
    rcu_assign_pointer(wa->task, NULL);
    __set_current_state(TASK_RUNNING);
}

int taek_wq_bombear(void) {
    if (!w.activo || w.bombeando) return -1;
    w.bombeando = 1;
    jiffies_actualizar();
    int hechos = 0, otra = 1;
    while (otra) {
        otra = 0;
        struct timer_list *t;
        struct hlist_node *n;
        hlist_for_each_entry_safe(t, n, &w.timers, entry) {
            if (time_before(jiffies, t->expires)) continue;
            timer_quitar(t);
            w.st.timers_disparados++;
            t->function(t);
            hechos++;
            otra = 1;   /* El callback pudo alterar la lista: volver a empezar. */
            break;
        }
    }
    struct rcu_head *lista = w.rcu_primero;
    w.rcu_primero = NULL;
    w.rcu_cola = &w.rcu_primero;
    while (lista) {
        struct rcu_head *h = lista;
        lista = h->next;
        w.st.rcu_pendientes--;
        w.st.rcu_ejecutados++;
        /* Regla de __is_kvfree_rcu_offset: kfree_rcu guarda el desplazamiento, no una funcion. */
        unsigned long f = (unsigned long)h->func;
        if (f < 4096) kfree((char *)h - f);
        else h->func(h);
        hechos++;
    }
    w.bombeando = 0;
    return hechos;
}

/* ------------------------------------------------------------- texto basico */
#ifndef TAEK_EARLY   /* La etapa early enlaza lib/string.c y lib/hexdump.c originales. */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        int x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x - y;
        if (!x) break;
    }
    return 0;
}
ssize_t strscpy(char *d, const char *s, size_t n) {
    if (!n) return -E2BIG;
    size_t i = 0;
    for (; i < n; i++) {
        d[i] = s[i];
        if (!s[i]) return (ssize_t)i;
    }
    d[n - 1] = 0;
    return -E2BIG;
}
char *strnchrnul(const char *s, size_t n, int c) {
    while (n-- && *s && *s != (char)c) s++;
    return (char *)s;
}
int hex_to_bin(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
#endif
/* Mismo cuerpo que sched/core.c; try_to_wake_up es la frontera de task_runtime. */
int default_wake_function(wait_queue_entry_t *curr, unsigned mode, int wake_flags, void *key) {
    (void)key;
    return try_to_wake_up(curr->private, mode, wake_flags);
}
void __set_task_comm(struct task_struct *t, const char *from, bool exec) {
    (void)exec;
    strscpy(t->comm, from, sizeof(t->comm));
}
/* Sin planificador de prioridades: se registra el valor, la cola sigue siendo FIFO. */
void set_user_nice(struct task_struct *t, long nice) { t->static_prio = NICE_TO_PRIO(nice); }

/* Subconjunto de vsnprintf: d i u x X c s p % con flags 0 y -, anchura, * y l ll z h.
 * Un especificador desconocido se escribe tal cual, para que se vea en la salida. */
static void poner(char *b, size_t n, size_t *pos, char c) {
    if (*pos + 1 < n) b[*pos] = c;
    (*pos)++;
}
static void poner_numero(char *b, size_t n, size_t *pos, unsigned long long v, unsigned base,
                         int mayus, int neg, int ancho, int cero, int izq) {
    char t[24];
    int d = 0;
    const char *dig = mayus ? "0123456789ABCDEF" : "0123456789abcdef";
    do { t[d++] = dig[v % base]; v /= base; } while (v);
    int largo = d + !!neg;
    if (!izq && !cero) for (int i = largo; i < ancho; i++) poner(b, n, pos, ' ');
    if (neg) poner(b, n, pos, '-');
    if (!izq && cero) for (int i = largo; i < ancho; i++) poner(b, n, pos, '0');
    while (d) poner(b, n, pos, t[--d]);
    if (izq) for (int i = largo; i < ancho; i++) poner(b, n, pos, ' ');
}
int vsnprintf(char *b, size_t n, const char *f, va_list ap) {
    size_t pos = 0;
    for (; *f; f++) {
        if (*f != '%') { poner(b, n, &pos, *f); continue; }
        const char *inicio = f++;
        int cero = 0, izq = 0, ancho = 0, largo = 0, prec = -1;
        for (;; f++) { if (*f == '0') cero = 1; else if (*f == '-') izq = 1; else if (*f == '#' || *f == '+' || *f == ' ') continue; else break; }
        if (*f == '*') { ancho = va_arg(ap, int); f++; }
        else while (*f >= '0' && *f <= '9') ancho = ancho * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l' || *f == 'z' || *f == 'h') { if (*f != 'h') largo++; f++; }
        switch (*f) {
        case 'd': case 'i': {
            long long v = largo ? (largo > 1 ? va_arg(ap, long long) : va_arg(ap, long)) : va_arg(ap, int);
            poner_numero(b, n, &pos, v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v, 10, 0, v < 0, ancho, cero, izq);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long long v = largo ? (largo > 1 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned long)) : va_arg(ap, unsigned);
            poner_numero(b, n, &pos, v, *f == 'u' ? 10 : 16, *f == 'X', 0, ancho, cero, izq);
            break;
        }
        case 'p':
            if (f[1] == 'V') {   /* struct va_format: lo usan los mensajes de DRM. */
                struct va_format *vf = va_arg(ap, struct va_format *);
                va_list copia;
                f++;
                va_copy(copia, *vf->va);
                pos += vsnprintf(pos < n ? b + pos : NULL, pos < n ? n - pos : 0, vf->fmt, copia);
                va_end(copia);
                break;
            }
            poner_numero(b, n, &pos, (unsigned long)va_arg(ap, void *), 16, 0, 0, 0, 0, 0);
            while (f[1] >= 'a' && f[1] <= 'z') f++;   /* Extensiones %p: valor crudo. */
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            size_t l = strlen(s);
            if (prec >= 0 && l > (size_t)prec) l = prec;
            if (!izq) for (size_t i = l; (long)i < ancho; i++) poner(b, n, &pos, ' ');
            for (size_t i = 0; i < l; i++) poner(b, n, &pos, s[i]);
            if (izq) for (size_t i = l; (long)i < ancho; i++) poner(b, n, &pos, ' ');
            break;
        }
        case 'c': poner(b, n, &pos, (char)va_arg(ap, int)); break;
        case '%': poner(b, n, &pos, '%'); break;
        case 0: f--; for (const char *q = inicio; *q; q++) poner(b, n, &pos, *q); break;
        default: for (const char *q = inicio; q <= f; q++) poner(b, n, &pos, *q); break;
        }
    }
    if (n) b[pos < n ? pos : n - 1] = 0;
    return (int)pos;
}
int snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
int vscnprintf(char *b, size_t n, const char *f, va_list ap) {
    int r = vsnprintf(b, n, f, ap);
    if (!n) return 0;
    return (size_t)r >= n ? (int)n - 1 : r;
}
int scnprintf(char *b, size_t n, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = vscnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
int _printk(const char *f, ...) {
    char t[160];
    va_list ap; va_start(ap, f);
    if (f[0] == '\001' && f[1]) f += 2;
    int r = vsnprintf(t, sizeof(t), f, ap);
    va_end(ap);
    if (w.ops.mensaje) w.ops.mensaje(t);
    return r;
}
/* printk es sincrono aqui: no hay buffer diferido que proteger. */
void __printk_safe_enter(void) {}
void __printk_safe_exit(void) {}
void dump_stack(void) { if (w.ops.mensaje) w.ops.mensaje("[WQ] dump_stack sin desenrollado disponible"); }

/* -------------------------------------------------- inalcanzable en este perfil
 * Solo WQ_SYSFS o el initcall wq_sysfs_init llegan a estos simbolos, y ninguno de los
 * dos se usa aqui. Si algo llega, es un error de perfil: se detiene, no se simula. */
#define INALCANZABLE(n) wq_fatal(400 + (n)); __builtin_unreachable()
int device_register(struct device *d) { (void)d; INALCANZABLE(1); }
void device_unregister(struct device *d) { (void)d; INALCANZABLE(2); }
#ifndef TAEK_EARLY   /* early_frontera.c da un modelo de dispositivo real. */
void put_device(struct device *d) { (void)d; INALCANZABLE(3); }
int dev_set_name(struct device *d, const char *f, ...) { (void)d; (void)f; INALCANZABLE(4); }
#endif
int device_create_file(struct device *d, const struct device_attribute *a) { (void)d; (void)a; INALCANZABLE(5); }
struct device *bus_get_dev_root(const struct bus_type *b) { (void)b; INALCANZABLE(6); }
int subsys_virtual_register(struct bus_type *b, const struct attribute_group **g) { (void)b; (void)g; INALCANZABLE(7); }
int kobject_uevent(struct kobject *k, enum kobject_action a) { (void)k; (void)a; INALCANZABLE(8); }
int sscanf(const char *s, const char *f, ...) { (void)s; (void)f; INALCANZABLE(9); }

/* ------------------------------------------------------------------ arranque */
int taek_wq_frontera_iniciar(const struct taek_wq_ops *ops) {
    if (w.activo) return -EBUSY;
    if (!ops || ops->bytes < sizeof(*ops) || ops->version != TAEK_WQ_VERSION || !ops->asignar ||
        !ops->liberar || !ops->clock_ms || !ops->mensaje || !ops->fatal) return -EINVAL;
    w.ops = *ops;
    INIT_HLIST_HEAD(&w.timers);
    w.rcu_primero = NULL;
    w.rcu_cola = &w.rcu_primero;
    memset(&w.st, 0, sizeof(w.st));
    w.activo = 1;
    jiffies_actualizar();
    radix_tree_init();
    return 0;
}
void taek_wq_estadisticas(struct taek_wq_stats *o) { if (o) *o = w.st; }
