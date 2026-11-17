/* HLS de YouTube -> MP4 progresivo (H186). Cabecera sin syscalls: la usa youtube.elf y se prueba en el host.
 *
 * Las variantes H.264 del HLS de YouTube (cliente VISIONOS) traen el video en MPEG-TS y el audio aparte en "packed
 * audio" (cabecera ID3 + tramas ADTS). El reproductor del nucleo lee MP4 con avc1/avcC + mp4a y tablas stts/ctts/
 * stsz/stsc/co64. Esto arma ese MP4 sin tocar el nucleo:
 *   - TS (ISO/IEC 13818-1): PAT -> PMT -> PID de H.264 (tipo 0x1B); PES por PUSI; PTS/DTS de 90 kHz.
 *   - H.264 Annex B -> AVCC (largo de 4 bytes por NAL; sin AUD); SPS/PPS al avcC; IDR = muestra de sincronia.
 *   - ADTS (ISO/IEC 14496-3 1.A.2) -> AAC crudo de 1024 muestras; AudioSpecificConfig al esds.
 *   - Salida: ftyp | mdat (tamano de 64 bits) con las muestras a medida que llegan | moov al final.
 * Uso: hm_iniciar(salida, cap); hm_ts(...) y hm_aac(...) por segmento, en cualquier orden; hm_terminar() -> bytes. */
#ifndef TAEK_HLS_MP4_H
#define TAEK_HLS_MP4_H

#ifndef HM_MALLOC
#define HM_MALLOC(n) malloc(n)
#define HM_REALLOC(p, n) realloc(p, n)
#define HM_FREE(p) free(p)
#endif

struct hm_pista {
    uint64_t *off;  uint32_t *tam; uint64_t *dts; int32_t *cts; uint8_t *sinc;
    uint32_t n, cap;
};
struct hm {
    uint8_t *o; uint64_t cap, pos;           /* salida */
    int error;                               /* 1 sin espacio, 2 sin memoria, 3 formato */
    struct hm_pista v, a;
    /* TS */
    int pid_pmt, pid_video;
    uint8_t *pes; uint32_t pes_n, pes_cap; int pes_abierto;
    uint8_t sps[256], pps[256]; uint32_t sps_n, pps_n;
    /* AAC */
    uint32_t frecuencia, canales, objeto, idx_frec;
    uint64_t audio_ts0; int audio_ts0_ok;    /* marca ID3 del primer segmento (90 kHz) */
    uint64_t audio_muestras;                 /* muestras de audio acumuladas (a la frecuencia) */
    uint64_t video_dts0; int video_dts0_ok;
};

static int hm_crecer(struct hm_pista *p) {
    if (p->n < p->cap) return 0;
    uint32_t c = p->cap ? p->cap * 2 : 4096;
    uint64_t *off = HM_REALLOC(p->off, (size_t)c * 8); if (!off) return -1; p->off = off;
    uint32_t *tam = HM_REALLOC(p->tam, (size_t)c * 4); if (!tam) return -1; p->tam = tam;
    uint64_t *dts = HM_REALLOC(p->dts, (size_t)c * 8); if (!dts) return -1; p->dts = dts;
    int32_t *cts = HM_REALLOC(p->cts, (size_t)c * 4); if (!cts) return -1; p->cts = cts;
    uint8_t *s = HM_REALLOC(p->sinc, (size_t)c); if (!s) return -1; p->sinc = s;
    p->cap = c;
    return 0;
}
static void hm_liberar(struct hm *h) {
    struct hm_pista *ps[2] = { &h->v, &h->a };
    for (int i = 0; i < 2; i++) { HM_FREE(ps[i]->off); HM_FREE(ps[i]->tam); HM_FREE(ps[i]->dts); HM_FREE(ps[i]->cts); HM_FREE(ps[i]->sinc); }
    HM_FREE(h->pes);
}
static void hm_b(struct hm *h, uint8_t v) { if (h->pos < h->cap) h->o[h->pos] = v; else h->error = 1; h->pos++; }
static void hm_16(struct hm *h, uint32_t v) { hm_b(h, (uint8_t)(v >> 8)); hm_b(h, (uint8_t)v); }
static void hm_32(struct hm *h, uint32_t v) { hm_16(h, v >> 16); hm_16(h, v & 0xFFFF); }
static void hm_64(struct hm *h, uint64_t v) { hm_32(h, (uint32_t)(v >> 32)); hm_32(h, (uint32_t)v); }
static void hm_tipo(struct hm *h, const char *t) { for (int i = 0; i < 4; i++) hm_b(h, (uint8_t)t[i]); }
static void hm_bytes(struct hm *h, const uint8_t *d, uint32_t n) { for (uint32_t i = 0; i < n; i++) hm_b(h, d[i]); }
static void hm_ceros(struct hm *h, uint32_t n) { while (n--) hm_b(h, 0); }
/* Caja con el tamano rellenado al cerrarla. */
static uint64_t hm_abrir(struct hm *h, const char *t) { uint64_t p = h->pos; hm_32(h, 0); hm_tipo(h, t); return p; }
static void hm_cerrar(struct hm *h, uint64_t p) {
    uint64_t n = h->pos - p;
    if (p + 4 <= h->cap) { h->o[p] = (uint8_t)(n >> 24); h->o[p + 1] = (uint8_t)(n >> 16); h->o[p + 2] = (uint8_t)(n >> 8); h->o[p + 3] = (uint8_t)n; }
}

static void hm_iniciar(struct hm *h, uint8_t *salida, uint64_t cap) {
    memset(h, 0, sizeof(*h));
    h->o = salida; h->cap = cap;
    h->pid_pmt = -1; h->pid_video = -1;
    uint64_t f = hm_abrir(h, "ftyp");
    hm_tipo(h, "isom"); hm_32(h, 0x200); hm_tipo(h, "isom"); hm_tipo(h, "iso2"); hm_tipo(h, "avc1"); hm_tipo(h, "mp41");
    hm_cerrar(h, f);
    hm_32(h, 1); hm_tipo(h, "mdat"); hm_64(h, 0);     /* largesize: se rellena al terminar */
}

/* ------------------------------------------------------------------ video: TS -> AVCC */
static void hm_muestra_video(struct hm *h, const uint8_t *es, uint32_t n, uint64_t pts, uint64_t dts) {
    if (hm_crecer(&h->v)) { h->error = 2; return; }
    uint64_t ini = h->pos; int idr = 0;
    uint32_t i = 0;
    while (i + 3 <= n && !(es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1)) i++;   /* primer codigo de inicio */
    while (i + 3 <= n) {
        uint32_t a = i + 3, b = a;
        while (b + 3 <= n && !(es[b] == 0 && es[b + 1] == 0 && es[b + 2] == 1)) b++;
        if (b + 3 > n) b = n;                               /* ultimo NAL */
        uint32_t fin = b;
        while (fin > a && es[fin - 1] == 0) fin--;          /* ceros finales y el 00 de un codigo de 4 bytes */
        if (fin > a) {
            uint8_t t = es[a] & 31;
            if (t == 7 && !h->sps_n && fin - a <= sizeof(h->sps)) { memcpy(h->sps, es + a, fin - a); h->sps_n = fin - a; }
            if (t == 8 && !h->pps_n && fin - a <= sizeof(h->pps)) { memcpy(h->pps, es + a, fin - a); h->pps_n = fin - a; }
            if (t == 5) idr = 1;
            if (t != 9) { hm_32(h, fin - a); hm_bytes(h, es + a, fin - a); }   /* sin AUD */
        }
        if (b >= n) break;
        i = b;
    }
    if (h->pos == ini) return;
    if (!h->video_dts0_ok) { h->video_dts0 = dts; h->video_dts0_ok = 1; }
    struct hm_pista *p = &h->v;
    p->off[p->n] = ini; p->tam[p->n] = (uint32_t)(h->pos - ini);
    p->dts[p->n] = dts; p->cts[p->n] = (int32_t)(pts - dts); p->sinc[p->n] = (uint8_t)idr;
    p->n++;
}
static uint64_t hm_marca(const uint8_t *p) {      /* PTS/DTS de 33 bits en 5 bytes */
    return ((uint64_t)(p[0] & 0x0E) << 29) | ((uint64_t)p[1] << 22) | ((uint64_t)(p[2] & 0xFE) << 14) |
           ((uint64_t)p[3] << 7) | ((uint64_t)p[4] >> 1);
}
static void hm_pes_cerrar(struct hm *h) {
    if (!h->pes_abierto || h->pes_n < 9) { h->pes_abierto = 0; h->pes_n = 0; return; }
    const uint8_t *p = h->pes;
    h->pes_abierto = 0;
    if (p[0] || p[1] || p[2] != 1) { h->pes_n = 0; return; }
    uint32_t hl = 9u + p[8];
    if (hl > h->pes_n) { h->pes_n = 0; return; }
    uint8_t fl = p[7] >> 6;
    uint64_t pts = 0, dts = 0;
    if (fl & 2) pts = hm_marca(p + 9);
    dts = (fl == 3) ? hm_marca(p + 14) : pts;
    hm_muestra_video(h, p + hl, h->pes_n - hl, pts, dts);
    h->pes_n = 0;
}
static int hm_ts(struct hm *h, const uint8_t *d, uint64_t n) {
    for (uint64_t k = 0; k + 188 <= n; k += 188) {
        const uint8_t *t = d + k;
        if (t[0] != 0x47) { h->error = 3; return -1; }
        int pusi = (t[1] >> 6) & 1, pid = ((t[1] & 0x1F) << 8) | t[2], afc = (t[3] >> 4) & 3;
        uint32_t i = 4;
        if (afc == 2) continue;                         /* solo campo de adaptacion */
        if (afc == 3) i += 1u + t[4];
        if (i >= 188) continue;
        const uint8_t *q = t + i; uint32_t m = 188 - i;
        if (pid == 0 || pid == h->pid_pmt) {            /* PAT / PMT (secciones cortas, en un paquete) */
            if (!pusi || m < 1) continue;
            uint32_t s = 1u + q[0];
            if (s + 8 > m) continue;
            const uint8_t *sec = q + s;
            uint32_t largo = ((sec[1] & 0x0F) << 8) | sec[2];
            if (3 + largo > m - s) continue;
            if (pid == 0 && sec[0] == 0x00) {
                for (uint32_t j = 8; j + 4 <= 3 + largo - 4; j += 4) {
                    int prog = (sec[j] << 8) | sec[j + 1];
                    if (prog) { h->pid_pmt = ((sec[j + 2] & 0x1F) << 8) | sec[j + 3]; break; }
                }
            } else if (sec[0] == 0x02) {
                uint32_t info = ((sec[10] & 0x0F) << 8) | sec[11];
                for (uint32_t j = 12 + info; j + 5 <= 3 + largo - 4; ) {
                    uint8_t tipo = sec[j]; int epid = ((sec[j + 1] & 0x1F) << 8) | sec[j + 2];
                    uint32_t es_info = ((sec[j + 3] & 0x0F) << 8) | sec[j + 4];
                    if (tipo == 0x1B && h->pid_video < 0) h->pid_video = epid;
                    j += 5 + es_info;
                }
            }
            continue;
        }
        if (pid != h->pid_video) continue;
        if (pusi) { hm_pes_cerrar(h); h->pes_abierto = 1; }
        if (!h->pes_abierto) continue;
        if (h->pes_n + m > h->pes_cap) {
            uint32_t c = h->pes_cap ? h->pes_cap * 2 : (1u << 20);
            while (c < h->pes_n + m) c *= 2;
            uint8_t *x = HM_REALLOC(h->pes, c);
            if (!x) { h->error = 2; return -1; }
            h->pes = x; h->pes_cap = c;
        }
        memcpy(h->pes + h->pes_n, q, m); h->pes_n += m;
    }
    hm_pes_cerrar(h);                                   /* el segmento termina en frontera de PES */
    return h->error ? -1 : 0;
}

/* ------------------------------------------------------------------ audio: ID3 + ADTS -> AAC crudo */
static const uint32_t hm_frecs[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
static int hm_aac(struct hm *h, const uint8_t *d, uint64_t n) {
    uint64_t i = 0;
    while (i + 10 <= n && d[i] == 'I' && d[i + 1] == 'D' && d[i + 2] == '3') {
        uint32_t t = ((uint32_t)(d[i + 6] & 0x7F) << 21) | ((uint32_t)(d[i + 7] & 0x7F) << 14) | ((uint32_t)(d[i + 8] & 0x7F) << 7) | (d[i + 9] & 0x7F);
        uint64_t fin = i + 10 + t + ((d[i + 5] & 0x10) ? 10 : 0);
        /* PRIV com.apple.streaming.transportStreamTimestamp: 8 bytes con la marca de 33 bits a 90 kHz */
        for (uint64_t j = i + 10; j + 10 + 53 <= fin && j + 10 + 53 <= n; ) {
            uint32_t fs = ((uint32_t)d[j + 4] << 24) | ((uint32_t)d[j + 5] << 16) | ((uint32_t)d[j + 6] << 8) | d[j + 7];
            if (d[j] == 'P' && d[j + 1] == 'R' && d[j + 2] == 'I' && d[j + 3] == 'V' && fs >= 53 && !h->audio_ts0_ok) {
                const uint8_t *m = d + j + 10 + 45;
                h->audio_ts0 = ((uint64_t)(m[3] & 1) << 32) | ((uint64_t)m[4] << 24) | ((uint64_t)m[5] << 16) | ((uint64_t)m[6] << 8) | m[7];
                h->audio_ts0_ok = 1;
            }
            if (!fs) break;
            j += 10 + fs;
        }
        i = fin;
    }
    while (i + 7 <= n) {
        if (d[i] != 0xFF || (d[i + 1] & 0xF0) != 0xF0) { i++; continue; }
        int sin_crc = d[i + 1] & 1;
        uint32_t perfil = (d[i + 2] >> 6) & 3, fi = (d[i + 2] >> 2) & 15, ch = ((d[i + 2] & 1) << 2) | (d[i + 3] >> 6);
        uint32_t largo = ((uint32_t)(d[i + 3] & 3) << 11) | ((uint32_t)d[i + 4] << 3) | (d[i + 5] >> 5);
        uint32_t cab = sin_crc ? 7 : 9;
        if (largo < cab || i + largo > n || fi > 12) break;
        if (!h->frecuencia) { h->frecuencia = hm_frecs[fi]; h->idx_frec = fi; h->canales = ch; h->objeto = perfil + 1; }
        if (hm_crecer(&h->a)) { h->error = 2; return -1; }
        uint64_t ini = h->pos;
        hm_bytes(h, d + i + cab, largo - cab);
        struct hm_pista *p = &h->a;
        p->off[p->n] = ini; p->tam[p->n] = largo - cab; p->dts[p->n] = h->audio_muestras; p->cts[p->n] = 0; p->sinc[p->n] = 1;
        p->n++;
        h->audio_muestras += 1024;
        i += largo;
    }
    return h->error ? -1 : 0;
}

/* ------------------------------------------------------------------ moov */
static void hm_matriz(struct hm *h) {
    static const uint32_t m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };
    for (int i = 0; i < 9; i++) hm_32(h, m[i]);
}
static uint64_t hm_duracion(const struct hm_pista *p) {
    if (!p->n) return 0;
    if (p->n == 1) return 1;
    uint64_t ult = p->dts[p->n - 1] - p->dts[p->n - 2];
    return p->dts[p->n - 1] - p->dts[0] + ult;
}
static void hm_trak(struct hm *h, struct hm_pista *p, int video, uint32_t escala, uint32_t ancho, uint32_t alto) {
    uint64_t dur = hm_duracion(p);
    uint64_t trak = hm_abrir(h, "trak");
    uint64_t b = hm_abrir(h, "tkhd");
    hm_32(h, 7); hm_32(h, 0); hm_32(h, 0); hm_32(h, video ? 1 : 2); hm_32(h, 0);
    hm_32(h, (uint32_t)(dur * 1000 / escala)); hm_ceros(h, 8); hm_16(h, 0); hm_16(h, 0);
    hm_16(h, video ? 0 : 0x100); hm_16(h, 0); hm_matriz(h);
    hm_32(h, video ? ancho << 16 : 0); hm_32(h, video ? alto << 16 : 0);
    hm_cerrar(h, b);
    uint64_t mdia = hm_abrir(h, "mdia");
    b = hm_abrir(h, "mdhd"); hm_32(h, 0); hm_32(h, 0); hm_32(h, 0); hm_32(h, escala); hm_32(h, (uint32_t)dur); hm_16(h, 0x55C4); hm_16(h, 0); hm_cerrar(h, b);
    b = hm_abrir(h, "hdlr"); hm_32(h, 0); hm_32(h, 0); hm_tipo(h, video ? "vide" : "soun"); hm_ceros(h, 12); hm_b(h, 0); hm_cerrar(h, b);
    uint64_t minf = hm_abrir(h, "minf");
    if (video) { b = hm_abrir(h, "vmhd"); hm_32(h, 1); hm_ceros(h, 8); hm_cerrar(h, b); }
    else { b = hm_abrir(h, "smhd"); hm_32(h, 0); hm_32(h, 0); hm_cerrar(h, b); }
    uint64_t dinf = hm_abrir(h, "dinf"); b = hm_abrir(h, "dref"); hm_32(h, 0); hm_32(h, 1);
    uint64_t u = hm_abrir(h, "url "); hm_32(h, 1); hm_cerrar(h, u); hm_cerrar(h, b); hm_cerrar(h, dinf);
    uint64_t stbl = hm_abrir(h, "stbl");
    b = hm_abrir(h, "stsd"); hm_32(h, 0); hm_32(h, 1);
    if (video) {
        uint64_t e = hm_abrir(h, "avc1");
        hm_ceros(h, 6); hm_16(h, 1); hm_ceros(h, 16); hm_16(h, ancho); hm_16(h, alto);
        hm_32(h, 0x480000); hm_32(h, 0x480000); hm_32(h, 0); hm_16(h, 1); hm_ceros(h, 32); hm_16(h, 0x18); hm_16(h, 0xFFFF);
        uint64_t c = hm_abrir(h, "avcC");
        hm_b(h, 1); hm_b(h, h->sps_n > 3 ? h->sps[1] : 77); hm_b(h, h->sps_n > 3 ? h->sps[2] : 0); hm_b(h, h->sps_n > 3 ? h->sps[3] : 31);
        hm_b(h, 0xFF); hm_b(h, 0xE1); hm_16(h, h->sps_n); hm_bytes(h, h->sps, h->sps_n);
        hm_b(h, 1); hm_16(h, h->pps_n); hm_bytes(h, h->pps, h->pps_n);
        hm_cerrar(h, c); hm_cerrar(h, e);
    } else {
        uint64_t e = hm_abrir(h, "mp4a");
        hm_ceros(h, 6); hm_16(h, 1); hm_ceros(h, 8); hm_16(h, h->canales); hm_16(h, 16); hm_16(h, 0); hm_16(h, 0); hm_32(h, h->frecuencia << 16);
        uint64_t es = hm_abrir(h, "esds"); hm_32(h, 0);
        hm_b(h, 3); hm_b(h, 25); hm_16(h, 0); hm_b(h, 0);                 /* ES_Descriptor */
        hm_b(h, 4); hm_b(h, 17); hm_b(h, 0x40); hm_b(h, 0x15); hm_b(h, 0); hm_16(h, 0); hm_32(h, 0); hm_32(h, 0);
        hm_b(h, 5); hm_b(h, 2);                                            /* AudioSpecificConfig */
        hm_b(h, (uint8_t)((h->objeto << 3) | (h->idx_frec >> 1))); hm_b(h, (uint8_t)(((h->idx_frec & 1) << 7) | (h->canales << 3)));
        hm_b(h, 6); hm_b(h, 1); hm_b(h, 2);                                /* SLConfig */
        hm_cerrar(h, es); hm_cerrar(h, e);
    }
    hm_cerrar(h, b);
    /* stts: duraciones por diferencia de DTS, en tramos */
    b = hm_abrir(h, "stts"); hm_32(h, 0);
    uint64_t pos_n = h->pos; hm_32(h, 0); uint32_t tramos = 0;
    for (uint32_t i = 0; i < p->n; ) {
        uint32_t d = i + 1 < p->n ? (uint32_t)(p->dts[i + 1] - p->dts[i]) : (i ? (uint32_t)(p->dts[i] - p->dts[i - 1]) : 1);
        uint32_t k = 1;
        while (i + k < p->n) {
            uint32_t d2 = i + k + 1 < p->n ? (uint32_t)(p->dts[i + k + 1] - p->dts[i + k]) : d;
            if (d2 != d) break;
            k++;
        }
        hm_32(h, k); hm_32(h, d); tramos++; i += k;
    }
    if (pos_n + 4 <= h->cap) { h->o[pos_n] = (uint8_t)(tramos >> 24); h->o[pos_n + 1] = (uint8_t)(tramos >> 16); h->o[pos_n + 2] = (uint8_t)(tramos >> 8); h->o[pos_n + 3] = (uint8_t)tramos; }
    hm_cerrar(h, b);
    if (video) {
        int32_t minimo = 0;
        for (uint32_t i = 0; i < p->n; i++) if (p->cts[i] < minimo) minimo = p->cts[i];
        b = hm_abrir(h, "ctts"); hm_32(h, 0);
        pos_n = h->pos; hm_32(h, 0); tramos = 0;
        for (uint32_t i = 0; i < p->n; ) {
            uint32_t k = 1;
            while (i + k < p->n && p->cts[i + k] == p->cts[i]) k++;
            hm_32(h, k); hm_32(h, (uint32_t)(p->cts[i] - minimo)); tramos++; i += k;
        }
        if (pos_n + 4 <= h->cap) { h->o[pos_n] = (uint8_t)(tramos >> 24); h->o[pos_n + 1] = (uint8_t)(tramos >> 16); h->o[pos_n + 2] = (uint8_t)(tramos >> 8); h->o[pos_n + 3] = (uint8_t)tramos; }
        hm_cerrar(h, b);
        uint32_t ns = 0; for (uint32_t i = 0; i < p->n; i++) ns += p->sinc[i];
        b = hm_abrir(h, "stss"); hm_32(h, 0); hm_32(h, ns);
        for (uint32_t i = 0; i < p->n; i++) if (p->sinc[i]) hm_32(h, i + 1);
        hm_cerrar(h, b);
    }
    b = hm_abrir(h, "stsc"); hm_32(h, 0); hm_32(h, 1); hm_32(h, 1); hm_32(h, 1); hm_32(h, 1); hm_cerrar(h, b);
    b = hm_abrir(h, "stsz"); hm_32(h, 0); hm_32(h, 0); hm_32(h, p->n); for (uint32_t i = 0; i < p->n; i++) hm_32(h, p->tam[i]); hm_cerrar(h, b);
    b = hm_abrir(h, "co64"); hm_32(h, 0); hm_32(h, p->n); for (uint32_t i = 0; i < p->n; i++) hm_64(h, p->off[i]); hm_cerrar(h, b);
    hm_cerrar(h, stbl); hm_cerrar(h, minf); hm_cerrar(h, mdia); hm_cerrar(h, trak);
}
/* Cierra el mdat y escribe el moov. Devuelve los bytes del MP4 o 0 si algo fallo. */
static uint64_t hm_terminar(struct hm *h, uint32_t ancho, uint32_t alto) {
    if (h->error || !h->v.n || !h->sps_n || !h->pps_n) return 0;
    uint64_t largo = h->pos - 32;                            /* ftyp de 32 bytes; el mdat va de 32 al final de las muestras */
    for (int i = 0; i < 8; i++) h->o[40 + i] = (uint8_t)(largo >> (56 - 8 * i));
    uint64_t dv = hm_duracion(&h->v) * 1000 / 90000, da = h->a.n && h->frecuencia ? hm_duracion(&h->a) * 1000 / h->frecuencia : 0;
    uint64_t moov = hm_abrir(h, "moov");
    uint64_t b = hm_abrir(h, "mvhd");
    hm_32(h, 0); hm_32(h, 0); hm_32(h, 0); hm_32(h, 1000); hm_32(h, (uint32_t)(dv > da ? dv : da));
    hm_32(h, 0x10000); hm_16(h, 0x100); hm_ceros(h, 10); hm_matriz(h); hm_ceros(h, 24); hm_32(h, 3);
    hm_cerrar(h, b);
    hm_trak(h, &h->v, 1, 90000, ancho, alto);
    if (h->a.n && h->frecuencia) hm_trak(h, &h->a, 0, h->frecuencia, 0, 0);
    hm_cerrar(h, moov);
    return h->error ? 0 : h->pos;
}

#endif
