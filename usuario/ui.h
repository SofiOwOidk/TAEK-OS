#ifndef TAEK_UI_H
#define TAEK_UI_H
/* Dibujo por software para programas de Ring 3: figuras con borde suavizado y texto del atlas. Todo en alfa premultiplicado. */
#include "taek.h"

struct lienzo { uint32_t *px; int an, al; };

static inline uint32_t premul(uint32_t a, uint32_t rgb) {
    uint32_t rb = (rgb & 0xFF00FFu) * a + 0x800080u;
    rb = ((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
    uint32_t g = (rgb & 0xFF00u) * a + 0x8000u;
    g = ((g + ((g >> 8) & 0xFF00u)) >> 8) & 0xFF00u;
    return a << 24 | rb | g;
}
/* Escala un color ya premultiplicado por una cobertura. */
static inline uint32_t cubrir(uint32_t c, uint32_t cob) {
    uint32_t rb = (c & 0xFF00FFu) * cob + 0x800080u;
    rb = ((rb + ((rb >> 8) & 0xFF00FFu)) >> 8) & 0xFF00FFu;
    uint32_t ag = ((c >> 8) & 0xFF00FFu) * cob + 0x800080u;
    ag = (ag + ((ag >> 8) & 0xFF00FFu)) & 0xFF00FF00u;
    return ag | rb;
}
static inline uint32_t sobre(uint32_t d, uint32_t s) {
    uint32_t a = s >> 24;
    if (a == 255) return s;
    if (!a) return d;
    return s + cubrir(d, 255 - a);
}

static void limpiar(struct lienzo *l, uint32_t c) {
    uint64_t n = (uint64_t)l->an * l->al;
    for (uint64_t i = 0; i < n; i++) l->px[i] = c;
}

static void rect(struct lienzo *l, int x, int y, int an, int al, uint32_t c) {
    int x1 = x + an, y1 = y + al;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > l->an) x1 = l->an;
    if (y1 > l->al) y1 = l->al;
    for (int f = y; f < y1; f++) {
        uint32_t *p = l->px + (uint64_t)f * l->an;
        for (int k = x; k < x1; k++) p[k] = sobre(p[k], c);
    }
}

/* Paralelogramo: la fila de arriba va desplazada `sesgo` pixeles a la derecha. Bordes laterales suavizados. */
static void para(struct lienzo *l, int x, int y, int an, int al, int sesgo, uint32_t c) {
    if (al <= 0 || an <= 0) return;
    for (int f = 0; f < al; f++) {
        int fy = y + f;
        if (fy < 0 || fy >= l->al) continue;
        int32_t despl = (int32_t)(((int64_t)(al - 1 - f) * sesgo << 8) / al);
        int32_t izq = (x << 8) + despl, der = izq + (an << 8);
        int xi = izq >> 8, xd = (der >> 8);
        uint32_t *p = l->px + (uint64_t)fy * l->an;
        uint32_t ci = 255 - (izq & 255), cd = der & 255;
        if (xi >= 0 && xi < l->an) p[xi] = sobre(p[xi], cubrir(c, ci));
        if (xd >= 0 && xd < l->an && cd) p[xd] = sobre(p[xd], cubrir(c, cd));
        int a = xi + 1 < 0 ? 0 : xi + 1, b = xd > l->an ? l->an : xd;
        for (int k = a; k < b; k++) p[k] = sobre(p[k], c);
    }
}

/* Un huevo: mas estrecho arriba que abajo. Degradado vertical entre dos colores opacos. La v3 ya no lo usa. */
__attribute__((unused)) static void huevo(struct lienzo *l, int cx, int cy, int rx, int ry, uint32_t arriba, uint32_t abajo, uint32_t alfa) {
    for (int y = -ry; y <= ry; y++) {
        int fy = cy + y;
        if (fy < 0 || fy >= l->al) continue;
        float ny = (float)y / (float)ry;
        float r = (float)rx * (0.84f + 0.16f * ny);
        uint32_t t = (uint32_t)(y + ry) * 255u / (uint32_t)(2 * ry);
        uint32_t base = (cubrir(arriba | 0xFF000000u, 255 - t) + cubrir(abajo | 0xFF000000u, t)) & 0x00FFFFFFu;
        uint32_t col = premul(alfa, base);
        for (int x = -rx; x <= rx; x++) {
            int fx = cx + x;
            if (fx < 0 || fx >= l->an) continue;
            float nx = (float)x / r;
            float d = (nx * nx + ny * ny - 1.0f) * r * 0.5f;
            if (d >= 0.5f) continue;
            uint32_t *p = l->px + (uint64_t)fy * l->an + fx;
            *p = sobre(*p, d <= -0.5f ? col : cubrir(col, (uint32_t)((0.5f - d) * 255.0f)));
        }
    }
}

/* Linea gruesa: pasos de disco. Para detalles, no para rendimiento. */
static void linea(struct lienzo *l, int x0, int y0, int x1, int y1, int grosor, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (!n) n = 1;
    for (int i = 0; i <= n; i++) rect(l, x0 + dx * i / n - grosor / 2, y0 + dy * i / n - grosor / 2, grosor, grosor, c);
}

/* Mezcla de dos colores premultiplicados: t de 0 a 255. */
static inline uint32_t entre(uint32_t a, uint32_t b, uint32_t t) { return cubrir(a, 255 - t) + cubrir(b, t); }

/* Degradado vertical entre dos colores premultiplicados, mezclado sobre lo que haya. */
static void degradado(struct lienzo *l, int x, int y, int an, int al, uint32_t c0, uint32_t c1) {
    for (int f = 0; f < al; f++) rect(l, x, y + f, an, 1, entre(c0, c1, al > 1 ? (uint32_t)f * 255u / (uint32_t)(al - 1) : 0));
}

/* Rectangulo redondeado con degradado vertical. Las esquinas se suavizan por distancia al centro del arco. */
static void redondo_deg(struct lienzo *l, int x, int y, int an, int al, int r, uint32_t c0, uint32_t c1) {
    if (an <= 0 || al <= 0) return;
    if (r > an / 2) r = an / 2;
    if (r > al / 2) r = al / 2;
    for (int f = 0; f < al; f++) {
        int fy = y + f;
        if (fy < 0 || fy >= l->al) continue;
        uint32_t c = entre(c0, c1, al > 1 ? (uint32_t)f * 255u / (uint32_t)(al - 1) : 0);
        uint32_t *p = l->px + (uint64_t)fy * l->an;
        int dy = f < r ? r - f : f >= al - r ? f - (al - r - 1) : 0;
        for (int k = 0; k < an; k++) {
            int fx = x + k;
            if (fx < 0 || fx >= l->an) continue;
            int dx = k < r ? r - k : k >= an - r ? k - (an - r - 1) : 0;
            if (dx && dy) {
                float d = (float)(dx * dx + dy * dy) - (float)(r * r);
                /* d crece unas 2r por pixel de distancia: de ahi la anchura del suavizado */
                float m = (float)(2 * r);
                if (d >= m * 0.5f) continue;
                if (d > -m * 0.5f) { p[fx] = sobre(p[fx], cubrir(c, (uint32_t)((0.5f - d / m) * 255.0f))); continue; }
            }
            p[fx] = sobre(p[fx], c);
        }
    }
}
static void redondo(struct lienzo *l, int x, int y, int an, int al, int r, uint32_t c) { redondo_deg(l, x, y, an, al, r, c, c); }

/* Borde de rectangulo redondeado de un pixel: dibujar el exterior y vaciar no sirve con alfa, se traza directamente. */
static void redondo_borde(struct lienzo *l, int x, int y, int an, int al, int r, uint32_t c) {
    rect(l, x + r, y, an - 2 * r, 1, c);
    rect(l, x + r, y + al - 1, an - 2 * r, 1, c);
    rect(l, x, y + r, 1, al - 2 * r, c);
    rect(l, x + an - 1, y + r, 1, al - 2 * r, c);
    for (int f = 0; f < r; f++) for (int k = 0; k < r; k++) {
        float d = (float)((r - k) * (r - k) + (r - f) * (r - f)) - (float)(r * r);
        float m = (float)(2 * r);
        if (d <= -m || d >= m) continue;
        uint32_t cob = (uint32_t)((1.0f - (d < 0 ? -d : d) / m) * 255.0f);
        uint32_t v = cubrir(c, cob);
        int xs[2] = { x + k, x + an - 1 - k }, ys[2] = { y + f, y + al - 1 - f };
        for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++)
            if (xs[a] >= 0 && xs[a] < l->an && ys[b] >= 0 && ys[b] < l->al) {
                uint32_t *p = l->px + (uint64_t)ys[b] * l->an + xs[a];
                *p = sobre(*p, v);
            }
    }
}

/* Circulo o anillo suavizado. interior 0: relleno. */
static void anillo(struct lienzo *l, int cx, int cy, int r, int interior, uint32_t c) {
    float re = (float)r, ri = (float)interior;
    for (int y = -r - 1; y <= r + 1; y++) {
        int fy = cy + y;
        if (fy < 0 || fy >= l->al) continue;
        uint32_t *p = l->px + (uint64_t)fy * l->an;
        for (int x = -r - 1; x <= r + 1; x++) {
            int fx = cx + x;
            if (fx < 0 || fx >= l->an) continue;
            float q = (float)(x * x + y * y);
            float de = (q - re * re) / (2.0f * re);
            if (de >= 0.5f) continue;
            float cob = de > -0.5f ? 0.5f - de : 1.0f;
            if (interior) {
                float di = (ri * ri - q) / (2.0f * ri);
                if (di >= 0.5f) continue;
                if (di > -0.5f) cob *= 0.5f - di;
            }
            p[fx] = sobre(p[fx], cubrir(c, (uint32_t)(cob * 255.0f)));
        }
    }
}

/* Burbuja de cristal: borde brillante, centro casi vacio y un reflejo arriba a la izquierda. */
static void burbuja(struct lienzo *l, int cx, int cy, int r, uint32_t tono) {
    float rf = (float)r;
    for (int y = -r; y <= r; y++) {
        int fy = cy + y;
        if (fy < 0 || fy >= l->al) continue;
        uint32_t *p = l->px + (uint64_t)fy * l->an;
        for (int x = -r; x <= r; x++) {
            int fx = cx + x;
            if (fx < 0 || fx >= l->an) continue;
            float q = (float)(x * x + y * y) / (rf * rf);
            if (q > 1.02f) continue;
            float borde = q * q * q * q;
            uint32_t a = (uint32_t)(18.0f + borde * 170.0f);
            if (a > 255) a = 255;
            float cob = q > 0.98f ? (1.02f - q) * 25.0f : 1.0f;
            float bx = (float)x / rf + 0.38f, by = (float)y / rf + 0.42f;
            float luz = 1.0f - (bx * bx + by * by) * 7.0f;
            uint32_t s = premul(a, tono);
            if (luz > 0) s = sobre(s, premul((uint32_t)(luz * 235.0f), 0xFFFFFFu));
            p[fx] = sobre(p[fx], cubrir(s, (uint32_t)(cob * 255.0f)));
        }
    }
}

/* Panel con dos esquinas cortadas a 45 grados, arriba a la derecha y abajo a la izquierda. */
static void chaflan(struct lienzo *l, int x, int y, int an, int al, int corte, uint32_t c) {
    for (int f = 0; f < al; f++) {
        int a = x, b = x + an;
        if (f < corte) b -= corte - f;
        if (f >= al - corte) a += f - (al - corte);
        rect(l, a, y + f, b - a, 1, c);
    }
}
/* Marcos de visor en las cuatro esquinas. */
static void esquinas(struct lienzo *l, int x, int y, int an, int al, int largo, int g, uint32_t c) {
    rect(l, x, y, largo, g, c); rect(l, x, y, g, largo, c);
    rect(l, x + an - largo, y, largo, g, c); rect(l, x + an - g, y, g, largo, c);
    rect(l, x, y + al - g, largo, g, c); rect(l, x, y + al - largo, g, largo, c);
    rect(l, x + an - largo, y + al - g, largo, g, c); rect(l, x + an - g, y + al - largo, g, largo, c);
}
/* Franjas diagonales de precaucion. */
static void peligro(struct lienzo *l, int x, int y, int an, int al, int paso, uint32_t a, uint32_t b) {
    for (int f = 0; f < al; f++) {
        int fy = y + f;
        if (fy < 0 || fy >= l->al) continue;
        uint32_t *p = l->px + (uint64_t)fy * l->an;
        for (int k = 0; k < an; k++) {
            int fx = x + k;
            if (fx >= 0 && fx < l->an) p[fx] = sobre(p[fx], ((k + f) / paso) & 1 ? a : b);
        }
    }
}

/* Desenfoque de caja de dos pasadas por eje: suficiente para un vidrio y barato, porque se hace una sola vez. */
static void desenfocar(struct lienzo *l, int radio) {
    static uint32_t linea[8192];
    int ven = 2 * radio + 1;
    for (int paso = 0; paso < 2; paso++) {
        for (int y = 0; y < l->al; y++) {
            uint32_t *p = l->px + (uint64_t)y * l->an, r = 0, g = 0, b = 0;
            for (int k = -radio; k <= radio; k++) { uint32_t v = p[k < 0 ? 0 : k >= l->an ? l->an - 1 : k]; r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; }
            for (int x = 0; x < l->an; x++) {
                linea[x] = 0xFF000000u | (r / (uint32_t)ven) << 16 | (g / (uint32_t)ven) << 8 | (b / (uint32_t)ven);
                uint32_t e = p[x + radio + 1 >= l->an ? l->an - 1 : x + radio + 1], s = p[x - radio < 0 ? 0 : x - radio];
                r += ((e >> 16) & 255) - ((s >> 16) & 255); g += ((e >> 8) & 255) - ((s >> 8) & 255); b += (e & 255) - (s & 255);
            }
            memcpy(p, linea, (size_t)l->an * 4);
        }
        for (int x = 0; x < l->an; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int k = -radio; k <= radio; k++) { uint32_t v = l->px[(uint64_t)(k < 0 ? 0 : k >= l->al ? l->al - 1 : k) * l->an + x]; r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; }
            for (int y = 0; y < l->al; y++) {
                linea[y] = 0xFF000000u | (r / (uint32_t)ven) << 16 | (g / (uint32_t)ven) << 8 | (b / (uint32_t)ven);
                uint32_t e = l->px[(uint64_t)(y + radio + 1 >= l->al ? l->al - 1 : y + radio + 1) * l->an + x], s = l->px[(uint64_t)(y - radio < 0 ? 0 : y - radio) * l->an + x];
                r += ((e >> 16) & 255) - ((s >> 16) & 255); g += ((e >> 8) & 255) - ((s >> 8) & 255); b += (e & 255) - (s & 255);
            }
            for (int y = 0; y < l->al; y++) l->px[(uint64_t)y * l->an + x] = linea[y];
        }
    }
}

/* Seno sin biblioteca: polinomio de Taylor tras llevar el angulo a [-pi, pi]. Sobra para agujas y curvas. */
static float sen(float x) {
    const float pi = 3.14159265f;
    while (x > pi) x -= 2 * pi;
    while (x < -pi) x += 2 * pi;
    float x2 = x * x;
    return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}
static float cosen(float x) { return sen(x + 1.57079633f); }

/* Largo aproximado de un vector: mayor + 0.43 * menor. Evita la raiz cuadrada. */
static inline float largo(float a, float b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    return a > b ? a + b * 0.43f : b + a * 0.43f;
}

/* Triangulo suavizado por distancia a sus tres lados. */
static void triangulo(struct lienzo *l, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c) {
    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (area == 0.0f) return;
    if (area < 0) { float t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    float ax = x1 - x0, ay = y1 - y0, bx = x2 - x1, by = y2 - y1, cx = x0 - x2, cy = y0 - y2;
    float la = largo(ax, ay), lb = largo(bx, by), lc = largo(cx, cy);
    float minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    float miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int xa = (int)minx - 1, xb = (int)maxx + 2, ya = (int)miny - 1, yb = (int)maxy + 2;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb > l->an) xb = l->an;
    if (yb > l->al) yb = l->al;
    for (int y = ya; y < yb; y++) {
        uint32_t *p = l->px + (uint64_t)y * l->an;
        float py = (float)y + 0.5f;
        for (int x = xa; x < xb; x++) {
            float px = (float)x + 0.5f;
            float d0 = (ax * (py - y0) - ay * (px - x0)) / la;
            float d1 = (bx * (py - y1) - by * (px - x1)) / lb;
            float d2 = (cx * (py - y2) - cy * (px - x2)) / lc;
            float d = d0 < d1 ? (d0 < d2 ? d0 : d2) : (d1 < d2 ? d1 : d2);
            if (d <= -0.5f) continue;
            p[x] = sobre(p[x], d >= 0.5f ? c : cubrir(c, (uint32_t)((d + 0.5f) * 255.0f)));
        }
    }
}

/* NekoSaka: el gato poligonal, por Neko. Cabeza de frente en facetas; `base` da la familia de tonos y `ojos` el acento. */
static void gato(struct lienzo *l, int cx, int cy, int tam, uint32_t base, uint32_t ojos) {
    /* Reticula de -100 a 100. A-I: contorno; P, Q, R, N: vertices interiores de las facetas. */
    enum { A, B, C, D, E, F, G, H, I, P, Q, R, N, NPUNTOS };
    static const signed char pts[NPUNTOS][2] = {
        { -78, -30 }, { -22, -52 }, { 22, -52 }, { 78, -30 }, { 88, 18 }, { 52, 66 }, { 0, 86 }, { -52, 66 }, { -88, 18 },
        { 0, -8 }, { -30, 22 }, { 30, 22 }, { 0, 34 } };
    /* Cuanta luz recibe cada faceta: la izquierda mira hacia arriba. 0 oscuro, 4 claro. */
    static const unsigned char facetas[][4] = {
        { B, C, P, 4 }, { A, B, P, 3 }, { D, C, P, 2 }, { A, P, Q, 2 }, { D, P, R, 1 }, { A, Q, I, 3 }, { D, R, E, 1 },
        { P, Q, N, 3 }, { P, R, N, 2 }, { I, Q, H, 1 }, { E, R, F, 0 }, { Q, N, H, 2 }, { R, N, F, 1 }, { H, N, G, 3 }, { F, N, G, 1 } };
    uint32_t tonos[5];
    for (int i = 0; i < 5; i++) tonos[i] = cubrir(base | 0xFF000000u, (uint32_t)(120 + i * 33)) | 0xFF000000u;
#define GX(v) ((float)cx + (float)(v) * (float)tam / 200.0f)
#define GY(v) ((float)cy + (float)(v) * (float)tam / 200.0f)
#define TRI(x0, y0, x1, y1, x2, y2, col) triangulo(l, GX(x0), GY(y0), GX(x1), GY(y1), GX(x2), GY(y2), col)
    /* Orejas: fuera, con su interior mas claro. */
    TRI(-78, -30, -70, -98, -22, -52, tonos[2]);
    TRI(78, -30, 70, -98, 22, -52, tonos[0]);
    TRI(-66, -44, -63, -80, -34, -52, tonos[4]);
    TRI(66, -44, 63, -80, 34, -52, tonos[3]);
    /* Una pasada de un solo tono tapa las rendijas que el suavizado deja entre facetas. */
    for (unsigned i = 0; i < sizeof(facetas) / sizeof(facetas[0]); i++) {
        const unsigned char *f = facetas[i];
        TRI(pts[f[0]][0], pts[f[0]][1], pts[f[1]][0], pts[f[1]][1], pts[f[2]][0], pts[f[2]][1], tonos[2]);
    }
    for (unsigned i = 0; i < sizeof(facetas) / sizeof(facetas[0]); i++) {
        const unsigned char *f = facetas[i];
        TRI(pts[f[0]][0], pts[f[0]][1], pts[f[1]][0], pts[f[1]][1], pts[f[2]][0], pts[f[2]][1], tonos[f[3]]);
    }
    /* Ojos rasgados con pupila de rendija, nariz y bigotes. */
    uint32_t co = ojos | 0xFF000000u, neg = 0xFF05050Au;
    TRI(-58, 0, -18, 8, -48, 20, co); TRI(58, 0, 18, 8, 48, 20, co);
    TRI(-41, 2, -35, 3, -38, 18, neg); TRI(41, 2, 35, 3, 38, 18, neg);
    TRI(-9, 30, 9, 30, 0, 41, 0xFFFF4A6Au);
    int g = tam / 90 > 0 ? tam / 90 : 1;
    linea(l, (int)GX(-55), (int)GY(40), (int)GX(-104), (int)GY(30), g, 0xFFF2F2F2u);
    linea(l, (int)GX(-52), (int)GY(50), (int)GX(-102), (int)GY(54), g, 0xFFF2F2F2u);
    linea(l, (int)GX(55), (int)GY(40), (int)GX(104), (int)GY(30), g, 0xFFF2F2F2u);
    linea(l, (int)GX(52), (int)GY(50), (int)GX(102), (int)GY(54), g, 0xFFF2F2F2u);
#undef TRI
#undef GX
#undef GY
}

/* ------------------------------------------------------------------ texto */

struct __attribute__((packed)) f_tam { uint32_t px, asc, alto, n, off; };
struct __attribute__((packed)) f_glifo { uint32_t cp; int16_t an, al, izq, arriba, avance, relleno; uint32_t off; };

/* Un programa puede definir FUENTE antes de incluir este archivo para usar otro atlas TF1 (o un puntero que cambie
 * de familia, como youtube.c con Chakra Petch y Share Tech Mono). */
#ifndef FUENTE
extern const uint8_t _binary_fuente_ui_bin_start[];
#define FUENTE _binary_fuente_ui_bin_start
#endif

static const struct f_tam *fuente_tam(int px) {
    uint32_t n = *(const uint32_t *)(const void *)(FUENTE + 4);
    const struct f_tam *t = (const struct f_tam *)(const void *)(FUENTE + 8), *mejor = t;
    int md = 1 << 30;
    for (uint32_t i = 0; i < n; i++) {
        int d = (int)t[i].px - px;
        if (d < 0) d = -d;
        if (d < md) { md = d; mejor = &t[i]; }
    }
    return mejor;
}

static const struct f_glifo *fuente_glifo(const struct f_tam *t, uint32_t cp) {
    const struct f_glifo *g = (const struct f_glifo *)(const void *)(FUENTE + t->off);
    int a = 0, b = (int)t->n - 1;
    while (a <= b) {
        int m = (a + b) / 2;
        if (g[m].cp == cp) return &g[m];
        if (g[m].cp < cp) a = m + 1; else b = m - 1;
    }
    return NULL;
}

static uint32_t utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    if (p[0] < 0x80) { *s += 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) { *s += 2; return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F); }
    *s += 1;
    return '?';
}

static int texto_ancho(int px, const char *s) {
    const struct f_tam *t = fuente_tam(px);
    int w = 0;
    while (*s) { const struct f_glifo *g = fuente_glifo(t, utf8(&s)); if (g) w += g->avance; }
    return w;
}

/* y es la linea base. sesgo: pixeles de inclinacion por cada 100 de alto. Devuelve la x final. */
static int texto(struct lienzo *l, int x, int y, int px, uint32_t rgb, uint32_t alfa, int sesgo, const char *s) {
    const struct f_tam *t = fuente_tam(px);
    uint32_t c = premul(alfa, rgb);
    while (*s) {
        const struct f_glifo *g = fuente_glifo(t, utf8(&s));
        if (!g) continue;
        const uint8_t *m = FUENTE + g->off;
        for (int f = 0; f < g->al; f++) {
            int fy = y - g->arriba + f;
            if (fy < 0 || fy >= l->al) continue;
            int despl = (g->arriba - f) * sesgo / 100;
            uint32_t *p = l->px + (uint64_t)fy * l->an;
            for (int k = 0; k < g->an; k++) {
                uint32_t a = m[(uint64_t)f * (uint64_t)g->an + (uint64_t)k];
                if (!a) continue;
                int fx = x + g->izq + k + despl;
                if (fx >= 0 && fx < l->an) p[fx] = sobre(p[fx], cubrir(c, a));
            }
        }
        x += g->avance;
    }
    return x;
}

/* Parrafo con corte en espacios. Devuelve la y de la linea siguiente. */
static int parrafo(struct lienzo *l, int x, int y, int ancho, int px, int alto, uint32_t rgb, uint32_t alfa, const char *s) {
    char b[160];
    while (*s) {
        int n = 0, ultimo = -1;
        while (s[n] && n + 1 < (int)sizeof(b)) {
            b[n] = s[n]; b[n + 1] = 0;
            if (texto_ancho(px, b) > ancho) break;
            if (s[n] == ' ') ultimo = n;
            n++;
        }
        if (s[n] && ultimo >= 0) n = ultimo;
        b[n] = 0;
        texto(l, x, y, px, rgb, alfa, 0, b);
        s += n;
        while (*s == ' ') s++;
        y += alto;
    }
    return y;
}

/* ------------------------------------------------------- texto a medida */
struct txt { char s[128]; int n; };
static void t_ini(struct txt *t) { t->n = 0; t->s[0] = 0; }
static void t_add(struct txt *t, const char *o) {
    while (*o && t->n + 1 < (int)sizeof(t->s)) t->s[t->n++] = *o++;
    t->s[t->n] = 0;
}
static void t_num(struct txt *t, uint64_t v) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i && t->n + 1 < (int)sizeof(t->s)) t->s[t->n++] = b[--i];
    t->s[t->n] = 0;
}
static void t_dos(struct txt *t, uint64_t v) { if (v < 10) t_add(t, "0"); t_num(t, v); }
static void t_hex4(struct txt *t, uint32_t v) {
    static const char h[] = "0123456789abcdef";
    char b[5] = { h[(v >> 12) & 15], h[(v >> 8) & 15], h[(v >> 4) & 15], h[v & 15], 0 };
    t_add(t, b);
}
#endif
