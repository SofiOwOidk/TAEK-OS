#ifndef TAEK_H264_CABAC_H
#define TAEK_H264_CABAC_H
#include "interno.h"
typedef struct {
    h264_bits *bits;
    unsigned rango, valor;
    uint8_t contexto[460];
} h264_cabac;
int h264_cabac_iniciar(h264_cabac *c, h264_bits *b, int qp, unsigned modo);
unsigned h264_cabac_bin(h264_cabac *c, unsigned ctx);
unsigned h264_cabac_bypass(h264_cabac *c);
unsigned h264_cabac_terminar(h264_cabac *c);
#endif
