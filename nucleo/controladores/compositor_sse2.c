/* Mezcla de la capa CPU con SSE2. Solo se llama dentro de una llamada al sistema: la trampa ya guardo el estado SSE del programa
 * y las interrupciones estan apagadas. Misma cuenta exacta que la version escalar: d = o + d * (255 - a) / 255 con redondeo. */
#include <emmintrin.h>
#include <stdint.h>
#include "compositor_sse2.h"

static inline uint32_t una(uint32_t d, uint32_t o) {
    uint32_t a = o >> 24, i = 255 - a;
    uint32_t rb = (d & 0x00FF00FFu) * i + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    uint32_t g = (d & 0x0000FF00u) * i + 0x00008000u;
    g = ((g + ((g >> 8) & 0x0000FF00u)) >> 8) & 0x0000FF00u;
    return (rb + g + (o & 0x00FFFFFFu)) & 0x00FFFFFFu;
}

void comp_mezclar_sse2(uint32_t *d, uint32_t pd, const uint32_t *o, uint32_t po, uint32_t an, uint32_t al) {
    const __m128i cero = _mm_setzero_si128(), r128 = _mm_set1_epi16(128), max = _mm_set1_epi16(255);
    const __m128i rgb = _mm_set1_epi32(0x00FFFFFF), alfa = _mm_set1_epi32((int)0xFF000000u);
    for (uint32_t y = 0; y < al; y++, d += pd, o += po) {
        uint32_t x = 0;
        for (; x + 4 <= an; x += 4) {
            __m128i s = _mm_loadu_si128((const __m128i *)(const void *)(o + x));
            __m128i a = _mm_and_si128(s, alfa);
            int m = _mm_movemask_epi8(_mm_cmpeq_epi32(a, cero));
            if (m == 0xFFFF && _mm_movemask_epi8(_mm_cmpeq_epi32(s, cero)) == 0xFFFF) continue;  /* Todo transparente */
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(a, alfa)) == 0xFFFF) {                        /* Todo opaco */
                _mm_storeu_si128((__m128i *)(void *)(d + x), _mm_and_si128(s, rgb));
                continue;
            }
            /* 255 - a en los cuatro canales de cada pixel */
            __m128i al32 = _mm_srli_epi32(s, 24);
            __m128i a4 = _mm_or_si128(al32, _mm_slli_epi32(al32, 16));
            __m128i iba = _mm_sub_epi16(max, _mm_shufflelo_epi16(_mm_shufflehi_epi16(a4, 0xA0), 0xA0));
            __m128i dv = _mm_loadu_si128((const __m128i *)(const void *)(d + x));
            __m128i ib = _mm_unpacklo_epi16(iba, iba), ih = _mm_unpackhi_epi16(iba, iba);
            __m128i bajo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(dv, cero), ib), r128);
            __m128i alto = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(dv, cero), ih), r128);
            bajo = _mm_srli_epi16(_mm_add_epi16(bajo, _mm_srli_epi16(bajo, 8)), 8);
            alto = _mm_srli_epi16(_mm_add_epi16(alto, _mm_srli_epi16(alto, 8)), 8);
            __m128i r = _mm_add_epi32(_mm_packus_epi16(bajo, alto), _mm_and_si128(s, rgb));
            _mm_storeu_si128((__m128i *)(void *)(d + x), _mm_and_si128(r, rgb));
        }
        for (; x < an; x++) d[x] = una(d[x], o[x]);
    }
}
