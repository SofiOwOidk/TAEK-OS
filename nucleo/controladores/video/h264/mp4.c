#include "interno.h"

#define TIPO(a,b,c,d) ((uint32_t)(a)<<24 | (uint32_t)(b)<<16 | (uint32_t)(c)<<8 | (d))
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p)<<32 | be32(p+4); }
static unsigned be16(const uint8_t *p) { return (unsigned)p[0]<<8 | p[1]; }
typedef struct { const uint8_t *p; size_t n; } vista;

/* ISO BMFF: cada caja queda limitada por su contenedor, incluso con largesize. */
static int caja(vista *resto, uint32_t *tipo, vista *contenido) {
    if (!resto->n) return 0;
    if (resto->n < 8) return -1;
    uint64_t n = be32(resto->p);
    size_t cab = 8;
    *tipo = be32(resto->p+4);
    if (n == 1) {
        if (resto->n < 16) return -1;
        n = be64(resto->p+8); cab = 16;
    } else if (!n) n = resto->n;
    if (n < cab || n > resto->n) return -1;
    contenido->p = resto->p + cab;
    contenido->n = (size_t)n - cab;
    resto->p += (size_t)n; resto->n -= (size_t)n;
    return 1;
}

static int buscar(vista padre, uint32_t tipo, vista *hijo) {
    uint32_t t;
    vista v;
    int r;
    while ((r = caja(&padre, &t, &v)) > 0)
        if (t == tipo) { *hijo = v; return 1; }
    return r;
}

static int tabla(vista v, unsigned paso) {
    return v.n >= 8 && !v.p[0] && be32(v.p+4) > 0 &&
           be32(v.p+4) <= (v.n-8)/paso;
}

static h264_resultado pista(h264_mp4 *m, vista trak) {
    vista mdia, hdlr, mdhd, minf, stbl, stsd, avcc, v;
    if (buscar(trak, TIPO('m','d','i','a'), &mdia) != 1 ||
        buscar(mdia, TIPO('h','d','l','r'), &hdlr) != 1 || hdlr.n < 12)
        return H264_DATOS_INVALIDOS;
    if (be32(hdlr.p+8) != TIPO('v','i','d','e')) return H264_NO_SOPORTADO;
    if (buscar(mdia, TIPO('m','d','h','d'), &mdhd) != 1 || mdhd.n < 20)
        return H264_DATOS_INVALIDOS;
    if (mdhd.p[0] > 1 || (mdhd.p[0] == 1 && mdhd.n < 32)) return H264_DATOS_INVALIDOS;
    m->escala_tiempo = be32(mdhd.p + (mdhd.p[0] ? 20 : 12));
    if (!m->escala_tiempo) return H264_DATOS_INVALIDOS;
    if (buscar(mdia, TIPO('m','i','n','f'), &minf) != 1 ||
        buscar(minf, TIPO('s','t','b','l'), &stbl) != 1 ||
        buscar(stbl, TIPO('s','t','s','d'), &stsd) != 1 || stsd.n < 8)
        return H264_DATOS_INVALIDOS;
    /* Una descripción de muestra AVC. Cambios de códec requieren otro contexto. */
    if (stsd.p[0] || be32(stsd.p+4) != 1) return H264_NO_SOPORTADO;
    vista entradas = {stsd.p+8, stsd.n-8}, muestra;
    uint32_t tipo;
    if (caja(&entradas, &tipo, &muestra) != 1) return H264_DATOS_INVALIDOS;
    if (tipo != TIPO('a','v','c','1') && tipo != TIPO('a','v','c','3'))
        return H264_NO_SOPORTADO;
    if (muestra.n < 78) return H264_DATOS_INVALIDOS;
    vista extensiones = {muestra.p+78, muestra.n-78};
    if (buscar(extensiones, TIPO('a','v','c','C'), &avcc) != 1 || avcc.n < 7)
        return H264_DATOS_INVALIDOS;
    if (avcc.p[0] != 1 || (avcc.p[4]&3) == 2) return H264_NO_SOPORTADO;
    m->avcc = avcc.p; m->avcc_bytes = avcc.n;
    m->longitud_nal = (avcc.p[4]&3)+1;
    if (buscar(stbl, TIPO('s','t','s','z'), &v) != 1 || v.n < 12 || v.p[0])
        return H264_DATOS_INVALIDOS;
    m->stsz = v.p; m->stsz_bytes = v.n; m->muestras = be32(v.p+8);
    if (!m->muestras || (!be32(v.p+4) && m->muestras > (v.n-12)/4))
        return H264_DATOS_INVALIDOS;
    if (buscar(stbl, TIPO('s','t','s','c'), &v) != 1 || !tabla(v,12))
        return H264_DATOS_INVALIDOS;
    m->stsc = v.p; m->stsc_bytes = v.n;
    int r = buscar(stbl, TIPO('s','t','c','o'), &v);
    m->offsets_64 = 0;
    if (!r) { r = buscar(stbl, TIPO('c','o','6','4'), &v); m->offsets_64 = 1; }
    if (r != 1 || !tabla(v, m->offsets_64 ? 8 : 4)) return H264_DATOS_INVALIDOS;
    m->stco = v.p; m->stco_bytes = v.n;
    if (buscar(stbl, TIPO('s','t','t','s'), &v) != 1 || !tabla(v,8))
        return H264_DATOS_INVALIDOS;
    m->stts = v.p; m->stts_bytes = v.n;
    r = buscar(stbl, TIPO('c','t','t','s'), &v);
    if (r < 0) return H264_DATOS_INVALIDOS;
    m->ctts = NULL; m->ctts_bytes = 0;
    if (r) {
        if (v.n < 8 || v.p[0] > 1 || !be32(v.p+4) || be32(v.p+4) > (v.n-8)/8)
            return H264_DATOS_INVALIDOS;
        m->ctts = v.p; m->ctts_bytes = v.n;
    }
    /* Validar reglas de chunks y contadores antes de exponer muestras. */
    uint32_t chunks = be32(m->stco+4), reglas = be32(m->stsc+4);
    uint64_t cuenta = 0;
    for (uint32_t i = 0; i < reglas; ++i) {
        const uint8_t *e = m->stsc+8+(size_t)i*12;
        uint32_t primero = be32(e), siguientes = i+1 < reglas ? be32(e+12) : chunks+1;
        if ((!i && primero != 1) || !primero || primero > chunks ||
            siguientes <= primero || siguientes > (uint64_t)chunks+1 ||
            !be32(e+4) || be32(e+8) != 1) return H264_DATOS_INVALIDOS;
        cuenta += (uint64_t)(siguientes-primero)*be32(e+4);
        if (cuenta > m->muestras) return H264_DATOS_INVALIDOS;
    }
    if (cuenta != m->muestras) return H264_DATOS_INVALIDOS;
    for (unsigned t = 0; t < 2; t++) {
        const uint8_t *p = t ? m->ctts : m->stts;
        if (!p) continue;
        cuenta = 0;
        for (uint32_t i = 0; i < be32(p+4); i++) {
            uint32_t n = be32(p+8+(size_t)i*8);
            if (!n) return H264_DATOS_INVALIDOS;
            cuenta += n;
        }
        if (cuenta != m->muestras) return H264_DATOS_INVALIDOS;
    }
    return H264_OK;
}

h264_resultado h264_mp4_abrir(h264_mp4 *m, const void *datos, size_t bytes) {
    if (!m || !datos || bytes < 8) return H264_DATOS_INVALIDOS;
    h264_cero(m, sizeof(*m));
    vista todo = {datos,bytes}, moov, trak;
    if (buscar(todo, TIPO('m','o','o','v'), &moov) != 1) return H264_DATOS_INVALIDOS;
    uint32_t tipo;
    int r;
    h264_resultado ultimo = H264_NO_SOPORTADO;
    while ((r = caja(&moov,&tipo,&trak)) > 0) {
        if (tipo != TIPO('t','r','a','k')) continue;
        h264_mp4 candidato;
        h264_cero(&candidato, sizeof(candidato));
        ultimo = pista(&candidato, trak);
        if (ultimo == H264_OK) {
            *m = candidato; m->archivo = datos; m->bytes = bytes;
            return H264_OK;
        }
        if (ultimo != H264_NO_SOPORTADO) return ultimo;
    }
    return r < 0 ? H264_DATOS_INVALIDOS : ultimo;
}

h264_resultado h264_mp4_configurar(const h264_mp4 *m, h264_decodificador *d) {
    if (!m || !m->avcc || m->avcc_bytes < 7) return H264_DATOS_INVALIDOS;
    size_t pos = 6;
    unsigned n = m->avcc[5]&31;
    for (unsigned grupo = 0; grupo < 2; grupo++) {
        if (grupo) {
            if (pos >= m->avcc_bytes) return H264_DATOS_INVALIDOS;
            n = m->avcc[pos++];
        }
        for (unsigned i = 0; i < n; i++) {
            if (m->avcc_bytes-pos < 2) return H264_DATOS_INVALIDOS;
            unsigned tam = be16(m->avcc+pos); pos += 2;
            if (!tam || tam > m->avcc_bytes-pos) return H264_DATOS_INVALIDOS;
            h264_resultado r = h264_nal(d,m->avcc+pos,tam,0);
            if (r != H264_OK) return r;
            pos += tam;
        }
    }
    return H264_OK;
}

int h264_mp4_siguiente(h264_mp4 *m, const uint8_t **datos, size_t *bytes,
                      int64_t *presentacion, uint32_t *duracion) {
    if (!m || !m->archivo || !datos || !bytes || !presentacion || !duracion)
        return H264_DATOS_INVALIDOS;
    if (m->indice == m->muestras) return 0;
    if (!m->muestra_fragmento) {
        uint32_t n = be32(m->stco+4);
        if (m->fragmento >= n) return H264_DATOS_INVALIDOS;
        m->offset_muestra = m->offsets_64 ? be64(m->stco+8+(size_t)m->fragmento*8)
                                        : be32(m->stco+8+(size_t)m->fragmento*4);
        while (m->regla_fragmento+1 < be32(m->stsc+4) &&
               be32(m->stsc+8+(size_t)(m->regla_fragmento+1)*12) <= m->fragmento+1)
            m->regla_fragmento++;
    }
    uint32_t tam = be32(m->stsz+4);
    if (!tam) tam = be32(m->stsz+12+(size_t)m->indice*4);
    if (!tam || m->offset_muestra > m->bytes || tam > m->bytes-m->offset_muestra)
        return H264_DATOS_INVALIDOS;
    if (!m->restante_tiempo) {
        if (m->regla_tiempo >= be32(m->stts+4)) return H264_DATOS_INVALIDOS;
        m->restante_tiempo = be32(m->stts+8+(size_t)m->regla_tiempo*8);
    }
    uint32_t delta = be32(m->stts+12+(size_t)m->regla_tiempo*8);
    int64_t offset = 0;
    if (m->ctts) {
        if (!m->restante_composicion) {
            if (m->regla_composicion >= be32(m->ctts+4)) return H264_DATOS_INVALIDOS;
            m->restante_composicion = be32(m->ctts+8+(size_t)m->regla_composicion*8);
        }
        uint32_t u = be32(m->ctts+12+(size_t)m->regla_composicion*8);
        offset = m->ctts[0] ? (int64_t)(int32_t)u : (int64_t)u;
        if (!--m->restante_composicion) m->regla_composicion++;
    }
    if (m->tiempo_decodificacion > INT64_MAX-UINT32_MAX) return H264_LIMITE_EXCEDIDO;
    *datos = m->archivo+(size_t)m->offset_muestra;
    *bytes = tam;
    *presentacion = (int64_t)m->tiempo_decodificacion+offset;
    *duracion = delta;
    m->offset_muestra += tam; m->tiempo_decodificacion += delta;
    if (!--m->restante_tiempo) m->regla_tiempo++;
    m->indice++;
    if (++m->muestra_fragmento == be32(m->stsc+12+(size_t)m->regla_fragmento*12)) {
        m->muestra_fragmento = 0; m->fragmento++;
    }
    return 1;
}

h264_resultado h264_mp4_muestra(const h264_mp4 *m, h264_decodificador *d,
                               const uint8_t *datos, size_t bytes, int64_t tiempo) {
    if (!m || !datos || !m->longitud_nal || m->longitud_nal > 4) return H264_DATOS_INVALIDOS;
    while (bytes) {
        if (bytes < m->longitud_nal) return H264_DATOS_INVALIDOS;
        uint32_t tam = 0;
        for (unsigned i = 0; i < m->longitud_nal; i++) tam = tam<<8 | datos[i];
        datos += m->longitud_nal; bytes -= m->longitud_nal;
        if (!tam || tam > bytes) return H264_DATOS_INVALIDOS;
        h264_resultado r = h264_nal(d,datos,tam,tiempo);
        if (r != H264_OK) return r;
        datos += tam; bytes -= tam;
    }
    return H264_OK;
}
