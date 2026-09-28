#ifndef TAEK_H264_INTER_KERNELS_H
#define TAEK_H264_INTER_KERNELS_H
#include <stdint.h>
/* SSE2 aislado, sin AVX ni estado YMM. El llamador garantiza XMM habilitado y
 * conservado por IRQ/contexto. No se asigna memoria ni se modifica la fuente.
 * luma: w=4/8/16 h=4/8/16; croma: w=2/4/8 h=2/4/8. Sin alineación requerida.
 * Lecturas exactas del ancho más taps -2/+3 (luma) o +1 (croma), sólo en ejes
 * fraccionarios. Origen ya extendido si hay borde. Salida contigua w*h bytes.
 * Los filtros intermedios horizontales son int16 (-2550..10710); el diagonal
 * acumula int32 sin saturar/recortar hasta el redondeo +512 y >>10.
 * Devuelven 0 si el tamaño no es apto; el llamador usa el kernel escalar. */
int h264_pred_sse2(const uint8_t *src,int paso,int w,int h,int dx,int dy,int croma,uint8_t *salida);
int h264_combinar_sse2(uint8_t *dst,int paso,int w,int h,const uint8_t *a,const uint8_t *b,
                       int modo,int peso0,int peso1,int den,int offset);
#endif
