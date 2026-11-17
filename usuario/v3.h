#ifndef TAEK_V3_H
#define TAEK_V3_H
/* Dibujo de la interfaz v3: poligonos con borde suavizado, trazos gruesos, texto girado y el logo NekoSaka.
 * Todo lo de la v3 (el gato que se desarma, las letras que caen, las lineas que vuelan) se reduce a una sola operacion:
 * rellenar poligonos. Una linea es un rectangulo girado; una letra, un poligono con huecos. */
#include "ui.h"
#include "logo_v3.h"

static inline float raizf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}
static inline float absf(float x) { return x < 0 ? -x : x; }
static inline float mezclaf(float a, float b, float t) { return a + (b - a) * t; }
static inline float limitar01(float t) { return t < 0 ? 0 : t > 1 ? 1 : t; }

/* Arco tangente por polinomio: error menor a 0,005 rad, de sobra para girar lineas. */
static float arco_tan2(float y, float x) {
    const float pi = 3.14159265f;
    float ax = absf(x), ay = absf(y);
    if (ax < 1e-6f && ay < 1e-6f) return 0;
    float a = ax > ay ? ay / ax : ax / ay, s = a * a;
    float r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
    if (ay > ax) r = pi / 2 - r;
    if (x < 0) r = pi - r;
    return y < 0 ? -r : r;
}

/* Curvas de tiempo: la de entrada y salida suave de la v3, y una que frena al llegar. */
static float suave(float t) {
    t = limitar01(t);
    if (t < 0.5f) return 4 * t * t * t;
    float u = -2 * t + 2;
    return 1 - u * u * u / 2;
}
static float sale(float t) { t = 1 - limitar01(t); return 1 - t * t * t; }

/* ---------------------------------------------------------------- relleno */

/* Cada arista guarda y0 < y1 y su sentido original, para la regla de la vuelta. */
#define RELL_MAX 512
struct arista { float x0, y0, x1, y1, pendiente; int sentido; };
static struct arista rell_a[RELL_MAX];
static int rell_n;
static float rell_xmin, rell_xmax, rell_ymin, rell_ymax;
static uint16_t rell_cob[8192];

static void rell_ini(void) {
    rell_n = 0;
    rell_xmin = rell_ymin = 1e9f;
    rell_xmax = rell_ymax = -1e9f;
}
static void rell_arista(float x0, float y0, float x1, float y1) {
    if (y0 == y1 || rell_n >= RELL_MAX) return;
    int s = 1;
    if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; s = -1; }
    rell_a[rell_n++] = (struct arista){ x0, y0, x1, y1, (x1 - x0) / (y1 - y0), s };
    if (x0 < rell_xmin) rell_xmin = x0;
    if (x1 < rell_xmin) rell_xmin = x1;
    if (x0 > rell_xmax) rell_xmax = x0;
    if (x1 > rell_xmax) rell_xmax = x1;
    if (y0 < rell_ymin) rell_ymin = y0;
    if (y1 > rell_ymax) rell_ymax = y1;
}
static void rell_contorno(const float *p, int n) {
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        rell_arista(p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1]);
    }
}

/* Degradado de tres paradas a lo largo de un eje; NULL es color liso. */
struct deg3 { float x0, y0, ex, ey; uint32_t c[3]; };
static inline uint32_t deg3_color(const struct deg3 *g, int x, int y) {
    float t = limitar01(((float)x - g->x0) * g->ex + ((float)y - g->y0) * g->ey) * 2.0f;
    return t < 1 ? entre(g->c[0], g->c[1], (uint32_t)(t * 255.0f)) : entre(g->c[1], g->c[2], (uint32_t)((t - 1) * 255.0f));
}

static inline void rell_tramo(float a, float b, int xa, int xb, int *lo, int *hi) {
    if (a < (float)xa) a = (float)xa;
    if (b > (float)xb) b = (float)xb;
    if (b <= a) return;
    int ia = (int)a, ib = (int)b;
    if (ia == ib) rell_cob[ia] += (uint16_t)((b - a) * 64.0f + 0.5f);
    else {
        rell_cob[ia] += (uint16_t)(((float)(ia + 1) - a) * 64.0f + 0.5f);
        for (int x = ia + 1; x < ib; x++) rell_cob[x] += 64;
        if (ib < xb) rell_cob[ib] += (uint16_t)((b - (float)ib) * 64.0f + 0.5f);
    }
    if (ia < *lo) *lo = ia;
    if ((ib < xb ? ib : xb - 1) > *hi) *hi = ib < xb ? ib : xb - 1;
}

/* Cuatro sublineas por fila; en cada una, cobertura horizontal exacta. Par-impar para letras con hueco. */
static void rell_pintar(struct lienzo *l, uint32_t c, int parimpar, const struct deg3 *g) {
    if (!rell_n) return;
    int ya = (int)rell_ymin, yb = (int)rell_ymax + 1, xa = (int)rell_xmin, xb = (int)rell_xmax + 2;
    if (rell_ymin < 0) ya = 0;
    if (rell_xmin < 0) xa = 0;
    if (yb > l->al) yb = l->al;
    if (xb > l->an) xb = l->an;
    if (xb > (int)(sizeof(rell_cob) / sizeof(rell_cob[0]))) xb = (int)(sizeof(rell_cob) / sizeof(rell_cob[0]));
    if (ya >= yb || xa >= xb) return;
    float cx[96];
    int cs[96];
    for (int y = ya; y < yb; y++) {
        int lo = xb, hi = xa - 1;
        for (int s = 0; s < 4; s++) {
            float sy = (float)y + ((float)s + 0.5f) * 0.25f;
            int k = 0;
            for (int i = 0; i < rell_n && k < 96; i++) {
                const struct arista *e = &rell_a[i];
                if (sy < e->y0 || sy >= e->y1) continue;
                float x = e->x0 + (sy - e->y0) * e->pendiente;
                int j = k++;
                while (j > 0 && cx[j - 1] > x) { cx[j] = cx[j - 1]; cs[j] = cs[j - 1]; j--; }
                cx[j] = x; cs[j] = e->sentido;
            }
            int vuelta = 0;
            for (int i = 0; i + 1 < k; i++) {
                vuelta += cs[i];
                if (parimpar ? !(i & 1) : vuelta != 0) rell_tramo(cx[i], cx[i + 1], xa, xb, &lo, &hi);
            }
        }
        uint32_t *p = l->px + (uint64_t)y * l->an;
        for (int x = lo; x <= hi; x++) {
            uint32_t a = rell_cob[x];
            if (!a) continue;
            rell_cob[x] = 0;
            uint32_t col = g ? deg3_color(g, x, y) : c;
            p[x] = sobre(p[x], a >= 255 ? col : cubrir(col, a));
        }
    }
}

static void poligono(struct lienzo *l, const float *p, int n, uint32_t c) {
    rell_ini();
    rell_contorno(p, n);
    rell_pintar(l, c, 0, NULL);
}

/* Linea gruesa como rectangulo girado. remate: se alarga medio grosor por cada punta, casi como una punta redonda. */
static void trazo(struct lienzo *l, float x0, float y0, float x1, float y1, float w, uint32_t c, int remate) {
    float dx = x1 - x0, dy = y1 - y0, largo_ = raizf(dx * dx + dy * dy);
    if (largo_ < 0.01f) { dx = 1; dy = 0; largo_ = 1; }
    float ux = dx / largo_, uy = dy / largo_, nx = -uy * w * 0.5f, ny = ux * w * 0.5f;
    if (remate) {
        float e = w * 0.5f;
        x0 -= ux * e; y0 -= uy * e; x1 += ux * e; y1 += uy * e;
    }
    float q[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
    poligono(l, q, 4, c);
}

static void contorno(struct lienzo *l, const float *p, int n, int cerrado, float w, uint32_t c) {
    for (int i = 0; i + 1 < n + (cerrado ? 1 : 0); i++) {
        int j = (i + 1) % n;
        trazo(l, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1], w, c, 1);
    }
}

/* -------------------------------------------------------------- texto girado */

/* Texto que se lee de abajo hacia arriba, escalado. Se dibuja horizontal en un borrador y se copia girado con muestreo bilineal.
 * El borrador lo presta quien llama (2400 x 300 pixeles): un arreglo estatico de ese tamano no cabe en los 4 MB que el
 * cargador de Anillo 3 deja para la imagen del programa. */
static void texto_girado(struct lienzo *l, uint32_t *borrador, int x, int y_base, int px, float escala, uint32_t rgb, uint32_t alfa, const char *s) {
    uint32_t *giro_borrador = borrador;
    struct lienzo b = { giro_borrador, 2400, 300 };
    int w = texto_ancho(px, s) + 8;
    if (w > b.an) w = b.an;
    int alto = px * 5 / 4 + 8;
    if (alto > b.al) alto = b.al;
    for (int f = 0; f < alto; f++) for (int k = 0; k < w; k++) giro_borrador[f * b.an + k] = 0;
    texto(&b, 4, px + 2, px, 0xFFFFFF, 255, 0, s);
    /* Destino: columna de ancho alto*escala que sube desde y_base. Fila del borrador -> x; columna -> -y. */
    int dw = (int)((float)alto * escala), dh = (int)((float)w * escala);
    for (int dy = 0; dy < dh; dy++) {
        int fy = y_base - dy;
        if (fy < 0 || fy >= l->al) continue;
        float sx = (float)dy / escala;
        int sx0 = (int)sx;
        float fx_ = sx - (float)sx0;
        if (sx0 + 1 >= w) continue;
        uint32_t *p = l->px + (uint64_t)fy * l->an;
        for (int dx = 0; dx < dw; dx++) {
            int fx = x + dx;
            if (fx < 0 || fx >= l->an) continue;
            float sy = (float)dx / escala;
            int sy0 = (int)sy;
            if (sy0 < 0 || sy0 + 1 >= alto) continue;
            float fy_ = sy - (float)sy0;
            const uint32_t *q = giro_borrador + sy0 * b.an + sx0;
            float a = (float)(q[0] >> 24) * (1 - fx_) * (1 - fy_) + (float)(q[1] >> 24) * fx_ * (1 - fy_) +
                      (float)(q[b.an] >> 24) * (1 - fx_) * fy_ + (float)(q[b.an + 1] >> 24) * fx_ * fy_;
            uint32_t cob = (uint32_t)a * alfa / 255;
            if (cob) p[fx] = sobre(p[fx], premul(cob, rgb));
        }
    }
}

/* Texto que se descifra: lo ya fijo, y el resto con glifos al azar. u de 0 a 1. */
static void descifrado(char *d, int max, const char *s, float u, uint32_t semilla) {
    static const char glifos[] = "01#%&@$ABCDEFXZ<>/";
    int n = 0;
    const char *p = s;
    while (*p) { utf8(&p); n++; }
    int fijos = (int)(u * (float)n);
    int i = 0, o = 0;
    p = s;
    while (*p && o + 4 < max) {
        const char *ini = p;
        uint32_t cp = utf8(&p);
        if (i < fijos || cp == ' ') { while (ini < p && o + 1 < max) d[o++] = *ini++; }
        else { semilla = semilla * 1103515245u + 12345u; d[o++] = glifos[(semilla >> 16) % (sizeof(glifos) - 1)]; }
        i++;
    }
    d[o] = 0;
}

/* ------------------------------------------------------------------- logo */

/* Del logo a pixeles de una pantalla de 1080 de alto, y luego a la escala real. */
static float logo_esc = 1.0f;
static inline void logo_punto(float x, float y, float *ox, float *oy) {
    *ox = (LOGO_M[0] * x + LOGO_M[2] * y + LOGO_M[4]) * logo_esc;
    *oy = (LOGO_M[1] * x + LOGO_M[3] * y + LOGO_M[5]) * logo_esc;
}
static inline float logo_k(void) { return 0.632f * logo_esc; }  /* Largo de una unidad del logo en pantalla */

/* Caja del hexagono en pantalla, sin la capa: para la reticula y para colocar la capa del heroe. */
static void logo_caja(float *x0, float *y0, float *x1, float *y1) {
    *x0 = *y0 = 1e9f; *x1 = *y1 = -1e9f;
    for (unsigned i = 0; i < sizeof(logo_marco_ext) / sizeof(logo_marco_ext[0]) / 2; i++) {
        float x, y;
        logo_punto(logo_marco_ext[2 * i], logo_marco_ext[2 * i + 1], &x, &y);
        if (x < *x0) *x0 = x;
        if (x > *x1) *x1 = x;
        if (y < *y0) *y0 = y;
        if (y > *y1) *y1 = y;
    }
}

#define NFACETAS 15
/* Estado de dibujo del gato: cuanto se separa cada faceta y cuanto brilla en blanco (0 a 255). */
struct pose_gato { float sep; uint8_t blanco[NFACETAS]; uint8_t marco_blanco; uint8_t sin_gato; };

static void faceta_desplazamiento(int i, float sep, float *dx, float *dy) {
    float cx, cy, ox, oy;
    logo_punto(logo_facetas[i].cx, logo_facetas[i].cy, &cx, &cy);
    logo_punto(LOGO_CENTRO_X, LOGO_CENTRO_Y, &ox, &oy);
    float vx = cx - ox, vy = cy - oy, d = raizf(vx * vx + vy * vy);
    if (d < 1) d = 1;
    /* Hacia fuera, mas las de lejos; y un leve giro para que no parezca una explosion simetrica. */
    *dx = (vx / d * 0.55f + vy / d * 0.18f) * sep * (0.6f + d / (300.0f * logo_esc));
    *dy = (vy / d * 0.55f - vx / d * 0.18f) * sep * (0.6f + d / (300.0f * logo_esc));
}

/* Centro de una faceta en la pantalla; (ox, oy): cuanto se ha movido la capa desde su reposo. */
static void faceta_centro(int i, float sep, float ox, float oy, float *x, float *y) {
    float dx, dy;
    logo_punto(logo_facetas[i].cx, logo_facetas[i].cy, x, y);
    faceta_desplazamiento(i, sep, &dx, &dy);
    *x += dx + ox; *y += dy + oy;
}

static void pintar_fragmentos(struct lienzo *l, float ox, float oy) {
    for (unsigned i = 0; i < sizeof(logo_fragmentos) / sizeof(logo_fragmentos[0]); i++) {
        const struct logo_fragmento *f = &logo_fragmentos[i];
        float p[6];
        for (int k = 0; k < 3; k++) { p[2 * k] = f->x[k] * logo_esc - ox; p[2 * k + 1] = f->y[k] * logo_esc - oy; }
        poligono(l, p, 3, f->relleno);
        contorno(l, p, 3, 1, 1.2f * logo_esc, f->borde);
    }
}

/* El emblema: marco hexagonal con degradado rojo, filo de laser y las 15 facetas del gato. (ox, oy): origen de la capa. */
static void pintar_emblema(struct lienzo *l, float ox, float oy, const struct pose_gato *pg) {
    if (pg->sin_gato) return;  /* Mientras sus lineas vuelan, el gato no esta; los fragmentos si */
    enum { NE = sizeof(logo_marco_ext) / sizeof(logo_marco_ext[0]) / 2, NI = sizeof(logo_marco_int) / sizeof(logo_marco_int[0]) / 2 };
    float ext[NE * 2], in[NI * 2];
    for (int i = 0; i < NE; i++) { logo_punto(logo_marco_ext[2 * i], logo_marco_ext[2 * i + 1], &ext[2 * i], &ext[2 * i + 1]); ext[2 * i] -= ox; ext[2 * i + 1] -= oy; }
    for (int i = 0; i < NI; i++) { logo_punto(logo_marco_int[2 * i], logo_marco_int[2 * i + 1], &in[2 * i], &in[2 * i + 1]); in[2 * i] -= ox; in[2 * i + 1] -= oy; }
    float x0, y0, x1, y1;
    logo_caja(&x0, &y0, &x1, &y1);
    float k = logo_k();
    /* Resplandor: lo que en el navegador era un desenfoque, aqui son dos trazos anchos y tenues */
    contorno(l, ext, NE, 1, 26 * k, premul(34, 0xFF003Cu));
    contorno(l, ext, NE, 1, 14 * k, premul(70, 0xFF003Cu));
    struct deg3 g = { x0 - ox, y0 - oy, 0, 0, { 0xFFFF003Cu, 0xFFD60029u, 0xFF800018u } };
    float ax = x1 - x0, ay = y1 - y0, q = ax * ax + ay * ay;
    g.ex = ax / q; g.ey = ay / q;
    if (pg->marco_blanco) { uint32_t b = premul(255, 0xFFFFFFu); for (int i = 0; i < 3; i++) g.c[i] = entre(g.c[i], b, pg->marco_blanco); }
    rell_ini();
    rell_contorno(ext, NE);
    rell_contorno(in, NI);
    rell_pintar(l, 0, 1, &g);
    contorno(l, ext, NE, 1, 3.2f * k, 0xFFFFFFFFu);
    contorno(l, in, NI, 1, 2.0f * k, premul(200, 0xFCEE0Au));
    for (int i = 0; i < NFACETAS; i++) {
        const struct logo_faceta *f = &logo_facetas[i];
        float p[2 * 16], dx, dy;
        faceta_desplazamiento(i, pg->sep, &dx, &dy);
        for (int j = 0; j < f->n && j < 16; j++) {
            logo_punto(logo_faceta_pts[2 * (f->ini + j)], logo_faceta_pts[2 * (f->ini + j) + 1], &p[2 * j], &p[2 * j + 1]);
            p[2 * j] += dx - ox; p[2 * j + 1] += dy - oy;
        }
        uint32_t bl = pg->blanco[i];
        uint32_t relleno = entre(premul(52, 0xFF003Cu), premul(235, 0xFFFFFFu), bl);
        uint32_t borde = entre(0xFFFF003Cu, 0xFFFFFFFFu, bl);
        poligono(l, p, f->n, relleno);
        contorno(l, p, f->n, 1, 7 * k, borde);
    }
}

/* Los cables que salen de la cabeza del gato, sobre el fondo. */
static void pintar_cables(struct lienzo *l) {
    enum { NR = sizeof(logo_cable_rojo) / sizeof(logo_cable_rojo[0]) / 2, NO = sizeof(logo_cable_oro) / sizeof(logo_cable_oro[0]) / 2 };
    float r[NR * 2], o[NO * 2];
    for (int i = 0; i < NR * 2; i++) r[i] = (float)logo_cable_rojo[i] * logo_esc;
    for (int i = 0; i < NO * 2; i++) o[i] = (float)logo_cable_oro[i] * logo_esc;
    contorno(l, r, NR, 0, 9 * logo_esc, premul(60, 0xFF003Cu));
    contorno(l, r, NR, 0, 4.5f * logo_esc, premul(180, 0xFF003Cu));
    contorno(l, r, NR, 0, 1.6f * logo_esc, 0xFFFFFFFFu);
    contorno(l, o, NO, 0, 6 * logo_esc, premul(110, 0xFCEE0Au));
    /* Cable de datos: negro discontinuo sobre oro */
    for (int i = 0; i + 1 < NO; i += 2)
        trazo(l, o[2 * i], o[2 * i + 1], o[2 * i + 2], o[2 * i + 3], 2.2f * logo_esc, 0xFF0E1017u, 0);
}

/* ------------------------------------------------------------- segmentos */

/* Una linea que vuela: centro, medio largo, angulo, grosor, color (RGB) y opacidad. */
struct seg { float x1, y1, x2, y2, w, o; uint32_t c; };

/* Parte un contorno en k segmentos de igual largo. */
static int remuestrear(struct seg *out, const float *p, int n, int cerrado, int k, float w, uint32_t c, float o) {
    if (k <= 0 || n < 2) return 0;
    int m = cerrado ? n + 1 : n;
    float acum[200];
    if (m > 200) m = 200;
    acum[0] = 0;
    for (int i = 1; i < m; i++) {
        int a = i - 1, b = i % n;
        float dx = p[2 * b] - p[2 * a], dy = p[2 * b + 1] - p[2 * a + 1];
        acum[i] = acum[i - 1] + raizf(dx * dx + dy * dy);
    }
    float L = acum[m - 1] > 0 ? acum[m - 1] : 1;
    int i = 1;
    float px = p[0], py = p[1];
    for (int j = 0; j < k; j++) {
        float s = (float)(j + 1) * L / (float)k;
        while (i < m - 1 && acum[i] < s) i++;
        int a = i - 1, b = i % n;
        float d = acum[i] - acum[a], t = d > 0 ? (s - acum[a]) / d : 0;
        float qx = mezclaf(p[2 * a], p[2 * b], t), qy = mezclaf(p[2 * a + 1], p[2 * b + 1], t);
        out[j] = (struct seg){ px, py, qx, qy, w, o, c };
        px = qx; py = qy;
    }
    return k;
}

static float perimetro(const float *p, int n, int cerrado) {
    float L = 0;
    for (int i = 0; i + 1 < n + (cerrado ? 1 : 0); i++) {
        int j = (i + 1) % n;
        float dx = p[2 * j] - p[2 * i], dy = p[2 * j + 1] - p[2 * i + 1];
        L += raizf(dx * dx + dy * dy);
    }
    return L;
}

/* Reparte n segmentos entre varias formas segun su peso; cada una recibe al menos su minimo. */
struct forma { const float *p; int n, cerrado, min; float peso, w, o; uint32_t c; };
static int repartir(struct seg *out, const struct forma *f, int nf, int n) {
    int k[64];
    float total = 0;
    for (int i = 0; i < nf; i++) total += f[i].peso > 0 ? f[i].peso : 1;
    int suma = 0;
    for (int i = 0; i < nf; i++) {
        k[i] = (int)((float)n * (f[i].peso > 0 ? f[i].peso : 1) / total);
        if (k[i] < f[i].min) k[i] = f[i].min;
        suma += k[i];
    }
    /* Ajuste: lo que sobra o falta, de la mas pesada a la mas ligera */
    while (suma != n) {
        int mejor = -1;
        for (int i = 0; i < nf; i++) {
            if (suma > n && k[i] <= f[i].min) continue;
            if (mejor < 0 || f[i].peso > f[mejor].peso) mejor = i;
        }
        if (mejor < 0) break;
        k[mejor] += suma < n ? 1 : -1;
        suma += suma < n ? 1 : -1;
    }
    int o = 0;
    for (int i = 0; i < nf; i++) o += remuestrear(out + o, f[i].p, f[i].n, f[i].cerrado, k[i], f[i].w, f[i].c, f[i].o);
    return o;
}

static void ordenar_segs(struct seg *s, int n, int por_x) {
    for (int i = 1; i < n; i++) {
        struct seg v = s[i];
        float kv = por_x ? v.x1 + v.x2 : v.y1 + v.y2;
        int j = i;
        while (j > 0 && (por_x ? s[j - 1].x1 + s[j - 1].x2 : s[j - 1].y1 + s[j - 1].y2) > kv) { s[j] = s[j - 1]; j--; }
        s[j] = v;
    }
}

#endif
