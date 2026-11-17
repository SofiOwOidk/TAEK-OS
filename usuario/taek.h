#ifndef TAEK_USUARIO_H
#define TAEK_USUARIO_H
/* Biblioteca minima de los programas de Ring 3. Solo las llamadas al sistema y lo que el compilador da por hecho. */
#include <stddef.h>
#include <stdint.h>
#include "nucleo/arquitectura/x86_64/anillo3_abi.h"

static inline long sys5(long n, long a, long b, long c, long d, long e) {
    register long r8 __asm__("r8") = e;
    register long rcx __asm__("rcx") = d;
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "c"(rcx), "r"(r8) : "memory");
    return r;
}
static inline long sys1(long n, long a) { return sys5(n, a, 0, 0, 0, 0); }

static inline __attribute__((noreturn)) void taek_salir(long codigo) {
    sys1(SYS_SALIR, codigo);
    for (;;) {}
}
static inline long taek_ms(void) { return sys1(SYS_TIEMPO, 0); }
static inline long taek_superficie(struct taek_superficie *s) { return sys1(SYS_SUPERFICIE, (long)s); }
static inline long taek_presentar(uint32_t id, long x, long y, long an, long al) { return sys5(SYS_PRESENTAR, id, x, y, an, al); }
static inline long taek_mover(uint32_t id, long x, long y) { return sys5(SYS_MOVER, id, x, y, 0, 0); }
static inline long taek_evento(struct taek_evento *e) { return sys1(SYS_EVENTO, (long)e); }
static inline long taek_pantalla(struct taek_pantalla *p) { return sys1(SYS_PANTALLA, (long)p); }
static inline long taek_esperar(long ms) { return sys1(SYS_ESPERAR, ms); }
static inline long taek_info(struct taek_info *t, long con_bateria) { return sys5(SYS_INFO, (long)t, con_bateria, 0, 0, 0); }
static inline long taek_captura(const char *nombre, long n) { return sys5(SYS_CAPTURA, (long)nombre, n, 0, 0, 0); }
static inline long taek_escribir(const char *s, long n) { return sys5(SYS_ESCRIBIR, (long)s, n, 0, 0, 0); }
static inline long taek_equipo(struct taek_equipo *t) { return sys1(SYS_EQUIPO, (long)t); }
static inline long taek_elevar(uint32_t id) { return sys1(SYS_ELEVAR, id); }
static inline long taek_web(struct taek_web *w) { return sys1(SYS_WEB, (long)w); }
/* H144: mueve el final del monton (como sbrk). Devuelve el final anterior o un negativo. Ver usuario/memoria.h. */
static inline long taek_memoria(long bytes) { return sys1(SYS_MEMORIA, bytes); }
/* H145: archivos (solo lectura), abrir otra aplicacion al salir y leer el argumento propio */
static inline long taek_archivos(struct taek_archivos *a) { return sys1(SYS_ARCHIVOS, (long)a); }
static inline long taek_lanzar(const struct taek_lanzar *l) { return sys1(SYS_LANZAR, (long)l); }
static inline long taek_argumento(char *b, long cap) { return sys5(SYS_ARGUMENTO, (long)b, cap, 0, 0, 0); }
/* H146: sonido PCM hacia la tarjeta */
static inline long taek_audio(struct taek_audio *a) { return sys1(SYS_AUDIO, (long)a); }
/* H148: otro tamano visible para una superficie, sin pedir memoria (dibujar antes con el ancho nuevo como fila) */
static inline long taek_tamano(uint32_t id, long x, long y, long an, long al) { return sys5(SYS_TAMANO, id, an, al, x, y); }
/* H150: temperatura, red, stream LAN y monton del nucleo. Un nucleo anterior devuelve -TAEK_ENOSYS. */
static inline long taek_sensores(struct taek_sensores *s) { return sys1(SYS_SENSORES, (long)s); }
/* H151: una orden de texto de la terminal; devuelve los bytes impresos o -TAEK_EPANTALLA / -TAEK_ENOPERMITIDA */
static inline long taek_orden(struct taek_orden *o) { return sys1(SYS_ORDEN, (long)o); }
/* H183: sockets TCP y TLS 1.3 (ver struct taek_red). Un nucleo anterior devuelve -TAEK_ENOSYS. */
static inline long taek_red(struct taek_red *r) { return sys1(SYS_RED, (long)r); }
/* H185: entrega un MP4 de la memoria del programa al nucleo (ver TAEK_VIDEO_MEM) */
static inline long taek_video(const void *datos, uint64_t bytes) { return sys5(SYS_VIDEO, (long)datos, (long)bytes, 0, 0, 0); }
/* H187: decodificacion en flujo (ver struct taek_flujo) */
static inline long taek_flujo(struct taek_flujo *f) { return sys1(SYS_FLUJO, (long)f); }
/* H213: hilos del programa y futex (ver usuario/hilos.h). Un nucleo anterior devuelve -TAEK_ENOSYS. */
static inline long taek_hilo(long op, long arg) { return sys5(SYS_HILO, op, arg, 0, 0, 0); }
static inline long taek_futex(long op, volatile uint32_t *dir, long valor, long ms) { return sys5(SYS_FUTEX, op, (long)dir, valor, ms, 0); }
static inline long taek_ceder(void) { return sys1(SYS_CEDER, 0); }

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *o, size_t n);

long principal(uint64_t a, uint64_t b);

#define TAEK_PROGRAMA \
    void *memset(void *d, int c, size_t n) { void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r; } \
    void *memcpy(void *d, const void *o, size_t n) { void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(o), "+c"(n) :: "memory"); return r; } \
    __attribute__((section(".text._start"))) void _start(uint64_t a, uint64_t b) { taek_salir(principal(a, b)); }

#endif
