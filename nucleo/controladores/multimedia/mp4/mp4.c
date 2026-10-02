#include "mp4.h"

#define TIPO(a,b,c,d) ((uint32_t)(a)<<24 | (uint32_t)(b)<<16 | (uint32_t)(c)<<8 | (d))

static inline void mp4_cero(void *p, size_t n) {
    uint8_t *q = (uint8_t *)p;
    while (n--) *q++ = 0;
}

static inline uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

static inline uint64_t be64(const uint8_t *p) {
    return (uint64_t)be32(p)<<32 | be32(p+4);
}

static inline unsigned be16(const uint8_t *p) {
    return (unsigned)p[0]<<8 | p[1];
}

typedef struct { const uint8_t *p; size_t n; } vista;

/* ISO BMFF: cada caja queda limitada por su contenedor, incluso con largesize. */
static int caja(vista *resto, uint32_t *tipo, vista *contenido) {
    if (!resto->n) return 0;
    if (resto->n < 8) return -1;
    uint64_t n = be32(resto->p);
    size_t cab = 8;
    *tipo = be32(resto->p + 4);
    if (n == 1) {
        if (resto->n < 16) return -1;
        n = be64(resto->p + 8);
        cab = 16;
    } else if (!n) {
        n = resto->n;
    }
    if (n < cab || n > resto->n) return -1;
    contenido->p = resto->p + cab;
    contenido->n = (size_t)n - cab;
    resto->p += (size_t)n;
    resto->n -= (size_t)n;
    return 1;
}

static int buscar(vista padre, uint32_t tipo, vista *hijo) {
    uint32_t t;
    vista v;
    int r;
    while ((r = caja(&padre, &t, &v)) > 0) {
        if (t == tipo) {
            *hijo = v;
            return 1;
        }
    }
    return r;
}

static int tabla(vista v, unsigned paso) {
    return v.n >= 8 && !v.p[0] && be32(v.p + 4) > 0 &&
           be32(v.p + 4) <= (v.n - 8) / paso;
}

static mp4_resultado pista_video(mp4_contenedor *m, vista trak) {
    vista mdia, hdlr, mdhd, minf, stbl, stsd, avcc, v;
    if (buscar(trak, TIPO('m','d','i','a'), &mdia) != 1 ||
        buscar(mdia, TIPO('h','d','l','r'), &hdlr) != 1 || hdlr.n < 12)
        return MP4_DATOS_INVALIDOS;
    if (be32(hdlr.p + 8) != TIPO('v','i','d','e')) return MP4_NO_SOPORTADO;
    if (buscar(mdia, TIPO('m','d','h','d'), &mdhd) != 1 || mdhd.n < 20)
        return MP4_DATOS_INVALIDOS;
    if (mdhd.p[0] > 1 || (mdhd.p[0] == 1 && mdhd.n < 32)) return MP4_DATOS_INVALIDOS;
    m->v_escala_tiempo = be32(mdhd.p + (mdhd.p[0] ? 20 : 12));
    if (!m->v_escala_tiempo) return MP4_DATOS_INVALIDOS;
    if (buscar(mdia, TIPO('m','i','n','f'), &minf) != 1 ||
        buscar(minf, TIPO('s','t','b','l'), &stbl) != 1 ||
        buscar(stbl, TIPO('s','t','s','d'), &stsd) != 1 || stsd.n < 8)
        return MP4_DATOS_INVALIDOS;

    /* Una descripción de muestra AVC. Cambios de códec requieren otro contexto. */
    if (stsd.p[0] || be32(stsd.p + 4) != 1) return MP4_NO_SOPORTADO;
    vista entradas = {stsd.p + 8, stsd.n - 8}, muestra;
    uint32_t tipo;
    if (caja(&entradas, &tipo, &muestra) != 1) return MP4_DATOS_INVALIDOS;
    if (tipo != TIPO('a','v','c','1') && tipo != TIPO('a','v','c','3'))
        return MP4_NO_SOPORTADO;
    if (muestra.n < 78) return MP4_DATOS_INVALIDOS;
    vista extensiones = {muestra.p + 78, muestra.n - 78};
    if (buscar(extensiones, TIPO('a','v','c','C'), &avcc) != 1 || avcc.n < 7)
        return MP4_DATOS_INVALIDOS;
    if (avcc.p[0] != 1 || (avcc.p[4] & 3) == 2) return MP4_NO_SOPORTADO;
    m->avcc = avcc.p;
    m->avcc_bytes = avcc.n;
    m->v_longitud_nal = (avcc.p[4] & 3) + 1;

    if (buscar(stbl, TIPO('s','t','s','z'), &v) != 1 || v.n < 12 || v.p[0])
        return MP4_DATOS_INVALIDOS;
    m->v_stsz = v.p;
    m->v_stsz_bytes = v.n;
    m->v_muestras = be32(v.p + 8);
    if (!m->v_muestras || (!be32(v.p + 4) && m->v_muestras > (v.n - 12) / 4))
        return MP4_DATOS_INVALIDOS;

    if (buscar(stbl, TIPO('s','t','s','c'), &v) != 1 || !tabla(v, 12))
        return MP4_DATOS_INVALIDOS;
    m->v_stsc = v.p;
    m->v_stsc_bytes = v.n;

    int r = buscar(stbl, TIPO('s','t','c','o'), &v);
    m->v_offsets_64 = 0;
    if (!r) {
        r = buscar(stbl, TIPO('c','o','6','4'), &v);
        m->v_offsets_64 = 1;
    }
    if (r != 1 || !tabla(v, m->v_offsets_64 ? 8 : 4)) return MP4_DATOS_INVALIDOS;
    m->v_stco = v.p;
    m->v_stco_bytes = v.n;

    if (buscar(stbl, TIPO('s','t','t','s'), &v) != 1 || !tabla(v, 8))
        return MP4_DATOS_INVALIDOS;
    m->v_stts = v.p;
    m->v_stts_bytes = v.n;

    r = buscar(stbl, TIPO('c','t','t','s'), &v);
    if (r < 0) return MP4_DATOS_INVALIDOS;
    m->v_ctts = NULL;
    m->v_ctts_bytes = 0;
    if (r) {
        if (v.n < 8 || v.p[0] > 1 || !be32(v.p + 4) || be32(v.p + 4) > (v.n - 8) / 8)
            return MP4_DATOS_INVALIDOS;
        m->v_ctts = v.p;
        m->v_ctts_bytes = v.n;
    }

    /* Validar consistencia de fragmentos y muestras */
    uint32_t chunks = be32(m->v_stco + 4), reglas = be32(m->v_stsc + 4);
    uint64_t cuenta = 0;
    for (uint32_t i = 0; i < reglas; ++i) {
        const uint8_t *e = m->v_stsc + 8 + (size_t)i * 12;
        uint32_t primero = be32(e), siguientes = (i + 1 < reglas) ? be32(e + 12) : chunks + 1;
        if ((!i && primero != 1) || !primero || primero > chunks ||
            siguientes <= primero || siguientes > (uint64_t)chunks + 1 ||
            !be32(e + 4) || be32(e + 8) != 1) return MP4_DATOS_INVALIDOS;
        cuenta += (uint64_t)(siguientes - primero) * be32(e + 4);
        if (cuenta > m->v_muestras) return MP4_DATOS_INVALIDOS;
    }
    if (cuenta != m->v_muestras) return MP4_DATOS_INVALIDOS;

    for (unsigned t = 0; t < 2; t++) {
        const uint8_t *p = t ? m->v_ctts : m->v_stts;
        if (!p) continue;
        cuenta = 0;
        for (uint32_t i = 0; i < be32(p + 4); i++) {
            uint32_t n = be32(p + 8 + (size_t)i * 8);
            if (!n) return MP4_DATOS_INVALIDOS;
            cuenta += n;
        }
        if (cuenta != m->v_muestras) return MP4_DATOS_INVALIDOS;
    }

    m->v_duracion_ticks = 0;
    for (uint32_t i = 0; i < be32(m->v_stts + 4); i++) {
        uint64_t n = be32(m->v_stts + 8 + (size_t)i * 8);
        uint64_t delta = be32(m->v_stts + 12 + (size_t)i * 8);
        if (delta && n > (UINT64_MAX - m->v_duracion_ticks) / delta) return MP4_LIMITE_EXCEDIDO;
        m->v_duracion_ticks += n * delta;
    }
    if (!m->v_duracion_ticks) return MP4_DATOS_INVALIDOS;

    m->tiene_video = 1;
    return MP4_OK;
}

static mp4_resultado pista_audio(mp4_contenedor *m, vista trak) {
    vista mdia, hdlr, mdhd, minf, stbl, stsd, v;
    if (buscar(trak, TIPO('m','d','i','a'), &mdia) != 1 ||
        buscar(mdia, TIPO('h','d','l','r'), &hdlr) != 1 || hdlr.n < 12)
        return MP4_DATOS_INVALIDOS;
    if (be32(hdlr.p + 8) != TIPO('s','o','u','n')) return MP4_NO_SOPORTADO;
    if (buscar(mdia, TIPO('m','d','h','d'), &mdhd) != 1 || mdhd.n < 20)
        return MP4_DATOS_INVALIDOS;
    if (mdhd.p[0] > 1 || (mdhd.p[0] == 1 && mdhd.n < 32)) return MP4_DATOS_INVALIDOS;
    m->a_escala_tiempo = be32(mdhd.p + (mdhd.p[0] ? 20 : 12));
    if (!m->a_escala_tiempo) return MP4_DATOS_INVALIDOS;
    if (buscar(mdia, TIPO('m','i','n','f'), &minf) != 1 ||
        buscar(minf, TIPO('s','t','b','l'), &stbl) != 1 ||
        buscar(stbl, TIPO('s','t','s','d'), &stsd) != 1 || stsd.n < 8)
        return MP4_DATOS_INVALIDOS;
    if (stsd.p[0] || be32(stsd.p + 4) < 1) return MP4_NO_SOPORTADO;

    vista entradas = {stsd.p + 8, stsd.n - 8}, muestra;
    uint32_t tipo;
    if (caja(&entradas, &tipo, &muestra) != 1) return MP4_DATOS_INVALIDOS;
    if (tipo != TIPO('m','p','4','a')) return MP4_NO_SOPORTADO;
    if (muestra.n < 28) return MP4_DATOS_INVALIDOS;

    m->a_canales = be16(muestra.p + 16);
    m->a_frecuencia = be32(muestra.p + 24) >> 16;
    if (!m->a_canales) m->a_canales = 2;
    if (!m->a_frecuencia) m->a_frecuencia = m->a_escala_tiempo;

    if (buscar(stbl, TIPO('s','t','s','z'), &v) != 1 || v.n < 12 || v.p[0])
        return MP4_DATOS_INVALIDOS;
    m->a_stsz = v.p;
    m->a_stsz_bytes = v.n;
    m->a_muestras = be32(v.p + 8);
    if (!m->a_muestras || (!be32(v.p + 4) && m->a_muestras > (v.n - 12) / 4))
        return MP4_DATOS_INVALIDOS;

    if (buscar(stbl, TIPO('s','t','s','c'), &v) != 1 || !tabla(v, 12))
        return MP4_DATOS_INVALIDOS;
    m->a_stsc = v.p;
    m->a_stsc_bytes = v.n;

    int r = buscar(stbl, TIPO('s','t','c','o'), &v);
    m->a_offsets_64 = 0;
    if (!r) {
        r = buscar(stbl, TIPO('c','o','6','4'), &v);
        m->a_offsets_64 = 1;
    }
    if (r != 1 || !tabla(v, m->a_offsets_64 ? 8 : 4)) return MP4_DATOS_INVALIDOS;
    m->a_stco = v.p;
    m->a_stco_bytes = v.n;

    if (buscar(stbl, TIPO('s','t','t','s'), &v) != 1 || !tabla(v, 8))
        return MP4_DATOS_INVALIDOS;
    m->a_stts = v.p;
    m->a_stts_bytes = v.n;

    uint32_t chunks = be32(m->a_stco + 4), reglas = be32(m->a_stsc + 4);
    uint64_t cuenta = 0;
    for (uint32_t i = 0; i < reglas; ++i) {
        const uint8_t *e = m->a_stsc + 8 + (size_t)i * 12;
        uint32_t primero = be32(e), siguientes = (i + 1 < reglas) ? be32(e + 12) : chunks + 1;
        if ((!i && primero != 1) || !primero || primero > chunks ||
            siguientes <= primero || siguientes > (uint64_t)chunks + 1 ||
            !be32(e + 4) || be32(e + 8) != 1) return MP4_DATOS_INVALIDOS;
        cuenta += (uint64_t)(siguientes - primero) * be32(e + 4);
        if (cuenta > m->a_muestras) return MP4_DATOS_INVALIDOS;
    }
    if (cuenta != m->a_muestras) return MP4_DATOS_INVALIDOS;

    m->tiene_audio = 1;
    return MP4_OK;
}

static mp4_resultado mp4_parsear_moov(mp4_contenedor *m, vista moov) {
    vista trak;
    uint32_t tipo;
    int r;
    while ((r = caja(&moov, &tipo, &trak)) > 0) {
        if (tipo != TIPO('t','r','a','k')) continue;
        if (!m->tiene_video) {
            if (pista_video(m, trak) == MP4_OK) {
                continue;
            }
        }
        if (!m->tiene_audio) {
            pista_audio(m, trak);
        }
    }

    if (!m->tiene_video && !m->tiene_audio) return MP4_NO_SOPORTADO;
    return MP4_OK;
}

mp4_resultado mp4_abrir(mp4_contenedor *m, const void *datos, size_t bytes) {
    if (!m || !datos || bytes < 8) return MP4_DATOS_INVALIDOS;
    mp4_cero(m, sizeof(*m));
    m->archivo = (const uint8_t *)datos;
    m->bytes = bytes;
    vista moov;
    if (buscar((vista){(const uint8_t *)datos, bytes}, TIPO('m','o','o','v'), &moov) != 1)
        return MP4_DATOS_INVALIDOS;
    return mp4_parsear_moov(m, moov);
}

static mp4_resultado mp4_leer_exacto(mp4_contenedor *m,int pista,uint64_t offset,void *destino,size_t cantidad){
    int64_t r=m->leer_fuente(m->fuente_contexto,pista,offset,destino,cantidad);
    if(r==(int64_t)cantidad)return MP4_OK;
    m->ultimo_error_lectura=r<0?r:MP4_ERROR_LECTURA;
    return MP4_ERROR_LECTURA;
}

mp4_resultado mp4_abrir_fuente(mp4_contenedor *m, uint64_t bytes,
                               mp4_lectura_posicional leer, void *contexto,
                               uint8_t *metadatos, size_t metadatos_capacidad,
                               uint8_t *muestra_video, size_t muestra_video_capacidad,
                               uint8_t *muestra_audio, size_t muestra_audio_capacidad) {
    if (!m || !leer || !metadatos || metadatos_capacidad < 8 || bytes < 8 ||
        !muestra_video || !muestra_video_capacidad || !muestra_audio || !muestra_audio_capacidad)
        return MP4_DATOS_INVALIDOS;
    mp4_cero(m, sizeof(*m));
    m->bytes = bytes;
    m->leer_fuente = leer;
    m->fuente_contexto = contexto;
    m->muestra_video_buffer = muestra_video;
    m->muestra_video_capacidad = muestra_video_capacidad;
    m->muestra_audio_buffer = muestra_audio;
    m->muestra_audio_capacidad = muestra_audio_capacidad;

    uint64_t offset = 0;
    while (offset <= bytes - 8) {
        uint8_t h[16];
        if (mp4_leer_exacto(m,MP4_FUENTE_VIDEO,offset,h,8)!=MP4_OK)return MP4_ERROR_LECTURA;
        uint64_t tam = be32(h);
        uint32_t t = be32(h + 4);
        size_t cabecera = 8;
        if (tam == 1) {
            if (bytes - offset < 16)return MP4_DATOS_INVALIDOS;
            if(mp4_leer_exacto(m,MP4_FUENTE_VIDEO,offset,h,16)!=MP4_OK)return MP4_ERROR_LECTURA;
            tam = be64(h + 8);
            cabecera = 16;
        } else if (tam == 0) {
            tam = bytes - offset;
        }
        if (tam < cabecera || tam > bytes - offset) return MP4_DATOS_INVALIDOS;
        if (t == TIPO('m','o','o','v')) {
            if (tam > metadatos_capacidad || tam > SIZE_MAX) return MP4_LIMITE_EXCEDIDO;
            if (mp4_leer_exacto(m,MP4_FUENTE_VIDEO,offset,metadatos,(size_t)tam)!=MP4_OK)
                return MP4_ERROR_LECTURA;
            m->metadatos = metadatos;
            m->metadatos_bytes = (size_t)tam;
            vista resto = {metadatos, (size_t)tam}, contenido;
            uint32_t tipo_moov;
            if (caja(&resto, &tipo_moov, &contenido) != 1 || tipo_moov != TIPO('m','o','o','v'))
                return MP4_DATOS_INVALIDOS;
            return mp4_parsear_moov(m, contenido);
        }
        offset += tam;
    }
    return MP4_DATOS_INVALIDOS;
}

static int mp4_leer_muestra(mp4_contenedor *m, int pista, uint64_t offset, size_t tam,
                            uint8_t *buffer, size_t capacidad, const uint8_t **datos) {
    if (offset > m->bytes || (uint64_t)tam > m->bytes - offset) return MP4_DATOS_INVALIDOS;
    if (m->solo_indice) { *datos = NULL; return 1; }
    if (m->leer_fuente) {
        if (!buffer || tam > capacidad) return MP4_LIMITE_EXCEDIDO;
        if (mp4_leer_exacto(m,pista,offset,buffer,tam)!=MP4_OK)
            return MP4_ERROR_LECTURA;
        *datos = buffer;
    } else {
        *datos = m->archivo + (size_t)offset;
    }
    return 1;
}

int mp4_tiene_video(const mp4_contenedor *m) {
    return m ? m->tiene_video : 0;
}

int mp4_tiene_audio(const mp4_contenedor *m) {
    return m ? m->tiene_audio : 0;
}

int mp4_siguiente_video(mp4_contenedor *m, const uint8_t **datos, size_t *bytes,
                        int64_t *presentacion, uint32_t *duracion) {
    if (!m || (!m->archivo && !m->leer_fuente) || !m->tiene_video || !datos || !bytes || !presentacion || !duracion)
        return MP4_DATOS_INVALIDOS;
    if (m->v_indice == m->v_muestras) return 0;

    if (!m->v_muestra_fragmento) {
        uint32_t n = be32(m->v_stco + 4);
        if (m->v_fragmento >= n) return MP4_DATOS_INVALIDOS;
        m->v_offset_muestra = m->v_offsets_64 ? be64(m->v_stco + 8 + (size_t)m->v_fragmento * 8)
                                             : be32(m->v_stco + 8 + (size_t)m->v_fragmento * 4);
        while (m->v_regla_fragmento + 1 < be32(m->v_stsc + 4) &&
               be32(m->v_stsc + 8 + (size_t)(m->v_regla_fragmento + 1) * 12) <= m->v_fragmento + 1)
            m->v_regla_fragmento++;
    }

    uint32_t tam = be32(m->v_stsz + 4);
    if (!tam) tam = be32(m->v_stsz + 12 + (size_t)m->v_indice * 4);
    if (!tam) return MP4_DATOS_INVALIDOS;

    if (!m->v_restante_tiempo) {
        if (m->v_regla_tiempo >= be32(m->v_stts + 4)) return MP4_DATOS_INVALIDOS;
        m->v_restante_tiempo = be32(m->v_stts + 8 + (size_t)m->v_regla_tiempo * 8);
    }
    uint32_t delta = be32(m->v_stts + 12 + (size_t)m->v_regla_tiempo * 8);

    int64_t offset = 0;
    if (m->v_ctts) {
        if (!m->v_restante_composicion) {
            if (m->v_regla_composicion >= be32(m->v_ctts + 4)) return MP4_DATOS_INVALIDOS;
            m->v_restante_composicion = be32(m->v_ctts + 8 + (size_t)m->v_regla_composicion * 8);
        }
        uint32_t u = be32(m->v_ctts + 12 + (size_t)m->v_regla_composicion * 8);
        offset = m->v_ctts[0] ? (int64_t)(int32_t)u : (int64_t)u;
        if (!--m->v_restante_composicion) m->v_regla_composicion++;
    }

    if (m->v_tiempo_decodificacion > INT64_MAX - UINT32_MAX) return MP4_LIMITE_EXCEDIDO;

    int lectura = mp4_leer_muestra(m, MP4_FUENTE_VIDEO, m->v_offset_muestra, tam, m->muestra_video_buffer,
                                   m->muestra_video_capacidad, datos);
    if (lectura < 0) return lectura;
    *bytes = tam;
    *presentacion = (int64_t)m->v_tiempo_decodificacion + offset;
    *duracion = delta;

    m->v_offset_muestra += tam;
    m->v_tiempo_decodificacion += delta;
    if (!--m->v_restante_tiempo) m->v_regla_tiempo++;
    m->v_indice++;
    if (++m->v_muestra_fragmento == be32(m->v_stsc + 12 + (size_t)m->v_regla_fragmento * 12)) {
        m->v_muestra_fragmento = 0;
        m->v_fragmento++;
    }
    return 1;
}

int mp4_siguiente_audio(mp4_contenedor *m, const uint8_t **datos, size_t *bytes, int64_t *tiempo) {
    if (!m || (!m->archivo && !m->leer_fuente) || !m->tiene_audio || !datos || !bytes || !tiempo)
        return MP4_DATOS_INVALIDOS;
    if (m->a_indice == m->a_muestras) return 0;

    if (!m->a_muestra_fragmento) {
        uint32_t n = be32(m->a_stco + 4);
        if (m->a_fragmento >= n) return MP4_DATOS_INVALIDOS;
        m->a_offset_muestra = m->a_offsets_64 ? be64(m->a_stco + 8 + (size_t)m->a_fragmento * 8)
                                             : be32(m->a_stco + 8 + (size_t)m->a_fragmento * 4);
        while (m->a_regla_fragmento + 1 < be32(m->a_stsc + 4) &&
               be32(m->a_stsc + 8 + (size_t)(m->a_regla_fragmento + 1) * 12) <= m->a_fragmento + 1)
            m->a_regla_fragmento++;
    }

    uint32_t tam = be32(m->a_stsz + 4);
    if (!tam) tam = be32(m->a_stsz + 12 + (size_t)m->a_indice * 4);
    if (!tam) return MP4_DATOS_INVALIDOS;

    if (!m->a_restante_tiempo) {
        if (m->a_regla_tiempo >= be32(m->a_stts + 4)) return MP4_DATOS_INVALIDOS;
        m->a_restante_tiempo = be32(m->a_stts + 8 + (size_t)m->a_regla_tiempo * 8);
    }
    uint32_t delta = be32(m->a_stts + 12 + (size_t)m->a_regla_tiempo * 8);

    int lectura = mp4_leer_muestra(m, MP4_FUENTE_AUDIO, m->a_offset_muestra, tam, m->muestra_audio_buffer,
                                   m->muestra_audio_capacidad, datos);
    if (lectura < 0) return lectura;
    *bytes = tam;
    *tiempo = (int64_t)m->a_tiempo;

    m->a_offset_muestra += tam;
    m->a_tiempo += delta;
    if (!--m->a_restante_tiempo) m->a_regla_tiempo++;
    m->a_indice++;
    if (++m->a_muestra_fragmento == be32(m->a_stsc + 12 + (size_t)m->a_regla_fragmento * 12)) {
        m->a_muestra_fragmento = 0;
        m->a_fragmento++;
    }
    return 1;
}

void mp4_rebobinar_video(mp4_contenedor *m) {
    if (!m) return;
    m->v_indice = 0;
    m->v_fragmento = 0;
    m->v_muestra_fragmento = 0;
    m->v_regla_fragmento = 0;
    m->v_regla_tiempo = 0;
    m->v_restante_tiempo = 0;
    m->v_regla_composicion = 0;
    m->v_restante_composicion = 0;
    m->v_offset_muestra = 0;
    m->v_tiempo_decodificacion = 0;
}

void mp4_rebobinar_audio(mp4_contenedor *m) {
    if (!m) return;
    m->a_indice = 0;
    m->a_fragmento = 0;
    m->a_muestra_fragmento = 0;
    m->a_regla_fragmento = 0;
    m->a_regla_tiempo = 0;
    m->a_restante_tiempo = 0;
    m->a_offset_muestra = 0;
    m->a_tiempo = 0;
}
