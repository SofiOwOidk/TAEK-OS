#ifndef TAEK_AAC_MEMORIA_H
#define TAEK_AAC_MEMORIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void aac_fijar_asignador(void *(*asignar)(size_t bytes), void (*liberar)(void *ptr));
void *helix_malloc(int size);
void helix_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
