/* YouTube nativo de NekoSaka para TAEK OS (H185, fase B). 'ejecutar /youtube.elf [busqueda]'.
 *
 * Sin navegador: habla con la API interna de YouTube (InnerTube, cliente ANDROID, anonimo) por SYS_RED (TCP + TLS 1.3
 * del nucleo). Todo el HTTP/1.1 (conexion persistente, trozos, redirecciones), el JSON y la interfaz viven aqui.
 *   - Busqueda: POST /youtubei/v1/search -> compactVideoRenderer (id, titulo, canal, duracion, vistas).
 *   - Miniaturas: https://i.ytimg.com/vi/<id>/mqdefault.jpg, decodificadas aqui con stb_image.
 *   - Ver: /youtubei/v1/player -> itag 18 (MP4 360p, H.264 + AAC, URL directa) -> descarga por rangos de 4 MiB ->
 *     SYS_VIDEO lo entrega al nucleo -> TAEK_PROGRAMA_ABRIR "mem:<titulo>" lo reproduce a pantalla completa y vuelve
 *     a abrir esta app con la misma busqueda.
 * Protocolo seguido de ViTube (PS Vita, GPL-3.0) sin copiar su codigo: TAEK es MIT.
 * H187-H188: 720p/1080p en flujo (HLS de VISIONOS -> SYS_FLUJO, segmento a segmento) con pausa, saltos y capa de
 * controles; pestanas Inicio (cuenta o relacionados de lo visto, cliente TVHTML5), Buscar, Suscripciones (cuenta),
 * Historial (/youtube/historial.txt, sigue donde quedo), Descargas (/youtube/descargas/<id>, se ven sin red) y Cuenta
 * (OAuth device flow con /youtube.conf del usuario).
 * Teclas: Tab cambia de pestana; escribir busca; Intro ve; F2 descarga; Supr quita; Esc sale.
 * En el video: Espacio pausa, flechas saltan 10 s / 1 min, Esc vuelve. Clic elige; clic sobre el elegido lo abre. */
#pragma clang diagnostic ignored "-Wunused-function"
#include "taek.h"
/* NekoTube QM-2 (paso 1): las fuentes del prototipo. Chakra Petch Bold para titulos y textos de interfaz, Share Tech
 * Mono para telemetria y etiquetas. texto() usa la que este en g_fuente; por defecto Chakra. */
extern const uint8_t _binary_fuente_chakra_bin_start[], _binary_fuente_mono_bin_start[];
#define F_CHAKRA _binary_fuente_chakra_bin_start
#define F_MONO   _binary_fuente_mono_bin_start
static const uint8_t *g_fuente = F_CHAKRA;
#define FUENTE g_fuente
#include "ui.h"
#include "memoria.h"
#include "apps.h"
#include "hls_mp4.h"

TAEK_PROGRAMA

#define STBI_MALLOC(n) malloc(n)
#define STBI_REALLOC(p, n) realloc(p, n)
#define STBI_FREE(p) free(p)
#define STBI_NO_STD_HEADERS
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 4096
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#include "nucleo/controladores/multimedia/imagen/stb_image.h"
#pragma clang diagnostic pop

/* ------------------------------------------------------------------ colores y medidas (paleta QM-2, style.css :root) */
#define Q_Y     0xFCEE0Au   /* --y */
#define Q_CIAN  0x00F0FFu   /* --cyan */
#define Q_ROJO  0xFF003Cu   /* --red */
#define Q_VERDE 0x39FF7Au   /* --green */
#define Q_NAR   0xFF7A00u   /* --orange */
#define Q_BG    0x05060Au   /* --bg */
#define Q_BG2   0x0A0D14u   /* --bg-2 */
#define Q_PANEL 0x090C13u   /* --panel, a 0.9 */
#define Q_TXT   0xE8EDF5u   /* --txt */
#define Q_DIM   0x5D6B82u   /* --dim */
#define Q_DIM2  0x8796ADu   /* --dim-2 */
#define Q_LINEA premul(56, Q_CIAN)   /* --line: cian a 0.22 */
/* Nombres de antes, ya con la paleta nueva */
#define FONDO   (0xFF000000u | Q_BG)
#define BARRA   (0xFF000000u | Q_BG2)
#define CARTA   (0xFF000000u | Q_BG2)
#define CAMPO   0xFF151A26u
#define ROJO    Q_ROJO
#define ROSA    Q_Y
#define TEXTO   Q_TXT
#define TENUE   Q_DIM2
#define BLANCO  0xFFFFFFu

static int AN, AL;
static int S(int v) { return v * AL / 1080; }
static struct taek_superficie sup;
static struct lienzo L;

/* ------------------------------------------------------------------ texto */
static int lg(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void cp(char *d, const char *o, int cap) { int i = 0; while (o[i] && i + 1 < cap) { d[i] = o[i]; i++; } d[i] = 0; }
static void add(char *d, const char *o, int cap) { int n = lg(d); cp(d + n, o, cap - n); }
static void addn(char *d, uint64_t v, int cap) { char b[24]; int i = 0; do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v); int n = lg(d); while (i && n + 1 < cap) d[n++] = b[--i]; d[n] = 0; }
static int igual(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static char minus(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static const char *hallar(const char *a, const char *b, const char *aguja) {
    int n = lg(aguja);
    for (const char *p = a; p + n <= b; p++) {
        int i = 0;
        while (i < n && p[i] == aguja[i]) i++;
        if (i == n) return p;
    }
    return 0;
}
static int prefijo_ci(const char *s, const char *p) { while (*p) { if (minus(*s) != minus(*p)) return 0; s++; p++; } return 1; }
static void yt_log(const char *que, const char *detalle) { ap_marca("YOUTUBE", que, detalle); }

/* ------------------------------------------------------------------ red: conexion persistente */
struct con { long id; char host[128]; int tls; uint8_t *rx; uint32_t n, i; };
static struct con c_api, c_img, c_vid;

static void con_cerrar(struct con *c) {
    if (c->id >= 0) { struct taek_red r; memset(&r, 0, sizeof(r)); r.op = TAEK_RED_CERRAR; r.id = (int32_t)c->id; taek_red(&r); }
    c->id = -1; c->host[0] = 0; c->n = c->i = 0;
}
static int con_abrir(struct con *c, const char *host) {
    if (c->id >= 0 && igual(c->host, host)) return 0;
    con_cerrar(c);
    if (!c->rx) c->rx = malloc(65536);
    if (!c->rx) return -1;
    struct taek_red r; memset(&r, 0, sizeof(r));
    r.op = TAEK_RED_ABRIR; cp(r.host, host, TAEK_RED_HOST); r.puerto = 443; r.banderas = TAEK_RED_TLS; r.espera_ms = 15000;
    long x = taek_red(&r);
    if (x < 0) { char m[200] = ""; add(m, host, 200); add(m, " error ", 200); addn(m, (uint64_t)-x, 200); add(m, " detalle ", 200); addn(m, (uint64_t)(r.error < 0 ? -r.error : r.error), 200); yt_log("no se pudo conectar", m); return -1; }
    c->id = x; cp(c->host, host, sizeof(c->host)); c->n = c->i = 0;
    return 0;
}
static int con_enviar(struct con *c, const char *d, uint32_t n) {
    for (uint32_t off = 0; off < n;) {
        struct taek_red r; memset(&r, 0, sizeof(r));
        r.op = TAEK_RED_ENVIAR; r.id = (int32_t)c->id; r.datos = (uint64_t)(uintptr_t)(d + off);
        r.largo = n - off > 65536 ? 65536 : n - off; r.espera_ms = 15000;
        long x = taek_red(&r);
        if (x <= 0) return -1;
        off += (uint32_t)x;
    }
    return 0;
}
/* Trae mas bytes al bufer: >0 bytes, 0 sin datos en 15 s, <0 conexion cerrada. */
static int con_rellenar(struct con *c) {
    struct taek_red r; memset(&r, 0, sizeof(r));
    r.op = TAEK_RED_RECIBIR; r.id = (int32_t)c->id; r.datos = (uint64_t)(uintptr_t)c->rx; r.largo = 65536; r.espera_ms = 15000;
    long x = taek_red(&r);
    if (x > 0) { c->n = (uint32_t)x; c->i = 0; }
    return (int)x;
}
static int con_linea(struct con *c, char *l, int cap) {
    int k = 0;
    for (;;) {
        if (c->i == c->n && con_rellenar(c) <= 0) return -1;
        char ch = (char)c->rx[c->i++];
        if (ch == '\n') { while (k && l[k - 1] == '\r') k--; l[k] = 0; return k; }
        if (k + 1 < cap) l[k++] = ch;
    }
}
static int64_t con_leer(struct con *c, uint8_t *d, uint64_t n) {
    uint64_t hecho = 0;
    while (hecho < n) {
        if (c->i == c->n && con_rellenar(c) <= 0) break;
        uint32_t a = c->n - c->i;
        if (a > n - hecho) a = (uint32_t)(n - hecho);
        if (d) memcpy(d + hecho, c->rx + c->i, a);
        c->i += a; hecho += a;
    }
    return (int64_t)hecho;
}

/* ------------------------------------------------------------------ HTTP/1.1 */
struct resp { int estado; int64_t largo; uint64_t total; int cerrar, trozos; char lugar[2048]; };
typedef void (*fn_progreso)(uint64_t hecho, uint64_t total);
static char g_pet[12288];

/* Envia una peticion y lee la respuesta entera en 'cuerpo' (hasta cap). Reintenta una vez si la conexion reusada
 * estaba cerrada. Devuelve los bytes del cuerpo o un negativo. */
static int64_t http(struct con *c, const char *host, const char *metodo, const char *ruta, const char *extra,
                    const char *datos, uint32_t datos_n, uint8_t *cuerpo, uint64_t cap, struct resp *r, fn_progreso prog) {
    for (int intento = 0; intento < 2; intento++) {
        int nueva = !(c->id >= 0 && igual(c->host, host));
        if (con_abrir(c, host)) return -1;
        g_pet[0] = 0;
        add(g_pet, metodo, sizeof(g_pet)); add(g_pet, " ", sizeof(g_pet)); add(g_pet, ruta, sizeof(g_pet));
        add(g_pet, " HTTP/1.1\r\nHost: ", sizeof(g_pet)); add(g_pet, host, sizeof(g_pet));
        add(g_pet, "\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n", sizeof(g_pet));
        if (extra) add(g_pet, extra, sizeof(g_pet));
        if (datos) { add(g_pet, "Content-Length: ", sizeof(g_pet)); addn(g_pet, datos_n, sizeof(g_pet)); add(g_pet, "\r\n", sizeof(g_pet)); }
        add(g_pet, "\r\n", sizeof(g_pet));
        if (con_enviar(c, g_pet, (uint32_t)lg(g_pet)) || (datos && con_enviar(c, datos, datos_n))) { con_cerrar(c); if (nueva) return -2; continue; }
        char l[2200];
        if (con_linea(c, l, sizeof(l)) < 12 || !prefijo_ci(l, "HTTP/1.")) { con_cerrar(c); if (nueva) return -3; continue; }
        memset(r, 0, sizeof(*r));
        r->estado = (l[9] - '0') * 100 + (l[10] - '0') * 10 + (l[11] - '0');
        r->largo = -1;
        for (;;) {
            int k = con_linea(c, l, sizeof(l));
            if (k < 0) { con_cerrar(c); return -4; }
            if (!k) break;
            if (prefijo_ci(l, "content-length:")) { int64_t v = 0; for (const char *p = l + 15; *p; p++) if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0'); r->largo = v; }
            else if (prefijo_ci(l, "transfer-encoding:") && hallar(l, l + k, "chunked")) r->trozos = 1;
            else if (prefijo_ci(l, "connection:") && hallar(l, l + k, "close")) r->cerrar = 1;
            else if (prefijo_ci(l, "location:")) { const char *p = l + 9; while (*p == ' ') p++; cp(r->lugar, p, sizeof(r->lugar)); }
            else if (prefijo_ci(l, "content-range:")) { const char *p = hallar(l, l + k, "/"); uint64_t v = 0; if (p) for (p++; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint64_t)(*p - '0'); r->total = v; }
        }
        uint64_t hecho = 0;
        if (r->trozos) {
            for (;;) {
                if (con_linea(c, l, sizeof(l)) < 0) { con_cerrar(c); return -5; }
                uint64_t t = 0;
                for (const char *p = l; *p; p++) {
                    char h = minus(*p);
                    if (h >= '0' && h <= '9') t = t * 16 + (uint64_t)(h - '0');
                    else if (h >= 'a' && h <= 'f') t = t * 16 + (uint64_t)(h - 'a' + 10);
                    else break;
                }
                if (!t) { while (con_linea(c, l, sizeof(l)) > 0) {} break; }   /* trailer y linea vacia */
                uint64_t cabe = hecho < cap ? cap - hecho : 0;
                uint64_t a = t < cabe ? t : cabe;
                if (con_leer(c, cuerpo ? cuerpo + hecho : 0, a) != (int64_t)a || (t > a && con_leer(c, 0, t - a) != (int64_t)(t - a))) { con_cerrar(c); return -6; }
                hecho += a;
                if (con_linea(c, l, sizeof(l)) < 0) { con_cerrar(c); return -7; }
                if (prog) prog(hecho, 0);
            }
        } else if (r->largo >= 0) {
            uint64_t n = (uint64_t)r->largo, a = n < cap ? n : cap;
            while (hecho < a) {
                uint64_t trozo = a - hecho > (1u << 20) ? (1u << 20) : a - hecho;
                int64_t x = con_leer(c, cuerpo + hecho, trozo);
                if (x <= 0) { con_cerrar(c); return -8; }
                hecho += (uint64_t)x;
                if (prog) prog(hecho, r->total ? r->total : n);
            }
            if (n > a && con_leer(c, 0, n - a) != (int64_t)(n - a)) { con_cerrar(c); return -9; }
        } else {
            for (;;) {                                       /* hasta que el servidor cierre */
                if (c->i == c->n && con_rellenar(c) <= 0) break;
                uint32_t a = c->n - c->i;
                if (hecho + a > cap) a = (uint32_t)(cap - hecho);
                memcpy(cuerpo + hecho, c->rx + c->i, a);
                c->i += a; hecho += a;
                if (hecho >= cap) break;
            }
            r->cerrar = 1;
        }
        if (r->cerrar) con_cerrar(c);
        return (int64_t)hecho;
    }
    return -10;
}
/* https://host/ruta -> host y ruta */
static int partir_url(const char *u, char *host, int hcap, const char **ruta) {
    if (!prefijo_ci(u, "https://")) return -1;
    u += 8;
    int k = 0;
    while (*u && *u != '/' && k + 1 < hcap) host[k++] = *u++;
    host[k] = 0;
    *ruta = *u ? u : "/";
    return k ? 0 : -1;
}

/* ------------------------------------------------------------------ JSON acotado */
static int utf8_poner(char *o, int k, int cap, uint32_t cp_) {
    if (cp_ < 0x80) { if (k + 1 < cap) o[k++] = (char)cp_; }
    else if (cp_ < 0x800) { if (k + 2 < cap) { o[k++] = (char)(0xC0 | (cp_ >> 6)); o[k++] = (char)(0x80 | (cp_ & 0x3F)); } }
    else if (cp_ < 0x10000) { if (k + 3 < cap) { o[k++] = (char)(0xE0 | (cp_ >> 12)); o[k++] = (char)(0x80 | ((cp_ >> 6) & 0x3F)); o[k++] = (char)(0x80 | (cp_ & 0x3F)); } }
    else if (k + 4 < cap) { o[k++] = (char)(0xF0 | (cp_ >> 18)); o[k++] = (char)(0x80 | ((cp_ >> 12) & 0x3F)); o[k++] = (char)(0x80 | ((cp_ >> 6) & 0x3F)); o[k++] = (char)(0x80 | (cp_ & 0x3F)); }
    return k;
}
static uint32_t hex4(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) { char h = minus(p[i]); v = v * 16 + (uint32_t)(h >= 'a' ? h - 'a' + 10 : h - '0'); }
    return v;
}
/* p apunta a la comilla de apertura. Copia la cadena decodificada (UTF-8) y devuelve el puntero tras la de cierre. */
static const char *json_cadena(const char *p, const char *fin, char *o, int cap) {
    int k = 0;
    if (p >= fin || *p != '"') { if (cap) o[0] = 0; return p; }
    for (p++; p < fin && *p != '"'; p++) {
        if (*p != '\\') { if (k + 1 < cap) o[k++] = *p; continue; }
        if (++p >= fin) break;
        char e = *p;
        if (e == 'n') { if (k + 1 < cap) o[k++] = ' '; }
        else if (e == 't' || e == 'r') { if (k + 1 < cap) o[k++] = ' '; }
        else if (e == 'u' && p + 4 < fin) {
            uint32_t v = hex4(p + 1); p += 4;
            if (v >= 0xD800 && v < 0xDC00 && p + 6 < fin && p[1] == '\\' && p[2] == 'u') {
                uint32_t b = hex4(p + 3);
                if (b >= 0xDC00 && b < 0xE000) { v = 0x10000 + ((v - 0xD800) << 10) + (b - 0xDC00); p += 6; }
            }
            k = utf8_poner(o, k, cap, v);
        } else if (k + 1 < cap) o[k++] = e;                    /* \" \\ \/ */
    }
    if (cap) o[k < cap ? k : cap - 1] = 0;
    return p < fin ? p + 1 : fin;
}
/* p en '{' o '[': devuelve el puntero tras su cierre (saltando cadenas). */
static const char *json_bloque(const char *p, const char *fin) {
    int prof = 0;
    for (; p < fin; p++) {
        if (*p == '"') { for (p++; p < fin && *p != '"'; p++) if (*p == '\\') p++; continue; }
        if (*p == '{' || *p == '[') prof++;
        else if ((*p == '}' || *p == ']') && --prof == 0) return p + 1;
    }
    return fin;
}
/* Dentro de [a, b): la clave "clave" y luego el primer "text" o "simpleText" (los objetos de texto de YouTube). */
static int json_texto_de(const char *a, const char *b, const char *clave, char *o, int cap) {
    char k[64] = "\""; add(k, clave, sizeof(k)); add(k, "\":", sizeof(k));
    const char *p = hallar(a, b, k);
    if (!p) { o[0] = 0; return 0; }
    const char *lim = p + 3000 < b ? p + 3000 : b;
    const char *t = hallar(p, lim, "\"text\":\""), *s = hallar(p, lim, "\"simpleText\":\"");
    if (!t || (s && s < t)) t = s ? s + 13 : 0; else t += 7;
    if (!t) { o[0] = 0; return 0; }
    json_cadena(t, b, o, cap);
    return 1;
}


/* ------------------------------------------------------------------ InnerTube */
#define VERSION "21.26.364"
#define UA "User-Agent: com.google.android.youtube/" VERSION " (Linux; U; Android 11) gzip\r\n"
static const char *EXTRA_API = "Content-Type: application/json\r\n" UA
                               "X-Youtube-Client-Name: 3\r\nX-Youtube-Client-Version: " VERSION "\r\n";
static const char *CONTEXTO = "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"" VERSION
                              "\",\"androidSdkVersion\":30,\"hl\":\"es\",\"gl\":\"PE\"}},";
/* H188: cliente de TV (como ViTube): relacionados sin cuenta y, con cuenta, inicio y suscripciones. tileRenderer. */
#define TV_VERSION "7.20250101.00.00"

struct video {
    char id[12]; char titulo[240]; char canal[96]; char dur[16]; char vistas[64];
    uint32_t *mini; int mini_estado;
    uint32_t pos_ms, dur_ms;          /* H188: historial (donde quedo) */
};
#define MAX_RES 40
static struct video res[MAX_RES];
static int nres;
static uint8_t *g_json;
static int64_t g_json_n;                      /* H202: bytes de la ultima respuesta de busqueda en g_json */
#define JSON_CAP (6u << 20)

static char consulta[128], buscada[128], estado[200], seccion[200];
static char g_visitante[600];                 /* H186: visitorData de la busqueda, para VISIONOS */
static int g_calidad = 1080;                  /* H186: calidad maxima */
static int sel, desde_fila, ocupado_pct = -1;
static char ocupado_msg[160];

/* H188: pestanas */
enum { P_INICIO, P_BUSCAR, P_SHORTS, P_SUBS, P_HIST, P_DESC, P_PERFIL, P_CUENTA, NPEST };   /* paso 4: PERFIL; paso 6: SHORTS */
static int pest = P_INICIO;

static void dibujar(void);
static void mostrar(void) { dibujar(); taek_presentar(sup.id, 0, 0, AN, AL); }
static void ocupado(const char *m, int pct) { cp(ocupado_msg, m, sizeof(ocupado_msg)); ocupado_pct = pct; mostrar(); }
static void listo(void) { ocupado_pct = -1; ocupado_msg[0] = 0; }
static void fmt_tiempo(char *o, int cap, uint32_t ms) {
    uint32_t s = ms / 1000, h = s / 3600, m = (s / 60) % 60;
    o[0] = 0;
    if (h) { addn(o, h, cap); add(o, ":", cap); if (m < 10) add(o, "0", cap); }
    addn(o, m, cap); add(o, ":", cap); if (s % 60 < 10) add(o, "0", cap); addn(o, s % 60, cap);
}
static uint32_t dur_de_texto(const char *s) {     /* "1:02:03" o "4:13" -> ms */
    uint32_t t = 0, v = 0; int hay = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); hay = 1; }
        else if (*s == ':') { t = (t + v) * 60; v = 0; }
        else if (hay) break;
    }
    return (t + v) * 1000;
}
static void limpiar_res(void) {
    for (int i = 0; i < nres; i++) if (res[i].mini) { free(res[i].mini); res[i].mini = 0; }
    nres = 0; sel = 0; desde_fila = 0;
}

/* ------------------------------------------------------------------ archivos en el USB (H188) */
static int g_disco_ok;
/* El USB de arranque: el que tiene /wifi.conf, /youtube.conf o /youtube */
static void elegir_disco(void) {
    if (g_disco_ok) return;
    g_disco_ok = 1;
    ap_leer_discos();
    for (int i = 0; i < ap_ndiscos; i++) {
        ap_disco = ap_discos[i].numero + 1;
        struct taek_entrada *e; int n = ap_listar("/", &e, 256);
        if (n < 0) continue;
        int hay = 0;
        for (int k = 0; k < n; k++)
            if (!ap_comparar(e[k].nombre, "wifi.conf") || !ap_comparar(e[k].nombre, "youtube") || !ap_comparar(e[k].nombre, "youtube.conf")) hay = 1;
        free(e);
        if (hay) return;
    }
    ap_disco = 0;
}
static long ar_op(uint32_t op, const char *ruta, const void *d, uint64_t n) {
    memset(&ap_ar, 0, sizeof(ap_ar));
    ap_ar.op = op; ap_ar.disco = ap_disco; ap_copiar(ap_ar.ruta, ruta, sizeof(ap_ar.ruta));
    ap_ar.destino = (uint64_t)(uintptr_t)d; ap_ar.capacidad = n;
    return taek_archivos(&ap_ar);
}
static long ar_escribir(const char *ruta, const void *d, uint64_t n) { return ar_op(TAEK_AR_ESCRIBIR, ruta, d, n); }
static long ar_borrar(const char *ruta) { return ar_op(TAEK_AR_BORRAR, ruta, 0, 0); }
static long ar_carpeta(const char *ruta) { long r = ar_op(TAEK_AR_CREAR_CARPETA, ruta, 0, 0); return r == -31 ? 0 : r; }
static void sin_tabs(char *s) { for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' '; }

/* ------------------------------------------------------------------ historial local (H188) */
#define MAX_HIST 100
static struct video *g_hist;
static int nhist;
static int campo(const char **p, const char *fin, char *o, int cap) {      /* hasta \t o \n */
    int k = 0;
    while (*p < fin && **p != '\t' && **p != '\n') { if (k + 1 < cap) o[k++] = **p; (*p)++; }
    o[k] = 0;
    if (*p < fin && **p == '\t') { (*p)++; return 1; }
    return 0;
}
static uint32_t num(const char *s) { uint32_t v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0'); return v; }
static void hist_cargar(void) {
    if (!g_hist) g_hist = malloc(sizeof(struct video) * MAX_HIST);
    if (!g_hist) return;
    nhist = 0;
    uint64_t n = 0;
    uint8_t *b = ap_leer_todo("/youtube/historial.txt", &n, 1u << 20);
    if (!b) return;
    const char *p = (const char *)b, *fin = p + n;
    while (p < fin && nhist < MAX_HIST) {
        struct video *v = &g_hist[nhist];
        memset(v, 0, sizeof(*v));
        char t[24];
        campo(&p, fin, v->id, sizeof(v->id)); campo(&p, fin, v->titulo, sizeof(v->titulo));
        campo(&p, fin, v->canal, sizeof(v->canal)); campo(&p, fin, v->dur, sizeof(v->dur));
        campo(&p, fin, t, sizeof(t)); v->pos_ms = num(t);
        campo(&p, fin, t, sizeof(t)); v->dur_ms = num(t);
        while (p < fin && *p != '\n') p++;
        if (p < fin) p++;
        if (lg(v->id) == 11) nhist++;
    }
    free(b);
}
static void hist_guardar(void) {
    static char b[MAX_HIST * 420];
    b[0] = 0;
    for (int i = 0; i < nhist; i++) {
        struct video *v = &g_hist[i];
        sin_tabs(v->titulo); sin_tabs(v->canal); sin_tabs(v->dur);
        add(b, v->id, sizeof(b)); add(b, "\t", sizeof(b)); add(b, v->titulo, sizeof(b)); add(b, "\t", sizeof(b));
        add(b, v->canal, sizeof(b)); add(b, "\t", sizeof(b)); add(b, v->dur, sizeof(b)); add(b, "\t", sizeof(b));
        addn(b, v->pos_ms, sizeof(b)); add(b, "\t", sizeof(b)); addn(b, v->dur_ms, sizeof(b)); add(b, "\n", sizeof(b));
    }
    elegir_disco();
    ar_carpeta("/youtube");
    long r = nhist ? ar_escribir("/youtube/historial.txt", b, (uint64_t)lg(b)) : ar_borrar("/youtube/historial.txt");
    if (r < 0) { char m[60] = "codigo -"; addn(m, (uint64_t)-r, sizeof(m)); yt_log("historial no guardado", m); }
}
static struct video *hist_buscar(const char *id) {
    for (int i = 0; i < nhist; i++) if (igual(g_hist[i].id, id)) return &g_hist[i];
    return 0;
}
/* Pone (o sube) el video al principio con su posicion */
static void hist_poner(const struct video *v, uint32_t pos, uint32_t dur) {
    if (!g_hist) return;
    struct video x = *v;
    x.mini = 0; x.mini_estado = 0; x.pos_ms = pos; x.dur_ms = dur;
    int j = nhist;
    for (int i = 0; i < nhist; i++) if (igual(g_hist[i].id, v->id)) { j = i; break; }
    if (j == nhist && nhist < MAX_HIST) nhist++;
    if (j >= MAX_HIST) j = MAX_HIST - 1;
    for (int i = j; i > 0; i--) g_hist[i] = g_hist[i - 1];
    g_hist[0] = x;
    hist_guardar();
}
static void hist_quitar(const char *id) {
    for (int i = 0; i < nhist; i++) if (igual(g_hist[i].id, id)) {
        for (int k = i; k + 1 < nhist; k++) g_hist[k] = g_hist[k + 1];
        nhist--; hist_guardar(); return;
    }
}

/* ------------------------------------------------------------------ cuenta de Google (H188)
 * OAuth "device flow" de YouTube en TV, como ViTube: el usuario aprueba en google.com/device con el codigo que se
 * muestra. El client_id y el secreto del cliente de TV los pone el usuario en /youtube.conf (no van en el codigo):
 *     client_id=...
 *     client_secret=...
 * El token de refresco queda en /youtube/cuenta.txt del USB (en texto: quien tenga el USB tiene la sesion). */
static struct con c_web;
static char g_cfg_id[200], g_cfg_secreto[120], g_refresh[600], g_access[2600];
static long g_access_hasta;
static char g_login_codigo[32], g_login_url[120];
static int g_login_activo;
static char *despues_de(char *s, char *fin, const char *clave) {
    char *p = (char *)hallar(s, fin, clave);
    return p ? p + lg(clave) : 0;
}
static void linea_a(const char *p, const char *fin, char *o, int cap) {
    int k = 0;
    while (p < fin && *p != '\n' && *p != '\r' && *p != ' ' && k + 1 < cap) o[k++] = *p++;
    o[k] = 0;
}
static void cuenta_cargar(void) {
    elegir_disco();
    uint64_t n = 0;
    uint8_t *b = ap_leer_todo("/youtube.conf", &n, 64 * 1024);
    g_cfg_id[0] = g_cfg_secreto[0] = 0;
    if (b) {
        char *s = (char *)b, *fin = s + n, *p;
        if ((p = despues_de(s, fin, "client_id="))) linea_a(p, fin, g_cfg_id, sizeof(g_cfg_id));
        if ((p = despues_de(s, fin, "client_secret="))) linea_a(p, fin, g_cfg_secreto, sizeof(g_cfg_secreto));
        free(b);
    }
    g_refresh[0] = 0;
    b = ap_leer_todo("/youtube/cuenta.txt", &n, 64 * 1024);
    if (b) {
        char *s = (char *)b, *p = despues_de(s, s + n, "refresh_token=");
        if (p) linea_a(p, s + n, g_refresh, sizeof(g_refresh));
        free(b);
    }
}
static int con_cuenta(void) { return g_refresh[0] != 0; }
static void urlcod(char *o, int cap, const char *s) {
    static const char *H = "0123456789ABCDEF";
    int k = lg(o);
    for (; *s && k + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') o[k++] = (char)c;
        else { o[k++] = '%'; o[k++] = H[c >> 4]; o[k++] = H[c & 15]; }
    }
    o[k] = 0;
}
/* "clave": "valor" o "clave": 123 (Google pone espacios) */
static int json_valor(const char *j, const char *fin, const char *clave, char *o, int cap) {
    char k[64] = "\""; add(k, clave, sizeof(k)); add(k, "\"", sizeof(k));
    const char *p = hallar(j, fin, k);
    o[0] = 0;
    if (!p) return 0;
    p += lg(k);
    while (p < fin && (*p == ' ' || *p == ':' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    if (p < fin && *p == '"') { json_cadena(p, fin, o, cap); return 1; }
    int n = 0;
    while (p < fin && ((*p >= '0' && *p <= '9') || *p == '-') && n + 1 < cap) o[n++] = *p++;
    o[n] = 0;
    return n > 0;
}
static int64_t oauth(const char *ruta, const char *cuerpo) {
    struct resp r;
    int64_t m = http(&c_web, "oauth2.googleapis.com", "POST", ruta,
                     "Content-Type: application/x-www-form-urlencoded\r\nUser-Agent: TAEK-OS/1.0\r\n",
                     cuerpo, (uint32_t)lg(cuerpo), g_json, JSON_CAP - 1, &r, 0);
    con_cerrar(&c_web);
    if (m >= 0) g_json[m] = 0;
    return m;
}
static void cuenta_guardar_refresh(void) {
    char b[700] = "refresh_token="; add(b, g_refresh, sizeof(b)); add(b, "\n", sizeof(b));
    elegir_disco(); ar_carpeta("/youtube");
    long r = ar_escribir("/youtube/cuenta.txt", b, (uint64_t)lg(b));
    if (r < 0) { char m[60] = "codigo -"; addn(m, (uint64_t)-r, sizeof(m)); yt_log("cuenta no guardada", m); }
}
/* Token de acceso vigente (lo renueva con el de refresco). 0 bien. */
static int token(void) {
    if (!con_cuenta() || !g_cfg_id[0]) return -1;
    if (g_access[0] && taek_ms() < g_access_hasta) return 0;
    static char cuerpo[1400];
    cuerpo[0] = 0;
    add(cuerpo, "client_id=", sizeof(cuerpo)); urlcod(cuerpo, sizeof(cuerpo), g_cfg_id);
    add(cuerpo, "&client_secret=", sizeof(cuerpo)); urlcod(cuerpo, sizeof(cuerpo), g_cfg_secreto);
    add(cuerpo, "&refresh_token=", sizeof(cuerpo)); urlcod(cuerpo, sizeof(cuerpo), g_refresh);
    add(cuerpo, "&grant_type=refresh_token", sizeof(cuerpo));
    int64_t m = oauth("/token", cuerpo);
    if (m <= 0) return -1;
    const char *j = (const char *)g_json, *fin = j + m;
    char exp[24];
    if (!json_valor(j, fin, "access_token", g_access, sizeof(g_access))) {
        char e[80]; json_valor(j, fin, "error", e, sizeof(e)); yt_log("token no renovado", e);
        return -1;
    }
    json_valor(j, fin, "expires_in", exp, sizeof(exp));
    g_access_hasta = taek_ms() + (long)(num(exp) > 120 ? num(exp) - 60 : 60) * 1000;
    return 0;
}
static void cerrar_sesion(void) {
    elegir_disco();
    ar_borrar("/youtube/cuenta.txt");
    g_refresh[0] = g_access[0] = 0;
    cp(estado, "Sesion cerrada.", sizeof(estado));
    yt_log("cuenta", "sesion cerrada");
}
static int evento_esc(void);
static void iniciar_sesion(void) {
    cuenta_cargar();
    if (!g_cfg_id[0] || !g_cfg_secreto[0]) { cp(estado, "Falta /youtube.conf con client_id y client_secret en el USB.", sizeof(estado)); return; }
    static char cuerpo[1400];
    cuerpo[0] = 0;
    add(cuerpo, "client_id=", sizeof(cuerpo)); urlcod(cuerpo, sizeof(cuerpo), g_cfg_id);
    add(cuerpo, "&scope=http%3A%2F%2Fgdata.youtube.com%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fyoutube", sizeof(cuerpo));
    ocupado("Pidiendo un codigo a Google...", 1);
    int64_t m = oauth("/device/code", cuerpo);
    listo();
    static char dispositivo[600];
    char t[24];
    const char *j = (const char *)g_json, *fin = j + (m > 0 ? m : 0);
    if (m <= 0 || !json_valor(j, fin, "device_code", dispositivo, sizeof(dispositivo))) {
        char e[120] = ""; if (m > 0) json_valor(j, fin, "error", e, sizeof(e));
        estado[0] = 0; add(estado, "Google no dio codigo: ", sizeof(estado)); add(estado, e[0] ? e : "sin respuesta", sizeof(estado));
        yt_log("cuenta", estado); return;
    }
    json_valor(j, fin, "user_code", g_login_codigo, sizeof(g_login_codigo));
    if (!json_valor(j, fin, "verification_url", g_login_url, sizeof(g_login_url))) cp(g_login_url, "https://www.google.com/device", sizeof(g_login_url));
    json_valor(j, fin, "interval", t, sizeof(t));
    uint32_t intervalo = num(t) ? num(t) : 5;
    json_valor(j, fin, "expires_in", t, sizeof(t));
    long vence = taek_ms() + (long)(num(t) ? num(t) : 1800) * 1000;
    g_login_activo = 1;
    yt_log("cuenta", "codigo de dispositivo mostrado");
    long prox = taek_ms() + (long)intervalo * 1000;
    for (;;) {
        mostrar();
        if (evento_esc()) { cp(estado, "Inicio de sesion cancelado.", sizeof(estado)); break; }
        long ahora = taek_ms();
        if (ahora > vence) { cp(estado, "El codigo vencio. Vuelve a intentarlo.", sizeof(estado)); break; }
        if (ahora < prox) { taek_esperar(100); continue; }
        prox = ahora + (long)intervalo * 1000;
        cuerpo[0] = 0;
        add(cuerpo, "client_id=", sizeof(cuerpo)); urlcod(cuerpo, sizeof(cuerpo), g_cfg_id);
        add(cuerpo, "&client_secret=", sizeof(cuerpo)); urlcod(cuerpo, sizeof(cuerpo), g_cfg_secreto);
        add(cuerpo, "&code=", sizeof(cuerpo)); urlcod(cuerpo, sizeof(cuerpo), dispositivo);
        add(cuerpo, "&grant_type=http%3A%2F%2Foauth.net%2Fgrant_type%2Fdevice%2F1.0", sizeof(cuerpo));
        m = oauth("/token", cuerpo);
        if (m <= 0) continue;
        j = (const char *)g_json; fin = j + m;
        if (json_valor(j, fin, "refresh_token", g_refresh, sizeof(g_refresh)) && g_refresh[0]) {
            json_valor(j, fin, "access_token", g_access, sizeof(g_access));
            json_valor(j, fin, "expires_in", t, sizeof(t));
            g_access_hasta = taek_ms() + (long)(num(t) > 120 ? num(t) - 60 : 60) * 1000;
            cuenta_guardar_refresh();
            cp(estado, "Sesion iniciada. Ya puedes ver Inicio y Suscripciones de tu cuenta.", sizeof(estado));
            yt_log("cuenta", "sesion iniciada");
            break;
        }
        char e[80]; json_valor(j, fin, "error", e, sizeof(e));
        if (igual(e, "authorization_pending")) continue;
        if (igual(e, "slow_down")) { intervalo += 5; continue; }
        estado[0] = 0; add(estado, "Google rechazo el inicio de sesion: ", sizeof(estado)); add(estado, e, sizeof(estado));
        yt_log("cuenta", estado);
        break;
    }
    g_login_activo = 0;
}

/* ------------------------------------------------------------------ listas */
static int buscar(void) {
    limpiar_res();
    cp(buscada, consulta, sizeof(buscada));
    ocupado("Buscando en YouTube...", 0);
    static char cuerpo[1024];
    cuerpo[0] = 0; add(cuerpo, CONTEXTO, sizeof(cuerpo)); add(cuerpo, "\"query\":\"", sizeof(cuerpo));
    int n = lg(cuerpo);
    for (const char *q = consulta; *q && n + 3 < (int)sizeof(cuerpo); q++) { if (*q == '"' || *q == '\\') cuerpo[n++] = '\\'; cuerpo[n++] = *q; }
    cuerpo[n] = 0; add(cuerpo, "\"}", sizeof(cuerpo));
    struct resp r;
    long t0 = taek_ms();
    int64_t m = http(&c_api, "youtubei.googleapis.com", "POST", "/youtubei/v1/search?prettyPrint=false", EXTRA_API,
                     cuerpo, (uint32_t)lg(cuerpo), g_json, JSON_CAP - 1, &r, 0);
    listo();
    if (m <= 0 || r.estado != 200) {
        estado[0] = 0; add(estado, "La busqueda fallo (HTTP ", sizeof(estado)); addn(estado, (uint64_t)(r.estado > 0 ? r.estado : 0), sizeof(estado));
        add(estado, ", codigo ", sizeof(estado)); addn(estado, (uint64_t)(m < 0 ? -m : 0), sizeof(estado)); add(estado, ")", sizeof(estado));
        yt_log("busqueda fallida", estado);
        return -1;
    }
    const char *j = (const char *)g_json, *fin = j + m;
    g_json_n = m;                                           /* H202: Shorts vuelve a leer la respuesta */
    for (const char *p = j; nres < MAX_RES && (p = hallar(p, fin, "\"compactVideoRenderer\":{")); ) {
        const char *a = p + 23, *b = json_bloque(a, fin);
        p = b;
        struct video *v = &res[nres];
        memset(v, 0, sizeof(*v));
        const char *id = hallar(a, b, "\"videoId\":\"");
        if (!id || id + 22 > b) continue;
        json_cadena(id + 10, b, v->id, sizeof(v->id));
        if (lg(v->id) != 11) continue;
        int repetido = 0;
        for (int i = 0; i < nres; i++) if (igual(res[i].id, v->id)) repetido = 1;
        if (repetido) continue;
        json_texto_de(a, b, "title", v->titulo, sizeof(v->titulo));
        json_texto_de(a, b, "shortBylineText", v->canal, sizeof(v->canal));
        if (!json_texto_de(a, b, "lengthText", v->dur, sizeof(v->dur))) cp(v->dur, "EN VIVO", sizeof(v->dur));
        json_texto_de(a, b, "viewCountText", v->vistas, sizeof(v->vistas));
        nres++;
    }
    const char *vd = hallar(j, fin, "\"visitorData\":\"");          /* H186: para el cliente VISIONOS */
    if (vd) json_cadena(vd + 14, fin, g_visitante, sizeof(g_visitante));
    estado[0] = 0; addn(estado, (uint64_t)nres, sizeof(estado)); add(estado, " videos para \"", sizeof(estado)); add(estado, buscada, sizeof(estado));
    add(estado, "\" en ", sizeof(estado)); addn(estado, (uint64_t)(taek_ms() - t0), sizeof(estado)); add(estado, " ms (", sizeof(estado));
    addn(estado, (uint64_t)m / 1024, sizeof(estado)); add(estado, " KiB de JSON)", sizeof(estado));
    yt_log("busqueda", estado);
    seccion[0] = 0; add(seccion, "Resultados para \"", sizeof(seccion)); add(seccion, buscada, sizeof(seccion)); add(seccion, "\"", sizeof(seccion));
    return nres;
}
/* Sin visitorData no hay alta calidad (VISIONOS): una busqueda minima lo consigue */
static void asegurar_visitante(void) {
    if (g_visitante[0]) return;
    static const char *cuerpo = "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"" VERSION "\",\"androidSdkVersion\":30}},\"query\":\"x\"}";
    struct resp r;
    int64_t m = http(&c_api, "youtubei.googleapis.com", "POST", "/youtubei/v1/search?prettyPrint=false", EXTRA_API,
                     cuerpo, (uint32_t)lg(cuerpo), g_json, JSON_CAP - 1, &r, 0);
    if (m <= 0) return;
    const char *vd = hallar((const char *)g_json, (const char *)g_json + m, "\"visitorData\":\"");
    if (vd) json_cadena(vd + 14, (const char *)g_json + m, g_visitante, sizeof(g_visitante));
}

/* H188: tileRenderer del cliente de TV -> res[]. Solo videos (con watchEndpoint). */
static int tiles(const char *j, const char *fin) {
    int antes = nres;
    for (const char *p = j; nres < MAX_RES && (p = hallar(p, fin, "\"tileRenderer\":{")); ) {
        const char *a = p + 15, *b = json_bloque(a, fin);
        p = b;
        if (hallar(a, b, "TILE_CONTENT_TYPE_CHANNEL") || hallar(a, b, "TILE_CONTENT_TYPE_PLAYLIST")) continue;
        const char *id = hallar(a, b, "\"watchEndpoint\":{\"videoId\":\"");
        if (!id) continue;
        struct video *v = &res[nres];
        memset(v, 0, sizeof(*v));
        json_cadena(id + 27, b, v->id, sizeof(v->id));
        if (lg(v->id) != 11) continue;
        int repetido = 0;
        for (int i = 0; i < nres; i++) if (igual(res[i].id, v->id)) repetido = 1;
        if (repetido) continue;
        const char *meta = hallar(a, b, "\"tileMetadataRenderer\"");
        json_texto_de(meta ? meta : a, b, "title", v->titulo, sizeof(v->titulo));
        const char *ov = hallar(a, b, "\"thumbnailOverlayTimeStatusRenderer\"");
        if (!ov || !json_texto_de(ov, b, "thumbnailOverlayTimeStatusRenderer", v->dur, sizeof(v->dur)) || !v->dur[0]) cp(v->dur, "EN VIVO", sizeof(v->dur));
        const char *l1 = meta ? hallar(meta, b, "\"lineRenderer\"") : 0;
        if (l1) {
            json_texto_de(l1, b, "lineRenderer", v->canal, sizeof(v->canal));
            const char *l2 = hallar(l1 + 14, b, "\"lineRenderer\"");
            if (l2) json_texto_de(l2, b, "lineItemRenderer", v->vistas, sizeof(v->vistas));
        }
        nres++;
    }
    return nres - antes;
}
static int64_t tv_pedir(const char *ruta, const char *extra_cuerpo, int con_token) {
    static char cuerpo[1200], extra[3400];
    cuerpo[0] = 0;
    add(cuerpo, "{\"context\":{\"client\":{\"clientName\":\"TVHTML5\",\"clientVersion\":\"" TV_VERSION "\",\"hl\":\"es\",\"gl\":\"PE\"}},", sizeof(cuerpo));
    add(cuerpo, extra_cuerpo, sizeof(cuerpo)); add(cuerpo, "}", sizeof(cuerpo));
    extra[0] = 0;
    add(extra, "Content-Type: application/json\r\nUser-Agent: Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version\r\n"
               "Origin: https://www.youtube.com\r\nX-Youtube-Client-Name: 7\r\nX-Youtube-Client-Version: " TV_VERSION "\r\n", sizeof(extra));
    if (con_token) { add(extra, "Authorization: Bearer ", sizeof(extra)); add(extra, g_access, sizeof(extra)); add(extra, "\r\n", sizeof(extra)); }
    struct resp r;
    int64_t m = http(&c_web, "www.youtube.com", "POST", ruta, extra, cuerpo, (uint32_t)lg(cuerpo), g_json, JSON_CAP - 1, &r, 0);
    con_cerrar(&c_web);
    if (m > 0 && r.estado != 200) {
        char e[80] = "HTTP "; addn(e, (uint64_t)r.estado, sizeof(e)); yt_log("tv fallo", e);
        return -100 - r.estado;
    }
    return m;
}
static void cargar_inicio(void) {
    limpiar_res();
    long t0 = taek_ms();
    if (con_cuenta() && !token()) {
        ocupado("Cargando tu inicio...", 1);
        int64_t m = tv_pedir("/youtubei/v1/browse?prettyPrint=false", "\"browseId\":\"FEwhat_to_watch\"", 1);
        listo();
        if (m > 0) tiles((const char *)g_json, (const char *)g_json + m);
        cp(seccion, "Recomendados para tu cuenta", sizeof(seccion));
    }
    if (!nres && nhist) {
        /* Sin cuenta: lo relacionado con lo ultimo que viste */
        for (int h = 0; h < nhist && h < 3 && nres < MAX_RES; h++) {
            ocupado("Buscando videos para ti...", 1 + h * 30);
            char c[40] = "\"videoId\":\""; add(c, g_hist[h].id, sizeof(c)); add(c, "\"", sizeof(c));
            int64_t m = tv_pedir("/youtubei/v1/next?prettyPrint=false", c, 0);
            listo();
            if (m > 0) tiles((const char *)g_json, (const char *)g_json + m);
            if (nres >= 24) break;
        }
        seccion[0] = 0; add(seccion, "Porque viste \"", sizeof(seccion)); add(seccion, g_hist[0].titulo, sizeof(seccion)); add(seccion, "\"", sizeof(seccion));
    }
    if (!nres) cp(seccion, nhist || con_cuenta() ? "No llego nada para el inicio." : "Mira algo (o inicia sesion en Cuenta) y aqui apareceran recomendaciones.", sizeof(seccion));
    estado[0] = 0; addn(estado, (uint64_t)nres, sizeof(estado)); add(estado, " videos en ", sizeof(estado)); addn(estado, (uint64_t)(taek_ms() - t0), sizeof(estado)); add(estado, " ms", sizeof(estado));
    yt_log("inicio", estado);
}
static void cargar_subs(void) {
    limpiar_res();
    void armar_canales(void);
    if (!con_cuenta()) { armar_canales(); return; }
    if (token()) { armar_canales(); cp(seccion, "No se pudo renovar la sesion (mira la pestana Cuenta). Canales de tu historial.", sizeof(seccion)); return; }
    ocupado("Cargando suscripciones...", 1);
    long t0 = taek_ms();
    int64_t m = tv_pedir("/youtubei/v1/browse?prettyPrint=false", "\"browseId\":\"FEsubscriptions\"", 1);
    listo();
    if (m > 0) tiles((const char *)g_json, (const char *)g_json + m);
    estado[0] = 0; addn(estado, (uint64_t)nres, sizeof(estado)); add(estado, " videos de suscripciones en ", sizeof(estado));
    addn(estado, (uint64_t)(taek_ms() - t0), sizeof(estado)); add(estado, " ms (", sizeof(estado)); addn(estado, (uint64_t)(m > 0 ? m / 1024 : 0), sizeof(estado)); add(estado, " KiB)", sizeof(estado));
    yt_log("suscripciones", estado);
    armar_canales();
}

/* ------------------------------------------------------------------ paso 4: nodos de canal (renderChannels) */
/* Con cuenta, los canales del feed de suscripciones; sin cuenta, los de tu historial. La afinidad sale de cuantas
 * veces aparece el canal en el historial (no se inventa: el prototipo sumaba un azar fijo por canal). */
#define MAX_CAN 40
struct canal { char nombre[96]; uint32_t uid; int videos, vistos, nivel; uint32_t seg; int del_feed; };
static struct canal g_can[MAX_CAN];
static int ncan, can_sel, can_desde;
static uint32_t fnv32(const char *s) { uint32_t h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }
/* Videos de varios canales: YouTube escribe "Canal y 3 mas"; el nodo (y la busqueda) usan solo el primero */
static void sin_y_mas(char *s) {
    int n = lg(s), i = n;
    static const char *MAS = " m\xC3\xA1s";
    int lm = lg(MAS);
    if (n <= lm || !igual(s + n - lm, MAS)) return;
    i = n - lm;
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') i--;
    if (i >= 3 && i < n - lm && s[i - 1] == ' ' && s[i - 2] == 'y' && s[i - 3] == ' ') s[i - 3] = 0;
}
static struct canal *canal_de(const char *nombre_) {
    char nombre[96]; cp(nombre, nombre_, sizeof(nombre)); sin_y_mas(nombre);
    if (!nombre[0]) return 0;
    for (int i = 0; i < ncan; i++) if (igual(g_can[i].nombre, nombre)) return &g_can[i];
    if (ncan >= MAX_CAN) return 0;
    struct canal *c = &g_can[ncan++];
    memset(c, 0, sizeof(*c));
    cp(c->nombre, nombre, sizeof(c->nombre)); c->uid = fnv32(nombre);
    return c;
}
void armar_canales(void) {
    ncan = 0; can_sel = 0; can_desde = 0;
    for (int i = 0; i < nres; i++) { struct canal *c = canal_de(res[i].canal); if (c) { c->videos++; c->seg += dur_de_texto(res[i].dur) / 1000; c->del_feed = 1; } }
    int del_feed = ncan;
    for (int i = 0; i < nhist; i++) {
        struct canal *c = del_feed ? 0 : canal_de(g_hist[i].canal);
        char nom[96]; cp(nom, g_hist[i].canal, sizeof(nom)); sin_y_mas(nom);
        if (del_feed) for (int k = 0; k < ncan; k++) if (igual(g_can[k].nombre, nom)) c = &g_can[k];
        if (!c) continue;
        c->vistos++;
        if (!del_feed) { c->videos++; c->seg += g_hist[i].dur_ms ? g_hist[i].dur_ms / 1000 : dur_de_texto(g_hist[i].dur) / 1000; }
    }
    for (int i = 0; i < ncan; i++) g_can[i].nivel = 1 + g_can[i].vistos > 10 ? 10 : 1 + g_can[i].vistos;
    for (int i = 1; i < ncan; i++)       /* los mas vistos primero */
        for (int k = i; k > 0 && g_can[k].vistos > g_can[k - 1].vistos; k--) { struct canal t = g_can[k]; g_can[k] = g_can[k - 1]; g_can[k - 1] = t; }
    seccion[0] = 0;
    addn(seccion, (uint64_t)ncan, sizeof(seccion)); add(seccion, " CANALES // ", sizeof(seccion));
    if (del_feed) { addn(seccion, (uint64_t)nres, sizeof(seccion)); add(seccion, " VIDEOS NUEVOS EN TU FEED", sizeof(seccion)); }
    else { add(seccion, con_cuenta() ? "DE TU HISTORIAL" : "SIN CUENTA: DE TU HISTORIAL", sizeof(seccion)); }
    if (!ncan) cp(seccion, con_cuenta() ? "No llego nada de las suscripciones." : "Sin cuenta y sin historial: mira algo o inicia sesion en Cuenta.", sizeof(seccion));
}

/* ------------------------------------------------------------------ paso 4: perfil (renderVault) */
/* El prototipo media atributos por categoria; InnerTube no da la categoria, asi que el radar usa cinco medidas
 * reales del historial y del USB. Niveles con los escalones del prototipo (STAT_STEPS 0, 1, 3, 6, 10). */
#define NATR 5
static const char *ATR_NOMBRE[NATR] = { "CONSTANCIA", "RESISTENCIA", "AMPLITUD", "ENFOQUE", "ARCHIVO" };
static const char *ATR_UNIDAD[NATR] = { "VIDEOS", "MIN", "CANALES", "%", "DESCARGAS" };
static const int ATR_ESCALON[NATR][5] = { { 0, 1, 3, 6, 10 }, { 0, 5, 20, 60, 180 }, { 0, 1, 3, 6, 10 }, { 0, 25, 50, 75, 95 }, { 0, 1, 3, 6, 10 } };
static int atr_pts[NATR], atr_nivel[NATR];
static float atr_valor[NATR];
static uint32_t g_min_vistos;
static int g_descargas;
static void cargar_perfil(void) {
    limpiar_res();
    /* lo ultimo visto, para el REGISTRO (y para poder abrirlo con Intro) */
    for (int i = 0; i < nhist && nres < 10; i++) { res[nres] = g_hist[i]; res[nres].mini = 0; res[nres].mini_estado = 0; nres++; }
    uint64_t ms = 0, hecho = 0, total = 0;
    int canales = 0;
    for (int i = 0; i < nhist; i++) {
        ms += g_hist[i].pos_ms;
        if (g_hist[i].dur_ms) { hecho += g_hist[i].pos_ms > g_hist[i].dur_ms ? g_hist[i].dur_ms : g_hist[i].pos_ms; total += g_hist[i].dur_ms; }
        char ci[96]; cp(ci, g_hist[i].canal, sizeof(ci)); sin_y_mas(ci);
        int nuevo = ci[0] != 0;
        for (int k = 0; k < i && nuevo; k++) { char ck[96]; cp(ck, g_hist[k].canal, sizeof(ck)); sin_y_mas(ck); if (igual(ck, ci)) nuevo = 0; }
        canales += nuevo;
    }
    g_descargas = 0;
    elegir_disco();
    struct taek_entrada *e;
    int n = ap_listar("/youtube/descargas", &e, 256);
    for (int i = 0; n > 0 && i < n; i++) if (e[i].carpeta && lg(e[i].nombre) == 11) g_descargas++;
    if (n > 0) free(e);
    g_min_vistos = (uint32_t)(ms / 60000);
    atr_pts[0] = nhist; atr_pts[1] = (int)g_min_vistos; atr_pts[2] = canales;
    atr_pts[3] = total ? (int)(hecho * 100 / total) : 0; atr_pts[4] = g_descargas;
    for (int k = 0; k < NATR; k++) {
        int lvl = 1;
        for (int s = 0; s < 5; s++) if (atr_pts[k] >= ATR_ESCALON[k][s]) lvl = s + 1;
        float frac = lvl >= 5 ? 1 : (float)(atr_pts[k] - ATR_ESCALON[k][lvl - 1]) / (float)(ATR_ESCALON[k][lvl] - ATR_ESCALON[k][lvl - 1] > 0 ? ATR_ESCALON[k][lvl] - ATR_ESCALON[k][lvl - 1] : 1);
        atr_nivel[k] = lvl;
        atr_valor[k] = (float)(lvl - 1) + frac + 0.3f;
        if (atr_valor[k] > 5) atr_valor[k] = 5;
    }
    seccion[0] = 0;
    addn(seccion, (uint64_t)nhist, sizeof(seccion)); add(seccion, " EVENTOS // ", sizeof(seccion));
    addn(seccion, g_min_vistos, sizeof(seccion)); add(seccion, " MIN VISTOS // ", sizeof(seccion));
    addn(seccion, (uint64_t)canales, sizeof(seccion)); add(seccion, " CANALES", sizeof(seccion));
}
static void cargar_hist(void) {
    limpiar_res();
    for (int i = 0; i < nhist && nres < MAX_RES; i++) { res[nres] = g_hist[i]; res[nres].mini = 0; res[nres].mini_estado = 0; nres++; }
    seccion[0] = 0; add(seccion, "Historial en este USB (", sizeof(seccion)); addn(seccion, (uint64_t)nhist, sizeof(seccion)); add(seccion, ")  \xC2\xB7  Supr quita", sizeof(seccion));
}
/* Descargas: /youtube/descargas/<id>/info.txt (clave=valor) */
static uint8_t *info_leer(const char *id, uint64_t *n) {
    char r[200] = "/youtube/descargas/"; add(r, id, sizeof(r)); add(r, "/info.txt", sizeof(r));
    return ap_leer_todo(r, n, 256 * 1024);
}
static void info_campo(char *s, char *fin, const char *clave, char *o, int cap) {
    char k[40] = "\n"; add(k, clave, sizeof(k)); add(k, "=", sizeof(k));
    char *p = despues_de(s, fin, k);
    int i = 0;
    if (p) while (p < fin && *p != '\n' && i + 1 < cap) o[i++] = *p++;
    o[i] = 0;
}
static void cargar_desc(void) {
    limpiar_res();
    elegir_disco();
    struct taek_entrada *e;
    int n = ap_listar("/youtube/descargas", &e, 256);
    for (int i = 0; n > 0 && i < n && nres < MAX_RES; i++) {
        if (!e[i].carpeta || lg(e[i].nombre) != 11) continue;
        uint64_t k = 0;
        uint8_t *b = info_leer(e[i].nombre, &k);
        if (!b) continue;
        char *s = (char *)b, *fin = s + k;
        struct video *v = &res[nres];
        memset(v, 0, sizeof(*v));
        cp(v->id, e[i].nombre, sizeof(v->id));
        char lado[16], mb[24];
        info_campo(s, fin, "titulo", v->titulo, sizeof(v->titulo));
        info_campo(s, fin, "canal", v->canal, sizeof(v->canal));
        info_campo(s, fin, "dur", v->dur, sizeof(v->dur));
        info_campo(s, fin, "lado", lado, sizeof(lado));
        info_campo(s, fin, "mib", mb, sizeof(mb));
        v->vistas[0] = 0; add(v->vistas, "Descargado ", sizeof(v->vistas)); add(v->vistas, lado, sizeof(v->vistas)); add(v->vistas, "p \xC2\xB7 ", sizeof(v->vistas));
        add(v->vistas, mb, sizeof(v->vistas)); add(v->vistas, " MiB", sizeof(v->vistas));
        free(b);
        nres++;
    }
    if (n > 0) free(e);
    seccion[0] = 0; add(seccion, "Descargas en este USB (", sizeof(seccion)); addn(seccion, (uint64_t)nres, sizeof(seccion));
    add(seccion, ")  \xC2\xB7  se ven sin red  \xC2\xB7  Supr borra", sizeof(seccion));
}
static int g_cat;                    /* paso 2: categoria de Inicio (0 = TODOS); los nombres estan con el menu */
static const char *const *g_cat_consulta;
static void cargar_categoria(void) {
    /* Una categoria es una busqueda real; lo escrito en Buscar no se pierde */
    char guarda_c[128], guarda_b[128];
    cp(guarda_c, consulta, sizeof(guarda_c)); cp(guarda_b, buscada, sizeof(guarda_b));
    cp(consulta, g_cat_consulta[g_cat], sizeof(consulta));
    buscar();
    if (nres) { seccion[0] = 0; add(seccion, "Resultados de YouTube para \"", sizeof(seccion)); add(seccion, consulta, sizeof(seccion)); add(seccion, "\"", sizeof(seccion)); }
    cp(consulta, guarda_c, sizeof(consulta)); cp(buscada, guarda_b, sizeof(buscada));
}
/* H202: SHORTS. Una busqueda real de "#shorts": los reelItemRenderer que traiga y los videos de hasta 3 minutos. */
static int g_shorts_total, g_shorts_reels;
static void cargar_shorts(void) {
    char guarda_c[128], guarda_b[128];
    cp(guarda_c, consulta, sizeof(guarda_c)); cp(guarda_b, buscada, sizeof(guarda_b));
    cp(consulta, "#shorts", sizeof(consulta));
    g_json_n = 0;
    buscar();
    const char *j = (const char *)g_json, *fin = j + (g_json_n > 0 ? g_json_n : 0);
    int reels = 0;
    for (const char *p = j; g_json_n > 0 && nres < MAX_RES && (p = hallar(p, fin, "\"reelItemRenderer\":{")); ) {
        const char *a = p + 19, *b = json_bloque(a, fin);
        p = b;
        struct video *v = &res[nres];
        memset(v, 0, sizeof(*v));
        const char *id = hallar(a, b, "\"videoId\":\"");
        if (!id || id + 22 > b) continue;
        json_cadena(id + 10, b, v->id, sizeof(v->id));
        if (lg(v->id) != 11) continue;
        int repetido = 0;
        for (int i = 0; i < nres; i++) if (igual(res[i].id, v->id)) repetido = 1;
        if (repetido) continue;
        json_texto_de(a, b, "headline", v->titulo, sizeof(v->titulo));
        json_texto_de(a, b, "viewCountText", v->vistas, sizeof(v->vistas));
        json_texto_de(a, b, "shortBylineText", v->canal, sizeof(v->canal));
        cp(v->dur, "SHORT", sizeof(v->dur));
        nres++; reels++;
    }
    g_shorts_total = nres; g_shorts_reels = reels;
    int k = 0;
    for (int i = 0; i < nres; i++) {
        uint32_t d = dur_de_texto(res[i].dur);
        if (igual(res[i].dur, "SHORT") || (d > 0 && d <= 180000)) { if (k != i) res[k] = res[i]; k++; }
        else if (res[i].mini) { free(res[i].mini); res[i].mini = 0; }
    }
    if (k) nres = k;
    sel = 0; desde_fila = 0;
    cp(consulta, guarda_c, sizeof(consulta)); cp(buscada, guarda_b, sizeof(buscada));
    seccion[0] = 0; addn(seccion, (uint64_t)nres, sizeof(seccion)); add(seccion, " R\xC3\x81\x46\x41GAS // #shorts", sizeof(seccion));
    char m[120] = ""; addn(m, (uint64_t)nres, sizeof(m)); add(m, " de ", sizeof(m)); addn(m, (uint64_t)g_shorts_total, sizeof(m));
    add(m, " (", sizeof(m)); addn(m, (uint64_t)reels, sizeof(m)); add(m, " reels)", sizeof(m));
    yt_log("shorts", m);
}
static void cargar_pestana(void) {
    estado[0] = 0;
    if (pest == P_INICIO && g_cat && g_cat_consulta) cargar_categoria();
    else if (pest == P_INICIO) cargar_inicio();
    else if (pest == P_BUSCAR) { if (consulta[0]) buscar(); else { limpiar_res(); cp(seccion, "Escribe y pulsa Intro para buscar.", sizeof(seccion)); } }
    else if (pest == P_SUBS) cargar_subs();
    else if (pest == P_HIST) cargar_hist();
    else if (pest == P_DESC) cargar_desc();
    else if (pest == P_PERFIL) cargar_perfil();
    else if (pest == P_SHORTS) cargar_shorts();
    else { limpiar_res(); cuenta_cargar(); seccion[0] = 0; }
}

/* ------------------------------------------------------------------ miniaturas */
static int CARTA_AN, CARTA_AL, MINI_AN, MINI_AL, COLS, X0, Y0, ESP;
static uint8_t *g_jpg;
static void escalar(const uint8_t *rgba, int w, int h, uint32_t *d, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        int sy = y * h / dh;
        for (int x = 0; x < dw; x++) {
            const uint8_t *p = rgba + ((size_t)sy * (size_t)w + (size_t)(x * w / dw)) * 4;
            d[(size_t)y * (size_t)dw + (size_t)x] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        }
    }
}
static int cargar_mini(struct video *v) {
    if (!g_jpg) g_jpg = malloc(512 * 1024);
    if (!g_jpg) return -1;
    char ruta[64] = "/vi/"; add(ruta, v->id, sizeof(ruta)); add(ruta, "/mqdefault.jpg", sizeof(ruta));
    struct resp r;
    int64_t n = http(&c_img, "i.ytimg.com", "GET", ruta, "User-Agent: TAEK-OS/1.0\r\n", 0, 0, g_jpg, 512 * 1024, &r, 0);
    if (n <= 0 || r.estado != 200) { v->mini_estado = -1; return -1; }
    int w, h, ch;
    uint8_t *rgba = stbi_load_from_memory(g_jpg, (int)n, &w, &h, &ch, 4);
    if (!rgba) { v->mini_estado = -1; return -1; }
    v->mini = malloc((size_t)MINI_AN * (size_t)MINI_AL * 4);
    if (v->mini) { escalar(rgba, w, h, v->mini, MINI_AN, MINI_AL); v->mini_estado = 1; }
    free(rgba);
    return 0;
}

/* ------------------------------------------------------------------ dibujo */
static void texto_cortado_l(struct lienzo *l, int x, int y, int px, uint32_t c, uint32_t a, const char *s, int ancho) {
    static char b[256];
    cp(b, s, sizeof(b));
    if (texto_ancho(px, b) <= ancho) { texto(l, x, y, px, c, a, 0, b); return; }
    int n = lg(b);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80) n--;
        b[n] = 0;
        char t[260]; cp(t, b, sizeof(t)); add(t, "...", sizeof(t));
        if (texto_ancho(px, t) <= ancho) { texto(l, x, y, px, c, a, 0, t); return; }
    }
}
static void texto_cortado(int x, int y, int px, uint32_t c, uint32_t a, const char *s, int ancho) { texto_cortado_l(&L, x, y, px, c, a, s, ancho); }
static int g_flujo_ok;                        /* se define mas abajo; el HUD lo muestra */
/* ------------------------------------------------------------------ NekoTube QM-2, paso 1: piezas de la base visual */
/* Texto en Share Tech Mono (telemetria, etiquetas); al terminar se vuelve a Chakra Petch */
static int tmono(struct lienzo *l, int x, int y, int px, uint32_t c, uint32_t a, const char *s) {
    g_fuente = F_MONO; int r = texto(l, x, y, px, c, a, 0, s); g_fuente = F_CHAKRA; return r;
}
static int tmono_ancho(int px, const char *s) { g_fuente = F_MONO; int r = texto_ancho(px, s); g_fuente = F_CHAKRA; return r; }
static void tmono_cortado(struct lienzo *l, int x, int y, int px, uint32_t c, uint32_t a, const char *s, int ancho) {
    g_fuente = F_MONO; texto_cortado_l(l, x, y, px, c, a, s, ancho); g_fuente = F_CHAKRA;
}
/* letter-spacing del CSS: el texto letra a letra con `esp` pixeles de mas */
static int u8_largo(const char *s) {
    unsigned char b = (unsigned char)*s;
    int n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
    for (int i = 1; i < n; i++) if (!s[i]) return i;
    return n;
}
static int texto_esp(struct lienzo *l, int x, int y, int px, uint32_t c, uint32_t a, int esp, const char *s) {
    char u[5];
    int primero = 1;
    while (*s) {
        int n = u8_largo(s);
        for (int i = 0; i < n; i++) u[i] = s[i];
        u[n] = 0;
        if (!primero) x += esp;
        x = texto(l, x, y, px, c, a, 0, u);
        primero = 0; s += n;
    }
    return x;
}
static int ancho_esp(int px, int esp, const char *s) {
    int n = 0;
    for (const char *p = s; *p; p += u8_largo(p)) n++;
    return texto_ancho(px, s) + (n > 1 ? (n - 1) * esp : 0);
}
static void hexn(char *o, uint64_t v, int n) {
    static const char h[] = "0123456789ABCDEF";
    for (int i = 0; i < n; i++) o[i] = h[(v >> ((n - 1 - i) * 4)) & 15];
    o[n] = 0;
}
/* clip-path: --chamfer (corte arriba a la derecha y abajo a la izquierda), solo el filo */
static void chaflan_borde(struct lienzo *l, int x, int y, int an, int al, int c, int g, uint32_t col) {
    rect(l, x, y, an - c, g, col);
    rect(l, x + an - g, y + c, g, al - c, col);
    rect(l, x + c, y + al - g, an - c, g, col);
    rect(l, x, y, g, al - c, col);
    for (int k = 0; k < c; k++) { rect(l, x + an - c + k, y + k, g, 1, col); rect(l, x + k, y + al - c + k, g, 1, col); }
}
/* .panel: fondo --panel, borde izquierdo amarillo tenue y el filo amarillo en el corte */
static void panel_q(struct lienzo *l, int x, int y, int an, int al, int c) {
    chaflan(l, x, y, an, al, c, premul(230, Q_PANEL));
    rect(l, x, y, S(2), al - c, premul(89, Q_Y));
    for (int k = 0; k < c; k++) rect(l, x + an - c + k, y + k, S(2), 1, premul(255, Q_Y));
}
/* Miniatura con las esquinas cortadas y las lineas de barrido de .vc-thumb::after */
static void imagen_chaflan(const uint32_t *o, int ow, int oh, int x, int y, int w, int h, int c) {
    for (int yy = 0; yy < h; yy++) {
        int fy = y + yy;
        if (fy < 0 || fy >= AL) continue;
        int a = 0, b = w;
        if (yy < c) b -= c - yy;
        if (yy >= h - c) a += yy - (h - c);
        uint32_t *d = L.px + (size_t)fy * (size_t)AN;
        const uint32_t *f = o + (size_t)(yy * oh / h) * (size_t)ow;
        int barrido = yy % 3 == 0;
        for (int xx = a; xx < b; xx++) {
            int fx = x + xx;
            if (fx < 0 || fx >= AN) continue;
            uint32_t p = f[xx * ow / w];
            d[fx] = barrido ? 0xFF000000u | (cubrir(p, 209) & 0xFFFFFFu) : p;
        }
    }
}
/* Un cuadro con borde de 1 px (las teclas de #prompts y .key) */
static int tecla_q(struct lienzo *l, int x, int ybase, const char *k) {
    int w = tmono_ancho(S(12), k) + S(14), h = S(20), y = ybase - S(15);
    rect(l, x, y, w, 1, premul(255, Q_Y)); rect(l, x, y + h - 1, w, 1, premul(255, Q_Y));
    rect(l, x, y, 1, h, premul(255, Q_Y)); rect(l, x + w - 1, y, 1, h, premul(255, Q_Y));
    tmono(l, x + S(7), ybase, S(12), Q_Y, 255, k);
    return x + w;
}

/* ------------------------------------------------------------------ fondo (#bg), pintado una vez */
static uint32_t *g_fondo;
static void fondo_pintar(void) {
    struct lienzo f = { g_fondo, AN, AL };
    limpiar(&f, FONDO);
    /* .grid-floor: rejilla cian en perspectiva desde media pantalla, que aparece hacia abajo (mask hasta 70 %) */
    int hy = AL / 2, cx = AN / 2;
    for (int k = -34; k <= 34; k++)
        for (int t = 0; t < 10; t++) {
            int a0 = t * 100, a1 = (t + 1) * 100;    /* milesimas del recorrido */
            int x0 = cx + k * (S(22) + (S(300) - S(22)) * a0 / 1000), y0 = hy + (AL - hy) * a0 / 1000;
            int x1 = cx + k * (S(22) + (S(300) - S(22)) * a1 / 1000), y1 = hy + (AL - hy) * a1 / 1000;
            int m = (a0 + a1) / 2, alfa = m >= 700 ? 41 : 41 * m / 700;
            if (alfa > 0) linea(&f, x0, y0, x1, y1, 1, premul((uint32_t)alfa, Q_CIAN));
        }
    for (int k = 1; k <= 18; k++) {
        int m = k * k * 1000 / (18 * 18), y = hy + (AL - hy) * m / 1000;
        int alfa = m >= 700 ? 41 : 41 * m / 700;
        rect(&f, 0, y, AN, 1, premul((uint32_t)alfa, Q_CIAN));
    }
    /* .bg-band: franja amarilla rayada a 45 grados, girada 18 grados, al 58 % del ancho */
    int bcx = AN * 66 / 100, bcy = AL / 2, bw = AN * 16 / 100, paso = S(22) > 2 ? S(22) : 2, raya = S(10) > 1 ? S(10) : 1;
    for (int y = 0; y < AL; y++) {
        uint32_t *p = g_fondo + (size_t)y * (size_t)AN;
        for (int x = 0; x < AN; x++) {
            int u = ((x - bcx) * 951 + (y - bcy) * 309) / 1000;   /* distancia al eje de la franja */
            if (u < -bw / 2 - 1 || u > bw / 2 + 1) continue;
            if (u <= -bw / 2) p[x] = sobre(p[x], premul(89, Q_Y));
            else if (u >= bw / 2) p[x] = sobre(p[x], premul(38, Q_Y));
            else if ((x + y) % paso < raya) p[x] = sobre(p[x], premul(26, Q_Y));
        }
    }
    /* .stream: renglones de datos y la palabra grande, muy tenues */
    static const char *dato = "SYS.0x5917 // RING 3 // NEKOTUBE.QM2 // SYS_SENSORES // INNERTUBE // SYS_FLUJO // NEKOSAKA // ";
    for (int x = -S(40); x < AN; ) x = tmono(&f, x, AL * 11 / 100 + S(40), S(12), Q_CIAN, 38, dato) + S(12);
    for (int x = -S(200); x < AN; ) x = tmono(&f, x, AL * 88 / 100, S(11), Q_Y, 80, dato) + S(12);
    for (int x = -S(120); x < AN; ) x = texto_esp(&f, x, AL * 58 / 100 + S(72), S(72), Q_Y, 16, S(4), "NEKOTUBE // QM-2 // ") + S(20);
    /* .scanlines y .vignette en una pasada */
    for (int y = 0; y < AL; y++) {
        uint32_t *p = g_fondo + (size_t)y * (size_t)AN;
        float ny = ((float)y - (float)AL * 0.45f) / ((float)AL * 0.8f);
        for (int x = 0; x < AN; x++) {
            float nx = ((float)x - (float)AN * 0.4f) / ((float)AN * 0.75f);
            float t = (nx * nx + ny * ny - 0.12f) / 0.88f;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            uint32_t queda = 255u - (uint32_t)(204.0f * t);
            if (y % 3 == 0) queda = queda * 199u / 255u;
            p[x] = 0xFF000000u | (cubrir(p[x], queda) & 0xFFFFFFu);
        }
    }
}
static void fondo_poner(void) {
    if (!g_fondo) { g_fondo = malloc((size_t)AN * (size_t)AL * 4); if (g_fondo) fondo_pintar(); }
    if (g_fondo) memcpy(L.px, g_fondo, (size_t)AN * (size_t)AL * 4);
    else limpiar(&L, FONDO);
}

/* ------------------------------------------------------------------ telemetria real (SYS_SENSORES y SYS_INFO) */
static struct taek_sensores g_sns;
static struct taek_info g_info;
static long g_sns_r = -1, g_sns_t, g_info_t = -100000, g_fps_t, g_reloj_ms;
static int g_fps = -1, g_cuadros_fps, g_cpu_pct = -1, g_red_kbs = -1, g_reloj_s = -1;
static uint64_t g_cuadro;
static int g_cpu_barras[24], g_cpu_n;          /* paso 2: las ultimas 24 lecturas de CPU0, para el pie del menu */
static void telemetria(void) {
    long ahora = taek_ms();
    g_cuadro++; g_cuadros_fps++;
    if (ahora - g_fps_t >= 1000) { g_fps = (int)((long)g_cuadros_fps * 1000 / (ahora - g_fps_t)); g_cuadros_fps = 0; g_fps_t = ahora; }
    if (ahora - g_sns_t >= 500) {
        uint64_t tsc = g_sns.tsc, esp = g_sns.espera_ciclos, red = g_sns.rx_bytes + g_sns.tx_bytes;
        int antes = g_sns_r == 0;
        g_sns.op = 0;
        g_sns_r = taek_sensores(&g_sns);
        if (g_sns_r == 0 && antes) {
            if (g_sns.version >= 2 && g_sns.tsc > tsc) {
                uint64_t d = g_sns.tsc - tsc, e = g_sns.espera_ciclos - esp;
                g_cpu_pct = e >= d ? 0 : (int)(100 - e * 100 / d);
                g_cpu_barras[g_cpu_n % 24] = g_cpu_pct; g_cpu_n++;
            }
            uint64_t r = g_sns.rx_bytes + g_sns.tx_bytes;
            g_red_kbs = g_sns.red_presente && r >= red ? (int)((r - red) * 1000 / 1024 / (uint64_t)(ahora - g_sns_t)) : -1;
        }
        g_sns_t = ahora;
    }
    if (ahora - g_info_t >= 10000) {
        if (taek_info(&g_info, 0) == 0 && g_info.fecha_ok) {
            g_reloj_s = g_info.hora * 3600 + g_info.minuto * 60 + g_info.segundo; g_reloj_ms = ahora;
        }
        g_info_t = ahora;
    }
}
static int64_t dias_civil(int y, int m, int d) {           /* dias desde 1970-01-01 (H. Hinnant) */
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* ------------------------------------------------------------------ #hud: marca, telemetria, LOG y reloj */
#define HUD_AL S(78)
static uint32_t *g_tick_px;
static void hud_q(void) {
    int H = HUD_AL;
    for (int f = 0; f < H; f++) rect(&L, 0, f, AN, 1, premul((uint32_t)(230 - 128 * f / H), Q_BG));
    rect(&L, 0, H, AN, 1, Q_LINEA);
    /* .brand: NEKOTUBE con los canales corridos y la placa QM-2 */
    int x = S(24), yb = S(40);
    texto_esp(&L, x + S(2), yb, S(30), Q_ROJO, 255, S(4), "NEKOTUBE");
    texto_esp(&L, x - S(2), yb, S(30), Q_CIAN, 153, S(4), "NEKOTUBE");
    int fin = texto_esp(&L, x, yb, S(30), Q_Y, 255, S(4), "NEKOTUBE");
    int pw = tmono_ancho(S(11), "QM-2") + S(10);
    rect(&L, x, S(49), pw, S(15), premul(255, Q_Y));
    tmono(&L, x + S(5), S(60), S(11), Q_BG, 255, "QM-2");
    char sub[48] = " BUILD 0x", hx[12];
    if (g_info.gpu_id) { hexn(hx, g_info.gpu_id, 4); add(sub, hx, sizeof(sub)); } else add(sub, "----", sizeof(sub));
    add(sub, " // RING 3", sizeof(sub));
    int f2 = tmono(&L, x + pw + S(4), S(60), S(11), Q_DIM2, 255, sub);
    if (f2 > fin) fin = f2;
    /* .telemetry: celdas con datos que da el nucleo; sin dato, "--" */
    struct txt v[7];
    static const char *et[7] = { "FPS", "FRAME", "UPTIME", "HEAP", "TEMP", "CPU0", "NET" };
    for (int i = 0; i < 7; i++) t_ini(&v[i]);
    if (g_fps >= 0) t_num(&v[0], (uint64_t)g_fps); else t_add(&v[0], "--");
    t_num(&v[1], g_cuadro);
    uint64_t up = (uint64_t)taek_ms() / 1000;
    t_dos(&v[2], up / 3600); t_add(&v[2], ":"); t_dos(&v[2], up / 60 % 60); t_add(&v[2], ":"); t_dos(&v[2], up % 60);
    int sns_ok = g_sns_r == 0 && g_sns.version >= 1;
    uint32_t c_temp = Q_CIAN;
    if (sns_ok) { t_num(&v[3], g_sns.heap_en_uso >> 20); t_add(&v[3], " MiB"); } else t_add(&v[3], "--");
    if (sns_ok && g_sns.temp_ok) {
        t_num(&v[4], (uint64_t)(g_sns.temp_nucleo_c < 0 ? 0 : g_sns.temp_nucleo_c)); t_add(&v[4], " \xC2\xB0" "C");
        c_temp = g_sns.temp_nucleo_c >= 90 ? Q_ROJO : g_sns.temp_nucleo_c >= 65 ? Q_Y : Q_CIAN;
    } else t_add(&v[4], "--");
    if (g_cpu_pct >= 0) { t_num(&v[5], (uint64_t)g_cpu_pct); t_add(&v[5], " %"); } else t_add(&v[5], "--");
    if (g_red_kbs >= 0) { t_num(&v[6], (uint64_t)g_red_kbs); t_add(&v[6], " KB/s"); } else t_add(&v[6], "--");
    int cx = fin + S(22), cy = S(16), ch = S(44), gx = cx;
    int anchos[7], total = 0;
    for (int i = 0; i < 7; i++) {
        int a = tmono_ancho(S(14), v[i].s), b = tmono_ancho(S(10), et[i]);
        anchos[i] = (a > b ? a : b) + S(18);
        if (anchos[i] < S(64)) anchos[i] = S(64);
        total += anchos[i] + 1;
    }
    /* El reloj manda: si no cabe todo, las celdas del final se quedan fuera */
    int reloj_x = AN - S(24) - S(150);
    while (total > 0 && cx + total > reloj_x - S(40)) total -= anchos[6] + 1, anchos[6] = 0;
    rect(&L, cx, cy, total + 1, ch, Q_LINEA);
    for (int i = 0; i < 7 && anchos[i]; i++) {
        rect(&L, gx + 1, cy + 1, anchos[i], ch - 2, premul(255, Q_BG2));
        tmono(&L, gx + 1 + S(9), cy + S(15), S(10), Q_DIM, 255, et[i]);
        tmono(&L, gx + 1 + S(9), cy + S(35), S(14), i == 4 ? c_temp : Q_CIAN, 255, v[i].s);
        gx += anchos[i] + 1;
    }
    /* .clock: hora, fecha con el dia y la marca de tiempo en hexadecimal */
    char hora[16] = "--:--:--", fecha[40] = "----------", ep[16] = "0x--------";
    const char *dia = "---";
    if (g_reloj_s >= 0) {
        long s = (g_reloj_s + (taek_ms() - g_reloj_ms) / 1000) % 86400;
        struct txt t; t_ini(&t); t_dos(&t, (uint64_t)s / 3600); t_add(&t, ":"); t_dos(&t, (uint64_t)s / 60 % 60); t_add(&t, ":"); t_dos(&t, (uint64_t)s % 60);
        cp(hora, t.s, sizeof(hora));
        t_ini(&t); t_num(&t, g_info.anio); t_add(&t, "-"); t_dos(&t, g_info.mes); t_add(&t, "-"); t_dos(&t, g_info.dia);
        cp(fecha, t.s, sizeof(fecha));
        int64_t d = dias_civil(g_info.anio, g_info.mes, g_info.dia);
        static const char *DIAS[7] = { "DOM", "LUN", "MAR", "MI\xC3\x89", "JUE", "VIE", "S\xC3\x81\x42" };
        dia = DIAS[((d % 7) + 11) % 7];
        cp(ep, "0x", sizeof(ep)); hexn(ep + 2, (uint64_t)(d * 86400 + s), 8);
    }
    int der = AN - S(24);
    texto_esp(&L, der - ancho_esp(S(26), S(2), hora), S(34), S(26), Q_TXT, 255, S(2), hora);
    int dw = tmono_ancho(S(11), dia);
    tmono(&L, der - dw, S(51), S(11), Q_Y, 255, dia);
    tmono(&L, der - dw - S(6) - tmono_ancho(S(11), fecha), S(51), S(11), Q_DIM2, 255, fecha);
    tmono(&L, der - tmono_ancho(S(10), ep), S(65), S(10), Q_DIM, 255, ep);
    /* .ticker: LOG con lo que la app esta haciendo, corriendo de derecha a izquierda */
    int tx0 = cx + total + S(20), tx1 = der - S(160), ty = S(26), th = S(26);
    if (tx1 - tx0 < S(140)) return;
    rect(&L, tx0, ty, tx1 - tx0, 1, Q_LINEA); rect(&L, tx0, ty + th - 1, tx1 - tx0, 1, Q_LINEA);
    rect(&L, tx0, ty, 1, th, Q_LINEA); rect(&L, tx1 - 1, ty, 1, th, Q_LINEA);
    int tw = tmono_ancho(S(11), "LOG") + S(16);
    rect(&L, tx0 + 1, ty + 1, tw, th - 2, premul(255, Q_CIAN));
    tmono(&L, tx0 + 1 + S(8), ty + S(17), S(11), Q_BG, 255, "LOG");
    int wx = tx0 + 1 + tw + S(4), ww = tx1 - 1 - wx, wh = th - 2;
    if (ww <= 0) return;
    if (!g_tick_px) g_tick_px = malloc((size_t)AN * (size_t)(S(26) + 2) * 4);
    if (!g_tick_px) return;
    struct lienzo t = { g_tick_px, ww, wh };
    for (int f = 0; f < wh; f++) memcpy(g_tick_px + (size_t)f * (size_t)ww, L.px + (size_t)(ty + 1 + f) * (size_t)AN + wx, (size_t)ww * 4);
    char msg[600] = "";
    add(msg, estado[0] ? estado : seccion, sizeof(msg));
    add(msg, "  //  ", sizeof(msg)); addn(msg, (uint64_t)nres, sizeof(msg)); add(msg, " OBJ", sizeof(msg));
    add(msg, "  //  CALIDAD ", sizeof(msg)); addn(msg, (uint64_t)g_calidad, sizeof(msg)); add(msg, "P", sizeof(msg));
    add(msg, g_flujo_ok ? "  //  SYS_FLUJO OK" : "  //  SIN SYS_FLUJO", sizeof(msg));
    if (sns_ok && g_sns.ip_ok) {
        add(msg, "  //  IP ", sizeof(msg));
        for (int i = 0; i < 4; i++) { if (i) add(msg, ".", sizeof(msg)); addn(msg, g_sns.ip[i], sizeof(msg)); }
    }
    add(msg, con_cuenta() ? "  //  CUENTA: SESION INICIADA" : "  //  SIN CUENTA", sizeof(msg));
    int mw = tmono_ancho(S(12), msg), ciclo = mw + ww;
    int desp = (int)((taek_ms() * (long)(S(60) > 0 ? S(60) : 1) / 1000) % (ciclo > 0 ? ciclo : 1));
    tmono(&t, ww - desp, S(17), S(12), Q_DIM2, 255, msg);
    for (int f = 0; f < wh; f++) memcpy(L.px + (size_t)(ty + 1 + f) * (size_t)AN + wx, g_tick_px + (size_t)f * (size_t)ww, (size_t)ww * 4);
}

/* ------------------------------------------------------------------ paso 3: transiciones (Transition del prototipo) */
/* Poligono convexo suavizado: cobertura por la distancia a cada lado (como triangulo, con n lados) */
static void poligono(struct lienzo *l, const float *px, const float *py, int n, uint32_t c) {
    float area = 0, minx = px[0], maxx = px[0], miny = py[0], maxy = py[0];
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += px[i] * py[j] - px[j] * py[i];
        if (px[i] < minx) minx = px[i];
        if (px[i] > maxx) maxx = px[i];
        if (py[i] < miny) miny = py[i];
        if (py[i] > maxy) maxy = py[i];
    }
    if (area == 0.0f) return;
    float s = area > 0 ? 1.0f : -1.0f, ex[12], ey[12], el[12], ox_[12], oy_[12];
    if (n > 12) n = 12;
    /* Un lado de largo cero (esquina sin recorte) no limita nada: se descarta, o daria distancia 0 en todo el interior */
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float dx = px[j] - px[i], dy = py[j] - py[i], lgd = largo(dx, dy);
        if (lgd < 0.01f) continue;
        ex[m] = dx; ey[m] = dy; el[m] = lgd; ox_[m] = px[i]; oy_[m] = py[i]; m++;
    }
    n = m;
    int xa = (int)minx - 1, xb = (int)maxx + 2, ya = (int)miny - 1, yb = (int)maxy + 2;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > l->an) xb = l->an;
    if (yb > l->al) yb = l->al;
    /* Paso 3: por filas. Cada lado es lineal en x (d = a x + b): el tramo donde todos dan >= 0,5 se rellena entero y
     * solo los bordes, donde alguno queda entre -0,5 y 0,5, se calculan pixel a pixel. */
    for (int y = ya; y < yb; y++) {
        uint32_t *p = l->px + (uint64_t)y * l->an;
        float fy = (float)y + 0.5f, a[12], b[12];
        float o0 = (float)xa, o1 = (float)xb, i0 = (float)xa, i1 = (float)xb;
        int vacio = 0;
        for (int i = 0; i < n; i++) {
            a[i] = -s * ey[i] / el[i];
            b[i] = s * (ex[i] * (fy - oy_[i]) + ey[i] * ox_[i]) / el[i];
            /* fx = x + 0.5 cumple a fx + b >= k */
            if (a[i] > 1e-6f) { float lo = (-0.5f - b[i]) / a[i] - 0.5f, li = (0.5f - b[i]) / a[i] - 0.5f; if (lo > o0) o0 = lo; if (li > i0) i0 = li; }
            else if (a[i] < -1e-6f) { float hi = (-0.5f - b[i]) / a[i] - 0.5f, hi2 = (0.5f - b[i]) / a[i] - 0.5f; if (hi < o1) o1 = hi; if (hi2 < i1) i1 = hi2; }
            else { if (b[i] <= -0.5f) vacio = 1; if (b[i] < 0.5f) i1 = i0 - 1; }
        }
        if (vacio || o1 < o0) continue;
        int xo0 = (int)o0 - 1, xo1 = (int)o1 + 2, xi0 = (int)i0 + 1, xi1 = (int)i1;
        if (xo0 < xa) xo0 = xa;
        if (xo1 > xb) xo1 = xb;
        if (xi1 < xi0) { xi0 = xo1; xi1 = xo1; }
        if (xi0 < xo0) xi0 = xo0;
        if (xi1 > xo1) xi1 = xo1;
        for (int x = xo0; x < xo1; x++) {
            if (x == xi0 && xi1 > xi0) { rect(l, xi0, y, xi1 - xi0, 1, c); x = xi1 - 1; continue; }
            float fx = (float)x + 0.5f, d = 1e9f;
            for (int i = 0; i < n && d > -0.5f; i++) { float di = a[i] * fx + b[i]; if (di < d) d = di; }
            if (d <= -0.5f) continue;
            p[x] = sobre(p[x], d >= 0.5f ? c : cubrir(c, (uint32_t)((d + 0.5f) * 255.0f)));
        }
    }
}
static uint32_t g_azar = 0x9E3779B9u;
static uint32_t azar(void) { g_azar ^= g_azar << 13; g_azar ^= g_azar >> 17; g_azar ^= g_azar << 5; return g_azar; }
static float azar_f(float a, float b) { return a + (b - a) * (float)(azar() % 10000u) / 10000.0f; }
static int azar_i(int a, int b) { return a + (int)(azar() % (uint32_t)(b - a + 1)); }
static float raiz(float v) { if (v <= 0) return 0; float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(v)); return r; }
static float lim01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }
static float e_io(float t) { t = lim01(t); return t < 0.5f ? 4 * t * t * t : 1 - (2 - 2 * t) * (2 - 2 * t) * (2 - 2 * t) / 2; }   /* (.7,0,.2,1) */
static float e_out(float t) { t = lim01(t); float u = 1 - t; return 1 - u * u * u; }                                           /* ease-out, (.16,1,.3,1) */
static float e_in(float t) { t = lim01(t); return t * t * t; }                                                                   /* (.5,0,.75,0) */
static float e_atras(float t) { t = lim01(t); float u = t - 1; return 1 + 2.70158f * u * u * u + 1.70158f * u * u; }       /* (.34,1.56,.64,1) */
static float pasos(float t, int n) { t = lim01(t); return t >= 1 ? 1 : (float)(int)(t * (float)n) / (float)n; }              /* steps(n) */
static uint32_t mezcla(uint32_t a, uint32_t b, float t) { return entre(premul(255, a), premul(255, b), (uint32_t)(lim01(t) * 255.0f)) & 0xFFFFFFu; }

/* scramble(): letras al azar que se van fijando de izquierda a derecha */
static void mezclar_letras(char *o, int cap, const char *s, long trans, long dur) {
    static const char G[] = "ABCDEFGHJKLMNPQRSTUVWXYZ0123456789#$%&@*+=/<>";
    int n = 0;
    for (const char *p = s; *p; p += u8_largo(p)) n++;
    int fijas = dur <= 0 || trans >= dur ? n : trans <= 0 ? 0 : (int)((long)n * trans / dur);
    int k = 0, i = 0;
    for (const char *p = s; *p && k + 5 < cap; i++) {
        int l = u8_largo(p);
        if (i < fijas || *p == ' ') for (int j = 0; j < l; j++) o[k++] = p[j];
        else o[k++] = G[azar() % (sizeof(G) - 1)];
        p += l;
    }
    o[k] = 0;
}

/* Copia transformada de una imagen ARGB premultiplicada: centro, escala x/y, giro, alfa y recorte (fracciones de la
 * imagen, como clip-path: inset). Muestreo bilineal, para que el texto agrandado no se vea en escalones. */
static void pegar_tr(const uint32_t *src, int sw, int sh, float cx, float cy, float sx, float sy, float rot,
                     uint32_t alfa, float c_izq, float c_der, float c_arr, float c_aba) {
    if (alfa == 0 || sx <= 0.01f || sy <= 0.01f) return;
    float rad = rot * 3.14159265f / 180.0f, cs = cosen(rad), sn = sen(rad);
    float hw = (float)sw * sx / 2, hh = (float)sh * sy / 2;
    float ex = (hw * (cs < 0 ? -cs : cs) + hh * (sn < 0 ? -sn : sn)), ey = (hw * (sn < 0 ? -sn : sn) + hh * (cs < 0 ? -cs : cs));
    int x0 = (int)(cx - ex) - 1, x1 = (int)(cx + ex) + 2, y0 = (int)(cy - ey) - 1, y1 = (int)(cy + ey) + 2;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > AN) x1 = AN;
    if (y1 > AL) y1 = AL;
    float ul = c_izq * (float)sw, ur = (1 - c_der) * (float)sw, vt = c_arr * (float)sh, vb = (1 - c_aba) * (float)sh;
    for (int y = y0; y < y1; y++) {
        uint32_t *d = L.px + (size_t)y * (size_t)AN;
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float u = (dx * cs + dy * sn) / sx + (float)sw / 2 - 0.5f, v = (-dx * sn + dy * cs) / sy + (float)sh / 2 - 0.5f;
            if (u < ul - 0.5f || u >= ur - 0.5f || v < vt - 0.5f || v >= vb - 0.5f || u < -0.5f || v < -0.5f || u > (float)sw - 0.5f || v > (float)sh - 0.5f) continue;
            int iu = (int)(u + 1) - 1, iv = (int)(v + 1) - 1;
            uint32_t fu = (uint32_t)((u - (float)iu) * 255.0f), fv = (uint32_t)((v - (float)iv) * 255.0f);
            uint32_t p00 = iu >= 0 && iv >= 0 ? src[iv * sw + iu] : 0, p10 = iu + 1 < sw && iv >= 0 ? src[iv * sw + iu + 1] : 0;
            uint32_t p01 = iu >= 0 && iv + 1 < sh ? src[(iv + 1) * sw + iu] : 0, p11 = iu + 1 < sw && iv + 1 < sh ? src[(iv + 1) * sw + iu + 1] : 0;
            uint32_t p = entre(entre(p00, p10, fu), entre(p01, p11, fu), fv);
            if (alfa < 255) p = cubrir(p, alfa);
            if (p >> 24) d[x] = sobre(d[x], p);
        }
    }
}

/* Franja de una fila donde a*x + b queda entre lo y hi */
static int tramo(float a, float b, float lo, float hi, float *x0, float *x1) {
    if (a > -1e-6f && a < 1e-6f) { if (b < lo || b > hi) return 0; *x0 = -1e9f; *x1 = 1e9f; return 1; }
    float p = (lo - b) / a, q = (hi - b) / a;
    if (p > q) { float t = p; p = q; q = t; }
    if (q < *x0 || p > *x1) return 0;
    if (p > *x0) *x0 = p;
    if (q < *x1) *x1 = q;
    return *x1 > *x0;
}
static void fila(int y, float x0, float x1, uint32_t c) {
    int a = x0 < 0 ? 0 : (int)(x0 + 0.5f), b = x1 > (float)AN ? AN : (int)(x1 + 0.5f);
    if (b > a) rect(&L, a, y, b - a, 1, c);
}

enum { TR_CUCHILLAS, TR_IRIS, TR_PERSIANA, TR_TV, TR_PARTIR, TR_EVA, NTR };
static const char *TR_NOMBRE[NTR] = { "cuchillas", "iris", "persiana", "tv", "partir", "eva" };
enum { F_ENTRA, F_CUBIERTO, F_SALE };
#define TR_MAX 12
static struct {
    int activa, tipo, fase, n, dir, alt, colapsa, anti, layout, ep, invierte, canal;
    long t0, t_et, t_ini;
    long d_in[TR_MAX], r_in[TR_MAX], d_out[TR_MAX], r_out[TR_MAX];
    int orden[TR_MAX];
    uint32_t pal[TR_MAX], gc, ca;
    float ang, ox, oy, ex, ey, rot_a, rot_b, et_rot, et_dx, regla;
    int et_corte;
    long t_entra, t_sale, t_espera;
    char etq[48], sub[80], ruta[32], hex[12], hora[16], crc[12];
} g_tr;
static long g_rev_t0 = -100000;       /* entrada de la cuadricula */
static float g_radar_rot;             /* paso 4: giro con que llega la forma del radar */
static int g_rev_var;
static int g_tr_bolsa[NTR], g_tr_nb, g_tr_ultimo = -1;
static int tr_siguiente(void) {
    if (!g_tr_nb) {
        for (int i = 0; i < NTR; i++) g_tr_bolsa[i] = i;
        for (int i = NTR - 1; i > 0; i--) { int j = (int)(azar() % (uint32_t)(i + 1)), t = g_tr_bolsa[i]; g_tr_bolsa[i] = g_tr_bolsa[j]; g_tr_bolsa[j] = t; }
        if (g_tr_bolsa[NTR - 1] == g_tr_ultimo) { int t = g_tr_bolsa[0]; g_tr_bolsa[0] = g_tr_bolsa[NTR - 1]; g_tr_bolsa[NTR - 1] = t; }
        g_tr_nb = NTR;
    }
    return g_tr_ultimo = g_tr_bolsa[--g_tr_nb];
}
static void barajar(int *v, int n) { for (int i = 0; i < n; i++) v[i] = i; for (int i = n - 1; i > 0; i--) { int j = (int)(azar() % (uint32_t)(i + 1)), t = v[i]; v[i] = v[j]; v[j] = t; } }

/* Etiqueta .t-label (o el titulo grande de EVA) pintada aparte para poder agrandarla, girarla y recortarla */
static uint32_t *g_et_px; static int g_et_an, g_et_al;
static void tr_etiqueta(long ms, int solo_texto) {
    if (!g_et_px) g_et_px = malloc((size_t)AN * (size_t)S(200) * 4);
    if (!g_et_px) return;
    char t[96]; mezclar_letras(t, sizeof(t), g_tr.etq, ms, solo_texto ? 0 : 260);
    int px = S(72), esp = S(4), tw = ancho_esp(px, esp, g_tr.etq), bw = tw + S(36), bh = px + S(8);
    const char *m[3] = { g_tr.ruta, g_tr.hex, g_tr.hora };
    char prog[160] = "";
    if (ocupado_pct >= 0) { cp(prog, ocupado_msg, sizeof(prog)); if (ocupado_pct > 0) { add(prog, " ", sizeof(prog)); addn(prog, (uint64_t)(ocupado_pct > 100 ? 100 : ocupado_pct), sizeof(prog)); add(prog, "%", sizeof(prog)); } m[2] = prog; }
    int mw = 0; for (int i = 0; i < 3; i++) mw += tmono_ancho(S(14), m[i]) + S(16) + (i ? S(14) : 0);
    int w = solo_texto ? tw + S(4) : (bw > mw ? bw : mw), h = solo_texto ? px + S(8) : bh + S(8) + S(22);
    if ((size_t)w * (size_t)h > (size_t)AN * (size_t)S(200)) w = AN * S(200) / h;
    g_et_an = w; g_et_al = h;
    struct lienzo e = { g_et_px, w, h };
    limpiar(&e, 0);
    int asc = (int)fuente_tam(px)->asc;
    if (solo_texto) { texto_esp(&e, 0, S(4) + asc, px, 0xF4F0E6u, 255, esp, t); return; }
    chaflan(&e, 0, 0, bw, bh, S(22), premul(255, Q_BG));
    texto_esp(&e, S(18), S(4) + asc, px, Q_TXT, 255, esp, t);
    static const uint32_t fondo_m[3] = { Q_Y, Q_CIAN, Q_TXT };
    int x = 0, y = bh + S(8);
    for (int i = 0; i < 3; i++) {
        int sw = tmono_ancho(S(14), m[i]) + S(16);
        rect(&e, x, y, sw, S(22), premul(255, fondo_m[i]));
        tmono(&e, x + S(8), y + S(17), S(14), Q_BG, 255, m[i]);
        x += sw + S(14);
    }
}
/* popLabel / outLabel */
static void tr_dibujar_etiqueta(long now) {
    if (g_tr.fase == F_ENTRA) return;
    float cx = (float)AN / 2, cy = (float)AL / 2;
    if (g_tr.fase == F_CUBIERTO) {
        long el = now - g_tr.t_et;
        tr_etiqueta(el, 0);
        float s, a, r = g_tr.et_rot, dx = 0, dy = 0;
        if (g_tr.tipo == TR_PARTIR) {      /* golpe: escala 3 -> .93 y sacudida */
            float t = lim01((float)el / 300.0f);
            if (t < 0.55f) { float k = e_out(t / 0.55f); s = 3 - 2.07f * k; a = k; }
            else if (t < 0.7f) { float k = (t - 0.55f) / 0.15f; s = 0.93f + 0.07f * k; dx = (float)S(8) * k; a = 1; }
            else if (t < 0.85f) { float k = (t - 0.7f) / 0.15f; s = 1; dx = (float)S(8) - (float)S(14) * k; dy = (float)S(4) * k; a = 1; }
            else { float k = (t - 0.85f) / 0.15f; s = 1; dx = -(float)S(6) * (1 - k); dy = (float)S(4) * (1 - k); a = 1; }
        } else {
            float t = lim01((float)el / 190.0f);
            if (t < 0.7f) { float k = e_out(t / 0.7f); s = 2.2f - 1.25f * k; a = k; r = g_tr.et_rot - 6 * (1 - k); }
            else { float k = (t - 0.7f) / 0.3f; s = 0.95f + 0.05f * k; a = 1; }
        }
        pegar_tr(g_et_px, g_et_an, g_et_al, cx + dx, cy + dy, s * 1.4f, s * 1.4f, r, (uint32_t)(a * 255.0f), 0, 0, 0, 0);
    } else {
        long el = now - g_tr.t0;
        if (el >= 170) return;
        float t = e_in((float)el / 170.0f);
        float ci = 0, cd = 0, ca = 0, cb = 0;
        if (g_tr.et_corte == 0) ci = t; else if (g_tr.et_corte == 1) cd = t; else { ca = t / 2; cb = t / 2; }
        pegar_tr(g_et_px, g_et_an, g_et_al, cx + g_tr.et_dx * t, cy, 1.4f, 1.4f, g_tr.et_rot, (uint32_t)((1 - t) * 255.0f), ci, cd, ca, cb);
    }
}

/* Cada tipo: progreso de su fase. el = ms desde el comienzo de la fase */
static void tr_cuchillas(int fase, long el) {
    float W = 2.4f * (float)(AN > AL ? AN : AL), rad = g_tr.ang * 3.14159265f / 180.0f, cs = cosen(rad), sn = sen(rad);
    float cx = (float)AN / 2, cy = (float)AL / 2;
    for (int i = 0; i < g_tr.n; i++) {
        int d = g_tr.alt && (i & 1) ? -g_tr.dir : g_tr.dir;
        float off;
        if (fase == F_ENTRA) off = -(float)d * (1 - e_io((float)(el - g_tr.r_in[i]) / (float)g_tr.d_in[i]));
        else if (fase == F_SALE) off = (float)d * e_io((float)(el - g_tr.r_out[i]) / (float)g_tr.d_out[i]);
        else off = 0;
        float v0 = -W / 2 + (float)i * W / (float)g_tr.n, v1 = v0 + W / (float)g_tr.n + W * 0.004f;
        float u0 = -W / 2 + off * W, u1 = W / 2 + off * W;
        for (int y = 0; y < AL; y++) {
            float dy = (float)y + 0.5f - cy, x0 = 0, x1 = (float)AN;
            /* u = (x - cx) cs + dy sn ; v = -(x - cx) sn + dy cs */
            if (!tramo(cs, -cx * cs + dy * sn, u0, u1, &x0, &x1)) continue;
            if (!tramo(-sn, cx * sn + dy * cs, v0, v1, &x0, &x1)) continue;
            fila(y, x0 - 0.5f, x1 - 0.5f, premul(255, g_tr.pal[i]));
        }
    }
}
static void circulo_rejilla(float cx, float cy, float r) {
    uint32_t bg = premul(255, Q_BG);
    int paso = S(28) > 2 ? S(28) : 2;
    for (int y = (int)(cy - r) < 0 ? 0 : (int)(cy - r); y < AL && (float)y <= cy + r; y++) {
        float dy = (float)y + 0.5f - cy, h = r * r - dy * dy;
        if (h <= 0) continue;
        float c = raiz(h);
        int a = (int)(cx - c) < 0 ? 0 : (int)(cx - c), b = (int)(cx + c) > AN ? AN : (int)(cx + c);
        if (b <= a) continue;
        rect(&L, a, y, b - a, 1, bg);
        if (y % paso == 0) rect(&L, a, y, b - a, 1, g_tr.gc);
        else for (int x = ((a + paso - 1) / paso) * paso; x < b; x += paso) L.px[(size_t)y * (size_t)AN + x] = sobre(L.px[(size_t)y * (size_t)AN + x], g_tr.gc);
    }
}
static void anillo_q(float cx, float cy, float r, float g, uint32_t c) {
    for (int y = (int)(cy - r - g) < 0 ? 0 : (int)(cy - r - g); y < AL && (float)y <= cy + r + g; y++) {
        float dy = (float)y + 0.5f - cy, ho = (r + g) * (r + g) - dy * dy, hi = (r - g) * (r - g) - dy * dy;
        if (ho <= 0) continue;
        float co = raiz(ho), ci = hi > 0 ? raiz(hi) : 0;
        if (ci > 0) { fila(y, cx - co, cx - ci, c); fila(y, cx + ci, cx + co, c); }
        else fila(y, cx - co, cx + co, c);
    }
}
static void tr_iris(int fase, long el) {
    float R = raiz((float)AN * (float)AN + (float)AL * (float)AL);
    float cx = g_tr.ox, cy = g_tr.oy, r = R;
    if (fase == F_ENTRA) r = R * e_io((float)el / 300.0f);
    else if (fase == F_SALE) { float t = e_io((float)el / 330.0f); cx += (g_tr.ex - g_tr.ox) * t; cy += (g_tr.ey - g_tr.oy) * t; r = R * (1 - t); }
    circulo_rejilla(cx, cy, r);
    long tr = taek_ms() - g_tr.t_ini;          /* el anillo dura 420 ms desde el principio */
    if (tr < 420) { float t = (float)tr / 420.0f; anillo_q(g_tr.ox, g_tr.oy, 5 + (R - 5) * e_out(t), (float)S(1), premul((uint32_t)((1 - t) * 255.0f), Q_Y)); }
}
static void tr_persiana(int fase, long el) {
    for (int i = 0; i < g_tr.n; i++) {
        int y0 = AL * i / g_tr.n, y1 = AL * (i + 1) / g_tr.n + 1, h = y1 - y0;
        float off = 0, esc = 1;
        if (fase == F_ENTRA) off = (float)(i & 1 ? 1 : -1) * (1 - e_io((float)(el - g_tr.orden[i] * 20) / 200.0f));
        else if (fase == F_SALE) {
            float t = e_io((float)(el - g_tr.orden[i] * 16) / 210.0f);
            if (g_tr.colapsa) esc = 1 - t; else off = (float)(i & 1 ? -1 : 1) * t;
        }
        int hh = (int)((float)h * esc), yy = y0 + (h - hh) / 2;
        rect(&L, (int)(off * (float)AN), yy, AN, hh, premul(255, g_tr.pal[i & 1]));
    }
}
static uint32_t brillo(uint32_t c, float b) {
    uint32_t r = (uint32_t)((float)((c >> 16) & 255) * b), g = (uint32_t)((float)((c >> 8) & 255) * b), bl = (uint32_t)((float)(c & 255) * b);
    return (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (bl > 255 ? 255 : bl);
}
static void tr_tv(int fase, long el) {
    float sx = 1, sy = 1, b = 1, k = 1;
    uint32_t base = Q_BG;
    if (fase == F_ENTRA) {
        float t = e_out((float)el / 280.0f);
        if (t < 0.45f) { sy = 0.004f; sx = t / 0.45f; k = 0; }
        else { k = (t - 0.45f) / 0.55f; sy = 0.004f + 0.996f * k; }
        base = mezcla(Q_TXT, Q_BG, k);
    } else if (fase == F_SALE) {
        float t = e_in((float)el / 300.0f);
        if (t < 0.6f) { float q = t / 0.6f; sy = 1 - 0.996f * q; b = 1 + 3 * q; }
        else { sy = 0.004f; sx = 1 - (t - 0.6f) / 0.4f; b = 4; }
    }
    int w = (int)((float)AN * sx), h = (int)((float)AL * sy);
    if (h < 2) h = 2;
    int x0 = (AN - w) / 2, y0 = (AL - h) / 2;
    if (w <= 0) return;
    if (sy < 0.02f) { rect(&L, x0, y0, w, h, premul(255, brillo(k > 0 ? base : Q_TXT, b))); return; }
    /* ruido de 160 x 90 a pantalla, al 35 %, con el tinte rojo-cian */
    for (int cy = 0; cy < 90; cy++) {
        int ya = y0 + h * cy / 90, yb = y0 + h * (cy + 1) / 90;
        for (int cx = 0; cx < 160; cx++) {
            int xa = x0 + w * cx / 160, xb = x0 + w * (cx + 1) / 160;
            uint32_t v = azar() & 255, gris = v << 16 | v << 8 | v;
            uint32_t c = mezcla(base, gris, 0.35f * k);
            c = mezcla(c, mezcla(Q_ROJO, Q_CIAN, (float)cx / 160.0f), 0.15f * k);
            rect(&L, xa, ya, xb - xa, yb - ya, premul(255, brillo(c, b)));
        }
    }
    if (k > 0.8f && sy > 0.9f) {
        char s[80] = "SE\xC3\x91\x41L PERDIDA // RESINCRONIZANDO CH-"; char n[4]; struct txt t; t_ini(&t); t_dos(&t, (uint64_t)g_tr.canal); cp(n, t.s, sizeof(n)); add(s, n, sizeof(s));
        g_fuente = F_MONO; texto_esp(&L, x0 + w * 5 / 100, y0 + h * 6 / 100 + S(16), S(16), brillo(Q_Y, b), 255, S(2), s); g_fuente = F_CHAKRA;
    }
}
static void tr_triangulo(const float *px, const float *py, float dx, float dy, float rot, uint32_t c) {
    float rad = rot * 3.14159265f / 180.0f, cs = cosen(rad), sn = sen(rad), cx = (float)AN / 2, cy = (float)AL / 2, x[3], y[3];
    for (int i = 0; i < 3; i++) { float ax = px[i] - cx, ay = py[i] - cy; x[i] = cx + ax * cs - ay * sn + dx; y[i] = cy + ax * sn + ay * cs + dy; }
    poligono(&L, x, y, 3, c);
}
static void tr_partir(int fase, long el) {
    float W = (float)AN, H = (float)AL;
    float ax_[3], ay_[3], bx_[3], by_[3];
    if (g_tr.anti) { float a1[3] = { 0, W, W }, a2[3] = { 0, 0, H }, b1[3] = { 0, W, 0 }, b2[3] = { 0, H, H }; memcpy(ax_, a1, 12); memcpy(ay_, a2, 12); memcpy(bx_, b1, 12); memcpy(by_, b2, 12); }
    else { float a1[3] = { 0, W, 0 }, a2[3] = { 0, 0, H }, b1[3] = { W, W, 0 }, b2[3] = { 0, H, H }; memcpy(ax_, a1, 12); memcpy(ay_, a2, 12); memcpy(bx_, b1, 12); memcpy(by_, b2, 12); }
    float fa[2] = { g_tr.anti ? 0.6f : -0.6f, -0.6f }, fb[2] = { g_tr.anti ? -0.6f : 0.6f, 0.6f };
    /* entrada: desde fa/fb hasta su sitio; salida: hasta 1,8 veces fa/fb, girando */
    float ka = 0, kb = 0, ra = 0, rb = 0;
    if (fase == F_ENTRA) { ka = 1 - e_io((float)el / 230.0f); kb = 1 - e_io((float)(el - 30) / 230.0f); }
    else if (fase == F_SALE) { float t = e_in((float)el / 300.0f); ka = kb = 1.8f * t; ra = g_tr.rot_a * t; rb = g_tr.rot_b * t; }
    tr_triangulo(ax_, ay_, fa[0] * W * ka, fa[1] * H * ka, ra, premul(255, g_tr.ca));
    tr_triangulo(bx_, by_, fb[0] * W * kb, fb[1] * H * kb, rb, premul(255, Q_BG));
}
/* Tarjeta de titulo EVA: negro, episodio, regla amarilla, titulo estirado que se escribe por pasos y bloque de datos */
static void tr_eva(int fase, long el, long now) {
    float alfa = 1;
    float c_arr = 0, c_aba = 0;
    int inv = 0;
    if (fase == F_ENTRA) alfa = lim01((float)el / 70.0f);
    else if (fase == F_SALE) {
        if (g_tr.invierte) { int paso = (int)(el * 3 / 120); inv = paso == 1; alfa = paso >= 2 ? 1 - lim01((float)(el - 80) / 40.0f) : 1; }
        else { float t = e_io((float)el / 180.0f); if (g_tr.layout == 2) c_arr = t; else if (g_tr.layout == 1) c_aba = t; else { c_arr = t / 2; c_aba = t / 2; } }
    }
    uint32_t a = (uint32_t)(alfa * 255.0f);
    uint32_t fondo = inv ? 0xF4F0E6u : 0x000000u, tinta = inv ? 0x000000u : 0xF4F0E6u;
    int ya = (int)((float)AL * c_arr), yb = AL - (int)((float)AL * c_aba);
    rect(&L, 0, ya, AN, yb - ya, premul(a, fondo));
    if (alfa <= 0.01f || yb <= ya) return;
    int pad = AN * 9 / 100, der = g_tr.layout == 1;
    float esc_x = 2.0f, esc_y = 2.0f * 1.35f;
    tr_etiqueta(0, 1);
    int big_w = (int)((float)g_et_an * esc_x), big_h = (int)((float)g_et_al * esc_y);
    int bloque = S(30) + S(21) + S(3) + S(21) + big_h + S(54) + S(18);
    int y = g_tr.layout == 2 ? AL - AL * 14 / 100 - bloque : (AL - bloque) / 2;
    char ep[32] = "EP.", n[4]; struct txt t; t_ini(&t); t_dos(&t, (uint64_t)g_tr.ep); cp(n, t.s, sizeof(n)); add(ep, n, sizeof(ep));
    if (!g_tr.ep) cp(ep, "PR\xC3\x93LOGO", sizeof(ep));
    int ew = ancho_esp(S(30), S(9), ep);
    texto_esp(&L, der ? AN - pad - ew : pad, y + S(26), S(30), tinta, a, S(9), ep);
    int rw = (int)((float)(AN - 2 * pad) * g_tr.regla);
    rect(&L, der ? AN - pad - rw : pad, y + S(30) + S(21), rw, S(3), premul(a, inv ? 0x000000u : Q_Y));
    /* el titulo se escribe en pasos (steps) una vez cubierta la pantalla */
    float wipe = fase == F_ENTRA ? 0 : fase == F_CUBIERTO ? pasos((float)(now - g_tr.t_et) / 220.0f, lg(g_tr.etq) < 3 ? 3 : lg(g_tr.etq) > 10 ? 10 : lg(g_tr.etq)) : 1;
    float by = (float)(y + S(30) + S(45)) + (float)big_h / 2, bx = der ? (float)(AN - pad) - (float)big_w / 2 : (float)pad + (float)big_w / 2;
    if (inv) { for (int i = 0; i < g_et_an * g_et_al; i++) if (g_et_px[i]) g_et_px[i] = (g_et_px[i] & 0xFF000000u); }
    pegar_tr(g_et_px, g_et_an, g_et_al, bx, by, esc_x, esc_y, 0, a, 0, 1 - wipe, 0, 0);
    int sy = y + S(30) + S(45) + big_h + S(54);
    g_fuente = F_MONO;
    int sw = ancho_esp(S(18), S(4), g_tr.sub);
    texto_esp(&L, der ? AN - pad - sw : pad, sy, S(18), inv ? 0x000000u : Q_Y, a, S(4), g_tr.sub);
    g_fuente = F_CHAKRA;
    /* .data: ruta, hora, CRC y lo que se esta cargando */
    char l[4][120];
    cp(l[0], "RUTA ", 120); add(l[0], g_tr.ruta, 120);
    cp(l[1], "T ", 120); add(l[1], g_tr.hora, 120);
    cp(l[2], "CRC ", 120); add(l[2], g_tr.crc, 120);
    l[3][0] = 0;
    if (ocupado_pct >= 0) { cp(l[3], ocupado_msg, 120); if (ocupado_pct > 0) { add(l[3], " ", 120); addn(l[3], (uint64_t)(ocupado_pct > 100 ? 100 : ocupado_pct), 120); add(l[3], "%", 120); } }
    int dy = AL - AL * 4 / 100 - 3 * S(19);
    for (int i = 0; i < 4; i++) {
        if (!l[i][0]) continue;
        tmono(&L, AN - AN * 4 / 100 - tmono_ancho(S(12), l[i]), dy + i * S(19) - (l[3][0] ? S(19) : 0), S(12), i == 3 ? Q_Y : Q_DIM2, a, l[i]);
    }
}
static void tr_dibujar(void) {
    if (!g_tr.activa) return;
    long now = taek_ms(), el = now - g_tr.t0;
    switch (g_tr.tipo) {
    case TR_CUCHILLAS: tr_cuchillas(g_tr.fase, el); break;
    case TR_IRIS: tr_iris(g_tr.fase, el); break;
    case TR_PERSIANA: tr_persiana(g_tr.fase, el); break;
    case TR_TV: tr_tv(g_tr.fase, el); break;
    case TR_PARTIR: tr_partir(g_tr.fase, el); break;
    default: tr_eva(g_tr.fase, el, now); return;
    }
    tr_dibujar_etiqueta(now);
}
static void tr_preparar(int tipo, const char *etq, const char *sub, const char *ruta, int ox, int oy) {
    memset(&g_tr, 0, sizeof(g_tr));
    g_tr.tipo = tipo;
    cp(g_tr.etq, etq, sizeof(g_tr.etq)); cp(g_tr.sub, sub, sizeof(g_tr.sub)); cp(g_tr.ruta, ruta, sizeof(g_tr.ruta));
    cp(g_tr.hex, "0x", sizeof(g_tr.hex)); hexn(g_tr.hex + 2, azar(), 8);
    { long s = g_reloj_s >= 0 ? (g_reloj_s + (taek_ms() - g_reloj_ms) / 1000) % 86400 : taek_ms() / 1000 % 86400;
      struct txt t; t_ini(&t); t_dos(&t, (uint64_t)s / 3600); t_add(&t, ":"); t_dos(&t, (uint64_t)s / 60 % 60); t_add(&t, ":"); t_dos(&t, (uint64_t)s % 60);
      t_add(&t, "."); t_num(&t, (uint64_t)(taek_ms() % 1000)); cp(g_tr.hora, t.s, sizeof(g_tr.hora)); }
    { uint32_t h = 2166136261u; for (const char *p = etq; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u; h ^= (uint32_t)taek_ms(); h *= 16777619u; hexn(g_tr.crc, h, 8); }
    g_tr.et_rot = azar_f(-4, 2); g_tr.et_dx = azar_f((float)-S(60), (float)S(60)); g_tr.et_corte = azar_i(0, 2);
    g_tr.ox = ox >= 0 ? (float)ox : (float)AN * azar_f(0.3f, 0.7f); g_tr.oy = oy >= 0 ? (float)oy : (float)AL * azar_f(0.3f, 0.7f);
    g_tr.t_espera = azar_i(150, 260);
    static const uint32_t PAL[5] = { Q_Y, Q_CIAN, Q_ROJO, Q_BG2, Q_TXT };
    switch (tipo) {
    case TR_CUCHILLAS: {
        g_tr.ang = azar_f(12, 34) * (azar() & 1 ? -1.0f : 1.0f); g_tr.n = azar_i(3, 5);
        int o[5]; barajar(o, 5);
        for (int i = 0; i < g_tr.n - 1; i++) g_tr.pal[i] = PAL[o[i]];
        g_tr.pal[g_tr.n - 1] = Q_BG;
        g_tr.dir = azar() & 1 ? -1 : 1; g_tr.alt = azar() % 100 < 45;
        for (int i = 0; i < g_tr.n; i++) {
            g_tr.d_in[i] = azar_i(220, 290); g_tr.r_in[i] = i * azar_i(20, 50);
            g_tr.d_out[i] = azar_i(220, 310); g_tr.r_out[i] = (g_tr.n - i) * azar_i(20, 45);
            if (g_tr.d_in[i] + g_tr.r_in[i] > g_tr.t_entra) g_tr.t_entra = g_tr.d_in[i] + g_tr.r_in[i];
            if (g_tr.d_out[i] + g_tr.r_out[i] > g_tr.t_sale) g_tr.t_sale = g_tr.d_out[i] + g_tr.r_out[i];
        }
        break; }
    case TR_IRIS:
        g_tr.gc = premul(77, azar() & 1 ? Q_CIAN : Q_Y);
        g_tr.ex = (float)AN * azar_f(0.1f, 0.9f); g_tr.ey = (float)AL * azar_f(0.1f, 0.9f);
        g_tr.t_entra = 300; g_tr.t_sale = 330; g_tr.t_espera = azar_i(150, 250);
        break;
    case TR_PERSIANA: {
        g_tr.n = azar_i(6, 12);
        static const uint32_t PARES[4][2] = { { Q_BG, Q_Y }, { Q_BG, Q_CIAN }, { Q_BG2, Q_BG }, { Q_BG, Q_ROJO } };
        int p = azar_i(0, 3); g_tr.pal[0] = PARES[p][0]; g_tr.pal[1] = PARES[p][1];
        int o[TR_MAX]; barajar(o, g_tr.n);
        for (int i = 0; i < g_tr.n; i++) g_tr.orden[o[i]] = i;
        g_tr.colapsa = azar() & 1;
        g_tr.t_entra = 200 + (g_tr.n - 1) * 20; g_tr.t_sale = 210 + (g_tr.n - 1) * 16; g_tr.t_espera = azar_i(140, 240);
        break; }
    case TR_TV: g_tr.canal = azar_i(1, 99); g_tr.t_entra = 280; g_tr.t_sale = 300; g_tr.t_espera = azar_i(220, 340); break;
    case TR_PARTIR:
        g_tr.anti = azar() & 1; g_tr.ca = PAL[azar_i(0, 2)];
        g_tr.rot_a = azar_f(-6, 6); g_tr.rot_b = azar_f(-6, 6);
        g_tr.t_entra = 260; g_tr.t_sale = 300; g_tr.t_espera = azar_i(140, 240) + 110;
        break;
    default: {
        static int ep = 0;                 /* 0: el prologo del arranque */
        g_tr.layout = azar_i(0, 2); g_tr.regla = azar_f(0.3f, 0.7f); g_tr.invierte = azar() & 1;
        g_tr.ep = ep++;
        g_tr.t_entra = 70; g_tr.t_sale = g_tr.invierte ? 120 : 180; g_tr.t_espera = 220 + azar_i(360, 520);
        break; }
    }
}
static void rev_empezar(void);
static int g_tr_cuadros;
static void tr_cuadro(void) {
    long t = taek_ms();
    g_tr_cuadros++;
    mostrar();
    long g = taek_ms() - t;
    if (g < 16) taek_esperar(16 - g);
}
/* Transition.run: cubre, llama a `medio` con la pantalla tapada (lo que tarde la red lo muestra la etiqueta), y
 * descubre. La entrada de la cuadricula empieza cuando la pantalla se destapa. */
static void tr_correr(int tipo, const char *etq, const char *sub, const char *ruta, int ox, int oy, void (*medio)(int), int arg) {
    tr_preparar(tipo, etq, sub, ruta, ox, oy);
    g_tr.activa = 1; g_tr.fase = F_ENTRA; g_tr.t0 = g_tr.t_ini = taek_ms();
    g_tr_cuadros = 0;
    while (taek_ms() - g_tr.t0 < g_tr.t_entra) tr_cuadro();
    int c_entra = g_tr_cuadros;
    g_tr.fase = F_CUBIERTO; g_tr.t_et = g_tr.t0 = taek_ms();
    if (g_tr.tipo == TR_EVA) tr_cuadro();
    long t_medio = taek_ms();
    if (medio) medio(arg);
    t_medio = taek_ms() - t_medio;
    long pop = g_tr.tipo == TR_PARTIR ? 300 : g_tr.tipo == TR_EVA ? 0 : 190;
    /* Si la carga se comio la espera, la etiqueta igual se ve completa un momento antes de destapar */
    long tras_carga = taek_ms() + 180;
    while (taek_ms() - g_tr.t_et < pop + g_tr.t_espera || taek_ms() < tras_carga) tr_cuadro();
    g_tr.fase = F_SALE; g_tr.t0 = taek_ms();
    rev_empezar();
    while (taek_ms() - g_tr.t0 < g_tr.t_sale) tr_cuadro();
    g_tr.activa = 0;
    /* Para la bitacora: cuadros y tiempo de la animacion (sin contar la carga) */
    { long total = taek_ms() - g_tr.t_ini;
      char m[200] = ""; add(m, TR_NOMBRE[tipo], sizeof(m)); add(m, " -> ", sizeof(m)); add(m, etq, sizeof(m));
      add(m, ": ", sizeof(m)); addn(m, (uint64_t)g_tr_cuadros, sizeof(m)); add(m, " cuadros (entrada ", sizeof(m)); addn(m, (uint64_t)c_entra, sizeof(m));
      add(m, ") en ", sizeof(m)); addn(m, (uint64_t)(total - t_medio), sizeof(m)); add(m, " ms + carga ", sizeof(m)); addn(m, (uint64_t)t_medio, sizeof(m)); add(m, " ms", sizeof(m));
      yt_log("transicion", m); }
}

/* Entrada de la cuadricula (renderFeedList): cada tarjeta llega con retardo; la variante sale de una bolsa */
enum { RV_SUBE, RV_IZQ, RV_ESCRIBE, RV_DIAGONAL, RV_CAE, NRV };
static int g_rv_bolsa[NRV], g_rv_nb, g_rv_ultimo = -1;
static void rev_empezar(void) {
    if (!g_rv_nb) {
        barajar(g_rv_bolsa, NRV);
        if (g_rv_bolsa[NRV - 1] == g_rv_ultimo) { int t = g_rv_bolsa[0]; g_rv_bolsa[0] = g_rv_bolsa[NRV - 1]; g_rv_bolsa[NRV - 1] = t; }
        g_rv_nb = NRV;
    }
    g_rev_var = g_rv_ultimo = g_rv_bolsa[--g_rv_nb];
    g_rev_t0 = taek_ms();
    g_radar_rot = azar_f(-90, 90);
}
/* Entrada del menu (menuEntrance): variantes left / up / type / drop y letras mezcladas */
static long g_ment_t0 = 0x3FFFFFFFL;
static int g_ment_var;
static float g_ment_d[8];
static void menu_entrar(void) {
    g_ment_t0 = taek_ms(); g_ment_var = azar_i(0, 3);
    for (int i = 0; i < 8; i++) g_ment_d[i] = g_ment_var == 1 ? azar_f(60, 160) : azar_f(100, 240);
}
/* ------------------------------------------------------------------ paso 2: menu lateral (#menu), zonas y categorias */
static const char *PEST_QM[NPEST] = { "INICIO", "BUSCAR", "SHORTS", "SUSCRIPCIONES", "HISTORIAL", "DESCARGAS", "PERFIL", "CUENTA" };
static const char *PEST_RUTA[NPEST] = { "/qm2/inicio", "/qm2/buscar", "/qm2/shorts", "/qm2/suscripciones", "/qm2/historial", "/qm2/descargas", "/qm2/perfil", "/qm2/cuenta" };
static const char *PEST_SUB[NPEST] = { "RECOMENDADOS", "INNERTUBE // ANDROID", "VIDEOS VERTICALES", "TUS CANALES", "LO QUE VISTE", "VIDEOS SIN RED", "ESTAD\xC3\x8DSTICAS E HISTORIAL", "SESI\xC3\x93N Y CALIDAD" };
/* Zonas como en el prototipo: en el menu las flechas mueven el cursor; en la vista, la cuadricula */
enum { Z_MENU, Z_VISTA };
static int g_zona = Z_MENU, g_menu;
/* Categorias de Inicio (Q / E). InnerTube no dice la categoria de cada video: cada una es una busqueda real. */
#define NCAT 8
static const char *CAT_NOMBRE[NCAT] = { "TODOS", "M\xC3\x9AS\x49\x43\x41", "GAMING", "ANIME", "HUMOR", "LO-FI", "HARDWARE", "OSDEV" };
static const char *CAT_CONSULTA[NCAT] = { "", "m\xC3\xBAsica", "gaming", "anime", "humor", "lofi", "hardware pc", "osdev sistema operativo" };
static int g_cat_x[NCAT], g_cat_w[NCAT];
#define MENU_AN S(440)
#define VISTA_Y (HUD_AL + S(14))
#define GRID_FIN (AL - S(36) - S(40))
static int g_mi_y[NPEST], g_mi_h[NPEST];

/* Flechas dibujadas (el atlas no las tiene): "U", "D", "L", "R" en un cuadro como .key */
static int tecla_flechas(struct lienzo *l, int x, int ybase, const char *dirs) {
    int n = lg(dirs), w = n * S(13) + S(10), h = S(20), y = ybase - S(15);
    rect(l, x, y, w, 1, premul(255, Q_Y)); rect(l, x, y + h - 1, w, 1, premul(255, Q_Y));
    rect(l, x, y, 1, h, premul(255, Q_Y)); rect(l, x + w - 1, y, 1, h, premul(255, Q_Y));
    uint32_t c = premul(255, Q_Y);
    for (int i = 0; i < n; i++) {
        float cx = (float)(x + S(5) + i * S(13) + S(6)), cy = (float)(y + h / 2), r = (float)S(4);
        if (dirs[i] == 'U') triangulo(l, cx - r, cy + r * 0.7f, cx + r, cy + r * 0.7f, cx, cy - r * 0.8f, c);
        else if (dirs[i] == 'D') triangulo(l, cx - r, cy - r * 0.7f, cx + r, cy - r * 0.7f, cx, cy + r * 0.8f, c);
        else if (dirs[i] == 'L') triangulo(l, cx + r * 0.7f, cy - r, cx + r * 0.7f, cy + r, cx - r * 0.8f, cy, c);
        else triangulo(l, cx - r * 0.7f, cy - r, cx - r * 0.7f, cy + r, cx + r * 0.8f, cy, c);
    }
    return x + w;
}
/* Pistas del pie: "U D" etc. empiezan con '^' para dibujar flechas */
static int pista(struct lienzo *l, int x, int yb, const char *k, const char *que) {
    x = (k[0] == '^' ? tecla_flechas(l, x, yb, k + 1) : tecla_q(l, x, yb, k)) + S(6);
    return tmono(l, x, yb, S(12), Q_TXT, 255, que) + S(20);
}
static int pista_ancho(const char *k, const char *que) {
    int kw = k[0] == '^' ? lg(k + 1) * S(13) + S(10) : tmono_ancho(S(12), k) + S(14);
    return kw + S(6) + tmono_ancho(S(12), que) + S(20);
}

/* Cursor del menu: dos capas (cian detras, amarilla delante) con las esquinas cortadas al azar en cada movimiento,
 * un giro leve y un resorte con rebote (cubic-bezier(.34,1.56,.64,1) del CSS) en posicion, tamano y giro. */
struct muelle { float p, v, o; };
static void muelle_paso(struct muelle *m) { m->v += (m->o - m->p) * 0.22f; m->v *= 0.64f; m->p += m->v; }
static struct muelle mc_x, mc_y, mc_w, mc_h, mc_rot;
static float mc_corte[2][8];
static int mc_listo, mc_ultimo = -1;
static void cursor_mutar(void) {
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 8; i++)   /* pares: recorte en x (0 o 2-7 %); impares: en y (0 o 15-45 %) */
            mc_corte[k][i] = azar() % 10 < 3 ? 0 : (i & 1 ? azar_f(15, 45) : azar_f(2, 7));
    mc_rot.o = azar_f(-2.5f, 1.0f);
}
static void cursor_poligono(float x, float y, float w, float h, float rot, const float *c, uint32_t col) {
    float px[8] = { 0, c[0], 100 - c[2], 100, 100, 100 - c[4], c[6], 0 };
    float py[8] = { c[1], 0, 0, c[3], 100 - c[5], 100, 100, 100 - c[7] };
    float rad = rot * 3.14159265f / 180.0f, cs = cosen(rad), sn = sen(rad), ox = x + (float)S(12), oy = y + h / 2;
    for (int i = 0; i < 8; i++) {
        float ax = x + px[i] * w / 100.0f - ox, ay = y + py[i] * h / 100.0f - oy;
        px[i] = ox + ax * cs - ay * sn; py[i] = oy + ax * sn + ay * cs;
    }
    poligono(&L, px, py, 8, col);
}
static const char *menu_meta(int i, char *o, int cap) {
    o[0] = 0;
    switch (i) {
    case P_INICIO:
        if (pest == P_INICIO && nres) { addn(o, (uint64_t)nres, cap); add(o, " OBJ // ", cap); add(o, CAT_NOMBRE[g_cat], cap); }
        else add(o, con_cuenta() ? "TU CUENTA" : "LO RELACIONADO CON LO QUE VISTE", cap);
        break;
    case P_BUSCAR: if (consulta[0]) { add(o, "\"", cap); add(o, consulta, cap); add(o, "\"", cap); } else add(o, "ESCRIBE PARA BUSCAR", cap); break;
    case P_SUBS:
        if (pest == P_SUBS && ncan) { addn(o, (uint64_t)ncan, cap); add(o, " CANALES // ", cap); }
        add(o, con_cuenta() ? "CON CUENTA" : "DE TU HISTORIAL", cap);
        break;
    case P_HIST: addn(o, (uint64_t)nhist, cap); add(o, " VIDEOS // USB", cap); break;
    case P_DESC: add(o, "/youtube/descargas", cap); break;
    case P_PERFIL: addn(o, (uint64_t)nhist, cap); add(o, " EVENTOS // 5 ATRIBUTOS", cap); break;
    case P_SHORTS:
        if (pest == P_SHORTS && nres) { addn(o, (uint64_t)nres, cap); add(o, " R\xC3\x81\x46\x41GAS // 9:16", cap); }
        else add(o, "#shorts // 9:16", cap);
        break;
    default: addn(o, (uint64_t)g_calidad, cap); add(o, "P // ", cap); add(o, con_cuenta() ? "CON CUENTA" : "SIN CUENTA", cap); break;
    }
    return o;
}
static void menu_q(void) {
    int x0 = S(24), der = MENU_AN - S(10), en_vista = g_zona == Z_VISTA;
    rect(&L, MENU_AN, HUD_AL + 1, 1, AL - HUD_AL - 1 - S(36), Q_LINEA);
    /* .menu-head */
    int yb = VISTA_Y + S(4) + S(11);
    /* Medidas de cada .mi: la elegida es mas alta (lleva la linea de datos) y mas grande */
    int y = yb + S(14);
    for (int i = 0; i < NPEST; i++) {
        int activo = i == g_menu;
        g_mi_y[i] = y; g_mi_h[i] = activo ? S(7) + S(36) + S(3) + S(14) + S(7) : S(7) + S(30) + S(7);
        y += g_mi_h[i] + S(6);
    }
    /* El cursor va a la elegida; al cambiar, cambia de forma */
    int ix = x0 + g_menu * S(9);
    float w = (float)(der - ix) * 1.08f + (float)S(18), h = (float)g_mi_h[g_menu] * 1.08f + (float)S(6);
    mc_x.o = (float)(ix - S(6)); mc_y.o = (float)g_mi_y[g_menu] + (float)g_mi_h[g_menu] / 2 - h / 2; mc_w.o = w; mc_h.o = h;
    static long sel_t0;
    long now = taek_ms(), me = now - g_ment_t0;
    if (mc_ultimo != g_menu) { cursor_mutar(); mc_ultimo = g_menu; sel_t0 = now; }
    /* Paso 3: el cursor aparece cuando termina la entrada del menu (lis.length * 60 + 200 ms) */
    int cur_vis = me >= NPEST * 60 + 200;
    if (!cur_vis) mc_listo = 0;
    else {
        if (!mc_listo) { mc_x.p = mc_x.o; mc_y.p = mc_y.o; mc_w.p = mc_w.o; mc_h.p = mc_h.o; mc_rot.p = mc_rot.o; mc_listo = 1; }
        muelle_paso(&mc_x); muelle_paso(&mc_y); muelle_paso(&mc_w); muelle_paso(&mc_h); muelle_paso(&mc_rot);
        cursor_poligono(mc_x.p + (float)S(6), mc_y.p + (float)S(5), mc_w.p, mc_h.p, mc_rot.p, mc_corte[1], premul(en_vista ? 77 : 217, Q_CIAN));
        cursor_poligono(mc_x.p, mc_y.p, mc_w.p, mc_h.p, mc_rot.p, mc_corte[0], premul(255, en_vista ? 0x8A8306u : Q_Y));
    }
    /* .menu-head despues del cursor, para que el giro no lo tape */
    g_fuente = F_MONO; texto_esp(&L, x0, yb, S(11), Q_DIM, 255, S(2), "// DIRECTORIO"); g_fuente = F_CHAKRA;
    char pos[16]; struct txt t; t_ini(&t); t_dos(&t, (uint64_t)g_menu + 1); t_add(&t, "/"); t_dos(&t, NPEST); cp(pos, t.s, sizeof(pos));
    tmono(&L, der - S(10) - tmono_ancho(S(11), pos), yb, S(11), Q_Y, 255, pos);
    /* Las entradas */
    for (int i = 0; i < NPEST; i++) {
        /* menuEntrance: cada entrada llega con su retardo (left / up / type / drop) */
        long ei = me - i * 55;
        if (ei < 0) continue;
        float te = lim01((float)ei / 380.0f), ea = e_atras(te);
        int dx = 0, dy = 0, visibles = 99;
        float op = 1;
        if (g_ment_var == 0) dx = (int)(-(1 - ea) * (float)S(470));
        else if (g_ment_var == 1) { dy = (int)(g_ment_d[i] * (float)S(1) * (1 - ea)); op = te; }
        else if (g_ment_var == 2) visibles = (int)(pasos(te, 10) * 20);
        else { dy = (int)(-g_ment_d[i] * (float)S(1) * (1 - ea)); op = te; }
        int activo = i == g_menu && cur_vis, x = x0 + i * S(9) + (activo ? S(6) : 0) + S(12) + dx;
        uint32_t a = activo ? (en_vista ? 190 : 255) : (en_vista ? 89 : 199);
        a = (uint32_t)((float)a * op);
        uint32_t c_num = activo ? Q_BG : Q_CIAN, c_txt = activo ? Q_BG : Q_TXT;
        int px = activo ? S(36) : S(30), base = g_mi_y[i] + S(7) + (activo ? S(33) : S(27)) + dy;
        char num[4]; hexn(num, (uint64_t)i + 1, 2);
        int nx = tmono(&L, x, base - (activo ? S(4) : S(2)), S(12), c_num, a, num) + S(10);
        /* scramble: 500 + i*60 ms al entrar; 220 ms la elegida al moverse */
        char et[48];
        long me_i = me, d_i = 500 + i * 60;
        if (me_i >= d_i && i == g_menu) { me_i = now - sel_t0; d_i = 220; }
        mezclar_letras(et, sizeof(et), PEST_QM[i], me_i, d_i);
        if (visibles < 20) { int k = 0, c = 0; while (et[k] && c < visibles * lg(PEST_QM[i]) / 20) { k += u8_largo(et + k); c++; } et[k] = 0; }
        texto_esp(&L, nx, base, px, c_txt, a, S(2), et);
        if (activo) {
            char m[160]; menu_meta(i, m, sizeof(m));
            tmono_cortado(&L, nx, base + S(17), S(11), Q_BG, 191, m, der - nx - S(30));
            float ax = (float)(der - S(10)), ay = (float)(base - S(12));
            triangulo(&L, ax - (float)S(6), ay - (float)S(7), ax - (float)S(6), ay + (float)S(7), ax + (float)S(5), ay, premul(a, Q_BG));
        }
    }
    /* .menu-foot: destino, ruta y la carga de la CPU0 (SYS_SENSORES) en barras */
    int fy = AL - S(36) - S(14) - S(78);
    rect(&L, x0, fy, der - x0, 1, Q_LINEA);
    tmono(&L, x0, fy + S(20), S(12), Q_DIM, 255, "DESTINO");
    { char d[64]; mezclar_letras(d, sizeof(d), PEST_SUB[g_menu], now - sel_t0, 260);
      tmono_cortado(&L, x0 + S(74), fy + S(20), S(12), Q_Y, 255, d, der - x0 - S(84)); }
    tmono(&L, x0, fy + S(38), S(12), Q_DIM, 255, "RUTA");
    tmono(&L, x0 + S(74), fy + S(38), S(12), Q_Y, 255, PEST_RUTA[g_menu]);
    int bx = x0, bw = (der - x0 - 23 * S(2)) / 24, by = fy + S(48), bh = S(26);
    for (int k = 0; k < 24; k++) {
        int v = g_cpu_n ? g_cpu_barras[(g_cpu_n + k) % 24] : 0;
        int hh = bh * (v < 4 ? 4 : v > 100 ? 100 : v) / 100;
        rect(&L, bx + k * (bw + S(2)), by + bh - hh, bw, hh, premul(140, Q_CIAN));
    }
    tmono(&L, der - tmono_ancho(S(10), "CPU0"), fy + S(38), S(10), Q_DIM, 255, "CPU0");
}
/* Fila .tabs de Inicio: Q, las categorias, E y el conteo */
static void categorias_q(void) {
    int yb = VISTA_Y + S(20), y0 = VISTA_Y, y1 = VISTA_Y + S(28);
    int x = tecla_q(&L, X0, yb, "Q") + S(8);
    for (int i = 0; i < NCAT; i++) {
        int w = ancho_esp(S(15), S(2), CAT_NOMBRE[i]) + S(24);
        g_cat_x[i] = x; g_cat_w[i] = w;
        if (i == g_cat) { rect(&L, x, y0, w, y1 - y0, premul(255, Q_Y)); rect(&L, x, y1 - S(2), w, S(2), premul(255, Q_CIAN)); }
        texto_esp(&L, x + S(12), yb, S(15), i == g_cat ? Q_BG : Q_DIM2, 255, S(2), CAT_NOMBRE[i]);
        x += w + S(2);
    }
    x = tecla_q(&L, x + S(6), yb, "E") + S(16);
    char c[32] = ""; addn(c, (uint64_t)nres, sizeof(c)); add(c, " OBJ", sizeof(c));
    int cw = tmono_ancho(S(12), c), cx = AN - S(22) - cw;
    tmono(&L, cx, yb, S(12), Q_CIAN, 255, c);
    if (cx - S(16) > x) tmono_cortado(&L, x, yb, S(11), Q_DIM2, 255, seccion, cx - S(16) - x);
}
/* .section-title de las otras vistas */
static void titulo_seccion_q(void) {
    int yb = VISTA_Y + S(34);
    int x = texto_esp(&L, X0, yb, S(36), Q_Y, 255, S(4), PEST_QM[pest]) + S(14);
    tmono_cortado(&L, x, yb, S(12), Q_DIM2, 255, seccion, AN - S(22) - x);
    rect(&L, X0, yb + S(10), AN - S(22) - X0, 1, Q_LINEA);
}
static int filas_visibles(void) {
    int f = (GRID_FIN - Y0 + ESP) / (CARTA_AL + ESP);
    return f < 1 ? 1 : f;
}
/* Titulo en dos lineas por palabras; la segunda se corta con "..." */
static void titulo_2(int x, int y, int px, int ancho, const char *s, uint32_t c, int salto) {
    static char l1[256];
    int n = lg(s), corte = 0;
    for (int i = 0; i <= n; i++) {
        if (s[i] != ' ' && s[i]) continue;
        char t[256]; int k = i < 255 ? i : 255; for (int j = 0; j < k; j++) t[j] = s[j]; t[k] = 0;
        if (texto_ancho(px, t) <= ancho) corte = i; else break;
    }
    if (!corte || corte >= n) { texto_cortado(x, y, px, c, 255, s, ancho); return; }
    int k = corte < 255 ? corte : 255; for (int j = 0; j < k; j++) l1[j] = s[j]; l1[k] = 0;
    texto(&L, x, y, px, c, 255, 0, l1);
    texto_cortado(x, y + salto, px, c, 255, s + corte + 1, ancho);
}
static void carta_pos(int i, int *x, int *y) {
    int f = i / COLS - desde_fila, col = i % COLS;
    *x = X0 + col * (CARTA_AN + ESP);
    *y = Y0 + f * (CARTA_AL + ESP);
}
static void imagen_escalada(const uint32_t *o, int ow, int oh, int x, int y, int w, int h) {
    for (int yy = 0; yy < h; yy++) {
        if (y + yy < 0 || y + yy >= AL) continue;
        uint32_t *d = L.px + (size_t)(y + yy) * (size_t)AN;
        const uint32_t *f = o + (size_t)(yy * oh / h) * (size_t)ow;
        for (int xx = 0; xx < w; xx++) if (x + xx >= 0 && x + xx < AN) d[x + xx] = f[xx * ow / w];
    }
}
/* H188: pausa del reproductor (la sesion de video se cerro; se reanuda con un salto) */
static int g_pausa; static uint32_t g_pausa_pos, g_pausa_dur; static struct video *g_pausa_v;
/* .seek: pista oscura, relleno amarillo con muescas y la aguja cian */
static void barra_progreso(struct lienzo *l, int x, int y, int w, uint32_t pos, uint32_t dur, int alto) {
    rect(l, x, y, w, alto, premul(255, 0x151A26u));
    int lleno = dur ? (int)((uint64_t)w * (pos > dur ? dur : pos) / dur) : 0;
    if (lleno > 0) rect(l, x, y, lleno, alto, premul(255, Q_Y));
    for (int k = x + S(9); k < x + w; k += S(10) > 1 ? S(10) : 2) rect(l, k, y, 1, alto, premul(230, Q_BG));
    rect(l, x + lleno - S(1), y - alto, S(3) > 1 ? S(3) : 2, alto * 3, premul(255, Q_CIAN));
}
static void dibujar_pausa(void) {
    struct video *v = g_pausa_v;
    fondo_poner();
    hud_q();
    int w = S(960), h = w * 9 / 16, x = (AN - w) / 2, y = S(120);
    if (v && v->mini) imagen_chaflan(v->mini, MINI_AN, MINI_AL, x, y, w, h, S(22));
    else { chaflan(&L, x, y, w, h, S(22), premul(255, 0x000000u)); gato(&L, x + w / 2, y + h / 2, S(90), premul(120, TENUE), premul(200, Q_Y)); }
    chaflan(&L, x, y, w, h, S(22), premul(110, 0x000000u));
    esquinas(&L, x + S(10), y + S(10), w - S(20), h - S(20), S(30), S(2), premul(204, Q_CIAN));
    /* .big-play en pausa: rombo cortado con las dos barras */
    int b = S(92), bx = x + w / 2 - b / 2, by = y + h / 2 - b / 2;
    chaflan(&L, bx, by, b, b, S(18), premul(153, Q_BG));
    chaflan_borde(&L, bx, by, b, b, S(18), S(2), premul(255, Q_Y));
    rect(&L, bx + b / 2 - S(18), by + b / 2 - S(22), S(12), S(44), premul(255, Q_Y));
    rect(&L, bx + b / 2 + S(6), by + b / 2 - S(22), S(12), S(44), premul(255, Q_Y));
    tmono(&L, x + S(48), y + S(30), S(11), Q_DIM2, 255, "EN PAUSA // DECODE");
    if (v) {
        texto_cortado(x, y + h + S(52), S(30), Q_TXT, 255, v->titulo, w);
        tmono_cortado(&L, x, y + h + S(84), S(14), Q_DIM2, 255, v->canal, w);
    }
    barra_progreso(&L, x, y + h + S(116), w, g_pausa_pos, g_pausa_dur, S(8));
    char t[64], d[24]; fmt_tiempo(t, sizeof(t), g_pausa_pos); add(t, " / ", sizeof(t)); fmt_tiempo(d, sizeof(d), g_pausa_dur); add(t, d, sizeof(t));
    tmono(&L, x, y + h + S(162), S(18), Q_Y, 255, t);
    const char *ay = "ESPACIO SEGUIR  \xC2\xB7  <- -> 10 S  \xC2\xB7  ARRIBA/ABAJO 1 MIN  \xC2\xB7  ESC SALIR";
    tmono(&L, x + w - tmono_ancho(S(12), ay), y + h + S(162), S(12), Q_DIM2, 255, ay);
}
static void dibujar_cuenta(void) {
    int x = X0, y = Y0, w = AN - S(22) - X0;
    panel_q(&L, x, y, w, S(520), S(14));
    rect(&L, x, y + S(40), w - S(14), 1, Q_LINEA);
    texto_esp(&L, x + S(14), y + S(28), S(15), Q_Y, 255, S(2), "SESI\xC3\x93N");
    tmono(&L, x + w - S(30) - tmono_ancho(S(11), "OAUTH // DEVICE FLOW"), y + S(27), S(11), Q_DIM2, 255, "OAUTH // DEVICE FLOW");
    int tx = x + S(40), ty = y + S(100);
    if (g_login_activo) {
        texto(&L, tx, ty, S(30), BLANCO, 255, 0, "Inicia sesion desde tu celular o PC");
        texto(&L, tx, ty + S(60), S(22), TENUE, 255, 0, "1. Abre esta direccion:");
        texto(&L, tx + S(40), ty + S(110), S(36), Q_CIAN, 255, 0, g_login_url);
        texto(&L, tx, ty + S(180), S(22), TENUE, 255, 0, "2. Escribe este codigo:");
        int cw = texto_ancho(S(56), g_login_codigo) + S(60);
        chaflan(&L, tx + S(40), ty + S(210), cw, S(100), S(18), premul(255, CAMPO));
        chaflan_borde(&L, tx + S(40), ty + S(210), cw, S(100), S(18), S(2), premul(255, Q_Y));
        texto(&L, tx + S(70), ty + S(280), S(56), Q_Y, 255, 0, g_login_codigo);
        tmono(&L, tx, ty + S(370), S(14), TENUE, 255, "Esperando a que lo apruebes...  (Esc cancela)");
        return;
    }
    if (con_cuenta()) {
        texto(&L, tx, ty, S(30), BLANCO, 255, 0, "Sesion iniciada con tu cuenta de Google");
        texto(&L, tx, ty + S(50), S(18), TENUE, 255, 0, "Inicio y Suscripciones muestran lo de tu cuenta. Intro: cerrar sesion.");
    } else if (!g_cfg_id[0]) {
        texto(&L, tx, ty, S(30), BLANCO, 255, 0, "Sin cuenta");
        texto(&L, tx, ty + S(50), S(18), TENUE, 255, 0, "Para iniciar sesion, crea /youtube.conf en el USB con el cliente de YouTube para TV:");
        tmono(&L, tx + S(40), ty + S(95), S(18), Q_Y, 255, "client_id=...      client_secret=...");
        texto(&L, tx, ty + S(140), S(18), TENUE, 255, 0, "Luego vuelve aqui y pulsa Intro. Sin cuenta igual puedes buscar, ver y descargar.");
    } else {
        texto(&L, tx, ty, S(30), BLANCO, 255, 0, "Sin cuenta");
        texto(&L, tx, ty + S(50), S(18), TENUE, 255, 0, "Intro: iniciar sesion con un codigo (google.com/device).");
    }
    char c[80] = "CALIDAD MAXIMA: "; addn(c, (uint64_t)g_calidad, sizeof(c)); add(c, "P   (<- / -> para cambiar)", sizeof(c));
    texto(&L, tx, ty + S(230), S(22), Q_TXT, 255, 0, c);
    tmono(&L, tx, ty + S(270), S(14), TENUE, 255, "1080p y 720p van en flujo por HLS; 360p usa el MP4 directo (itag 18).");
    char h[120] = "HISTORIAL: "; addn(h, (uint64_t)nhist, sizeof(h)); add(h, " videos en /youtube/historial.txt", sizeof(h));
    tmono(&L, tx, ty + S(320), S(14), TENUE, 255, h);
}
/* Una tarjeta .vc: miniatura cortada con sus etiquetas, avatar y datos */
static void carta_dibujar(int i, int x, int y);
/* Paso 3: la tarjeta con su entrada (renderFeedList). La opacidad se hace mezclando el fondo de vuelta encima. */
static void carta_q(int i) {
    int x, y; carta_pos(i, &x, &y);
    int k = i - desde_fila * COLS, fila_ = k / COLS, col = k % COLS;
    long r = g_rev_var == RV_DIAGONAL ? (fila_ + col) * 70 : g_rev_var == RV_ESCRIBE ? fila_ * 90 + col * 30 : k * 40;
    long el = taek_ms() - g_rev_t0 - r;
    if (el >= 320 || !g_fondo) { carta_dibujar(i, x, y); return; }
    if (el < 0) return;
    float t = (float)el / 320.0f, e = e_out(t);
    int dx = 0, dy = 0;
    if (g_rev_var == RV_SUBE) dy = (int)((float)S(40) * (1 - e));
    else if (g_rev_var == RV_IZQ) dx = (int)((float)-S(60) * (1 - e));
    else if (g_rev_var == RV_DIAGONAL) { dx = (int)((float)-S(30) * (1 - e)); dy = (int)((float)S(30) * (1 - e)); }
    else if (g_rev_var == RV_CAE) { float k2 = t < 0.7f ? e_out(t / 0.7f) : 1; dy = t < 0.7f ? (int)((float)-S(50) + (float)S(55) * k2) : (int)((float)S(5) * (1 - (t - 0.7f) / 0.3f)); }
    /* Su lugar (con las escuadras de la elegida): lo que se dibuje fuera de el durante la entrada se devuelve como
     * estaba, para no pisar a las vecinas */
    int ax = x - S(8), ay = y - S(14), bx = x + CARTA_AN + S(8), by = y + CARTA_AL + S(4);
    if (ax < MENU_AN + 1) ax = MENU_AN + 1;
    if (ay < 0) ay = 0;
    if (bx > AN) bx = AN;
    if (by > AL) by = AL;
    int ux = ax + (dx < 0 ? dx : 0), uy = ay + (dy < 0 ? dy : 0), vx = bx + (dx > 0 ? dx : 0), vy = by + (dy > 0 ? dy : 0);
    if (ux < MENU_AN + 1) ux = MENU_AN + 1;
    if (uy < 0) uy = 0;
    if (vx > AN) vx = AN;
    if (vy > AL) vy = AL;
    static uint32_t *guarda; static size_t guarda_cap;
    size_t need = (size_t)(vx - ux) * (size_t)(vy - uy);
    if (need > guarda_cap) { free(guarda); guarda = malloc(need * 4); guarda_cap = guarda ? need : 0; }
    if (!guarda) { carta_dibujar(i, x, y); return; }
    for (int f = uy; f < vy; f++) memcpy(guarda + (size_t)(f - uy) * (size_t)(vx - ux), L.px + (size_t)f * AN + ux, (size_t)(vx - ux) * 4);
    carta_dibujar(i, x + dx, y + dy);
    for (int f = uy; f < vy; f++) {
        uint32_t *d = L.px + (size_t)f * AN, *g = guarda + (size_t)(f - uy) * (size_t)(vx - ux);
        for (int xx = ux; xx < vx; xx++) if (f < ay || f >= by || xx < ax || xx >= bx) d[xx] = g[xx - ux];
    }
    if (g_rev_var == RV_ESCRIBE) {         /* inset(0 0 100% 0) -> 0, en 6 pasos */
        int corte = ay + (int)((float)(by - ay) * pasos(t, 6));
        for (int f = corte; f < by; f++) memcpy(L.px + (size_t)f * AN + ax, g_fondo + (size_t)f * AN + ax, (size_t)(bx - ax) * 4);
        if (corte < by) rect(&L, ax, corte, bx - ax, S(2), premul(255, Q_Y));
        return;
    }
    uint32_t q = (uint32_t)(lim01(t * 1.4f) * 255.0f);     /* opacidad 0 -> 1 */
    for (int f = ay; f < by; f++) {
        uint32_t *d = L.px + (size_t)f * AN, *o = g_fondo + (size_t)f * AN;
        for (int xx = ax; xx < bx; xx++) d[xx] = entre(o[xx], d[xx], q) | 0xFF000000u;
    }
}
static void carta_dibujar(int i, int x, int y) {
    struct video *v = &res[i];
    int elegida = i == sel, c = S(16);
    if (elegida) y -= S(5);
    int mx = x, my = y, mw = MINI_AN, mh = MINI_AL;
    if (v->mini) imagen_chaflan(v->mini, MINI_AN, MINI_AL, mx, my, mw, mh, c);
    else {
        chaflan(&L, mx, my, mw, mh, c, premul(255, 0x000000u));
        gato(&L, mx + mw / 2, my + mh / 2, S(40), premul(120, TENUE), premul(200, Q_Y));
    }
    /* .vc-res (aqui el numero en la lista), .vc-id y .vc-dur */
    char num[8]; struct txt t; t_ini(&t); t_dos(&t, (uint64_t)(i + 1)); cp(num, t.s, sizeof(num));
    int nw = tmono_ancho(S(10), num) + S(12);
    rect(&L, mx + S(8), my + S(8), nw, S(16), premul(255, Q_CIAN));
    tmono(&L, mx + S(14), my + S(20), S(10), Q_BG, 255, num);
    int iw = tmono_ancho(S(10), v->id) + S(10);
    rect(&L, mx + mw - S(18) - iw, my + S(8), iw, S(16), premul(191, Q_BG));
    tmono(&L, mx + mw - S(18) - iw + S(5), my + S(20), S(10), Q_DIM2, 255, v->id);
    if (v->dur[0]) {
        int dw = tmono_ancho(S(12), v->dur) + S(12);
        rect(&L, mx + mw - S(8) - dw, my + mh - S(8) - S(18), dw, S(18), premul(224, Q_BG));
        tmono(&L, mx + mw - S(8) - dw + S(6), my + mh - S(8) - S(4), S(12), Q_Y, 255, v->dur);
    }
    struct video *h = pest == P_HIST ? v : hist_buscar(v->id);
    if (h && h->dur_ms && h->pos_ms) {                       /* .vc-prog: donde quedo */
        int pw = (int)((uint64_t)(mw - c) * (h->pos_ms > h->dur_ms ? h->dur_ms : h->pos_ms) / h->dur_ms);
        rect(&L, mx + c, my + mh - S(3), pw, S(3), premul(255, Q_ROJO));
    }
    /* .vc-frame y, si es la elegida, el filo amarillo y las cuatro escuadras */
    if (elegida) {
        chaflan_borde(&L, mx, my, mw, mh, c, S(2), premul(255, Q_Y));
        esquinas(&L, mx - S(5), my - S(5), mw + S(10), mh + S(10), S(18), S(2), premul(255, Q_Y));
    } else chaflan_borde(&L, mx, my, mw, mh, c, 1, premul(64, Q_CIAN));
    /* .vc-body: avatar con las iniciales del canal, titulo en dos lineas, canal y vistas */
    int ay = my + mh + S(10), av = S(36);
    char ini[8] = "";
    { int k = 0; for (const char *p = v->canal; *p && k < 2; p++) if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) ini[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p); ini[k] = 0; }
    if (!ini[0]) cp(ini, "YT", sizeof(ini));
    chaflan(&L, x, ay, av, av, S(7), premul(255, elegida ? Q_Y : Q_BG2));
    chaflan_borde(&L, x, ay, av, av, S(7), 1, premul(255, elegida ? Q_Y : Q_CIAN));
    texto(&L, x + (av - texto_ancho(S(13), ini)) / 2, ay + S(23), S(13), elegida ? Q_BG : Q_CIAN, 255, 0, ini);
    int tx = x + av + S(10), tw = CARTA_AN - av - S(12);
    titulo_2(tx, ay + S(15), S(15), tw, v->titulo, elegida ? Q_Y : Q_TXT, S(19));
    tmono_cortado(&L, tx, ay + S(56), S(12), Q_DIM2, 255, v->canal, tw);
    if (v->vistas[0]) tmono_cortado(&L, tx, ay + S(72), S(11), Q_DIM, 255, v->vistas, tw);
}
/* ------------------------------------------------------------------ paso 4: dibujo de Suscripciones y Perfil */
/* Linea suavizada de grosor g: un cuadrilatero convexo */
static void linea_aa(float x0, float y0, float x1, float y1, float g, uint32_t c) {
    float dx = x1 - x0, dy = y1 - y0, l = raiz(dx * dx + dy * dy);
    if (l < 0.01f) return;
    float nx = -dy / l * g / 2, ny = dx / l * g / 2;
    float px[4] = { x0 + nx, x1 + nx, x1 - nx, x0 - nx }, py[4] = { y0 + ny, y1 + ny, y1 - ny, y0 - ny };
    poligono(&L, px, py, 4, c);
}
/* Relleno por filas de un poligono cualquiera (par-impar), sin suavizado: el filo lo pone linea_aa */
static void rellenar(const float *px, const float *py, int n, uint32_t c) {
    float miny = py[0], maxy = py[0];
    for (int i = 1; i < n; i++) { if (py[i] < miny) miny = py[i]; if (py[i] > maxy) maxy = py[i]; }
    for (int y = (int)miny < 0 ? 0 : (int)miny; y <= (int)maxy + 1 && y < AL; y++) {
        float fy = (float)y + 0.5f, cx[16];
        int m = 0;
        for (int i = 0; i < n && m < 16; i++) {
            int j = (i + 1) % n;
            float a = py[i], b = py[j];
            if ((a <= fy && b > fy) || (b <= fy && a > fy)) cx[m++] = px[i] + (fy - a) / (b - a) * (px[j] - px[i]);
        }
        for (int i = 1; i < m; i++) for (int k = i; k > 0 && cx[k] < cx[k - 1]; k--) { float t = cx[k]; cx[k] = cx[k - 1]; cx[k - 1] = t; }
        for (int i = 0; i + 1 < m; i += 2) fila(y, cx[i], cx[i + 1], c);
    }
}
/* .panel-head */
static void cabeza_panel(int x, int y, int w, const char *t, const char *em) {
    texto_esp(&L, x + S(14), y + S(22), S(15), Q_Y, 255, S(2), t);
    if (em) tmono(&L, x + w - S(28) - tmono_ancho(S(11), em), y + S(21), S(11), Q_DIM2, 255, em);
    rect(&L, x, y + S(30), w - S(14), 1, Q_LINEA);
}
/* .kv: etiqueta y valor */
static void kv(int x, int y, int w, const char *et, const char *v, uint32_t cv) {
    rect(&L, x, y, w, S(36), premul(255, Q_BG2));
    tmono(&L, x + S(9), y + S(14), S(10), Q_DIM, 255, et);
    tmono_cortado(&L, x + S(9), y + S(31), S(14), cv, 255, v, w - S(18));
}
static void fmt_seg(char *o, int cap, uint32_t s) {
    o[0] = 0;
    if (s >= 3600) { addn(o, s / 3600, cap); add(o, ":", cap); }
    struct txt t; t_ini(&t);
    if (s >= 3600) t_dos(&t, s / 60 % 60); else t_num(&t, s / 60);
    t_add(&t, ":"); t_dos(&t, s % 60); add(o, t.s, cap);
}

static const char *NODO_CLASE[8] = { "OPERADOR", "ARCHIVISTA", "RASTREADOR", "EMISOR", "NODO MADRE", "REPETIDOR", "CENTINELA", "OR\xC3\x81\x43ULO" };
static int CAN_COLS, CAN_AN, CAN_AL;
static void canal_pos(int i, int *x, int *y) {
    int f = i / CAN_COLS - can_desde, c = i % CAN_COLS;
    *x = X0 + c * (CAN_AN + S(14));
    *y = Y0 + f * (CAN_AL + S(14));
}
static int canal_filas(void) { int f = (GRID_FIN - Y0 + S(14)) / (CAN_AL + S(14)); return f < 1 ? 1 : f; }
static void nodo_q(int i) {
    struct canal *c = &g_can[i];
    int x, y; canal_pos(i, &x, &y);
    int elegido = i == can_sel && g_zona == Z_VISTA;
    /* entrada (rise / deal / type del prototipo): cada nodo 55 ms despues del anterior */
    long el = taek_ms() - g_rev_t0 - (long)(i - can_desde * CAN_COLS) * 55;
    if (el < 0) return;
    float t = lim01((float)el / 380.0f);
    int corte = CAN_AL;
    if (t < 1) {
        if (g_rev_var % 3 == 0) y += (int)((float)S(50) * (1 - e_atras(t)));
        else if (g_rev_var % 3 == 1) x -= (int)((float)S(160) * (1 - e_atras(t)));
        else corte = (int)((float)CAN_AL * pasos(t, 6));
    }
    if (elegido) y -= S(4);
    int w = CAN_AN, h = corte;
    if (h <= 0) return;
    chaflan(&L, x, y, w, h, S(14), premul(240, elegido ? 0x14160Au : Q_PANEL));
    rect(&L, x, y, S(2), h - S(14), premul(elegido ? 255 : 89, Q_Y));
    for (int k = 0; k < S(14) && k < h; k++) rect(&L, x + w - S(14) + k, y + k, S(2), 1, premul(255, Q_Y));
    if (corte < CAN_AL) { rect(&L, x, y + h - S(2), w, S(2), premul(255, Q_Y)); if (h < S(40)) return; }
    char uid[12] = "0x"; hexn(uid + 2, c->uid, 8);
    cabeza_panel(x, y, w, NODO_CLASE[c->uid % 8], uid);
    if (h < S(110)) return;
    /* .node-body: avatar con iniciales, nombre y lo que se sabe */
    int av = S(52), ay = y + S(42);
    char ini[8] = "";
    { int k = 0; for (const char *p = c->nombre; *p && k < 2; p++) if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) ini[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p); ini[k] = 0; }
    chaflan(&L, x + S(14), ay, av, av, S(10), premul(255, Q_BG2));
    chaflan_borde(&L, x + S(14), ay, av, av, S(10), 1, premul(255, elegido ? Q_Y : Q_CIAN));
    texto(&L, x + S(14) + (av - texto_ancho(S(18), ini)) / 2, ay + S(33), S(18), elegido ? Q_Y : Q_CIAN, 255, 0, ini);
    texto_cortado(x + S(78), ay + S(22), S(15), elegido ? Q_Y : Q_TXT, 255, c->nombre, w - S(92));
    char sub[48] = ""; addn(sub, (uint64_t)c->videos, sizeof(sub)); add(sub, c->del_feed ? " NUEVOS EN TU FEED" : " EN TU HISTORIAL", sizeof(sub));
    tmono_cortado(&L, x + S(78), ay + S(42), S(11), Q_DIM2, 255, sub, w - S(92));
    if (h < S(190)) return;
    /* .kv-grid */
    int gx = x + S(14), gy = ay + av + S(10), gw = w - S(28), cw = (gw - 1) / 2;
    rect(&L, gx, gy, gw, S(36) * 2 + 1, Q_LINEA);
    char a[24] = "", b[24] = "", vv[24] = "";
    struct txt t2; t_ini(&t2); t_dos(&t2, (uint64_t)c->videos); cp(a, t2.s, sizeof(a));
    fmt_seg(b, sizeof(b), c->seg);
    addn(vv, (uint64_t)c->vistos, sizeof(vv)); add(vv, "\xC3\x97", sizeof(vv));
    kv(gx, gy, cw, "OBJETOS", a, Q_Y);
    kv(gx + cw + 1, gy, gw - cw - 1, "DURACI\xC3\x93N", b, Q_TXT);
    kv(gx, gy + S(37), cw, "VISTO", vv, Q_CIAN);
    kv(gx + cw + 1, gy + S(37), gw - cw - 1, "FUENTE", c->del_feed ? "SUSCRIPCIONES" : "HISTORIAL", Q_TXT);
    if (h < CAN_AL) return;
    /* .node-link: afinidad en 10 bloques */
    int ly = gy + S(37) * 2 + S(14);
    tmono(&L, gx, ly + S(7), S(10), Q_DIM, 255, "AFINIDAD");
    int bx = gx + tmono_ancho(S(10), "AFINIDAD") + S(8);
    char nv[12] = "NV "; struct txt t3; t_ini(&t3); t_dos(&t3, (uint64_t)c->nivel); add(nv, t3.s, sizeof(nv));
    int nvw = tmono_ancho(S(11), nv), bw = gx + gw - nvw - S(8) - bx, paso = bw / 10;
    for (int k = 0; k < 10; k++) rect(&L, bx + k * paso, ly, paso - S(2), S(6), premul(255, k < c->nivel ? Q_CIAN : 0x1B2131u));
    tmono(&L, gx + gw - nvw, ly + S(8), S(11), Q_CIAN, 255, nv);
}
static void dibujar_canales(void) {
    if (!ncan) { if (ocupado_pct < 0) gato(&L, (X0 + AN) / 2, AL / 2 + S(40), S(90), premul(160, TENUE), premul(255, Q_Y)); return; }
    int filas = canal_filas() + 1;
    for (int i = can_desde * CAN_COLS; i < ncan && i / CAN_COLS - can_desde < filas; i++) if (i != can_sel) nodo_q(i);
    if (can_sel < ncan && can_sel / CAN_COLS - can_desde < filas && can_sel >= can_desde * CAN_COLS) nodo_q(can_sel);
    if (g_fondo) for (int y = GRID_FIN; y < AL; y++)
        memcpy(L.px + (size_t)y * (size_t)AN + MENU_AN + 1, g_fondo + (size_t)y * (size_t)AN + MENU_AN + 1, (size_t)(AN - MENU_AN - 1) * 4);
}

/* Boveda: radar a la izquierda; atributos y registro a la derecha */
static int g_reg_x0 = 1 << 30, g_reg_x1, g_reg_y0;       /* filas del REGISTRO, para el raton */
static void dibujar_perfil(void) {
    int x0 = X0, y0 = Y0, x1 = AN - S(22), y1 = AL - S(36) - S(14);
    int cw = (x1 - x0 - S(18)) / 2;
    long el = taek_ms() - g_rev_t0;
    /* .radar-wrap */
    panel_q(&L, x0, y0, cw, y1 - y0, S(14));
    float esc = (float)cw / 420.0f, e2 = (float)(y1 - y0) / 330.0f;
    if (e2 < esc) esc = e2;
    float R = 112 * esc, cx = (float)x0 + (float)cw / 2, cy = (float)y0 + (float)(y1 - y0) / 2 + 8 * esc;
    float ax[NATR], ay[NATR];
    for (int k = 0; k < NATR; k++) { float a = -1.5707963f + (float)k * 6.2831853f / NATR; ax[k] = cosen(a); ay[k] = sen(a); }
    for (int r = 1; r <= 5; r++)
        for (int k = 0; k < NATR; k++) { int j = (k + 1) % NATR; float q = R * (float)r / 5; linea_aa(cx + ax[k] * q, cy + ay[k] * q, cx + ax[j] * q, cy + ay[j] * q, 1, premul(46, Q_CIAN)); }
    for (int k = 0; k < NATR; k++) linea_aa(cx, cy, cx + ax[k] * R, cy + ay[k] * R, 1, premul(36, Q_CIAN));
    /* la forma llega girando y crece con un rebote (650 ms) */
    float t = lim01((float)el / 650.0f), s;
    if (t < 0.7f) s = 1.06f * e_out(t / 0.7f); else s = 1.06f - 0.06f * e_out((t - 0.7f) / 0.3f);
    float rot = g_radar_rot * (1 - e_out(t)) * 3.14159265f / 180.0f, cr = cosen(rot), sr = sen(rot);
    float px[NATR], py[NATR];
    for (int k = 0; k < NATR; k++) {
        float q = R * atr_valor[k] / 5 * s, vx = ax[k] * q, vy = ay[k] * q;
        px[k] = cx + vx * cr - vy * sr; py[k] = cy + vx * sr + vy * cr;
    }
    if (s > 0.01f) {
        rellenar(px, py, NATR, premul(56, Q_Y));
        for (int k = 0; k < NATR; k++) { int j = (k + 1) % NATR; linea_aa(px[k], py[k], px[j], py[j], 2, premul(255, Q_Y)); }
        for (int k = 0; k < NATR; k++) rect(&L, (int)px[k] - S(3), (int)py[k] - S(3), S(6), S(6), premul(255, Q_Y));
    }
    for (int k = 0; k < NATR; k++) {
        float lx = cx + ax[k] * (R + 28 * esc), ly = cy + ay[k] * (R + 28 * esc);
        int w = ancho_esp(S(22), S(2), ATR_NOMBRE[k]);
        texto_esp(&L, (int)lx - w / 2, (int)ly + S(8), S(22), Q_TXT, 255, S(2), ATR_NOMBRE[k]);
        char rk[48] = "NV "; addn(rk, (uint64_t)atr_nivel[k], sizeof(rk)); add(rk, " // ", sizeof(rk)); addn(rk, (uint64_t)atr_pts[k], sizeof(rk)); add(rk, " ", sizeof(rk)); add(rk, ATR_UNIDAD[k], sizeof(rk));
        tmono(&L, (int)lx - tmono_ancho(S(16), rk) / 2, (int)ly + S(32), S(16), Q_CIAN, 255, rk);
    }
    /* .vault-side: ATRIBUTOS */
    int sx = x0 + cw + S(18), sy = y0, sh = S(30) + S(20) + NATR * S(30);
    panel_q(&L, sx, sy, cw, sh, S(14));
    cabeza_panel(sx, sy, cw, "ATRIBUTOS", "NIVEL 1-5");
    for (int k = 0; k < NATR; k++) {
        int ry = sy + S(48) + k * S(30);
        tmono(&L, sx + S(14), ry + S(12), S(12), Q_TXT, 255, ATR_NOMBRE[k]);
        int bx = sx + S(14) + S(130), bw = cw - S(14) - S(130) - S(60) - S(14), paso = bw / 20;
        int on = (int)(atr_valor[k] * 4 + 0.5f);
        for (int j = 0; j < 20; j++) {
            int encendido = j < on && el >= 120 + k * 80 + j * 18;
            rect(&L, bx + j * paso, ry + S(2), paso - S(2), S(10), premul(255, encendido ? Q_Y : 0x1B2131u));
        }
        char pc[8] = ""; addn(pc, (uint64_t)(atr_valor[k] * 20 + 0.5f), sizeof(pc)); add(pc, "%", sizeof(pc));
        tmono(&L, sx + cw - S(14) - tmono_ancho(S(12), pc), ry + S(12), S(12), Q_CIAN, 255, pc);
    }
    /* REGISTRO: lo ultimo visto (Intro lo abre) */
    int hy = sy + sh + S(12), hh = y1 - hy;
    panel_q(&L, sx, hy, cw, hh, S(14));
    char ev[24] = ""; addn(ev, (uint64_t)nhist, sizeof(ev)); add(ev, " EVENTOS", sizeof(ev));
    cabeza_panel(sx, hy, cw, "REGISTRO", ev);
    g_reg_x0 = sx; g_reg_x1 = sx + cw; g_reg_y0 = hy + S(38);
    if (!nres) { tmono(&L, sx + S(14), hy + S(56), S(12), Q_DIM2, 255, "> REGISTRO VAC\xC3\x8DO // LOS ATRIBUTOS SUBEN CON CADA REPRODUCCI\xC3\x93N"); return; }
    for (int i = 0; i < nres; i++) {
        int ry = hy + S(38) + i * S(26);
        if (ry + S(24) > hy + hh) break;
        struct video *v = &res[i];
        int elegido = i == sel && g_zona == Z_VISTA;
        if (elegido) { rect(&L, sx + S(8), ry, cw - S(30), S(24), premul(26, Q_Y)); rect(&L, sx + S(8), ry, S(2), S(24), premul(255, Q_Y)); }
        rect(&L, sx + S(8), ry + S(24), cw - S(30), 1, premul(15, Q_CIAN));
        char pc[16] = "";
        if (v->dur_ms) { addn(pc, (uint64_t)((uint64_t)(v->pos_ms > v->dur_ms ? v->dur_ms : v->pos_ms) * 100 / v->dur_ms), sizeof(pc)); add(pc, "% VISTO", sizeof(pc)); }
        else cp(pc, "--", sizeof(pc));
        tmono(&L, sx + S(14), ry + S(17), S(12), Q_DIM, 255, pc);
        int dw = tmono_ancho(S(12), v->dur);
        tmono_cortado(&L, sx + S(100), ry + S(17), S(12), elegido ? Q_Y : Q_TXT, 255, v->titulo, cw - S(100) - dw - S(44));
        tmono(&L, sx + cw - S(22) - dw, ry + S(17), S(12), Q_CIAN, 255, v->dur);
    }
}
/* H201: estado del Reproductor (la pagina con el video en marco) */
struct fuente;
static struct {
    int activo, iris, pausa, vw, vh, nh;
    struct fuente *R;
    uint32_t pos, dur, cuadros, tardios, motor, cola_ms, k, ult_cuadros, ult_tardios, capas;
    uint64_t kib;
    long ult_t;
    int fps[48], perd[48];
    char aviso[48];
    int shorts, cambio;           /* H202: pagina de Shorts; -1/+1 = pasar al anterior o al siguiente */
} g_rep;
static void dibujar_reproductor(int con_video);
static void dibujar_shorts(int con_video);
static void panel_ocupado(void);static void dibujar(void) {
    telemetria();
    if (g_rep.activo) { dibujar_reproductor(0); if (ocupado_pct >= 0) panel_ocupado(); return; }   /* H201 */
    fondo_poner();
    hud_q();
    menu_q();
    /* cabecera de la vista: categorias en Inicio, buscador en Buscar, titulo en las demas */
    if (pest == P_INICIO) categorias_q();
    else if (pest == P_BUSCAR) {
        int bx = X0, bw = S(760), by = VISTA_Y + S(2), bh = S(46);
        chaflan(&L, bx, by, bw, bh, S(12), premul(220, 0x10121Cu));
        rect(&L, bx, by + bh - S(2), bw - S(12), S(2), consulta[0] ? premul(255, Q_Y) : premul(120, Q_CIAN));
        tmono(&L, bx + S(14), by + S(16), S(11), consulta[0] ? Q_Y : Q_DIM, 255, "BUSCAR //");
        if (consulta[0]) texto_cortado(bx + S(14), by + S(38), S(18), BLANCO, 255, consulta, bw - S(60));
        else texto(&L, bx + S(14), by + S(38), S(18), Q_DIM, 255, 0, "Buscar en YouTube (escribe y pulsa Intro)");
        if (ocupado_pct < 0 && g_zona == Z_VISTA && (taek_ms() / 500) & 1) rect(&L, bx + S(16) + texto_ancho(S(18), consulta), by + S(22), S(10), S(18), premul(255, Q_Y));
        if (seccion[0]) tmono_cortado(&L, bx + bw + S(24), by + S(32), S(12), Q_DIM2, 255, seccion, AN - S(22) - bx - bw - S(24));
    } else if (pest != P_SHORTS) titulo_seccion_q();
    if (pest == P_CUENTA) dibujar_cuenta();
    else if (pest == P_SHORTS) dibujar_shorts(0);
    else if (pest == P_SUBS) dibujar_canales();
    else if (pest == P_PERFIL) dibujar_perfil();
    else {
        /* La fila de abajo puede asomar a medias: se dibuja entera y luego se devuelve el fondo bajo GRID_FIN */
        int filas_vis = filas_visibles() + 1;
        for (int i = desde_fila * COLS; i < nres && i / COLS - desde_fila < filas_vis; i++) if (i != sel) carta_q(i);
        if (sel >= desde_fila * COLS && sel < nres && sel / COLS - desde_fila < filas_vis) carta_q(sel);   /* encima de las demas */
        if (g_fondo) for (int y = GRID_FIN; y < AL; y++)
            memcpy(L.px + (size_t)y * (size_t)AN + MENU_AN + 1, g_fondo + (size_t)y * (size_t)AN + MENU_AN + 1, (size_t)(AN - MENU_AN - 1) * 4);
        if (!nres && ocupado_pct < 0) gato(&L, (X0 + AN) / 2, AL / 2 + S(40), S(90), premul(160, TENUE), premul(255, Q_Y));
        /* .sel-bar: lo elegido en una fila de datos */
        if (nres && sel < nres) {
            struct video *v = &res[sel];
            int y = AL - S(36) - S(32), h = S(22), x = X0, fin = AN - S(22);
            char pos[24] = ""; struct txt t; t_ini(&t); t_dos(&t, (uint64_t)sel + 1); t_add(&t, "/"); t_dos(&t, (uint64_t)nres); cp(pos, t.s, sizeof(pos));
            const char *celdas[5] = { "SEL", pos, v->id, v->titulo, v->dur };
            rect(&L, x, y, fin - x, h, Q_LINEA);
            for (int k = 0; k < 5; k++) {
                int resto = fin - x, w = k == 3 ? resto - (tmono_ancho(S(11), v->dur) + S(20)) - 1 : tmono_ancho(S(11), celdas[k]) + S(20);
                if (k == 4) w = resto;
                if (w <= 0) break;
                rect(&L, x, y, w, h, premul(255, k == 0 ? (g_zona == Z_VISTA ? Q_Y : 0x8A8306u) : Q_BG2));
                tmono_cortado(&L, x + S(10), y + S(16), S(11), k == 0 ? Q_BG : k == 2 ? Q_CIAN : Q_DIM2, 255, celdas[k], w - S(20));
                x += w + 1;
            }
        }
    }
    /* #prompts: ruta o aviso a la izquierda, teclas de la zona a la derecha */
    int py = AL - S(36);
    rect(&L, 0, py, AN, S(36), premul(178, Q_BG));
    rect(&L, 0, py, AN, 1, Q_LINEA);
    int yb = py + S(24);
    const char *k[7][2]; int nk = 0;
#define PISTA(a, b) do { k[nk][0] = (a); k[nk][1] = (b); nk++; } while (0)
    if (g_zona == Z_MENU) { PISTA("^UD", "MOVER"); PISTA("INTRO", "ELEGIR"); PISTA("TAB", "SECCI\xC3\x93N"); PISTA("ESC", "SALIR"); }
    else if (pest == P_CUENTA) { PISTA("INTRO", "SESI\xC3\x93N"); PISTA("^LR", "CALIDAD"); PISTA("ESC", "MEN\xC3\x9A"); }
    else if (pest == P_SUBS) { PISTA("^UDLR", "MOVER"); PISTA("INTRO", "BUSCAR CANAL"); PISTA("ESC", "MEN\xC3\x9A"); }
    else if (pest == P_PERFIL) { PISTA("^UD", "REGISTRO"); PISTA("INTRO", "VER"); PISTA("ESC", "MEN\xC3\x9A"); }
    else if (pest == P_SHORTS) { PISTA("^UD", "CAMBIAR"); PISTA("INTRO", "VER"); PISTA("ESC", "MEN\xC3\x9A"); }
    else {
        PISTA("^UDLR", "MOVER"); PISTA("INTRO", pest == P_BUSCAR ? "BUSCAR / VER" : "VER");
        if (pest == P_INICIO) PISTA("Q E", "CATEGOR\xC3\x8D\x41");
        if (pest != P_DESC) PISTA("F2", "DESCARGAR");
        if (pest == P_HIST || pest == P_DESC) PISTA("SUPR", "QUITAR");
        PISTA("ESC", "MEN\xC3\x9A");
    }
#undef PISTA
    int ancho_k = 0;
    for (int i = 0; i < nk; i++) ancho_k += pista_ancho(k[i][0], k[i][1]);
    int kx = AN - S(24) - ancho_k;
    for (int i = 0; i < nk; i++) kx = pista(&L, kx, yb, k[i][0], k[i][1]);
    if (estado[0]) tmono_cortado(&L, S(24), yb, S(12), Q_Y, 255, estado, AN - S(72) - ancho_k);
    else tmono(&L, S(24), yb, S(12), Q_DIM2, 255, PEST_RUTA[g_zona == Z_MENU ? g_menu : pest]);
    /* paso 3: una transicion en curso tapa todo; lo que se este cargando lo dice su etiqueta */
    if (g_tr.activa) { tr_dibujar(); return; }
    if (ocupado_pct >= 0) panel_ocupado();
}
/* panel de trabajo (PROCESANDO) */
static void panel_ocupado(void) {
    {
        int pw = S(760), ph = S(150), px = (AN - pw) / 2, ppy = (AL - ph) / 2;
        rect(&L, 0, HUD_AL + 1, AN, AL - HUD_AL - 1 - S(36), premul(150, Q_BG));   /* velo: lo de atras se apaga */
        chaflan(&L, px, ppy, pw, ph, S(14), premul(255, Q_BG));
        panel_q(&L, px, ppy, pw, ph, S(14));
        rect(&L, px, ppy + S(36), pw - S(14), 1, Q_LINEA);
        texto_esp(&L, px + S(16), ppy + S(25), S(15), Q_Y, 255, S(2), "PROCESANDO");
        if (ocupado_pct > 0) {
            char pc[16] = ""; addn(pc, (uint64_t)(ocupado_pct > 100 ? 100 : ocupado_pct), sizeof(pc)); add(pc, " %", sizeof(pc));
            tmono(&L, px + pw - S(30) - tmono_ancho(S(12), pc), ppy + S(24), S(12), Q_CIAN, 255, pc);
        }
        texto_cortado(px + S(24), ppy + S(78), S(22), Q_TXT, 255, ocupado_msg, pw - S(48));
        int bx = px + S(24), bw = pw - S(48), paso = S(10) > 3 ? S(10) : 4, n = bw / paso;
        int encendidos = ocupado_pct > 0 ? n * (ocupado_pct > 100 ? 100 : ocupado_pct) / 100 : (int)((taek_ms() / 60) % (n + 1));
        for (int i = 0; i < n; i++)
            rect(&L, bx + i * paso, ppy + S(104), paso - S(3), S(16), i < encendidos ? premul(255, Q_Y) : premul(50, Q_Y));
    }
}
/* Un Esc pulsado (sin otras teclas que importen) mientras se espera algo largo */
static int evento_esc(void) {
    struct taek_evento e;
    for (int k = 0; k < 8; k++) {
        long hay = taek_evento(&e);
        if (hay <= 0) break;
        uint32_t t = ap_filtrar(e.tecla);
        if (t == 'q' || t == 'Q') return 1;
    }
    return ap_esc_suelto();
}

/* ------------------------------------------------------------------ descargas no bloqueantes (H187) y archivos */
static struct con c_aud;
struct bajada {
    struct con *c; uint8_t *buf; uint64_t cap, n;
    int fase;             /* 0 libre, 1 estado, 2 cabeceras, 3 cuerpo, 4 tamano de trozo, 5 trozo, 6 fin de trozo, 7 trailer, 9 hecho, -1 error */
    int estado, trozos, cerrar, redir, archivo;
    int fd; uint64_t total;               /* H188: archivo del USB por SYS_ARCHIVOS ABRIR_FD/LEER_FD */
    int64_t largo; uint64_t resta;
    char url[4096], lugar[2048], l[2200]; int ln;
};
static uint64_t g_trozo = 128u << 10, g_lect_n, g_lect_ms, g_lect_b, g_lect_max;   /* H188: lecturas del USB */
static long ar_abrir_fd(const char *ruta, uint64_t *total) {
    memset(&ap_ar, 0, sizeof(ap_ar));
    ap_ar.op = TAEK_AR_ABRIR_FD; ap_ar.disco = ap_disco; ap_copiar(ap_ar.ruta, ruta, sizeof(ap_ar.ruta));
    long r = taek_archivos(&ap_ar);
    if (total) *total = ap_ar.total;
    return r ? r : (long)ap_ar.manejador;
}
static long ar_leer_fd(int h, void *d, uint64_t cap) {
    memset(&ap_ar, 0, sizeof(ap_ar));
    ap_ar.op = TAEK_AR_LEER_FD; ap_ar.disco = ap_disco; ap_ar.manejador = (uint32_t)h;
    ap_ar.destino = (uint64_t)(uintptr_t)d; ap_ar.capacidad = cap;
    long r = taek_archivos(&ap_ar);
    return r ? r : (long)ap_ar.n;
}
static void ar_cerrar_fd(int h) {
    memset(&ap_ar, 0, sizeof(ap_ar));
    ap_ar.op = TAEK_AR_CERRAR_FD; ap_ar.disco = ap_disco; ap_ar.manejador = (uint32_t)h;
    taek_archivos(&ap_ar);
}
static int bajada_iniciar(struct bajada *b, const char *url) {
    char host[160]; const char *ruta;
    if (url != b->url) cp(b->url, url, sizeof(b->url));
    b->fase = -1;
    if (partir_url(b->url, host, sizeof(host), &ruta)) return -1;
    for (int intento = 0; intento < 2; intento++) {
        int nueva = !(b->c->id >= 0 && igual(b->c->host, host));
        if (con_abrir(b->c, host)) return -1;
        g_pet[0] = 0;
        add(g_pet, "GET ", sizeof(g_pet)); add(g_pet, ruta, sizeof(g_pet));
        add(g_pet, " HTTP/1.1\r\nHost: ", sizeof(g_pet)); add(g_pet, host, sizeof(g_pet));
        add(g_pet, "\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\nUser-Agent: TAEK-OS/1.0\r\n\r\n", sizeof(g_pet));
        if (con_enviar(b->c, g_pet, (uint32_t)lg(g_pet))) { con_cerrar(b->c); if (nueva) return -1; continue; }
        b->fase = 1; b->ln = 0; b->n = 0; b->estado = 0; b->largo = -1; b->trozos = b->cerrar = 0; b->lugar[0] = 0;
        return 0;
    }
    return -1;
}
static void bajada_linea(struct bajada *b) {
    char *l = b->l; int k = b->ln;
    while (k && l[k - 1] == '\r') k--;
    l[k] = 0; b->ln = 0;
    if (b->fase == 1) {
        if (k < 12 || !prefijo_ci(l, "HTTP/1.")) { b->fase = -1; return; }
        b->estado = (l[9] - '0') * 100 + (l[10] - '0') * 10 + (l[11] - '0');
        b->fase = 2;
    } else if (b->fase == 2) {
        if (!k) {
            if (b->trozos) b->fase = 4;
            else if (b->largo == 0) b->fase = 9;
            else b->fase = 3;
            if (b->largo < 0 && !b->trozos) b->cerrar = 1;
            return;
        }
        if (prefijo_ci(l, "content-length:")) { int64_t v = 0; for (const char *p = l + 15; *p; p++) if (*p >= '0' && *p <= '9') v = v * 10 + (*p - '0'); b->largo = v; }
        else if (prefijo_ci(l, "transfer-encoding:") && hallar(l, l + k, "chunked")) b->trozos = 1;
        else if (prefijo_ci(l, "connection:") && hallar(l, l + k, "close")) b->cerrar = 1;
        else if (prefijo_ci(l, "location:")) { const char *p = l + 9; while (*p == ' ') p++; cp(b->lugar, p, sizeof(b->lugar)); }
    } else if (b->fase == 4) {
        uint64_t t = 0;
        for (const char *p = l; *p; p++) {
            char h = minus(*p);
            if (h >= '0' && h <= '9') t = t * 16 + (uint64_t)(h - '0');
            else if (h >= 'a' && h <= 'f') t = t * 16 + (uint64_t)(h - 'a' + 10);
            else break;
        }
        if (t) { b->resta = t; b->fase = 5; } else b->fase = 7;
    } else if (b->fase == 6) b->fase = 4;
    else if (b->fase == 7 && !k) b->fase = 9;
}
static void bajada_copiar(struct bajada *b, const uint8_t *d, uint32_t a) {
    if (b->n + a > b->cap) { b->fase = -1; return; }
    memcpy(b->buf + b->n, d, a); b->n += a;
}
/* 1 terminada, 0 sigue, -1 error. espera_ms: cuanto esperar al primer byte si no hay nada (0-1 = no bloquear). */
static int bajada_avanzar(struct bajada *b, uint32_t espera_ms) {
    if (b->archivo) {                                  /* H188: segmento descargado en el USB, por trozos */
        if (b->fase == 9) return 1;
        if (b->fase != 3) return -1;
        long t0 = taek_ms();
        if (b->fd < 0) {                               /* abrir cuesta lo mismo que una lectura: es un paso aparte */
            long h = ar_abrir_fd(b->url, &b->total);
            g_lect_n++; g_lect_ms += (uint64_t)(taek_ms() - t0);
            if (h < 0 || b->total > b->cap) { if (h >= 0) ar_cerrar_fd((int)h); b->fase = -1; return -1; }
            b->fd = (int)h;
            return 0;
        }
        uint64_t k = b->total - b->n;
        if (k > g_trozo) k = g_trozo;
        long r = k ? ar_leer_fd(b->fd, b->buf + b->n, k) : 0;
        long dt = taek_ms() - t0;
        g_lect_n++; g_lect_ms += (uint64_t)dt; if (r > 0) g_lect_b += (uint64_t)r;
        if ((uint64_t)dt > g_lect_max) g_lect_max = (uint64_t)dt;
        /* Trozo que tarde ~10-20 ms: el nucleo tiene dos cuadros listos (66 ms) de colchon */
        if (dt < 8 && g_trozo < (2u << 20)) g_trozo *= 2;
        else if (dt > 20 && g_trozo > (32u << 10)) g_trozo /= 2;
        if (r < 0) { ar_cerrar_fd(b->fd); b->fd = -1; b->fase = -1; return -1; }
        b->n += (uint64_t)r;
        if (b->n >= b->total || !r) { ar_cerrar_fd(b->fd); b->fd = -1; b->fase = b->n == b->total ? 9 : -1; return b->fase == 9 ? 1 : -1; }
        return 0;
    }
    struct con *c = b->c;
    for (int vueltas = 0; vueltas < 24 && b->fase > 0 && b->fase < 9; vueltas++) {
        if (c->i == c->n) {
            struct taek_red r; memset(&r, 0, sizeof(r));
            r.op = TAEK_RED_RECIBIR; r.id = (int32_t)c->id; r.datos = (uint64_t)(uintptr_t)c->rx; r.largo = 65536; r.espera_ms = espera_ms;
            long x = taek_red(&r);
            if (x < 0) {
                if (b->fase == 3 && b->largo < 0) { b->fase = 9; break; }
                b->fase = -1; break;
            }
            if (!x) break;
            c->n = (uint32_t)x; c->i = 0;
        }
        while (c->i < c->n && b->fase > 0 && b->fase < 9) {
            if (b->fase == 3) {
                uint32_t a = c->n - c->i;
                if (b->largo >= 0 && (uint64_t)a > (uint64_t)b->largo - b->n) a = (uint32_t)((uint64_t)b->largo - b->n);
                bajada_copiar(b, c->rx + c->i, a); c->i += a;
                if (b->fase == 3 && b->largo >= 0 && b->n == (uint64_t)b->largo) b->fase = 9;
            } else if (b->fase == 5) {
                uint32_t a = c->n - c->i;
                if ((uint64_t)a > b->resta) a = (uint32_t)b->resta;
                bajada_copiar(b, c->rx + c->i, a); c->i += a; b->resta -= a;
                if (b->fase == 5 && !b->resta) b->fase = 6;
            } else {
                char ch = (char)c->rx[c->i++];
                if (ch == '\n') bajada_linea(b);
                else if (b->ln + 1 < (int)sizeof(b->l)) b->l[b->ln++] = ch;
            }
        }
    }
    if (b->fase < 0) { con_cerrar(c); return -1; }
    if (b->fase != 9) return 0;
    if (b->cerrar) con_cerrar(c);
    if ((b->estado == 301 || b->estado == 302 || b->estado == 303 || b->estado == 307) && b->lugar[0] && b->redir < 5) {
        b->redir++;
        static char u[2048]; cp(u, b->lugar, sizeof(u));
        return bajada_iniciar(b, u) ? -1 : 0;
    }
    return b->estado == 200 ? 1 : -1;
}
static int bajada_esperar(struct bajada *b, long limite_ms) {
    long t0 = taek_ms();
    for (;;) {
        int r = bajada_avanzar(b, 20);
        if (r) return r;
        if (taek_ms() - t0 > limite_ms) return -1;
    }
}
static void bajada_soltar(struct bajada *b);
static int bajada_empezar(struct bajada *b, struct con *c, uint8_t *buf, uint64_t cap, const char *url) {
    bajada_soltar(b);
    b->c = c; b->buf = buf; b->cap = cap; b->redir = 0; b->archivo = 0;
    return bajada_iniciar(b, url);
}
/* Suelta lo que una bajada a medias tenga abierto (un descriptor del USB) */
static void bajada_soltar(struct bajada *b) {
    if (b->archivo && b->fd >= 0) { ar_cerrar_fd(b->fd); b->fd = -1; }
}
static void bajada_archivo(struct bajada *b, uint8_t *buf, uint64_t cap, const char *ruta) {
    bajada_soltar(b);
    b->c = 0; b->buf = buf; b->cap = cap; b->n = 0; b->archivo = 1; b->estado = 200; b->largo = -1; b->fase = 3;
    b->fd = -1; b->total = 0;
    cp(b->url, ruta, sizeof(b->url));
}
/* GET de una URL absoluta https://... a 'dst' (sigue hasta 4 redirecciones). */
static int64_t bajar_url(const char *url, uint8_t *dst, uint64_t cap, int *estado_http) {
    static char u2[4096];
    cp(u2, url, sizeof(u2));
    for (int redir = 0; redir < 5; redir++) {
        char host[160]; const char *ruta;
        if (partir_url(u2, host, sizeof(host), &ruta)) return -1;
        static char ruta_c[4096]; cp(ruta_c, ruta, sizeof(ruta_c));
        struct resp r;
        int64_t n = http(&c_vid, host, "GET", ruta_c, "User-Agent: TAEK-OS/1.0\r\n", 0, 0, dst, cap, &r, 0);
        if (estado_http) *estado_http = r.estado;
        if (n < 0) return n;
        if ((r.estado == 301 || r.estado == 302 || r.estado == 303 || r.estado == 307) && r.lugar[0]) { cp(u2, r.lugar, sizeof(u2)); continue; }
        return r.estado == 200 ? n : -100 - r.estado;
    }
    return -2;
}

/* ------------------------------------------------------------------ fuente de un video: HLS en linea o descarga */
struct variante { int lado, w, h; uint64_t bw; char grupo[24]; char *url; };
struct fuente {
    struct video *v;
    int offline; char carpeta[200];
    char **vs, **as; int nvs, nas;
    uint32_t *ini;                         /* inicio de cada segmento de video en ms; ini[nvs] = duracion */
    int lado;
    int ancho, alto;                       /* H201: de la variante HLS (0 si no se sabe: se supone 16:9) */
};
static uint64_t num_tras(const char *l, const char *clave) {
    const char *p = hallar(l, l + lg(l), clave);
    uint64_t v = 0;
    if (p) for (p += lg(clave); *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint64_t)(*p - '0');
    return v;
}
static void cadena_tras(const char *l, const char *clave, char *o, int cap) {
    const char *p = hallar(l, l + lg(l), clave);
    int k = 0;
    if (p) for (p += lg(clave); *p && *p != '"' && k + 1 < cap; p++) o[k++] = *p;
    o[k] = 0;
}
static char *duplicar(const char *s, int n) { char *d = malloc((size_t)n + 1); if (d) { memcpy(d, s, (size_t)n); d[n] = 0; } return d; }
static void liberar_urls(char **u, int n) { if (!u) return; for (int i = 0; i < n; i++) free(u[i]); free(u); }
/* Lista m3u8 -> URLs y (si ini) el inicio de cada segmento en ms segun #EXTINF */
static int lista_segmentos(char *txt, int64_t n, char ***out, uint32_t **ini_out) {
    int cap = 64, k = 0;
    char **u = malloc(sizeof(char *) * (size_t)cap);
    uint32_t *ini = malloc(sizeof(uint32_t) * (size_t)(cap + 1));
    if (!u || !ini) { free(u); free(ini); return -1; }
    uint32_t t = 0, dur = 0;
    for (int64_t i = 0; i < n; ) {
        int64_t j = i; while (j < n && txt[j] != '\n' && txt[j] != '\r') j++;
        if (j - i > 8 && prefijo_ci(txt + i, "#EXTINF:")) {
            uint32_t ent = 0, fr = 0, div = 1; int64_t q = i + 8;
            while (q < j && txt[q] >= '0' && txt[q] <= '9') ent = ent * 10 + (uint32_t)(txt[q++] - '0');
            if (q < j && txt[q] == '.') for (q++; q < j && txt[q] >= '0' && txt[q] <= '9' && div < 1000; q++) { fr = fr * 10 + (uint32_t)(txt[q] - '0'); div *= 10; }
            dur = ent * 1000 + fr * 1000 / div;
        } else if (j > i && txt[i] != '#') {
            if (k == cap) {
                cap *= 2;
                char **x = realloc(u, sizeof(char *) * (size_t)cap); uint32_t *y = realloc(ini, sizeof(uint32_t) * (size_t)(cap + 1));
                if (!x || !y) { if (x) u = x; if (y) ini = y; break; }
                u = x; ini = y;
            }
            u[k] = duplicar(txt + i, (int)(j - i));
            if (u[k]) { ini[k] = t; t += dur; k++; }
            dur = 0;
        }
        i = j + 1;
    }
    ini[k] = t;
    *out = u;
    if (ini_out) *ini_out = ini; else free(ini);
    return k;
}
static void fuente_liberar(struct fuente *R) {
    liberar_urls(R->vs, R->nvs); liberar_urls(R->as, R->nas); free(R->ini);
    R->vs = R->as = 0; R->ini = 0; R->nvs = R->nas = 0;
}
static uint32_t fuente_dur(const struct fuente *R) { return R->ini && R->nvs ? R->ini[R->nvs] : 0; }
static int g_flujo_ok;
/* /player VISIONOS -> HLS: la mejor variante H.264 hasta g_calidad y sus listas de video y audio. 0 bien. */
static int preparar_hls(struct video *v, struct fuente *R) {
    memset(R, 0, sizeof(*R)); R->v = v;
    asegurar_visitante();
    if (!g_visitante[0]) { yt_log("hls", "sin visitorData"); return -1; }
    ocupado("Pidiendo el video...", 1);
    static char cuerpo[1400], extra[900];
    cuerpo[0] = 0;
    add(cuerpo, "{\"context\":{\"client\":{\"clientName\":\"VISIONOS\",\"clientVersion\":\"1.02\",\"hl\":\"es\",\"gl\":\"PE\",\"visitorData\":\"", sizeof(cuerpo));
    add(cuerpo, g_visitante, sizeof(cuerpo)); add(cuerpo, "\"}},\"videoId\":\"", sizeof(cuerpo)); add(cuerpo, v->id, sizeof(cuerpo));
    add(cuerpo, "\",\"contentCheckOk\":true,\"racyCheckOk\":true,\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"}}}", sizeof(cuerpo));
    extra[0] = 0;
    add(extra, "Content-Type: application/json\r\nOrigin: https://www.youtube.com\r\nX-Youtube-Client-Name: 101\r\nX-Youtube-Client-Version: 1.02\r\nX-Goog-Visitor-Id: ", sizeof(extra));
    add(extra, g_visitante, sizeof(extra)); add(extra, "\r\n", sizeof(extra));
    struct resp r;
    int64_t m = http(&c_web, "www.youtube.com", "POST", "/youtubei/v1/player?prettyPrint=false", extra, cuerpo, (uint32_t)lg(cuerpo), g_json, JSON_CAP - 1, &r, 0);
    con_cerrar(&c_web);
    if (m <= 0 || r.estado != 200) { yt_log("hls", "player VISIONOS fallo"); return -1; }
    const char *j = (const char *)g_json, *fin = j + m;
    if (!hallar(j, fin, "\"status\":\"OK\"")) { char e[120] = ""; json_texto_de(j, fin, "status", e, sizeof(e)); yt_log("hls no disponible", e); return -1; }
    if (!v->titulo[0]) { const char *t = hallar(j, fin, "\"videoDetails\""); if (t) { json_valor(t, fin, "title", v->titulo, sizeof(v->titulo)); json_valor(t, fin, "author", v->canal, sizeof(v->canal)); } }
    const char *hu = hallar(j, fin, "\"hlsManifestUrl\":\"");
    if (!hu) { yt_log("hls", "sin hlsManifestUrl"); return -1; }
    static char maestro_url[4096];
    json_cadena(hu + 17, fin, maestro_url, sizeof(maestro_url));
    int eh = 0;
    m = bajar_url(maestro_url, g_json, JSON_CAP - 1, &eh);
    if (m <= 0) { yt_log("hls", "manifiesto fallo"); return -1; }
    char *txt = (char *)g_json; txt[m] = 0;
    struct variante var[24]; int nv = 0;
    char *audio_url[8] = { 0 }; char audio_grupo[8][24]; int audio_def[8] = { 0 }, na = 0;
    for (int64_t i = 0; i < m; ) {
        int64_t k = i; while (k < m && txt[k] != '\n') k++;
        txt[k] = 0; if (k > i && txt[k - 1] == '\r') txt[k - 1] = 0;
        char *l = txt + i;
        if (prefijo_ci(l, "#EXT-X-STREAM-INF:") && hallar(l, l + lg(l), "avc1") && hallar(l, l + lg(l), "mp4a.40.2") && nv < 24) {
            struct variante *x = &var[nv];
            memset(x, 0, sizeof(*x));
            x->bw = num_tras(l, "BANDWIDTH=");
            const char *rs = hallar(l, l + lg(l), "RESOLUTION=");
            if (rs) { rs += 11; while (*rs >= '0' && *rs <= '9') x->w = x->w * 10 + (*rs++ - '0'); if (*rs == 'x') rs++; while (*rs >= '0' && *rs <= '9') x->h = x->h * 10 + (*rs++ - '0'); }
            x->lado = x->w < x->h ? x->w : x->h;
            cadena_tras(l, "AUDIO=\"", x->grupo, sizeof(x->grupo));
            int64_t a = k + 1, b = a; while (b < m && txt[b] != '\n') b++;
            if (b > a && txt[b - 1] == '\r') b--;
            x->url = duplicar(txt + a, (int)(b - a));
            if (x->url && x->lado) nv++;
        } else if (prefijo_ci(l, "#EXT-X-MEDIA:") && hallar(l, l + lg(l), "TYPE=AUDIO") && na < 8) {
            char u[3000]; cadena_tras(l, "URI=\"", u, sizeof(u));
            cadena_tras(l, "GROUP-ID=\"", audio_grupo[na], sizeof(audio_grupo[na]));
            audio_def[na] = hallar(l, l + lg(l), "DEFAULT=YES") != 0;
            audio_url[na] = duplicar(u, lg(u));
            if (audio_url[na]) na++;
        }
        i = k + 1;
    }
    int elegida = -1;
    for (int a = 0; a < nv; a++) {
        if (var[a].lado > g_calidad) continue;
        if (elegida < 0 || var[a].lado > var[elegida].lado || (var[a].lado == var[elegida].lado && var[a].bw > var[elegida].bw)) elegida = a;
    }
    const char *aurl = 0;
    if (elegida >= 0)
        for (int a = 0; a < na; a++) if (!lg(var[elegida].grupo) || igual(audio_grupo[a], var[elegida].grupo)) { if (!aurl || audio_def[a]) aurl = audio_url[a]; }
    int ok = -1;
    if (elegida < 0 || !aurl) { yt_log("hls", "ninguna variante"); goto fin; }
    {
        char m2[120] = ""; addn(m2, (uint64_t)var[elegida].w, sizeof(m2)); add(m2, "x", sizeof(m2)); addn(m2, (uint64_t)var[elegida].h, sizeof(m2));
        add(m2, ", ", sizeof(m2)); addn(m2, var[elegida].bw / 1000, sizeof(m2)); add(m2, " kbit/s", sizeof(m2));
        yt_log("hls variante", m2);
    }
    R->lado = var[elegida].lado;
    R->ancho = (int)var[elegida].w; R->alto = (int)var[elegida].h;
    m = bajar_url(var[elegida].url, g_json, JSON_CAP - 1, &eh);
    if (m <= 0 || (R->nvs = lista_segmentos((char *)g_json, m, &R->vs, &R->ini)) <= 0) { yt_log("hls", "lista de video fallo"); R->nvs = 0; goto fin; }
    m = bajar_url(aurl, g_json, JSON_CAP - 1, &eh);
    if (m <= 0 || (R->nas = lista_segmentos((char *)g_json, m, &R->as, 0)) <= 0) { yt_log("hls", "lista de audio fallo"); R->nas = 0; goto fin; }
    ok = 0;
fin:
    for (int a = 0; a < nv; a++) free(var[a].url);
    for (int a = 0; a < na; a++) free(audio_url[a]);
    if (ok) fuente_liberar(R);
    listo();
    return ok;
}
/* Descarga del USB -> fuente (sin red) */
static int preparar_descarga(struct video *v, struct fuente *R) {
    memset(R, 0, sizeof(*R)); R->v = v; R->offline = 1;
    uint64_t n = 0;
    uint8_t *b = info_leer(v->id, &n);
    if (!b) return -1;
    char *s = (char *)b, *fin = s + n;
    char t[24];
    info_campo(s, fin, "nv", t, sizeof(t)); R->nvs = (int)num(t);
    info_campo(s, fin, "na", t, sizeof(t)); R->nas = (int)num(t);
    info_campo(s, fin, "lado", t, sizeof(t)); R->lado = (int)num(t);
    R->ini = malloc(sizeof(uint32_t) * (size_t)(R->nvs + 1));
    char *p = despues_de(s, fin, "\nini=");
    for (int k = 0; R->ini && p && k <= R->nvs; k++) {
        R->ini[k] = num(p);
        while (p < fin && *p >= '0' && *p <= '9') p++;
        if (p < fin && *p == ',') p++;
    }
    free(b);
    if (!R->ini || R->nvs <= 0 || !p) { free(R->ini); R->ini = 0; R->nvs = R->nas = 0; return -1; }
    R->carpeta[0] = 0; add(R->carpeta, "/youtube/descargas/", sizeof(R->carpeta)); add(R->carpeta, v->id, sizeof(R->carpeta));
    return 0;
}
static void ruta_seg(char *o, int cap, const struct fuente *R, int k, int audio) {
    cp(o, R->carpeta, cap); add(o, audio ? "/a" : "/v", cap);
    if (k < 100) add(o, "0", cap);
    if (k < 10) add(o, "0", cap);
    addn(o, (uint64_t)k, cap); add(o, audio ? ".aac" : ".ts", cap);
}

/* ------------------------------------------------------------------ reproductor (H187 flujo, H188 controles) */
static struct hm g_hm;
#define FL_INF ((int64_t)0x7FFFFFFFFFFFFFFFLL)
#define FL_RAW_V (16u << 20)
#define FL_RAW_A (2u << 20)
#define FL_MUESTRAS (20u << 20)
static uint8_t *g_raw_v, *g_raw_a, *g_muestras;
static struct bajada g_bv, g_ba;
enum { S_FIN, S_SALIR, S_PAUSA, S_SALTO, S_ERROR, S_CAMBIO };   /* H202: S_CAMBIO = otro short */
static int buffers(void) {
    if (!g_raw_v) g_raw_v = malloc(FL_RAW_V);
    if (!g_raw_a) g_raw_a = malloc(FL_RAW_A);
    if (!g_muestras) g_muestras = malloc(FL_MUESTRAS);
    return g_raw_v && g_raw_a && g_muestras ? 0 : -1;
}
static int seg_pedir(struct fuente *R, int k) {
    if (R->offline) {
        char r[260];
        ruta_seg(r, sizeof(r), R, k, 0); bajada_archivo(&g_bv, g_raw_v, FL_RAW_V, r);
        if (k < R->nas) { ruta_seg(r, sizeof(r), R, k, 1); bajada_archivo(&g_ba, g_raw_a, FL_RAW_A, r); }
        return 0;
    }
    if (bajada_empezar(&g_bv, &c_vid, g_raw_v, FL_RAW_V, R->vs[k])) return -1;
    if (k < R->nas && bajada_empezar(&g_ba, &c_aud, g_raw_a, FL_RAW_A, R->as[k])) return -1;
    return 0;
}
static int seg_esperar(struct fuente *R, int k) {
    if (bajada_esperar(&g_bv, 20000) < 0) return -1;
    if (k < R->nas && bajada_esperar(&g_ba, 20000) < 0) return -1;
    return 0;
}
static int seg_bombear(struct fuente *R, int k) {
    int e = 0;
    if (g_bv.fase > 0 && g_bv.fase < 9 && bajada_avanzar(&g_bv, 1) < 0) e = -1;
    if (k < R->nas && g_ba.fase > 0 && g_ba.fase < 9 && bajada_avanzar(&g_ba, 1) < 0) e = -1;
    return e;
}
/* ------------------------------------------------------------------ paso 5: Reproductor con el video en marco (H201)
 * La pagina entera del prototipo (.watch-layout) se dibuja como capa de la sesion (SYS_FLUJO OSD, a pantalla completa) con
 * un hueco transparente en el reproductor; Iris pone el video en ese marco (SYS_FLUJO MARCO) y la capa encima. Asi las
 * esquinas cortadas, las escuadras, la etiqueta DECODE y el contador FRAME quedan sobre el video. En pausa y mientras carga
 * la misma pagina se dibuja en la superficie de la app, con la miniatura en el marco. */
static uint32_t *g_capa_px;

/* ------------------------------------------------------------------ paso 6: SHORTS (H202)
 * .shorts-layout del prototipo: el anterior | el marco 9:16 | el siguiente | RAFAGA. Sin video, la miniatura recortada
 * (cover) en el marco; con video, el mismo hueco + marco de H201 (SYS_FLUJO MARCO) y la capa encima: el filo amarillo,
 * el contador, la resolucion y el titulo sobre un degradado quedan encima del video. */
static long g_sh_t0 = -100000;
static int g_sh_dir = 1, g_sh_var;
static void sh_geometria(int *fx, int *fy, int *fw, int *fh, int *ix, int *dx, int *lado, int *mx, int *mw) {
    int x0 = X0, x1 = AN - S(22), y0 = VISTA_Y, y1 = AL - S(36) - S(14), sw = x1 - x0, gap = S(24);
    *fh = y1 - y0;
    if (*fh > AL * 76 / 100) *fh = AL * 76 / 100;
    *fw = *fh * 9 / 16;
    *mw = sw * 26 / 100; if (*mw < S(220)) *mw = S(220);
    *lado = (sw - *fw - *mw - 3 * gap) / 2;
    if (*lado < 0) *lado = 0;
    *ix = x0;
    *fx = x0 + *lado + gap;
    *dx = *fx + *fw + gap;
    *mx = *dx + *lado + gap;
    *fy = y0 + (y1 - y0 - *fh) / 2;
}
/* Miniatura recortada al centro (object-fit: cover), con chaflan; gris y tenue para los costados */
static void imagen_cover(const uint32_t *o, int ow, int oh, int x, int y, int w, int h, int cut, int gris, uint32_t alfa, int cy0, int cy1) {
    int sw = (int)((int64_t)oh * w / h), sx0 = 0, sh = oh, sy0 = 0;
    if (sw > ow) { sw = ow; sh = (int)((int64_t)ow * h / w); sy0 = (oh - sh) / 2; } else sx0 = (ow - sw) / 2;
    for (int yy = 0; yy < h; yy++) {
        int fy = y + yy;
        if (fy < 0 || fy >= L.al || fy < cy0 || fy >= cy1) continue;
        int a = 0, b = w;
        if (yy < cut) b -= cut - yy;
        if (yy >= h - cut) a += yy - (h - cut);
        uint32_t *d = L.px + (size_t)fy * (size_t)L.an;
        const uint32_t *f = o + (size_t)(sy0 + yy * sh / h) * (size_t)ow + sx0;
        for (int xx = a; xx < b; xx++) {
            int fx = x + xx;
            if (fx < 0 || fx >= L.an) continue;
            uint32_t p = f[xx * sw / w];
            if (gris) { uint32_t g = (((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8; p = 0xFF000000u | g << 16 | g << 8 | g; }
            d[fx] = alfa >= 255 ? p : sobre(d[fx], cubrir(p | 0xFF000000u, alfa));
        }
    }
}
static void sh_lateral(int i, int cx, int cy, int desplaza) {
    if (nres < 2) return;
    int w = S(120), h = w * 16 / 9, x = cx - w / 2, y = cy - h / 2 + desplaza;
    struct video *v = &res[(i % nres + nres) % nres];
    if (v->mini) imagen_cover(v->mini, MINI_AN, MINI_AL, x, y, w, h, S(10), 1, 89, 0, AL);
    else chaflan(&L, x, y, w, h, S(10), premul(60, Q_BG2));
    chaflan_borde(&L, x, y, w, h, S(10), 1, premul(60, Q_DIM2));
}
/* con_video: 1 = capa de la sesion (hueco); 0 = vista o pausa (miniatura) */
static void dibujar_shorts(int con_video) {
    int fx, fy, fw, fh, ix, dx, lado, mx, mw;
    sh_geometria(&fx, &fy, &fw, &fh, &ix, &dx, &lado, &mx, &mw);
    int i = sel;
    struct video *v = nres ? &res[(i % nres + nres) % nres] : 0;
    long el = taek_ms() - g_sh_t0;
    /* .shorts-side: el anterior arriba (-14 %) y el siguiente abajo (+14 %) */
    int ch = fy + fh / 2;
    sh_lateral(i - 1, ix + lado / 2, ch, -fh * 14 / 100 / 2);
    sh_lateral(i + 1, dx + lado / 2, ch, fh * 14 / 100 / 2);
    int cut = S(20);
    /* .short-frame */
    if (con_video) {
        for (int f = 0; f < fh; f++) {
            int a = 0, b = fw;
            if (f < cut) b -= cut - f;
            if (f >= fh - cut) a += f - (fh - cut);
            memset(L.px + (size_t)(fy + f) * (size_t)AN + fx + a, 0, (size_t)(b - a) * 4);
        }
    } else {
        /* entrada al cambiar: slide (el contenido corre dentro del marco) o slice (se descubre por filas) */
        float t = lim01((float)el / 300.0f), e = e_out(t);
        int corre = t < 1 && g_sh_var == 0 ? (int)((float)(g_sh_dir * fh * 60 / 100) * (1 - e)) : 0;
        chaflan(&L, fx, fy, fw, fh, cut, premul(255, 0x000000u));
        if (v && v->mini) imagen_cover(v->mini, MINI_AN, MINI_AL, fx, fy + corre, fw, fh, cut, 0, 255, fy, fy + fh);
        else if (v) gato(&L, fx + fw / 2, fy + fh / 2, S(80), premul(120, TENUE), premul(200, Q_Y));
        if (t < 1 && g_sh_var == 1 && g_fondo) {
            int vis = (int)((float)fh * pasos(t, 8));
            int ya = g_sh_dir > 0 ? fy + vis : fy, yb = g_sh_dir > 0 ? fy + fh : fy + fh - vis;
            for (int f = ya; f < yb; f++) memcpy(L.px + (size_t)f * AN + fx, g_fondo + (size_t)f * AN + fx, (size_t)fw * 4);
            if (yb > ya) rect(&L, fx, g_sh_dir > 0 ? ya : yb - S(2), fw, S(2), premul(255, Q_Y));
        }
        if (g_rep.activo && g_rep.pausa) chaflan(&L, fx, fy, fw, fh, cut, premul(110, 0x000000u));
    }
    /* .short-info: degradado abajo con el titulo y el canal */
    int gi = fh * 30 / 100;
    for (int f = 0; f < gi; f++) {
        int y = fy + fh - gi + f, a = 0, b = fw;
        if (f >= gi - cut) a += f - (gi - cut);
        rect(&L, fx + a, y, b - a, 1, premul((uint32_t)(235 * f / gi), Q_BG));
    }
    if (v) {
        titulo_2(fx + S(14), fy + fh - S(50), S(18), fw - S(28), v->titulo, Q_TXT, S(20));
        char c[110] = "@"; { int k = 1; for (const char *p = v->canal; *p && k + 1 < (int)sizeof(c); p++) if (*p != ' ') c[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p); c[k] = 0; }
        tmono_cortado(&L, fx + S(14), fy + fh - S(14), S(11), Q_CIAN, 255, c, fw - S(28));
    }
    /* ::before: el filo amarillo */
    chaflan_borde(&L, fx, fy, fw, fh, cut, S(2), premul(255, Q_Y));
    /* .sf-hud: posicion y resolucion */
    char pos[24] = ""; { struct txt t2; t_ini(&t2); t_dos(&t2, (uint64_t)(nres ? (i % nres + nres) % nres + 1 : 0)); t_add(&t2, "/"); t_dos(&t2, (uint64_t)nres); cp(pos, t2.s, sizeof(pos)); }
    char rs[32] = "-";
    if (g_rep.activo && g_rep.vw) { rs[0] = 0; addn(rs, (uint64_t)g_rep.vw, sizeof(rs)); add(rs, "\xC3\x97", sizeof(rs)); addn(rs, (uint64_t)g_rep.vh, sizeof(rs)); }
    else cp(rs, "- \xC3\x97 -", sizeof(rs));
    int pw = tmono_ancho(S(11), pos) + S(12), rw = tmono_ancho(S(11), rs) + S(12);
    rect(&L, fx + S(12), fy + S(10), pw, S(18), premul(191, Q_BG));
    tmono(&L, fx + S(18), fy + S(23), S(11), Q_Y, 255, pos);
    rect(&L, fx + fw - S(12) - rw - cut / 2, fy + S(10), rw, S(18), premul(191, Q_BG));
    tmono(&L, fx + fw - S(12) - rw - cut / 2 + S(6), fy + S(23), S(11), Q_Y, 255, rs);
    if (g_rep.activo && g_rep.pausa) {
        int b = S(80), bx = fx + fw / 2 - b / 2, by = fy + fh / 2 - b / 2;
        chaflan(&L, bx, by, b, b, S(16), premul(153, Q_BG));
        chaflan_borde(&L, bx, by, b, b, S(16), S(2), premul(255, Q_Y));
        triangulo(&L, (float)(bx + b / 2 - S(10)), (float)(by + b / 2 - S(17)), (float)(bx + b / 2 - S(10)), (float)(by + b / 2 + S(17)), (float)(bx + b / 2 + S(17)), (float)(by + b / 2), premul(255, Q_Y));
    }
    /* .short-meta: RAFAGA */
    int mh = S(30) + S(10) + S(37) * (g_rep.activo ? 5 : 3) + S(70);
    int my = fy + (fh - mh) / 2;
    panel_q(&L, mx, my, mw, mh, S(14));
    cabeza_panel(mx, my, mw, "R\xC3\x81\x46\x41GA", "VERTICAL");
    int gx = mx + S(8), gy = my + S(38), gw = mw - S(16), c2 = (gw - 1) / 2, filas = g_rep.activo ? 5 : 3;
    rect(&L, gx, gy, gw, S(37) * filas, Q_LINEA);
    if (v) {
        char d[32]; cp(d, v->dur, sizeof(d));
        kv(gx, gy, c2, "ID", v->id, Q_Y); kv(gx + c2 + 1, gy, gw - c2 - 1, "DURACI\xC3\x93N", d, Q_CIAN);
        kv(gx, gy + S(37), gw, "CANAL", v->canal[0] ? v->canal : "-", Q_TXT);
        kv(gx, gy + S(74), c2, "VISTAS", v->vistas[0] ? v->vistas : "-", Q_TXT); kv(gx + c2 + 1, gy + S(74), gw - c2 - 1, "FUENTE", "#shorts", Q_TXT);
        if (g_rep.activo) {
            char a[24] = "", b[24] = "", m[24] = "";
            addn(a, g_rep.cuadros, sizeof(a)); addn(b, g_rep.tardios, sizeof(b));
            cp(m, !con_video ? "-" : g_rep.iris ? "IRIS" : "CPU", sizeof(m));
            kv(gx, gy + S(111), c2, "CUADROS", a, Q_TXT); kv(gx + c2 + 1, gy + S(111), gw - c2 - 1, "TARD\xC3\x8DOS", b, g_rep.tardios ? Q_ROJO : Q_TXT);
            kv(gx, gy + S(148), c2, "SALIDA", m, Q_CIAN);
            char tc[24]; fmt_tiempo(tc, sizeof(tc), g_rep.pos); kv(gx + c2 + 1, gy + S(148), gw - c2 - 1, "POSICI\xC3\x93N", tc, Q_Y);
        }
    }
    /* .hint */
    int hy = gy + S(37) * filas + S(30), hx = gx + S(4);
    hx = tecla_flechas(&L, hx, hy, "UD") + S(6); hx = tmono(&L, hx, hy, S(11), Q_DIM2, 255, "CAMBIAR") + S(12);
    hx = tecla_q(&L, hx, hy, g_rep.activo ? "ESPACIO" : "INTRO") + S(6); tmono(&L, hx, hy, S(11), Q_DIM2, 255, g_rep.activo ? "PAUSA" : "VER");
    if (!nres && ocupado_pct < 0) tmono(&L, fx + S(14), fy + fh / 2, S(12), Q_DIM2, 255, "> 0 R\xC3\x81\x46\x41GAS // NO LLEG\xC3\x93 NADA");
}
/* Geometria: el reproductor (.player, 16:9) y el video encajado dentro (object-fit: contain) */
static void rep_geometria(int *px, int *py, int *pw, int *ph, int *vx, int *vy, int *vw, int *vh) {
    int ancho = AN - S(22) - X0, lado = ancho * 28 / 100;
    if (lado < S(250)) lado = S(250);
    *pw = ancho - lado - S(18); *ph = *pw * 9 / 16;
    if (*ph > AL * 58 / 100) { *ph = AL * 58 / 100; *pw = *ph * 16 / 9; }
    *px = X0; *py = VISTA_Y;
    if (g_rep.shorts) { int ix, dx, ld, mx, mw; sh_geometria(px, py, pw, ph, &ix, &dx, &ld, &mx, &mw); }   /* H202: el marco 9:16 */
    int w = g_rep.vw > 0 ? g_rep.vw : 16, h = g_rep.vh > 0 ? g_rep.vh : 9;
    if ((int64_t)*pw * h <= (int64_t)*ph * w) { *vw = *pw; *vh = (int)((int64_t)*pw * h / w); }
    else { *vh = *ph; *vw = (int)((int64_t)*ph * w / h); }
    *vx = *px + (*pw - *vw) / 2; *vy = *py + (*ph - *vh) / 2;
}
static void icono_play(int cx, int cy, int r, uint32_t c) {
    triangulo(&L, (float)(cx - r * 6 / 10), (float)(cy - r), (float)(cx - r * 6 / 10), (float)(cy + r), (float)(cx + r), (float)cy, c);
}
static void icono_pausa(int cx, int cy, int r, uint32_t c) {
    rect(&L, cx - r * 8 / 10, cy - r, r * 6 / 10, 2 * r, c); rect(&L, cx + r * 2 / 10, cy - r, r * 6 / 10, 2 * r, c);
}
/* con_video: 1 = la capa de la sesion (hueco para el video); 0 = la app (miniatura en el marco) */
static void dibujar_reproductor(int con_video) {
    if (g_rep.shorts) {   /* H202: la pagina de Shorts, con el video en su marco vertical */
        fondo_poner(); hud_q(); menu_q();
        dibujar_shorts(con_video);
        int py2 = AL - S(36), yb = py2 + S(24);
        rect(&L, 0, py2, AN, S(36), premul(178, Q_BG)); rect(&L, 0, py2, AN, 1, Q_LINEA);
        const char *k[4][2] = { { "ESPACIO", g_rep.pausa ? "SEGUIR" : "PAUSA" }, { "^UD", "CAMBIAR" }, { "^LR", "10 S" }, { "ESC", "SALIR" } };
        int ak = 0; for (int q = 0; q < 4; q++) ak += pista_ancho(k[q][0], k[q][1]);
        int kx = AN - S(24) - ak; for (int q = 0; q < 4; q++) kx = pista(&L, kx, yb, k[q][0], k[q][1]);
        tmono(&L, S(24), yb, S(12), g_rep.aviso[0] ? Q_Y : Q_DIM2, 255, g_rep.aviso[0] ? g_rep.aviso : "/qm2/shorts");
        return;
    }
    struct fuente *R = g_rep.R;
    struct video *v = R ? R->v : 0;
    long ahora = taek_ms();
    fondo_poner();
    hud_q();
    menu_q();
    int px, py, pw, ph, vx, vy, vw, vh;
    rep_geometria(&px, &py, &pw, &ph, &vx, &vy, &vw, &vh);
    int cut = S(22);
    /* .player */
    if (con_video) {
        for (int f = 0; f < ph; f++) {       /* el hueco: lo de adentro del chaflan queda transparente */
            int a = 0, b = pw;
            if (f < cut) b -= cut - f;
            if (f >= ph - cut) a += f - (ph - cut);
            memset(L.px + (size_t)(py + f) * (size_t)AN + px + a, 0, (size_t)(b - a) * 4);
        }
    } else {
        chaflan(&L, px, py, pw, ph, cut, premul(255, 0x000000u));
        if (v && v->mini) imagen_chaflan(v->mini, MINI_AN, MINI_AL, px, py, pw, ph, cut);   /* la miniatura es 16:9, como el reproductor */
        else gato(&L, px + pw / 2, py + ph / 2, S(80), premul(120, TENUE), premul(200, Q_Y));
        chaflan(&L, px, py, pw, ph, cut, premul(g_rep.pausa ? 120 : 70, 0x000000u));
    }
    /* .pl-corner */
    int cl = S(30), cg = S(2);
    uint32_t cc = premul(204, Q_CIAN);
    rect(&L, px + S(10), py + S(10), cl, cg, cc); rect(&L, px + S(10), py + S(10), cg, cl, cc);
    rect(&L, px + pw - S(30) - cl, py + S(10), cl, cg, cc); rect(&L, px + pw - S(30) - cg, py + S(10), cg, cl, cc);
    rect(&L, px + S(30), py + ph - S(10) - cg, cl, cg, cc); rect(&L, px + S(30), py + ph - S(10) - cl, cg, cl, cc);
    rect(&L, px + pw - S(10) - cl, py + ph - S(10) - cg, cl, cg, cc); rect(&L, px + pw - S(10) - cg, py + ph - S(10) - cl, cg, cl, cc);
    /* .pl-tag: DECODE con el punto rojo que late mientras corre */
    int corre = con_video && !g_rep.pausa;
    const char *tag = g_rep.pausa ? "EN PAUSA" : con_video ? (g_rep.iris ? "DECODE // IRIS" : "DECODE") : "CARGANDO";
    int tw = tmono_ancho(S(11), tag) + S(32);
    rect(&L, px + S(48), py + S(16), tw, S(20), premul(191, Q_BG));
    if (!corre || (ahora / 500) % 2 == 0) {
        float cx = (float)(px + S(48) + S(11)), cy = (float)(py + S(26)), r = (float)S(4);
        triangulo(&L, cx - r, cy, cx, cy - r, cx + r, cy, premul(255, corre ? Q_ROJO : Q_DIM));
        triangulo(&L, cx - r, cy, cx + r, cy, cx, cy + r, premul(255, corre ? Q_ROJO : Q_DIM));
    }
    tmono(&L, px + S(48) + S(22), py + S(31), S(11), corre ? Q_TXT : Q_DIM2, 255, tag);
    /* .pl-frame: el contador de cuadros presentados (de SYS_FLUJO) */
    char fr[16]; { struct txt t; t_ini(&t); uint32_t c = g_rep.cuadros % 1000000; char d[8]; for (int i = 5; i >= 0; i--) { d[i] = (char)('0' + c % 10); c /= 10; } d[6] = 0; t_add(&t, d); cp(fr, t.s, sizeof(fr)); }
    tmono(&L, px + pw - S(18) - tmono_ancho(S(10), "FRAME"), py + ph - S(50), S(10), Q_DIM2, 255, "FRAME");
    texto_esp(&L, px + pw - S(18) - ancho_esp(S(30), S(1), fr), py + ph - S(18), S(30), Q_Y, 255, S(1), fr);
    /* .big-play en pausa */
    if (g_rep.pausa) {
        int b = S(92), bx = px + pw / 2 - b / 2, by = py + ph / 2 - b / 2;
        chaflan(&L, bx, by, b, b, S(18), premul(153, Q_BG));
        chaflan_borde(&L, bx, by, b, b, S(18), S(2), premul(255, Q_Y));
        icono_play(bx + b / 2 + S(3), by + b / 2, S(20), premul(255, Q_Y));
    }
    /* .controls */
    int cy0 = py + ph + S(10), ch = S(44);
    panel_q(&L, px, cy0, pw, ch, S(10));
    int cx = px + S(18), mid = cy0 + ch / 2;
    if (g_rep.pausa || !con_video) icono_play(cx + S(8), mid, S(9), premul(255, Q_TXT)); else icono_pausa(cx + S(8), mid, S(9), premul(255, Q_TXT));
    cx += S(34);
    char tc[32]; { uint32_t p = g_rep.pos; fmt_tiempo(tc, sizeof(tc), p); add(tc, ".", sizeof(tc)); struct txt t; t_ini(&t); uint32_t ms = p % 1000; if (ms < 100) t_add(&t, "0"); if (ms < 10) t_add(&t, "0"); t_num(&t, ms); add(tc, t.s, sizeof(tc)); }
    cx = tmono(&L, cx, mid + S(5), S(14), Q_Y, 255, tc) + S(14);
    char du[24]; fmt_tiempo(du, sizeof(du), g_rep.dur);
    char tags[48] = ""; if (R) { addn(tags, (uint64_t)R->lado, sizeof(tags)); add(tags, R->offline ? "P // USB" : "P // HLS", sizeof(tags)); }
    int tgw = tmono_ancho(S(11), tags) + S(16), duw = tmono_ancho(S(14), du);
    int sx = cx, sw = px + pw - S(18) - tgw - S(14) - duw - S(14) - sx;
    if (sw > S(40)) {
        int ty = mid - S(4);
        rect(&L, sx, ty, sw, S(8), premul(255, 0x151A26u));
        if (R && R->nvs && g_rep.dur) {           /* .seek-buf: hasta donde hay segmentos pedidos */
            uint32_t idx = g_rep.k + 2 > (uint32_t)R->nvs ? (uint32_t)R->nvs : g_rep.k + 2;
            uint32_t hasta = R->ini ? R->ini[idx] : g_rep.dur;
            rect(&L, sx, ty, (int)((uint64_t)sw * (hasta > g_rep.dur ? g_rep.dur : hasta) / g_rep.dur), S(8), premul(64, Q_CIAN));
        }
        int lleno = g_rep.dur ? (int)((uint64_t)sw * (g_rep.pos > g_rep.dur ? g_rep.dur : g_rep.pos) / g_rep.dur) : 0;
        rect(&L, sx, ty, lleno, S(8), premul(255, Q_Y));
        for (int k = sx + S(9); k < sx + sw; k += S(10) > 1 ? S(10) : 2) rect(&L, k, ty, 1, S(8), premul(230, Q_BG));
        rect(&L, sx + lleno - 1, mid - S(10), S(3) > 1 ? S(3) : 2, S(20), premul(255, Q_CIAN));
    }
    tmono(&L, px + pw - S(18) - tgw - S(14) - duw, mid + S(5), S(14), Q_DIM2, 255, du);
    rect(&L, px + pw - S(18) - tgw, mid - S(9), tgw, S(18), premul(255, Q_CIAN));
    tmono(&L, px + pw - S(18) - tgw + S(8), mid + S(4), S(11), Q_BG, 255, tags);
    /* .watch-info */
    int iy = cy0 + ch + S(12), ih = AL - S(36) - S(14) - iy;
    panel_q(&L, px, iy, pw, ih, S(14));
    if (v) {
        char id[24] = "ID "; add(id, v->id, sizeof(id));
        int idw = tmono_ancho(S(11), id);
        tmono(&L, px + pw - S(28) - idw, iy + S(24), S(11), Q_DIM2, 255, id);
        titulo_2(px + S(16), iy + S(32), S(26), pw - S(60) - idw, v->titulo, Q_TXT, S(30));
        int ay = iy + S(80), av = S(42);
        char ini[8] = ""; { int k = 0; for (const char *p = v->canal; *p && k < 2; p++) if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) ini[k++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p); ini[k] = 0; }
        chaflan(&L, px + S(16), ay, av, av, S(9), premul(255, Q_BG2));
        chaflan_borde(&L, px + S(16), ay, av, av, S(9), 1, premul(255, Q_CIAN));
        texto(&L, px + S(16) + (av - texto_ancho(S(15), ini)) / 2, ay + S(27), S(15), Q_CIAN, 255, 0, ini);
        texto_cortado(px + S(70), ay + S(18), S(15), Q_TXT, 255, v->canal, pw - S(90));
        tmono_cortado(&L, px + S(70), ay + S(36), S(11), Q_DIM2, 255, v->vistas[0] ? v->vistas : "-", pw - S(90));
        /* .desc: lo que se sabe de esta reproduccion */
        int dy = ay + av + S(16);
        if (dy + S(20) < iy + ih) {
            rect(&L, px + S(16), dy, pw - S(46), 1, Q_LINEA);
            char l1[160] = "FUENTE ", l2[160] = "SALIDA ";
            if (R) {
                add(l1, R->offline ? "descarga en el USB" : "HLS de YouTube (VISIONOS)", sizeof(l1)); add(l1, " // ", sizeof(l1));
                addn(l1, (uint64_t)R->nvs, sizeof(l1)); add(l1, " segmentos", sizeof(l1));
                if (R->ancho) { add(l1, " // ", sizeof(l1)); addn(l1, (uint64_t)R->ancho, sizeof(l1)); add(l1, "x", sizeof(l1)); addn(l1, (uint64_t)R->alto, sizeof(l1)); }
            }
            add(l2, g_rep.iris ? "Iris (GPU): video en marco, la app encima" : "CPU: pantalla completa con controles abajo", sizeof(l2));
            tmono_cortado(&L, px + S(16), dy + S(22), S(12), Q_DIM2, 255, l1, pw - S(46));
            tmono_cortado(&L, px + S(16), dy + S(42), S(12), Q_DIM2, 255, l2, pw - S(46));
        }
    }
    /* .watch-side: TELEMETRIA */
    int sx0 = px + pw + S(18), swd = AN - S(22) - sx0, ty0 = VISTA_Y;
    int th = S(30) + S(37) * 4 + S(10) + S(56) + S(26);
    panel_q(&L, sx0, ty0, swd, th, S(14));
    cabeza_panel(sx0, ty0, swd, "TELEMETR\xC3\x8D\x41", "DECODIFICADOR");
    {
        int gx = sx0 + S(8), gy = ty0 + S(36), gw = swd - S(16), cw2 = (gw - 1) / 2;
        rect(&L, gx, gy, gw, S(37) * 4, Q_LINEA);
        char a[32], b[32], c[32], d[32], e[32], f2[32], g[32], h[32];
        cp(a, g_rep.motor == 1 ? "GPU INTEL" : g_rep.motor == 2 ? "GPU > SOFT" : con_video ? "SOFTWARE" : "-", sizeof(a));
        cp(b, !con_video ? "-" : g_rep.iris ? "IRIS" : "CPU", sizeof(b));
        c[0] = 0; addn(c, g_rep.cuadros, sizeof(c));
        d[0] = 0; addn(d, g_rep.tardios, sizeof(d));
        e[0] = 0; addn(e, g_rep.cola_ms, sizeof(e)); add(e, " ms", sizeof(e));
        f2[0] = 0; if (R) { addn(f2, (uint64_t)g_rep.k + 1, sizeof(f2)); add(f2, "/", sizeof(f2)); addn(f2, (uint64_t)R->nvs, sizeof(f2)); } else cp(f2, "-", sizeof(f2));
        g[0] = 0; addn(g, g_rep.kib, sizeof(g)); add(g, " KiB", sizeof(g));
        h[0] = 0; if (g_rep.vw) { addn(h, (uint64_t)g_rep.vw, sizeof(h)); add(h, "x", sizeof(h)); addn(h, (uint64_t)g_rep.vh, sizeof(h)); } else cp(h, "-", sizeof(h));
        kv(gx, gy, cw2, "MOTOR", a, Q_Y); kv(gx + cw2 + 1, gy, gw - cw2 - 1, "SALIDA", b, Q_CIAN);
        kv(gx, gy + S(37), cw2, "CUADROS", c, Q_TXT); kv(gx + cw2 + 1, gy + S(37), gw - cw2 - 1, "TARD\xC3\x8DOS", d, g_rep.tardios ? Q_ROJO : Q_TXT);
        kv(gx, gy + S(74), cw2, "COLA AUDIO", e, Q_TXT); kv(gx + cw2 + 1, gy + S(74), gw - cw2 - 1, "SEGMENTO", f2, Q_TXT);
        kv(gx, gy + S(111), cw2, "RED", g, Q_TXT); kv(gx + cw2 + 1, gy + S(111), gw - cw2 - 1, "RESOLUCI\xC3\x93N", h, Q_TXT);
        /* #tel-graph: cuadros por segundo (cian) y perdidos (rojo), una muestra por cada envio de la capa */
        int ggy = gy + S(37) * 4 + S(10), ggh = S(56);
        rect(&L, gx, ggy, gw, ggh, premul(255, Q_BG2));
        rect(&L, gx, ggy, gw, 1, Q_LINEA);
        int n = g_rep.nh < 48 ? g_rep.nh : 48, maxf = 30;
        for (int i = 0; i < n; i++) { int vv = g_rep.fps[(g_rep.nh - n + i) % 48]; if (vv > maxf) maxf = vv; }
        float paso = (float)gw / 47.0f, lx = 0, ly = 0;
        for (int i = 0; i < n; i++) {
            int k = (g_rep.nh - n + i) % 48;
            float x = (float)gx + (float)(48 - n + i) * paso, y = (float)(ggy + ggh - S(4)) - (float)(ggh - S(10)) * (float)g_rep.fps[k] / (float)maxf;
            if (g_rep.perd[k]) rect(&L, (int)x - 1, ggy + ggh - S(4) - (g_rep.perd[k] > 10 ? 10 : g_rep.perd[k]) * S(4), S(3), (g_rep.perd[k] > 10 ? 10 : g_rep.perd[k]) * S(4), premul(255, Q_ROJO));
            if (i) linea_aa(lx, ly, x, y, 2, premul(255, Q_CIAN));
            lx = x; ly = y;
        }
        tmono(&L, gx + S(4), ggy + ggh + S(18), S(10), Q_CIAN, 255, "CUADROS/s");
        tmono(&L, gx + S(100), ggy + ggh + S(18), S(10), Q_ROJO, 255, "PERDIDOS");
    }
    /* .upnext: COLA (lo que sigue en la lista de la que vino el video) */
    int uy = ty0 + th + S(12), uh = AL - S(36) - S(14) - uy;
    panel_q(&L, sx0, uy, swd, uh, S(14));
    int actual = -1;
    for (int i = 0; i < nres; i++) if (v && igual(res[i].id, v->id)) actual = i;
    char qn[24] = ""; { struct txt t; t_ini(&t); t_dos(&t, (uint64_t)(nres > 0 ? nres - 1 : 0)); cp(qn, t.s, sizeof(qn)); } add(qn, " OBJ", sizeof(qn));
    cabeza_panel(sx0, uy, swd, "COLA", qn);
    int ry = uy + S(40), n2 = 0;
    for (int j = 1; j < nres && ry + S(56) <= uy + uh; j++) {
        int i = actual >= 0 ? (actual + j) % nres : j - 1;
        struct video *u = &res[i];
        char ix[8]; struct txt t; t_ini(&t); t_dos(&t, (uint64_t)++n2); cp(ix, t.s, sizeof(ix));
        tmono(&L, sx0 + S(12), ry + S(30), S(11), Q_DIM, 255, ix);
        int tx = sx0 + S(36), tw2 = S(88), th2 = S(50);
        if (u->mini) imagen_chaflan(u->mini, MINI_AN, MINI_AL, tx, ry, tw2, th2, 0);
        else rect(&L, tx, ry, tw2, th2, premul(255, Q_BG2));
        int lx2 = tx + tw2 + S(8), lw = sx0 + swd - S(16) - lx2;
        titulo_2(lx2, ry + S(14), S(13), lw, u->titulo, Q_TXT, S(15));
        tmono_cortado(&L, lx2, ry + S(46), S(10), Q_DIM, 255, u->canal, lw);
        ry += S(58);
    }
    /* #prompts */
    int py2 = AL - S(36);
    rect(&L, 0, py2, AN, S(36), premul(178, Q_BG));
    rect(&L, 0, py2, AN, 1, Q_LINEA);
    int yb = py2 + S(24);
    const char *k[5][2] = { { "ESPACIO", g_rep.pausa ? "SEGUIR" : "PAUSA" }, { "^LR", "10 S" }, { "^UD", "1 MIN" }, { "ESC", "SALIR" } };
    int nk = 4, ancho_k = 0;
    for (int i = 0; i < nk; i++) ancho_k += pista_ancho(k[i][0], k[i][1]);
    int kx = AN - S(24) - ancho_k;
    for (int i = 0; i < nk; i++) kx = pista(&L, kx, yb, k[i][0], k[i][1]);
    if (g_rep.aviso[0]) tmono(&L, S(24), yb, S(12), Q_Y, 255, g_rep.aviso);
    else tmono(&L, S(24), yb, S(12), Q_DIM2, 255, "/qm2/reproductor");
    /* si hay un salto pedido, el aviso grande sobre el video */
    if (g_rep.aviso[0]) {
        int aw = ancho_esp(S(44), S(2), g_rep.aviso) + S(40), ah = S(64), ax = px + pw / 2 - aw / 2, ay2 = py + ph / 2 - ah / 2;
        chaflan(&L, ax, ay2, aw, ah, S(14), premul(230, Q_BG));
        chaflan_borde(&L, ax, ay2, aw, ah, S(14), S(2), premul(255, Q_Y));
        texto_esp(&L, ax + S(20), ay2 + S(48), S(44), Q_Y, 255, S(2), g_rep.aviso);
    }
}
/* La capa de la sesion: la pagina con el hueco, a SYS_FLUJO OSD. Unos 4 envios por segundo. */
static void capa_enviar(void) {
    if (!g_capa_px) g_capa_px = malloc((size_t)AN * (size_t)AL * 4);
    if (!g_capa_px) return;
    struct lienzo guarda = L;
    L = (struct lienzo){ g_capa_px, AN, AL };
    telemetria();                 /* reloj y sensores del HUD tambien en la capa */
    dibujar_reproductor(1);
    L = guarda;
    struct taek_flujo f; memset(&f, 0, sizeof(f));
    f.op = TAEK_FLUJO_OSD; f.datos = (uint64_t)(uintptr_t)g_capa_px; f.largo = (uint32_t)((uint64_t)AN * (uint64_t)AL * 4);
    f.osd_x = 0; f.osd_y = 0; f.osd_ancho = (uint32_t)AN; f.osd_alto = (uint32_t)AL; f.osd_ms = 5000;
    taek_flujo(&f);
    g_rep.capas++;
}
/* Una muestra para la grafica: cuadros por segundo y perdidos desde la anterior */
static void rep_muestra(void) {
    long ahora = taek_ms();
    if (g_rep.ult_t && ahora > g_rep.ult_t) {
        int k = g_rep.nh % 48;
        g_rep.fps[k] = (int)((long)(g_rep.cuadros - g_rep.ult_cuadros) * 1000 / (ahora - g_rep.ult_t));
        g_rep.perd[k] = (int)(g_rep.tardios - g_rep.ult_tardios);
        g_rep.nh++;
    }
    g_rep.ult_t = ahora; g_rep.ult_cuadros = g_rep.cuadros; g_rep.ult_tardios = g_rep.tardios;
}
/* Capa de controles sobre el video (SYS_FLUJO OSD): ARGB premultiplicado */
static uint32_t *g_osd_px; static struct lienzo g_osd; static int OSD_AL;
static void osd_enviar(struct fuente *R, uint32_t pos, uint32_t ms, const char *aviso) {
    if (!g_osd_px) {
        OSD_AL = S(210);
        g_osd_px = malloc((size_t)AN * (size_t)OSD_AL * 4);
        if (!g_osd_px) return;
        g_osd = (struct lienzo){ g_osd_px, AN, OSD_AL };
    }
    struct lienzo *o = &g_osd;
    limpiar(o, 0);
    degradado(o, 0, 0, AN, OSD_AL, 0, premul(225, 0x05060Au));
    uint32_t dur = fuente_dur(R);
    int x = S(48), w = AN - S(96);
    if (R->v) {
        texto_cortado_l(o, x, S(82), S(30), BLANCO, 255, R->v->titulo, w - S(400));
        char c[160] = ""; add(c, R->v->canal, sizeof(c));
        if (R->lado) { add(c, "  \xC2\xB7  ", sizeof(c)); addn(c, (uint64_t)R->lado, sizeof(c)); add(c, "p", sizeof(c)); }
        if (R->offline) add(c, "  \xC2\xB7  descargado", sizeof(c));
        texto_cortado_l(o, x, S(116), S(20), TENUE, 255, c, w - S(400));
    }
    if (aviso && aviso[0]) {
        int aw = texto_ancho(S(30), aviso) + S(48);
        redondo(o, AN - x - aw, S(46), aw, S(56), S(28), premul(230, ROJO));
        texto(o, AN - x - aw + S(24), S(85), S(30), BLANCO, 255, 0, aviso);
    }
    barra_progreso(o, x, OSD_AL - S(72), w, pos, dur, S(8));
    char t[64], d[24]; fmt_tiempo(t, sizeof(t), pos); add(t, " / ", sizeof(t)); fmt_tiempo(d, sizeof(d), dur); add(t, d, sizeof(t));
    texto(o, x, OSD_AL - S(24), S(22), BLANCO, 255, 0, t);
    const char *ay = "Espacio: pausa  \xC2\xB7  <- ->: 10 s  \xC2\xB7  arriba/abajo: 1 min  \xC2\xB7  Esc: salir";   /* el atlas no tiene flechas */
    texto(o, AN - x - texto_ancho(S(18), ay), OSD_AL - S(24), S(18), TENUE, 255, 0, ay);
    struct taek_flujo f; memset(&f, 0, sizeof(f));
    f.op = TAEK_FLUJO_OSD; f.datos = (uint64_t)(uintptr_t)g_osd_px; f.largo = (uint32_t)((uint64_t)AN * (uint64_t)OSD_AL * 4);
    f.osd_x = 0; f.osd_y = (uint32_t)(AL - OSD_AL); f.osd_ancho = (uint32_t)AN; f.osd_alto = (uint32_t)OSD_AL; f.osd_ms = ms;
    taek_flujo(&f);
}
static int segmento_de(const struct fuente *R, uint32_t pos) {
    int k = 0;
    while (k + 1 < R->nvs && R->ini[k + 1] <= pos) k++;
    return k;
}
/* Una sesion de SYS_FLUJO desde 'pos' (ms). Termina al final, o por pausa, salto o salida; deja en *nueva la posicion. */
static int sesion(struct fuente *R, uint32_t pos, uint32_t *nueva) {
    uint32_t dur = fuente_dur(R);
    if (dur > 2000 && pos > dur - 2000) pos = dur - 2000;
    *nueva = pos;
    int k = segmento_de(R, pos);
    /* H201: estado de la pagina del Reproductor */
    g_rep.R = R; g_rep.dur = dur; g_rep.pos = pos; g_rep.vw = R->ancho; g_rep.vh = R->alto; g_rep.k = (uint32_t)k;
    g_rep.iris = 0; g_rep.pausa = 0; g_rep.aviso[0] = 0; g_rep.cuadros = g_rep.tardios = 0; g_rep.ult_t = 0; g_rep.capas = 0;
    long capa_t = 0;
    char m[200];
    m[0] = 0; add(m, R->offline ? "Abriendo la descarga" : "Cargando", sizeof(m));
    if (pos) { char t[24]; fmt_tiempo(t, sizeof(t), pos); add(m, " desde ", sizeof(m)); add(m, t, sizeof(m)); }
    add(m, "...", sizeof(m));
    ocupado(m, 1);
    long t0 = taek_ms();
    g_lect_n = g_lect_ms = g_lect_b = g_lect_max = 0;
    if (seg_pedir(R, k) || seg_esperar(R, k)) {
        listo(); con_cerrar(&c_vid); con_cerrar(&c_aud);
        cp(estado, R->offline ? "No se pudo leer la descarga del USB." : "No se pudo bajar el video.", sizeof(estado));
        yt_log("flujo", "primer segmento fallo"); return S_ERROR;
    }
    struct taek_flujo f;
    int abierta = 0, cod = S_FIN, desde = 0, pos0_ok = 0, salto = 0;
    int64_t base = 0, inicio = 0, a_fin = -1, salto_obj = 0;
    uint32_t pos0 = pos, cur = pos, motor = 0, cuadros = 0, tardios = 0;
    long osd_hasta = 0, osd_t = 0, salto_t = 0;
    uint64_t bytes = 0, esperas_ms = 0;
    char aviso[48] = "";
    for (; k < R->nvs; k++) {
        bytes += g_bv.n + (k < R->nas ? g_ba.n : 0);
        hm_liberar(&g_hm); hm_iniciar(&g_hm, g_muestras, FL_MUESTRAS);
        if (hm_ts(&g_hm, g_raw_v, g_bv.n) || (k < R->nas && hm_aac(&g_hm, g_raw_a, g_ba.n)) || !g_hm.v.n) {
            yt_log("flujo", "segmento ilegible"); cp(estado, "Un segmento del video llego mal.", sizeof(estado)); cod = S_ERROR; break;
        }
        int64_t abase = g_hm.audio_ts0_ok ? (int64_t)g_hm.audio_ts0 : (a_fin >= 0 ? a_fin : (int64_t)g_hm.v.dts[0]);
        uint32_t frec = g_hm.frecuencia ? g_hm.frecuencia : 44100;
        int sig = k + 1 < R->nvs, ev = 0;
        if (!abierta) {
            base = (int64_t)g_hm.v.dts[0] - (int64_t)R->ini[k] * 90;
            if (pos > R->ini[k] + 300) { desde = 1; inicio = base + (int64_t)pos * 90; }
            static uint8_t avcc[600];
            uint32_t n = 0;
            if (g_hm.sps_n < 4 || g_hm.sps_n + g_hm.pps_n + 11 > sizeof(avcc)) { cp(estado, "El video no trae SPS/PPS.", sizeof(estado)); cod = S_ERROR; break; }
            avcc[n++] = 1; avcc[n++] = g_hm.sps[1]; avcc[n++] = g_hm.sps[2]; avcc[n++] = g_hm.sps[3];
            avcc[n++] = 0xFF; avcc[n++] = 0xE1; avcc[n++] = (uint8_t)(g_hm.sps_n >> 8); avcc[n++] = (uint8_t)g_hm.sps_n;
            memcpy(avcc + n, g_hm.sps, g_hm.sps_n); n += g_hm.sps_n;
            avcc[n++] = 1; avcc[n++] = (uint8_t)(g_hm.pps_n >> 8); avcc[n++] = (uint8_t)g_hm.pps_n;
            memcpy(avcc + n, g_hm.pps, g_hm.pps_n); n += g_hm.pps_n;
            /* H201: el marco del video (antes de ABRIR: asi la sesion presenta por Iris) */
            {
                int px, py, pw, ph, vx, vy, vw, vh;
                rep_geometria(&px, &py, &pw, &ph, &vx, &vy, &vw, &vh);
                memset(&f, 0, sizeof(f));
                f.op = TAEK_FLUJO_MARCO; f.marco_x = vx; f.marco_y = vy; f.marco_ancho = vw; f.marco_alto = vh;
                long xm = taek_flujo(&f);
                if (xm) { m[0] = 0; add(m, "MARCO devolvio -", sizeof(m)); addn(m, (uint64_t)-xm, sizeof(m)); yt_log("flujo", m); }
            }
            memset(&f, 0, sizeof(f));
            f.op = TAEK_FLUJO_ABRIR; f.datos = (uint64_t)(uintptr_t)avcc; f.largo = n; f.escala = 90000;
            f.canales = R->nas ? g_hm.canales : 0; f.frecuencia = R->nas ? frec : 0;
            f.banderas = desde ? TAEK_FLUJO_DESDE : 0; f.inicio = inicio;
            long x = taek_flujo(&f);
            listo();
            if (x) { m[0] = 0; add(m, "ABRIR devolvio -", sizeof(m)); addn(m, (uint64_t)-x, sizeof(m)); yt_log("flujo", m); cp(estado, "El nucleo no abrio el video.", sizeof(estado)); cod = S_ERROR; break; }
            abierta = 1;
            m[0] = 0; add(m, "abierto en ", sizeof(m)); addn(m, (uint64_t)(taek_ms() - t0), sizeof(m)); add(m, " ms, segmento ", sizeof(m));
            addn(m, (uint64_t)k, sizeof(m)); add(m, "/", sizeof(m)); addn(m, (uint64_t)R->nvs, sizeof(m));
            if (desde) { add(m, ", desde ", sizeof(m)); addn(m, pos, sizeof(m)); add(m, " ms", sizeof(m)); }
            g_rep.iris = (int)f.iris;
            add(m, g_rep.iris ? ", Iris con marco" : ", sin Iris (controles abajo)", sizeof(m));
            yt_log("flujo", m);
            if (g_rep.iris) { capa_enviar(); capa_t = taek_ms(); }
            else { osd_enviar(R, pos, 3500, 0); osd_hasta = taek_ms() + 3500; osd_t = taek_ms(); }
        }
        if (sig) ev = seg_pedir(R, k + 1);
        uint32_t iv = 0, ia = 0;
        while (iv < g_hm.v.n || ia < g_hm.a.n) {
            int64_t vd = iv < g_hm.v.n ? (int64_t)g_hm.v.dts[iv] : FL_INF;
            int64_t ap = ia < g_hm.a.n ? abase + (int64_t)(g_hm.a.dts[ia] * 90000u / frec) : FL_INF;
            memset(&f, 0, sizeof(f));
            if (ia < g_hm.a.n && (iv >= g_hm.v.n || ap <= vd + 54000)) {
                if (desde && ap < inicio) { ia++; continue; }        /* salto: el audio empieza en el destino */
                if (!pos0_ok) { pos0 = ap > base ? (uint32_t)((ap - base) / 90) : 0; pos0_ok = 1; }
                f.op = TAEK_FLUJO_AUDIO; f.datos = (uint64_t)(uintptr_t)(g_muestras + g_hm.a.off[ia]); f.largo = g_hm.a.tam[ia]; f.pts = ap; ia++;
            } else {
                f.op = TAEK_FLUJO_VIDEO; f.datos = (uint64_t)(uintptr_t)(g_muestras + g_hm.v.off[iv]); f.largo = g_hm.v.tam[iv];
                f.pts = (int64_t)g_hm.v.dts[iv] + g_hm.v.cts[iv]; iv++;
            }
            long t_llamada = taek_ms();
            int era_video = f.op == TAEK_FLUJO_VIDEO;
            long x = taek_flujo(&f);
            long bloqueo = taek_ms() - t_llamada;
            cuadros = f.cuadros; tardios = f.tardios; motor = f.motor;
            if (pos0_ok) cur = pos0 + (uint32_t)f.reloj_ms;
            g_rep.cuadros = cuadros; g_rep.tardios = tardios; g_rep.motor = motor; g_rep.cola_ms = f.cola_audio_ms;
            g_rep.k = (uint32_t)k; g_rep.kib = (bytes + g_bv.n) >> 10; g_rep.pos = cur;
            if (x == 1) { cod = S_SALIR; goto cerrar; }               /* Ctrl+C */
            if (x < 0) { m[0] = 0; add(m, "muestra devolvio -", sizeof(m)); addn(m, (uint64_t)-x, sizeof(m)); yt_log("flujo", m); cp(estado, "El nucleo rechazo una muestra.", sizeof(estado)); cod = S_ERROR; goto cerrar; }
            long ahora = taek_ms();
            /* Teclas del usuario (el nucleo las pasa durante la sesion) */
            for (uint32_t t = 0; t < f.teclas_n && t < sizeof(f.teclas); t++) {
                uint32_t c = ap_filtrar(f.teclas[t]);
                if (!c) continue;
                if (c == ' ' || c == 'k' || c == 'K' || c == '\n' || c == '\r') { cod = S_PAUSA; goto cerrar; }
                if (c == 'q' || c == 'Q' || c == 8 || c == 127) { cod = S_SALIR; goto cerrar; }
                if (g_rep.shorts && (c == TAEK_TECLA_ARRIBA || c == TAEK_TECLA_ABAJO)) { g_rep.cambio = c == TAEK_TECLA_ABAJO ? 1 : -1; cod = S_CAMBIO; goto cerrar; }
                int d = c == TAEK_TECLA_IZQUIERDA ? -10000 : c == TAEK_TECLA_DERECHA ? 10000 : c == TAEK_TECLA_ARRIBA ? 60000 : c == TAEK_TECLA_ABAJO ? -60000 : 0;
                if (d) {
                    if (!salto) salto_obj = cur;
                    salto = 1; salto_obj += d;
                    if (salto_obj < 0) salto_obj = 0;
                    if (dur > 1000 && salto_obj > (int64_t)dur - 1000) salto_obj = dur - 1000;
                    salto_t = ahora;
                    int64_t dif = salto_obj - (int64_t)cur;
                    aviso[0] = 0; add(aviso, dif < 0 ? "-" : "+", sizeof(aviso)); addn(aviso, (uint64_t)(dif < 0 ? -dif : dif) / 1000, sizeof(aviso)); add(aviso, " s", sizeof(aviso));
                }
                osd_hasta = ahora + 3000; osd_t = 0;
            }
            if (ap_esc_suelto()) { cod = S_SALIR; goto cerrar; }
            if (salto && ahora - salto_t > 650) { *nueva = (uint32_t)salto_obj; cod = S_SALTO; goto cerrar; }
            if (g_rep.iris) {
                /* H201: la pagina entera, unas 4 veces por segundo, o enseguida tras una tecla (osd_t = 0) */
                if (salto) { g_rep.pos = (uint32_t)salto_obj; cp(g_rep.aviso, aviso, sizeof(g_rep.aviso)); }
                if (ahora - capa_t >= 250 || !osd_t) { rep_muestra(); capa_enviar(); capa_t = ahora; osd_t = ahora; }
            } else if (ahora < osd_hasta && ahora - osd_t >= 1000) {
                osd_enviar(R, salto ? (uint32_t)salto_obj : cur, (uint32_t)(osd_hasta - ahora), salto ? aviso : 0);
                osd_t = ahora;
            }
            /* Siguiente segmento. Del USB la lectura bloquea: solo cuando el nucleo esta adelantado (la ultima muestra
             * de video espero su hora) o si este segmento se acaba. */
            if (sig && !ev && (!R->offline || (era_video && bloqueo >= 12) || iv + 45 >= g_hm.v.n) && seg_bombear(R, k + 1) < 0) ev = -1;
        }
        a_fin = abase + (int64_t)(g_hm.audio_muestras * 90000u / frec);
        if (!sig) break;
        long te = taek_ms();
        if (ev || seg_esperar(R, k + 1) < 0) {
            if (seg_pedir(R, k + 1) || seg_esperar(R, k + 1) < 0) {
                yt_log("flujo", "segmento siguiente fallo"); cp(estado, "Se corto la descarga del video.", sizeof(estado)); cod = S_ERROR; break;
            }
        }
        esperas_ms += (uint64_t)(taek_ms() - te);
    }
cerrar:
    listo();
    if (abierta) {
        memset(&f, 0, sizeof(f));
        f.op = TAEK_FLUJO_CERRAR; f.banderas = cod == S_FIN ? TAEK_FLUJO_DRENAR : 0;
        taek_flujo(&f);
        static const char *NOM[] = { "fin", "salir", "pausa", "salto", "error", "cambio" };
        m[0] = 0; addn(m, bytes >> 10, sizeof(m)); add(m, " KiB, ", sizeof(m)); addn(m, (uint64_t)(taek_ms() - t0), sizeof(m)); add(m, " ms, cuadros ", sizeof(m));
        addn(m, cuadros, sizeof(m)); add(m, ", tardios ", sizeof(m)); addn(m, tardios, sizeof(m)); add(m, ", espera red ", sizeof(m));
        addn(m, esperas_ms, sizeof(m)); add(m, " ms, motor ", sizeof(m)); addn(m, motor, sizeof(m)); add(m, ", ", sizeof(m)); add(m, NOM[cod], sizeof(m));
        add(m, " en ", sizeof(m)); addn(m, cur, sizeof(m)); add(m, " ms", sizeof(m));
        if (g_rep.iris) { add(m, ", iris, capas ", sizeof(m)); addn(m, g_rep.capas, sizeof(m)); }
        if (R->offline) {
            add(m, "; USB ", sizeof(m)); addn(m, g_lect_n, sizeof(m)); add(m, " lecturas, ", sizeof(m)); addn(m, g_lect_ms, sizeof(m));
            add(m, " ms, ", sizeof(m)); addn(m, g_lect_b >> 10, sizeof(m)); add(m, " KiB, max ", sizeof(m)); addn(m, g_lect_max, sizeof(m));
            add(m, " ms, trozo ", sizeof(m)); addn(m, g_trozo >> 10, sizeof(m)); add(m, " KiB", sizeof(m));
        }
        yt_log("flujo fin", m);
    }
    if (cod != S_SALTO) *nueva = cur;
    bajada_soltar(&g_bv); bajada_soltar(&g_ba);
    hm_liberar(&g_hm); memset(&g_hm, 0, sizeof(g_hm));
    con_cerrar(&c_vid); con_cerrar(&c_aud);
    return cod;
}
/* Pausa: la pantalla es de la app; espacio sigue (con un salto a la misma posicion) */
static int pausa(struct fuente *R, uint32_t *pos) {
    g_pausa = 1; g_pausa_pos = *pos; g_pausa_dur = fuente_dur(R); g_pausa_v = R->v;
    g_rep.pausa = 1; g_rep.pos = *pos; g_rep.aviso[0] = 0;   /* H201: la misma pagina, con la miniatura */
    yt_log("pausa", "");
    int r = S_SALIR;
    struct taek_evento e;
    for (;;) {
        long t = taek_ms();
        int fin = 0;
        for (int k = 0; k < 16; k++) {
            long hay = taek_evento(&e);
            if (hay <= 0) break;
            uint32_t c = ap_filtrar(e.tecla);
            if (!c) continue;
            if (c == ' ' || c == 'k' || c == 'K' || c == '\n' || c == '\r') { r = S_SALTO; fin = 1; break; }
            if (c == 'q' || c == 'Q' || c == 8 || c == 127) { r = S_SALIR; fin = 1; break; }
            if (g_rep.shorts && (c == TAEK_TECLA_ARRIBA || c == TAEK_TECLA_ABAJO)) { g_rep.cambio = c == TAEK_TECLA_ABAJO ? 1 : -1; r = S_CAMBIO; fin = 1; break; }
            int64_t d = c == TAEK_TECLA_IZQUIERDA ? -10000 : c == TAEK_TECLA_DERECHA ? 10000 : c == TAEK_TECLA_ARRIBA ? 60000 : c == TAEK_TECLA_ABAJO ? -60000 : 0;
            int64_t p = (int64_t)g_pausa_pos + d;
            if (p < 0) p = 0;
            if (g_pausa_dur > 1000 && p > (int64_t)g_pausa_dur - 1000) p = g_pausa_dur - 1000;
            g_pausa_pos = (uint32_t)p;
            g_rep.pos = g_pausa_pos;
        }
        if (!fin && ap_esc_suelto()) { r = S_SALIR; fin = 1; }
        if (fin) break;
        mostrar();
        long g = taek_ms() - t;
        if (g < 33) taek_esperar(33 - g);
    }
    *pos = g_pausa_pos;
    g_pausa = 0; g_rep.pausa = 0;
    return r;
}
static int g_modo_shorts;   /* H202: el siguiente reproducir es un short */
static void reproducir(struct fuente *R, uint32_t pos) {
    uint32_t dur = fuente_dur(R);
    /* H201: mientras dure, la app es la pagina del Reproductor */
    memset(&g_rep, 0, sizeof(g_rep));
    g_rep.activo = 1; g_rep.R = R; g_rep.dur = dur; g_rep.pos = pos; g_rep.vw = R->ancho; g_rep.vh = R->alto;
    g_rep.shorts = g_modo_shorts;
    g_zona = Z_VISTA;
    hist_poner(R->v, pos, dur);
    for (;;) {
        uint32_t nueva = pos;
        int c = sesion(R, pos, &nueva);
        pos = nueva;
        if (c == S_PAUSA) {
            hist_poner(R->v, pos, dur);
            int pr = pausa(R, &pos);
            if (pr == S_SALIR || pr == S_CAMBIO) c = pr; else continue;
        }
        if (c == S_SALTO) continue;
        char t[24]; fmt_tiempo(t, sizeof(t), pos);
        if (c == S_CAMBIO) { hist_poner(R->v, pos, dur); break; }                          /* H202: otro short */
        if (c == S_FIN && g_rep.shorts) { hist_poner(R->v, 0, dur); g_rep.cambio = 1; break; }   /* H202: al terminar, el siguiente */
        if (c == S_FIN) { hist_poner(R->v, 0, dur); cp(estado, "Fin del video.", sizeof(estado)); }
        else if (c == S_SALIR) { hist_poner(R->v, pos, dur); estado[0] = 0; add(estado, "Video detenido en ", sizeof(estado)); add(estado, t, sizeof(estado)); add(estado, " (Intro lo sigue).", sizeof(estado)); }
        break;
    }
    g_rep.activo = 0;
    listo();
}

/* ------------------------------------------------------------------ 360p de siempre (itag 18, SYS_VIDEO) */
static uint8_t *g_mp4; static uint64_t g_mp4_cap;
static uint64_t g_prog_base, g_prog_total; static long g_prog_t0, g_prog_ult;
static void progreso(uint64_t hecho, uint64_t total) {
    (void)total;
    long t = taek_ms();
    if (t - g_prog_ult < 120) return;
    g_prog_ult = t;
    uint64_t h = g_prog_base + hecho;
    char m[160] = "Descargando 360p: ";
    addn(m, h >> 20, sizeof(m)); add(m, " / ", sizeof(m)); addn(m, g_prog_total >> 20, sizeof(m)); add(m, " MiB  ", sizeof(m));
    if (t > g_prog_t0) { addn(m, h * 8 / (uint64_t)(t - g_prog_t0) / 1000, sizeof(m)); add(m, " Mbit/s", sizeof(m)); }
    ocupado(m, g_prog_total ? (int)(h * 100 / g_prog_total) : 1);
}
static int entregar_y_salir(struct video *v, uint8_t *mp4, uint64_t n) {
    if (taek_video(mp4, n)) { cp(estado, "El nucleo no acepto el video (memoria).", sizeof(estado)); yt_log("SYS_VIDEO fallo", v->id); return 0; }
    char abrir[TAEK_RUTA] = TAEK_VIDEO_MEM, volver[TAEK_RUTA] = "q:";
    int k = lg(abrir);
    for (const char *t = v->titulo; *t && k + 1 < 120; t++) abrir[k++] = (*t == '/' || *t == '\\') ? '-' : *t;
    abrir[k] = 0;
    add(volver, buscada, sizeof(volver));
    if (ap_lanzar(TAEK_PROGRAMA_ABRIR, abrir, volver)) { cp(estado, "No se pudo pedir la reproduccion.", sizeof(estado)); return 0; }
    yt_log("reproducir", v->id);
    return 1;
}
static int ver_360(struct video *v) {
    ocupado("Pidiendo el video...", 1);
    static char cuerpo[512];
    cuerpo[0] = 0; add(cuerpo, CONTEXTO, sizeof(cuerpo)); add(cuerpo, "\"videoId\":\"", sizeof(cuerpo)); add(cuerpo, v->id, sizeof(cuerpo));
    add(cuerpo, "\",\"contentCheckOk\":true,\"racyCheckOk\":true}", sizeof(cuerpo));
    struct resp r;
    int64_t m = http(&c_api, "youtubei.googleapis.com", "POST", "/youtubei/v1/player?prettyPrint=false", EXTRA_API,
                     cuerpo, (uint32_t)lg(cuerpo), g_json, JSON_CAP - 1, &r, 0);
    if (m <= 0 || r.estado != 200) { listo(); cp(estado, "No se pudo pedir el video (player).", sizeof(estado)); yt_log("player fallido", v->id); return 0; }
    const char *j = (const char *)g_json, *fin = j + m;
    if (!hallar(j, fin, "\"status\":\"OK\"")) {
        char motivo[160] = "";
        json_texto_de(j, fin, "reason", motivo, sizeof(motivo));
        listo(); cp(estado, "YouTube no deja verlo: ", sizeof(estado)); add(estado, motivo[0] ? motivo : "sin motivo", sizeof(estado));
        yt_log("no reproducible", estado);
        return 0;
    }
    const char *it = hallar(j, fin, "\"itag\":18,");
    const char *u = it ? hallar(it, it + 4000 < fin ? it + 4000 : fin, "\"url\":\"") : 0;
    if (!u) { listo(); cp(estado, "Este video no trae MP4 360p con URL directa (itag 18).", sizeof(estado)); yt_log("sin itag 18", v->id); return 0; }
    static char url[4096];
    json_cadena(u + 6, fin, url, sizeof(url));
    char host[160]; const char *ruta;
    if (partir_url(url, host, sizeof(host), &ruta)) { listo(); cp(estado, "URL de video invalida.", sizeof(estado)); return 0; }
    uint64_t total = 0, hecho = 0;
    g_prog_base = 0; g_prog_total = 0; g_prog_t0 = g_prog_ult = taek_ms();
    static char ruta_act[4096], extra[256];
    cp(ruta_act, ruta, sizeof(ruta_act));
    for (int redir = 0; ;) {
        uint64_t hasta = hecho + (4u << 20) - 1;
        extra[0] = 0; add(extra, "User-Agent: com.google.android.youtube/" VERSION " (Linux; U; Android 11) gzip\r\nRange: bytes=", sizeof(extra));
        addn(extra, hecho, sizeof(extra)); add(extra, "-", sizeof(extra)); addn(extra, hasta, sizeof(extra)); add(extra, "\r\n", sizeof(extra));
        if (!g_mp4) { g_mp4_cap = 8u << 20; g_mp4 = malloc(g_mp4_cap); if (!g_mp4) { listo(); cp(estado, "Sin memoria para el video.", sizeof(estado)); return 0; } }
        if (total && g_mp4_cap < total) {
            uint8_t *n2 = realloc(g_mp4, total);
            if (!n2) { listo(); cp(estado, "Sin memoria para el video.", sizeof(estado)); return 0; }
            g_mp4 = n2; g_mp4_cap = total;
        }
        g_prog_base = hecho;
        uint64_t cabe = g_mp4_cap - hecho;
        int64_t n = http(&c_vid, host, "GET", ruta_act, extra, 0, 0, g_mp4 + hecho, cabe < (4u << 20) ? cabe : (4u << 20), &r, progreso);
        if (n < 0) { listo(); cp(estado, "La descarga se corto.", sizeof(estado)); yt_log("descarga cortada", host); return 0; }
        if ((r.estado == 301 || r.estado == 302 || r.estado == 303 || r.estado == 307) && r.lugar[0] && redir < 5) {
            redir++;
            if (partir_url(r.lugar, host, sizeof(host), &ruta)) { listo(); cp(estado, "Redireccion invalida.", sizeof(estado)); return 0; }
            cp(ruta_act, ruta, sizeof(ruta_act));
            continue;
        }
        if (r.estado != 206 && r.estado != 200) {
            listo(); estado[0] = 0; add(estado, "googlevideo respondio HTTP ", sizeof(estado)); addn(estado, (uint64_t)r.estado, sizeof(estado));
            yt_log("descarga fallida", estado);
            return 0;
        }
        if (!total) {
            total = r.estado == 206 ? r.total : (uint64_t)n;
            if (!total || total > TAEK_VIDEO_MAX) { listo(); cp(estado, "Video demasiado grande para la memoria (max. 128 MiB a 360p).", sizeof(estado)); return 0; }
            g_prog_total = total;
        }
        hecho += (uint64_t)n;
        if (r.estado == 200 || hecho >= total || !n) break;
    }
    listo();
    if (hecho != total || hecho < 8 || g_mp4[4] != 'f' || g_mp4[5] != 't' || g_mp4[6] != 'y' || g_mp4[7] != 'p') {
        cp(estado, "El archivo no llego completo o no es MP4.", sizeof(estado)); yt_log("mp4 incompleto", v->id);
        return 0;
    }
    return entregar_y_salir(v, g_mp4, hecho);
}

/* ------------------------------------------------------------------ ver, descargar */
/* 1 = la app sale (360p por SYS_VIDEO); 0 = sigue en la lista */
static int ver(int i) {
    if (i < 0 || i >= nres) return 0;
    struct video *v = &res[i];
    yt_log("ver", v->id);
    struct video *h = hist_buscar(v->id);
    uint32_t pos = h && h->pos_ms > 10000 && (!h->dur_ms || h->pos_ms + 15000 < h->dur_ms) ? h->pos_ms : 0;
    struct fuente R;
    if (!g_flujo_ok) return ver_360(v);
    if (buffers()) { cp(estado, "Sin memoria para el reproductor.", sizeof(estado)); return 0; }
    if (pest == P_DESC) {
        if (preparar_descarga(v, &R)) { cp(estado, "La descarga esta incompleta o danada.", sizeof(estado)); return 0; }
    } else if (g_calidad <= 360 || preparar_hls(v, &R)) {
        return ver_360(v);
    }
    reproducir(&R, pos);
    fuente_liberar(&R);
    return 0;
}
/* H202: un short en su marco vertical. Arriba/abajo pasan al anterior o al siguiente; al terminar, el siguiente.
 * Si uno no trae HLS se salta (en la direccion en que iba). */
static void ver_short(int i) {
    if (!nres) return;
    if (!g_flujo_ok) { cp(estado, "Shorts necesita SYS_FLUJO (nucleo H187 o posterior).", sizeof(estado)); return; }
    int dir = 1, fallos = 0;
    while (fallos < nres) {
        i = (i % nres + nres) % nres;
        if (sel != i) { g_sh_dir = dir; g_sh_t0 = taek_ms(); g_sh_var = (int)(azar() & 1); }
        sel = i;
        struct video *v = &res[i];
        yt_log("short", v->id);
        if (buffers()) { cp(estado, "Sin memoria para el reproductor.", sizeof(estado)); return; }
        struct fuente R;
        if (preparar_hls(v, &R)) { yt_log("short", "sin HLS: el que sigue"); fallos++; i += dir; continue; }
        fallos = 0;
        g_modo_shorts = 1;
        reproducir(&R, 0);
        g_modo_shorts = 0;
        int c = g_rep.cambio;
        g_rep.shorts = 0;
        fuente_liberar(&R);
        if (!c) return;
        dir = c; i += c;
    }
    cp(estado, "Ningun short de la lista trae HLS.", sizeof(estado));
}
static void descargar(struct video *v) {
    struct fuente R;
    if (buffers()) { cp(estado, "Sin memoria.", sizeof(estado)); return; }
    if (preparar_hls(v, &R)) { cp(estado, "Este video no se puede descargar (sin HLS).", sizeof(estado)); return; }
    elegir_disco();
    char carpeta[200] = "/youtube/descargas/"; add(carpeta, v->id, sizeof(carpeta));
    long e = ar_carpeta("/youtube");
    if (!e) e = ar_carpeta("/youtube/descargas");
    if (!e) e = ar_carpeta(carpeta);
    char m[200];
    if (e) { estado[0] = 0; add(estado, "No se pudo crear la carpeta en el USB (codigo -", sizeof(estado)); addn(estado, (uint64_t)-e, sizeof(estado)); add(estado, ").", sizeof(estado)); fuente_liberar(&R); return; }
    long t0 = taek_ms();
    uint64_t total = 0;
    int ok = 1, eh = 0;
    for (int k = 0; k < R.nvs && ok; k++) {
        for (int audio = 0; audio < 2 && ok; audio++) {
            if (audio && k >= R.nas) break;
            int64_t n = bajar_url(audio ? R.as[k] : R.vs[k], audio ? g_raw_a : g_raw_v, audio ? FL_RAW_A : FL_RAW_V, &eh);
            if (n <= 0) { ok = 0; cp(estado, "Se corto la descarga.", sizeof(estado)); break; }
            char r[260]; R.carpeta[0] = 0; add(R.carpeta, carpeta, sizeof(R.carpeta)); ruta_seg(r, sizeof(r), &R, k, audio);
            long w = ar_escribir(r, audio ? g_raw_a : g_raw_v, (uint64_t)n);
            if (w < 0) { ok = 0; estado[0] = 0; add(estado, "No se pudo escribir en el USB (codigo -", sizeof(estado)); addn(estado, (uint64_t)-w, sizeof(estado)); add(estado, ").", sizeof(estado)); break; }
            total += (uint64_t)n;
        }
        long t = taek_ms();
        m[0] = 0; add(m, "Descargando ", sizeof(m)); addn(m, (uint64_t)R.lado, sizeof(m)); add(m, "p: ", sizeof(m)); addn(m, (uint64_t)(k + 1), sizeof(m));
        add(m, "/", sizeof(m)); addn(m, (uint64_t)R.nvs, sizeof(m)); add(m, " segmentos, ", sizeof(m)); addn(m, total >> 20, sizeof(m)); add(m, " MiB", sizeof(m));
        if (t > t0) { add(m, ", ", sizeof(m)); addn(m, total * 8 / (uint64_t)(t - t0) / 1000, sizeof(m)); add(m, " Mbit/s", sizeof(m)); }
        add(m, "  (Esc cancela)", sizeof(m));
        ocupado(m, (k + 1) * 100 / R.nvs);
        if (evento_esc()) { ok = 0; cp(estado, "Descarga cancelada.", sizeof(estado)); }
    }
    if (ok) {
        static char info[64 * 1024];
        info[0] = 0;
        sin_tabs(v->titulo); sin_tabs(v->canal);
        add(info, "\nid=", sizeof(info)); add(info, v->id, sizeof(info));
        add(info, "\ntitulo=", sizeof(info)); add(info, v->titulo, sizeof(info));
        add(info, "\ncanal=", sizeof(info)); add(info, v->canal, sizeof(info));
        add(info, "\ndur=", sizeof(info)); add(info, v->dur, sizeof(info));
        add(info, "\nlado=", sizeof(info)); addn(info, (uint64_t)R.lado, sizeof(info));
        add(info, "\nmib=", sizeof(info)); addn(info, total >> 20, sizeof(info));
        add(info, "\nnv=", sizeof(info)); addn(info, (uint64_t)R.nvs, sizeof(info));
        add(info, "\nna=", sizeof(info)); addn(info, (uint64_t)(R.nas < R.nvs ? R.nas : R.nvs), sizeof(info));
        add(info, "\nini=", sizeof(info));
        for (int k = 0; k <= R.nvs; k++) { if (k) add(info, ",", sizeof(info)); addn(info, R.ini[k], sizeof(info)); }
        add(info, "\n", sizeof(info));
        char r[220]; cp(r, carpeta, sizeof(r)); add(r, "/info.txt", sizeof(r));
        long w = ar_escribir(r, info, (uint64_t)lg(info));
        if (w < 0) { estado[0] = 0; add(estado, "No se pudo escribir info.txt (codigo -", sizeof(estado)); addn(estado, (uint64_t)-w, sizeof(estado)); add(estado, ").", sizeof(estado)); }
        else {
            estado[0] = 0; add(estado, "Descargado: ", sizeof(estado)); addn(estado, total >> 20, sizeof(estado)); add(estado, " MiB en ", sizeof(estado));
            addn(estado, (uint64_t)(taek_ms() - t0) / 1000, sizeof(estado)); add(estado, " s. Esta en Descargas.", sizeof(estado));
        }
    }
    listo();
    con_cerrar(&c_vid);
    yt_log("descarga", estado);
    fuente_liberar(&R);
}
static void borrar_descarga(struct video *v) {
    struct fuente R;
    char r[260];
    if (!preparar_descarga(v, &R)) {
        for (int k = 0; k < R.nvs; k++) { ruta_seg(r, sizeof(r), &R, k, 0); ar_borrar(r); if (k < R.nas) { ruta_seg(r, sizeof(r), &R, k, 1); ar_borrar(r); } }
        fuente_liberar(&R);
    }
    cp(r, "/youtube/descargas/", sizeof(r)); add(r, v->id, sizeof(r)); add(r, "/info.txt", sizeof(r));
    long e = ar_borrar(r);
    estado[0] = 0; add(estado, e < 0 ? "No se pudo borrar la descarga." : "Descarga borrada.", sizeof(estado));
}

/* ------------------------------------------------------------------ programa */
static void asegurar_visible(void) {
    if (pest == P_SUBS) {             /* paso 4: los nodos de canal tienen su propio desplazamiento */
        int fv = canal_filas(), f = can_sel / CAN_COLS;
        if (f < can_desde) can_desde = f;
        if (f >= can_desde + fv) can_desde = f - fv + 1;
        return;
    }
    if (pest == P_PERFIL || pest == P_SHORTS) { desde_fila = 0; return; }
    int filas_vis = filas_visibles();
    int f = sel / COLS;
    if (f < desde_fila) desde_fila = f;
    if (f >= desde_fila + filas_vis) desde_fila = f - filas_vis + 1;
}
static void ir_a(int p) { pest = p; cargar_pestana(); rev_empezar(); }
/* Paso 3: cambio de seccion con una transicion de la bolsa (confirmMenu del prototipo) */
static void ir_a_medio(int p) { ir_a(p); }
static void ir_a_con_transicion(int p, int ox, int oy) {
    tr_correr(tr_siguiente(), PEST_QM[p], PEST_SUB[p], PEST_RUTA[p], ox, oy, ir_a_medio, p);
}
/* Paso 4: Intro en un nodo de canal busca ese canal en YouTube (con transicion hacia BUSCAR) */
static void buscar_canal_medio(int i) {
    cp(consulta, g_can[i].nombre, sizeof(consulta));
    pest = P_BUSCAR; g_menu = P_BUSCAR;
    estado[0] = 0;
    buscar();
    rev_empezar();
}
static void buscar_canal(int i) {
    if (i < 0 || i >= ncan) return;
    char sub[120] = "CANAL // "; add(sub, g_can[i].nombre, sizeof(sub));
    tr_correr(tr_siguiente(), "BUSCAR", sub, "/qm2/buscar", -1, -1, buscar_canal_medio, i);
    g_zona = Z_VISTA;
}
static void arranque(int p) {
    elegir_disco();
    hist_cargar();
    cuenta_cargar();
    { char m[120] = "historial "; addn(m, (uint64_t)nhist, sizeof(m)); add(m, con_cuenta() ? ", con cuenta" : ", sin cuenta", sizeof(m)); add(m, g_flujo_ok ? ", SYS_FLUJO" : ", sin SYS_FLUJO", sizeof(m)); yt_log("estado", m); }
    ir_a(p);
}

long principal(uint64_t a0, uint64_t a1) {
    (void)a0; (void)a1;
    struct taek_pantalla pan;
    if (taek_pantalla(&pan) || !pan.ancho || !pan.alto) return 2;
    AN = (int)pan.ancho; AL = (int)pan.alto;
    sup = (struct taek_superficie){ (uint32_t)AN, (uint32_t)AL, 0, 0, 0, 0, 0 };
    if (taek_superficie(&sup)) return 3;
    L = (struct lienzo){ (uint32_t *)(uintptr_t)sup.pixeles, AN, AL };
    c_api.id = c_img.id = c_vid.id = c_aud.id = c_web.id = -1;
    g_json = malloc(JSON_CAP);
    if (!g_json) return 4;
    /* QM-2 paso 2: menu lateral de MENU_AN y la cuadricula .feed-grid a su derecha */
    ESP = S(18); X0 = MENU_AN + S(22); Y0 = VISTA_Y + S(54);
    COLS = (AN - S(22) - X0 + ESP) / (S(300) + ESP);
    if (COLS < 1) COLS = 1;
    CARTA_AN = (AN - S(22) - X0 - (COLS - 1) * ESP) / COLS;
    MINI_AN = CARTA_AN; MINI_AL = MINI_AN * 9 / 16;
    CARTA_AL = MINI_AL + S(92);
    /* paso 4: .channel-grid, repeat(auto-fill, minmax(230px, 1fr)) con 14 px de hueco */
    CAN_COLS = (AN - S(22) - X0 + S(14)) / (S(230) + S(14));
    if (CAN_COLS < 1) CAN_COLS = 1;
    CAN_AN = (AN - S(22) - X0 - (CAN_COLS - 1) * S(14)) / CAN_COLS;
    CAN_AL = S(216);
    g_cat_consulta = CAT_CONSULTA;
    g_azar ^= (uint32_t)taek_ms() * 2654435761u;
    { struct taek_flujo q; memset(&q, 0, sizeof(q)); q.op = TAEK_FLUJO_ESTADO; g_flujo_ok = taek_flujo(&q) >= 0; }

    char arg[160] = "";
    long na = taek_argumento(arg, sizeof(arg) - 1);
    if (na > 0) arg[na] = 0; else arg[0] = 0;
    if (arg[0] == 'q' && arg[1] == ':') cp(consulta, arg + 2, sizeof(consulta));
    else if (arg[0]) cp(consulta, arg, sizeof(consulta));
    yt_log("ABIERTO", consulta);
    /* Con una busqueda (al volver de un video) se entra directo a la vista; si no, al menu como en el prototipo */
    g_menu = consulta[0] ? P_BUSCAR : P_INICIO;
    g_zona = consulta[0] ? Z_VISTA : Z_MENU;
    /* Paso 3: al arrancar, la tarjeta EVA mientras carga; despues entra el menu. Al volver de un video, sin tarjeta. */
    if (!consulta[0]) {
        char sub[80] = "QUICK MENU 2 // RING 3 // 8086:", hx[8];
        if (taek_info(&g_info, 0) == 0 && g_info.gpu_id) { hexn(hx, g_info.gpu_id, 4); add(sub, hx, sizeof(sub)); } else add(sub, "----", sizeof(sub));
        tr_correr(TR_EVA, "NEKOTUBE", sub, "/qm2/arranque", -1, -1, arranque, g_menu);
        menu_entrar();
    } else {
        g_ment_t0 = taek_ms() - 100000;
        mostrar();
        arranque(g_menu);
    }
    int mini_sig = 0;

    struct taek_evento e;
    long ultimo_clic = 0;
    uint32_t botones_antes = 0;
    int raton_x = -1, raton_y = -1;
    for (;;) {
        long t = taek_ms();
        int salir = 0, recargar = 0;
        for (int k = 0; k < 16; k++) {
            long hay = taek_evento(&e);
            if (hay < 0) return 1;
            int pulso = (e.botones & 1) && !(botones_antes & 1);
            botones_antes = e.botones;
            int movio = e.x != raton_x || e.y != raton_y;
            raton_x = e.x; raton_y = e.y;
            int en_menu = e.x < MENU_AN && e.y > HUD_AL && e.y < AL - S(36), sobre_mi = -1;
            if (en_menu) for (int i = 0; i < NPEST; i++) if (e.y >= g_mi_y[i] && e.y < g_mi_y[i] + g_mi_h[i]) sobre_mi = i;
            /* mouseenter del prototipo: el cursor sigue al raton por el menu */
            if (hay && movio && sobre_mi >= 0 && g_zona == Z_MENU) g_menu = sobre_mi;
            if (pulso) {
                if (en_menu) {
                    if (sobre_mi >= 0) {
                        g_menu = sobre_mi;
                        if (g_menu != pest) { ir_a_con_transicion(g_menu, e.x, e.y); mini_sig = 0; }
                        g_zona = Z_VISTA;
                    }
                } else if (pest == P_INICIO && e.y >= VISTA_Y && e.y < VISTA_Y + S(28)) {
                    for (int i = 0; i < NCAT; i++)
                        if (e.x >= g_cat_x[i] && e.x < g_cat_x[i] + g_cat_w[i]) { g_cat = i; ir_a(P_INICIO); mini_sig = 0; }
                    g_zona = Z_VISTA;
                } else if (pest == P_SUBS && e.y >= Y0 - S(8) && e.y < GRID_FIN) {
                    g_zona = Z_VISTA;
                    for (int i = can_desde * CAN_COLS; i < ncan; i++) {
                        int x, y; canal_pos(i, &x, &y);
                        if (e.x < x || e.x >= x + CAN_AN || e.y < y || e.y >= y + CAN_AL) continue;
                        if (i == can_sel && t - ultimo_clic > 250) { buscar_canal(i); mini_sig = 0; }
                        else can_sel = i;
                        ultimo_clic = t;
                        break;
                    }
                } else if (pest == P_SHORTS && e.x >= MENU_AN && e.y > HUD_AL && e.y < AL - S(36)) {
                    g_zona = Z_VISTA;
                    int fx, fy, fw, fh, ix, dx, ld, mx, mw;
                    sh_geometria(&fx, &fy, &fw, &fh, &ix, &dx, &ld, &mx, &mw);
                    if (nres && e.x >= fx && e.x < fx + fw) { ver_short(sel); mini_sig = 0; }
                    else if (nres && e.x >= ix && e.x < ix + ld) { g_sh_dir = -1; sel = (sel + nres - 1) % nres; g_sh_t0 = t; }
                    else if (nres && e.x >= dx && e.x < dx + ld) { g_sh_dir = 1; sel = (sel + 1) % nres; g_sh_t0 = t; }
                } else if (pest == P_PERFIL && e.x >= g_reg_x0 && e.x < g_reg_x1 && e.y >= g_reg_y0) {
                    g_zona = Z_VISTA;
                    int i = (e.y - g_reg_y0) / S(26);
                    if (i >= 0 && i < nres) {
                        if (i == sel && t - ultimo_clic > 250) { if (ver(i)) return 0; recargar = 1; }
                        sel = i; ultimo_clic = t;
                    }
                } else if (pest != P_SUBS && pest != P_PERFIL && e.y >= Y0 - S(8) && e.y < GRID_FIN) {
                    g_zona = Z_VISTA;
                    if (pest != P_CUENTA)
                        for (int i = desde_fila * COLS; i < nres; i++) {
                            int x, y; carta_pos(i, &x, &y);
                            if (e.x < x || e.x >= x + CARTA_AN || e.y < y || e.y >= y + CARTA_AL) continue;
                            if (i == sel && t - ultimo_clic > 250) { if (ver(i)) return 0; }
                            sel = i; ultimo_clic = t;
                            break;
                        }
                } else if (e.x >= MENU_AN) g_zona = Z_VISTA;
            }
            if (!hay) break;
            uint32_t tecla = ap_filtrar(e.tecla);
            if (!tecla) continue;
            if (tecla == 9) { g_menu = (pest + 1) % NPEST; ir_a_con_transicion(g_menu, -1, -1); mini_sig = 0; continue; }
            /* Zona del menu: arriba/abajo mueven el cursor; Intro o derecha entran a la vista */
            if (g_zona == Z_MENU) {
                if (tecla == TAEK_TECLA_ABAJO) g_menu = (g_menu + 1) % NPEST;
                else if (tecla == TAEK_TECLA_ARRIBA) g_menu = (g_menu + NPEST - 1) % NPEST;
                else if (tecla == '\n' || tecla == '\r' || tecla == TAEK_TECLA_DERECHA || tecla == ' ') {
                    if (g_menu != pest) { ir_a_con_transicion(g_menu, -1, -1); mini_sig = 0; }
                    g_zona = Z_VISTA;
                } else if (tecla >= 33 && tecla < 127) {           /* escribir sigue llevando a Buscar */
                    if (pest != P_BUSCAR) { pest = P_BUSCAR; consulta[0] = 0; limpiar_res(); seccion[0] = 0; }
                    g_menu = P_BUSCAR; g_zona = Z_VISTA;
                    int n = lg(consulta); if (n + 1 < (int)sizeof(consulta)) { consulta[n] = (char)tecla; consulta[n + 1] = 0; }
                }
                continue;
            }
            if (pest == P_SUBS && (tecla == '\n' || tecla == '\r' || tecla == 8 || tecla == 127 || (tecla >= TAEK_TECLA_ARRIBA && tecla <= TAEK_TECLA_ABAJO) || tecla == TAEK_TECLA_IZQUIERDA || tecla == TAEK_TECLA_DERECHA)) {
                if (tecla == '\n' || tecla == '\r') { if (ncan) { buscar_canal(can_sel); mini_sig = 0; } }
                else if (tecla == 8 || tecla == 127) g_zona = Z_MENU;
                else if (tecla == TAEK_TECLA_DERECHA) { if (can_sel + 1 < ncan) can_sel++; }
                else if (tecla == TAEK_TECLA_IZQUIERDA) { if (!ncan || can_sel % CAN_COLS == 0) g_zona = Z_MENU; else can_sel--; }
                else if (tecla == TAEK_TECLA_ABAJO) { if (can_sel + CAN_COLS < ncan) can_sel += CAN_COLS; else if (ncan) can_sel = ncan - 1; }
                else if (tecla == TAEK_TECLA_ARRIBA) { if (can_sel - CAN_COLS >= 0) can_sel -= CAN_COLS; }
                continue;
            }
            if (pest == P_SHORTS && (tecla == TAEK_TECLA_ABAJO || tecla == TAEK_TECLA_ARRIBA || tecla == TAEK_TECLA_IZQUIERDA ||
                                     tecla == TAEK_TECLA_DERECHA || tecla == '\n' || tecla == '\r' || tecla == 8 || tecla == 127)) {
                if ((tecla == TAEK_TECLA_ABAJO || tecla == TAEK_TECLA_ARRIBA) && nres) {   /* showShort: cambia con su entrada */
                    g_sh_dir = tecla == TAEK_TECLA_ABAJO ? 1 : -1;
                    sel = ((sel + g_sh_dir) % nres + nres) % nres;
                    g_sh_t0 = taek_ms(); g_sh_var = (int)(azar() & 1);
                } else if (tecla == '\n' || tecla == '\r') { if (nres) { ver_short(sel); mini_sig = 0; } }
                else if (tecla == TAEK_TECLA_IZQUIERDA || tecla == 8 || tecla == 127) g_zona = Z_MENU;
                continue;
            }
            if (pest == P_PERFIL && (tecla == TAEK_TECLA_ABAJO || tecla == TAEK_TECLA_ARRIBA || tecla == TAEK_TECLA_DERECHA || tecla == TAEK_TECLA_IZQUIERDA)) {
                if (tecla == TAEK_TECLA_ABAJO) { if (sel + 1 < nres) sel++; }
                else if (tecla == TAEK_TECLA_ARRIBA) { if (sel > 0) sel--; }
                else if (tecla == TAEK_TECLA_IZQUIERDA) g_zona = Z_MENU;
                continue;
            }
            if (pest == P_CUENTA) {
                if (tecla == '\n' || tecla == '\r') { if (con_cuenta()) cerrar_sesion(); else iniciar_sesion(); }
                else if (tecla == TAEK_TECLA_DERECHA || tecla == TAEK_TECLA_IZQUIERDA) {
                    static const int C[4] = { 360, 480, 720, 1080 };
                    int k2 = 0; while (k2 < 3 && C[k2] != g_calidad) k2++;
                    k2 = (k2 + (tecla == TAEK_TECLA_DERECHA ? 1 : 3)) % 4; g_calidad = C[k2];
                }
                else if (tecla == 8 || tecla == 127) g_zona = Z_MENU;
                continue;
            }
            if (pest == P_INICIO && (tecla == 'q' || tecla == 'Q' || tecla == 'e' || tecla == 'E')) {
                g_cat = (g_cat + (tecla == 'e' || tecla == 'E' ? 1 : NCAT - 1)) % NCAT;
                ir_a(P_INICIO); mini_sig = 0;
                continue;
            }
            if (tecla == '\n' || tecla == '\r') {
                if (pest == P_BUSCAR && consulta[0] && (!igual(consulta, buscada) || !nres)) { buscar(); rev_empezar(); mini_sig = 0; }
                else if (nres) { if (ver(sel)) return 0; recargar = pest == P_HIST || pest == P_INICIO || pest == P_PERFIL; }
            } else if (tecla == 8 || tecla == 127) {
                if (pest != P_BUSCAR) g_zona = Z_MENU;          /* como en el prototipo: Retroceso vuelve al menu */
                else { int n = lg(consulta); if (n) { n--; while (n && ((unsigned char)consulta[n] & 0xC0) == 0x80) n--; consulta[n] = 0; } }
            } else if (tecla == TAEK_TECLA_DERECHA) { if (sel + 1 < nres) sel++; }
            else if (tecla == TAEK_TECLA_IZQUIERDA) { if (!nres || sel % COLS == 0) g_zona = Z_MENU; else sel--; }
            else if (tecla == TAEK_TECLA_ABAJO) { if (sel + COLS < nres) sel += COLS; else if (nres) sel = nres - 1; }
            else if (tecla == TAEK_TECLA_ARRIBA) { if (sel - COLS >= 0) sel -= COLS; }
            else if (tecla == TAEK_TECLA_F2) { if (nres && pest != P_DESC && pest != P_PERFIL) { descargar(&res[sel]); } }
            else if (tecla == TAEK_TECLA_SUPRIMIR) {
                if (nres && pest == P_HIST) { hist_quitar(res[sel].id); recargar = 1; }
                else if (nres && pest == P_DESC) { borrar_descarga(&res[sel]); recargar = 1; }
            }
            else if (tecla >= 32 && tecla < 127) {
                if (pest != P_BUSCAR) { pest = P_BUSCAR; consulta[0] = 0; limpiar_res(); seccion[0] = 0; mini_sig = 0; }
                g_menu = P_BUSCAR;
                int n = lg(consulta); if (n + 1 < (int)sizeof(consulta)) { consulta[n] = (char)tecla; consulta[n + 1] = 0; }
            }
        }
        if (recargar) {
            char guardado[200]; cp(guardado, estado, sizeof(guardado));
            int s = sel; cargar_pestana(); sel = s < nres ? s : (nres ? nres - 1 : 0); mini_sig = 0;
            cp(estado, guardado, sizeof(estado));
        }
        /* Esc: de la vista vuelve al menu; en el menu, sale */
        if (ap_esc_suelto()) { if (g_zona == Z_VISTA) { g_zona = Z_MENU; g_menu = pest; } else salir = 1; }
        if (salir) break;
        asegurar_visible();
        /* Una miniatura por cuadro, empezando por las visibles (Suscripciones y Perfil no las muestran) */
        if (mini_sig < nres && pest != P_SUBS && pest != P_PERFIL) {
            int i = desde_fila * COLS + mini_sig;
            if (i >= nres) i = mini_sig;
            struct video *v = &res[i < nres ? i : 0];
            if (!v->mini && !v->mini_estado) cargar_mini(v);
            mini_sig++;
            if (mini_sig == nres) con_cerrar(&c_img);
        }
        mostrar();
        long g = taek_ms() - t;
        if (g < 33) taek_esperar(33 - g);
    }
    con_cerrar(&c_api); con_cerrar(&c_img); con_cerrar(&c_vid); con_cerrar(&c_aud); con_cerrar(&c_web);
    yt_log("CERRADO", "");
    return 0;
}
