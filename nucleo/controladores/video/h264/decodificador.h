#ifndef TAEK_H264_DECODIFICADOR_H
#define TAEK_H264_DECODIFICADOR_H
#include "cabac.h"

/* Índices espaciales internos: bloques 4x4 en orden raster. */
typedef struct {
    uint8_t tipo, salto, directo, transformada8, cbp, dc, modo_c, qp;
    int8_t filtro, alfa, beta;
    uint8_t modo[16], nz[24];
    uint16_t reconstruidos;
    int16_t slice;
    int8_t ref[2][16];
    int8_t ref_foto[2][16];
    uint16_t movimiento_listo[2], bloques_directos;
    int16_t mv[2][16][2];
    uint8_t mvd[2][16][2];
} h264_mb;

typedef struct {
    uint8_t *pixeles;
    h264_mb *mb;
    unsigned w, h, ancho_mb, alto_mb;
    int frame_num, poc, ref, larga, salida, ocupado;
    int64_t tiempo;
    unsigned crop_x, crop_y, ancho_visible, alto_visible;
    int rango_completo, matriz_color;
    uint64_t identificador, referencias_id[18];
} h264_foto;

typedef struct {
    unsigned primero, tipo, pps_id, frame_num, poc_lsb, idr, nal_ref;
    int delta_poc[2], qp, filtro, alfa, beta, previo_delta;
    unsigned refs[2], cabac_id, directo_espacial;
    unsigned reord_n[2], reord_tipo[2][64], reord_val[2][64];
    unsigned marcas_n, marcas_op[64], marcas_a[64], marcas_b[64];
    unsigned adaptativo, larga_idr, omitir_salida;
    int peso[2][32][3], offset[2][32][3], denom[3];
    h264_foto *lista[2][32];
} h264_slice;

struct h264_decodificador {
    h264_servicios servicios;
    h264_sps sps[H264_MAX_SPS];
    h264_pps pps[H264_MAX_PPS];
    h264_sps *s;
    h264_pps *p;
    h264_slice sl;
    uint8_t *rbsp;
    size_t capacidad_rbsp;
    h264_bits bits;
    h264_cabac cabac;
    h264_foto fotos[18], *actual;
    unsigned mb_actual, mb_completos;
    int slice_id, poc_msb_anterior, poc_lsb_anterior, frame_num_anterior, frame_offset;
    int fallo, max_larga;
    uint64_t siguiente_identificador;
    const char *error;
};

static const uint8_t h264_orden4[16] = {0,1,4,5,2,3,6,7,8,9,12,13,10,11,14,15};
static const uint8_t h264_scan4[16] = {0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
static const uint8_t h264_scan8[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

h264_mb *h264_vecino(h264_decodificador *d, int *x4, int *y4, int croma);
int h264_intra(h264_decodificador *d, unsigned intra_tipo);
int h264_residuo(h264_decodificador *d, int32_t niveles[64], int categoria, int bloque);
int h264_predecir(h264_decodificador *d, int plano, int bx, int by, int n, int modo);
void h264_transformar4(int32_t *c, uint8_t *dst, unsigned paso);
void h264_transformar8(int32_t *c, uint8_t *dst, unsigned paso);
int h264_qp_croma(int qp, int offset);
void h264_desbloquear(h264_decodificador *d);
int h264_inter(h264_decodificador *d, unsigned tipo, int salto);
int h264_preparar_referencias(h264_decodificador *d);
int h264_leer_mb_tipo(h264_decodificador *d);
int h264_leer_cbp(h264_decodificador *d);
int h264_leer_delta_qp(h264_decodificador *d);
int h264_reconstruir_residuo(h264_decodificador *d, int modo16);
#endif
