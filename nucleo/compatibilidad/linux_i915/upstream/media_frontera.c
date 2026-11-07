/* Lo que Media Driver pide al kernel. Es su "espacio de usuario": su memoria y su pila se ven en la mitad baja de las direcciones
 * (alias 0x6000_0000_0000 del mapa fisico), asi que el access_ok y el copy_from_user de i915 valen sin modificar i915. Los ioctl
 * entran por drm_ioctl original sobre un drm_file original. La traza de ioctl realmente usados es la tabla que pide M14. */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/signal.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/fs.h>
#include <linux/pid.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include "../media/taek_media.h"

extern void taek_early_trampa(const char *) __attribute__((noreturn));
struct drm_file *drm_file_alloc(struct drm_minor *minor);
struct drm_i915_gem_object;
void *taek_original_mmap(struct drm_file *, unsigned long, unsigned long, struct drm_i915_gem_object **, void **);
void taek_original_munmap(struct drm_i915_gem_object *, void *);

#define DIRECTO 0xffff888000000000UL
#define USUARIO 0x0000600000000000UL
#define VENTANA (1UL << 39)
static void *a_usuario(void *p) {
    unsigned long v = (unsigned long)p;
    if (v - DIRECTO >= VENTANA) taek_media_trampa("direccion fuera del mapa lineal");
    return (void *)(v - DIRECTO + USUARIO);
}
static void *a_kernel(void *p) {
    unsigned long v = (unsigned long)p;
    if (v - USUARIO >= VENTANA) taek_media_trampa("direccion fuera de la ventana de usuario");
    return (void *)(v - USUARIO + DIRECTO);
}

void *taek_media_alloc(size_t n) { void *p = kmalloc(n, GFP_KERNEL); return p ? a_usuario(p) : NULL; }
void taek_media_free(void *p) { kfree(a_kernel(p)); }
void taek_media_log(const char *l) { pr_info("[MEDIA] %s\n", l); }
unsigned long long taek_media_ns(void) { return local_clock(); }
void taek_media_dormir_us(unsigned long long us) { usleep_range(us, us); }
void *taek_media_yo(void) { return current; }
void taek_media_trampa(const char *s) { pr_err("[MEDIA] trampa: %s\n", s); taek_early_trampa(s); }

/* ------------------------------------------------------------- memoria de usuario */
unsigned long _copy_from_user(void *d, const void __user *s, unsigned long n) {
    if (!access_ok(s, n)) { memset(d, 0, n); return n; }
    memcpy(d, (__force const void *)s, n);
    return 0;
}
unsigned long _copy_to_user(void __user *d, const void *s, unsigned long n) {
    if (!access_ok(d, n)) return n;
    memcpy((__force void *)d, s, n);
    return 0;
}
long __copy_user_nocache(void *d, const void __user *s, unsigned n) { memcpy(d, (__force const void *)s, n); return 0; }
void *memdup_user(const void __user *s, size_t n) {
    void *p = kmalloc(n, GFP_KERNEL);
    if (!p) return ERR_PTR(-ENOMEM);
    if (_copy_from_user(p, s, n)) { kfree(p); return ERR_PTR(-EFAULT); }
    return p;
}
/* Convencion de getuser.S y putuser.S. Un puntero invalido del driver es un fallo de pagina con su evidencia, no un error mudo. */
asm(".pushsection .text.taek_user,\"ax\"\n"
    ".global __get_user_1,__get_user_2,__get_user_4,__get_user_8,__get_user_nocheck_1,__get_user_nocheck_2,__get_user_nocheck_4,__get_user_nocheck_8\n"
    "__get_user_1:\n__get_user_nocheck_1: movzbl (%rax),%edx; xor %eax,%eax; ret\n"
    "__get_user_2:\n__get_user_nocheck_2: movzwl (%rax),%edx; xor %eax,%eax; ret\n"
    "__get_user_4:\n__get_user_nocheck_4: movl (%rax),%edx; xor %eax,%eax; ret\n"
    "__get_user_8:\n__get_user_nocheck_8: movq (%rax),%rdx; xor %eax,%eax; ret\n"
    ".global __put_user_1,__put_user_2,__put_user_4,__put_user_8,__put_user_nocheck_1,__put_user_nocheck_2,__put_user_nocheck_4,__put_user_nocheck_8\n"
    "__put_user_1:\n__put_user_nocheck_1: movb %al,(%rcx); xor %ecx,%ecx; ret\n"
    "__put_user_2:\n__put_user_nocheck_2: movw %ax,(%rcx); xor %ecx,%ecx; ret\n"
    "__put_user_4:\n__put_user_nocheck_4: movl %eax,(%rcx); xor %ecx,%ecx; ret\n"
    "__put_user_8:\n__put_user_nocheck_8: movq %rax,(%rcx); xor %ecx,%ecx; ret\n"
    ".popsection\n");

/* ------------------------------------------------------------- un solo proceso, en Ring 0 */
bool capable(int cap) { (void)cap; return true; }
static struct pid pid_unico = {.count = REFCOUNT_INIT(1), .level = 0};
static struct signal_struct senales;
struct pid *get_task_pid(struct task_struct *t, enum pid_type ty) { (void)t; (void)ty; return get_pid(&pid_unico); }
void put_pid(struct pid *p) { if (p && p != &pid_unico) taek_media_trampa("put_pid ajeno"); }
struct task_struct *pid_task(struct pid *p, enum pid_type ty) { (void)p; (void)ty; return current; }
pid_t __task_pid_nr_ns(struct task_struct *t, enum pid_type ty, struct pid_namespace *ns) { (void)t; (void)ty; (void)ns; return 1; }

/* ------------------------------------------------------------- dispositivo DRM */
#define FD_DRM 100
static struct drm_device *dispositivo;
static struct file archivo;
static struct drm_file *abierto;
#define NR_MAX 0x100
static struct { unsigned veces, errores; } traza[NR_MAX];
static unsigned errores_impresos;

int taek_media_abrir_drm(void) {
    if (abierto) return FD_DRM;
    if (!dispositivo || !dispositivo->render) return -ENODEV;
    if (!current->signal) current->signal = &senales;
    struct drm_file *f = drm_file_alloc(dispositivo->render);
    if (IS_ERR(f)) return PTR_ERR(f);
    f->filp = &archivo;
    archivo.private_data = f;
    abierto = f;
    return FD_DRM;
}
int taek_media_cerrar_drm(int fd) { return fd == FD_DRM ? 0 : -EBADF; }
int taek_media_ioctl(int fd, unsigned long o, void *a) {
    if (fd != FD_DRM || !abierto) return -EBADF;
    if (_IOC_TYPE(o) != DRM_IOCTL_BASE) return -ENOTTY;
    /* Cada orden del modulo corre en una tarea nueva: todas son el mismo "proceso". */
    if (!current->signal) current->signal = &senales;
    long r = drm_ioctl(&archivo, o, (unsigned long)a);
    unsigned nr = _IOC_NR(o) & (NR_MAX - 1);
    traza[nr].veces++;
    if (r < 0) {
        traza[nr].errores++;
        if (errores_impresos++ < 40) pr_info("[MEDIA] ioctl nr=%#x rc=%ld\n", _IOC_NR(o), r);
    }
    return r;
}

#define MAPAS_MAX 4096
static struct { void *u; size_t b; long off; struct drm_i915_gem_object *o; void *gtt; } mapas[MAPAS_MAX];
/* Linux quita aqui las paginas de usuario de un objeto. El shim no crea ninguna: cada mapa de esta tabla retiene el objeto y lo deja
 * fijado hasta su munmap, de modo que al liberar un objeto no queda nada que quitar. Pedirlo para un mapa vivo seria revocarlo, y
 * eso no esta hecho: se detiene con su nombre. */
void unmap_mapping_range(struct address_space *m, loff_t const ini, loff_t const n, int cows) {
    (void)m; (void)cows;
    for (unsigned i = 0; i < MAPAS_MAX; i++)
        if (mapas[i].u && mapas[i].off + (long)mapas[i].b > ini && (!n || mapas[i].off < ini + n))
            taek_media_trampa("unmap_mapping_range de un mapa vivo");
}
void *taek_media_mmap(void *d, size_t b, int p, int f, int fd, long off) {
    (void)d; (void)p; (void)f;
    if (fd != FD_DRM || !abierto) return ERR_PTR(-EBADF);
    struct drm_i915_gem_object *o;
    void *gtt = NULL;
    void *k = taek_original_mmap(abierto, off, b, &o, &gtt);
    if (IS_ERR(k)) return k;
    /* La apertura es memoria de dispositivo mapeada aparte: el driver la usa tal cual, solo desde la CPU. */
    void *u = gtt ? k : a_usuario(k);
    for (unsigned i = 0; i < MAPAS_MAX; i++)
        if (!mapas[i].u) { mapas[i].u = u; mapas[i].b = b; mapas[i].off = off; mapas[i].o = o; mapas[i].gtt = gtt; return u; }
    taek_media_trampa("tabla de mapas llena");
}
int taek_media_munmap(void *d, size_t b) {
    for (unsigned i = 0; i < MAPAS_MAX; i++)
        if (mapas[i].u == d) {
            if (mapas[i].b != b) taek_media_trampa("munmap parcial");
            taek_original_munmap(mapas[i].o, mapas[i].gtt);
            mapas[i].u = NULL;
            return 0;
        }
    return -EINVAL;
}

/* ------------------------------------------------------------- pila propia */
#define PILA_BYTES (2u << 20)
#define PILA_MARCA 0x5a
static int llamar_en_pila(char *tope, int orden, char *t, size_t n) {
    register int r asm("eax");
    /* rbx es callee-saved: conserva la pila de la fibra mientras el driver usa la grande. */
    asm volatile("mov %%rsp, %%rbx\n\t"
                 "mov %[tope], %%rsp\n\t"
                 "movabs $taek_media_entrada, %%rax\n\t"
                 "call *%%rax\n\t"
                 "mov %%rbx, %%rsp\n\t"
                 : "=a"(r), "+D"(orden), "+S"(t), "+d"(n)
                 : [tope] "r"(tope)
                 : "rbx", "rcx", "r8", "r9", "r10", "r11", "memory", "cc");
    return r;
}

int taek_media_probar(struct drm_device *drm, int orden, char *t, size_t n, unsigned *pila_usada) {
    char *pila = kmalloc(PILA_BYTES, GFP_KERNEL);
    /* t vive en la pila de la fibra, fuera de la ventana de usuario: el driver escribe en una copia. */
    char *texto = kzalloc(n, GFP_KERNEL);
    if (!pila || !texto) return -ENOMEM;
    dispositivo = drm;
    memset(pila, PILA_MARCA, PILA_BYTES);
    int r = llamar_en_pila((char *)a_usuario(pila) + PILA_BYTES - 64, orden, a_usuario(texto), n);
    memcpy(t, texto, n);
    kfree(texto);
    unsigned libre = 0;
    while (libre < PILA_BYTES && pila[libre] == PILA_MARCA) libre++;
    *pila_usada = PILA_BYTES - libre;
    if (!libre) taek_media_trampa("pila de Media Driver desbordada");
    kfree(pila);
    for (unsigned i = 0; i < NR_MAX; i++)
        if (traza[i].veces) pr_info("[MEDIA] traza ioctl nr=%#x veces=%u errores=%u\n", i, traza[i].veces, traza[i].errores);
    return r;
}

/* El reproductor llama por fotograma: la pila se conserva y solo se vigila su extremo. */
int taek_media_op(struct drm_device *drm, void *peticion) {
    static char *pila;
    if (!pila) {
        pila = kmalloc(PILA_BYTES, GFP_KERNEL);
        if (!pila) return -ENOMEM;
        memset(pila, PILA_MARCA, PILA_BYTES);
    }
    dispositivo = drm;
    int r = llamar_en_pila((char *)a_usuario(pila) + PILA_BYTES - 64, TAEK_MEDIA_OP, peticion, 0);
    for (unsigned i = 0; i < 64; i++)
        if (pila[i] != PILA_MARCA) taek_media_trampa("pila de Media Driver desbordada");
    return r;
}
