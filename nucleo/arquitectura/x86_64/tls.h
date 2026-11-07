#ifndef TAEK_X86_64_TLS_H
#define TAEK_X86_64_TLS_H
#include <stddef.h>
#include <stdint.h>

/* TLS estatico del ELF enlazado: variante II, TP en FS, self en FS:0.
 * No es el TCB de glibc ni acredita TLS de modulos cargados dinamicamente. */
int taek_tls_cpu_iniciar(unsigned cpu);
void *taek_tls_crear(void);
int taek_tls_destruir(void *tp);
/* Backing contabilizado por un propietario. Un liberar fallido conserva el
 * TCB valido y permite reintento. El llamador mantiene los mismos callbacks. */
void *taek_tls_crear_con(void *(*asignar)(size_t));
int taek_tls_destruir_con(void *tp,int (*liberar)(void *));
uintptr_t taek_tls_base_actual(void);
size_t taek_tls_imagen_bytes(void);
#endif
