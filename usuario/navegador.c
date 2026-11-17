/* Navegador sencillo de NekoSaka para TAEK OS (H143). Programa de Anillo 3 que no va dentro del nucleo: se copia al
 * disco y se lanza con 'ejecutar /navegador.elf', sin reiniciar.
 *
 * Reparto del trabajo:
 *   - El nucleo descarga (SYS_WEB): DNS, TCP, TLS 1.3, HTTP, redirecciones y trozos. El programa no toca la red.
 *   - El programa entiende el HTML (un subconjunto: titulos, parrafos, listas, enlaces, preformateado), lo reparte en
 *     lineas y lo dibuja en su superficie. Scripts, estilos e imagenes se ignoran.
 *
 * Teclas: escribir cambia la direccion; Intro la abre (con palabras sueltas busca en DuckDuckGo Lite); flechas
 * arriba/abajo desplazan; Ctrl+D / Ctrl+U media pagina; Ctrl+B o clic derecho, atras; Esc sale a la terminal. */
#pragma clang diagnostic ignored "-Wunused-function"
#include "ui.h"
#include "memoria.h"

TAEK_PROGRAMA

/* ------------------------------------------------------------------ colores y medidas */

#define FONDO    0xFF0B0E14u
#define BARRA    0xFF141924u
#define CAMPO    0xFF1E2533u
#define ROJO     0xFF003Cu
#define NEON     0xFCEE0Au
#define CIAN     0x5BE0FFu
#define TEXTO    0xD8DCE6u
#define TENUE    0x8A93A6u
#define BLANCO   0xFFFFFFu
#define CODIGO   0x9BE8A0u

static int AN, AL;
static int S(int v) { return v * AL / 1080; }

static struct taek_superficie sup;
static struct lienzo L;
static int BARRA_AL, VISTA_Y, VISTA_AL, COL_X, COL_AN;
static int TAM[4];   /* Cuerpo, h3, h2, h1 */

/* ------------------------------------------------------------------ cadenas */

static int largo_c(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void copiar_c(char *d, const char *o, int cap) { int i = 0; while (o[i] && i + 1 < cap) { d[i] = o[i]; i++; } d[i] = 0; }
static int empieza(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static char minus(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int empieza_sin_caso(const char *s, const char *e, const char *p) {
    while (*p) { if (s >= e || minus(*s) != *p) return 0; s++; p++; }
    return 1;
}

/* ------------------------------------------------------------------ documento */

/* H144: los bufers grandes se piden con malloc al arrancar, no van en la imagen (que ademas tiene la fuente, 1,6 MB) */
#define MAX_TEXTO    (2 * 1024 * 1024)
#define MAX_PAL      120000
#define MAX_ENLACES  4096
#define MAX_HREF     (512 * 1024)
#define CUERPO_MAX   (1024 * 1024)   /* Lo que descarga el nucleo como mucho */

struct pal { uint32_t off; int32_t x, y; int16_t ancho, enlace; uint8_t tam, color; uint8_t subraya, relleno; };
static char *texto_doc;
static uint32_t texto_n;
static struct pal *pals;
static int npal;
static uint32_t *href_off;
static char *hrefs;
static uint32_t hrefs_n;
static int nenlaces;
static char titulo[160];
static int doc_alto;

/* Estado del reparto en lineas */
static int cur_x, cur_y, linea_alto, linea_ini, sangria, pend_espacio, tam_act, color_act, enlace_act, en_pre;

static const uint32_t PALETA[] = { TEXTO, BLANCO, CIAN, TENUE, CODIGO, NEON };
enum { C_TEXTO, C_FUERTE, C_ENLACE, C_TENUE, C_CODIGO, C_TITULO };

static int alto_de(int tam) { return fuente_tam(TAM[tam])->alto * 13 / 10; }

static void doc_vaciar(void) {
    texto_n = 0; npal = 0; hrefs_n = 0; nenlaces = 0; titulo[0] = 0;
    cur_x = 0; cur_y = S(24); linea_alto = alto_de(0); linea_ini = 0; sangria = 0; pend_espacio = 0;
    tam_act = 0; color_act = C_TEXTO; enlace_act = -1; en_pre = 0;
}

static void cerrar_linea(void) {
    /* Las palabras de la linea guardan su arriba; todas comparten el alto de la mas grande */
    cur_y += linea_alto;
    cur_x = 0;
    linea_alto = alto_de(tam_act);
    linea_ini = npal;
}
static void salto(void) { if (cur_x > 0) cerrar_linea(); pend_espacio = 0; }
static void espacio_vertical(int px) { salto(); if (npal && cur_y > S(24)) cur_y += px; }

static void poner_palabra(const char *s, int n) {
    if (!n || npal >= MAX_PAL || texto_n + (uint32_t)n + 1 > MAX_TEXTO) return;
    char *d = texto_doc + texto_n;
    for (int i = 0; i < n; i++) d[i] = s[i];
    d[n] = 0;
    int px = TAM[tam_act];
    int w = texto_ancho(px, d), esp = pend_espacio && cur_x > 0 ? texto_ancho(px, " ") : 0;
    int margen = sangria;
    if (cur_x > 0 && margen + cur_x + esp + w > COL_AN) { cerrar_linea(); esp = 0; }
    if (alto_de(tam_act) > linea_alto) linea_alto = alto_de(tam_act);
    struct pal *p = &pals[npal++];
    p->off = texto_n; p->x = margen + cur_x + esp; p->y = cur_y; p->ancho = (int16_t)(w > 32767 ? 32767 : w);
    p->enlace = (int16_t)enlace_act; p->tam = (uint8_t)tam_act; p->color = (uint8_t)(enlace_act >= 0 ? C_ENLACE : color_act);
    p->subraya = enlace_act >= 0;
    texto_n += (uint32_t)n + 1;
    cur_x += esp + w;
    pend_espacio = 0;
}

/* Palabra en curso, ya en UTF-8 de 1 o 2 bytes (lo que tiene la fuente) */
static char pal_b[200];
static int pal_n;
static void pal_cerrar(void) { if (pal_n) { poner_palabra(pal_b, pal_n); pal_n = 0; } }

/* Lo que la fuente no tiene se acerca a ASCII; lo demas se tira */
static uint32_t aproximar(uint32_t cp) {
    if (cp < 0x80) return cp;
    if (fuente_glifo(fuente_tam(TAM[0]), cp)) return cp;
    switch (cp) {
    case 0xA0: return ' ';
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0xAB: case 0xBB: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: case 0x25CF: return 0xB7;
    case 0x2026: return '.';
    case 0xE0: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return 'a';
    case 0xC0: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return 'A';
    case 0xE8: case 0xEA: case 0xEB: return 'e';
    case 0xC8: case 0xCA: case 0xCB: return 'E';
    case 0xEC: case 0xEE: case 0xEF: return 'i';
    case 0xF2: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return 'o';
    case 0xF9: case 0xFB: return 'u';
    case 0xE7: return 'c';
    case 0xC7: return 'C';
    default: return 0;
    }
}

static void pal_cp(uint32_t cp) {
    cp = aproximar(cp);
    if (!cp) return;
    if (cp == ' ') { pal_cerrar(); pend_espacio = 1; return; }
    if (pal_n + 2 >= (int)sizeof(pal_b)) pal_cerrar();
    if (cp < 0x80) pal_b[pal_n++] = (char)cp;
    else { pal_b[pal_n++] = (char)(0xC0 | (cp >> 6)); pal_b[pal_n++] = (char)(0x80 | (cp & 0x3F)); }
}

static int agregar_enlace(const char *h, int n) {
    if (nenlaces >= MAX_ENLACES || hrefs_n + (uint32_t)n + 1 > MAX_HREF) return -1;
    href_off[nenlaces] = hrefs_n;
    for (int i = 0; i < n; i++) hrefs[hrefs_n + (uint32_t)i] = h[i];
    hrefs[hrefs_n + (uint32_t)n] = 0;
    hrefs_n += (uint32_t)n + 1;
    return nenlaces++;
}

/* ------------------------------------------------------------------ HTML */

/* Un caracter del texto: UTF-8 si es valido, si no Latin-1 (paginas viejas en ISO-8859-1) */
static uint32_t leer_cp(const char **pp, const char *fin) {
    const unsigned char *p = (const unsigned char *)*pp;
    uint32_t c = p[0];
    if (c < 0x80) { *pp += 1; return c; }
    int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n && (const char *)p + n <= fin) {
        uint32_t v = c & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
        int ok = 1;
        for (int i = 1; i < n; i++) { if ((p[i] & 0xC0) != 0x80) { ok = 0; break; } v = v << 6 | (p[i] & 0x3F); }
        if (ok) { *pp += n; return v; }
    }
    *pp += 1;
    return c;
}

struct entidad { const char *n; uint16_t cp; };
static const struct entidad ENT[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9}, {"reg", 0xAE},
    {"laquo", 0xAB}, {"raquo", 0xBB}, {"middot", 0xB7}, {"bull", 0x2022}, {"hellip", 0x2026}, {"ndash", 0x2013},
    {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"iexcl", 0xA1},
    {"iquest", 0xBF}, {"aacute", 0xE1}, {"eacute", 0xE9}, {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA},
    {"Aacute", 0xC1}, {"Eacute", 0xC9}, {"Iacute", 0xCD}, {"Oacute", 0xD3}, {"Uacute", 0xDA}, {"ntilde", 0xF1},
    {"Ntilde", 0xD1}, {"uuml", 0xFC}, {"Uuml", 0xDC}, {"deg", 0xB0}, {"euro", 'E'}, {"times", 'x'},
};

static uint32_t leer_entidad(const char **pp, const char *fin) {
    const char *p = *pp + 1;
    char nom[12]; int n = 0;
    if (p < fin && *p == '#') {
        p++;
        uint32_t v = 0; int hex = p < fin && (*p == 'x' || *p == 'X'), dig = 0;
        if (hex) p++;
        while (p < fin && dig < 8) {
            char c = *p;
            int d = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * (hex ? 16u : 10u) + (uint32_t)d; p++; dig++;
        }
        if (!dig) { *pp += 1; return '&'; }
        if (p < fin && *p == ';') p++;
        *pp = p;
        return v;
    }
    while (p < fin && n < 11 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) nom[n++] = *p++;
    nom[n] = 0;
    for (unsigned i = 0; i < sizeof(ENT) / sizeof(ENT[0]); i++) {
        const char *a = ENT[i].n, *b = nom;
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b) { if (p < fin && *p == ';') p++; *pp = p; return ENT[i].cp; }
    }
    *pp += 1;
    return '&';
}

static int es_nombre(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return !*a && !*b; }
static int en_lista(const char *n, const char *const *l) { for (; *l; l++) if (es_nombre(n, *l)) return 1; return 0; }

static const char *const BLOQUES[] = { "div", "section", "article", "header", "footer", "nav", "main", "aside", "form",
    "tr", "dl", "dt", "dd", "figure", "figcaption", "center", "address", "fieldset", "details", "summary", "caption", 0 };
static const char *const PARRAFOS[] = { "p", "ul", "ol", "blockquote", "table", 0 };
static const char *const OMITIR[] = { "script", "style", "noscript", "svg", "template", "iframe", "object", "select",
    "textarea", "math", "canvas", 0 };

/* Busca un atributo dentro de la etiqueta [a, b). Devuelve su largo y deja *v al principio del valor. */
static int atributo(const char *a, const char *b, const char *nombre, const char **v) {
    int ln = largo_c(nombre);
    for (const char *p = a; p + ln < b; p++) {
        if ((p == a || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') && empieza_sin_caso(p, b, nombre)) {
            const char *q = p + ln;
            while (q < b && (*q == ' ' || *q == '\t')) q++;
            if (q >= b || *q != '=') continue;
            q++;
            while (q < b && (*q == ' ' || *q == '\t')) q++;
            char cierre = 0;
            if (q < b && (*q == '"' || *q == '\'')) cierre = *q++;
            const char *ini = q;
            while (q < b && (cierre ? *q != cierre : (*q != ' ' && *q != '>' && *q != '\t' && *q != '\n'))) q++;
            *v = ini;
            return (int)(q - ini);
        }
    }
    return -1;
}

static int en_titulo, nivel_h;
static char titulo_b[160]; static int titulo_n;

static void etiqueta(const char *nom, int cierre, const char *ini, const char *fin) {
    if (es_nombre(nom, "title")) { en_titulo = !cierre; if (!cierre) titulo_n = 0; else { titulo_b[titulo_n] = 0; copiar_c(titulo, titulo_b, sizeof(titulo)); } return; }
    pal_cerrar();
    if (es_nombre(nom, "br")) { if (cur_x > 0) cerrar_linea(); else cur_y += linea_alto; pend_espacio = 0; return; }
    if (nom[0] == 'h' && nom[1] >= '1' && nom[1] <= '6' && !nom[2]) {
        espacio_vertical(S(cierre ? 6 : 14));
        if (cierre) { tam_act = 0; color_act = C_TEXTO; nivel_h = 0; }
        else { nivel_h = nom[1] - '0'; tam_act = nivel_h == 1 ? 3 : nivel_h == 2 ? 2 : 1; color_act = nivel_h == 1 ? C_TITULO : C_FUERTE; }
        linea_alto = alto_de(tam_act);
        return;
    }
    if (es_nombre(nom, "li")) {
        salto();
        if (!cierre) { int c = color_act; color_act = C_TENUE; pal_cp(0xB7); pal_cerrar(); color_act = c; pend_espacio = 1; }
        return;
    }
    if (es_nombre(nom, "ul") || es_nombre(nom, "ol") || es_nombre(nom, "blockquote")) {
        sangria += cierre ? -S(28) : S(28);
        if (sangria < 0) sangria = 0;
        if (sangria > COL_AN / 3) sangria = COL_AN / 3;
    }
    if (en_lista(nom, PARRAFOS)) { espacio_vertical(S(10)); return; }
    if (en_lista(nom, BLOQUES)) { salto(); return; }
    if (es_nombre(nom, "hr")) { espacio_vertical(S(10)); return; }
    if (es_nombre(nom, "pre")) { espacio_vertical(S(8)); en_pre = !cierre; color_act = cierre ? C_TEXTO : C_CODIGO; return; }
    if (es_nombre(nom, "td") || es_nombre(nom, "th")) { pend_espacio = 1; return; }
    if (es_nombre(nom, "b") || es_nombre(nom, "strong")) { if (!nivel_h) color_act = cierre ? C_TEXTO : C_FUERTE; return; }
    if (es_nombre(nom, "code") || es_nombre(nom, "tt") || es_nombre(nom, "kbd")) { if (!en_pre) color_act = cierre ? C_TEXTO : C_CODIGO; return; }
    if (es_nombre(nom, "a")) {
        if (cierre) { enlace_act = -1; return; }
        const char *v; int n = atributo(ini, fin, "href", &v);
        enlace_act = n > 0 && v[0] != '#' && !empieza_sin_caso(v, v + n, "javascript:") && !empieza_sin_caso(v, v + n, "mailto:") ? agregar_enlace(v, n) : -1;
        return;
    }
    if (es_nombre(nom, "img") && !cierre) {
        const char *v; int n = atributo(ini, fin, "alt", &v);
        if (n > 0) { int c = color_act; color_act = C_TENUE; pal_cp('['); for (const char *q = v; q < v + n;) pal_cp(leer_cp(&q, v + n)); pal_cp(']'); pal_cerrar(); color_act = c; pend_espacio = 1; }
    }
}

static void html(const char *p, const char *fin) {
    en_titulo = 0; nivel_h = 0; titulo_n = 0;
    while (p < fin) {
        char c = *p;
        if (c == '<') {
            if (empieza_sin_caso(p, fin, "<!--")) {
                const char *q = p + 4;
                while (q + 2 < fin && !(q[0] == '-' && q[1] == '-' && q[2] == '>')) q++;
                p = q + 3;
                continue;
            }
            const char *q = p + 1;
            if (q < fin && (*q == '!' || *q == '?')) {   /* <!doctype ...>, <![CDATA[...]>, <?xml ...?> */
                while (q < fin && *q != '>') q++;
                p = q < fin ? q + 1 : fin;
                continue;
            }
            int cierre = q < fin && *q == '/';
            if (cierre) q++;
            char nom[16]; int n = 0;
            while (q < fin && n < 15 && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9'))) nom[n++] = minus(*q++);
            nom[n] = 0;
            if (!n) { pal_cp('<'); p++; continue; }   /* Un '<' suelto en el texto */
            const char *ini = q;
            char comilla = 0;
            while (q < fin && (comilla || *q != '>')) { if (comilla ? *q == comilla : (*q == '"' || *q == '\'')) comilla = comilla ? 0 : *q; q++; }
            const char *fin_etq = q;
            p = q < fin ? q + 1 : fin;
            if (!cierre && en_lista(nom, OMITIR) && !(fin_etq > ini && fin_etq[-1] == '/')) {   /* <svg ... /> no tiene cierre */
                /* Hasta su cierre, sin mirar dentro */
                char busca[20] = "</"; int k = 2;
                for (int i = 0; nom[i] && k < 18; i++) busca[k++] = nom[i];
                busca[k] = 0;
                while (p < fin && !empieza_sin_caso(p, fin, busca)) p++;
                continue;
            }
            etiqueta(nom, cierre, ini, fin_etq);
            continue;
        }
        uint32_t cp;
        if (c == '&') cp = leer_entidad(&p, fin);
        else cp = leer_cp(&p, fin);
        if (en_titulo) {
            if (cp == '\n' || cp == '\r' || cp == '\t') cp = ' ';
            if (titulo_n + 3 < (int)sizeof(titulo_b) && !(cp == ' ' && (!titulo_n || titulo_b[titulo_n - 1] == ' '))) {
                cp = aproximar(cp);
                if (cp && cp < 0x80) titulo_b[titulo_n++] = (char)cp;
                else if (cp) { titulo_b[titulo_n++] = (char)(0xC0 | (cp >> 6)); titulo_b[titulo_n++] = (char)(0x80 | (cp & 0x3F)); }
            }
            continue;
        }
        if (en_pre && cp == '\n') { pal_cerrar(); if (cur_x > 0) cerrar_linea(); else cur_y += linea_alto; continue; }
        if (cp == ' ' || cp == '\n' || cp == '\r' || cp == '\t' || cp == '\f') { pal_cerrar(); pend_espacio = 1; continue; }
        pal_cp(cp);
    }
    pal_cerrar();
    salto();
    doc_alto = cur_y + S(60);
}

/* ------------------------------------------------------------------ direcciones */

#define URL_MAX TAEK_WEB_URL
static char url_actual[URL_MAX];
static char hist[16][URL_MAX];
static int hist_n;

/* Une href con la direccion de la pagina: absoluta, //host, /ruta, ?consulta o relativa (con ./ y ../). */
static int resolver(const char *base, const char *h, char *out, int cap) {
    if (empieza(h, "http://") || empieza(h, "https://")) { if (largo_c(h) >= cap) return -1; copiar_c(out, h, cap); return 0; }
    int esq = empieza(base, "https://") ? 8 : empieza(base, "http://") ? 7 : 0;
    if (!esq) return -1;
    const char *host_fin = base + esq;
    while (*host_fin && *host_fin != '/' && *host_fin != '?' && *host_fin != '#') host_fin++;
    char t[URL_MAX]; int n = 0;
    #define PON(c) do { if (n + 1 >= (int)sizeof(t)) return -1; t[n++] = (c); } while (0)
    if (h[0] == '/' && h[1] == '/') {
        for (const char *p = base; p < base + esq - 2; p++) PON(*p);
        for (const char *p = h; *p; p++) PON(*p);
    } else {
        for (const char *p = base; p < host_fin; p++) PON(*p);
        int raiz = n;
        if (h[0] == '/') {
            for (const char *p = h; *p; p++) PON(*p);
        } else if (h[0] == '?') {
            const char *p = host_fin;
            while (*p && *p != '?' && *p != '#') PON(*p++);
            if (n == raiz) PON('/');
            for (p = h; *p; p++) PON(*p);
        } else {
            /* Directorio de la base, sin consulta */
            const char *p = host_fin, *ultimo = host_fin;
            while (*p && *p != '?' && *p != '#') { if (*p == '/') ultimo = p; p++; }
            for (p = host_fin; p < ultimo; p++) PON(*p);
            PON('/');
            const char *q = h;
            for (;;) {
                if (q[0] == '.' && q[1] == '/') { q += 2; continue; }
                if (q[0] == '.' && q[1] == '.' && (q[2] == '/' || !q[2])) {
                    q += q[2] ? 3 : 2;
                    if (n - 1 > raiz) { n--; while (n > raiz && t[n - 1] != '/') n--; }
                    continue;
                }
                break;
            }
            for (; *q; q++) PON(*q);
        }
    }
    #undef PON
    t[n] = 0;
    for (int i = 0; i < n; i++) if (t[i] == '#') { t[i] = 0; break; }
    if (largo_c(t) >= cap) return -1;
    copiar_c(out, t, cap);
    return 0;
}

static void codificar(const char *s, char *out, int cap) {
    static const char h[] = "0123456789ABCDEF";
    int n = largo_c(out);
    for (; *s && n + 4 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out[n++] = (char)c;
        else if (c == ' ') out[n++] = '+';
        else { out[n++] = '%'; out[n++] = h[c >> 4]; out[n++] = h[c & 15]; }
    }
    out[n] = 0;
}

/* Lo escrito en la barra a una URL: con esquema tal cual; con un punto y sin espacios, https://; si no, busqueda. */
static void a_url(const char *escrito, char *out, int cap) {
    while (*escrito == ' ') escrito++;
    if (empieza(escrito, "http://") || empieza(escrito, "https://")) { copiar_c(out, escrito, cap); return; }
    int punto = 0, espacio = 0;
    for (const char *p = escrito; *p; p++) { if (*p == '.') punto = 1; if (*p == ' ') espacio = 1; }
    if (punto && !espacio) { copiar_c(out, "https://", cap); int n = largo_c(out); copiar_c(out + n, escrito, cap - n); return; }
    /* DuckDuckGo, Mojeek y NPR cortan el saludo TLS del nucleo (alerta fatal); Wikipedia no, y no pide JavaScript. */
    copiar_c(out, "https://es.wikipedia.org/w/index.php?fulltext=1&search=", cap);
    codificar(escrito, out, cap);
}

/* ------------------------------------------------------------------ dibujo */

static char barra_txt[URL_MAX];
static int barra_n, barra_sel = 1;
static char estado_txt[96];
static int scroll, enlace_bajo = -1;

static void txt_num(char *d, int cap, uint64_t v) {
    char b[24]; int i = 0, n = largo_c(d);
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i && n + 1 < cap) d[n++] = b[--i];
    d[n] = 0;
}
static void txt_add(char *d, int cap, const char *o) { int n = largo_c(d); copiar_c(d + n, o, cap - n); }

static int boton_atras_x, boton_inicio_x, boton_an, campo_x, campo_an;

static void dibujar_barra(void) {
    rect(&L, 0, 0, AN, BARRA_AL, BARRA);
    rect(&L, 0, BARRA_AL - S(3), AN, S(3), 0xFF000000u | ROJO);
    int px = TAM[0], y = BARRA_AL / 2 + fuente_tam(px)->asc / 2 - S(2);
    boton_an = S(118);
    boton_atras_x = S(16); boton_inicio_x = boton_atras_x + boton_an + S(10);
    redondo(&L, boton_atras_x, S(14), boton_an, BARRA_AL - S(31), S(6), hist_n > 1 ? CAMPO : 0xFF161B26u);
    texto(&L, boton_atras_x + (boton_an - texto_ancho(px, "ATRÁS")) / 2, y, px, hist_n > 1 ? BLANCO : TENUE, 255, 0, "ATRÁS");
    redondo(&L, boton_inicio_x, S(14), boton_an, BARRA_AL - S(31), S(6), CAMPO);
    texto(&L, boton_inicio_x + (boton_an - texto_ancho(px, "INICIO")) / 2, y, px, BLANCO, 255, 0, "INICIO");
    campo_x = boton_inicio_x + boton_an + S(16);
    int estado_an = S(380);
    campo_an = AN - campo_x - estado_an - S(16);
    redondo(&L, campo_x, S(14), campo_an, BARRA_AL - S(31), S(6), CAMPO);
    if (barra_sel) rect(&L, campo_x + S(10), y - fuente_tam(px)->asc, texto_ancho(px, barra_txt) + S(4), fuente_tam(px)->alto, 0xFF2D3A55u);
    /* Si no cabe, se ve el final, que es donde se escribe */
    const char *ver = barra_txt;
    while (*ver && texto_ancho(px, ver) > campo_an - S(30)) ver++;
    int fx = texto(&L, campo_x + S(12), y, px, BLANCO, 255, 0, ver);
    rect(&L, fx + S(2), y - fuente_tam(px)->asc + S(2), S(2), fuente_tam(px)->asc, 0xFF000000u | NEON);
    /* Estado y titulo a la derecha */
    int ex = AN - estado_an;
    char t[160]; copiar_c(t, titulo[0] ? titulo : "NekoSaka", sizeof(t));
    while (largo_c(t) > 3 && texto_ancho(TAM[0] - 4, t) > estado_an - S(24)) { int n = largo_c(t); t[n - 1] = 0; }
    texto(&L, ex, S(36), TAM[0] - 4, BLANCO, 255, 0, t);
    texto(&L, ex, S(62), TAM[0] - 6, TENUE, 255, 0, estado_txt);
}

static void dibujar_vista(void) {
    rect(&L, 0, VISTA_Y, AN, VISTA_AL, FONDO);
    /* Las palabras van en orden de y: se salta a la primera visible */
    int a = 0, b = npal;
    while (a < b) { int m = (a + b) / 2; if (pals[m].y + S(80) < scroll) a = m + 1; else b = m; }
    for (int i = a; i < npal; i++) {
        const struct pal *p = &pals[i];
        int y = VISTA_Y + p->y - scroll;
        if (y > VISTA_Y + VISTA_AL) break;
        int px = TAM[p->tam];
        const struct f_tam *ft = fuente_tam(px);
        int base = y + (int)ft->asc;
        if (y + (int)ft->alto < VISTA_Y) continue;
        uint32_t color = PALETA[p->color];
        if (p->enlace >= 0 && p->enlace == enlace_bajo) color = NEON;
        texto(&L, COL_X + p->x, base, px, color, 255, 0, texto_doc + p->off);
        if (p->subraya) {
            int bajo = p->enlace == enlace_bajo;
            rect(&L, COL_X + p->x, base + S(3), p->ancho, bajo ? 2 : 1, 0xFF000000u | (bajo ? NEON : 0x2F6F85u));
        }
    }
    /* La barra de la vista no debe quedar tapada por texto que empiece arriba */
    rect(&L, 0, VISTA_Y - S(1), AN, S(1), 0xFF000000u | ROJO);
    /* Barra de desplazamiento */
    if (doc_alto > VISTA_AL) {
        int pista = VISTA_AL - S(16), alto = pista * VISTA_AL / doc_alto;
        if (alto < S(40)) alto = S(40);
        int maxs = doc_alto - VISTA_AL, y = VISTA_Y + S(8) + (pista - alto) * scroll / (maxs ? maxs : 1);
        redondo(&L, AN - S(14), VISTA_Y + S(8), S(6), pista, S(3), 0xFF1A2030u);
        redondo(&L, AN - S(14), y, S(6), alto, S(3), 0xFF000000u | ROJO);
    }
}

static void presentar_todo(void) { taek_presentar(sup.id, 0, 0, AN, AL); }
static void presentar_barra(void) { dibujar_barra(); taek_presentar(sup.id, 0, 0, AN, BARRA_AL); }
static void presentar_vista(void) { dibujar_vista(); taek_presentar(sup.id, 0, VISTA_Y - S(1), AN, VISTA_AL + S(1)); }

static void limitar_scroll(void) {
    int maxs = doc_alto - VISTA_AL;
    if (maxs < 0) maxs = 0;
    if (scroll > maxs) scroll = maxs;
    if (scroll < 0) scroll = 0;
}

/* ------------------------------------------------------------------ paginas */

static const char INICIO[] =
    "<title>Inicio</title>"
    "<h1>NekoSaka \xC2\xB7 Navegador</h1>"
    "<p>Un navegador sencillo que corre en Anillo 3 de TAEK OS. No va dentro del núcleo: se copió al disco y se "
    "lanzó con <code>ejecutar /navegador.elf</code>, sin reiniciar. El núcleo descarga la página (HTTP o HTTPS con TLS 1.3) y "
    "este programa la lee y la dibuja.</p>"
    "<p>Escribe una direcci\xC3\xB3n arriba y pulsa Intro. Si escribes palabras sueltas, se buscan en Wikipedia.</p>"
    "<h2>Para empezar</h2><ul>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">La primera p\xC3\xA1gina web (CERN, 1991)</a></li>"
    "<li><a href=\"https://es.wikipedia.org/wiki/Gato\">Wikipedia: Gato</a></li>"
    "<li><a href=\"https://search.marginalia.nu/\">Marginalia, un buscador de la web peque\xC3\xB1" "a</a></li>"
    "<li><a href=\"https://example.com/\">example.com</a></li>"
    "<li><a href=\"http://neverssl.com/\">neverssl.com, sin cifrar</a></li>"
    "</ul><h2>Teclas</h2><ul>"
    "<li>Flechas arriba y abajo: desplazar. Ctrl+D y Ctrl+U: media p\xC3\xA1gina.</li>"
    "<li>Clic en un enlace: abrirlo. Clic derecho, Ctrl+B o el bot\xC3\xB3n ATR\xC3\x81S: volver.</li>"
    "<li>Esc: volver a la terminal.</li></ul>"
    "<h3>Lo que no hace (todavía)</h3><p>No ejecuta JavaScript, no aplica estilos y no muestra im\xC3\xA1genes: "
    "de cada imagen queda su texto alternativo entre corchetes. No abre p\xC3\xA1ginas de m\xC3\xA1s de 1 MB, "
    "y algunos sitios (DuckDuckGo, por ejemplo) rechazan el TLS del n\xC3\xBA" "cleo.</p>";

static char *cuerpo;
static struct taek_web web;

static void mostrar_html(const char *h, int n) {
    doc_vaciar();
    html(h, h + n);
    scroll = 0; enlace_bajo = -1;
}

static void pagina_error(const char *que, const char *detalle) {
    static char b[2048];
    b[0] = 0;
    txt_add(b, sizeof(b), "<title>No se pudo abrir</title><h1>No se pudo abrir la p\xC3\xA1gina</h1><p>");
    txt_add(b, sizeof(b), que);
    txt_add(b, sizeof(b), "</p><p><code>");
    txt_add(b, sizeof(b), detalle);
    txt_add(b, sizeof(b), "</code></p><p>Comprueba la direcci\xC3\xB3n o prueba con otra. Muchas p\xC3\xA1ginas grandes "
                          "piden m\xC3\xA1s de lo que este navegador entiende.</p>");
    mostrar_html(b, largo_c(b));
}

static void abrir(const char *url, int guardar) {
    if (empieza(url, "taek:inicio")) {
        copiar_c(url_actual, "taek:inicio", sizeof(url_actual));
        mostrar_html(INICIO, (int)sizeof(INICIO) - 1);
        copiar_c(estado_txt, "P\xC3\xA1gina de inicio", sizeof(estado_txt));
    } else {
        copiar_c(estado_txt, "Cargando\xC2\xB7\xC2\xB7\xC2\xB7", sizeof(estado_txt));
        copiar_c(barra_txt, url, sizeof(barra_txt)); barra_n = largo_c(barra_txt); barra_sel = 0;
        presentar_barra();
        memset(&web, 0, sizeof(web));
        copiar_c(web.url, url, sizeof(web.url));
        web.destino = (uint64_t)(uintptr_t)cuerpo;
        web.capacidad = CUERPO_MAX;
        long t0 = taek_ms();
        long r = taek_web(&web);
        long ms = taek_ms() - t0;
        copiar_c(url_actual, web.url, sizeof(url_actual));
        if (r < 0) {
            char d[96] = "codigo "; txt_num(d, sizeof(d), (uint64_t)web.error);
            const char *que = web.error == TAEK_WEB_EURL ? "La direcci\xC3\xB3n no es v\xC3\xA1lida." :
                              web.error == TAEK_WEB_EHTTP ? "El servidor respondi\xC3\xB3 algo que no es HTTP v\xC3\xA1lido." :
                              web.error == TAEK_WEB_EREDIR ? "La redirecci\xC3\xB3n no es v\xC3\xA1lida o es insegura." :
                              web.error == 2012 ? "La p\xC3\xA1gina pasa de 1 MB, lo m\xC3\xA1ximo que descarga el n\xC3\xBA" "cleo." :
                              web.error == 2009 || web.error == 2007 ? "El sitio rechaz\xC3\xB3 el saludo TLS del n\xC3\xBA" "cleo (TLS 1.3 con lo que este sabe negociar)." :
                              web.error == 2003 ? "No se encontr\xC3\xB3 el nombre (DNS)." :
                              "No se pudo descargar: sin respuesta, sin red o la conexi\xC3\xB3n se cort\xC3\xB3.";
            pagina_error(que, d);
            copiar_c(estado_txt, "Error ", sizeof(estado_txt)); txt_num(estado_txt, sizeof(estado_txt), (uint64_t)web.error);
        } else {
            cuerpo[web.bytes] = 0;
            mostrar_html(cuerpo, (int)web.bytes);
            estado_txt[0] = 0;
            txt_num(estado_txt, sizeof(estado_txt), (uint64_t)web.estado);
            txt_add(estado_txt, sizeof(estado_txt), " \xC2\xB7 ");
            txt_num(estado_txt, sizeof(estado_txt), web.total / 1024);
            txt_add(estado_txt, sizeof(estado_txt), " KB \xC2\xB7 ");
            txt_num(estado_txt, sizeof(estado_txt), (uint64_t)ms);
            txt_add(estado_txt, sizeof(estado_txt), " ms");
            if (web.total > web.bytes) txt_add(estado_txt, sizeof(estado_txt), " \xC2\xB7 cortada");
        }
    }
    if (guardar) {
        if (hist_n == 16) { for (int i = 1; i < 16; i++) copiar_c(hist[i - 1], hist[i], URL_MAX); hist_n--; }
        copiar_c(hist[hist_n++], url_actual, URL_MAX);
    }
    copiar_c(barra_txt, url_actual, sizeof(barra_txt)); barra_n = largo_c(barra_txt); barra_sel = 1;
    dibujar_barra(); dibujar_vista(); presentar_todo();
    /* Marca para quien mira la consola o prueba por LAN */
    char m[160] = "[NAVEGADOR] "; txt_add(m, sizeof(m), estado_txt); txt_add(m, sizeof(m), " "); txt_add(m, sizeof(m), url_actual); txt_add(m, sizeof(m), "\n");
    taek_escribir(m, largo_c(m));
}

static void atras(void) {
    if (hist_n < 2) return;
    hist_n--;
    abrir(hist[hist_n - 1], 0);
}

/* ------------------------------------------------------------------ entrada */

static int esc_paso; static long esc_t;
static uint32_t filtrar(uint32_t k) {
    if (esc_paso == 1) { if (k == '[') { esc_paso = 2; return 0; } esc_paso = 0; }
    else if (esc_paso == 2) { esc_paso = 0; return k == 'A' ? 17 : k == 'B' ? 18 : 0; }
    else if (k == 27) { esc_paso = 1; esc_t = taek_ms(); return 0; }
    return k;
}

static void desplazar(int d) { int antes = scroll; scroll += d; limitar_scroll(); if (scroll != antes) presentar_vista(); }

/* 1 si hay que salir */
static int tecla(uint32_t k) {
    if (k == 27) return 1;
    if (k == 17) { desplazar(-S(90)); return 0; }
    if (k == 18) { desplazar(S(90)); return 0; }
    if (k == 4) { desplazar(VISTA_AL / 2); return 0; }
    if (k == 21) { desplazar(-VISTA_AL / 2); return 0; }
    if (k == 2) { atras(); return 0; }
    if (k == '\n' || k == '\r') {
        if (!barra_n) return 0;
        char u[URL_MAX];
        if (barra_sel) copiar_c(u, url_actual, sizeof(u)); else a_url(barra_txt, u, sizeof(u));
        abrir(u, 1);
        return 0;
    }
    if (k == 8 || k == 127) {
        if (barra_sel) { barra_n = 0; barra_sel = 0; }
        else if (barra_n) { barra_n--; while (barra_n && ((unsigned char)barra_txt[barra_n] & 0xC0) == 0x80) barra_n--; }
        barra_txt[barra_n] = 0;
        presentar_barra();
        return 0;
    }
    if (k >= 32 && k < 256) {
        if (barra_sel) { barra_n = 0; barra_sel = 0; }
        if (barra_n + 2 < URL_MAX) {
            if (k < 0x80) barra_txt[barra_n++] = (char)k;
            else { barra_txt[barra_n++] = (char)(0xC0 | (k >> 6)); barra_txt[barra_n++] = (char)(0x80 | (k & 0x3F)); }
        }
        barra_txt[barra_n] = 0;
        presentar_barra();
    }
    return 0;
}

static int enlace_en(int x, int y) {
    if (y < VISTA_Y) return -1;
    int dy = y - VISTA_Y + scroll, dx = x - COL_X;
    for (int i = 0; i < npal; i++) {
        const struct pal *p = &pals[i];
        if (p->y > dy) break;
        if (p->enlace < 0) continue;
        int alto = alto_de(p->tam);
        if (dy >= p->y && dy < p->y + alto && dx >= p->x - S(3) && dx < p->x + p->ancho + S(3)) return p->enlace;
    }
    return -1;
}

static uint32_t botones_antes;
static int arrastre;
static void raton(const struct taek_evento *e) {
    if (!e->hay_puntero) return;
    uint32_t subida = e->botones & ~botones_antes;
    botones_antes = e->botones;
    /* Arrastrar la barra de desplazamiento */
    if ((subida & 1) && e->x >= AN - S(24) && e->y >= VISTA_Y) arrastre = 1;
    if (!(e->botones & 1)) arrastre = 0;
    if (arrastre && doc_alto > VISTA_AL) {
        int antes = scroll;
        scroll = (int)((int64_t)(e->y - VISTA_Y) * (doc_alto - VISTA_AL) / (VISTA_AL ? VISTA_AL : 1));
        limitar_scroll();
        if (scroll != antes) presentar_vista();
        return;
    }
    int en = enlace_en(e->x, e->y);
    if (en != enlace_bajo) { enlace_bajo = en; presentar_vista(); }
    if (subida & 2) { atras(); return; }
    if (!(subida & 1)) return;
    if (e->y < BARRA_AL) {
        if (e->x >= boton_atras_x && e->x < boton_atras_x + boton_an) atras();
        else if (e->x >= boton_inicio_x && e->x < boton_inicio_x + boton_an) abrir("taek:inicio", 1);
        else if (e->x >= campo_x && e->x < campo_x + campo_an) { barra_sel = 1; presentar_barra(); }
        return;
    }
    if (en >= 0) {
        char u[URL_MAX];
        if (resolver(empieza(url_actual, "taek:") ? "https://example.com/" : url_actual, hrefs + href_off[en], u, sizeof(u)) == 0) abrir(u, 1);
    }
}

/* ------------------------------------------------------------------ principal */

long principal(uint64_t a, uint64_t b) {
    (void)a; (void)b;
    struct taek_pantalla pan;
    if (taek_pantalla(&pan) || !pan.ancho || !pan.alto) return 2;
    AN = (int)pan.ancho; AL = (int)pan.alto;
    sup = (struct taek_superficie){ (uint32_t)AN, (uint32_t)AL, 0, 0, 0, 0, 0 };
    if (taek_superficie(&sup)) return 3;
    L = (struct lienzo){ (uint32_t *)(uintptr_t)sup.pixeles, AN, AL };
    texto_doc = malloc(MAX_TEXTO);
    pals = malloc(sizeof(struct pal) * MAX_PAL);
    href_off = malloc(sizeof(uint32_t) * MAX_ENLACES);
    hrefs = malloc(MAX_HREF);
    cuerpo = malloc(CUERPO_MAX + 1);
    if (!texto_doc || !pals || !href_off || !hrefs || !cuerpo) return 4;
    if (AL >= 1000) { TAM[0] = 22; TAM[1] = 28; TAM[2] = 36; TAM[3] = 48; }
    else { TAM[0] = 18; TAM[1] = 22; TAM[2] = 28; TAM[3] = 36; }
    BARRA_AL = S(84);
    VISTA_Y = BARRA_AL;
    VISTA_AL = AL - BARRA_AL;
    COL_AN = AN - S(160) > S(1150) ? S(1150) : AN - S(160);
    COL_X = (AN - COL_AN) / 2;
    limpiar(&L, FONDO);
    abrir("taek:inicio", 1);
    taek_escribir("[NAVEGADOR] ABIERTO\n", 20);

    struct taek_evento e;
    for (;;) {
        long t = taek_ms();
        uint32_t botones = 0;
        for (int i = 0; i < 16; i++) {
            long r = taek_evento(&e);
            if (r < 0) return 1;
            botones |= e.botones;
            if (!r) break;
            uint32_t k = filtrar(e.tecla);
            if (k == 3) return 0;   /* Ctrl+C */
            if (k && tecla(k)) return 0;
        }
        if (esc_paso == 1 && taek_ms() - esc_t > 40) { esc_paso = 0; return 0; }
        e.botones = botones;
        raton(&e);
        long g = taek_ms() - t;
        if (g < 16) taek_esperar(16 - g);
    }
}
