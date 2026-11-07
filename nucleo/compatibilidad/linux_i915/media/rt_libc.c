/* libc de Media Driver en TAEK. Lo que aqui existe funciona de verdad; lo que TAEK no tiene devuelve el error que daria un
 * sistema sin ese servicio (ENOENT, ENOSYS). Lo no listado no esta: el enlace lo convierte en una trampa con su nombre.
 * Sin cabeceras del sistema a proposito: estos prototipos son los del ABI de glibc con el que se compilo el driver. */
#include "taek_media.h"
#include <stdarg.h>
#include <stdint.h>

#define ENOENT 2
#define ESRCH 3
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EINVAL 22
#define ENOSYS 38
#define ETIMEDOUT 110
static int errno_taek;
int *__errno_location(void) { return &errno_taek; }
char __libc_single_threaded = 1;   /* Fibras cooperativas en un solo CPU: sin carreras en los contadores de shared_ptr. */

/* ------------------------------------------------------------- memoria basica */
void *memcpy(void *d, const void *s, size_t n) { void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); return r; }
void *memset(void *d, int c, size_t n) { void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    if (a == b || !n) return d;
    if (a < b || a >= b + n) return memcpy(d, s, n);
    while (n--) a[n] = b[n];
    return d;
}
int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
int bcmp(const void *x, const void *y, size_t n) { return memcmp(x, y, n); }
void *memchr(const void *s, int c, size_t n) {
    const unsigned char *a = s;
    for (; n; n--, a++) if (*a == (unsigned char)c) return (void *)a;
    return 0;
}

/* Cabecera delante de cada bloque: permite alinear a cualquier potencia de 2 y conocer el tamano en realloc. */
struct cab { void *crudo; size_t bytes; };
static void *reservar(size_t alin, size_t n) {
    if (alin < 16) alin = 16;
    if (n > (size_t)1 << 40) { errno_taek = ENOMEM; return 0; }
    char *c = taek_media_alloc(n + alin + sizeof(struct cab));
    if (!c) { errno_taek = ENOMEM; return 0; }
    uintptr_t u = ((uintptr_t)c + sizeof(struct cab) + alin - 1) & ~(uintptr_t)(alin - 1);
    struct cab *h = (struct cab *)u - 1;
    h->crudo = c; h->bytes = n;
    return (void *)u;
}
void *malloc(size_t n) { return reservar(16, n); }
void free(void *p) { if (p) taek_media_free(((struct cab *)p - 1)->crudo); }
void *calloc(size_t a, size_t b) {
    size_t n;
    if (__builtin_mul_overflow(a, b, &n)) { errno_taek = ENOMEM; return 0; }
    void *p = malloc(n);
    return p ? memset(p, 0, n) : 0;
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (!n) { free(p); return 0; }
    size_t v = ((struct cab *)p - 1)->bytes;
    void *q = malloc(n);
    if (q) { memcpy(q, p, v < n ? v : n); free(p); }
    return q;
}
void *memalign(size_t a, size_t n) {
    if (!a || (a & (a - 1))) { errno_taek = EINVAL; return 0; }
    return reservar(a, n);
}
int posix_memalign(void **o, size_t a, size_t n) {
    if (!a || (a & (a - 1)) || a % sizeof(void *)) return EINVAL;
    void *p = reservar(a, n);
    if (!p) return ENOMEM;
    *o = p;
    return 0;
}

/* ------------------------------------------------------------- cadenas */
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) break; }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
char *strstr(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; ; h++) { if (!strncmp(h, n, l)) return (char *)h; if (!*h) return 0; }
}
char *strndup(const char *s, size_t n) { size_t l = strnlen(s, n); char *d = malloc(l + 1); if (d) { memcpy(d, s, l); d[l] = 0; } return d; }
char *strtok_r(char *s, const char *sep, char **g) {
    if (!s) s = *g;
    while (*s && strchr(sep, *s)) s++;
    if (!*s) { *g = s; return 0; }
    char *t = s;
    while (*s && !strchr(sep, *s)) s++;
    if (*s) *s++ = 0;
    *g = s;
    return t;
}
char *strerror(int e) { (void)e; return (char *)"error"; }

static unsigned long long leer_ull(const char *s, char **fin, int base, int *neg) {
    const char *p = s; unsigned long long v = 0; int hay = 0;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    *neg = 0;
    if (*p == '-' || *p == '+') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x') { p += 2; base = 16; }
    else if (base == 0) base = *p == '0' ? 8 : 10;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'z' ? (*p | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d; hay = 1;
    }
    if (fin) *fin = (char *)(hay ? p : s);
    return v;
}
#define STRTO(N, T, SIGNO) T N(const char *s, char **f, int b) { int n; unsigned long long v = leer_ull(s, f, b, &n); return (T)(n ? 0 - v : v); }
STRTO(strtol, long, 1) STRTO(strtoll, long long, 1) STRTO(strtoul, unsigned long, 0) STRTO(strtoull, unsigned long long, 0)
STRTO(__isoc23_strtol, long, 1) STRTO(__isoc23_strtoll, long long, 1) STRTO(__isoc23_strtoul, unsigned long, 0) STRTO(__isoc23_strtoull, unsigned long long, 0)

/* ------------------------------------------------------------- formato */
struct salida { char *b; size_t cap, n; };
static void ponc(struct salida *o, char c) { if (o->n + 1 < o->cap) o->b[o->n] = c; o->n++; }
int vsnprintf(char *b, size_t cap, const char *f, va_list ap) {
    struct salida o = {b, cap, 0};
    for (; *f; f++) {
        if (*f != '%') { ponc(&o, *f); continue; }
        f++;
        int menos = 0, cero = 0, mas = 0, esp = 0, alt = 0, ancho = 0, prec = -1, largo = 0;
        for (;; f++) {
            if (*f == '-') menos = 1; else if (*f == '0') cero = 1; else if (*f == '+') mas = 1;
            else if (*f == ' ') esp = 1; else if (*f == '#') alt = 1; else break;
        }
        if (*f == '*') { ancho = va_arg(ap, int); f++; if (ancho < 0) { menos = 1; ancho = -ancho; } }
        else while (*f >= '0' && *f <= '9') ancho = ancho * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        for (;; f++) {
            if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't') largo = largo < 8 ? 8 : largo;
            else if (*f == 'h') { if (!largo) largo = -1; else if (largo == -1) largo = -2; }
            else break;
        }
        char t[32]; const char *s = t; size_t l = 0; char signo = 0;
        unsigned long long v = 0; unsigned base = 10; int mayus = 0;
        switch (*f) {
        case '%': ponc(&o, '%'); continue;
        case 'c': t[0] = (char)va_arg(ap, int); l = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; l = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s); break;
        case 'd': case 'i': {
            long long x = largo == 8 ? va_arg(ap, long long) : largo == -1 ? (short)va_arg(ap, int) : largo == -2 ? (signed char)va_arg(ap, int) : va_arg(ap, int);
            v = x < 0 ? 0ull - (unsigned long long)x : (unsigned long long)x;
            signo = x < 0 ? '-' : mas ? '+' : esp ? ' ' : 0;
            goto digitos;
        }
        case 'X': mayus = 1; /* fallthrough */
        case 'x': base = 16; goto sin_signo;
        case 'o': base = 8; /* fallthrough */
        case 'u': sin_signo:
            v = largo == 8 ? va_arg(ap, unsigned long long) : largo == -1 ? (unsigned short)va_arg(ap, unsigned) : largo == -2 ? (unsigned char)va_arg(ap, unsigned) : va_arg(ap, unsigned);
            goto digitos;
        case 'p': v = (uintptr_t)va_arg(ap, void *); base = 16; alt = 1; goto digitos;
        digitos: {
            char r[24]; int k = 0;
            do { unsigned d = (unsigned)(v % base); r[k++] = (char)(d < 10 ? '0' + d : (mayus ? 'A' : 'a') + d - 10); v /= base; } while (v);
            while (k < prec && k < (int)sizeof(r)) r[k++] = '0';
            if (alt && base == 16) { r[k++] = mayus ? 'X' : 'x'; r[k++] = '0'; }
            if (signo) r[k++] = signo;
            for (int i = 0; i < k; i++) t[i] = r[k - 1 - i];
            l = (size_t)k;
            break;
        }
        default:
            /* %f y compania no estan: mejor parar con nombre que imprimir algo falso. */
            taek_media_trampa("vsnprintf: conversion no admitida");
        }
        size_t relleno = (size_t)ancho > l ? (size_t)ancho - l : 0;
        if (!menos) for (size_t i = 0; i < relleno; i++) ponc(&o, cero && *f != 's' && *f != 'c' ? '0' : ' ');
        for (size_t i = 0; i < l; i++) ponc(&o, s[i]);
        if (menos) for (size_t i = 0; i < relleno; i++) ponc(&o, ' ');
    }
    if (cap) b[o.n < cap ? o.n : cap - 1] = 0;
    return (int)o.n;
}
int snprintf(char *b, size_t c, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(b, c, f, a); va_end(a); return r; }
int vasprintf(char **o, const char *f, va_list a) {
    va_list b; va_copy(b, a);
    int n = vsnprintf(0, 0, f, b);
    va_end(b);
    char *p = malloc((size_t)n + 1);
    if (!p) return -1;
    vsnprintf(p, (size_t)n + 1, f, a);
    *o = p;
    return n;
}

/* stdout y stderr son solo dos marcas: lo escrito va al registro del kernel. stdin y archivos no existen (fopen falla). */
static char marca_out, marca_err;
void *stdout = &marca_out, *stderr = &marca_err;
static void registrar(const char *f, va_list a) { char t[200]; vsnprintf(t, sizeof(t), f, a); taek_media_log(t); }
int printf(const char *f, ...) { va_list a; va_start(a, f); registrar(f, a); va_end(a); return 0; }
int fprintf(void *s, const char *f, ...) { (void)s; va_list a; va_start(a, f); registrar(f, a); va_end(a); return 0; }
int puts(const char *s) { taek_media_log(s); return 0; }
int fputs(const char *s, void *o) { (void)o; taek_media_log(s); return 0; }
size_t fwrite(const void *p, size_t a, size_t b, void *o) { (void)p; (void)o; return a * b; }
int fflush(void *o) { (void)o; return 0; }

/* Solo con taek_traza.h: un mensaje de error del driver, con su archivo y linea. */
void taek_traza(const char *a, int l, const char *m) {
    const char *b = strrchr(a, '/');
    char t[220];
    snprintf(t, sizeof(t), "%s:%d %s", b ? b + 1 : a, l, m);
    taek_media_log(t);
}

/* ------------------------------------------------------------- matematicas exactas */
double floor(double x) {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    double t = (double)(long long)x;
    return t > x ? t - 1.0 : t;
}
double ceil(double x) { return -floor(-x); }
float floorf(float x) { return (float)floor((double)x); }
double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

/* ------------------------------------------------------------- tiempo: sin reloj de pared, cuenta desde el arranque */
struct ts { long s, ns; };
int clock_gettime(int id, struct ts *t) { (void)id; unsigned long long n = taek_media_ns(); t->s = (long)(n / 1000000000ull); t->ns = (long)(n % 1000000000ull); return 0; }
int clock_getres(int id, struct ts *t) { (void)id; if (t) { t->s = 0; t->ns = 1; } return 0; }
int gettimeofday(struct ts *t, void *z) { (void)z; unsigned long long n = taek_media_ns(); t->s = (long)(n / 1000000000ull); t->ns = (long)(n % 1000000000ull / 1000); return 0; }
long time(long *o) { long s = (long)(taek_media_ns() / 1000000000ull); if (o) *o = s; return s; }
int usleep(unsigned u) { taek_media_dormir_us(u); return 0; }

/* ------------------------------------------------------------- proceso */
char *getenv(const char *n) { (void)n; return 0; }
int getpagesize(void) { return 4096; }
long sysconf(int n) {
    if (n == 30) return 4096;   /* _SC_PAGESIZE */
    if (n == 83 || n == 84) return 1;   /* _SC_NPROCESSORS_CONF, _SC_NPROCESSORS_ONLN */
    errno_taek = EINVAL;
    return -1;
}
int getpid(void) { return 1; }
unsigned getuid(void) { return 0; }
unsigned geteuid(void) { return 0; }
int fork(void) { errno_taek = ENOSYS; return -1; }
int kill(int p, int s) { (void)p; (void)s; errno_taek = ESRCH; return -1; }
int backtrace(void **b, int n) { (void)b; (void)n; return 0; }
int atexit(void (*f)(void)) { (void)f; return 0; }   /* Los modulos no se descargan: al salir no se ejecuta nada. */

/* Sin IPC entre procesos: un solo espacio de direcciones. */
int ftok(const char *p, int i) { (void)p; (void)i; errno_taek = ENOSYS; return -1; }
int semget(int k, int n, int f) { (void)k; (void)n; (void)f; errno_taek = ENOSYS; return -1; }
int semctl(int i, int n, int c, ...) { (void)i; (void)n; (void)c; errno_taek = ENOSYS; return -1; }
int semop(int i, void *o, size_t n) { (void)i; (void)o; (void)n; errno_taek = ENOSYS; return -1; }
int shmget(int k, size_t b, int f) { (void)k; (void)b; (void)f; errno_taek = ENOSYS; return -1; }
void *shmat(int i, const void *a, int f) { (void)i; (void)a; (void)f; errno_taek = ENOSYS; return (void *)-1; }
int shmdt(const void *a) { (void)a; errno_taek = ENOSYS; return -1; }
int shmctl(int i, int c, void *b) { (void)i; (void)c; (void)b; errno_taek = ENOSYS; return -1; }

/* Sin cargador dinamico. */
static const char *dl_error = "TAEK no tiene cargador dinamico";
void *dlopen(const char *f, int m) { (void)f; (void)m; return 0; }
void *dlsym(void *h, const char *s) { (void)h; (void)s; return 0; }
int dlclose(void *h) { (void)h; return -1; }
char *dlerror(void) { return (char *)dl_error; }

/* ------------------------------------------------------------- archivos: solo existe el dispositivo DRM */
#define FD_DRM_MIN 100
static int es_drm(int fd) { return fd >= FD_DRM_MIN; }
int open(const char *p, int f, ...) {
    (void)f;
    if (!strncmp(p, "/dev/dri/", 9)) return taek_media_abrir_drm();
    errno_taek = ENOENT;
    return -1;
}
int close(int fd) { if (es_drm(fd)) return taek_media_cerrar_drm(fd); errno_taek = EBADF; return -1; }
long read(int fd, void *b, size_t n) { (void)fd; (void)b; (void)n; errno_taek = EBADF; return -1; }
long write(int fd, const void *b, size_t n) { (void)b; if (fd == 1 || fd == 2) return (long)n; errno_taek = EBADF; return -1; }
long lseek(int fd, long o, int w) { (void)fd; (void)o; (void)w; errno_taek = EBADF; return -1; }
int stat(const char *p, void *s) { (void)p; (void)s; errno_taek = ENOENT; return -1; }
int fstat(int fd, void *s) { (void)fd; (void)s; errno_taek = ENOSYS; return -1; }   /* El DRM no es un nodo de ningun sistema de archivos. */
long readlink(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; errno_taek = ENOENT; return -1; }
char *realpath(const char *p, char *o) { (void)p; (void)o; errno_taek = ENOENT; return 0; }
void *fopen(const char *p, const char *m) { (void)p; (void)m; errno_taek = ENOENT; return 0; }
void *opendir(const char *p) { (void)p; errno_taek = ENOENT; return 0; }
int ioctl(int fd, unsigned long o, ...) {
    va_list a; va_start(a, o); void *arg = va_arg(a, void *); va_end(a);
    int r = taek_media_ioctl(fd, o, arg);
    if (r < 0) { errno_taek = -r; return -1; }
    return r;
}
#define MAP_ANONIMO 0x20
void *mmap(void *d, size_t b, int p, int f, int fd, long o) {
    if (f & MAP_ANONIMO) {
        void *a = memalign(4096, b);
        if (!a) return (void *)-1;
        return memset(a, 0, b);
    }
    void *r = taek_media_mmap(d, b, p, f, fd, o);
    if ((unsigned long)r > (unsigned long)-4096) { errno_taek = -(int)(long)r; return (void *)-1; }
    return r;
}
int munmap(void *d, size_t b) {
    if (taek_media_munmap(d, b)) free(d);   /* No era un objeto de la GPU: era memoria anonima. */
    return 0;
}

/* ------------------------------------------------------------- hilos: cooperativo, un CPU, sin hilos nuevos */
struct mutex_taek { void *dueno; unsigned veces, tipo; };
_Static_assert(sizeof(struct mutex_taek) <= 40, "pthread_mutex_t");
int pthread_mutexattr_init(unsigned *a) { *a = 0; return 0; }
int pthread_mutexattr_settype(unsigned *a, int t) { *a = (unsigned)t; return 0; }
int pthread_mutex_init(struct mutex_taek *m, const unsigned *a) { m->dueno = 0; m->veces = 0; m->tipo = a ? *a : 0; return 0; }
int pthread_mutex_destroy(struct mutex_taek *m) {
    if (m->veces) taek_media_trampa("pthread_mutex_destroy con el mutex tomado");
    return 0;
}
int pthread_mutex_lock(struct mutex_taek *m) {
    void *yo = taek_media_yo();
    if (m->veces && m->dueno == yo) {
        if (m->tipo != 1) taek_media_trampa("pthread_mutex_lock: reentrada en mutex no recursivo");
        m->veces++;
        return 0;
    }
    while (m->veces) taek_media_dormir_us(1000);
    m->dueno = yo; m->veces = 1;
    return 0;
}
int pthread_mutex_unlock(struct mutex_taek *m) {
    if (!m->veces || m->dueno != taek_media_yo()) taek_media_trampa("pthread_mutex_unlock sin ser el dueno");
    if (!--m->veces) m->dueno = 0;
    return 0;
}
int pthread_create(void *h, const void *a, void *(*f)(void *), void *x) { (void)h; (void)a; (void)f; (void)x; return EAGAIN; }

struct sem_taek { int v; };
_Static_assert(sizeof(struct sem_taek) <= 32, "sem_t");
int sem_init(struct sem_taek *s, int p, unsigned v) { (void)p; s->v = (int)v; return 0; }
int sem_destroy(struct sem_taek *s) { (void)s; return 0; }
int sem_post(struct sem_taek *s) { s->v++; return 0; }
int sem_trywait(struct sem_taek *s) { if (s->v > 0) { s->v--; return 0; } errno_taek = EAGAIN; return -1; }
int sem_wait(struct sem_taek *s) { while (s->v <= 0) taek_media_dormir_us(1000); s->v--; return 0; }
int sem_timedwait(struct sem_taek *s, const struct ts *t) {
    unsigned long long fin = (unsigned long long)t->s * 1000000000ull + (unsigned long long)t->ns;
    while (s->v <= 0) {
        if (taek_media_ns() >= fin) { errno_taek = ETIMEDOUT; return -1; }
        taek_media_dormir_us(1000);
    }
    s->v--;
    return 0;
}
