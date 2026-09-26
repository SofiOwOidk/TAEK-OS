#ifndef TAEK_H264_INTERNO_H
#define TAEK_H264_INTERNO_H
#include "h264.h"

#define H264_MAX_SPS 32
#define H264_MAX_PPS 256
#define H264_MAX_REFS 16
#define H264_MAX_DIMENSION 4096
#define H264_MAX_NAL (16u * 1024u * 1024u)

static inline int h264_recortar(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static inline int h264_abs(int x) { return x < 0 ? -x : x; }
static inline void h264_cero(void *p, size_t n) {
    uint8_t *q = p;
    while (n--) *q++ = 0;
}
static inline void h264_copiar(void *p, const void *s, size_t n) {
    uint8_t *q = p;
    const uint8_t *r = s;
    while (n--) *q++ = *r++;
}
typedef struct {
    const uint8_t *datos;
    size_t bytes, posicion;
    int error;
} h264_bits;
uint32_t h264_bits_leer(h264_bits *b, unsigned n);
uint32_t h264_ue(h264_bits *b);
int32_t h264_se(h264_bits *b);
int h264_mas_rbsp(const h264_bits *b);
int h264_fin_rbsp(h264_bits *b);

typedef struct {
    int valido;
    unsigned perfil, nivel, id, log_frame, poc_tipo, log_poc;
    unsigned refs, ancho_mb, alto_mb, frame_solo, directo8;
    unsigned crop_izq, crop_der, crop_arriba, crop_abajo;
    unsigned ancho, alto, reordenar;
    int rango_completo, matriz_color;
    int delta_poc_cero, offset_no_ref, offset_arriba_abajo;
    unsigned ciclo_poc;
    int offsets_poc[256];
    uint32_t num_tick, escala_tick;
} h264_sps;
typedef struct {
    int valido;
    unsigned id, sps, cabac, poc_abajo, ref_def[2], ponderado, bipred;
    int qp_inicial, qs_inicial, qp_c[2];
    unsigned deblock_presente, intra_restringida, redundante, transformada8;
} h264_pps;

h264_resultado h264_leer_sps(h264_bits *b, h264_sps *s);
h264_resultado h264_leer_pps(h264_bits *b, h264_pps *p);

#endif
