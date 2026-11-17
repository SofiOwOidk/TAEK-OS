/* Runtime de Iris (Mesa) en TAEK, M27. Se enlaza junto a media/rt_libc.c compilado con TAEK_RT_HILOS_PROPIOS: de ahi salen
 * memoria, cadenas, formato, archivos DRM y mmap. Aqui va lo que Iris pide ademas (inventario M26):
 *   - hilos verdes: Mesa exige al menos un hilo para su cola de compilacion de shaders (util_queue_init falla sin el).
 *     Cada hilo tiene su pila y solo corre cuando otro espera (futex, cond, join, usleep): un CPU, sin desalojo;
 *   - libm con la FPU x87 para las trascendentes (precision de long double, sin reduccion de argumento exacta para |x| enormes);
 *   - libdrm minima sobre los ioctl reales (id y revision del chip por I915_GETPARAM, no fijos);
 *   - la libc que faltaba.
 * Lo que no tiene servicio no se define: el enlace lo convierte en trampa con su nombre. Prototipos del ABI de glibc. */
#include "../media/taek_media.h"
#include <stdarg.h>
#include <stdint.h>

#define EPERM 1
#define ENOENT 2
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EINVAL 22
#define ENOSYS 38
#define ETIMEDOUT 110
extern int *__errno_location(void);
#define ERRNO (*__errno_location())
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern int memcmp(const void *, const void *, size_t);
extern size_t strlen(const char *);
extern char *strchr(const char *, int);
extern char *strtok_r(char *, const char *, char **);
extern int vsnprintf(char *, size_t, const char *, va_list);
extern int vasprintf(char **, const char *, va_list);
extern int ioctl(int, unsigned long, ...);
extern int open(const char *, int, ...);
extern void *mmap(void *, size_t, int, int, int, long);

/* ======================================================================= hilos verdes */
#define HILOS_MAX 16
#define PILA_HILO (1u << 20)
struct hilo { uint64_t rsp; void *(*f)(void *); void *arg, *ret, *pila; int estado; };   /* estado: 0 libre, 1 vivo, 2 terminado */
static struct hilo hilos[HILOS_MAX] = {[0] = {.estado = 1}};
static int actual;

/* Guarda los registros que el ABI pide conservar (y MXCSR / control x87) en la pila actual, cambia de pila y los repone. */
void taek_iris_cambiar(uint64_t *guardar, uint64_t nuevo);
__asm__(".pushsection .text.taek_iris_cambiar,\"ax\"\n"
        ".global taek_iris_cambiar\n"
        "taek_iris_cambiar:\n"
        " push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " sub $8, %rsp\n stmxcsr (%rsp)\n fnstcw 4(%rsp)\n"
        " mov %rsp, (%rdi)\n mov %rsi, %rsp\n"
        " ldmxcsr (%rsp)\n fldcw 4(%rsp)\n add $8, %rsp\n"
        " pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n ret\n"
        ".popsection\n");

static int hay_otro(void) {
    for (int i = 0; i < HILOS_MAX; i++) if (i != actual && hilos[i].estado == 1) return 1;
    return 0;
}
/* Pasa al siguiente hilo vivo (en orden circular). Vuelve cuando alguien cede de nuevo hacia este. */
static void ceder(void) {
    for (int k = 1; k < HILOS_MAX; k++) {
        int i = (actual + k) % HILOS_MAX;
        if (hilos[i].estado != 1) continue;
        int yo = actual;
        actual = i;
        taek_iris_cambiar(&hilos[yo].rsp, hilos[i].rsp);
        return;
    }
}
/* Una espera: si hay otro hilo, que avance; si no, que avance el kernel (GPU, interrupciones por sondeo). */
static void esperar_un_poco(void) {
    static unsigned cesiones;
    /* Si todos esperan algo de fuera (GPU, reloj), cederse el turno entre ellos no avanza nada: cada tanto, dormir. */
    if (hay_otro() && ++cesiones % 8) ceder();
    else taek_media_dormir_us(50);
}
static void arranque(void) {
    struct hilo *h = &hilos[actual];
    h->ret = h->f(h->arg);
    h->estado = 2;
    for (;;) ceder();   /* Terminado: nadie vuelve a elegirlo. */
}

typedef unsigned long pthread_t;
int pthread_create(pthread_t *t, const void *atr, void *(*f)(void *), void *arg) {
    (void)atr;
    int i = 1;
    while (i < HILOS_MAX && hilos[i].estado) i++;
    if (i == HILOS_MAX) return EAGAIN;
    char *pila = malloc(PILA_HILO);
    if (!pila) return EAGAIN;
    uint64_t tope = ((uint64_t)(uintptr_t)pila + PILA_HILO) & ~(uint64_t)15;
    uint64_t *p = (uint64_t *)(uintptr_t)tope;
    *--p = 0;                                   /* direccion de vuelta ficticia de arranque */
    *--p = (uint64_t)(uintptr_t)arranque;       /* la toma el ret de taek_iris_cambiar */
    for (int r = 0; r < 6; r++) *--p = 0;       /* rbp rbx r12 r13 r14 r15 */
    *--p = (uint64_t)0x1F80 | ((uint64_t)0x037F << 32);   /* MXCSR y control x87 por defecto */
    hilos[i] = (struct hilo){.rsp = (uint64_t)(uintptr_t)p, .f = f, .arg = arg, .pila = pila, .estado = 1};
    *t = (pthread_t)i + 1;
    return 0;
}
pthread_t pthread_self(void) { return (pthread_t)actual + 1; }
int pthread_join(pthread_t t, void **ret) {
    int i = (int)t - 1;
    if (i <= 0 || i >= HILOS_MAX || !hilos[i].estado) return EINVAL;
    while (hilos[i].estado != 2) esperar_un_poco();
    if (ret) *ret = hilos[i].ret;
    free(hilos[i].pila);
    hilos[i] = (struct hilo){0};
    return 0;
}
int pthread_detach(pthread_t t) { (void)t; return 0; }
int sched_yield(void) { ceder(); return 0; }
int usleep(unsigned us) {
    uint64_t fin = taek_media_ns() + (uint64_t)us * 1000u;
    do esperar_un_poco(); while (taek_media_ns() < fin);
    return 0;
}

struct mutex_v { int dueno; unsigned veces, tipo; };
_Static_assert(sizeof(struct mutex_v) <= 40, "pthread_mutex_t");
int pthread_mutexattr_init(unsigned *a) { *a = 0; return 0; }
int pthread_mutexattr_destroy(unsigned *a) { (void)a; return 0; }
int pthread_mutexattr_settype(unsigned *a, int t) { *a = (unsigned)t; return 0; }
int pthread_mutex_init(struct mutex_v *m, const unsigned *a) { m->dueno = 0; m->veces = 0; m->tipo = a ? *a : 0; return 0; }
int pthread_mutex_destroy(struct mutex_v *m) { if (m->veces) taek_media_trampa("iris: pthread_mutex_destroy con el mutex tomado"); return 0; }
int pthread_mutex_lock(struct mutex_v *m) {
    int yo = actual + 1;
    if (m->veces && m->dueno == yo) {
        if (m->tipo != 1) taek_media_trampa("iris: reentrada en mutex no recursivo");
        m->veces++;
        return 0;
    }
    while (m->veces) esperar_un_poco();
    m->dueno = yo; m->veces = 1;
    return 0;
}
int pthread_mutex_trylock(struct mutex_v *m) {
    int yo = actual + 1;
    if (m->veces && m->dueno == yo && m->tipo == 1) { m->veces++; return 0; }
    if (m->veces) return 16;   /* EBUSY */
    m->dueno = yo; m->veces = 1;
    return 0;
}
int pthread_mutex_unlock(struct mutex_v *m) {
    if (!m->veces || m->dueno != actual + 1) taek_media_trampa("iris: pthread_mutex_unlock sin ser el dueno");
    if (!--m->veces) m->dueno = 0;
    return 0;
}

/* Condicion: una generacion. Despertar a todos esta permitido (los despertares espurios son legales). */
struct cond_v { unsigned gen; };
_Static_assert(sizeof(struct cond_v) <= 48, "pthread_cond_t");
struct ts { long s, ns; };
int pthread_condattr_init(unsigned *a) { *a = 0; return 0; }
int pthread_condattr_destroy(unsigned *a) { (void)a; return 0; }
int pthread_condattr_setclock(unsigned *a, int c) { (void)a; (void)c; return 0; }   /* un solo reloj, desde el arranque */
int pthread_cond_init(struct cond_v *c, const void *a) { (void)a; c->gen = 0; return 0; }
int pthread_cond_destroy(struct cond_v *c) { (void)c; return 0; }
int pthread_cond_signal(struct cond_v *c) { c->gen++; return 0; }
int pthread_cond_broadcast(struct cond_v *c) { c->gen++; return 0; }
int pthread_cond_wait(struct cond_v *c, struct mutex_v *m) {
    unsigned g = c->gen;
    pthread_mutex_unlock(m);
    while (c->gen == g) esperar_un_poco();
    return pthread_mutex_lock(m);
}
int pthread_cond_timedwait(struct cond_v *c, struct mutex_v *m, const struct ts *t) {
    uint64_t fin = (uint64_t)t->s * 1000000000ull + (uint64_t)t->ns;
    unsigned g = c->gen;
    int r = 0;
    pthread_mutex_unlock(m);
    while (c->gen == g) {
        if (taek_media_ns() >= fin) { r = ETIMEDOUT; break; }
        esperar_un_poco();
    }
    pthread_mutex_lock(m);
    return r;
}

struct barrera_v { unsigned n, cuenta, gen; };
_Static_assert(sizeof(struct barrera_v) <= 32, "pthread_barrier_t");
int pthread_barrier_init(struct barrera_v *b, const void *a, unsigned n) { (void)a; if (!n) return EINVAL; b->n = n; b->cuenta = 0; b->gen = 0; return 0; }
int pthread_barrier_destroy(struct barrera_v *b) { (void)b; return 0; }
int pthread_barrier_wait(struct barrera_v *b) {
    unsigned g = b->gen;
    if (++b->cuenta == b->n) { b->cuenta = 0; b->gen++; return -1; }   /* PTHREAD_BARRIER_SERIAL_THREAD */
    while (b->gen == g) esperar_un_poco();
    return 0;
}
int pthread_once(int *o, void (*f)(void)) {
    if (*o == 0) { *o = 1; f(); *o = 2; return 0; }
    while (*o == 1) esperar_un_poco();
    return 0;
}

/* Afinidad, nombre, prioridad y senales: un CPU y sin senales. Se aceptan sin efecto; la mascara dice "CPU 0". */
int pthread_setname_np(pthread_t t, const char *n) { (void)t; (void)n; return 0; }
int pthread_setschedparam(pthread_t t, int p, const void *s) { (void)t; (void)p; (void)s; return 0; }
int pthread_sigmask(int c, const void *n, void *v) { (void)c; (void)n; if (v) memset(v, 0, 128); return 0; }
int sigfillset(void *s) { memset(s, 0xff, 128); return 0; }
int sigdelset(void *s, int n) { if (n < 1 || n > 1024) { ERRNO = EINVAL; return -1; } ((uint64_t *)s)[(n - 1) / 64] &= ~(1ull << ((n - 1) % 64)); return 0; }
static void cpu0(size_t bytes, void *m) { memset(m, 0, bytes); if (bytes) *(unsigned char *)m = 1; }
int pthread_getaffinity_np(pthread_t t, size_t b, void *m) { (void)t; cpu0(b, m); return 0; }
int pthread_setaffinity_np(pthread_t t, size_t b, const void *m) { (void)t; (void)b; (void)m; return 0; }
int sched_getaffinity(int pid, size_t b, void *m) { (void)pid; cpu0(b, m); return 0; }
int __sched_cpucount(size_t b, const void *m) { int n = 0; const unsigned char *p = m; for (size_t i = 0; i < b; i++) n += __builtin_popcount(p[i]); return n; }
int setpriority(int w, unsigned q, int p) { (void)w; (void)q; (void)p; return 0; }

/* syscall: futex (para simple_mtx y util_queue_fence de Mesa) y gettid. Despertar no hace nada: quien espera vuelve a mirar. */
#define SYS_GETTID 186
#define SYS_FUTEX 202
long syscall(long n, ...) {
    va_list a; va_start(a, n);
    if (n == SYS_GETTID) { va_end(a); return actual + 1; }
    if (n != SYS_FUTEX) { va_end(a); ERRNO = ENOSYS; return -1; }
    volatile int *dir = va_arg(a, int *);
    int op = va_arg(a, int);
    int val = va_arg(a, int);
    const struct ts *t = va_arg(a, const struct ts *);
    va_end(a);
    int base = op & 127;   /* sin FUTEX_PRIVATE_FLAG (128) ni FUTEX_CLOCK_REALTIME (256) */
    if (base == 1 || base == 10) return 0;              /* WAKE, WAKE_BITSET */
    if (base != 0 && base != 9) { ERRNO = ENOSYS; return -1; }
    if (*dir != val) { ERRNO = EAGAIN; return -1; }
    uint64_t fin = 0;
    if (t) fin = (base == 0 ? taek_media_ns() : 0) + (uint64_t)t->s * 1000000000ull + (uint64_t)t->ns;   /* WAIT: relativo; BITSET: absoluto */
    while (*dir == val) {
        if (t && taek_media_ns() >= fin) { ERRNO = ETIMEDOUT; return -1; }
        esperar_un_poco();
    }
    return 0;
}

/* ======================================================================= libm (x87, precision extendida) */
double sin(double x) { long double r = x; __asm__("fsin" : "+t"(r)); return (double)r; }
double cos(double x) { long double r = x; __asm__("fcos" : "+t"(r)); return (double)r; }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
/* El compilador junta sin y cos del mismo argumento en una sola llamada. */
void sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
void sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
static long double log2_l(long double x) { long double r; __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(1.0L) : "st(1)"); return r; }
static long double exp2_l(long double x) {
    long double i, f, r;
    __asm__("frndint" : "=t"(i) : "0"(x));
    f = x - i;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1.0L;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
    return r;
}
#define INF __builtin_inf()
#define NAN_ __builtin_nan("")
static int es_nan(double x) { return x != x; }
double log2(double x) { if (es_nan(x) || x < 0) return NAN_; if (x == 0) return -INF; if (x == INF) return x; return (double)log2_l(x); }
double log(double x) { return log2(x) * 0.69314718055994530942; }
double log10(double x) { return log2(x) * 0.30102999566398119521; }
float logf(float x) { return (float)log(x); }
float log2f(float x) { return (float)log2(x); }
double exp2(double x) { if (es_nan(x)) return x; if (x >= 1024) return INF; if (x < -1080) return 0; return (double)exp2_l(x); }
float exp2f(float x) { return (float)exp2(x); }
double exp(double x) { return exp2(x * 1.44269504088896340736); }
double pow(double x, double y) {
    if (y == 0 || x == 1) return 1;
    if (es_nan(x) || es_nan(y)) return NAN_;
    long double yl = y, ent;
    __asm__("frndint" : "=t"(ent) : "0"(yl));
    int entero = ent == yl, impar = entero && __builtin_fabsl(yl) < 9007199254740992.0L && ((long long)yl & 1);
    if (x == 0) return y < 0 ? (impar ? __builtin_copysign(INF, x) : INF) : (impar ? x : 0);
    if (x < 0 && !entero) return NAN_;
    long double m = __builtin_fabsl((long double)x);
    long double e = yl * log2_l(m);
    if (e >= 16384) return impar && x < 0 ? -INF : INF;
    if (e <= -16500) return impar && x < 0 ? -0.0 : 0;
    double r = (double)exp2_l(e);
    return x < 0 && impar ? -r : r;
}
float powf(float x, float y) { return (float)pow(x, y); }
double fmin(double a, double b) { return es_nan(a) ? b : es_nan(b) ? a : a < b ? a : b; }
double fmax(double a, double b) { return es_nan(a) ? b : es_nan(b) ? a : a > b ? a : b; }
float fminf(float a, float b) { return (float)fmin(a, b); }
float fmaxf(float a, float b) { return (float)fmax(a, b); }
extern double ceil(double);
float ceilf(float x) { return (float)ceil(x); }
double ldexp(double x, int e) {
    long double r = x, i = e;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(i));
    return (double)r;
}
float ldexpf(float x, int e) { return (float)ldexp(x, e); }
double frexp(double x, int *e) {
    *e = 0;
    if (x == 0 || es_nan(x) || x == INF || x == -INF) return x;
    union { double d; uint64_t u; } v = {x};
    int ex = (int)((v.u >> 52) & 0x7ff);
    if (!ex) { v.d = x * 18014398509481984.0; ex = (int)((v.u >> 52) & 0x7ff) - 54; }   /* subnormal: x * 2^54 */
    *e = ex - 1022;
    v.u = (v.u & ~(0x7ffull << 52)) | (1022ull << 52);
    return v.d;
}
float frexpf(float x, int *e) { return (float)frexp(x, e); }
/* fma: fmaf es exacta salvo un doble redondeo raro (producto exacto en double). fma doble: producto exacto de Dekker y
 * suma compensada; correcta salvo casos limite de redondeo. Se usa al plegar constantes de shaders. */
float fmaf(float a, float b, float c) { return (float)((double)a * (double)b + (double)c); }
static void dividir(double a, double *hi, double *lo) { double t = a * 134217729.0; *hi = t - (t - a); *lo = a - *hi; }
double fma(double a, double b, double c) {
    if (es_nan(a) || es_nan(b) || es_nan(c)) return a * b + c;
    double p = a * b;
    if (p == INF || p == -INF || p == 0 || c == INF || c == -INF) return p + c;
    double ah, al, bh, bl;
    dividir(a, &ah, &al); dividir(b, &bh, &bl);
    double e = ((ah * bh - p) + ah * bl + al * bh) + al * bl;   /* a*b = p + e exacto */
    double s = p + c, v = s - p, err = (p - (s - v)) + (c - v);  /* p + c = s + err exacto */
    return s + (err + e);
}
int __popcountdi2(long long a) {
    unsigned long long x = (unsigned long long)a;
    x = x - ((x >> 1) & 0x5555555555555555ull);
    x = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (int)((x * 0x0101010101010101ull) >> 56);
}

/* ======================================================================= libdrm minima */
typedef struct { int version_major, version_minor, version_patchlevel, name_len; char *name; int date_len; char *date; int desc_len; char *desc; } drmVersion;
struct drm_version { int version_major, version_minor, version_patchlevel; size_t name_len; char *name; size_t date_len; char *date; size_t desc_len; char *desc; };
_Static_assert(sizeof(struct drm_version) == 64, "drm_version");
#define DRM_IOCTL_VERSION 0xC0406400ul
#define DRM_IOCTL_GET_CAP 0xC010640Cul
#define DRM_IOCTL_I915_GETPARAM 0xC0106446ul
drmVersion *drmGetVersion(int fd) {
    struct drm_version v; memset(&v, 0, sizeof(v));
    if (ioctl(fd, DRM_IOCTL_VERSION, &v)) return 0;
    char *n = calloc(1, v.name_len + 1), *d = calloc(1, v.date_len + 1), *s = calloc(1, v.desc_len + 1);
    drmVersion *r = calloc(1, sizeof(*r));
    if (!n || !d || !s || !r) { free(n); free(d); free(s); free(r); return 0; }
    v.name = n; v.date = d; v.desc = s;
    if (ioctl(fd, DRM_IOCTL_VERSION, &v)) { free(n); free(d); free(s); free(r); return 0; }
    *r = (drmVersion){v.version_major, v.version_minor, v.version_patchlevel, (int)v.name_len, n, (int)v.date_len, d, (int)v.desc_len, s};
    return r;
}
void drmFreeVersion(drmVersion *v) { if (v) { free(v->name); free(v->date); free(v->desc); free(v); } }
int drmGetCap(int fd, uint64_t cap, uint64_t *valor) {
    struct { uint64_t cap, val; } c = {cap, 0};
    int r = ioctl(fd, DRM_IOCTL_GET_CAP, &c);
    if (r) return -ERRNO;
    *valor = c.val;
    return 0;
}
typedef struct { uint16_t domain; uint8_t bus, dev, func; } drmPciBusInfo;
typedef struct { uint16_t vendor_id, device_id, subvendor_id, subdevice_id; uint8_t revision_id; } drmPciDeviceInfo;
typedef struct { char **nodes; int available_nodes, bustype; drmPciBusInfo *pci_bus; drmPciDeviceInfo *pci_dev; } drmDevice;
static int parametro(int fd, int p, int *v) { struct { int param; int *valor; } g = {p, v}; return ioctl(fd, DRM_IOCTL_I915_GETPARAM, &g); }
/* La GPU integrada de Intel esta siempre en 0000:00:02.0. Id y revision se leen del driver original. */
int drmGetDevice2(int fd, uint32_t flags, drmDevice **o) {
    (void)flags;
    int id = 0, rev = 0;
    if (parametro(fd, 4, &id) || parametro(fd, 32, &rev)) return -ERRNO;   /* I915_PARAM_CHIPSET_ID, I915_PARAM_REVISION */
    struct { drmDevice d; char *nodos[3]; char render[24]; drmPciBusInfo b; drmPciDeviceInfo i; } *t = calloc(1, sizeof(*t));
    if (!t) return -ENOMEM;
    memcpy(t->render, "/dev/dri/renderD128", 20);
    t->nodos[2] = t->render;                   /* DRM_NODE_RENDER */
    t->b = (drmPciBusInfo){0, 0, 2, 0};
    t->i = (drmPciDeviceInfo){0x8086, (uint16_t)id, 0, 0, (uint8_t)rev};
    t->d = (drmDevice){t->nodos, 1 << 2, 0 /* DRM_BUS_PCI */, &t->b, &t->i};
    *o = &t->d;
    return 0;
}
void drmFreeDevice(drmDevice **d) { if (d && *d) { free(*d); *d = 0; } }
int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *o) { (void)fd; (void)h; (void)f; (void)o; return -ENOSYS; }   /* sin dma-buf entre procesos */
/* H194: "dma-buf local". TAEK no tiene dma-buf; Iris solo importa por descriptor (iris_bo_import_dmabuf). Dentro de un mismo
 * drm_file un handle GEM ya es la identidad del objeto, asi que un descriptor ficticio lo representa: drmPrimeFDToHandle lo
 * traduce y lseek(SEEK_END) da el tamano. Solo para objetos del drm_file compartido (p. ej. la superficie de Media Driver). */
#define DMABUF_BASE 1000
#define DMABUF_MAX 16
static struct { int usado; uint32_t handle; uint64_t bytes; } dmabuf[DMABUF_MAX];
static int es_dmabuf(int fd) { return fd >= DMABUF_BASE && fd < DMABUF_BASE + DMABUF_MAX && dmabuf[fd - DMABUF_BASE].usado; }
int taek_iris_dmabuf(uint32_t handle, uint64_t bytes) {
    for (int i = 0; i < DMABUF_MAX; i++)
        if (!dmabuf[i].usado) { dmabuf[i].usado = 1; dmabuf[i].handle = handle; dmabuf[i].bytes = bytes; return DMABUF_BASE + i; }
    return -1;
}
void taek_iris_dmabuf_soltar(int fd) { if (es_dmabuf(fd)) dmabuf[fd - DMABUF_BASE].usado = 0; }
int drmPrimeFDToHandle(int fd, int pfd, uint32_t *h) {
    (void)fd;
    if (!es_dmabuf(pfd)) return -ENOSYS;
    *h = dmabuf[pfd - DMABUF_BASE].handle;
    return 0;
}

/* ======================================================================= libc que faltaba */
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); return d ? memcpy(d, s, n) : 0; }
char *strtok(char *s, const char *sep) { static char *g; return strtok_r(s, sep, &g); }
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *a) { size_t n = 0; while (s[n] && !strchr(a, s[n])) n++; return n; }
static int minus(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int strcasecmp(const char *a, const char *b) { while (*a && minus((unsigned char)*a) == minus((unsigned char)*b)) a++, b++; return minus((unsigned char)*a) - minus((unsigned char)*b); }
void *memmem(const void *h, size_t hn, const void *n, size_t nn) {
    if (!nn) return (void *)h;
    for (const char *p = h; hn >= nn; p++, hn--) if (!memcmp(p, n, nn)) return (void *)p;
    return 0;
}
static void intercambiar(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
static void hundir(char *v, size_t i, size_t n, size_t t, int (*c)(const void *, const void *)) {
    for (;;) {
        size_t h = 2 * i + 1, m = i;
        if (h < n && c(v + h * t, v + m * t) > 0) m = h;
        if (h + 1 < n && c(v + (h + 1) * t, v + m * t) > 0) m = h + 1;
        if (m == i) return;
        intercambiar(v + i * t, v + m * t, t);
        i = m;
    }
}
void qsort(void *b, size_t n, size_t t, int (*c)(const void *, const void *)) {   /* heapsort: sin recursion ni memoria */
    char *v = b;
    for (size_t i = n / 2; i-- > 0;) hundir(v, i, n, t, c);
    for (size_t k = n; k > 1; k--) { intercambiar(v, v + (k - 1) * t, t); hundir(v, 0, k - 1, t, c); }
}
/* Tabla de clases de caracteres de glibc (C locale): indices -128..255. */
static unsigned short clases[384];
const unsigned short **__ctype_b_loc(void) {
    static const unsigned short *p;
    if (!p) {
        for (int c = 0; c < 128; c++) {
            unsigned short m = 0;
            int up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z', di = c >= '0' && c <= '9';
            int sp = c == ' ' || (c >= 9 && c <= 13), bl = c == ' ' || c == '\t', cn = c < 32 || c == 127;
            int xd = di || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'), pr = c >= 32 && c < 127, gr = c > 32 && c < 127;
            if (up) m |= 0x100; if (lo) m |= 0x200; if (up || lo) m |= 0x400; if (di) m |= 0x800; if (xd) m |= 0x1000;
            if (sp) m |= 0x2000; if (pr) m |= 0x4000; if (gr) m |= 0x8000; if (bl) m |= 0x1; if (cn) m |= 0x2;
            if (gr && !up && !lo && !di) m |= 0x4; if (up || lo || di) m |= 0x8;
            clases[128 + c] = m;
        }
        p = clases + 128;
    }
    return &p;
}
int asprintf(char **o, const char *f, ...) { va_list a; va_start(a, f); int r = vasprintf(o, f, a); va_end(a); return r; }
int vfprintf(void *s, const char *f, va_list a) { (void)s; char t[200]; vsnprintf(t, sizeof(t), f, a); taek_media_log(t); return 0; }
int fputc(int c, void *s) { (void)s; return c; }
int putc(int c, void *s) { (void)s; return c; }
void abort(void) { taek_media_trampa("iris: abort"); }
void openlog(const char *i, int o, int f) { (void)i; (void)o; (void)f; }
void syslog(int p, const char *f, ...) { (void)p; va_list a; va_start(a, f); char t[200]; vsnprintf(t, sizeof(t), f, a); va_end(a); taek_media_log(t); }
char *program_invocation_name = (char *)"taek-iris";
/* Sin motor de expresiones regulares. driconf (xmlconfig.c) compara el nombre del programa con su tabla interna solo si
 * regcomp devuelve 0: con REG_ESPACE ninguna entrada por programa aplica, que es lo correcto para "taek-iris". */
int regcomp(void *r, const char *e, int f) { (void)r; (void)e; (void)f; return 12; }   /* REG_ESPACE */
int regexec(const void *r, const char *s, size_t n, void *m, int f) { (void)r; (void)s; (void)n; (void)m; (void)f; return 1; }   /* REG_NOMATCH */
void regfree(void *r) { (void)r; }
unsigned getgid(void) { return 0; }
unsigned getegid(void) { return 0; }

/* Archivos: solo el DRM (rt_libc.c). Variantes de 64 bits de glibc y lo minimo que pide Iris. */
#define FD_DRM_MIN 100
int open64(const char *p, int f, ...) { return open(p, f); }
int openat64(int d, const char *p, int f, ...) { (void)d; (void)p; (void)f; ERRNO = ENOENT; return -1; }
void *mmap64(void *d, size_t b, int p, int f, int fd, long o) { return mmap(d, b, p, f, fd, o); }
/* os_dupfd_cloexec: el "duplicado" del DRM es el mismo descriptor; cerrar el DRM no hace nada (rt_libc.c). */
int fcntl64(int fd, int c, ...) {
    if (fd < FD_DRM_MIN) { ERRNO = EBADF; return -1; }
    if (c == 0 || c == 1030) return fd;   /* F_DUPFD, F_DUPFD_CLOEXEC */
    if (c == 1 || c == 2 || c == 3 || c == 4) return 0;   /* F_GETFD, F_SETFD, F_GETFL, F_SETFL */
    ERRNO = EINVAL; return -1;
}
/* iris_bufmgr_get_for_fd empieza con fstat del DRM y compara st_rdev entre pantallas: se describe como el renderD128 de
 * Linux (dispositivo de caracteres, mayor 226, menor 128). struct stat de glibc x86-64: 144 bytes. */
struct stat_glibc { uint64_t dev, ino, nlink; uint32_t mode, uid, gid, pad; uint64_t rdev; int64_t size, blksize, blocks; int64_t t[6]; int64_t res[3]; };
_Static_assert(sizeof(struct stat_glibc) == 144, "struct stat");
int fstat(int fd, void *s) {
    if (fd < FD_DRM_MIN) { ERRNO = EBADF; return -1; }
    struct stat_glibc *e = s;
    memset(e, 0, sizeof(*e));
    e->dev = 5; e->ino = 1; e->nlink = 1;
    e->mode = 0020000 | 0666;                 /* S_IFCHR */
    e->rdev = (226u << 8) | 128u;             /* makedev(226, 128) de glibc */
    e->blksize = 4096;
    return 0;
}
int fstat64(int fd, void *s) { return fstat(fd, s); }
int stat64(const char *p, void *s) { (void)p; (void)s; ERRNO = ENOENT; return -1; }
int access(const char *p, int m) { (void)p; (void)m; ERRNO = ENOENT; return -1; }
long lseek64(int fd, long o, int w) {
    if (es_dmabuf(fd)) {   /* H194: iris_bo_import_dmabuf pide el tamano con SEEK_END */
        if (w == 2) return (long)dmabuf[fd - DMABUF_BASE].bytes;
        if (w == 0) return o;
    }
    ERRNO = EBADF; return -1;
}
void *fopen64(const char *p, const char *m) { (void)p; (void)m; ERRNO = ENOENT; return 0; }
int fclose(void *f) { (void)f; return 0; }
long ftell(void *f) { (void)f; ERRNO = EBADF; return -1; }
void *readdir64(void *d) { (void)d; return 0; }
int closedir(void *d) { (void)d; return 0; }
int unlink(const char *p) { (void)p; ERRNO = ENOENT; return -1; }
int mkfifoat(int d, const char *p, unsigned m) { (void)d; (void)p; (void)m; ERRNO = ENOSYS; return -1; }
