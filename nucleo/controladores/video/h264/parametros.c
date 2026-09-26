#include "interno.h"

/* Sintaxis 7.3.2 y semántica 7.4.2 de ITU-T H.264. */
static void leer_hrd(h264_bits *b) {
    unsigned n = h264_ue(b);
    if (n > 31) { b->error = 1; return; }
    h264_bits_leer(b, 8);
    for (unsigned i = 0; i <= n; i++) {
        h264_ue(b); h264_ue(b); h264_bits_leer(b, 1);
    }
    h264_bits_leer(b, 20);
}

static void leer_vui(h264_bits *b, h264_sps *s) {
    if (h264_bits_leer(b, 1)) {
        if (h264_bits_leer(b, 8) == 255) h264_bits_leer(b, 32);
    }
    if (h264_bits_leer(b, 1)) h264_bits_leer(b, 1);
    if (h264_bits_leer(b, 1)) {
        h264_bits_leer(b, 3);
        s->rango_completo = (int)h264_bits_leer(b, 1);
        if (h264_bits_leer(b, 1)) {
            h264_bits_leer(b, 16);
            s->matriz_color = (int)h264_bits_leer(b, 8);
        }
    }
    if (h264_bits_leer(b, 1)) { h264_ue(b); h264_ue(b); }
    if (h264_bits_leer(b, 1)) {
        s->num_tick = h264_bits_leer(b, 32);
        s->escala_tick = h264_bits_leer(b, 32);
        h264_bits_leer(b, 1);
        if (!s->num_tick || !s->escala_tick) b->error = 1;
    }
    unsigned nal_hrd = h264_bits_leer(b, 1);
    if (nal_hrd) leer_hrd(b);
    unsigned vcl_hrd = h264_bits_leer(b, 1);
    if (vcl_hrd) leer_hrd(b);
    if (nal_hrd || vcl_hrd) h264_bits_leer(b, 1);
    h264_bits_leer(b, 1);
    if (h264_bits_leer(b, 1)) {
        h264_bits_leer(b, 1);
        h264_ue(b); h264_ue(b); h264_ue(b); h264_ue(b);
        s->reordenar = h264_ue(b);
        unsigned dpb = h264_ue(b);
        if (dpb > H264_MAX_REFS || s->reordenar > dpb) b->error = 1;
    }
}

h264_resultado h264_leer_sps(h264_bits *b, h264_sps *s) {
    h264_cero(s, sizeof(*s));
    s->perfil = h264_bits_leer(b, 8);
    unsigned restricciones = h264_bits_leer(b, 8);
    if (restricciones & 3) return H264_DATOS_INVALIDOS;
    s->nivel = h264_bits_leer(b, 8);
    s->id = h264_ue(b);
    s->matriz_color = 2;
    s->reordenar = H264_MAX_REFS;
    if (s->id >= H264_MAX_SPS) return H264_DATOS_INVALIDOS;
    if (s->perfil == 100 || s->perfil == 110 || s->perfil == 122 ||
        s->perfil == 244 || s->perfil == 44 || s->perfil == 83 ||
        s->perfil == 86 || s->perfil == 118 || s->perfil == 128 ||
        s->perfil == 138 || s->perfil == 139 || s->perfil == 134 ||
        s->perfil == 135) {
        unsigned croma = h264_ue(b);
        if (croma != 1) return H264_NO_SOPORTADO;
        unsigned profundidad_y = h264_ue(b), profundidad_c = h264_ue(b);
        unsigned bypass = h264_bits_leer(b, 1);
        if (profundidad_y || profundidad_c || bypass) return H264_NO_SOPORTADO;
        if (h264_bits_leer(b, 1)) return H264_NO_SOPORTADO;
    } else if (s->perfil != 66 && s->perfil != 77 && s->perfil != 88) {
        return H264_NO_SOPORTADO;
    }
    unsigned log_frame = h264_ue(b);
    if (log_frame > 12) return H264_DATOS_INVALIDOS;
    s->log_frame = log_frame + 4;
    s->poc_tipo = h264_ue(b);
    if (s->poc_tipo == 0) {
        unsigned log_poc = h264_ue(b);
        if (log_poc > 12) return H264_DATOS_INVALIDOS;
        s->log_poc = log_poc + 4;
    } else if (s->poc_tipo == 1) {
        s->delta_poc_cero = (int)h264_bits_leer(b, 1);
        s->offset_no_ref = h264_se(b);
        s->offset_arriba_abajo = h264_se(b);
        s->ciclo_poc = h264_ue(b);
        if (s->ciclo_poc > 255) return H264_DATOS_INVALIDOS;
        for (unsigned i = 0; i < s->ciclo_poc; i++) s->offsets_poc[i] = h264_se(b);
    } else if (s->poc_tipo != 2) return H264_DATOS_INVALIDOS;
    s->refs = h264_ue(b);
    if (s->refs > H264_MAX_REFS) return H264_NO_SOPORTADO;
    if (h264_bits_leer(b, 1)) return H264_NO_SOPORTADO; /* huecos frame_num */
    unsigned w = h264_ue(b), h = h264_ue(b);
    if (w >= H264_MAX_DIMENSION / 16 || h >= H264_MAX_DIMENSION / 16)
        return H264_LIMITE_EXCEDIDO;
    s->ancho_mb = w + 1;
    s->alto_mb = h + 1;
    s->frame_solo = h264_bits_leer(b, 1);
    if (!s->frame_solo) return H264_NO_SOPORTADO;
    s->directo8 = h264_bits_leer(b, 1);
    if (h264_bits_leer(b, 1)) {
        s->crop_izq = h264_ue(b); s->crop_der = h264_ue(b);
        s->crop_arriba = h264_ue(b); s->crop_abajo = h264_ue(b);
    }
    if ((uint64_t)s->crop_izq + s->crop_der >= s->ancho_mb * 8u ||
        (uint64_t)s->crop_arriba + s->crop_abajo >= s->alto_mb * 8u)
        return H264_DATOS_INVALIDOS;
    s->ancho = s->ancho_mb * 16 - 2 * (s->crop_izq + s->crop_der);
    s->alto = s->alto_mb * 16 - 2 * (s->crop_arriba + s->crop_abajo);
    if (h264_bits_leer(b, 1)) leer_vui(b, s);
    if (!h264_fin_rbsp(b)) return H264_DATOS_INVALIDOS;
    s->valido = 1;
    return H264_OK;
}

h264_resultado h264_leer_pps(h264_bits *b, h264_pps *p) {
    h264_cero(p, sizeof(*p));
    p->id = h264_ue(b); p->sps = h264_ue(b);
    if (p->id >= H264_MAX_PPS || p->sps >= H264_MAX_SPS) return H264_DATOS_INVALIDOS;
    p->cabac = h264_bits_leer(b, 1);
    p->poc_abajo = h264_bits_leer(b, 1);
    if (h264_ue(b)) return H264_NO_SOPORTADO; /* FMO */
    unsigned ref0 = h264_ue(b), ref1 = h264_ue(b);
    if (ref0 >= 32 || ref1 >= 32) return H264_DATOS_INVALIDOS;
    p->ref_def[0] = ref0 + 1; p->ref_def[1] = ref1 + 1;
    p->ponderado = h264_bits_leer(b, 1);
    p->bipred = h264_bits_leer(b, 2);
    int qp = h264_se(b), qs = h264_se(b);
    if (qp < -26 || qp > 25 || qs < -26 || qs > 25 || p->bipred > 2)
        return H264_DATOS_INVALIDOS;
    p->qp_inicial = qp + 26; p->qs_inicial = qs + 26;
    p->qp_c[0] = h264_se(b);
    p->deblock_presente = h264_bits_leer(b, 1);
    p->intra_restringida = h264_bits_leer(b, 1);
    p->redundante = h264_bits_leer(b, 1);
    p->qp_c[1] = p->qp_c[0];
    if (h264_mas_rbsp(b)) {
        p->transformada8 = h264_bits_leer(b, 1);
        if (h264_bits_leer(b, 1)) return H264_NO_SOPORTADO;
        p->qp_c[1] = h264_se(b);
    }
    if (p->qp_c[0] < -12 || p->qp_c[0] > 12 || p->qp_c[1] < -12 || p->qp_c[1] > 12)
        return H264_DATOS_INVALIDOS;
    if (!h264_fin_rbsp(b)) return H264_DATOS_INVALIDOS;
    p->valido = 1;
    return H264_OK;
}
