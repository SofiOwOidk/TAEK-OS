#ifndef H264_ETAPAS_SSE2_H
#define H264_ETAPAS_SSE2_H
#include "h264.h"
/* SSE2, ninguna alineación requerida. Transformadas usan int32 y conservan
 * todos los desplazamientos aritméticos; suma saturada sólo al final.
 * Filtro: 2 (croma) o 4 (luma) muestras, márgenes p3..q3 ya válidos.
 * RGB: cuatro píxeles, lecturas individuales dentro de la imagen visible. */
void h264_transformada_sse2(const int32_t *,int32_t *,unsigned);
void h264_sumar_sse2(const int32_t *,uint8_t *,unsigned,unsigned);
void h264_hadamard_sse2(int32_t *);
void h264_filtro_sse2(uint8_t *,int,int,int,int,int,int,int);
void h264_rgb_sse2(const h264_imagen *,uint32_t *,unsigned,unsigned,unsigned,unsigned);
#endif
