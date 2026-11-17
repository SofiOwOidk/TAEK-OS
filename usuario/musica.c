/* Reproductor de musica de NekoSaka (H146). 'ejecutar /musica.elf [ruta]'.
 *
 * Decodifica MP3 en el propio programa con minimp3 (la misma biblioteca que usa el nucleo), lleva el sonido a 44 100 Hz
 * estereo y se lo pasa al nucleo con SYS_AUDIO, que lo envia a la tarjeta (HDA o AC97). La interfaz sigue viva mientras
 * suena: lista, disco que gira, progreso y un ecualizador calculado del sonido que esta saliendo en ese momento.
 * Teclas: arriba/abajo elegir, Intro tocar, espacio pausa, izquierda/derecha -10 s / +10 s, N / P siguiente y anterior,
 * + / - volumen, Esc sale (el sonido se detiene al salir). */
#pragma clang diagnostic ignored "-Wunused-function"
#include "ui.h"
#include "memoria.h"
#include "apps.h"

TAEK_PROGRAMA

#define MINIMP3_NO_STD_HEADERS
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#include "nucleo/controladores/multimedia/mp3/minimp3.h"
#pragma clang diagnostic pop

#define FONDO   0xFF0B0E14u
#define PANEL   0xFF10141Du
#define BARRA   0xFF141924u
#define FILA_SEL 0xFF232C3Fu
#define ROJO    0xFF003Cu
#define NEON    0xFCEE0Au
#define CIAN    0x5BE0FFu
#define BLANCO  0xFFFFFFu
#define TEXTO   0xD8DCE6u
#define TENUE   0x8A93A6u

static int AN, AL;
static int S(int v) { return v * AL / 1080; }
static struct taek_superficie sup;
static struct lienzo L;
static long cuadro_panel;
#include "ciber.h"

/* ------------------------------------------------------------------ lista */

static const char *const EXT[] = { "mp3", 0 };
#define MAX_PISTAS 1024
/* H148: disco de cada pista (N + 1, o 0 el abierto): la lista junta los MP3 de todos los discos */
struct pista { char ruta[TAEK_RUTA]; char titulo[120]; char artista[80]; uint32_t disco; };
static struct pista *pistas;
static int npistas, sel, sonando = -1, primera;
/* Lo que se ve: las pistas que pasan el filtro de busqueda. 'sel' y 'primera' son posiciones en 'vis'. */
static int *vis, nvis;
static struct ap_busqueda busca;

static void filtrar(void) {
    nvis = 0;
    for (int i = 0; i < npistas; i++)
        if (ap_contiene(pistas[i].titulo, busca.t) || ap_contiene(pistas[i].artista, busca.t)) vis[nvis++] = i;
    if (sel >= nvis) sel = nvis ? nvis - 1 : 0;
    primera = 0;
}
/* Posicion en la lista visible de la pista i, o -1 */
static int visible(int i) { for (int k = 0; k < nvis; k++) if (vis[k] == i) return k; return -1; }

static void agregar_medios(const struct ap_medio *m, int n) {
    for (int i = 0; i < n && npistas < MAX_PISTAS; i++) {
        struct pista *p = &pistas[npistas++];
        memset(p, 0, sizeof(*p));
        ap_copiar(p->ruta, m[i].ruta, sizeof(p->ruta));
        ap_copiar(p->titulo, m[i].nombre, sizeof(p->titulo));
        p->disco = m[i].disco;
    }
}
static void agregar_carpeta(const char *carpeta) {
    struct ap_medio *m = malloc(sizeof(*m) * MAX_PISTAS);
    if (!m) return;
    int n = 0, carpetas = 2;
    ap_recorrer(EXT, m, &n, MAX_PISTAS, carpeta, 2, &carpetas);
    for (int i = 1; i < n; i++) {   /* Por nombre */
        struct ap_medio t = m[i];
        int j = i;
        while (j > 0 && ap_comparar(m[j - 1].nombre, t.nombre) > 0) { m[j] = m[j - 1]; j--; }
        m[j] = t;
    }
    agregar_medios(m, n);
    free(m);
}
static void agregar_todos(void) {
    struct ap_medio *m = malloc(sizeof(*m) * MAX_PISTAS);
    if (!m) return;
    agregar_medios(m, ap_escanear(EXT, m, MAX_PISTAS));
    free(m);
}

/* ------------------------------------------------------------------ ID3v2: titulo y artista */

/* Texto de un cuadro ID3 a UTF-8 de 1 o 2 bytes (lo que tiene la fuente) */
static void id3_texto(const uint8_t *d, uint32_t n, char *out, int cap) {
    if (!n) return;
    int enc = d[0], k = 0;
    d++; n--;
    if (enc == 1 || enc == 2) {   /* UTF-16, con o sin BOM */
        int be = enc == 2;
        if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) { be = 0; d += 2; n -= 2; }
        else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) { be = 1; d += 2; n -= 2; }
        for (uint32_t i = 0; i + 1 < n && k + 3 < cap; i += 2) {
            uint32_t c = be ? (uint32_t)d[i] << 8 | d[i + 1] : (uint32_t)d[i + 1] << 8 | d[i];
            if (!c) break;
            if (c < 0x80) out[k++] = (char)c;
            else if (c < 0x100) { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
            else out[k++] = '?';
        }
    } else {
        for (uint32_t i = 0; i < n && k + 3 < cap; i++) {
            uint32_t c = d[i];
            if (!c) break;
            if (enc == 3 || c < 0x80) out[k++] = (char)c;   /* UTF-8 tal cual */
            else { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
        }
    }
    out[k] = 0;
}

/* Lee las etiquetas y devuelve donde empieza el audio */
static uint64_t leer_id3(struct pista *p, uint8_t *b, uint64_t n) {
    if (n < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    int ver = b[3];
    uint32_t tam = (uint32_t)(b[6] & 127) << 21 | (uint32_t)(b[7] & 127) << 14 | (uint32_t)(b[8] & 127) << 7 | (b[9] & 127);
    uint64_t fin = 10 + (uint64_t)tam + ((b[5] & 0x10) ? 10 : 0);
    uint64_t i = 10;
    while (i + 10 <= n && i + 10 <= 10 + (uint64_t)tam) {
        const uint8_t *f = b + i;
        if (!f[0]) break;
        uint32_t ft = ver >= 4 ? (uint32_t)(f[4] & 127) << 21 | (uint32_t)(f[5] & 127) << 14 | (uint32_t)(f[6] & 127) << 7 | (f[7] & 127)
                               : (uint32_t)f[4] << 24 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 8 | f[7];
        if (!ft || i + 10 + ft > n) break;
        if (!memcmp(f, "TIT2", 4)) { char t[120] = ""; id3_texto(f + 10, ft, t, sizeof(t)); if (t[0]) ap_copiar(p->titulo, t, sizeof(p->titulo)); }
        if (!memcmp(f, "TPE1", 4)) id3_texto(f + 10, ft, p->artista, sizeof(p->artista));
        i += 10 + ft;
    }
    return fin;
}

/* ------------------------------------------------------------------ reproduccion */

#define ENT_MAX (64 * 1024)
#define PEND_MAX (MINIMP3_MAX_SAMPLES_PER_FRAME * 8)
#define ANILLO_CUADROS (TAEK_AUDIO_HZ * 4)   /* 4 s de lo ya decodificado, para el ecualizador */

static struct {
    mp3dec_t dec;
    uint8_t ent[ENT_MAX]; uint32_t ent_n;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t pend[PEND_MAX]; uint32_t pend_n, pend_i;   /* Ya a 44,1 kHz, esperando sitio en la cola del nucleo */
    uint64_t lectura, inicio, fin, total;
    uint64_t fase;
    int hz, kbps, iniciado, archivo_acabo, pausa;
    uint64_t base_ms, duracion_ms, pausa_ms;
    uint64_t cuadros;                                   /* Cuadros de 44,1 kHz decodificados desde base_ms */
} R;
static int16_t *eco;                                 /* Estereo, ANILLO_CUADROS */
static struct taek_audio au;
static int hay_audio = 1, volumen = 0;
static char aviso[120];

static long audio(uint32_t op) { au.op = op; return taek_audio(&au); }

static void detener(void) {
    audio(TAEK_AU_DETENER);
    R.iniciado = 0; R.pend_n = R.pend_i = 0; R.ent_n = 0; R.archivo_acabo = 0;
}

static void ir_a(uint64_t ms) {
    detener();
    mp3dec_init(&R.dec);
    if (R.duracion_ms && ms >= R.duracion_ms) ms = R.duracion_ms ? R.duracion_ms - 1000 : 0;
    uint64_t byte = R.kbps ? ms * (uint64_t)R.kbps / 8 : 0;
    R.lectura = R.inicio + byte;
    if (R.lectura >= R.fin) R.lectura = R.inicio;
    R.base_ms = ms; R.cuadros = 0; R.fase = 0;
}

static void tocar(int i) {
    if (i < 0 || i >= npistas) return;
    detener();
    memset(&R, 0, sizeof(R));
    mp3dec_init(&R.dec);
    sonando = i;
    if (visible(i) >= 0) sel = visible(i);
    struct pista *p = &pistas[i];
    ap_disco = p->disco;
    static uint8_t cab[64 * 1024];
    long n = ap_leer(p->ruta, 0, cab, sizeof(cab), &R.total);
    if (n <= 0) { ap_copiar(aviso, "No se pudo leer la canci\xC3\xB3n.", sizeof(aviso)); sonando = -1; return; }
    R.inicio = leer_id3(p, cab, (uint64_t)n);
    R.fin = R.total;
    if (R.total > 128) {   /* ID3v1 al final */
        uint8_t t[3];
        if (ap_leer(p->ruta, R.total - 128, t, 3, NULL) == 3 && t[0] == 'T' && t[1] == 'A' && t[2] == 'G') R.fin -= 128;
    }
    R.lectura = R.inicio;
    aviso[0] = 0;
    ap_marca("MUSICA", "tocando", p->ruta);
}

/* Uno o mas cuadros MP3 hasta llenar la espera; 0 si se acabo el archivo */
static int decodificar(void) {
    struct pista *p = &pistas[sonando];
    while (R.pend_n + MINIMP3_MAX_SAMPLES_PER_FRAME * 4 < PEND_MAX) {
        if (R.ent_n < 16 * 1024 && R.lectura < R.fin) {
            uint64_t quiero = ENT_MAX - R.ent_n;
            if (quiero > R.fin - R.lectura) quiero = R.fin - R.lectura;
            long r = ap_leer(p->ruta, R.lectura, R.ent + R.ent_n, quiero, NULL);
            if (r <= 0) R.lectura = R.fin;
            else { R.ent_n += (uint32_t)r; R.lectura += (uint64_t)r; }
        }
        if (R.ent_n < 4) return 0;
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int m = mp3dec_decode_frame(&R.dec, R.ent, (int)R.ent_n, R.pcm, &info);
        if (info.frame_bytes > 0 && (uint32_t)info.frame_bytes <= R.ent_n) {
            R.ent_n -= (uint32_t)info.frame_bytes;
            memmove(R.ent, R.ent + info.frame_bytes, R.ent_n);
        } else if (R.lectura >= R.fin) return 0;
        else { memmove(R.ent, R.ent + 1, --R.ent_n); continue; }
        if (m <= 0 || (info.channels != 1 && info.channels != 2) || info.hz < 8000 || info.hz > 48000) continue;
        if (!R.hz) {
            R.hz = info.hz; R.kbps = info.bitrate_kbps;
            if (R.kbps) R.duracion_ms = (R.fin - R.inicio) * 8 / (uint64_t)R.kbps;
        }
        /* A 44,1 kHz con interpolacion lineal, como el reproductor del nucleo */
        uint64_t limite = (uint64_t)m * TAEK_AUDIO_HZ;
        while (R.fase < limite && R.pend_n + 2 <= PEND_MAX) {
            uint32_t i0 = (uint32_t)(R.fase / TAEK_AUDIO_HZ), fr = (uint32_t)(R.fase % TAEK_AUDIO_HZ);
            uint32_t i1 = i0 + 1 < (uint32_t)m ? i0 + 1 : i0;
            for (int c = 0; c < 2; c++) {
                int ch = info.channels == 2 ? c : 0;
                int32_t a = R.pcm[i0 * info.channels + ch], b = R.pcm[i1 * info.channels + ch];
                int16_t v = (int16_t)(a + (int32_t)(((int64_t)(b - a) * fr) / TAEK_AUDIO_HZ));
                R.pend[R.pend_n++] = v;
                eco[(R.cuadros % ANILLO_CUADROS) * 2 + (uint64_t)c] = v;
            }
            R.cuadros++;
            R.fase += (uint32_t)info.hz;
        }
        R.fase = R.fase >= limite ? R.fase - limite : 0;
    }
    return 1;
}

/* Llamada en cada cuadro: mantener la cola del nucleo con sonido */
static void bombear(void) {
    if (sonando < 0 || R.pausa || !hay_audio) return;
    for (int vueltas = 0; vueltas < 8; vueltas++) {
        if (R.pend_i >= R.pend_n) {
            R.pend_n = R.pend_i = 0;
            if (R.archivo_acabo || !decodificar()) R.archivo_acabo = 1;
            if (!R.pend_n) break;
        }
        au.datos = (uint64_t)(uintptr_t)(R.pend + R.pend_i);
        au.bytes = (R.pend_n - R.pend_i) * 2;
        if (audio(TAEK_AU_ENCOLAR) < 0) { hay_audio = 0; ap_copiar(aviso, "Este equipo no tiene salida de sonido.", sizeof(aviso)); return; }
        R.pend_i += au.aceptados / 2;
        if (au.aceptados < au.bytes) break;   /* La cola esta llena */
    }
    if (!R.iniciado && (au.ocupada >= 64 * 1024 || R.archivo_acabo)) { audio(TAEK_AU_INICIAR); R.iniciado = 1; }
}

static uint64_t posicion_ms(void) {
    if (R.pausa) return R.pausa_ms;
    return R.base_ms + (R.iniciado ? au.tocado_ms : 0);
}

/* ------------------------------------------------------------------ dibujo */

static int LISTA_AN, FILA;
static float giro;
static float barras[32];
#define LISTA_Y0 S(244)

/* La capa de contenido es transparente: cada zona se vacia antes de dibujarla, y debajo queda el fondo de neon */
static void vaciar(int x, int y, int an, int al) {
    for (int f = y; f < y + al && f < AL; f++) { uint32_t *p = L.px + (uint64_t)f * AN + x; for (int k = 0; k < an; k++) p[k] = 0; }
}

static void dibujar_lista(void) {
    vaciar(0, 0, LISTA_AN, AL);
    cb_panel(&L, S(28), S(140), LISTA_AN - S(48), AL - S(140) - S(70), CB_CIAN);
    char n[80] = ""; ap_num(n, sizeof(n), (uint64_t)nvis);
    if (busca.n) { ap_agregar(n, " DE ", sizeof(n)); ap_num(n, sizeof(n), (uint64_t)npistas); }
    ap_agregar(n, npistas == 1 ? " PISTA" : " PISTAS", sizeof(n));
    if (ap_leidos > 1) { ap_agregar(n, " // ", sizeof(n)); ap_num(n, sizeof(n), (uint64_t)ap_leidos); ap_agregar(n, " DISCOS", sizeof(n)); }
    texto(&L, S(48), S(164), S(13), CB_CIAN, 255, 0, n);
    cb_buscador(&L, S(44), S(178), LISTA_AN - S(88), busca.t, busca.n);
    int filas = (AL - LISTA_Y0 - S(80)) / FILA;
    if (sel < primera) primera = sel;
    if (sel >= primera + filas) primera = sel - filas + 1;
    for (int k = 0; k < filas && primera + k < nvis; k++) {
        int i = vis[primera + k], y = LISTA_Y0 + k * FILA;
        int este = primera + k == sel;
        char num[8] = ""; ap_dos(num, sizeof(num), (uint64_t)(primera + k + 1));
        texto(&L, S(48), y + S(28), S(13), i == sonando ? CB_ROJO : CB_TENUE, 255, 0, num);
        if (i == sonando) para(&L, S(40), y + S(6), S(6), FILA - S(16), S(3), OPACO(CB_ROJO));
        char t[130]; ap_copiar(t, pistas[i].titulo, sizeof(t));
        while (ap_largo(t) > 3 && texto_ancho(S(18), t) > LISTA_AN - S(150)) t[ap_largo(t) - 1] = 0;
        texto(&L, S(84), y + S(24), S(18), i == sonando ? NEON : este ? 0xFFFFFFu : TEXTO, 255, -4, t);
        if (ap_leidos > 1) texto(&L, S(84), y + S(40), S(11), CB_TENUE, 255, 0, ap_disco_nombre(pistas[i].disco));
    }
    if (nvis) cb_cursor(S(40), LISTA_Y0 + (sel - primera) * FILA, LISTA_AN - S(72), FILA - S(4));
    else {
        cb_cursor(0, 0, 0, 0);
        texto(&L, S(48), LISTA_Y0 + S(40), S(16), CB_TENUE, 255, 0, busca.n ? "NINGUNA PISTA TIENE ESO" : "NO HAY MP3 EN LOS DISCOS");
    }
}


/* Ecualizador: magnitud de 32 frecuencias (de 60 Hz a 12 kHz, en escala logaritmica) sobre 1024 cuadros del sonido que
 * esta saliendo ahora mismo. Una DFT a mano por banda: 32 x 1024 multiplicaciones por cuadro, nada para una CPU. */
static void analizar(void) {
    if (sonando < 0 || R.pausa || !R.iniciado) { for (int b = 0; b < 32; b++) barras[b] *= 0.9f; return; }
    uint64_t ahora = (uint64_t)au.tocado_ms * TAEK_AUDIO_HZ / 1000;
    if (ahora + 1024 > R.cuadros || R.cuadros - ahora > ANILLO_CUADROS - 2048) return;
    for (int b = 0; b < 32; b++) {
        float f = 60.0f;
        for (int k = 0; k < b; k++) f *= 1.187f;
        float w = 6.2831853f * f / (float)TAEK_AUDIO_HZ;
        float c = cosen(w), s = sen(w), re = 0, im = 0, cr = 1, ci = 0;
        for (int n = 0; n < 1024; n += 2) {
            uint64_t q = ((ahora + (uint64_t)n) % ANILLO_CUADROS) * 2;
            float v = ((float)eco[q] + (float)eco[q + 1]) * (1.0f / 65536.0f);
            re += v * cr; im += v * ci;
            float ncr = cr * c - ci * s; ci = cr * s + ci * c; cr = ncr;
            ncr = cr * c - ci * s; ci = cr * s + ci * c; cr = ncr;   /* Se avanza de a dos muestras */
        }
        float mag = (re * re + im * im) * (0.02f + (float)b * 0.004f);
        float v = mag > 1 ? 1 : mag;
        v = v < barras[b] ? barras[b] * 0.85f + v * 0.15f : v;
        barras[b] = v;
    }
}

/* Reactor: dos hexagonos que giran en sentidos contrarios, un nucleo rojo que late con los graves, el espectro en un
 * circulo de rayos alrededor y Neko al centro. Todo calculado del sonido que sale ahora mismo. */
static void hexagono(int cx, int cy, int r, float a, int g, uint32_t c) {
    for (int k = 0; k < 6; k++) {
        float a0 = a + (float)k * 1.0471976f, a1 = a0 + 1.0471976f;
        linea(&L, cx + (int)(cosen(a0) * (float)r), cy + (int)(sen(a0) * (float)r), cx + (int)(cosen(a1) * (float)r), cy + (int)(sen(a1) * (float)r), g, c);
    }
}
static void reactor(int cx, int cy, int r) {
    float graves = (barras[0] + barras[1] + barras[2] + barras[3]) * 0.25f;
    int g = S(3) > 0 ? S(3) : 1;
    /* Espectro en circulo: 32 bandas a cada lado, de amarillo a rojo segun la fuerza */
    for (int b = 0; b < 32; b++) {
        float v = barras[b], largo = (float)S(14) + v * (float)S(120);
        uint32_t c = v > 0.6f ? CB_ROJO : v > 0.3f ? 0xFF8A0Au : CB_AMARILLO;
        for (int lado = -1; lado <= 1; lado += 2) {
            float a = -1.5707963f + (float)lado * ((float)b + 0.5f) * 0.0981748f + giro * 0.15f;
            float r0 = (float)r + (float)S(18);
            linea(&L, cx + (int)(cosen(a) * r0), cy + (int)(sen(a) * r0), cx + (int)(cosen(a) * (r0 + largo)), cy + (int)(sen(a) * (r0 + largo)), g, premul(220, c));
        }
    }
    anillo(&L, cx, cy, r + S(10), r + S(8), premul(140, CB_CIAN));
    hexagono(cx, cy, r, giro, g, OPACO(CB_CIAN));
    hexagono(cx, cy, r * 3 / 4, -giro * 1.6f, g, OPACO(CB_AMARILLO));
    int rn = r / 3 + (int)(graves * (float)(r / 3));
    anillo(&L, cx, cy, rn + S(14), 0, premul((uint32_t)(30 + 60 * graves), CB_ROJO));
    anillo(&L, cx, cy, rn, 0, premul(200, 0x2A0010u));
    anillo(&L, cx, cy, rn, rn - g, OPACO(CB_ROJO));
    gato(&L, cx, cy + S(4), rn * 3 / 2, 0x0B0D14u, CB_CIAN);
    /* Marcas que giran por fuera */
    for (int k = 0; k < 12; k++) {
        float a = -giro * 0.5f + (float)k * 0.5235988f;
        int r1 = r + S(150), r2 = r + S(162);
        linea(&L, cx + (int)(cosen(a) * (float)r1), cy + (int)(sen(a) * (float)r1), cx + (int)(cosen(a) * (float)r2), cy + (int)(sen(a) * (float)r2), g, premul(120, k % 3 ? CB_CIAN : CB_AMARILLO));
    }
}

static void dibujar_panel(void) {
    int x0 = LISTA_AN, an = AN - LISTA_AN;
    vaciar(x0, S(130), an, AL - S(130));
    int cx = x0 + an / 2, cy = S(390), r = S(130);
    reactor(cx, cy, r);
    const struct pista *p = sonando >= 0 ? &pistas[sonando] : (nvis ? &pistas[vis[sel]] : NULL);
    if (p) {
        char t[130]; ap_copiar(t, p->titulo, sizeof(t));
        while (ap_largo(t) > 3 && texto_ancho(S(36), t) > an - S(160)) t[ap_largo(t) - 1] = 0;
        cb_titulo(&L, cx - texto_ancho(S(36), t) / 2, S(700), S(36), sonando >= 0 && !R.pausa && (cuadro_panel % 40) < 3 ? S(6) : S(2), t);
        const char *ar = p->artista[0] ? p->artista : "ARTISTA DESCONOCIDO";
        texto(&L, cx - texto_ancho(S(18), ar) / 2, S(736), S(18), CB_CIAN, 255, -4, ar);
    }
    /* Progreso en bloques, con los tiempos a los lados */
    int bx = x0 + S(140), bw = an - S(280), by = S(770);
    uint64_t pos = posicion_ms(), dur = R.duracion_ms;
    cb_barra(&L, bx, by, bw, S(12), sonando >= 0 && dur ? (int)((pos < dur ? pos : dur) * 1000 / dur) : 0, CB_AMARILLO);
    char a[24] = "", b[24] = "";
    ap_tiempo(a, sizeof(a), sonando >= 0 ? pos : 0);
    ap_tiempo(b, sizeof(b), dur);
    texto(&L, bx - texto_ancho(S(16), a) - S(16), by + S(13), S(16), CB_CIAN, 255, 0, a);
    texto(&L, bx + bw + S(12), by + S(13), S(16), CB_CIAN, 255, 0, b);
    const char *estado = sonando < 0 ? "ELIGE UNA PISTA E INTRO" : R.pausa ? "// EN PAUSA //" : "SONANDO";
    int parpadeo = R.pausa && (cuadro_panel / 15) % 2;
    if (!parpadeo) texto(&L, cx - texto_ancho(S(15), estado) / 2, by + S(44), S(15), R.pausa ? CB_AMARILLO : CB_TEXTO, 255, 0, estado);
    /* Ecualizador en segmentos, de amarillo a rojo, con su reflejo */
    int ew = an - S(280), ex = x0 + S(140), ey = AL - S(110), paso = ew / 32, seg = S(8), nseg = 12;
    for (int k = 0; k < 32; k++) {
        int h = (int)(barras[k] * (float)nseg + 0.5f);
        if (h < 1) h = 1;
        for (int s2 = 0; s2 < h; s2++) {
            uint32_t c = s2 >= 9 ? CB_ROJO : s2 >= 6 ? 0xFF8A0Au : CB_AMARILLO;
            rect(&L, ex + k * paso + S(3), ey - (s2 + 1) * seg, paso - S(6), seg - S(3), OPACO(c));
            if (s2 < 4) rect(&L, ex + k * paso + S(3), ey + S(4) + s2 * seg, paso - S(6), seg - S(3), premul((uint32_t)(50 - s2 * 12), c));
        }
    }
    /* Volumen arriba a la derecha */
    char v[40] = "VOL ";
    if (volumen >= 0) ap_num(v, sizeof(v), (uint64_t)volumen); else { ap_agregar(v, "-", sizeof(v)); ap_num(v, sizeof(v), (uint64_t)-volumen); }
    ap_agregar(v, " dB", sizeof(v));
    int vx = AN - S(330), vy = S(150);
    texto(&L, vx, vy + S(12), S(14), CB_TENUE, 255, 0, v);
    cb_barra(&L, vx + S(110), vy, S(200), S(12), (volumen + 30) * 1000 / 36 < 0 ? 0 : (volumen + 30) * 1000 / 36, CB_CIAN);
    if (aviso[0]) texto(&L, cx - texto_ancho(S(16), aviso) / 2, S(170), S(16), CB_AMARILLO, 255, 0, aviso);
    const char *h = "ESCRIBE: BUSCAR  //  INTRO: TOCAR  //  ESPACIO: PAUSA  //  IZQ / DER: 10 S  //  TAB: SIGUIENTE  //  + / -: VOLUMEN  //  ESC: SALIR";
    int hw = texto_ancho(S(12), h);
    para(&L, cx - hw / 2 - S(30), AL - S(46), hw + S(60), S(28), S(10), premul(200, 0x0A0C14u));
    texto(&L, cx - hw / 2, AL - S(26), S(12), CB_TENUE, 255, 0, h);
}

/* ------------------------------------------------------------------ principal */

long principal(uint64_t a0, uint64_t a1) {
    (void)a0; (void)a1;
    struct taek_pantalla pan;
    if (taek_pantalla(&pan) || !pan.ancho || !pan.alto) return 2;
    AN = (int)pan.ancho; AL = (int)pan.alto;
    /* H148: fondo de neon, contenido transparente encima y las capas que se mueven arriba */
    if (cb_crear_fondo("M\xC3\x9AS" "ICA", "NKSK // AUDIO-02")) return 3;
    sup = (struct taek_superficie){ (uint32_t)AN, (uint32_t)AL, 0, 0, TAEK_SUP_ALFA, 0, 0 };
    if (taek_superficie(&sup)) return 3;
    L = (struct lienzo){ (uint32_t *)(uintptr_t)sup.pixeles, AN, AL };
    if (cb_crear_capas()) return 3;
    pistas = malloc(sizeof(struct pista) * MAX_PISTAS);
    vis = malloc(sizeof(int) * MAX_PISTAS);
    eco = malloc(sizeof(int16_t) * 2 * ANILLO_CUADROS);
    if (!pistas || !vis || !eco) return 4;
    LISTA_AN = AN * 3 / 10; FILA = S(46);
    audio(TAEK_AU_ESTADO);
    hay_audio = (int)au.hay_audio; volumen = au.volumen_db;
    if (!hay_audio) ap_copiar(aviso, "Este equipo no tiene salida de sonido.", sizeof(aviso));

    /* Argumento: un MP3 (se toca y se recorre su carpeta), una carpeta, o nada (la raiz y sus subcarpetas) */
    char arg[TAEK_RUTA] = "";
    taek_argumento(arg, sizeof(arg));
    int empezar = -1;
    limpiar(&L, 0);
    cb_panel(&L, (AN - S(700)) / 2, AL / 2 - S(60), S(700), S(100), CB_CIAN);
    texto(&L, (AN - texto_ancho(S(20), "RASTREANDO M\xC3\x9AS" "ICA EN LOS DISCOS...")) / 2, AL / 2, S(20), CB_CIAN, 255, -6, "RASTREANDO M\xC3\x9AS" "ICA EN LOS DISCOS...");
    taek_presentar(sup.id, 0, 0, AN, AL);
    if (arg[0] && ap_extension(arg, EXT)) {
        char c[TAEK_RUTA]; ap_carpeta_de(arg, c, sizeof(c)); ap_leer_discos(); agregar_carpeta(c);
        for (int i = 0; i < npistas; i++) if (!ap_comparar(pistas[i].ruta, arg)) empezar = i;
    } else if (arg[0]) { ap_leer_discos(); agregar_carpeta(arg); }
    else agregar_todos();
    filtrar();
    if (empezar >= 0) tocar(empezar);
    char m[40] = ""; ap_num(m, sizeof(m), (uint64_t)npistas); ap_agregar(m, " en ", sizeof(m)); ap_num(m, sizeof(m), (uint64_t)ap_leidos); ap_agregar(m, " discos", sizeof(m));
    ap_marca("MUSICA", "ABIERTO", arg[0] ? arg : m);
    limpiar(&L, 0);
    dibujar_lista(); dibujar_panel();
    taek_presentar(sup.id, 0, 0, AN, AL);
    cb_entrar(-S(160));

    struct taek_evento e;
    uint32_t antes = 0;
    long ultimo_cuadro = 0;
    for (;;) {
        long t = taek_ms();
        int lista = 0;
        uint32_t botones = 0;
        for (int i = 0; i < 16; i++) {
            long r = taek_evento(&e);
            if (r < 0) return 1;
            botones |= e.botones;
            if (!r) break;
            uint32_t k = ap_filtrar(e.tecla);
            if (!k) continue;
            if (k == 3) return 0;
            /* Con la busqueda vacia, espacio y + / - siguen siendo pausa y volumen; lo demas que se escribe, busca */
            int control = !busca.n && (k == ' ' || k == '+' || k == '=' || k == '-');
            if (!control && k != '\n' && k != '\r' && ap_buscar_tecla(&busca, k)) { filtrar(); sel = 0; lista = 1; cb_glitch(2); }
            else if (k == TAEK_TECLA_ARRIBA && sel > 0) { sel--; lista = 1; }
            else if (k == TAEK_TECLA_ABAJO && sel + 1 < nvis) { sel++; lista = 1; }
            else if ((k == '\n' || k == '\r') && nvis) { tocar(vis[sel]); lista = 1; cb_glitch(4); }
            else if (k == 9 && npistas) { tocar((sonando + 1) % npistas); lista = 1; cb_glitch(4); }
            else if (k == ' ' && sonando >= 0) {
                if (R.pausa) { R.pausa = 0; ir_a(R.pausa_ms); }
                else { R.pausa_ms = posicion_ms(); R.pausa = 1; detener(); }
            }
            else if (k == TAEK_TECLA_DERECHA && sonando >= 0 && !R.pausa) ir_a(posicion_ms() + 10000);
            else if (k == TAEK_TECLA_IZQUIERDA && sonando >= 0 && !R.pausa) { uint64_t p = posicion_ms(); ir_a(p > 10000 ? p - 10000 : 0); }
            else if (k == '+' || k == '='){ au.volumen_db = volumen + 3; audio(TAEK_AU_VOLUMEN); volumen = au.volumen_db; }
            else if (k == '-') { au.volumen_db = volumen - 3; audio(TAEK_AU_VOLUMEN); volumen = au.volumen_db; }
        }
        if (ap_esc_suelto()) {
            if (!busca.n) return 0;
            busca.n = 0; busca.t[0] = 0; filtrar();
            if (sonando >= 0 && visible(sonando) >= 0) sel = visible(sonando);
            lista = 1;
        }
        if (e.hay_puntero) {
            uint32_t subida = botones & ~antes;
            antes = botones;
            if (subida & 2) return 0;
            if (subida & 1) {
                if (e.x < LISTA_AN && e.y >= LISTA_Y0) {
                    int i = primera + (e.y - LISTA_Y0) / FILA;
                    if (i < nvis) { if (i == sel) { tocar(vis[i]); cb_glitch(4); } sel = i; lista = 1; }
                } else if (e.y >= S(755) && e.y < S(795) && sonando >= 0 && R.duracion_ms) {
                    int bx = LISTA_AN + S(140), bw = AN - LISTA_AN - S(280);
                    if (e.x >= bx && e.x < bx + bw) ir_a(R.duracion_ms * (uint64_t)(e.x - bx) / (uint64_t)bw);
                }
            }
        }
        bombear();
        audio(TAEK_AU_ESTADO);
        /* Fin de la cancion: archivo leido, cola vacia y nada sonando. Sigue la proxima. */
        if (sonando >= 0 && !R.pausa && R.archivo_acabo && R.pend_i >= R.pend_n && (!au.sonando || !au.ocupada) && R.iniciado) {
            detener();
            if (sonando + 1 < npistas) tocar(sonando + 1);
            else sonando = -1;
            lista = 1;
        }
        if (lista) { dibujar_lista(); taek_presentar(sup.id, 0, 0, LISTA_AN, AL); }
        /* El panel se repinta a unos 30 cuadros por segundo */
        if (taek_ms() - ultimo_cuadro >= 33) {
            ultimo_cuadro = taek_ms();
            giro += sonando >= 0 && !R.pausa ? 0.045f : 0.008f;   /* Quieto, el reactor gira despacio */
            cuadro_panel++;
            analizar();
            dibujar_panel();
            taek_presentar(sup.id, LISTA_AN, S(130), AN - LISTA_AN, AL - S(130));
        }
        cb_animar();
        long g = taek_ms() - t;
        if (g < 10) taek_esperar(10 - g);
    }
}
