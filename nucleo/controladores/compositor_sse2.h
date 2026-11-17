#ifndef CONTROLADORES_COMPOSITOR_SSE2_H
#define CONTROLADORES_COMPOSITOR_SSE2_H
#include <stdint.h>
void comp_mezclar_sse2(uint32_t *d, uint32_t pd, const uint32_t *o, uint32_t po, uint32_t an, uint32_t al);
#endif
