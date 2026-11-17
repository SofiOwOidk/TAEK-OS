/* Visor de imagenes de NekoSaka (H146). 'ejecutar /imagenes.elf [ruta]'; el explorador y el escritorio lo abren solos.
 *
 * Decodifica en el propio programa con stb_image (JPEG, PNG, BMP, GIF: el primer cuadro), la misma biblioteca que usa
 * el nucleo para su visor de consola. Recorre las imagenes de la carpeta de la que se abrio.
 * Teclas: izquierda/derecha anterior y siguiente; + y - zoom (con zoom, arrastrar o flechas arriba/abajo mueven);
 * 0 vuelve a encajar; R rota; espacio inicia o para la presentacion; Esc sale. */
#pragma clang diagnostic ignored "-Wunused-function"
#include "ui.h"
#include "memoria.h"
#include "apps.h"

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
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_MAX_DIMENSIONS 16384
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#include "nucleo/controladores/multimedia/imagen/stb_image.h"
#pragma clang diagnostic pop

#define FONDO  0xFF07090Eu
#define BARRA  0xE0101520u
#define ROJO   0xFF003Cu
#define BLANCO 0xFFFFFFu
#define TENUE  0x8A93A6u
#define CIAN   0x5BE0FFu

static int AN, AL;
static int S(int v) { return v * AL / 1080; }
static struct taek_superficie sup;
static struct lienzo L;
#include "ciber.h"

static const char *const EXT[] = { "jpg", "jpeg", "png", "bmp", "gif", 0 };
#define MAX_IMG 2048
/* H148: sin argumento, las imagenes de todos los discos; con una carpeta o una imagen, esa carpeta del disco abierto */
static char carpeta[TAEK_RUTA];
static struct ap_medio *imgs;
static int nimg, actual;
/* Buscador: aparece al escribir, con las coincidencias en una lista; Intro abre la elegida */
static struct ap_busqueda busca;
static int *coinc, ncoinc, csel;
static void buscar(void) {
    ncoinc = 0;
    for (int i = 0; i < nimg; i++) if (ap_contiene(imgs[i].nombre, busca.t)) coinc[ncoinc++] = i;
    if (csel >= ncoinc) csel = ncoinc ? ncoinc - 1 : 0;
}

/* Imagen decodificada (ARGB opaco sobre el fondo) y su version a escala para la pantalla */
static uint32_t *img; static int iw, ih;
static uint32_t *vista; static int vw, vh;
static float zoom = 0;          /* 0: encajar */
static int px0, py0;            /* Desplazamiento con zoom */
static uint64_t img_bytes;
static char error[160];
static int presentacion; static long pres_t;

/* Solo esa carpeta, por nombre como el explorador */
static void listar(void) {
    nimg = 0;
    int carpetas = 2;
    ap_recorrer(EXT, imgs, &nimg, MAX_IMG, carpeta, 2, &carpetas);
    for (int i = 1; i < nimg; i++) {
        struct ap_medio t = imgs[i];
        int j = i;
        while (j > 0 && ap_comparar(imgs[j - 1].nombre, t.nombre) > 0) { imgs[j] = imgs[j - 1]; j--; }
        imgs[j] = t;
    }
}

static void soltar(void) {
    if (img) { free(img); img = NULL; }
    if (vista) { free(vista); vista = NULL; }
    iw = ih = vw = vh = 0;
}

static void cargar(int i) {
    soltar();
    error[0] = 0;
    if (!nimg) return;
    actual = (i + nimg) % nimg;
    zoom = 0; px0 = py0 = 0;
    const char *r = imgs[actual].ruta;
    ap_disco = imgs[actual].disco;
    uint64_t n = 0;
    uint8_t *datos = ap_leer_todo(r, &n, 96u << 20);
    if (!datos) { ap_copiar(error, "No se pudo leer el archivo (o pasa de 96 MB).", sizeof(error)); return; }
    img_bytes = n;
    int c;
    uint8_t *rgba = stbi_load_from_memory(datos, (int)n, &iw, &ih, &c, 4);
    free(datos);
    if (!rgba) { ap_copiar(error, "No se pudo decodificar: formato no admitido o archivo da\xC3\xB1" "ado.", sizeof(error)); iw = ih = 0; return; }
    img = malloc((size_t)iw * (size_t)ih * 4);
    if (!img) { free(rgba); ap_copiar(error, "No hay memoria para esta imagen.", sizeof(error)); iw = ih = 0; return; }
    /* RGBA a ARGB, con la transparencia mezclada sobre el fondo oscuro */
    for (uint64_t k = 0; k < (uint64_t)iw * (uint64_t)ih; k++) {
        uint32_t rr = rgba[k * 4], g = rgba[k * 4 + 1], b = rgba[k * 4 + 2], a = rgba[k * 4 + 3];
        uint32_t f = 0x0C;
        rr = (rr * a + f * (255 - a)) / 255; g = (g * a + f * (255 - a)) / 255; b = (b * a + f * (255 - a)) / 255;
        img[k] = 0xFF000000u | rr << 16 | g << 8 | b;
    }
    free(rgba);
    ap_marca("IMAGENES", "abierta", r);
}

static void rotar(void) {
    if (!img) return;
    uint32_t *n = malloc((size_t)iw * (size_t)ih * 4);
    if (!n) return;
    for (int y = 0; y < ih; y++) for (int x = 0; x < iw; x++) n[(uint64_t)x * ih + (ih - 1 - y)] = img[(uint64_t)y * iw + x];
    free(img); img = n;
    int t = iw; iw = ih; ih = t;
    if (vista) { free(vista); vista = NULL; }
}

static int AREA_X, AREA_Y, AREA_AN, AREA_AL;

/* Escala actual: encajar en el area (sin agrandar las pequenas) o el zoom elegido */
static float escala(void) {
    if (!iw) return 1;
    float ex = (float)AREA_AN / (float)iw, ey = (float)AREA_AL / (float)ih, e = ex < ey ? ex : ey;
    if (e > 1) e = 1;
    return zoom > 0 ? zoom : e;
}

/* Reducir promediando (cada pixel de salida es la media de su caja); agrandar con interpolacion bilineal */
static void escalar(void) {
    float e = escala();
    int nw = (int)((float)iw * e + 0.5f), nh = (int)((float)ih * e + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (vista && nw == vw && nh == vh) return;
    if (vista) free(vista);
    vista = malloc((size_t)nw * (size_t)nh * 4);
    if (!vista) { vw = vh = 0; return; }
    vw = nw; vh = nh;
    if (e <= 1.0f) {
        for (int y = 0; y < vh; y++) {
            int y0 = (int)((int64_t)y * ih / vh), y1 = (int)((int64_t)(y + 1) * ih / vh);
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < vw; x++) {
                int x0 = (int)((int64_t)x * iw / vw), x1 = (int)((int64_t)(x + 1) * iw / vw);
                if (x1 <= x0) x1 = x0 + 1;
                uint32_t r = 0, g = 0, b = 0, n = 0;
                int paso = (x1 - x0) * (y1 - y0) > 64 ? 2 : 1;   /* En cajas grandes, uno de cada dos basta */
                for (int yy = y0; yy < y1; yy += paso) {
                    const uint32_t *p = img + (uint64_t)yy * iw;
                    for (int xx = x0; xx < x1; xx += paso) { uint32_t c = p[xx]; r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255; n++; }
                }
                vista[(uint64_t)y * vw + x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
            }
        }
    } else {
        for (int y = 0; y < vh; y++) {
            float fy = ((float)y + 0.5f) / e - 0.5f; if (fy < 0) fy = 0;
            int y0 = (int)fy, y1 = y0 + 1 < ih ? y0 + 1 : y0; uint32_t ty = (uint32_t)((fy - (float)y0) * 256);
            for (int x = 0; x < vw; x++) {
                float fx = ((float)x + 0.5f) / e - 0.5f; if (fx < 0) fx = 0;
                int x0 = (int)fx, x1 = x0 + 1 < iw ? x0 + 1 : x0; uint32_t tx = (uint32_t)((fx - (float)x0) * 256);
                uint32_t a = img[(uint64_t)y0 * iw + x0], b = img[(uint64_t)y0 * iw + x1], c = img[(uint64_t)y1 * iw + x0], d = img[(uint64_t)y1 * iw + x1];
                uint32_t out = 0xFF000000u;
                for (int s = 0; s < 24; s += 8) {
                    uint32_t ca = a >> s & 255, cb = b >> s & 255, cc = c >> s & 255, cd = d >> s & 255;
                    uint32_t arriba = ca * (256 - tx) + cb * tx, abajo = cc * (256 - tx) + cd * tx;
                    out |= ((arriba * (256 - ty) + abajo * ty) >> 16) << s;
                }
                vista[(uint64_t)y * vw + x] = out;
            }
        }
    }
}

static void limitar_desplazamiento(void) {
    int mx = vw > AREA_AN ? (vw - AREA_AN) / 2 : 0, my = vh > AREA_AL ? (vh - AREA_AL) / 2 : 0;
    if (px0 > mx) px0 = mx;
    if (px0 < -mx) px0 = -mx;
    if (py0 > my) py0 = my;
    if (py0 < -my) py0 = -my;
}

static void dibujar(void) {
    limpiar(&L, 0);   /* Transparente: debajo esta el fondo de neon */
    if (img) {
        escalar();
        limitar_desplazamiento();
        int x = AREA_X + (AREA_AN - vw) / 2 + px0, y = AREA_Y + (AREA_AL - vh) / 2 + py0;
        int cx0 = x > AREA_X ? x : AREA_X, cy0 = y > AREA_Y ? y : AREA_Y;
        int cx1 = x + vw < AREA_X + AREA_AN ? x + vw : AREA_X + AREA_AN, cy1 = y + vh < AREA_Y + AREA_AL ? y + vh : AREA_Y + AREA_AL;
        /* Resplandor cian detras de la foto */
        for (int k = S(18); k > 0; k -= 3) rect(&L, cx0 - k, cy0 - k, cx1 - cx0 + 2 * k, cy1 - cy0 + 2 * k, premul(6, CB_CIAN));
        for (int yy = 0; yy < vh; yy++) {
            int sy = y + yy;
            if (sy < AREA_Y || sy >= AREA_Y + AREA_AL) continue;
            uint32_t *d = L.px + (uint64_t)sy * AN;
            const uint32_t *o = vista + (uint64_t)yy * vw;
            int a = x < AREA_X ? AREA_X - x : 0, b = x + vw > AREA_X + AREA_AN ? AREA_X + AREA_AN - x : vw;
            for (int xx = a; xx < b; xx++) d[x + xx] = o[xx];
            if (!(yy % 3)) for (int xx = a; xx < b; xx++) d[x + xx] = sobre(d[x + xx], premul(18, 0));   /* Lineas de monitor */
        }
        /* Marco de visor: esquinas amarillas, una marca roja y coordenadas */
        esquinas(&L, cx0 - S(12), cy0 - S(12), cx1 - cx0 + S(24), cy1 - cy0 + S(24), S(40), S(4), OPACO(CB_AMARILLO));
        rect(&L, cx0 - S(12), (cy0 + cy1) / 2 - S(14), S(4), S(28), OPACO(CB_ROJO));
        char c[60] = "X "; ap_num(c, sizeof(c), (uint64_t)(cx0 - AREA_X)); ap_agregar(c, " // Y ", sizeof(c)); ap_num(c, sizeof(c), (uint64_t)(cy0 - AREA_Y));
        texto(&L, cx0, cy1 + S(34), S(12), CB_CIAN, 200, 0, c);
    } else {
        const char *m = !nimg ? "NO HAY IM\xC3\x81GENES EN LOS DISCOS" : error[0] ? error : "";
        if (m[0]) {
            cb_panel(&L, (AN - S(820)) / 2, AL / 2 - S(60), S(820), S(110), CB_ROJO);
            texto(&L, (AN - texto_ancho(S(20), m)) / 2, AL / 2, S(20), 0xFFFFFFu, 255, -6, m);
        }
    }
    /* Ficha de la imagen, bajo el titulo */
    if (nimg) {
        char cod[40] = "IMG-"; ap_num(cod, sizeof(cod), (uint64_t)actual + 1); ap_agregar(cod, " / ", sizeof(cod)); ap_num(cod, sizeof(cod), (uint64_t)nimg);
        texto(&L, S(48), S(152), S(14), CB_AMARILLO, 255, 0, cod);
        char t[300]; ap_copiar(t, imgs[actual].nombre, sizeof(t));
        while (ap_largo(t) > 3 && texto_ancho(S(24), t) > AN - S(760)) t[ap_largo(t) - 1] = 0;
        texto(&L, S(48) + texto_ancho(S(14), cod) + S(24), S(154), S(24), 0xFFFFFFu, 255, -6, t);
        char d[200] = "";
        if (img) {
            ap_num(d, sizeof(d), (uint64_t)iw); ap_agregar(d, " x ", sizeof(d)); ap_num(d, sizeof(d), (uint64_t)ih);
            char b[24]; ap_tamano(b, sizeof(b), img_bytes); ap_agregar(d, "  //  ", sizeof(d)); ap_agregar(d, b, sizeof(d));
            ap_agregar(d, "  //  ZOOM ", sizeof(d)); ap_num(d, sizeof(d), (uint64_t)(escala() * 100 + 0.5f)); ap_agregar(d, " %", sizeof(d));
        }
        if (ap_leidos > 1) { ap_agregar(d, "  //  ", sizeof(d)); ap_agregar(d, ap_disco_nombre(imgs[actual].disco), sizeof(d)); }
        if (presentacion) ap_agregar(d, "  //  PRESENTACI\xC3\x93N", sizeof(d));
        texto(&L, AN - texto_ancho(S(14), d) - S(60), S(152), S(14), CB_CIAN, 255, 0, d);
    }
    const char *h = "ESCRIBE: BUSCAR  //  IZQ / DER: ANTERIOR Y SIGUIENTE  //  + / -: ZOOM  //  TAB: ROTAR  //  ESPACIO: PRESENTACI\xC3\x93N  //  ESC: SALIR";
    int hw = texto_ancho(S(12), h);
    para(&L, (AN - hw) / 2 - S(30), AL - S(46), hw + S(60), S(28), S(10), premul(200, 0x0A0C14u));
    texto(&L, (AN - hw) / 2, AL - S(26), S(12), CB_TENUE, 255, 0, h);
    /* El buscador, encima de todo mientras haya algo escrito; el cursor de neon marca la coincidencia */
    if (busca.n) {
        int filas = ncoinc < 10 ? ncoinc : 10, fa = S(46);
        int bw = S(820), bx = (AN - bw) / 2, by = S(200), bh = S(90) + (filas ? filas * fa + S(16) : S(56));
        rect(&L, 0, 0, AN, AL, premul(120, 0));
        cb_panel(&L, bx, by, bw, bh, CB_AMARILLO);
        cb_buscador(&L, bx + S(20), by + S(20), bw - S(40), busca.t, busca.n);
        char c[60] = ""; ap_num(c, sizeof(c), (uint64_t)ncoinc); ap_agregar(c, ncoinc == 1 ? " COINCIDENCIA" : " COINCIDENCIAS", sizeof(c));
        texto(&L, bx + bw - texto_ancho(S(12), c) - S(30), by + S(38), S(12), CB_CIAN, 255, 0, c);
        int primera = csel >= filas ? csel - filas + 1 : 0;
        for (int k = 0; k < filas; k++) {
            int i = coinc[primera + k], y = by + S(84) + k * fa;
            texto(&L, bx + S(40), y + S(29), S(20), primera + k == csel ? 0xFFFFFFu : CB_TEXTO, 255, -4, imgs[i].nombre);
            if (ap_leidos > 1) texto(&L, bx + bw - texto_ancho(S(12), ap_disco_nombre(imgs[i].disco)) - S(40), y + S(28), S(12), CB_TENUE, 255, 0, ap_disco_nombre(imgs[i].disco));
        }
        if (ncoinc) cb_cursor(bx + S(24), by + S(84) + (csel - primera) * fa, bw - S(48), fa - S(4));
        else { cb_cursor(0, 0, 0, 0); texto(&L, bx + S(40), by + S(120), S(16), CB_TENUE, 255, 0, "NINGUNA IMAGEN TIENE ESO EN EL NOMBRE"); }
    } else cb_cursor(0, 0, 0, 0);
    taek_presentar(sup.id, 0, 0, AN, AL);
}

static void cambiar_zoom(float factor) {
    float e = escala() * factor;
    if (e < 0.05f) e = 0.05f;
    if (e > 8) e = 8;
    zoom = e;
    px0 = (int)((float)px0 * factor); py0 = (int)((float)py0 * factor);
}

long principal(uint64_t a, uint64_t b) {
    (void)a; (void)b;
    struct taek_pantalla pan;
    if (taek_pantalla(&pan) || !pan.ancho || !pan.alto) return 2;
    AN = (int)pan.ancho; AL = (int)pan.alto;
    /* H148: fondo de neon, contenido transparente encima y las capas que se mueven arriba */
    if (cb_crear_fondo("IM\xC3\x81GENES", "NKSK // VISOR-01")) return 3;
    sup = (struct taek_superficie){ (uint32_t)AN, (uint32_t)AL, 0, 0, TAEK_SUP_ALFA, 0, 0 };
    if (taek_superficie(&sup)) return 3;
    L = (struct lienzo){ (uint32_t *)(uintptr_t)sup.pixeles, AN, AL };
    if (cb_crear_capas()) return 3;
    imgs = malloc(sizeof(*imgs) * MAX_IMG);
    coinc = malloc(sizeof(int) * MAX_IMG);
    if (!imgs || !coinc) return 4;
    AREA_X = S(60); AREA_Y = S(196); AREA_AN = AN - S(120); AREA_AL = AL - S(196) - S(90);

    /* Argumento: una imagen (se abre esa, y se recorre su carpeta), una carpeta, o nada */
    char arg[TAEK_RUTA] = "";
    taek_argumento(arg, sizeof(arg));
    int inicial = 0;
    limpiar(&L, 0);
    cb_panel(&L, (AN - S(700)) / 2, AL / 2 - S(60), S(700), S(100), CB_CIAN);
    texto(&L, (AN - texto_ancho(S(20), "RASTREANDO IM\xC3\x81GENES EN LOS DISCOS...")) / 2, AL / 2, S(20), CB_CIAN, 255, -6, "RASTREANDO IM\xC3\x81GENES EN LOS DISCOS...");
    taek_presentar(sup.id, 0, 0, AN, AL);
    if (arg[0] && ap_extension(arg, EXT)) {
        ap_leer_discos();
        ap_carpeta_de(arg, carpeta, sizeof(carpeta));
        listar();
        for (int i = 0; i < nimg; i++) if (!ap_comparar(imgs[i].ruta, arg)) inicial = i;
    } else if (arg[0]) { ap_leer_discos(); ap_copiar(carpeta, arg, sizeof(carpeta)); listar(); }
    else nimg = ap_escanear(EXT, imgs, MAX_IMG);
    cargar(inicial);
    char m[40] = ""; ap_num(m, sizeof(m), (uint64_t)nimg); ap_agregar(m, " en ", sizeof(m)); ap_num(m, sizeof(m), (uint64_t)ap_leidos); ap_agregar(m, " discos", sizeof(m));
    ap_marca("IMAGENES", "ABIERTO", arg[0] ? carpeta : m);
    dibujar();
    cb_entrar(S(160));

    struct taek_evento e;
    uint32_t antes = 0; int arr = 0, ax = 0, ay = 0;
    for (;;) {
        long t = taek_ms();
        int redibujar = 0, cambiar = 0, nueva = 0;
        uint32_t botones = 0;
        for (int i = 0; i < 16; i++) {
            long r = taek_evento(&e);
            if (r < 0) return 1;
            botones |= e.botones;
            if (!r) break;
            uint32_t k = ap_filtrar(e.tecla);
            if (!k) continue;
            if (k == 3) return 0;
            redibujar = 1;
            /* Con la busqueda vacia, espacio, + / - y 0 siguen siendo sus atajos; lo demas que se escribe, busca */
            int control = !busca.n && (k == ' ' || k == '+' || k == '=' || k == '-' || k == '0');
            if (!control && k != '\n' && k != '\r' && ap_buscar_tecla(&busca, k)) { csel = 0; buscar(); if (busca.n == 1) cb_glitch(3); }
            else if (busca.n && (k == '\n' || k == '\r')) {
                if (ncoinc) { cambiar = 1; nueva = coinc[csel]; }
                busca.n = 0; busca.t[0] = 0;
            }
            else if (busca.n && k == TAEK_TECLA_ARRIBA) { if (csel > 0) csel--; }
            else if (busca.n && k == TAEK_TECLA_ABAJO) { if (csel + 1 < ncoinc) csel++; }
            else if (k == TAEK_TECLA_DERECHA) { cambiar = 1; nueva = actual + 1; }
            else if (k == TAEK_TECLA_IZQUIERDA || k == 8 || k == 127) { cambiar = 1; nueva = actual - 1; }
            else if (k == '+' || k == '=') cambiar_zoom(1.25f);
            else if (k == '-') cambiar_zoom(0.8f);
            else if (k == '0') { zoom = 0; px0 = py0 = 0; }
            else if (k == 9) { rotar(); zoom = 0; }
            else if (k == ' ') { presentacion = !presentacion; pres_t = taek_ms(); }
            else if (k == TAEK_TECLA_ARRIBA) py0 += S(120);
            else if (k == TAEK_TECLA_ABAJO) py0 -= S(120);
        }
        if (ap_esc_suelto()) {
            if (!busca.n) return 0;
            busca.n = 0; busca.t[0] = 0; redibujar = 1;
        }
        /* Raton: arrastrar mueve; clic derecho cierra; clic en los bordes cambia de imagen */
        if (e.hay_puntero) {
            uint32_t subida = botones & ~antes;
            antes = botones;
            if (subida & 2) return 0;
            if (subida & 1) {
                if (e.x < AN / 8 && !zoom) { cambiar = 1; nueva = actual - 1; }
                else if (e.x > AN - AN / 8 && !zoom) { cambiar = 1; nueva = actual + 1; }
                else { arr = 1; ax = e.x; ay = e.y; }
            }
            if (!(botones & 1)) arr = 0;
            if (arr && (e.x != ax || e.y != ay)) { px0 += e.x - ax; py0 += e.y - ay; ax = e.x; ay = e.y; redibujar = 1; }
        }
        if (presentacion && nimg > 1 && taek_ms() - pres_t > 4000) { cambiar = 1; nueva = actual + 1; pres_t = taek_ms(); }
        if (cambiar) {
            texto(&L, AN - S(260), AL - S(70), S(16), CB_AMARILLO, 255, 0, "DECODIFICANDO...");
            taek_presentar(sup.id, AN - S(280), AL - S(100), S(280), S(40));
            /* La foto nueva llega deslizandose desde el lado hacia el que se avanza, con una rafaga */
            int adelante = nimg && (nueva == actual + 1 || (actual == nimg - 1 && nueva == nimg));
            cargar(nueva);
            redibujar = 1;
            dibujar(); redibujar = 0;
            cb_entrar(adelante ? S(140) : -S(140));
        }
        if (redibujar) dibujar();
        cb_animar();
        long g = taek_ms() - t;
        if (g < 16) taek_esperar(16 - g);
    }
}
