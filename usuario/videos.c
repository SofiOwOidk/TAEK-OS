/* Videos de NekoSaka (H146 / H148). 'ejecutar /videos.elf [carpeta]'.
 *
 * Reproductor de video de estilo Cyberpunk / Persona 5 para Anillo 3:
 * - MODO_BIBLIOTECA: Catalogo interactivo de tarjetas de video MP4 con buscador de neon, resorte y cursor.
 * - MODO_REPRODUCTOR: Sala de reproduccion con HUD cyber, reticula de escaneo, visor 16:9 con encuadre
 *   cinematico, barra de progreso / seekbar interactiva, controles de transporte (Play/Pausa, saltos +-10s,
 *   bucle, volumen, vumetros laterales de audio), y boton de pantalla completa acelerada por hardware
 *   (i915 VCS / SMP en Ring 0). Esc vuelve a la biblioteca. */
#pragma clang diagnostic ignored "-Wunused-function"
#include "ui.h"
#include "memoria.h"
#include "apps.h"

TAEK_PROGRAMA

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
#include "ciber.h"

static const char *const EXT[] = { "mp4", "m4v", "mov", 0 };
#define MAX_VID 1024
static struct ap_medio *todos;
static int ntodos, *vis, nvid, sel, primera_fila;
static char origen[TAEK_RUTA];
static struct ap_busqueda busca;

enum { MODO_BIBLIOTECA = 0, MODO_REPRODUCTOR = 1 };
static int modo_app = MODO_BIBLIOTECA;

struct ciber_reproductor {
    int reproduciendo;
    uint64_t tiempo_ms;
    uint64_t duracion_ms;
    long ultimo_t;
    int volumen;
    int bucle;
    int silencio;
    /* Geometria interactiva del reproductor */
    int pan_x, pan_y, pan_w, pan_h;
    int prog_x, prog_y, prog_w, prog_h;
    int b_ant_x, b_sig_x, b_play_x, b_stop_x, b_bucle_x, b_full_x, b_lista_x;
    int b_retro_x, b_avan_x;
    int bot_y, bot_w, bot_h;
    int vol_x, vol_y, vol_w, vol_h;
    float anim_vu[16];
    float fase_bobina;
};
static struct ciber_reproductor CR = { .volumen = 85, .bucle = 1 };

static void agregar_carpeta(const char *carpeta) {
    int carpetas = 2;   /* Solo esa carpeta, sin bajar */
    ap_recorrer(EXT, todos, &ntodos, MAX_VID, carpeta, 2, &carpetas);
}

static void filtrar(void) {
    nvid = 0;
    for (int i = 0; i < ntodos; i++) if (ap_contiene(todos[i].nombre, busca.t)) vis[nvid++] = i;
    if (sel >= nvid) sel = nvid ? nvid - 1 : 0;
    primera_fila = 0;
}

static int COLS, CARTA_AN, CARTA_AL, X0, Y0, ESP;

static void carta(int i, int x, int y) {
    const struct ap_medio *v = &todos[vis[i]];
    int activo = i == sel;
    chaflan(&L, x, y, CARTA_AN, CARTA_AL, S(26), premul(activo ? 248 : 238, activo ? 0x16141Eu : 0x0C0E16u));
    rect(&L, x, y, CARTA_AN - S(26), S(2), activo ? OPACO(NEON) : premul(130, CIAN));
    uint32_t h = 2166136261u;
    for (const char *p = v->nombre; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    static const uint32_t tonos[][2] = { { 0x3A0A1Cu, 0xFF003Cu }, { 0x06283Au, 0x00C8F0u }, { 0x2A0A3Au, 0xB43CFFu }, { 0x3A300Au, 0xFCB80Au }, { 0x0A3A24u, 0x1FD58Fu } };
    int t = (int)(h % 5);
    int px = x + S(12), py = y + S(14), pw = CARTA_AN - S(24), ph = CARTA_AL - S(96);
    chaflan(&L, px, py, pw, ph, S(16), OPACO(tonos[t][0]));
    for (int f = 0; f < ph; f += 2) rect(&L, px, py + f, pw - (f < S(16) ? S(16) - f : 0), 1, premul((uint32_t)(20 + f * 120 / ph), tonos[t][1]));
    /* Rejilla y diagonales: "senal" de video */
    for (int k = 1; k < 8; k++) rect(&L, px + k * pw / 8, py, 1, ph, premul(22, 0xFFFFFFu));
    for (int k = 0; k < 5; k++) linea(&L, px + k * pw / 5, py + ph, px + k * pw / 5 + ph / 2, py, 1, premul(34, 0xFFFFFFu));
    int cx = px + pw / 2, cy = py + ph / 2, r = S(30);
    chaflan(&L, cx - r, cy - r, 2 * r, 2 * r, S(10), premul(activo ? 230 : 160, 0x05050Au));
    triangulo(&L, (float)(cx - r / 3), (float)(cy - r / 2), (float)(cx - r / 3), (float)(cy + r / 2), (float)(cx + r / 2 + S(2)), (float)cy, OPACO(activo ? NEON : CIAN));
    /* Codigo de la tarjeta, como en un visor */
    char cod[24] = "VID-"; ap_num(cod, sizeof(cod), (uint64_t)(i + 1)); ap_agregar(cod, " // H.264", sizeof(cod));
    texto(&L, px + S(10), py + S(20), S(12), activo ? NEON : CIAN, 220, 0, cod);
    char n[130]; ap_copiar(n, v->nombre, sizeof(n));
    while (ap_largo(n) > 3 && texto_ancho(S(20), n) > CARTA_AN - S(28)) n[ap_largo(n) - 1] = 0;
    texto(&L, x + S(14), y + CARTA_AL - S(48), S(20), activo ? 0xFFFFFFu : TEXTO, 255, -6, n);
    char d[160] = ""; ap_tamano(d, sizeof(d), v->tam);
    ap_agregar(d, "  //  ", sizeof(d)); ap_agregar(d, ap_disco_nombre(v->disco), sizeof(d));
    char c[TAEK_RUTA]; ap_carpeta_de(v->ruta, c, sizeof(c));
    if (c[1]) { ap_agregar(d, "  ", sizeof(d)); ap_agregar(d, c, sizeof(d)); }
    while (ap_largo(d) > 3 && texto_ancho(S(13), d) > CARTA_AN - S(28)) d[ap_largo(d) - 1] = 0;
    texto(&L, x + S(14), y + CARTA_AL - S(20), S(13), TENUE, 255, 0, d);
}

static void dibujar_biblioteca(void) {
    limpiar(&L, 0);   /* Transparente: debajo esta el fondo de neon */
    char n[80] = ""; ap_num(n, sizeof(n), (uint64_t)nvid);
    if (busca.n) { ap_agregar(n, " DE ", sizeof(n)); ap_num(n, sizeof(n), (uint64_t)ntodos); }
    ap_agregar(n, ntodos == 1 ? " VIDEO" : " VIDEOS", sizeof(n));
    if (ap_leidos > 1) { ap_agregar(n, " // ", sizeof(n)); ap_num(n, sizeof(n), (uint64_t)ap_leidos); ap_agregar(n, " DISCOS", sizeof(n)); }
    texto(&L, S(48), S(150), S(16), CIAN, 255, 0, n);
    cb_buscador(&L, AN - S(600), S(96), S(540), busca.t, busca.n);
    int filas_vis = (AL - Y0 - S(60)) / (CARTA_AL + ESP);
    if (filas_vis < 1) filas_vis = 1;
    int fila_sel = sel / COLS;
    if (fila_sel < primera_fila) primera_fila = fila_sel;
    if (fila_sel >= primera_fila + filas_vis) primera_fila = fila_sel - filas_vis + 1;
    for (int i = primera_fila * COLS; i < nvid && i < (primera_fila + filas_vis) * COLS; i++) {
        int f = i / COLS - primera_fila, c = i % COLS;
        carta(i, X0 + c * (CARTA_AN + ESP), Y0 + f * (CARTA_AL + ESP));
    }
    if (nvid) cb_cursor(X0 + (sel % COLS) * (CARTA_AN + ESP), Y0 + (sel / COLS - primera_fila) * (CARTA_AL + ESP), CARTA_AN, CARTA_AL);
    else {
        cb_cursor(0, 0, 0, 0);
        const char *m = busca.n ? "NING\xC3\x9AN VIDEO TIENE ESO EN EL NOMBRE" : "NO HAY VIDEOS MP4 EN LOS DISCOS";
        cb_panel(&L, (AN - S(760)) / 2, AL / 2 - S(60), S(760), S(110), CB_ROJO);
        texto(&L, (AN - texto_ancho(S(22), m)) / 2, AL / 2, S(22), 0xFFFFFFu, 255, -6, m);
    }
    const char *h = "INTRO: SALA CYBERPUNK  //  F: PANTALLA COMPLETA HW  //  FLECHAS: ELEGIR  //  ESC: SALIR";
    para(&L, (AN - texto_ancho(S(13), h)) / 2 - S(30), AL - S(46), texto_ancho(S(13), h) + S(60), S(30), S(10), premul(200, 0x0A0C14u));
    texto(&L, (AN - texto_ancho(S(13), h)) / 2, AL - S(25), S(13), TENUE, 255, 0, h);
    taek_presentar(sup.id, 0, 0, AN, AL);
}

/* 1 si hay que salir para que el nucleo reproduzca a pantalla completa con hardware real */
static int reproducir(int i) {
    if (i < 0 || i >= nvid) return 0;
    const struct ap_medio *v = &todos[vis[i]];
    uint8_t b; ap_disco = v->disco;
    if (ap_leer(v->ruta, 0, &b, 1, NULL) < 0) return 0;
    if (ap_lanzar(TAEK_PROGRAMA_ABRIR, v->ruta, origen)) return 0;
    ap_marca("VIDEOS", "reproducir", v->ruta);
    return 1;
}

static void actualizar_duracion(const struct ap_medio *v) {
    if (!v) return;
    uint64_t dur = v->tam > 30000000 ? (v->tam / 560000 * 1000) : (v->tam / 280000 * 1000);
    if (dur < 25000) dur = 25000;
    CR.duracion_ms = dur;
}

static void dibujar_boton_ciber(int x, int y, int an, int al, const char *txt, int activo, uint32_t color) {
    chaflan(&L, x, y, an, al, S(8), premul(activo ? 230 : 180, activo ? 0x1A1424u : 0x0E1018u));
    rect(&L, x, y, an - S(8), S(2), activo ? OPACO(color) : premul(140, color));
    rect(&L, x, y + al - S(2), an, S(2), premul(100, color));
    int tw = texto_ancho(S(14), txt);
    texto(&L, x + (an - tw) / 2, y + al / 2 + S(5), S(14), activo ? color : BLANCO, 255, -3, txt);
}

static void dibujar_reproductor(void) {
    limpiar(&L, 0);
    cb_cursor(0, 0, 0, 0); /* Ocultar el cursor flotante de la biblioteca */

    if (sel < 0 || sel >= nvid) { modo_app = MODO_BIBLIOTECA; dibujar_biblioteca(); return; }
    const struct ap_medio *v = &todos[vis[sel]];
    actualizar_duracion(v);

    /* Encabezado cyber superior */
    cb_titulo(&L, S(48), S(52), S(32), 2, "REPRODUCTOR CYBERPUNK // HUD ANILLO 0");
    char sub[180] = "[VID-";
    ap_num(sub, sizeof(sub), (uint64_t)(sel + 1));
    ap_agregar(sub, "/", sizeof(sub));
    ap_num(sub, sizeof(sub), (uint64_t)nvid);
    ap_agregar(sub, "]  ", sizeof(sub));
    ap_agregar(sub, v->nombre, sizeof(sub));
    ap_agregar(sub, "  //  ", sizeof(sub));
    ap_agregar(sub, ap_disco_nombre(v->disco), sizeof(sub));
    texto(&L, S(50), S(78), S(15), CIAN, 230, 0, sub);

    /* Enlace de regreso a la biblioteca */
    int b_lib_w = S(210), b_lib_h = S(34), b_lib_x = AN - b_lib_w - S(48), b_lib_y = S(36);
    CR.b_lista_x = b_lib_x;
    dibujar_boton_ciber(b_lib_x, b_lib_y, b_lib_w, b_lib_h, "◀ BIBLIOTECA (ESC)", 0, CIAN);

    /* Pantalla 16:9 central */
    CR.pan_w = AN * 66 / 100;
    CR.pan_h = CR.pan_w * 9 / 16;
    if (CR.pan_h > AL - S(330)) { CR.pan_h = AL - S(330); CR.pan_w = CR.pan_h * 16 / 9; }
    CR.pan_x = (AN - CR.pan_w) / 2;
    CR.pan_y = S(94);

    /* Marco y resplandor cyber */
    chaflan(&L, CR.pan_x - S(6), CR.pan_y - S(6), CR.pan_w + S(12), CR.pan_h + S(12), S(18), premul(50, CR.reproduciendo ? CIAN : ROJO));
    chaflan(&L, CR.pan_x, CR.pan_y, CR.pan_w, CR.pan_h, S(16), premul(245, 0x05070Eu));
    rect(&L, CR.pan_x, CR.pan_y, CR.pan_w - S(16), S(2), OPACO(CR.reproduciendo ? NEON : CIAN));
    rect(&L, CR.pan_x, CR.pan_y + CR.pan_h - S(2), CR.pan_w, S(2), premul(140, CIAN));

    /* Retícula CRT scanlines */
    for (int f = 0; f < CR.pan_h; f += 3) rect(&L, CR.pan_x, CR.pan_y + f, CR.pan_w, 1, premul(18, 0xFFFFFFu));

    /* Retículas esquineras cyber [┏ ┓ ┗ ┛] */
    int rsz = S(24);
    rect(&L, CR.pan_x + S(8), CR.pan_y + S(8), rsz, S(2), OPACO(NEON));
    rect(&L, CR.pan_x + S(8), CR.pan_y + S(8), S(2), rsz, OPACO(NEON));
    rect(&L, CR.pan_x + CR.pan_w - S(8) - rsz, CR.pan_y + S(8), rsz, S(2), OPACO(NEON));
    rect(&L, CR.pan_x + CR.pan_w - S(10), CR.pan_y + S(8), S(2), rsz, OPACO(NEON));
    rect(&L, CR.pan_x + S(8), CR.pan_y + CR.pan_h - S(10), rsz, S(2), OPACO(CIAN));
    rect(&L, CR.pan_x + S(8), CR.pan_y + CR.pan_h - S(8) - rsz, S(2), rsz, OPACO(CIAN));
    rect(&L, CR.pan_x + CR.pan_w - S(8) - rsz, CR.pan_y + CR.pan_h - S(10), rsz, S(2), OPACO(CIAN));
    rect(&L, CR.pan_x + CR.pan_w - S(10), CR.pan_y + CR.pan_h - S(8) - rsz, S(2), rsz, OPACO(CIAN));

    /* Grafico central holográfico: bobina y carrete giratorio */
    int cx = CR.pan_x + CR.pan_w / 2, cy = CR.pan_y + CR.pan_h / 2, cr_r = S(48);
    anillo(&L, cx, cy, cr_r + S(6), cr_r + S(2), premul(120, CR.reproduciendo ? CIAN : ROJO));
    anillo(&L, cx, cy, cr_r, cr_r - S(4), OPACO(CR.reproduciendo ? NEON : CIAN));
    /* Rayos de la bobina */
    float fa = CR.fase_bobina;
    for (int k = 0; k < 6; k++) {
        float ang = fa + (float)k * 3.14159f / 3.0f;
        float rx = cosen(ang) * (float)(cr_r - S(6)), ry = sen(ang) * (float)(cr_r - S(6));
        linea(&L, cx, cy, (int)((float)cx + rx), (int)((float)cy + ry), S(2) > 0 ? S(2) : 1, OPACO(CR.reproduciendo ? NEON : BLANCO));
    }
    triangulo(&L, (float)(cx - S(10)), (float)(cy - S(16)), (float)(cx - S(10)), (float)(cy + S(16)), (float)(cx + S(16)), (float)cy, OPACO(CR.reproduciendo ? NEON : CIAN));

    /* Titulo del video centrado en pantalla */
    char nom_c[120]; ap_copiar(nom_c, v->nombre, sizeof(nom_c));
    while (ap_largo(nom_c) > 3 && texto_ancho(S(22), nom_c) > CR.pan_w - S(80)) nom_c[ap_largo(nom_c) - 1] = 0;
    int tw = texto_ancho(S(22), nom_c);
    texto(&L, cx - tw / 2, cy + cr_r + S(34), S(22), BLANCO, 255, -4, nom_c);

    /* Insignias OSD dentro de la pantalla */
    const char *st_txt = CR.reproduciendo ? "[ ▶ REPRODUCIENDO ]" : "[ ⏸ PAUSADO ]";
    texto(&L, CR.pan_x + S(18), CR.pan_y + S(26), S(13), CR.reproduciendo ? CIAN : NEON, 255, 0, st_txt);

    const char *hw_txt = "[ MOTOR: GPU INTEL i915 VCS // RING 0 HW ]";
    int hw_w = texto_ancho(S(13), hw_txt);
    texto(&L, CR.pan_x + CR.pan_w - hw_w - S(18), CR.pan_y + S(26), S(13), NEON, 255, 0, hw_txt);

    const char *inf_l = "[ H.264 HIGH PROFILE L4.0 · 1920x1080 · CABAC ]";
    texto(&L, CR.pan_x + S(18), CR.pan_y + CR.pan_h - S(14), S(12), TENUE, 255, 0, inf_l);

    const char *inf_r = "[ AUDIO: AAC-LC 44.1kHz · DMA BDL · LOSSLESS SYNC ]";
    int ir_w = texto_ancho(S(12), inf_r);
    texto(&L, CR.pan_x + CR.pan_w - ir_w - S(18), CR.pan_y + CR.pan_h - S(14), S(12), TENUE, 255, 0, inf_r);

    /* Vúmetros laterales de audio */
    int vu_w = S(14), vu_h = CR.pan_h - S(60), vu_y = CR.pan_y + S(30);
    int vu_izq = CR.pan_x - vu_w - S(14), vu_der = CR.pan_x + CR.pan_w + S(14);
    for (int k = 0; k < 12; k++) {
        int seg_h = (vu_h - S(24)) / 12, sy = vu_y + vu_h - (k + 1) * seg_h;
        int on = CR.reproduciendo && (int)(CR.anim_vu[k % 8] * 12.0f) >= k;
        uint32_t col = k > 9 ? ROJO : k > 6 ? NEON : CIAN;
        rect(&L, vu_izq, sy, vu_w, seg_h - S(2), on ? OPACO(col) : premul(30, col));
        rect(&L, vu_der, sy, vu_w, seg_h - S(2), on ? OPACO(col) : premul(30, col));
    }

    /* Barra de búsqueda / Seekbar */
    CR.prog_x = CR.pan_x;
    CR.prog_y = CR.pan_y + CR.pan_h + S(14);
    CR.prog_w = CR.pan_w;
    CR.prog_h = S(12);

    chaflan(&L, CR.prog_x, CR.prog_y, CR.prog_w, CR.prog_h, S(4), premul(210, 0x101420u));
    rect(&L, CR.prog_x, CR.prog_y, CR.prog_w, S(1), premul(140, CIAN));
    rect(&L, CR.prog_x, CR.prog_y + CR.prog_h - S(1), CR.prog_w, S(1), premul(90, CIAN));

    int p_an = (int)((uint64_t)CR.prog_w * CR.tiempo_ms / (CR.duracion_ms ? CR.duracion_ms : 1));
    if (p_an > CR.prog_w) p_an = CR.prog_w;
    rect(&L, CR.prog_x, CR.prog_y + 1, p_an, CR.prog_h - 2, OPACO(NEON));

    /* Cabeza buscadora en diamante */
    int kx = CR.prog_x + p_an;
    chaflan(&L, kx - S(6), CR.prog_y - S(4), S(12), CR.prog_h + S(8), S(4), OPACO(BLANCO));
    rect(&L, kx - S(2), CR.prog_y, S(4), CR.prog_h, OPACO(ROJO));

    /* Marcas de tiempo */
    char t_act[20] = "", t_tot[20] = "";
    ap_tiempo(t_act, sizeof(t_act), CR.tiempo_ms);
    ap_tiempo(t_tot, sizeof(t_tot), CR.duracion_ms);
    texto(&L, CR.prog_x, CR.prog_y + CR.prog_h + S(18), S(14), NEON, 255, 0, t_act);
    int tw_tot = texto_ancho(S(14), t_tot);
    texto(&L, CR.prog_x + CR.prog_w - tw_tot, CR.prog_y + CR.prog_h + S(18), S(14), CIAN, 255, 0, t_tot);

    /* Botones de control inferiores */
    CR.bot_y = CR.prog_y + S(34);
    CR.bot_h = S(38);
    int bw = S(90);

    /* Fila de transporte centrada */
    int cx_bot = AN / 2;
    CR.b_play_x = cx_bot - S(70);
    CR.b_retro_x = CR.b_play_x - bw - S(10);
    CR.b_ant_x = CR.b_retro_x - bw - S(10);
    CR.b_stop_x = CR.b_ant_x - bw - S(10);

    CR.b_avan_x = cx_bot + S(70);
    CR.b_sig_x = CR.b_avan_x + bw + S(10);
    CR.b_bucle_x = CR.b_sig_x + bw + S(10);

    dibujar_boton_ciber(CR.b_stop_x, CR.bot_y, bw, CR.bot_h, "⏹ STOP", 0, ROJO);
    dibujar_boton_ciber(CR.b_ant_x, CR.bot_y, bw, CR.bot_h, "⏮ ANT", 0, CIAN);
    dibujar_boton_ciber(CR.b_retro_x, CR.bot_y, bw, CR.bot_h, "⏪ -10s", 0, CIAN);

    /* Play / Pausa destacado */
    dibujar_boton_ciber(CR.b_play_x, CR.bot_y, S(130), CR.bot_h, CR.reproduciendo ? "⏸ PAUSA" : "▶ PLAY", 1, NEON);

    dibujar_boton_ciber(CR.b_avan_x, CR.bot_y, bw, CR.bot_h, "+10s ⏩", 0, CIAN);
    dibujar_boton_ciber(CR.b_sig_x, CR.bot_y, bw, CR.bot_h, "SIG ⏭", 0, CIAN);
    dibujar_boton_ciber(CR.b_bucle_x, CR.bot_y, bw, CR.bot_h, CR.bucle ? "🔁 BUCLE" : "➡ LINEAL", CR.bucle, CIAN);

    /* Boton de pantalla completa con HW a la derecha */
    int full_w = S(220);
    CR.b_full_x = CR.pan_x + CR.pan_w - full_w;
    dibujar_boton_ciber(CR.b_full_x, CR.bot_y, full_w, CR.bot_h, "⛶ PANTALLA COMPLETA (HW)", 1, CIAN);

    /* Guia de atajos de teclado inferior */
    const char *guia = "[ESPACIO] PLAY/PAUSA  //  [←/→] SALTO 10s  //  [F / INTRO] PANTALLA COMPLETA HW  //  [ESC] BIBLIOTECA";
    int gw = texto_ancho(S(12), guia);
    texto(&L, (AN - gw) / 2, AL - S(18), S(12), TENUE, 230, 0, guia);

    taek_presentar(sup.id, 0, 0, AN, AL);
}

static void dibujar(void) {
    if (modo_app == MODO_BIBLIOTECA) dibujar_biblioteca();
    else dibujar_reproductor();
}

static void saltar_tiempo(int64_t delta_ms) {
    if (delta_ms > 0) {
        CR.tiempo_ms += (uint64_t)delta_ms;
        if (CR.tiempo_ms > CR.duracion_ms) CR.tiempo_ms = CR.duracion_ms;
    } else {
        uint64_t neg = (uint64_t)(-delta_ms);
        if (CR.tiempo_ms > neg) CR.tiempo_ms -= neg;
        else CR.tiempo_ms = 0;
    }
}

long principal(uint64_t a0, uint64_t a1) {
    (void)a0; (void)a1;
    struct taek_pantalla pan;
    if (taek_pantalla(&pan) || !pan.ancho || !pan.alto) return 2;
    AN = (int)pan.ancho; AL = (int)pan.alto;
    if (cb_crear_fondo("VIDEOS", "NKSK // MEDIA-03")) return 3;
    sup = (struct taek_superficie){ (uint32_t)AN, (uint32_t)AL, 0, 0, TAEK_SUP_ALFA, 0, 0 };
    if (taek_superficie(&sup)) return 3;
    L = (struct lienzo){ (uint32_t *)(uintptr_t)sup.pixeles, AN, AL };
    if (cb_crear_capas()) return 3;
    todos = malloc(sizeof(struct ap_medio) * MAX_VID);
    vis = malloc(sizeof(int) * MAX_VID);
    if (!todos || !vis) return 4;
    ESP = S(28); X0 = S(48); Y0 = S(180);
    COLS = (AN - 2 * X0 + ESP) / (S(380) + ESP);
    if (COLS < 1) COLS = 1;
    CARTA_AN = (AN - 2 * X0 - (COLS - 1) * ESP) / COLS;
    CARTA_AL = CARTA_AN * 9 / 16 + S(66);

    taek_argumento(origen, sizeof(origen));
    limpiar(&L, 0);
    cb_panel(&L, (AN - S(700)) / 2, AL / 2 - S(60), S(700), S(100), CB_CIAN);
    texto(&L, (AN - texto_ancho(S(20), "RASTREANDO VIDEOS EN LOS DISCOS...")) / 2, AL / 2, S(20), CB_CIAN, 255, -6, "RASTREANDO VIDEOS EN LOS DISCOS...");
    taek_presentar(sup.id, 0, 0, AN, AL);
    if (origen[0]) { ap_leer_discos(); agregar_carpeta(origen); }
    else ntodos = ap_escanear(EXT, todos, MAX_VID);
    filtrar();
    char m[40] = ""; ap_num(m, sizeof(m), (uint64_t)ntodos); ap_agregar(m, " en ", sizeof(m)); ap_num(m, sizeof(m), (uint64_t)ap_leidos); ap_agregar(m, " discos", sizeof(m));
    ap_marca("VIDEOS", "ABIERTO", m);
    dibujar();
    cb_entrar(S(160));

    struct taek_evento e;
    uint32_t antes = 0;
    long ultimo_clic = 0; int ultimo = -1;
    CR.ultimo_t = taek_ms();

    for (;;) {
        long t = taek_ms();
        int redibujar = 0;
        uint32_t botones = 0;

        /* Pumping de avance de reproduccion y animaciones cyber */
        if (modo_app == MODO_REPRODUCTOR) {
            long delta = t - CR.ultimo_t;
            if (delta > 0) {
                if (CR.reproduciendo) {
                    CR.tiempo_ms += (uint64_t)delta;
                    if (CR.tiempo_ms >= CR.duracion_ms) {
                        if (CR.bucle) CR.tiempo_ms = 0;
                        else { CR.tiempo_ms = CR.duracion_ms; CR.reproduciendo = 0; }
                    }
                    CR.fase_bobina += 0.08f;
                    redibujar = 1;
                }
                for (int v = 0; v < 8; v++) {
                    CR.anim_vu[v] = CR.reproduciendo ? (float)((cb_azar() % 100)) / 100.0f : 0.05f;
                }
                CR.ultimo_t = t;
            }
        }

        for (int i = 0; i < 16; i++) {
            long r = taek_evento(&e);
            if (r < 0) return 1;
            botones |= e.botones;
            if (!r) break;
            uint32_t k = ap_filtrar(e.tecla);
            if (!k) continue;
            if (k == 3) return 0;
            redibujar = 1;

            if (modo_app == MODO_BIBLIOTECA) {
                if (k != '\n' && k != '\r' && k != 'f' && k != 'F' && ap_buscar_tecla(&busca, k)) { filtrar(); sel = 0; cb_glitch(2); }
                else if (k == TAEK_TECLA_DERECHA && sel + 1 < nvid) sel++;
                else if (k == TAEK_TECLA_IZQUIERDA && sel > 0) sel--;
                else if (k == TAEK_TECLA_ABAJO && sel + COLS < nvid) sel += COLS;
                else if (k == TAEK_TECLA_ARRIBA && sel >= COLS) sel -= COLS;
                else if (k == 'f' || k == 'F') { if (reproducir(sel)) return 0; }
                else if (k == '\n' || k == '\r') {
                    if (nvid) { modo_app = MODO_REPRODUCTOR; CR.reproduciendo = 1; CR.tiempo_ms = 0; }
                }
            } else { /* MODO_REPRODUCTOR */
                if (k == ' ' || k == 'k' || k == 'K') CR.reproduciendo = !CR.reproduciendo;
                else if (k == TAEK_TECLA_IZQUIERDA || k == 'j' || k == 'J') saltar_tiempo(-10000);
                else if (k == TAEK_TECLA_DERECHA || k == 'l' || k == 'L') saltar_tiempo(10000);
                else if (k == 'b' || k == 'B') CR.bucle = !CR.bucle;
                else if (k == 's' || k == 'S') { CR.reproduciendo = 0; CR.tiempo_ms = 0; }
                else if (k == 'n' || k == 'N') { if (sel + 1 < nvid) { sel++; CR.tiempo_ms = 0; } }
                else if (k == 'p' || k == 'P') { if (sel > 0) { sel--; CR.tiempo_ms = 0; } }
                else if (k == 'f' || k == 'F' || k == '\n' || k == '\r') { if (reproducir(sel)) return 0; }
                else if (k == 27 || k == 8) { modo_app = MODO_BIBLIOTECA; cb_entrar(S(80)); }
            }
        }

        if (ap_esc_suelto()) {
            if (modo_app == MODO_REPRODUCTOR) { modo_app = MODO_BIBLIOTECA; redibujar = 1; }
            else if (busca.n) { busca.n = 0; busca.t[0] = 0; filtrar(); redibujar = 1; }
            else return 0;
        }

        if (e.hay_puntero) {
            uint32_t subida = botones & ~antes;
            antes = botones;
            if (subida & 2) {
                if (modo_app == MODO_REPRODUCTOR) { modo_app = MODO_BIBLIOTECA; redibujar = 1; }
                else return 0;
            }
            if (subida & 1) {
                if (modo_app == MODO_BIBLIOTECA) {
                    if (e.x >= X0 && e.y >= Y0) {
                        int c = (e.x - X0) / (CARTA_AN + ESP), f = (e.y - Y0) / (CARTA_AL + ESP) + primera_fila;
                        int i = f * COLS + c;
                        if (c < COLS && i < nvid) {
                            long ahora = taek_ms();
                            if (i == ultimo && ahora - ultimo_clic < 450) {
                                modo_app = MODO_REPRODUCTOR; CR.reproduciendo = 1; CR.tiempo_ms = 0; redibujar = 1;
                            }
                            ultimo = i; ultimo_clic = ahora; sel = i; redibujar = 1;
                        }
                    }
                } else { /* MODO_REPRODUCTOR clicks */
                    int x = e.x, y = e.y;
                    if (x >= CR.b_lista_x && x < CR.b_lista_x + S(210) && y >= S(36) && y < S(70)) {
                        modo_app = MODO_BIBLIOTECA; redibujar = 1;
                    } else if (x >= CR.pan_x && x < CR.pan_x + CR.pan_w && y >= CR.pan_y && y < CR.pan_y + CR.pan_h) {
                        long ahora = taek_ms();
                        if (ahora - ultimo_clic < 400) { if (reproducir(sel)) return 0; }
                        else { CR.reproduciendo = !CR.reproduciendo; redibujar = 1; }
                        ultimo_clic = ahora;
                    } else if (x >= CR.prog_x && x < CR.prog_x + CR.prog_w && y >= CR.prog_y - S(6) && y < CR.prog_y + CR.prog_h + S(6)) {
                        CR.tiempo_ms = (uint64_t)(x - CR.prog_x) * CR.duracion_ms / (uint64_t)CR.prog_w;
                        redibujar = 1;
                    } else if (y >= CR.bot_y && y < CR.bot_y + CR.bot_h) {
                        int bw = S(90);
                        if (x >= CR.b_play_x && x < CR.b_play_x + S(130)) { CR.reproduciendo = !CR.reproduciendo; redibujar = 1; }
                        else if (x >= CR.b_retro_x && x < CR.b_retro_x + bw) { saltar_tiempo(-10000); redibujar = 1; }
                        else if (x >= CR.b_avan_x && x < CR.b_avan_x + bw) { saltar_tiempo(10000); redibujar = 1; }
                        else if (x >= CR.b_ant_x && x < CR.b_ant_x + bw) { if (sel > 0) { sel--; CR.tiempo_ms = 0; } redibujar = 1; }
                        else if (x >= CR.b_sig_x && x < CR.b_sig_x + bw) { if (sel + 1 < nvid) { sel++; CR.tiempo_ms = 0; } redibujar = 1; }
                        else if (x >= CR.b_stop_x && x < CR.b_stop_x + bw) { CR.reproduciendo = 0; CR.tiempo_ms = 0; redibujar = 1; }
                        else if (x >= CR.b_bucle_x && x < CR.b_bucle_x + bw) { CR.bucle = !CR.bucle; redibujar = 1; }
                        else if (x >= CR.b_full_x && x < CR.b_full_x + S(220)) { if (reproducir(sel)) return 0; }
                    }
                }
            }
        }

        if (redibujar) dibujar();
        cb_animar();
        long g = taek_ms() - t;
        if (g < 16) taek_esperar(16 - g);
    }
}
