// Parte cliente de la decodificacion H.264 por VA-API: lee el flujo, mantiene el DPB y llama al driver real. Lo mismo que hace
// un reproductor sobre libva. Los pixeles los produce la GPU y salen en orden de presentacion hacia quien los pida.
// Alcance: progresivo, 4:2:0 de 8 bits, POC tipo 0 y 2, sin FMO, ASO ni SP/SI. Lo que quede fuera se devuelve como error con nombre.
#include <va/va.h>
#include <va/va_backend.h>
#include <smmintrin.h>
#include <cpuid.h>
#include "taek_media.h"
#include "taek_video_abi.h"

extern "C" {
int snprintf(char *, size_t, const char *, ...);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
int printf(const char *, ...);
}

namespace {
const unsigned MAX_DPB = 16, NUM_SUP = MAX_DPB + 3, MAX_NAL = 512, MAX_LADO = 4096;
const VASurfaceID SIN_SUP = VA_INVALID_SURFACE;

char motivo_txt[96];
#define FALLA(rc_, ...) do { snprintf(motivo_txt, sizeof(motivo_txt), __VA_ARGS__); return (rc_); } while (0)
#define VA(x) do { VAStatus st_ = (x); if (st_ != VA_STATUS_SUCCESS) FALLA(TAEK_VIDEO_FALLO, "%s = %d", #x, st_); } while (0)

struct Bits {
    const uint8_t *d; size_t n, p;
    unsigned u1() { if (p >= n * 8) { malo = 1; return 0; } unsigned b = (d[p >> 3] >> (7 - (p & 7))) & 1; p++; return b; }
    unsigned u(unsigned k) { unsigned v = 0; while (k--) v = v * 2 + u1(); return v; }
    unsigned ue() { unsigned z = 0; while (!u1() && z < 32 && !malo) z++; return z >= 32 ? 0xffffffffu : (1u << z) - 1 + u(z); }
    int se() { unsigned k = ue(); return (k & 1) ? (int)((k + 1) / 2) : -(int)(k / 2); }
    int malo = 0;
};

struct Sps {
    bool ok; unsigned profile, nivel, restr, chroma, bd_l, bd_c, log2_fn, poc_tipo, log2_poc, refs, gaps, w_mbs, h_mbs, frame_only, d8x8, reordenar;
    unsigned recorte_x, recorte_y, ancho, alto; int rango_completo, matriz_color;
    bool escala; uint8_t l4[6][16], l8[6][64];
};
struct Pps {
    bool ok; unsigned sps, cabac, orden_inf, l0, l1, wp, wb, cip, deb, red, t8; int qp, qs, crq, crq2;
    bool escala; uint8_t l4[6][16], l8[6][64];
};
Sps sps[32];
Pps pps[256];

const uint8_t ZZ4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
const uint8_t ZZ8[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                         35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
const uint8_t D4I[16] = {6, 13, 13, 16, 16, 16, 18, 18, 18, 18, 18, 21, 21, 21, 21, 23}, D4P[16] = {10, 14, 14, 16, 16, 16, 16, 16, 16, 16, 18, 18, 18, 18, 20, 20};
const uint8_t D8I[64] = {6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
                         27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42};
const uint8_t D8P[64] = {9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
                         24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35};

// Listas en orden de exploracion; devuelve si uso la lista por defecto.
void leer_lista(Bits &b, uint8_t *o, unsigned n, bool &defecto) {
    int ult = 8, sig = 8; defecto = false;
    for (unsigned j = 0; j < n; j++) {
        if (sig) { sig = (ult + b.se() + 256) % 256; if (j == 0 && !sig) { defecto = true; return; } }
        o[j] = sig ? sig : ult; ult = o[j];
    }
}
// Reglas A (SPS) y B (PPS) de la tabla 7-2: fuera = lo que hereda cada lista ausente.
void leer_escala(Bits &b, uint8_t l4[6][16], uint8_t l8[6][64], unsigned n8, const uint8_t (*f4)[16], const uint8_t (*f8)[64]) {
    for (unsigned i = 0; i < 6 + n8; i++) {
        bool pres = b.u1(), def = false;
        if (i < 6) {
            if (pres) { leer_lista(b, l4[i], 16, def); if (def) memcpy(l4[i], i < 3 ? D4I : D4P, 16); }
            else if (i == 0 || i == 3) memcpy(l4[i], f4 ? f4[i] : (i == 0 ? D4I : D4P), 16);
            else memcpy(l4[i], l4[i - 1], 16);
        } else {
            unsigned k = i - 6;
            if (pres) { leer_lista(b, l8[k], 64, def); if (def) memcpy(l8[k], k % 2 ? D8P : D8I, 64); }
            else if (k < 2) memcpy(l8[k], f8 ? f8[k] : (k ? D8P : D8I), 64);
            else memcpy(l8[k], l8[k - 2], 64);
        }
    }
}

// Tabla A-1: cuantas imagenes caben en el DPB. Sin VUI es lo que hay que suponer de reordenamiento (7.4.1 de E.2.1).
unsigned dpb_por_nivel(const Sps &s) {
    unsigned mbs = 0, n = s.nivel;
    if (n == 11 && (s.restr & 0x10) && s.profile <= 88) mbs = 396;   // Nivel 1b.
    else if (n <= 10) mbs = 396;
    else if (n == 11) mbs = 900;
    else if (n <= 20) mbs = 2376;
    else if (n == 21) mbs = 4752;
    else if (n <= 30) mbs = 8100;
    else if (n == 31) mbs = 18000;
    else if (n == 32) mbs = 20480;
    else if (n <= 41) mbs = 32768;
    else if (n == 42) mbs = 34816;
    else if (n == 50) mbs = 110400;
    else mbs = 184320;
    unsigned f = mbs / (s.w_mbs * s.h_mbs);
    if (f > MAX_DPB) f = MAX_DPB;
    if (f < s.refs) f = s.refs;
    return f;
}

void saltar_hrd(Bits &b) {
    unsigned n = b.ue();
    if (n > 31) { b.malo = 1; return; }
    b.u(8);
    for (unsigned i = 0; i <= n; i++) { b.ue(); b.ue(); b.u1(); }
    b.u(20);
}
void leer_vui(Bits &b, Sps &s) {
    if (b.u1()) { if (b.u(8) == 255) b.u(32); }
    if (b.u1()) b.u1();
    if (b.u1()) {
        b.u(3);
        s.rango_completo = (int)b.u1();
        if (b.u1()) { b.u(16); s.matriz_color = (int)b.u(8); }
    }
    if (b.u1()) { b.ue(); b.ue(); }
    if (b.u1()) { b.u(32); b.u(32); b.u1(); }
    unsigned nal = b.u1();
    if (nal) saltar_hrd(b);
    unsigned vcl = b.u1();
    if (vcl) saltar_hrd(b);
    if (nal || vcl) b.u1();
    b.u1();
    if (b.u1()) {
        b.u1(); b.ue(); b.ue(); b.ue(); b.ue();
        unsigned r = b.ue(), d = b.ue();
        if (!b.malo && d <= MAX_DPB && r <= d) s.reordenar = r;
    }
}

// Devuelve el motivo si el SPS queda fuera de alcance; nullptr si vale.
const char *leer_sps(Bits &b) {
    unsigned perfil = b.u(8), restr = b.u(8), nivel = b.u(8);
    unsigned id = b.ue();
    if (id >= 32) return "SPS invalido";
    static Sps n;
    memset(&n, 0, sizeof(n));
    n.profile = perfil; n.restr = restr; n.nivel = nivel; n.chroma = 1; n.matriz_color = 2;
    if (perfil == 100 || perfil == 110 || perfil == 122 || perfil == 244 || perfil == 44 || perfil == 83 || perfil == 86 || perfil == 118 || perfil == 128) {
        n.chroma = b.ue();
        if (n.chroma != 1) return "croma distinto de 4:2:0";
        n.bd_l = b.ue(); n.bd_c = b.ue();
        if (n.bd_l || n.bd_c) return "mas de 8 bits";
        if (b.u1()) return "transformada sin perdida";
        if (b.u1()) { n.escala = true; leer_escala(b, n.l4, n.l8, 2, nullptr, nullptr); }
    } else if (perfil != 66 && perfil != 77) return "perfil no admitido";
    n.log2_fn = b.ue() + 4;
    n.poc_tipo = b.ue();
    if (n.poc_tipo == 0) n.log2_poc = b.ue() + 4;
    else if (n.poc_tipo == 1) return "POC tipo 1";
    else if (n.poc_tipo != 2) return "SPS invalido";
    n.refs = b.ue(); n.gaps = b.u1();
    if (n.refs > MAX_DPB || n.log2_fn > 16 || n.log2_poc > 16) return "SPS invalido";
    n.w_mbs = b.ue() + 1; n.h_mbs = b.ue() + 1;
    n.frame_only = b.u1();
    if (!n.frame_only) return "video entrelazado";
    n.d8x8 = b.u1();
    if (b.malo || !n.w_mbs || !n.h_mbs) return "SPS invalido";
    if (n.w_mbs > MAX_LADO / 16 || n.h_mbs > MAX_LADO / 16) return "resolucion mayor de 4096";
    unsigned c_i = 0, c_d = 0, c_a = 0, c_b = 0;
    if (b.u1()) { c_i = b.ue(); c_d = b.ue(); c_a = b.ue(); c_b = b.ue(); }
    if (b.malo || (uint64_t)c_i + c_d >= n.w_mbs * 8u || (uint64_t)c_a + c_b >= n.h_mbs * 8u) return "SPS invalido";
    n.recorte_x = c_i * 2; n.recorte_y = c_a * 2;
    n.ancho = n.w_mbs * 16 - 2 * (c_i + c_d); n.alto = n.h_mbs * 16 - 2 * (c_a + c_b);
    n.reordenar = n.poc_tipo == 2 ? 0 : dpb_por_nivel(n);
    if (b.u1()) {
        unsigned reord = n.reordenar; int rc = n.rango_completo, mc = n.matriz_color;
        leer_vui(b, n);
        // Un VUI roto no invalida la imagen: se queda con lo calculado.
        if (b.malo) { n.reordenar = reord; n.rango_completo = rc; n.matriz_color = mc; }
    }
    n.ok = true;
    sps[id] = n;
    return nullptr;
}
const char *leer_pps(Bits &b) {
    unsigned id = b.ue(), sid = b.ue();
    if (id >= 256 || sid >= 32) return "PPS invalido";
    static Pps n;
    memset(&n, 0, sizeof(n));
    n.sps = sid; n.cabac = b.u1(); n.orden_inf = b.u1();
    if (b.ue()) return "varios grupos de cortes (FMO)";
    n.l0 = b.ue() + 1; n.l1 = b.ue() + 1; n.wp = b.u1(); n.wb = b.u(2);
    n.qp = b.se() + 26; n.qs = b.se() + 26; n.crq = b.se(); n.deb = b.u1(); n.cip = b.u1(); n.red = b.u1();
    n.crq2 = n.crq;
    size_t resto = b.p;
    // more_rbsp_data: queda algo antes del bit de parada.
    size_t ultimo = b.n * 8;
    while (ultimo && !((b.d[(ultimo - 1) >> 3] >> (7 - ((ultimo - 1) & 7))) & 1)) ultimo--;
    if (resto + 1 < ultimo) {
        n.t8 = b.u1();
        if (b.u1()) {
            n.escala = true;
            const Sps &s = sps[sid];
            leer_escala(b, n.l4, n.l8, n.t8 ? 2 : 0, s.escala ? s.l4 : nullptr, s.escala ? s.l8 : nullptr);
        }
        n.crq2 = b.se();
    }
    if (b.malo || n.l0 > 32 || n.l1 > 32 || n.wb > 2) return "PPS invalido";
    n.ok = true;
    pps[id] = n;
    return nullptr;
}

struct Ref { VASurfaceID sup; unsigned frame_num; int poc, poc_t, poc_b; int pic_num, lt_idx; bool corto, largo; int fn_wrap; };
Ref dpb[MAX_DPB];
unsigned n_dpb;
VASurfaceID sups[NUM_SUP];
int poc_msb_prev, poc_lsb_prev;
unsigned frame_num_prev, offset_fn_prev, fn_ref_prev;

// Salida en orden de presentacion: como el DPB de C.4 pero sin copiar nada hasta que se pide.
struct Pendiente { VASurfaceID sup; int poc; long long pts; unsigned periodo; bool usada; };
Pendiente pendientes[NUM_SUP];
unsigned periodo_actual, reordenar_actual;
bool forzar_salida, drenando;

bool en_dpb(VASurfaceID s) { for (unsigned i = 0; i < n_dpb; i++) if (dpb[i].sup == s) return true; return false; }
bool pendiente(VASurfaceID s) { for (unsigned i = 0; i < NUM_SUP; i++) if (pendientes[i].usada && pendientes[i].sup == s) return true; return false; }
VASurfaceID sup_libre() { for (unsigned i = 0; i < NUM_SUP; i++) if (!en_dpb(sups[i]) && !pendiente(sups[i])) return sups[i]; return SIN_SUP; }
unsigned cuantos_pendientes() { unsigned n = 0; for (unsigned i = 0; i < NUM_SUP; i++) n += pendientes[i].usada; return n; }
unsigned calc_listos() {
    unsigned viejos = 0, actuales = 0;
    for (unsigned i = 0; i < NUM_SUP; i++) if (pendientes[i].usada) { if (pendientes[i].periodo < periodo_actual) viejos++; else actuales++; }
    if (drenando) return viejos + actuales;
    unsigned r = viejos + (actuales > reordenar_actual ? actuales - reordenar_actual : 0);
    if (!r && forzar_salida && viejos + actuales) r = 1;
    return r;
}
Pendiente *siguiente_salida() {
    Pendiente *m = nullptr;
    for (unsigned i = 0; i < NUM_SUP; i++) {
        Pendiente *p = &pendientes[i];
        if (p->usada && (!m || p->periodo < m->periodo || (p->periodo == m->periodo && p->poc < m->poc))) m = p;
    }
    return m;
}

void a_va(VAPictureH264 &o, const Ref &r) {
    memset(&o, 0, sizeof(o));
    o.picture_id = r.sup; o.frame_idx = r.largo ? r.lt_idx : r.frame_num;
    o.flags = r.largo ? VA_PICTURE_H264_LONG_TERM_REFERENCE : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    o.TopFieldOrderCnt = r.poc_t; o.BottomFieldOrderCnt = r.poc_b;
}
void va_vacia(VAPictureH264 &o) { memset(&o, 0, sizeof(o)); o.picture_id = VA_INVALID_SURFACE; o.flags = VA_PICTURE_H264_INVALID; }

void ordenar(const Ref **v, unsigned n, bool (*antes)(const Ref *, const Ref *)) {
    for (unsigned i = 1; i < n; i++) { const Ref *x = v[i]; unsigned j = i; while (j && antes(x, v[j - 1])) { v[j] = v[j - 1]; j--; } v[j] = x; }
}
bool pn_desc(const Ref *a, const Ref *b) { return a->pic_num > b->pic_num; }
bool lt_asc(const Ref *a, const Ref *b) { return a->lt_idx < b->lt_idx; }
bool poc_desc(const Ref *a, const Ref *b) { return a->poc > b->poc; }
bool poc_asc(const Ref *a, const Ref *b) { return a->poc < b->poc; }

struct Corte {
    unsigned tipo, nal_ref, nal_tipo, primero, frame_num, idr_id, poc_lsb, l0, l1, directo, cabac_idc, deb_idc, redundante;
    int delta_bot, qp_delta, alfa, beta;
    VASliceParameterBufferH264 va;
    const Ref *lista[2][33];
    bool sin_salida_previa; unsigned adaptativo, largo_idr;
    struct { unsigned op, a, b; } mmco[66]; unsigned n_mmco;
    struct { unsigned idc, v; } rplm[2][66]; unsigned n_rplm[2];
};
Corte c;

int calcular_poc(const Sps &s, const Corte &k, int &top, int &bot) {
    if (s.poc_tipo == 0) {
        int max = 1 << s.log2_poc, msb;
        if (k.nal_tipo == 5) { poc_msb_prev = 0; poc_lsb_prev = 0; }
        int lsb = (int)k.poc_lsb;
        if (lsb < poc_lsb_prev && poc_lsb_prev - lsb >= max / 2) msb = poc_msb_prev + max;
        else if (lsb > poc_lsb_prev && lsb - poc_lsb_prev > max / 2) msb = poc_msb_prev - max;
        else msb = poc_msb_prev;
        if (k.nal_ref) { poc_msb_prev = msb; poc_lsb_prev = lsb; }
        top = msb + lsb; bot = top + k.delta_bot;
        return top < bot ? top : bot;
    }
    // Tipo 2: el orden de salida es el de decodificacion.
    unsigned max_fn = 1u << s.log2_fn;
    unsigned off = k.nal_tipo == 5 ? 0 : (frame_num_prev > k.frame_num ? offset_fn_prev + max_fn : offset_fn_prev);
    offset_fn_prev = off;
    top = bot = k.nal_tipo == 5 ? 0 : (int)(2 * (off + k.frame_num)) - (k.nal_ref ? 0 : 1);
    return top;
}

// Marcado de referencias (8.2.5). Devuelve si la imagen llevaba una operacion 5, que actua como un IDR para lo que venga detras.
int marcar(const Sps &s, const Corte &k, VASurfaceID cur, int poc, int top, int bot, bool &mmco5) {
    mmco5 = false;
    if (!k.nal_ref) return 0;
    bool largo_cur = false; int lt_cur = 0;
    if (k.nal_tipo == 5) { n_dpb = 0; if (k.largo_idr) { largo_cur = true; lt_cur = 0; } }
    else if (k.adaptativo) {
        int cur_pn = (int)k.frame_num;
        for (unsigned i = 0; i < k.n_mmco; i++) {
            unsigned op = k.mmco[i].op, a = k.mmco[i].a, bb = k.mmco[i].b;
            if (op == 1 || op == 3) {
                int pn = cur_pn - (int)(a + 1);
                for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].corto && dpb[j].pic_num == pn) {
                    if (op == 1) dpb[j].corto = false;
                    else { for (unsigned t = 0; t < n_dpb; t++) if (dpb[t].largo && dpb[t].lt_idx == (int)bb) dpb[t].largo = false; dpb[j].corto = false; dpb[j].largo = true; dpb[j].lt_idx = bb; }
                }
            } else if (op == 2) { for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].largo && dpb[j].lt_idx == (int)a) dpb[j].largo = false; }
            else if (op == 4) { for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].largo && dpb[j].lt_idx >= (int)a) dpb[j].largo = false; }
            else if (op == 5) { for (unsigned j = 0; j < n_dpb; j++) dpb[j].corto = dpb[j].largo = false; mmco5 = true; }
            else if (op == 6) { for (unsigned t = 0; t < n_dpb; t++) if (dpb[t].largo && dpb[t].lt_idx == (int)a) dpb[t].largo = false; largo_cur = true; lt_cur = a; }
        }
    } else {
        unsigned vivos = 0; Ref *viejo = nullptr;
        for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].corto || dpb[j].largo) vivos++;
        unsigned lim = s.refs ? s.refs : 1;
        if (vivos >= lim) {
            for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].corto && (!viejo || dpb[j].fn_wrap < viejo->fn_wrap)) viejo = &dpb[j];
            if (viejo) viejo->corto = false;
        }
    }
    unsigned w = 0;
    for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].corto || dpb[j].largo) dpb[w++] = dpb[j];
    n_dpb = w;
    if (n_dpb >= MAX_DPB) FALLA(TAEK_VIDEO_FALLO, "DPB lleno");
    int t_final = mmco5 ? top - poc : top, b_final = mmco5 ? bot - poc : bot;
    dpb[n_dpb++] = Ref{cur, mmco5 ? 0 : k.frame_num, mmco5 ? 0 : poc, t_final, b_final, 0, lt_cur, !largo_cur, largo_cur, mmco5 ? 0 : (int)k.frame_num};
    return 0;
}

VADriverContextP ctx;
VAContextID contexto = VA_INVALID_ID;
VAConfigID config = VA_INVALID_ID;
unsigned ancho_mbs, alto_mbs;
const Sps *sps_activo;

int cerrar_sesion() {
    if (contexto != VA_INVALID_ID) { ctx->vtable->vaDestroyContext(ctx, contexto); ctx->vtable->vaDestroySurfaces(ctx, sups, NUM_SUP); contexto = VA_INVALID_ID; }
    if (config != VA_INVALID_ID) { ctx->vtable->vaDestroyConfig(ctx, config); config = VA_INVALID_ID; }
    ancho_mbs = alto_mbs = 0;
    return 0;
}
int crear_sesion(unsigned w, unsigned h) {
    VAConfigAttrib at; at.type = VAConfigAttribRTFormat; at.value = VA_RT_FORMAT_YUV420;
    VA(ctx->vtable->vaCreateConfig(ctx, VAProfileH264High, VAEntrypointVLD, &at, 1, &config));
    VAStatus st = ctx->vtable->vaCreateSurfaces2(ctx, VA_RT_FORMAT_YUV420, w * 16, h * 16, sups, NUM_SUP, nullptr, 0);
    if (st != VA_STATUS_SUCCESS) { ctx->vtable->vaDestroyConfig(ctx, config); config = VA_INVALID_ID; FALLA(TAEK_VIDEO_FALLO, "vaCreateSurfaces2 = %d", st); }
    st = ctx->vtable->vaCreateContext(ctx, config, w * 16, h * 16, VA_PROGRESSIVE, sups, NUM_SUP, &contexto);
    if (st != VA_STATUS_SUCCESS) {
        ctx->vtable->vaDestroySurfaces(ctx, sups, NUM_SUP); ctx->vtable->vaDestroyConfig(ctx, config);
        contexto = VA_INVALID_ID; config = VA_INVALID_ID;
        FALLA(TAEK_VIDEO_FALLO, "vaCreateContext = %d", st);
    }
    ancho_mbs = w; alto_mbs = h;
    return 0;
}
int asegurar_sesion(const Sps &s) {
    if (contexto != VA_INVALID_ID && (s.w_mbs != ancho_mbs || s.h_mbs != alto_mbs)) {
        if (n_dpb || cuantos_pendientes()) FALLA(TAEK_VIDEO_INCOMPATIBLE, "cambio de resolucion a mitad del video");
        cerrar_sesion();
    }
    return contexto == VA_INVALID_ID ? crear_sesion(s.w_mbs, s.h_mbs) : 0;
}

// Sin bytes de prevencion: las cabeceras se leen de aqui y el flujo original es lo que recibe la GPU.
uint8_t rbsp[8192];
size_t a_rbsp(const uint8_t *d, size_t n) {
    size_t o = 0; unsigned ceros = 0;
    for (size_t i = 0; i < n && o < sizeof(rbsp); i++) {
        if (ceros >= 2 && d[i] == 3) { ceros = 0; continue; }
        rbsp[o++] = d[i];
        ceros = d[i] ? 0 : ceros + 1;
    }
    return o;
}
// Referencias por nombre de imagen (8.2.4.1).
void preparar_numeros(const Sps &s, unsigned fn) {
    unsigned max = 1u << s.log2_fn;
    for (unsigned i = 0; i < n_dpb; i++) {
        Ref &r = dpb[i];
        if (r.corto) { r.fn_wrap = r.frame_num > fn ? (int)r.frame_num - (int)max : (int)r.frame_num; r.pic_num = r.fn_wrap; }
    }
}
unsigned lista_inicial(int poc, unsigned cual, unsigned tipo, const Ref **o) {
    const Ref *a[MAX_DPB], *bb[MAX_DPB], *l[MAX_DPB];
    unsigned na = 0, nb = 0, nl = 0, n = 0;
    for (unsigned i = 0; i < n_dpb; i++) {
        const Ref *r = &dpb[i];
        if (r->largo) l[nl++] = r;
        else if (r->corto) {
            if (tipo == 0) a[na++] = r;
            else if ((r->poc <= poc) == (cual == 0)) a[na++] = r;
            else bb[nb++] = r;
        }
    }
    ordenar(l, nl, lt_asc);
    if (tipo == 0) ordenar(a, na, pn_desc);
    else { ordenar(a, na, cual == 0 ? poc_desc : poc_asc); ordenar(bb, nb, cual == 0 ? poc_asc : poc_desc); }
    for (unsigned i = 0; i < na; i++) o[n++] = a[i];
    for (unsigned i = 0; i < nb; i++) o[n++] = bb[i];
    for (unsigned i = 0; i < nl; i++) o[n++] = l[i];
    return n;
}
void modificar_lista(const Sps &s, unsigned cual, unsigned activos, unsigned fn) {
    unsigned max = 1u << s.log2_fn; int pred = (int)fn; unsigned idx = 0;
    const Ref **l = c.lista[cual];
    for (unsigned i = 0; i < c.n_rplm[cual] && idx < activos; i++) {
        unsigned ic = c.rplm[cual][i].idc, v = c.rplm[cual][i].v; const Ref *r = nullptr;
        if (ic < 2) {
            int d = (int)v + 1;
            if (ic == 0) { pred -= d; if (pred < 0) pred += (int)max; }
            else { pred += d; if (pred >= (int)max) pred -= (int)max; }
            int pn = pred > (int)fn ? pred - (int)max : pred;
            for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].corto && dpb[j].pic_num == pn) r = &dpb[j];
        } else {
            for (unsigned j = 0; j < n_dpb; j++) if (dpb[j].largo && dpb[j].lt_idx == (int)v) r = &dpb[j];
        }
        if (!r) continue;
        for (unsigned j = activos; j > idx; j--) l[j] = l[j - 1];
        l[idx++] = r;
        unsigned w = idx;
        for (unsigned j = idx; j <= activos; j++) if (l[j] != r) l[w++] = l[j];
    }
}

// Lee la cabecera del corte. Deja el resultado en c y b justo al principio de los datos.
int leer_corte(unsigned nal, unsigned ref, Bits &b, const Pps *&ppp, const Sps *&sss) {
    memset(&c, 0, sizeof(c));
    c.nal_tipo = nal; c.nal_ref = ref;
    c.primero = b.ue();
    c.tipo = b.ue() % 5;
    unsigned pid = b.ue();
    if (pid >= 256 || !pps[pid].ok) FALLA(TAEK_VIDEO_FALLO, "corte sin PPS");
    const Pps &p = pps[pid];
    const Sps &s = sps[p.sps];
    ppp = &p; sss = &s;
    if (!s.ok) FALLA(TAEK_VIDEO_FALLO, "corte sin SPS");
    if (c.tipo > 2) FALLA(TAEK_VIDEO_INCOMPATIBLE, "cortes SP/SI");
    c.frame_num = b.u(s.log2_fn);
    if (nal == 5) c.idr_id = b.ue();
    if (s.poc_tipo == 0) { c.poc_lsb = b.u(s.log2_poc); if (p.orden_inf) c.delta_bot = b.se(); }
    if (p.red) c.redundante = b.ue();
    if (c.tipo == 1) c.directo = b.u1();
    c.l0 = p.l0; c.l1 = p.l1;
    if (c.tipo == 0 || c.tipo == 1) if (b.u1()) { c.l0 = b.ue() + 1; if (c.tipo == 1) c.l1 = b.ue() + 1; }
    if ((c.tipo != 2 && c.l0 > MAX_DPB) || (c.tipo == 1 && c.l1 > MAX_DPB)) FALLA(TAEK_VIDEO_FALLO, "lista de referencias excesiva");
    for (unsigned l = 0; l < (c.tipo == 1 ? 2u : c.tipo == 0 ? 1u : 0u); l++) {
        if (b.u1()) for (;;) {
            unsigned ic = b.ue();
            if (ic == 3 || b.malo) break;
            if (ic > 3) FALLA(TAEK_VIDEO_INCOMPATIBLE, "modificacion de lista no admitida");
            if (c.n_rplm[l] >= 66) FALLA(TAEK_VIDEO_FALLO, "demasiadas modificaciones de lista");
            c.rplm[l][c.n_rplm[l]].idc = ic; c.rplm[l][c.n_rplm[l]++].v = b.ue();
        }
    }
    VASliceParameterBufferH264 &v = c.va;
    v.luma_log2_weight_denom = v.chroma_log2_weight_denom = 0;
    if ((p.wp && c.tipo == 0) || (p.wb == 1 && c.tipo == 1)) {
        v.luma_log2_weight_denom = b.ue(); v.chroma_log2_weight_denom = b.ue();
        if (v.luma_log2_weight_denom > 7 || v.chroma_log2_weight_denom > 7) FALLA(TAEK_VIDEO_FALLO, "pesos invalidos");
        for (unsigned l = 0; l < (c.tipo == 1 ? 2u : 1u); l++) {
            unsigned nact = l ? c.l1 : c.l0;
            for (unsigned k = 0; k < nact; k++) {
                short lw = 1 << v.luma_log2_weight_denom, lo = 0, cw[2] = {(short)(1 << v.chroma_log2_weight_denom), (short)(1 << v.chroma_log2_weight_denom)}, co[2] = {0, 0};
                unsigned fl = b.u1(), fc = 0;
                if (fl) { lw = b.se(); lo = b.se(); }
                fc = b.u1();
                if (fc) for (int t = 0; t < 2; t++) { cw[t] = b.se(); co[t] = b.se(); }
                if (l == 0) { if (fl) v.luma_weight_l0_flag = 1; if (fc) v.chroma_weight_l0_flag = 1; v.luma_weight_l0[k] = lw; v.luma_offset_l0[k] = lo; for (int t = 0; t < 2; t++) { v.chroma_weight_l0[k][t] = cw[t]; v.chroma_offset_l0[k][t] = co[t]; } }
                else { if (fl) v.luma_weight_l1_flag = 1; if (fc) v.chroma_weight_l1_flag = 1; v.luma_weight_l1[k] = lw; v.luma_offset_l1[k] = lo; for (int t = 0; t < 2; t++) { v.chroma_weight_l1[k][t] = cw[t]; v.chroma_offset_l1[k][t] = co[t]; } }
            }
        }
    }
    if (ref) {
        if (nal == 5) { c.sin_salida_previa = b.u1(); c.largo_idr = b.u1(); }
        else if ((c.adaptativo = b.u1())) for (;;) {
            unsigned op = b.ue();
            if (!op || b.malo) break;
            if (c.n_mmco >= 66) FALLA(TAEK_VIDEO_FALLO, "demasiadas operaciones MMCO");
            unsigned a = 0, x = 0;
            if (op == 1 || op == 3 || op == 2 || op == 4 || op == 6) a = b.ue();
            if (op == 3) x = b.ue();
            c.mmco[c.n_mmco].op = op; c.mmco[c.n_mmco].a = a; c.mmco[c.n_mmco++].b = x;
        }
    }
    if (p.cabac && c.tipo != 2) c.cabac_idc = b.ue();
    c.qp_delta = b.se();
    if (p.deb) { c.deb_idc = b.ue(); if (c.deb_idc != 1) { c.alfa = b.se(); c.beta = b.se(); } }
    if (b.malo) FALLA(TAEK_VIDEO_FALLO, "cabecera de corte truncada");
    return 0;
}

struct Nal { const uint8_t *d; size_t n; };
struct Medidas { unsigned long long ns_gpu, ns_copia, decodificados, cortes; };
Medidas med;
unsigned corromper_activo, err_gpu, ultimo_tipo, ultimo_idr;
bool flujo_malo;
unsigned tipos_vistos[3];
static uint8_t copia_corrupta[1 << 21];
static VAPictureParameterBufferH264 pic;
static VAIQMatrixBufferH264 iq;

// Una unidad de acceso: parametros, y uno o varios cortes de la misma imagen. Sin efecto si no hay superficie libre.
int decodificar_unidad(const Nal *u, unsigned k, long long pts) {
    if (flujo_malo) FALLA(TAEK_VIDEO_FALLO, "sesion invalidada por un fallo anterior");
    bool hay_corte = false;
    for (unsigned i = 0; i < k; i++) { unsigned t = u[i].d[0] & 31; if (t == 1 || t == 5) hay_corte = true; }
    if (!hay_corte) {
        for (unsigned i = 0; i < k; i++) {
            unsigned t = u[i].d[0] & 31;
            if (t != 7 && t != 8) continue;
            size_t nr = a_rbsp(u[i].d + 1, u[i].n - 1);
            Bits b{rbsp, nr, 0};
            const char *m = t == 7 ? leer_sps(b) : leer_pps(b);
            if (m) FALLA(TAEK_VIDEO_INCOMPATIBLE, "%s", m);
        }
        return 0;
    }
    VASurfaceID actual = sup_libre();
    if (actual == SIN_SUP) {
        if (cuantos_pendientes()) { forzar_salida = true; return TAEK_VIDEO_OTRA_VEZ; }
        FALLA(TAEK_VIDEO_FALLO, "sin superficies libres");
    }
    forzar_salida = false;

    static VABufferID bufs[MAX_NAL * 2 + 2];
    unsigned nbuf = 0, ncortes = 0, primero_previo = 0;
    bool empezada = false;
    const Sps *s0 = nullptr;
    unsigned nal0 = 0, ref0 = 0, frame_num0 = 0, tipo0 = 0;
    int poc = 0, top = 0, bot = 0;
    #define FUERA(rc_, ...) do { snprintf(motivo_txt, sizeof(motivo_txt), __VA_ARGS__); if (empezada) flujo_malo = true; return (rc_); } while (0)
    #define VAF(x) do { VAStatus st_ = (x); if (st_ != VA_STATUS_SUCCESS) FUERA(TAEK_VIDEO_FALLO, "%s = %d", #x, st_); } while (0)
    for (unsigned i = 0; i < k; i++) {
        unsigned nal = u[i].d[0] & 31, ref = (u[i].d[0] >> 5) & 3;
        if (nal >= 2 && nal <= 4) FUERA(TAEK_VIDEO_INCOMPATIBLE, "particion de datos");
        if (nal != 7 && nal != 8 && nal != 1 && nal != 5) continue;
        size_t nr = a_rbsp(u[i].d + 1, u[i].n - 1);
        Bits b{rbsp, nr, 0};
        if (nal == 7 || nal == 8) {
            const char *m = nal == 7 ? leer_sps(b) : leer_pps(b);
            if (m) FUERA(TAEK_VIDEO_INCOMPATIBLE, "%s", m);
            continue;
        }
        const Pps *pp; const Sps *ss;
        int rl = leer_corte(nal, ref, b, pp, ss);
        if (rl) { if (empezada) flujo_malo = true; return rl; }
        const Pps &p = *pp; const Sps &s = *ss;
        if (c.redundante) continue;   // Copia redundante de un corte ya recibido.
        if (!empezada) {
            int ra = asegurar_sesion(s);
            if (ra) return ra;
            s0 = &s; sps_activo = &s; nal0 = nal; ref0 = ref; frame_num0 = c.frame_num; tipo0 = c.tipo;
            reordenar_actual = s.reordenar;
            unsigned max_fn = 1u << s.log2_fn;
            if (nal != 5 && med.decodificados && c.frame_num != fn_ref_prev && c.frame_num != (fn_ref_prev + 1) % max_fn) FUERA(TAEK_VIDEO_INCOMPATIBLE, "hueco en frame_num");
            if (nal == 5) {
                offset_fn_prev = 0;
                periodo_actual++;
                if (c.sin_salida_previa) for (unsigned j = 0; j < NUM_SUP; j++) pendientes[j].usada = false;
            }
            poc = calcular_poc(s, c, top, bot);
            preparar_numeros(s, c.frame_num);
            memset(&pic, 0, sizeof(pic));
            pic.CurrPic.picture_id = actual; pic.CurrPic.frame_idx = c.frame_num;
            pic.CurrPic.flags = ref ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
            pic.CurrPic.TopFieldOrderCnt = top; pic.CurrPic.BottomFieldOrderCnt = bot;
            for (unsigned j = 0; j < 16; j++) { if (nal != 5 && j < n_dpb) a_va(pic.ReferenceFrames[j], dpb[j]); else va_vacia(pic.ReferenceFrames[j]); }
            pic.picture_width_in_mbs_minus1 = s.w_mbs - 1; pic.picture_height_in_mbs_minus1 = s.h_mbs - 1;
            pic.num_ref_frames = s.refs;
            pic.seq_fields.bits.chroma_format_idc = s.chroma; pic.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = s.gaps;
            pic.seq_fields.bits.frame_mbs_only_flag = 1; pic.seq_fields.bits.direct_8x8_inference_flag = s.d8x8;
            pic.seq_fields.bits.MinLumaBiPredSize8x8 = s.nivel >= 31 ? 1 : 0;   // Tabla A-4.
            pic.seq_fields.bits.log2_max_frame_num_minus4 = s.log2_fn - 4; pic.seq_fields.bits.pic_order_cnt_type = s.poc_tipo;
            pic.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = s.log2_poc ? s.log2_poc - 4 : 0;
            pic.pic_init_qp_minus26 = p.qp - 26; pic.pic_init_qs_minus26 = p.qs - 26;
            pic.chroma_qp_index_offset = p.crq; pic.second_chroma_qp_index_offset = p.crq2;
            pic.pic_fields.bits.entropy_coding_mode_flag = p.cabac; pic.pic_fields.bits.weighted_pred_flag = p.wp; pic.pic_fields.bits.weighted_bipred_idc = p.wb;
            pic.pic_fields.bits.transform_8x8_mode_flag = p.t8; pic.pic_fields.bits.constrained_intra_pred_flag = p.cip; pic.pic_fields.bits.pic_order_present_flag = p.orden_inf;
            pic.pic_fields.bits.deblocking_filter_control_present_flag = p.deb; pic.pic_fields.bits.redundant_pic_cnt_present_flag = p.red;
            pic.pic_fields.bits.reference_pic_flag = ref != 0;
            pic.frame_num = c.frame_num;

            const uint8_t (*f4)[16] = p.escala ? p.l4 : s.escala ? s.l4 : nullptr;
            const uint8_t (*f8)[64] = p.escala ? p.l8 : s.escala ? s.l8 : nullptr;
            memset(&iq, 16, sizeof(iq));
            if (f4) for (unsigned a = 0; a < 6; a++) for (unsigned j = 0; j < 16; j++) iq.ScalingList4x4[a][ZZ4[j]] = f4[a][j];
            if (f8) for (unsigned a = 0; a < 2; a++) for (unsigned j = 0; j < 64; j++) iq.ScalingList8x8[a][ZZ8[j]] = f8[a][j];
            empezada = true;
            unsigned long long t1 = taek_media_ns();
            VAF(ctx->vtable->vaCreateBuffer(ctx, contexto, VAPictureParameterBufferType, sizeof(pic), 1, &pic, &bufs[nbuf])); nbuf++;
            VAF(ctx->vtable->vaCreateBuffer(ctx, contexto, VAIQMatrixBufferType, sizeof(iq), 1, &iq, &bufs[nbuf])); nbuf++;
            VAF(ctx->vtable->vaBeginPicture(ctx, contexto, actual));
            VAF(ctx->vtable->vaRenderPicture(ctx, contexto, bufs, 2));
            med.ns_gpu += taek_media_ns() - t1;
        } else if (c.primero <= primero_previo) FUERA(TAEK_VIDEO_INCOMPATIBLE, "cortes en orden arbitrario");
        primero_previo = c.primero;
        if (ncortes >= MAX_NAL) FUERA(TAEK_VIDEO_FALLO, "demasiados cortes");
        VASliceParameterBufferH264 &v = c.va;
        v.slice_data_size = (uint32_t)u[i].n; v.slice_data_offset = 0; v.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        v.slice_data_bit_offset = (uint16_t)(b.p + 8);   // Incluye el byte de cabecera NAL y no cuenta los de prevencion.
        v.first_mb_in_slice = c.primero; v.slice_type = c.tipo; v.direct_spatial_mv_pred_flag = c.directo;
        v.num_ref_idx_l0_active_minus1 = c.tipo == 2 ? 0 : c.l0 - 1; v.num_ref_idx_l1_active_minus1 = c.tipo == 1 ? c.l1 - 1 : 0;
        v.cabac_init_idc = c.cabac_idc; v.slice_qp_delta = c.qp_delta; v.disable_deblocking_filter_idc = c.deb_idc;
        v.slice_alpha_c0_offset_div2 = c.alfa; v.slice_beta_offset_div2 = c.beta;
        for (unsigned j = 0; j < 32; j++) { va_vacia(v.RefPicList0[j]); va_vacia(v.RefPicList1[j]); }
        if (c.tipo != 2) {
            for (unsigned l = 0; l < (c.tipo == 1 ? 2u : 1u); l++) {
                const Ref **o = c.lista[l];
                unsigned ni = lista_inicial(poc, l, c.tipo, o);
                if (l == 1 && ni > 1) {
                    bool igual = true;
                    const Ref *o0[33]; unsigned n0 = lista_inicial(poc, 0, c.tipo, o0);
                    if (n0 != ni) igual = false; else for (unsigned j = 0; j < ni; j++) if (o0[j] != o[j]) igual = false;
                    if (igual) { const Ref *t = o[0]; o[0] = o[1]; o[1] = t; }
                }
                unsigned act = l ? c.l1 : c.l0;
                for (unsigned j = ni; j < 33; j++) o[j] = nullptr;
                modificar_lista(s, l, act, c.frame_num);
                VAPictureH264 *dst = l ? v.RefPicList1 : v.RefPicList0;
                for (unsigned j = 0; j < act && j < 32; j++) if (o[j]) a_va(dst[j], *o[j]);
            }
        }
        const uint8_t *datos = u[i].d;
        if (corromper_activo) {
            size_t tam = u[i].n;
            if (tam > sizeof(copia_corrupta)) FUERA(TAEK_VIDEO_FALLO, "corte mayor que la copia de prueba");
            memcpy(copia_corrupta, datos, tam);
            copia_corrupta[tam / 2] ^= 0x55;
            datos = copia_corrupta;
        }
        unsigned long long t1 = taek_media_ns();
        VAF(ctx->vtable->vaCreateBuffer(ctx, contexto, VASliceParameterBufferType, sizeof(v), 1, &v, &bufs[nbuf])); nbuf++;
        VAF(ctx->vtable->vaCreateBuffer(ctx, contexto, VASliceDataBufferType, v.slice_data_size, 1, const_cast<uint8_t *>(datos), &bufs[nbuf])); nbuf++;
        VAF(ctx->vtable->vaRenderPicture(ctx, contexto, &bufs[nbuf - 2], 2));
        med.ns_gpu += taek_media_ns() - t1;
        ncortes++;
    }
    if (!empezada) return 0;
    if (!ncortes) { flujo_malo = true; FALLA(TAEK_VIDEO_FALLO, "imagen sin cortes"); }
    unsigned long long t1 = taek_media_ns();
    VAF(ctx->vtable->vaEndPicture(ctx, contexto));
    VAStatus sinc = ctx->vtable->vaSyncSurface(ctx, actual);
    // En el control negativo el hardware puede informar del flujo roto: se anota y se sigue, para comparar tambien su salida.
    if (corromper_activo && sinc == VA_STATUS_ERROR_DECODING_ERROR) err_gpu++;
    else if (sinc != VA_STATUS_SUCCESS) { flujo_malo = true; FALLA(TAEK_VIDEO_FALLO, "vaSyncSurface = %d", sinc); }
    med.ns_gpu += taek_media_ns() - t1;
    // Los buferes son del cliente: el driver no los suelta al terminar el fotograma.
    for (unsigned i = 0; i < nbuf; i++) VAF(ctx->vtable->vaDestroyBuffer(ctx, bufs[i]));
    med.decodificados++; med.cortes += ncortes;
    ultimo_tipo = tipo0; ultimo_idr = nal0 == 5;
    tipos_vistos[nal0 == 5 ? 2 : tipo0 == 2 ? 2 : tipo0]++;

    bool mmco5 = false;
    Corte &k0 = c;   // Todos los cortes de una imagen llevan el mismo marcado.
    k0.frame_num = frame_num0; k0.nal_ref = ref0; k0.nal_tipo = nal0;
    int rm = marcar(*s0, k0, actual, poc, top, bot, mmco5);
    if (rm) { flujo_malo = true; return rm; }
    if (mmco5) {
        // Lo anterior sale antes y lo que venga detras cuenta desde cero, como tras un IDR.
        periodo_actual++;
        poc_msb_prev = 0; poc_lsb_prev = top - poc; offset_fn_prev = 0;
    }
    frame_num_prev = mmco5 ? 0 : frame_num0;
    if (ref0) fn_ref_prev = mmco5 ? 0 : frame_num0;
    unsigned h = NUM_SUP;
    for (unsigned j = 0; j < NUM_SUP; j++) if (!pendientes[j].usada) { h = j; break; }
    if (h == NUM_SUP) { flujo_malo = true; FALLA(TAEK_VIDEO_FALLO, "cola de salida llena"); }
    pendientes[h] = Pendiente{actual, mmco5 ? 0 : poc, pts, periodo_actual, true};
    return 0;
}

void reiniciar_estado() {
    n_dpb = 0; frame_num_prev = 0; offset_fn_prev = 0; fn_ref_prev = 0; poc_msb_prev = poc_lsb_prev = 0;
    memset(pendientes, 0, sizeof(pendientes));
    periodo_actual = 0; forzar_salida = false; drenando = false; flujo_malo = false;
    memset(&med, 0, sizeof(med)); memset(tipos_vistos, 0, sizeof(tipos_vistos));
    err_gpu = 0;
}

// Lectura de la superficie. Puede estar en la apertura, con write-combining: leerla con MOVNTDQA es mucho mas rapido que con REP MOVSB.
bool hay_sse41() { static int v = -1; if (v < 0) { unsigned a, b, cc, d; v = __get_cpuid(1, &a, &b, &cc, &d) && (cc & bit_SSE4_1); } return v; }
__attribute__((target("sse4.1"))) void copia_rapida(uint8_t *d, const uint8_t *s, size_t n) {
    while (n && ((uintptr_t)s & 15)) { *d++ = *s++; n--; }
    for (; n >= 16; n -= 16, s += 16, d += 16) _mm_storeu_si128(reinterpret_cast<__m128i *>(d), _mm_stream_load_si128(reinterpret_cast<__m128i *>(const_cast<uint8_t *>(s))));
    while (n--) *d++ = *s++;
}
inline void copia_fila(uint8_t *d, const uint8_t *s, size_t n) { if (hay_sse41()) copia_rapida(d, s, n); else memcpy(d, s, n); }

int con_imagen(VASurfaceID s, VAImage &im, uint8_t *&m) {
    VA(ctx->vtable->vaDeriveImage(ctx, s, &im));
    void *p = nullptr;
    VA(ctx->vtable->vaMapBuffer(ctx, im.buf, &p));
    if (im.format.fourcc != VA_FOURCC_NV12) FALLA(TAEK_VIDEO_FALLO, "superficie que no es NV12");
    m = static_cast<uint8_t *>(p);
    return 0;
}
int sin_imagen(VAImage &im) {
    VA(ctx->vtable->vaUnmapBuffer(ctx, im.buf));
    VA(ctx->vtable->vaDestroyImage(ctx, im.image_id));
    return 0;
}

int tomar(taek_video_peticion *r) {
    Pendiente *p = siguiente_salida();
    const Sps *s = sps_activo;
    if (!p || !s) FALLA(TAEK_VIDEO_FALLO, "nada que tomar");
    unsigned long long t0 = taek_media_ns();
    VAImage im; uint8_t *m = nullptr;
    int rc = con_imagen(p->sup, im, m);
    if (rc) return rc;
    static uint8_t linea[MAX_LADO + 64];
    const uint8_t *py = m + im.offsets[0] + (size_t)s->recorte_y * im.pitches[0] + s->recorte_x;
    const uint8_t *puv = m + im.offsets[1] + (size_t)(s->recorte_y / 2) * im.pitches[1] + s->recorte_x;
    unsigned w = s->ancho, h = s->alto;
    for (unsigned y = 0; y < h; y++) copia_fila(r->y + (size_t)y * r->paso_y, py + (size_t)y * im.pitches[0], w);
    for (unsigned y = 0; y < h / 2; y++) {
        copia_fila(linea, puv + (size_t)y * im.pitches[1], w);
        uint8_t *du = r->u + (size_t)y * r->paso_c, *dv = r->v + (size_t)y * r->paso_c;
        for (unsigned x = 0; x < w / 2; x++) { du[x] = linea[2 * x]; dv[x] = linea[2 * x + 1]; }
    }
    rc = sin_imagen(im);
    if (rc) return rc;
    r->ancho = w; r->alto = h; r->poc = p->poc; r->pts_salida = p->pts; r->rango_completo = s->rango_completo; r->matriz_color = s->matriz_color;
    p->usada = false;
    med.ns_copia += taek_media_ns() - t0;
    return 0;
}

uint8_t *entrada; size_t entrada_cap;
unsigned muestra_actual, inyectar_tipo, inyectar_muestra;

int abrir(taek_video_peticion *r) {
    const uint8_t *d = r->datos; size_t n = r->n;
    reiniciar_estado();
    muestra_actual = 0; inyectar_tipo = r->inyectar_tipo; inyectar_muestra = r->inyectar_muestra;
    memset(sps, 0, sizeof(sps)); memset(pps, 0, sizeof(pps)); sps_activo = nullptr;
    if (!d || n < 7 || d[0] != 1) FALLA(TAEK_VIDEO_INCOMPATIBLE, "registro avcC invalido");
    size_t o = 5;
    unsigned ns = d[o++] & 31, primero = 32;
    for (unsigned i = 0; i < ns; i++) {
        if (o + 2 > n) FALLA(TAEK_VIDEO_INCOMPATIBLE, "registro avcC truncado");
        size_t l = (size_t)d[o] << 8 | d[o + 1]; o += 2;
        if (!l || l > n - o) FALLA(TAEK_VIDEO_INCOMPATIBLE, "registro avcC truncado");
        size_t nr = a_rbsp(d + o + 1, l - 1);
        Bits b{rbsp, nr, 0};
        const char *m = leer_sps(b);
        if (m) FALLA(TAEK_VIDEO_INCOMPATIBLE, "%s", m);
        if (primero == 32) { Bits q{rbsp, nr, 24}; primero = q.ue(); }
        o += l;
    }
    if (o >= n) FALLA(TAEK_VIDEO_INCOMPATIBLE, "registro avcC truncado");
    unsigned np = d[o++];
    for (unsigned i = 0; i < np; i++) {
        if (o + 2 > n) FALLA(TAEK_VIDEO_INCOMPATIBLE, "registro avcC truncado");
        size_t l = (size_t)d[o] << 8 | d[o + 1]; o += 2;
        if (!l || l > n - o) FALLA(TAEK_VIDEO_INCOMPATIBLE, "registro avcC truncado");
        size_t nr = a_rbsp(d + o + 1, l - 1);
        Bits b{rbsp, nr, 0};
        const char *m = leer_pps(b);
        if (m) FALLA(TAEK_VIDEO_INCOMPATIBLE, "%s", m);
        o += l;
    }
    if (primero >= 32 || !sps[primero].ok) FALLA(TAEK_VIDEO_INCOMPATIBLE, "MP4 sin SPS en el registro avcC");
    const Sps &s = sps[primero];
    sps_activo = &s;
    reordenar_actual = s.reordenar;
    int rc = asegurar_sesion(s);
    if (rc) return rc;
    r->ancho = s.ancho; r->alto = s.alto; r->rango_completo = s.rango_completo; r->matriz_color = s.matriz_color;
    return 0;
}

int muestra(taek_video_peticion *r) {
    if (r->longitud_nal < 1 || r->longitud_nal > 4) FALLA(TAEK_VIDEO_INCOMPATIBLE, "longitud NAL invalida");
    if (inyectar_tipo && muestra_actual + 1 == inyectar_muestra) {
        unsigned t = inyectar_tipo; inyectar_tipo = 0;
        if (t == TAEK_VIDEO_INYECTAR_BLOQUEO) taek_media_dormir_us(8000000);
        flujo_malo = true;
        FALLA(TAEK_VIDEO_FALLO, "fallo inyectado en la muestra %u", inyectar_muestra);
    }
    // El driver solo debe ver memoria de su ventana: la muestra del kernel se copia aqui.
    if (r->n > entrada_cap) {
        size_t cap = r->n + (1u << 20);
        uint8_t *q = static_cast<uint8_t *>(taek_media_alloc(cap));
        if (!q) FALLA(TAEK_VIDEO_FALLO, "sin memoria para la muestra");
        if (entrada) taek_media_free(entrada);
        entrada = q; entrada_cap = cap;
    }
    memcpy(entrada, r->datos, r->n);
    static Nal nals[MAX_NAL];
    unsigned k = 0; size_t o = 0;
    while (o < r->n) {
        if (r->n - o < r->longitud_nal) FALLA(TAEK_VIDEO_FALLO, "muestra truncada");
        size_t l = 0;
        for (unsigned i = 0; i < r->longitud_nal; i++) l = l << 8 | entrada[o++];
        if (!l || l > r->n - o) FALLA(TAEK_VIDEO_FALLO, "NAL fuera de la muestra");
        if (k >= MAX_NAL) FALLA(TAEK_VIDEO_FALLO, "demasiadas NAL en una muestra");
        nals[k++] = Nal{entrada + o, l};
        o += l;
    }
    int rc = decodificar_unidad(nals, k, r->pts);
    if (rc != TAEK_VIDEO_OTRA_VEZ && rc >= 0) muestra_actual++;
    r->listos = calc_listos();
    return rc;
}
}  // namespace

extern "C" const uint8_t taek_video_ini[], taek_video_fin[];
extern "C" const uint64_t taek_video_hash[];
extern "C" const unsigned taek_video_num;

extern "C" void taek_h264_contexto(VADriverContextP x) { ctx = x; }

extern "C" int taek_h264_op(struct taek_video_peticion *r) {
    motivo_txt[0] = 0;
    int rc = -22;
    switch (r->orden) {
        case TAEK_VIDEO_ABRIR: rc = abrir(r); break;
        case TAEK_VIDEO_MUESTRA: rc = muestra(r); break;
        case TAEK_VIDEO_TOMAR: rc = tomar(r); r->listos = calc_listos(); break;
        case TAEK_VIDEO_DRENAR: drenando = true; r->listos = calc_listos(); rc = 0; break;
        case TAEK_VIDEO_CERRAR:
            r->ns_gpu = med.ns_gpu; r->ns_copia = med.ns_copia; r->decodificados = med.decodificados; r->cortes = med.cortes;
            reiniciar_estado();
            rc = cerrar_sesion();
            break;
        default: snprintf(motivo_txt, sizeof(motivo_txt), "orden %u desconocida", r->orden); break;
    }
    if (rc < 0) memcpy(r->motivo, motivo_txt, sizeof(r->motivo));
    return rc;
}

// ---------------------------------------------------------------------------------------------------------------- prueba con vector
// Primeros fotogramas del video de 1080p incrustados con su referencia de FFmpeg. Solo existen en el modulo de pruebas.
namespace {
uint64_t hash_fnv(uint64_t h, const uint8_t *d, size_t n) {
    // FNV-1a por palabras de 8 bytes: leer la apertura byte a byte seria demasiado lento.
    for (; n >= 8; n -= 8, d += 8) { uint64_t w; memcpy(&w, d, 8); h = (h ^ w) * 0x100000001b3ull; }
    for (; n; n--, d++) h = (h ^ *d) * 0x100000001b3ull;
    return h;
}
uint64_t hash_linea(uint64_t h, const uint8_t *p, unsigned pitch, unsigned w, unsigned f) {
    static uint8_t linea[MAX_LADO + 64];
    for (unsigned y = 0; y < f; y++) { copia_fila(linea, p + (size_t)y * pitch, w); h = hash_fnv(h, linea, w); }
    return h;
}
struct Estad { unsigned decodificados, iguales, distintos, primer_mal_valido; int primer_mal; uint64_t hash_mal, hash_esp; unsigned pitch; unsigned salidos; };
Estad est;
int hash_siguiente() {
    Pendiente *p = siguiente_salida();
    const Sps *s = sps_activo;
    if (!p || !s) FALLA(TAEK_VIDEO_FALLO, "nada que comparar");
    VAImage im; uint8_t *m = nullptr;
    int rc = con_imagen(p->sup, im, m);
    if (rc) return rc;
    uint64_t h = 0xcbf29ce484222325ull;
    h = hash_linea(h, m + im.offsets[0], im.pitches[0], s->ancho, s->alto);
    h = hash_linea(h, m + im.offsets[1], im.pitches[1], s->ancho, s->alto / 2);
    est.pitch = im.pitches[0];
    rc = sin_imagen(im);
    if (rc) return rc;
    p->usada = false;
    unsigned v = est.salidos++;
    if (v < taek_video_num && h == taek_video_hash[v]) est.iguales++;
    else { est.distintos++; if (est.primer_mal < 0) { est.primer_mal = (int)v; est.hash_mal = h; est.hash_esp = v < taek_video_num ? taek_video_hash[v] : 0; } }
    return 0;
}
int unidad_vector(const Nal *u, unsigned k) {
    for (;;) {
        int rc = decodificar_unidad(u, k, 0);
        while (calc_listos()) { int rh = hash_siguiente(); if (rh) return rh; }
        if (rc != TAEK_VIDEO_OTRA_VEZ) return rc;
    }
}
}  // namespace

// Decodifica los primeros "maximo" fotogramas. corromper!=0 cambia un byte de cada corte: es el control negativo.
extern "C" int taek_h264_decodificar(unsigned maximo, unsigned corromper, char *t, size_t n) {
    if (!taek_video_num) { snprintf(t, n, "modulo sin vector de prueba"); return -19; }
    const uint8_t *d = taek_video_ini; size_t total = (size_t)(taek_video_fin - taek_video_ini);
    reiniciar_estado();
    memset(&est, 0, sizeof(est)); est.primer_mal = -1;
    corromper_activo = corromper;
    motivo_txt[0] = 0;
    unsigned long long t0 = taek_media_ns();
    static Nal grupo[MAX_NAL];
    unsigned k = 0, hay_corte = 0, hechos = 0;
    int r = 0;
    size_t i = 0;
    while (i + 3 < total && !r && hechos < maximo) {
        if (d[i] || d[i + 1] || d[i + 2] != 1) { i++; continue; }
        size_t ini = i + 3, fin = ini;
        while (fin + 3 <= total && !(d[fin] == 0 && d[fin + 1] == 0 && (d[fin + 2] == 1 || (d[fin + 2] == 0 && fin + 3 < total && d[fin + 3] == 1)))) fin++;
        if (fin + 3 > total) fin = total;
        i = fin;
        unsigned nal = d[ini] & 31;
        bool nuevo = nal == 7 || nal == 8 || nal == 9 || nal == 6;
        if (nal == 1 || nal == 5) {
            size_t nr = a_rbsp(d + ini + 1, fin - ini - 1 < 16 ? fin - ini - 1 : 16);
            Bits b{rbsp, nr, 0};
            if (b.ue() == 0) nuevo = true;
        }
        if (nuevo && hay_corte) { r = unidad_vector(grupo, k); k = 0; hay_corte = 0; hechos++; if (r || hechos >= maximo) break; }
        if (k >= MAX_NAL) { r = TAEK_VIDEO_FALLO; snprintf(motivo_txt, sizeof(motivo_txt), "demasiadas NAL"); break; }
        grupo[k++] = Nal{d + ini, fin - ini};
        if (nal == 1 || nal == 5) hay_corte = 1;
    }
    if (!r && hay_corte && hechos < maximo) { r = unidad_vector(grupo, k); hechos++; }
    if (!r) {
        drenando = true;
        while (calc_listos() && !r) r = hash_siguiente();
    }
    corromper_activo = 0;
    snprintf(t, n, "decod=%u igual=%u distinto=%u err_gpu=%u i=%u p=%u b=%u mal=%d gpu_ms=%u total_ms=%u pitch=%u %s", (unsigned)med.decodificados, est.iguales, est.distintos, err_gpu, tipos_vistos[2], tipos_vistos[0], tipos_vistos[1], est.primer_mal,
             (unsigned)(med.ns_gpu / 1000000), (unsigned)((taek_media_ns() - t0) / 1000000), est.pitch, motivo_txt);
    if (est.primer_mal >= 0) printf("primer distinto %d: gpu=%llx ffmpeg=%llx", est.primer_mal, (unsigned long long)est.hash_mal, (unsigned long long)est.hash_esp);
    if (r) return r;
    if (corromper) return est.distintos ? 0 : -5;   // El control negativo solo pasa si detecta la diferencia.
    return med.decodificados && !est.distintos ? 0 : -5;
}

extern "C" int taek_h264_caps(char *t, size_t n) {
    VAProfile perfiles[64]; int np = 0;
    VA(ctx->vtable->vaQueryConfigProfiles(ctx, perfiles, &np));
    int h264 = 0, vld = 0;
    for (int i = 0; i < np; i++) {
        printf("perfil %d", perfiles[i]);
        if (perfiles[i] == VAProfileH264High) h264 = 1;
    }
    VAEntrypoint ep[32]; int ne = 0;
    if (h264) { VA(ctx->vtable->vaQueryConfigEntrypoints(ctx, VAProfileH264High, ep, &ne)); for (int i = 0; i < ne; i++) if (ep[i] == VAEntrypointVLD) vld = 1; }
    // Ausencia real: un perfil que este driver no compila debe rechazarse.
    VAConfigID cf; VAStatus sin = ctx->vtable->vaCreateConfig(ctx, VAProfileHEVCMain, VAEntrypointVLD, nullptr, 0, &cf);
    VAConfigAttrib at; at.type = VAConfigAttribRTFormat; at.value = VA_RT_FORMAT_YUV420;
    VAConfigID c2;
    VA(ctx->vtable->vaCreateConfig(ctx, VAProfileH264High, VAEntrypointVLD, &at, 1, &c2));
    VASurfaceID sp[4];
    VA(ctx->vtable->vaCreateSurfaces2(ctx, VA_RT_FORMAT_YUV420, 1920, 1088, sp, 4, nullptr, 0));
    VA(ctx->vtable->vaDestroySurfaces(ctx, sp, 4));
    VA(ctx->vtable->vaDestroyConfig(ctx, c2));
    snprintf(t, n, "perfiles=%d h264high=%d vld=%d hevc_rechazado=%d superficies=ok", np, h264, vld, sin != VA_STATUS_SUCCESS);
    return h264 && vld && sin != VA_STATUS_SUCCESS ? 0 : -5;
}
