/* Frontera entre Media Driver (C++, con SSE, enlazado aparte) y el resto del shim i915 (C de kernel, sin SSE).
 * Media Driver no ve simbolos de Linux ni al reves: solo estos nombres cruzan. */
#ifndef TAEK_MEDIA_H
#define TAEK_MEDIA_H
#ifndef __KERNEL__
#include <stddef.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* Servicios del kernel (media_frontera.c). */
void *taek_media_alloc(size_t bytes);
void taek_media_free(void *p);
void taek_media_log(const char *linea);
unsigned long long taek_media_ns(void);
void taek_media_dormir_us(unsigned long long us);
void *taek_media_yo(void);
void taek_media_trampa(const char *simbolo) __attribute__((noreturn));
int taek_media_abrir_drm(void);
int taek_media_cerrar_drm(int fd);
int taek_media_ioctl(int fd, unsigned long orden, void *arg);
void *taek_media_mmap(void *dir, size_t bytes, int prot, int flags, int fd, long off);
int taek_media_munmap(void *dir, size_t bytes);

/* Ordenes de taek_media_entrada: se ejecuta en la pila grande de la frontera. Con TAEK_MEDIA_OP, texto es una taek_video_peticion. */
enum { TAEK_MEDIA_RT = 1, TAEK_MEDIA_INIT, TAEK_MEDIA_CAPS, TAEK_MEDIA_IDR, TAEK_MEDIA_MAL, TAEK_MEDIA_VIDEO, TAEK_MEDIA_OP };
int taek_media_entrada(int orden, char *texto, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
