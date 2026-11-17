/* Escritorio Frutiger Aero. Se incluye desde menu.c. Todo se dibuja con codigo: ningun fondo ni logo ajeno.
 *
 * El vidrio es un truco: desenfocar en cada cuadro sin GPU es impagable. Se desenfoca el fondo una sola vez y cada ventana copia
 * el trozo que tiene detras, tenido de blanco. Consecuencia: el vidrio muestra el cielo, no la ventana de abajo. Con la capa 3D
 * de la GPU podria ser de verdad. */

#define BARRA_AL S(48)
#define BARRA_EXTRA S(24)   /* El orbe sobresale por arriba */
#define VENT_MARGEN S(12)   /* Sombra alrededor de la ventana */
#define VENT_TITULO S(38)
#define NVENT 8             /* H148: ventanas de aplicacion, en un grupo de superficies que se reutilizan */
#define NBURB 6
#define NINICIO 12
/* H146: iconos en columnas de 5, como en Windows. Los de aplicacion abren su programa del disco con SYS_LANZAR.
 * H151: los ocho del prototipo "Interfaz taek Os v1": Terminal, Navegador, Reproductor, Huevo, Discos, Stream, Ruleta y
 * Don Cangrejo. Imagenes, Musica, Equipo y Documentos quedan en el menu Inicio, como en el prototipo. */
#define NICONOS 8
#define ICONO_FILAS 5
#define ICONO_AN S(150)
#define ICONO_AL S(124)
#define ICONOS_Y S(62)   /* Debajo de la barra superior */
#define TOP_AL S(42)
enum { IC_EQUIPO, IC_BATERIA, IC_ACERCA, IC_MENU, IC_NAVEGADOR, IC_ARCHIVOS, IC_IMAGENES, IC_MUSICA, IC_VIDEOS, IC_PAPELERA,
       IC_TERMINAL, IC_HUEVO, IC_STREAM, IC_RULETA, IC_CANGREJO };
static const struct { const char *nombre; int dibujo; const char *programa, *argumento; } iconos[NICONOS] = {
    { "Terminal TAEK", IC_TERMINAL, 0, 0 }, { "Navegador Web", IC_NAVEGADOR, "/navegador.elf", 0 }, { "Reproductor", IC_VIDEOS, 0, 0 },
    { "El Huevo", IC_HUEVO, 0, 0 }, { "Discos y VFS", IC_ARCHIVOS, "/archivos.elf", 0 }, { "Stream LAN", IC_STREAM, 0, 0 },
    { "Ruleta Rusa", IC_RULETA, 0, 0 }, { "Don Cangrejo", IC_CANGREJO, 0, 0 } };
enum { VT_EQUIPO, VT_BATERIA, VT_ACERCA, VT_IMAGENES, VT_MUSICA, VT_VIDEOS, VT_TERMINAL, VT_HUEVO, VT_STREAM, VT_RULETA, NTIPOS };
static const char *const vt_titulo[NTIPOS] = { "Equipo", "Batería", "Acerca de TAEK OS", "Imágenes", "Música", "Videos",
                                               "Terminal TAEK", "El Huevo Guardián", "Stream LAN 9153", "Ruleta Rusa" };
static const int vt_icono[NTIPOS] = { IC_EQUIPO, IC_BATERIA, IC_ACERCA, IC_IMAGENES, IC_MUSICA, IC_VIDEOS, IC_TERMINAL, IC_HUEVO, IC_STREAM, IC_RULETA };
static int icono_x(int i) { return (i / ICONO_FILAS) * ICONO_AN; }
static int icono_y(int i) { return (i % ICONO_FILAS) * ICONO_AL; }
/* H151: modo de los iconos, como el selector del prototipo: abrir en una ventana o saltar al menu (QuickMenu) */
static int modo_quickmenu;
static long onda_t0;   /* La onda de choque del orbe de inicio */
static int saltar_menu(int seccion, int opcion);

static struct taek_superficie s_dfondo, s_dif, s_burb[NBURB], s_iconos, s_reloj, s_barra, s_ini, s_top, s_gad, s_onda;
static struct lienzo l_dfondo, l_dif, l_burb[NBURB], l_iconos, l_reloj, l_barra, l_ini, l_top, l_gad, l_onda;
#define RELOJ_Y S(54)
#define GAD_AN S(200)
#define GAD_AL S(520)
/* H148: cada ventana tiene su tipo (Equipo, Imagenes, Musica...), puede minimizarse, maximizarse y escalarse.
 * x, y y l.an, l.al son los de la superficie, sombra incluida; rx.. guardan la geometria de antes de maximizar. */
struct ventana { struct taek_superficie s; struct lienzo l; int creada, abierta, minimizada, maximizada, tipo, x, y, rx, ry, ran, ral; float anim; };
static struct ventana vent[NVENT];
static int z[NVENT], nz;            /* Ventanas abiertas, la ultima al frente; las minimizadas tambien */
static int esc_reabrir_videos;      /* Al volver de un video, abrir otra vez la ventana de Videos */
static int arr_redim = -1, redim_bordes, rd_x0, rd_y0, rd_vx, rd_vy, rd_an, rd_al;
static long rd_ultimo, tit_ultimo_clic; static int tit_ultima = -1;
struct burbuja_mov { float x, y, v, fase; };
static struct burbuja_mov burb[NBURB];
static int fondo_listo, icono_sel, inicio_abierto, inicio_sel;
static struct muelle m_ini;
static int arrastre = -1, arr_dx, arr_dy, arr_x, arr_y;
static long arr_ultimo;
static long esc_cuadro;

/* ------------------------------------------------------------------ fondo */

/* Cintas de aurora y colinas: las usan el cielo y las lineas que vuelan a dibujarlo. */
static const struct { float base, a1, f1, a2, f2, fase; int banda; uint32_t tono; } cintas[] = {
    { 0.40f, 90, 0.0040f, 34, 0.0110f, 0.6f, 100, 0xC8FFE8u }, { 0.52f, 70, 0.0052f, 26, 0.0095f, 2.4f, 70, 0xFFFFFFu }, { 0.30f, 50, 0.0031f, 20, 0.0130f, 4.1f, 56, 0x9FE8FFu } };
static const struct { float base, a1, f1, a2, f2, fase; uint32_t alto, bajo, brillo; } colinas[] = {
    { 0.70f, 56, 0.0032f, 18, 0.0091f, 1.0f, 0x7ADF72u, 0x2F9E58u, 0xC8FFB4u }, { 0.80f, 66, 0.0024f, 22, 0.0071f, 3.2f, 0xB4F268u, 0x1E8C38u, 0xF4FFC8u } };
static int colina_y(int k, int x) {
    float fx = (float)x * 1080.0f / (float)H;
    return (int)((float)H * colinas[k].base + (float)S((int)colinas[k].a1) * sen(fx * colinas[k].f1 + colinas[k].fase) + (float)S((int)colinas[k].a2) * sen(fx * colinas[k].f2));
}
static int cinta_y(int k, int x) {
    float fx = (float)x * 1080.0f / (float)H;
    return (int)((float)H * cintas[k].base + (float)S((int)cintas[k].a1) * sen(fx * cintas[k].f1 + cintas[k].fase) + (float)S((int)cintas[k].a2) * sen(fx * cintas[k].f2));
}

static void pintar_cielo(void) {
    struct lienzo *l = &l_dfondo;
    uint32_t c0 = OPACO(0x0A58B8u), c1 = OPACO(0x58B8F0u), c2 = OPACO(0xD6F4FFu);
    for (int y = 0; y < H; y++) {
        uint32_t t = (uint32_t)y * 510u / (uint32_t)H;
        uint32_t c = t < 255 ? entre(c0, c1, t) : entre(c1, c2, t - 255);
        uint32_t *p = l->px + (uint64_t)y * W;
        for (int x = 0; x < W; x++) p[x] = c;
    }
    /* El sol: un resplandor que se desvanece */
    float sx = (float)W * 0.20f, sy = (float)H * 0.14f, r2 = (float)S(900) * (float)S(900);
    for (int y = 0; y < H; y++) {
        uint32_t *p = l->px + (uint64_t)y * W;
        for (int x = 0; x < W; x++) {
            float dx = (float)x - sx, dy = (float)y - sy, d = (dx * dx + dy * dy) / r2;
            if (d >= 1.0f) continue;
            float f = (1.0f - d) * (1.0f - d);
            p[x] = sobre(p[x], premul((uint32_t)(f * f * 235.0f), BLANCO));
        }
    }
    /* Cintas de aurora */
    for (unsigned k = 0; k < 3; k++) {
        int banda = S(cintas[k].banda);
        for (int x = 0; x < W; x++) {
            int yc = cinta_y((int)k, x);
            for (int d = -banda; d <= banda; d++) {
                int y = yc + d;
                if (y < 0 || y >= H) continue;
                float f = 1.0f - (float)(d < 0 ? -d : d) / (float)banda;
                uint32_t *q = l->px + (uint64_t)y * W + x;
                *q = sobre(*q, premul((uint32_t)(f * f * 58.0f), cintas[k].tono));
            }
        }
    }
    /* Dos colinas: la de atras mas azulada, la de adelante con el borde encendido por el sol */
    for (unsigned k = 0; k < 2; k++) {
        for (int x = 0; x < W; x++) {
            int yc = colina_y((int)k, x);
            if (yc < 0) yc = 0;
            int alto = H - yc;
            for (int y = yc; y < H; y++) {
                uint32_t t = (uint32_t)(y - yc) * 255u / (uint32_t)(alto > 0 ? alto : 1);
                uint32_t c = entre(OPACO(colinas[k].alto), OPACO(colinas[k].bajo), t);
                /* Los primeros pixeles, mas claros: el borde de luz */
                int d = y - yc;
                if (d < S(10)) c = sobre(c, premul((uint32_t)(150 - d * 150 / (S(10) > 0 ? S(10) : 1)), colinas[k].brillo));
                l->px[(uint64_t)y * W + x] = c;
            }
        }
    }
    /* Burbujas quietas y destellos */
    for (int i = 0; i < 14; i++) {
        int bx = (int)(azar() % (uint32_t)W), by = (int)(azar() % (uint32_t)(H * 3 / 4)), br = S(14) + (int)(azar() % (uint32_t)(S(46) > 0 ? S(46) : 1));
        burbuja(l, bx, by, br, 0xE8FFFFu);
    }
    for (int i = 0; i < 26; i++) {
        int bx = (int)(azar() % (uint32_t)W), by = (int)(azar() % (uint32_t)(H * 4 / 5));
        int t = S(2) + (int)(azar() % 4u);
        rect(l, bx - t * 2, by, t * 4, 1, premul(180, BLANCO));
        rect(l, bx, by - t * 2, 1, t * 4, premul(180, BLANCO));
    }
}

/* -------------------------------------------------------------------- vidrio */

/* Copia el fondo desenfocado que queda detras de (x, y) con esquinas redondas. */
static void vidrio(struct lienzo *l, int x, int y, int an, int al, int r, int sx, int sy, uint32_t tinte) {
    for (int f = 0; f < al; f++) {
        int fy = y + f, oy = sy + f;
        if (fy < 0 || fy >= l->al) continue;
        if (oy < 0) oy = 0;
        if (oy >= H) oy = H - 1;
        uint32_t *p = l->px + (uint64_t)fy * l->an;
        const uint32_t *o = l_dif.px + (uint64_t)oy * W;
        int dy = f < r ? r - f : f >= al - r ? f - (al - r - 1) : 0;
        for (int k = 0; k < an; k++) {
            int fx = x + k, ox = sx + k;
            if (fx < 0 || fx >= l->an) continue;
            if (ox < 0) ox = 0;
            if (ox >= W) ox = W - 1;
            uint32_t c = sobre(o[ox], tinte);
            int dx = k < r ? r - k : k >= an - r ? k - (an - r - 1) : 0;
            if (dx && dy) {
                float d = (float)(dx * dx + dy * dy) - (float)(r * r), m = (float)(2 * r);
                if (d >= m * 0.5f) continue;
                if (d > -m * 0.5f) c = cubrir(c, (uint32_t)((0.5f - d / m) * 255.0f));
            }
            p[fx] = sobre(p[fx], c);
        }
    }
}

/* Texto blanco con halo oscuro, como en los titulos de vidrio. */
static void texto_halo(struct lienzo *l, int x, int y, int px, const char *s) {
    texto(l, x + 1, y + 1, px, 0x001020u, 150, 0, s);
    texto(l, x - 1, y + 1, px, 0x001020u, 70, 0, s);
    texto(l, x, y, px, BLANCO, 255, 0, s);
}

static void boton_brillante(struct lienzo *l, int x, int y, int an, int al, uint32_t arriba, uint32_t abajo) {
    redondo_deg(l, x, y, an, al, S(4), OPACO(arriba), OPACO(abajo));
    redondo_deg(l, x + 1, y + 1, an - 2, al / 2, S(3), premul(120, BLANCO), premul(20, BLANCO));
    redondo_borde(l, x, y, an, al, S(4), premul(140, 0x10203Au));
}

/* -------------------------------------------------------------------- iconos */

/* H148: los desplazamientos van en proporcion al tamano pedido (68 en 1080p), asi el icono sirve tambien en la barra
 * de titulo y en la de tareas. T(n) vale lo mismo que S(n) cuando t = S(68). */
#define T(n) ((int)((int64_t)(n) * t / 68))
static void icono(struct lienzo *l, int cx, int cy, int t, int tipo) {
    int m = t / 2;
    switch (tipo) {
    case 0: /* Equipo: pantalla con patas */
        redondo_deg(l, cx - m, cy - m, t, t * 3 / 4, T(6), OPACO(0xF4F8FFu), OPACO(0x9CB2CCu));
        redondo_deg(l, cx - m + T(6), cy - m + T(6), t - T(12), t * 3 / 4 - T(12), T(3), OPACO(0x62D0FFu), OPACO(0x0A4CA8u));
        para(l, cx - m + T(8), cy - m + T(8), t / 2, t / 4, T(10), premul(90, BLANCO));
        rect(l, cx - T(5), cy + t / 4, T(10), t / 8, OPACO(0x8094ACu));
        redondo_deg(l, cx - t / 4, cy + t / 4 + t / 8, t / 2, T(6), T(3), OPACO(0xE0E8F2u), OPACO(0x7C90A8u));
        break;
    case 1: /* Bateria: cilindro verde */
        redondo_deg(l, cx - m, cy - t / 4, t - T(6), t / 2, T(6), OPACO(0xA4F478u), OPACO(0x1E8C38u));
        redondo_deg(l, cx - m + T(3), cy - t / 4 + T(3), t - T(12), t / 5, T(4), premul(150, BLANCO), premul(20, BLANCO));
        redondo_deg(l, cx + m - T(6), cy - t / 8, T(8), t / 4, T(2), OPACO(0xE0E8F2u), OPACO(0x7C90A8u));
        redondo_borde(l, cx - m, cy - t / 4, t - T(6), t / 2, T(6), premul(160, 0x0A3A18u));
        break;
    case 2: /* Acerca de: Neko dentro de una burbuja */
        anillo(l, cx, cy, m, 0, premul(120, 0x7CD8FFu));
        gato(l, cx, cy + T(2), t * 7 / 10, 0x3C3650u, 0x7CFFB0u);
        burbuja(l, cx, cy, m, 0xE8FFFFu);
        break;
    case IC_NAVEGADOR: { /* Globo azul con meridianos y un brillo, el globo de toda la vida */
        redondo_deg(l, cx - m, cy - m, t, t, m, OPACO(0x7FD8FFu), OPACO(0x0A58C0u));
        int g = T(2) > 0 ? T(2) : 1;
        for (int k = -1; k <= 1; k++) linea(l, cx - m + T(6), cy + k * m / 2, cx + m - T(6), cy + k * m / 2, g, premul(150, 0xD8F4FFu));
        for (int k = 0; k < 24; k++) {   /* Dos meridianos: elipses angostas */
            float a0 = (float)k * 0.2618f, a1 = (float)(k + 1) * 0.2618f;
            for (int e = 1; e <= 2; e++) {
                float rx = (float)m * (e == 1 ? 0.38f : 0.75f);
                linea(l, cx + (int)(sen(a0) * rx), cy + (int)(cosen(a0) * (float)m), cx + (int)(sen(a1) * rx), cy + (int)(cosen(a1) * (float)m), g, premul(140, 0xD8F4FFu));
            }
        }
        linea(l, cx, cy - m, cx, cy + m, g, premul(150, 0xD8F4FFu));
        redondo_deg(l, cx - m * 2 / 3, cy - m + T(5), m * 4 / 3, m * 2 / 3, m / 3, premul(150, BLANCO), premul(0, BLANCO));
        anillo(l, cx, cy, m, m - T(2), premul(200, 0x0A3A80u));
        break;
    }
    case IC_ARCHIVOS: /* Carpeta amarilla, abierta un poco */
        redondo_deg(l, cx - m, cy - m + T(8), t * 2 / 5, T(14), T(4), OPACO(0xFFD86Au), OPACO(0xE8A21Cu));
        redondo_deg(l, cx - m, cy - m + T(16), t, t * 3 / 4 - T(8), T(6), OPACO(0xFFCE4Au), OPACO(0xD88A10u));
        redondo_deg(l, cx - m + T(4), cy - m + T(26), t - T(8), t / 3, T(5), OPACO(0xFFF4C8u), OPACO(0xFFDE7Au));
        redondo_deg(l, cx - m, cy - T(4), t, t / 2 - T(4), T(6), OPACO(0xFFDC64u), OPACO(0xE09616u));
        redondo_borde(l, cx - m, cy - m + T(16), t, t * 3 / 4 - T(8), T(6), premul(150, 0x7A4A00u));
        break;
    case IC_IMAGENES: /* Foto con marco blanco: cielo, sol y colinas */
        redondo_deg(l, cx - m, cy - m + T(6), t, t - T(12), T(5), OPACO(0xFFFFFFu), OPACO(0xD8E2EEu));
        degradado(l, cx - m + T(6), cy - m + T(12), t - T(12), t - T(24), OPACO(0x6CC8FFu), OPACO(0xD8F4FFu));
        anillo(l, cx + m / 2, cy - m / 3, T(8), 0, OPACO(0xFFE45Au));
        triangulo(l, (float)(cx - m + T(6)), (float)(cy + m - T(12)), (float)(cx - T(6)), (float)(cy - T(2)), (float)(cx + T(14)), (float)(cy + m - T(12)), OPACO(0x2FA84Au));
        triangulo(l, (float)(cx - T(4)), (float)(cy + m - T(12)), (float)(cx + m / 2), (float)(cy + T(4)), (float)(cx + m - T(6)), (float)(cy + m - T(12)), OPACO(0x63CF5Au));
        redondo_borde(l, cx - m, cy - m + T(6), t, t - T(12), T(5), premul(150, 0x5A6A7Cu));
        break;
    case IC_MUSICA: { /* Nota doble sobre un disco naranja */
        redondo_deg(l, cx - m, cy - m, t, t, m, OPACO(0xFFA24Au), OPACO(0xE0325Au));
        redondo_deg(l, cx - m * 2 / 3, cy - m + T(4), m * 4 / 3, m * 2 / 3, m / 3, premul(130, BLANCO), premul(0, BLANCO));
        int g = T(4) > 0 ? T(4) : 1;
        anillo(l, cx - T(10), cy + T(14), T(8), 0, OPACO(BLANCO));
        anillo(l, cx + T(14), cy + T(8), T(8), 0, OPACO(BLANCO));
        rect(l, cx - T(4), cy - T(18), g, T(32), OPACO(BLANCO));
        rect(l, cx + T(20), cy - T(24), g, T(32), OPACO(BLANCO));
        para(l, cx - T(4), cy - T(24), T(28), T(8), 0, OPACO(BLANCO));
        break;
    }
    case IC_VIDEOS: /* Pelicula con perforaciones y un boton de reproducir */
        redondo_deg(l, cx - m, cy - m + T(8), t, t - T(16), T(6), OPACO(0x4A4F66u), OPACO(0x15182Au));
        for (int k = 0; k < 5; k++) {
            int px = cx - m + T(6) + k * (t - T(12)) / 5;
            rect(l, px, cy - m + T(12), T(7), T(6), OPACO(0xE8ECF4u));
            rect(l, px, cy + m - T(18), T(7), T(6), OPACO(0xE8ECF4u));
        }
        triangulo(l, (float)(cx - T(10)), (float)(cy - T(14)), (float)(cx - T(10)), (float)(cy + T(14)), (float)(cx + T(16)), (float)cy, OPACO(0xFF3A5Au));
        redondo_deg(l, cx - m + T(3), cy - m + T(10), t - T(6), T(10), T(4), premul(90, BLANCO), premul(0, BLANCO));
        break;
    case IC_PAPELERA: { /* Cubo de vidrio con tapa y estrias, el de toda la vida */
        int ab = t * 3 / 4, ar = t * 5 / 8;   /* El cuerpo se angosta hacia abajo */
        for (int y = 0; y < t * 3 / 4; y++) {
            int w = ab - (ab - ar) * y / (t * 3 / 4);
            uint32_t c = premul((uint32_t)(150 + 60 * y / (t * 3 / 4)), y < t / 4 ? 0xEAF4FFu : 0xB8D0E8u);
            rect(l, cx - w / 2, cy - m + T(12) + y, w, 1, c);
        }
        redondo_deg(l, cx - t * 2 / 5, cy - m + T(4), t * 4 / 5, T(12), T(5), OPACO(0xF4F8FFu), OPACO(0x9CB2CCu));
        redondo_deg(l, cx - T(9), cy - m, T(18), T(7), T(3), OPACO(0xE0E8F2u), OPACO(0x8094ACu));
        for (int k = -1; k <= 1; k++) linea(l, cx + k * T(10), cy - m + T(22), cx + k * T(8), cy + m - T(8), T(2) > 0 ? T(2) : 1, premul(150, 0x5A7898u));
        redondo_borde(l, cx - t * 2 / 5, cy - m + T(4), t * 4 / 5, T(12), T(5), premul(160, 0x3A5A7Au));
        break;
    }
    /* H151: los iconos nuevos del prototipo, dibujados aqui (sin imagenes ajenas) */
    case IC_TERMINAL: /* Pantalla negra con el prompt */
        redondo_deg(l, cx - m, cy - m + T(6), t, t - T(12), T(7), OPACO(0x4A5468u), OPACO(0x161A24u));
        redondo_deg(l, cx - m + T(5), cy - m + T(11), t - T(10), t - T(22), T(4), OPACO(0x0A0D12u), OPACO(0x05070Au));
        texto(l, cx - m + T(10), cy + T(8), T(24) > 14 ? T(24) : 14, 0x7CFF9Au, 255, 0, "$_");
        redondo_deg(l, cx - m + T(3), cy - m + T(8), t - T(6), T(10), T(4), premul(90, BLANCO), premul(0, BLANCO));
        break;
    case IC_HUEVO: { /* Un huevo dorado con su brillo */
        for (int y = -m; y <= m; y++) {
            float ny = (float)y / (float)m, r = (float)m * (0.66f + 0.14f * ny) * (1.0f - ny * ny * 0.15f);
            float w2 = r * r * (1.0f - ny * ny);
            if (w2 <= 0) continue;
            int w = 0; while ((float)(w * w) < w2) w++;
            uint32_t c = entre(OPACO(0xFFF3B0u), OPACO(0xE0A21Cu), (uint32_t)((y + m) * 255 / (2 * m)));
            rect(l, cx - w, cy + y, 2 * w, 1, c);
        }
        anillo(l, cx - m / 4, cy - m / 3, m / 4, 0, premul(150, BLANCO));
        break;
    }
    case IC_STREAM: { /* Antena con ondas */
        anillo(l, cx, cy - T(4), m, m - T(4), premul(200, 0x38BDF8u));
        anillo(l, cx, cy - T(4), m * 2 / 3, m * 2 / 3 - T(4), premul(220, 0x7DD3FCu));
        anillo(l, cx, cy - T(4), T(8), 0, OPACO(0xFF003Cu));
        rect(l, cx - T(3), cy, T(6), m, OPACO(0xE0E8F2u));
        triangulo(l, (float)(cx - T(16)), (float)(cy + m), (float)(cx + T(16)), (float)(cy + m), (float)cx, (float)(cy + T(6)), premul(200, 0x8094ACu));
        break;
    }
    case IC_RULETA: /* El tambor: seis recamaras, una roja */
        anillo(l, cx, cy, m, 0, OPACO(0x2A2A2Au));
        anillo(l, cx, cy, m, m - T(3), OPACO(0xFCEE0Au));
        for (int k = 0; k < 6; k++) {
            float a = (float)k * 1.0471976f;
            anillo(l, cx + (int)(cosen(a) * (float)m * 0.6f), cy + (int)(sen(a) * (float)m * 0.6f), T(8), 0, k ? OPACO(0x555555u) : OPACO(0xFF003Cu));
        }
        anillo(l, cx, cy, T(6), 0, OPACO(0x111111u));
        break;
    case IC_CANGREJO: /* Don Cangrejo: caparazon rojo, ojos y pinzas */
        redondo_deg(l, cx - m * 3 / 4, cy - T(6), m * 3 / 2, m, m / 2, OPACO(0xFF5A3Cu), OPACO(0xB01810u));
        anillo(l, cx - T(12), cy - T(16), T(7), 0, OPACO(BLANCO)); anillo(l, cx + T(12), cy - T(16), T(7), 0, OPACO(BLANCO));
        anillo(l, cx - T(12), cy - T(16), T(3), 0, OPACO(0x111111u)); anillo(l, cx + T(12), cy - T(16), T(3), 0, OPACO(0x111111u));
        anillo(l, cx - m + T(4), cy - T(14), T(11), T(5), OPACO(0xE0321Eu)); anillo(l, cx + m - T(4), cy - T(14), T(11), T(5), OPACO(0xE0321Eu));
        for (int k = -1; k <= 1; k += 2) for (int p = 0; p < 3; p++) linea(l, cx + k * (m / 2), cy + T(10) + p * T(6), cx + k * (m - T(2)), cy + T(16) + p * T(8), T(3) > 0 ? T(3) : 1, OPACO(0xB01810u));
        break;
    default: /* Volver al menu: flechas en rojo */
        redondo_deg(l, cx - m, cy - m + T(4), t, t - T(8), T(8), OPACO(0xFF6A5Au), OPACO(0xB00014u));
        redondo_deg(l, cx - m + T(3), cy - m + T(7), t - T(6), t / 3, T(5), premul(120, BLANCO), premul(10, BLANCO));
        triangulo(l, (float)(cx + T(14)), (float)(cy - T(16)), (float)(cx - T(14)), (float)cy, (float)(cx + T(14)), (float)(cy + T(16)), OPACO(BLANCO));
        break;
    }
}
#undef T

static void pintar_iconos(void) {
    struct lienzo *l = &l_iconos;
    limpiar(l, 0);
    for (int i = 0; i < NICONOS; i++) {
        int x = icono_x(i), y = icono_y(i);
        if (i == icono_sel) {
            redondo_deg(l, x + S(4), y + S(2), ICONO_AN - S(8), S(118), S(5), premul(90, 0xBEE8FFu), premul(46, 0x7CC8FFu));
            redondo_borde(l, x + S(4), y + S(2), ICONO_AN - S(8), S(118), S(5), premul(170, BLANCO));
        }
        icono(l, x + ICONO_AN / 2, y + S(50), S(68), iconos[i].dibujo);
        int w = texto_ancho(S(18), iconos[i].nombre);
        texto_halo(l, x + (ICONO_AN - w) / 2, y + S(110), S(18), iconos[i].nombre);
    }
    taek_presentar(s_iconos.id, 0, 0, l->an, l->al);
}

/* H146: abrir una aplicacion del disco. El menu termina con TAEK_ACCION_APP y el nucleo la corre; al cerrarla, vuelve
 * a abrir el menu directo en el escritorio. */
static int esc_lanzar(const char *programa, const char *argumento) {
    static struct taek_lanzar l;
    memset(&l, 0, sizeof(l));
    for (int i = 0; programa[i] && i < TAEK_RUTA - 1; i++) l.programa[i] = programa[i];
    for (int i = 0; argumento && argumento[i] && i < TAEK_RUTA - 1; i++) l.argumento[i] = argumento[i];
    return taek_lanzar(&l) ? 0 : TAEK_ACCION_APP;
}

/* ------------------------------------------------------------------ reloj */

static void pintar_reloj(void) {
    struct lienzo *l = &l_reloj;
    limpiar(l, 0);
    int c = l->an / 2, r = l->an / 2 - S(8);
    int rect_x = W - l->an - S(20), rect_y = RELOJ_Y;
    /* Disco de vidrio: el fondo desenfocado detras, con un velo claro */
    for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {
        int q = x * x + y * y;
        if (q > r * r) continue;
        int ox = rect_x + c + x, oy = rect_y + c + y;
        uint32_t v = sobre(l_dif.px[(uint64_t)oy * W + ox], premul(70, BLANCO));
        l->px[(uint64_t)(c + y) * l->an + c + x] = v;
    }
    anillo(l, c, c, r, r - S(3), premul(220, BLANCO));
    anillo(l, c, c, r - S(3), r - S(4), premul(90, 0x0A3060u));
    for (int i = 0; i < 12; i++) {
        float a = (float)i * 0.5235988f;
        int rd = i % 3 ? r - S(12) : r - S(18);
        linea(l, c + (int)(sen(a) * (float)rd), c - (int)(cosen(a) * (float)rd), c + (int)(sen(a) * (float)(r - S(7))), c - (int)(cosen(a) * (float)(r - S(7))), i % 3 ? 1 : (S(3) > 0 ? S(3) : 1), OPACO(0x0A2A50u));
    }
    if (info.fecha_ok) {
        float ah = ((float)(info.hora % 12) + (float)info.minuto / 60.0f) * 0.5235988f;
        float am = ((float)info.minuto + (float)info.segundo / 60.0f) * 0.1047198f;
        float as = (float)info.segundo * 0.1047198f;
        linea(l, c, c, c + (int)(sen(ah) * (float)r * 0.5f), c - (int)(cosen(ah) * (float)r * 0.5f), S(5), OPACO(0x0A2A50u));
        linea(l, c, c, c + (int)(sen(am) * (float)r * 0.72f), c - (int)(cosen(am) * (float)r * 0.72f), S(3), OPACO(0x0A2A50u));
        linea(l, c, c, c + (int)(sen(as) * (float)r * 0.8f), c - (int)(cosen(as) * (float)r * 0.8f), 1, OPACO(0xE01A2Au));
    }
    anillo(l, c, c, S(5), 0, OPACO(0xE01A2Au));
    /* Reflejo de la mitad superior */
    for (int y = -r + S(4); y < 0; y++) for (int x = -r; x <= r; x++) {
        float nx = (float)x / (float)r, ny = (float)y / (float)r + 0.45f;
        if (nx * nx + ny * ny * 3.0f > 0.72f) continue;
        uint32_t *p = l->px + (uint64_t)(c + y) * l->an + c + x;
        *p = sobre(*p, premul(34, BLANCO));
    }
    taek_presentar(s_reloj.id, 0, 0, l->an, l->al);
}

/* ------------------------------------------------- H151: barra superior y gadgets */

/* Los botones de la barra superior: modo ventana, modo QuickMenu, abrir QuickMenu */
static int top_x[3], top_an[3];
static void pintar_top(void) {
    struct lienzo *l = &l_top;
    limpiar(l, 0);
    vidrio(l, 0, 0, W, TOP_AL, 0, 0, 0, premul(80, 0x0E3A66u));
    degradado(l, 0, 0, W, TOP_AL / 2, premul(80, BLANCO), premul(10, BLANCO));
    rect(l, 0, TOP_AL - 1, W, 1, premul(170, BLANCO));
    texto_halo(l, S(18), S(28), S(16), "TAEK OS // ESCRITORIO FRUTIGER AERO (RING 3)");
    static const char *const et[3] = { "Ventana en escritorio", "Salto a QuickMenu", "ABRIR QUICKMENU [ESC]" };
    int x = W / 2 - S(150);
    texto_halo(l, x - texto_ancho(S(15), "Abrir apps en:") - S(12), S(27), S(15), "Abrir apps en:");
    for (int i = 0; i < 3; i++) {
        int w = texto_ancho(S(15), et[i]) + S(30);
        if (i == 2) x = W - GAD_AN - S(70) - w;
        top_x[i] = x; top_an[i] = w;
        int activo = (i == 0 && !modo_quickmenu) || (i == 1 && modo_quickmenu);
        if (i == 2) {
            redondo_deg(l, x, S(6), w, TOP_AL - S(12), S(4), premul(200, 0x14081Eu), premul(230, 0x06030Au));
            redondo_borde(l, x, S(6), w, TOP_AL - S(12), S(4), premul(220, 0x00F0FFu));
            rect(l, x + S(5), S(10), S(3), TOP_AL - S(20), OPACO(0xFF003Cu));
            texto(l, x + S(16), S(27), S(14), 0x00F0FFu, 255, 0, et[i]);
        } else {
            redondo_deg(l, x, S(6), w, TOP_AL - S(12), S(4), activo ? premul(220, 0xBEE8FFu) : premul(60, BLANCO), activo ? premul(170, 0x4CA8E8u) : premul(20, BLANCO));
            redondo_borde(l, x, S(6), w, TOP_AL - S(12), S(4), premul(activo ? 230 : 120, BLANCO));
            if (activo) texto(l, x + S(15), S(27), S(15), 0x0A2A50u, 255, 0, et[i]); else texto_halo(l, x + S(15), S(27), S(15), et[i]);
        }
        x += w + S(8);
    }
    taek_presentar(s_top.id, 0, 0, W, TOP_AL);
}

/* Medidor circular de vidrio, como los del prototipo */
static void gad_medidor(struct lienzo *l, int cx, int cy, int r, int pct, uint32_t tono, const char *k) {
    anillo(l, cx, cy, r, r - S(8), premul(90, BLANCO));
    for (int i = 0; i < 60; i++) {
        if (pct < 0 || i * 100 / 60 >= pct) break;
        float a = -1.5707963f + (float)i * 0.104719755f, b = a + 0.1f;
        trazo(l, (float)cx + cosen(a) * (float)(r - S(4)), (float)cy + sen(a) * (float)(r - S(4)), (float)cx + cosen(b) * (float)(r - S(4)), (float)cy + sen(b) * (float)(r - S(4)), SF(7), OPACO(tono), 0);
    }
    struct txt t; t_ini(&t);
    if (pct >= 0) { t_num(&t, (uint64_t)pct); t_add(&t, "%"); } else t_add(&t, "--");
    texto_halo(l, cx - texto_ancho(S(18), t.s) / 2, cy + S(7), S(18), t.s);
    texto_halo(l, cx - texto_ancho(S(12), k) / 2, cy + r + S(18), S(12), k);
}

static void pintar_gadgets(void) {
    struct lienzo *l = &l_gad;
    limpiar(l, 0);
    int gx = W - GAD_AN - S(20), gy = RELOJ_Y + S(206);
    struct txt t;
    /* 1. Medidores de silicio: carga de la CPU 0 (medida) y RAM */
    vidrio(l, 0, 0, GAD_AN, S(170), S(10), gx, gy, premul(70, 0xDCF0FFu));
    redondo_borde(l, 0, 0, GAD_AN, S(170), S(10), premul(200, BLANCO));
    texto_halo(l, S(14), S(24), S(12), "MEDIDORES DE SILICIO");
    int cpu = sen_carga_ok ? (int)(sen_carga[0] * 100.0f + 0.5f) : -1;
    uint64_t usada = info.ram_total > info.ram_libre ? info.ram_total - info.ram_libre : 0;
    gad_medidor(l, S(52), S(84), S(34), cpu, 0x38BDF8u, "CPU 0");
    gad_medidor(l, GAD_AN - S(52), S(84), S(34), info.ram_total ? (int)(usada * 100 / info.ram_total) : -1, 0x4ADE80u, "RAM");
    /* 2. Clima del silicio: la temperatura real (el prototipo traia un clima de muestra) */
    int y = S(184);
    vidrio(l, 0, y, GAD_AN, S(120), S(10), gx, gy + y, premul(70, 0xFFF4D6u));
    redondo_borde(l, 0, y, GAD_AN, S(120), S(10), premul(200, BLANCO));
    int c = sns.temp_paquete_ok ? sns.temp_paquete_c : sns.temp_nucleo_c, hay = sen_hay() && (sns.temp_ok || sns.temp_paquete_ok);
    anillo(l, S(40), y + S(52), S(22), 0, OPACO(hay ? (c >= 90 ? 0xFF003Cu : c >= 65 ? 0xFFAA00u : 0xFFD24Au) : 0xC8D0D8u));
    anillo(l, S(34), y + S(46), S(8), 0, premul(170, BLANCO));
    t_ini(&t); if (hay) { t_num(&t, (uint64_t)(c > 0 ? c : 0)); t_add(&t, "°C"); } else t_add(&t, "--");
    texto_halo(l, S(76), y + S(54), S(30), t.s);
    texto_halo(l, S(76), y + S(76), S(13), !hay ? "Sin dato" : c >= 90 ? "¡Alerta térmica!" : c >= 65 ? "Caliente" : "Silicio despejado");
    texto_halo(l, S(14), y + S(106), S(11), "CLIMA DEL SILICIO");
    /* 3. Calendario */
    y += S(134);
    static const char *const meses[] = { "ENERO", "FEBRERO", "MARZO", "ABRIL", "MAYO", "JUNIO", "JULIO", "AGOSTO", "SEPTIEMBRE", "OCTUBRE", "NOVIEMBRE", "DICIEMBRE" };
    static const char *const dias[] = { "DOMINGO", "LUNES", "MARTES", "MIÉRCOLES", "JUEVES", "VIERNES", "SÁBADO" };
    redondo(l, S(20), y, GAD_AN - S(40), S(150), S(8), OPACO(0xFBFDFFu));
    redondo_deg(l, S(20), y, GAD_AN - S(40), S(36), S(8), OPACO(0xFF6A5Au), OPACO(0xC81A2Au));
    if (info.fecha_ok && info.mes >= 1 && info.mes <= 12) {
        const char *m = meses[info.mes - 1];
        texto(l, GAD_AN / 2 - texto_ancho(S(14), m) / 2, y + S(24), S(14), BLANCO, 255, 0, m);
        t_ini(&t); t_dos(&t, info.dia);
        texto(l, GAD_AN / 2 - texto_ancho(S(56), t.s) / 2, y + S(104), S(56), 0x0A2A50u, 255, 0, t.s);
        const char *d = dias[dia_semana(info.anio, info.mes, info.dia)];
        texto(l, GAD_AN / 2 - texto_ancho(S(14), d) / 2, y + S(134), S(14), 0x5A6A7Cu, 255, 0, d);
    }
    taek_presentar(s_gad.id, 0, 0, l->an, l->al);
}

/* La onda de choque del orbe al abrir Inicio: un anillo que crece y se apaga en medio segundo */
static void animar_onda(void) {
    if (!onda_t0) return;
    long t = taek_ms() - onda_t0;
    struct lienzo *l = &l_onda;
    limpiar(l, 0);
    int c = l->an / 2;
    if (t < 520) {
        float u = (float)t / 520.0f;
        int r = S(30) + (int)(u * (float)(c - S(32)));
        uint32_t a = (uint32_t)((1.0f - u) * 230.0f);
        anillo(l, c, c, r, r - S(5), premul(a, 0x7DE8FFu));
        anillo(l, c, c, r - S(8), r - S(10), premul(a / 2, BLANCO));
    } else onda_t0 = 0;
    taek_presentar(s_onda.id, 0, 0, l->an, l->al);
    if (onda_t0) { taek_mover(s_onda.id, S(46) - c, H - BARRA_AL / 2 - S(4) - c); taek_elevar(s_onda.id); }
    else ocultar(&s_onda);
}

/* ------------------------------------------------------------------ barra */

static int boton_tarea_x(int i) { return S(100) + i * S(190); }

static void pintar_barra(void) {
    struct lienzo *l = &l_barra;
    limpiar(l, 0);
    int y = BARRA_EXTRA;
    vidrio(l, 0, y, W, BARRA_AL, 0, 0, H - BARRA_AL, premul(70, 0x103A68u));
    degradado(l, 0, y, W, BARRA_AL / 2, premul(90, BLANCO), premul(18, BLANCO));
    rect(l, 0, y, W, 1, premul(190, BLANCO));
    /* Orbe de inicio: una burbuja con Neko dentro */
    int ox = S(46), oy = y + BARRA_AL / 2 - S(4), or_ = S(34);
    anillo(l, ox, oy, or_ + S(3), 0, premul(110, 0x000A20u));
    anillo(l, ox, oy, or_, 0, inicio_abierto ? OPACO(0x48E0A0u) : OPACO(0x1E90E0u));
    anillo(l, ox, oy + or_ / 3, or_ * 2 / 3, 0, premul(90, 0x70FFC0u));
    gato(l, ox, oy + S(2), S(42), 0xF0F4FFu, 0x20D0FFu);
    burbuja(l, ox, oy, or_, 0xF0FFFFu);
    /* Una tarea por ventana abierta, con su icono; la minimizada, mas tenue */
    int activa = -1;
    for (int k = nz - 1; k >= 0; k--) if (!vent[z[k]].minimizada) { activa = z[k]; break; }
    for (int i = 0, b = 0; i < NVENT; i++) {
        if (!vent[i].abierta) continue;
        int bx = boton_tarea_x(b++);
        uint32_t a0 = i == activa ? premul(170, 0xB8E4FFu) : vent[i].minimizada ? premul(30, BLANCO) : premul(70, BLANCO);
        uint32_t a1 = i == activa ? premul(90, 0x5CB4F0u) : premul(vent[i].minimizada ? 8 : 20, BLANCO);
        redondo_deg(l, bx, y + S(5), S(180), BARRA_AL - S(10), S(4), a0, a1);
        redondo_borde(l, bx, y + S(5), S(180), BARRA_AL - S(10), S(4), premul(vent[i].minimizada ? 90 : 170, BLANCO));
        icono(l, bx + S(22), y + BARRA_AL / 2, S(26), vt_icono[vent[i].tipo]);
        texto_halo(l, bx + S(42), y + S(31), S(17), vt_titulo[vent[i].tipo]);
    }
    /* Bandeja */
    struct txt t;
    t_ini(&t);
    if (info.fecha_ok) { t_dos(&t, info.hora); t_add(&t, ":"); t_dos(&t, info.minuto); }
    int w = texto_ancho(S(18), t.s);
    texto_halo(l, W - w - S(24), y + S(21), S(18), t.s);
    t_ini(&t);
    if (info.fecha_ok) { t_dos(&t, info.dia); t_add(&t, "/"); t_dos(&t, info.mes); t_add(&t, "/"); t_num(&t, info.anio); }
    w = texto_ancho(S(14), t.s);
    texto_halo(l, W - w - S(24), y + S(40), S(14), t.s);
    t_ini(&t);
    if (info.bateria_pct >= 0) { t_add(&t, "Batería "); t_num(&t, (uint64_t)info.bateria_pct); t_add(&t, " %"); }
    else t_add(&t, "Sin batería");
    t_add(&t, info.red_ip_ok ? "  ·  Red" : "  ·  Sin red");
    w = texto_ancho(S(18), t.s);
    texto_halo(l, W - w - S(305), y + S(31), S(18), t.s);
    /* Boton de conmutacion a QuickMenu Cyberpunk en la barra */
    int qm_w = S(140), qm_x = W - S(290);
    int qm_y = y + S(6), qm_h = BARRA_AL - S(12);
    redondo_deg(l, qm_x, qm_y, qm_w, qm_h, S(4), premul(170, 0x14081Eu), premul(220, 0x06030Au));
    redondo_borde(l, qm_x, qm_y, qm_w, qm_h, S(4), premul(220, 0x00F0FFu));
    rect(l, qm_x + S(5), qm_y + S(4), S(3), qm_h - S(8), OPACO(0xFF003Cu));
    texto(l, qm_x + S(14), qm_y + S(22), S(13), 0x00F0FFu, 255, 0, "QUICKMENU //");
    taek_presentar(s_barra.id, 0, 0, W, l->al);
}

/* --------------------------------------------------------------- ventanas */

static int vfila(struct lienzo *l, int x, int y, int ancho, const char *k, const char *v) {
    texto(l, x, y, S(18), 0x5A6A7Cu, 255, 0, k);
    texto(l, x + S(150), y, S(18), 0x0A2A50u, 255, 0, v[0] ? v : "No informado");
    rect(l, x, y + S(9), ancho, 1, premul(40, 0x3A78B8u));
    return y + S(32);
}

static void barra_aero(struct lienzo *l, int x, int y, int an, int al, int pct, uint32_t arriba, uint32_t abajo) {
    redondo_deg(l, x, y, an, al, S(4), OPACO(0xE8EEF4u), OPACO(0xFAFCFFu));
    int w = (an - S(4)) * pct / 100;
    if (w > S(8)) {
        redondo_deg(l, x + S(2), y + S(2), w, al - S(4), S(3), OPACO(arriba), OPACO(abajo));
        redondo_deg(l, x + S(3), y + S(3), w - S(2), (al - S(4)) / 2, S(3), premul(150, BLANCO), premul(20, BLANCO));
    }
    redondo_borde(l, x, y, an, al, S(4), premul(160, 0x60748Au));
}

static void contenido_ventana(int i, struct lienzo *l, int x, int y, int an) {
    struct txt t;
    int a = an - S(40);
    x += S(20); y += S(40);
    if (i == 0) {
        icono(l, x + an - S(90), y + S(10), S(64), 0);
        texto(l, x, y, S(28), 0x0A5AB0u, 255, 0, eq.modelo[0] ? eq.modelo : "Este equipo");
        y += S(40);
        y = vfila(l, x, y, a, "Fabricante", eq.fabricante);
        cadena_cpu(&t, 36);
        y = vfila(l, x, y, a, "Procesador", t.s);
        t_ini(&t); t_num(&t, info.cpus); t_add(&t, " hilos, "); t_num(&t, info.tsc_mhz); t_add(&t, " MHz medidos");
        y = vfila(l, x, y, a, "Núcleos", t.s);
        t_ini(&t); t_num(&t, info.ram_libre >> 20); t_add(&t, " MiB libres de "); t_num(&t, info.ram_total >> 20);
        y = vfila(l, x, y, a, "Memoria", t.s);
        uint64_t usada = info.ram_total > info.ram_libre ? info.ram_total - info.ram_libre : 0;
        barra_aero(l, x, y - S(14), a, S(20), info.ram_total ? (int)(usada * 100 / info.ram_total) : 0, 0x70E060u, 0x20A040u);
        y += S(24);
        cadena_gpu(&t);
        y = vfila(l, x, y, a, "Gráficos", t.s);
        y = vfila(l, x, y, a, "Video H.264", info.gpu_video_probada ? "GPU, probada en este equipo" : "Por software");
        cadena_ip(&t);
        y = vfila(l, x, y, a, "Red", t.s);
        t_ini(&t); t_num(&t, eq.ndiscos); t_add(&t, eq.ndiscos == 1 ? " disco USB" : " discos USB");
        vfila(l, x, y, a, "Almacenamiento", t.s);
    } else if (i == 1) {
        icono(l, x + an - S(90), y + S(4), S(64), 1);
        t_ini(&t);
        if (eq.bat_pct >= 0) { t_num(&t, (uint64_t)eq.bat_pct); t_add(&t, " %"); } else t_add(&t, "Sin datos");
        texto(l, x, y + S(8), S(48), 0x0A5AB0u, 255, 0, t.s);
        y += S(38);
        const char *est = !eq.bat_presente ? "Este equipo no informa batería" : (eq.bat_estado & 2) ? "Cargando" : (eq.bat_estado & 1) ? "Descargando" : eq.bat_cargador == 1 ? "Con cargador" : "En reposo";
        texto(l, x, y, S(22), 0x34506Eu, 255, 0, est);
        y += S(22);
        int p = eq.bat_pct < 0 ? 0 : eq.bat_pct;
        barra_aero(l, x, y, a, S(30), p, p < 20 ? 0xFF8A70u : 0x98F070u, p < 20 ? 0xC81A1Au : 0x1E9C3Cu);
        y += S(66);
        const uint32_t NADA = 0xFFFFFFFFu;
        const char *u = eq.bat_unidad_mah ? " mAh" : " mWh";
        t_ini(&t); if (eq.bat_resto != NADA) { t_num(&t, eq.bat_resto); t_add(&t, u); }
        y = vfila(l, x, y, a, "Ahora", t.s);
        t_ini(&t); if (eq.bat_lleno != NADA) { t_num(&t, eq.bat_lleno); t_add(&t, u); }
        y = vfila(l, x, y, a, "Llena", t.s);
        t_ini(&t); if (eq.bat_diseno && eq.bat_diseno != NADA && eq.bat_lleno != NADA) { t_num(&t, (uint64_t)eq.bat_lleno * 100 / eq.bat_diseno); t_add(&t, " %"); }
        y = vfila(l, x, y, a, "Salud", t.s);
        t_ini(&t); if (eq.bat_mv != NADA) { mil(&t, eq.bat_mv); t_add(&t, " V"); }
        y = vfila(l, x, y, a, "Tensión", t.s);
        vfila(l, x, y, a, "Modelo", eq.bat_modelo);
    } else {
        gato(l, x + an - S(100), y + S(58), S(130), 0x3C3650u, 0x20C0FFu);
        texto(l, x, y + S(6), S(36), 0x0A5AB0u, 255, 0, "TAEK OS");
        y += S(46);
        texto(l, x, y, S(18), 0x34506Eu, 255, 0, "Versión v0.1.0, experimental");
        y += S(52);
        y = parrafo(l, x, y, a - S(120), S(18), S(28), 0x20344Cu, 255, "Hecho en español, para quien quiera hacer su propio sistema operativo sin pasar por los tutoriales gringos.");
        y += S(36);
        y = parrafo(l, x, y, a, S(18), S(28), 0x20344Cu, 255, "NekoSaka es el sello de la casa. Neko supervisa desde el logo, como corresponde.");
        y += S(24);
        y = parrafo(l, x, y, a, S(14), S(22), 0x5A6A7Cu, 255, info.hito);
        y += S(12);
        parrafo(l, x, y, a, S(14), S(22), 0x5A6A7Cu, 255, "Tab: inicio. Flechas e Intro: iconos. Esc: cerrar o volver. Ctrl+C: consola.");
    }
}

/* H151: las ventanas del prototipo con tema oscuro: Terminal, Huevo, Stream y Ruleta. Comparten el estado con los paneles
 * del menu (paneles.h y ordenes.h): la Terminal del escritorio es la misma sesion que la del submenu. */
static int ruleta_confirmar;
static void contenido_oscuro(int tipo, struct lienzo *l, int x, int y, int an, int al, int con_foco) {
    rect(l, x, y, an, al, OPACO(0x0B0D12u));
    int px = x + S(16), py = y + S(30), pan = an - S(32), ymax = y + al - S(16);
    struct txt t;
    switch (tipo) {
    case VT_TERMINAL: term_bienvenida(); term_dibujar(l, x, y, an, al, con_foco); break;
    case VT_HUEVO:
        texto(l, px, py, S(16), 0x2ECC71u, 255, 0, "AUDITORÍA DE INTEGRIDAD DEL MONTÓN Y DEL PMM");
        py += S(34);
        t_ini(&t);
        if (sen_hay()) t_add(&t, sns.heap_canarios_ok ? "Canarios 0x7AEECC05 / 0xCAFEBABEDEAD1000: INTACTOS" : "Canarios: ROTO");
        else t_add(&t, "Canarios: sin dato");
        texto(l, px, py, S(15), sen_hay() && !sns.heap_canarios_ok ? ROJO : NEON, 255, 0, t.s);
        py += S(26);
        if (sen_hay()) { t_ini(&t); t_add(&t, "Montón: "); t_bytes(&t, sns.heap_en_uso); t_add(&t, " de "); t_bytes(&t, sns.heap_capacidad); t_add(&t, " · huevo "); t_num(&t, sns.huevo_salud); t_add(&t, " %"); texto(l, px, py, S(15), 0xC8D0DCu, 255, 0, t.s); }
        py += S(34);
        if (pan_huevo_ok) pan_texto(l, px, py, pan, ymax - S(40), pan_huevo, 0xE4E4EEu);
        else parrafo(l, px, py, pan, S(15), S(22), 0x8E94A8u, 255, "Intro ejecuta 'memoria probar' en el núcleo: pide, llena, redimensiona y libera bloques y comprueba los canarios.");
        texto(l, px, ymax - S(6), S(14), 0x7CFF9Au, 255, 0, "[INTRO] EJECUTAR 'memoria probar'");
        break;
    case VT_STREAM: pan_stream(l, px, y + S(14), pan, ymax, con_foco); break;
    case VT_RULETA:
        pan_ruleta(l, px, y + S(10), pan);
        texto(l, px, ymax - S(6), S(15), ruleta_confirmar ? ROJO : NEON, 255, 0, ruleta_confirmar ? "[INTRO] OTRA VEZ: EL DUELO EMPIEZA · [ESC] CANCELA" : "[INTRO] DOS VECES PARA JALAR DEL GATILLO");
        break;
    default: break;
    }
}

#include "ventanas.h"   /* H148: marco, gestion de ventanas y las aplicaciones del escritorio */

/* ------------------------------------------------------------- menu inicio */

/* H151: el menu Inicio del prototipo. Izquierda: seis programas y el buscador (escribir filtra). Derecha: el usuario,
 * cinco accesos y Apagar (que lleva a las opciones de energia del menu, donde se confirma). */
#define INI_IZQ 6
#define INI_AN S(660)
#define INI_AL S(600)
static const char *const inicio_nombre[NINICIO] = { "Terminal de Comando TAEK", "Reproductor de video", "Explorador de Archivos",
    "Navegador de Internet", "El Huevo Guardián", "QuickMenu NekoSaka", "Documentos", "Imágenes", "Música", "Equipo",
    "Panel de control", "Apagar" };
static const char *const inicio_sub[INI_IZQ] = { "Órdenes reales del núcleo", "H.264 por GPU o software", "FAT32 y exFAT, con papelera",
    "HTTP y HTTPS con mbedTLS", "Prueba de memoria y canarios", "Volver a la interfaz principal [TAB]" };
static const int inicio_ic[NINICIO] = { IC_TERMINAL, IC_VIDEOS, IC_ARCHIVOS, IC_NAVEGADOR, IC_HUEVO, IC_MENU, IC_ARCHIVOS, IC_IMAGENES, IC_MUSICA, IC_EQUIPO, IC_EQUIPO, IC_MENU };
static char ini_busca[32];
static int ini_nbusca;
static int inicio_visible(int i) { return i >= INI_IZQ || !ini_nbusca || ap_contiene(inicio_nombre[i], ini_busca); }
static void inicio_rect(int i, int *x, int *y, int *w, int *h) {
    if (i < INI_IZQ) { *x = S(30); *y = S(30) + i * S(66); *w = S(372); *h = S(60); }
    else if (i < NINICIO - 1) { *x = S(420); *y = S(150) + (i - INI_IZQ) * S(54); *w = INI_AN - S(440); *h = S(46); }
    else { *x = INI_AN - S(170); *y = INI_AL - S(70); *w = S(150); *h = S(44); }
}

static void pintar_inicio(void) {
    struct lienzo *l = &l_ini;
    limpiar(l, 0);
    int an = l->an, al = l->al;
    for (int k = S(10); k > 0; k -= S(3) > 0 ? S(3) : 1) redondo(l, S(10) - k, S(10) - k, an - S(20) + 2 * k, al - S(10) + k, S(10) + k, premul(16, 0));
    vidrio(l, S(10), S(10), an - S(20), al - S(10), S(10), S(18), H - BARRA_AL - INI_AL, premul(90, 0x8CC8F0u));
    redondo_borde(l, S(10), S(10), an - S(20), al - S(10), S(10), premul(210, 0x18345Au));
    redondo_deg(l, S(11), S(11), an - S(22), S(46), S(9), premul(110, BLANCO), premul(10, BLANCO));
    /* Columna blanca con los programas */
    redondo(l, S(22), S(22), S(388), al - S(44), S(5), OPACO(0xFBFDFFu));
    for (int i = 0; i < NINICIO; i++) {
        int x, y, w, h;
        inicio_rect(i, &x, &y, &w, &h);
        int vis = inicio_visible(i);
        if (i == inicio_sel && vis) {
            redondo_deg(l, x, y, w, h, S(4), premul(200, 0xBEE4FFu), premul(120, 0x6CB8F0u));
            redondo_borde(l, x, y, w, h, S(4), premul(180, 0x3A78B8u));
        }
        if (i < INI_IZQ) {
            icono(l, x + S(30), y + h / 2, S(38), inicio_ic[i]);
            texto(l, x + S(62), y + S(27), S(19), vis ? 0x0A2A50u : 0xB8C4D0u, 255, 0, inicio_nombre[i]);
            texto(l, x + S(62), y + S(48), S(13), vis ? 0x5A6A7Cu : 0xC8D0D8u, 255, 0, inicio_sub[i]);
        } else if (i < NINICIO - 1) {
            texto_halo(l, x + S(16), y + S(31), S(19), inicio_nombre[i]);
        } else {
            boton_brillante(l, x, y, w, h, 0xF0A090u, 0xB82020u);
            texto(l, x + S(40), y + S(29), S(18), BLANCO, 255, 0, "Apagar");
            anillo(l, x + S(22), y + h / 2, S(9), S(7), OPACO(BLANCO));
            rect(l, x + S(21), y + h / 2 - S(11), S(3), S(10), OPACO(BLANCO));
        }
    }
    /* Buscador */
    int by = al - S(78);
    redondo(l, S(34), by, S(364), S(40), S(6), OPACO(BLANCO));
    redondo_borde(l, S(34), by, S(364), S(40), S(6), premul(200, 0x7AA6D0u));
    anillo(l, S(56), by + S(18), S(8), S(6), OPACO(0x3A78B8u));
    linea(l, S(61), by + S(23), S(67), by + S(30), S(3), OPACO(0x3A78B8u));
    texto(l, S(78), by + S(27), S(16), ini_nbusca ? 0x0A2A50u : 0x8A9AAAu, 255, 0, ini_nbusca ? ini_busca : "Buscar programas y archivos...");
    /* El usuario: Neko en un marco de vidrio */
    int ux = S(420), uy = S(32);
    redondo_deg(l, ux, uy, S(84), S(84), S(10), premul(220, 0xE8F6FFu), premul(220, 0x8CC8F0u));
    redondo_borde(l, ux, uy, S(84), S(84), S(10), premul(220, BLANCO));
    gato(l, ux + S(42), uy + S(46), S(66), 0x3C3650u, 0x20C0FFu);
    texto_halo(l, ux + S(100), uy + S(36), S(24), "sudo");
    texto_halo(l, ux + S(100), uy + S(62), S(14), "Anillo 3 // NekoSaka");
    taek_presentar(s_ini.id, 0, 0, an, al);
}

static void inicio_abrir(int abrir) {
    inicio_abierto = abrir;
    if (abrir) {
        inicio_sel = 0; ini_nbusca = 0; ini_busca[0] = 0;
        pintar_inicio();
        taek_elevar(s_ini.id);
        m_ini.pos = (float)S(90); m_ini.vel = 0; m_ini.obj = 0;
        m_ini.dibujado = -1;
        onda_t0 = taek_ms();   /* La onda de choque del orbe */
        sonar(SND_AERO);
    } else ocultar(&s_ini);
    pintar_barra();
}

/* Lo que pide el menu de inicio; devuelve una accion para el nucleo o 0. */
static int inicio_activar(int i) {
    if (!inicio_visible(i)) return 0;
    inicio_abrir(0);
    switch (i) {
    case 0: ven_abrir(VT_TERMINAL); return 0;
    case 1: ven_abrir(VT_VIDEOS); return 0;
    case 2: case 6: return esc_lanzar("/archivos.elf", "");
    case 3: return esc_lanzar("/navegador.elf", "");
    case 4: ven_abrir(VT_HUEVO); return 0;
    case 5: salir_escritorio(); return 0;
    case 7: ven_abrir(VT_IMAGENES); return 0;
    case 8: ven_abrir(VT_MUSICA); return 0;
    case 9: ven_abrir(VT_EQUIPO); return 0;
    case 10: return saltar_menu(3, 0);   /* Panel de control: los modulos de CONFIGURACION */
    default: return saltar_menu(4, 0);   /* Apagar: las opciones de energia, que se confirman alli */
    }
}
/* ------------------------------------------------------------------ estado */

static void esc_ocultar(void) {
    ocultar(&s_dfondo); ocultar(&s_iconos); ocultar(&s_reloj); ocultar(&s_barra); ocultar(&s_ini);
    ocultar(&s_top); ocultar(&s_gad); ocultar(&s_onda); onda_t0 = 0;
    for (int i = 0; i < NBURB; i++) ocultar(&s_burb[i]);
    for (int i = 0; i < NVENT; i++) if (vent[i].creada) ocultar(&vent[i].s);
    inicio_abierto = 0;
    arrastre = -1; arr_redim = -1;
}

static int esc_crear(void) {
    if (crear(&s_dfondo, &l_dfondo, W, H, OCULTO, 0, 0) || crear(&s_dif, &l_dif, W, H, OCULTO, 0, 0)) return -1;
    for (int i = 0; i < NBURB; i++) {
        int r = S(26) + i * S(13);
        if (crear(&s_burb[i], &l_burb[i], 2 * r + S(4), 2 * r + S(4), OCULTO, 0, TAEK_SUP_ALFA)) return -1;
        burbuja(&l_burb[i], r + S(2), r + S(2), r, 0xE8FFFFu);
        taek_presentar(s_burb[i].id, 0, 0, l_burb[i].an, l_burb[i].al);
        burb[i].x = (float)(azar() % (uint32_t)W);
        burb[i].y = (float)(azar() % (uint32_t)H);
        burb[i].v = (float)S(10 + i * 4) / 10.0f;
        burb[i].fase = (float)i;
    }
    if (crear(&s_iconos, &l_iconos, ((NICONOS + ICONO_FILAS - 1) / ICONO_FILAS) * ICONO_AN, ICONO_FILAS * ICONO_AL, OCULTO, ICONOS_Y, TAEK_SUP_ALFA) ||
        crear(&s_reloj, &l_reloj, S(200), S(200), OCULTO, S(20), TAEK_SUP_ALFA)) return -1;
    /* H148: las ventanas se crean al abrirse la primera vez (ventanas.h), del tamano de la pantalla */
    if (crear(&s_barra, &l_barra, W, BARRA_AL + BARRA_EXTRA, OCULTO, H - BARRA_AL - BARRA_EXTRA, TAEK_SUP_ALFA) ||
        crear(&s_ini, &l_ini, INI_AN, INI_AL, OCULTO, 0, TAEK_SUP_ALFA)) return -1;
    /* H151: barra superior, gadgets y la onda del orbe */
    if (crear(&s_top, &l_top, W, TOP_AL, OCULTO, 0, TAEK_SUP_ALFA) || crear(&s_gad, &l_gad, GAD_AN, GAD_AL, OCULTO, 0, TAEK_SUP_ALFA) ||
        crear(&s_onda, &l_onda, S(260), S(260), OCULTO, 0, TAEK_SUP_ALFA)) return -1;
    return 0;
}

/* Pintar y desenfocar el cielo cuesta unos 350 ms: se hace una vez al abrir la interfaz, no en mitad del vuelo. */
static void preparar_escritorio(void) {
    if (fondo_listo) return;
    pintar_cielo();
    memcpy(l_dif.px, l_dfondo.px, (uint64_t)W * H * 4);
    desenfocar(&l_dif, S(14) > 0 ? S(14) : 1);
    taek_presentar(s_dfondo.id, 0, 0, 1, 1);
    fondo_listo = 1;
}

/* Las formas del escritorio para las lineas que vuelan: colinas, una aurora, el sol, los iconos y el reloj;
 * y seis losas: la barra de tareas, la columna de iconos, el orbe y el reloj. Mismo numero de lineas que el gato. */
static int segs_aero(struct seg *s, int n) {
    /* Colinas 48 + aurora 24 + sol 20 + 12 por icono + reloj 16 puntos; 5 formas mas una por icono.
     * H146: con 9 iconos ya no cabian en 200 puntos y 12 formas, y el escritorio se caia al entrar. */
    static float p[2 * (48 + 24 + 20 + 12 * NICONOS + 16)];
    struct forma fm[5 + NICONOS];
    int nf = 0, u = 0;
#define PUNTOS() (p + u)
    for (int k = 0; k < 2; k++) {
        float *q = PUNTOS(); int m = 24;
        for (int i = 0; i < m; i++) { int x = (W - 1) * i / (m - 1); q[2 * i] = (float)x; q[2 * i + 1] = (float)colina_y(k, x); }
        fm[nf++] = (struct forma){ q, m, 0, 2, 7, SF(3), 1, k ? 0xA7F3D0u : 0xC8FFD4u };
        u += 2 * m;
    }
    {
        float *q = PUNTOS(); int m = 24;
        for (int i = 0; i < m; i++) { int x = (W - 1) * i / (m - 1); q[2 * i] = (float)x; q[2 * i + 1] = (float)cinta_y(0, x); }
        fm[nf++] = (struct forma){ q, m, 0, 2, 5, SF(3), 1, 0xE0FBFFu };
        u += 2 * m;
    }
    {
        float *q = PUNTOS();
        circulo_pts(q, 20, (float)W * 0.20f, (float)H * 0.14f, SF(70));
        fm[nf++] = (struct forma){ q, 20, 1, 3, 2.5f, SF(3), 1, 0xFFF7C2u };
        u += 40;
    }
    for (int i = 0; i < NICONOS; i++) {
        float *q = PUNTOS();
        circulo_pts(q, 12, SF(16) + (float)(icono_x(i) + ICONO_AN / 2), (float)ICONOS_Y + (float)icono_y(i) + SF(50), SF(38));
        fm[nf++] = (struct forma){ q, 12, 1, 3, 0.9f, SF(2.5f), 1, 0xBDF3FFu };
        u += 24;
    }
    {
        float *q = PUNTOS();
        circulo_pts(q, 16, (float)(W - S(120)), (float)RELOJ_Y + SF(100), SF(92));
        fm[nf++] = (struct forma){ q, 16, 1, 3, 2, SF(2.5f), 1, 0xFFFFFFu };
        u += 32;
    }
#undef PUNTOS
    int m = repartir(s, fm, nf, n);
    ordenar_segs(s, m, 1);
    float yb = (float)(H - BARRA_AL / 2), xi = SF(16) + (float)ICONO_AN, alto_ic = (float)(ICONO_FILAS * ICONO_AL);
    s[m++] = (struct seg){ 0, yb, (float)W / 2, yb, (float)BARRA_AL, 1, 0x7DD3FCu };
    s[m++] = (struct seg){ (float)W / 2, yb, (float)W, yb, (float)BARRA_AL, 1, 0x7DD3FCu };
    s[m++] = (struct seg){ xi, (float)ICONOS_Y, xi, (float)ICONOS_Y + alto_ic / 2, (float)(2 * ICONO_AN), 0.3f, 0xE0F2FEu };
    s[m++] = (struct seg){ xi, (float)ICONOS_Y + alto_ic / 2, xi, (float)ICONOS_Y + alto_ic, (float)(2 * ICONO_AN), 0.3f, 0xE0F2FEu };
    s[m++] = (struct seg){ SF(12), yb - SF(4), SF(80), yb - SF(4), SF(60), 1, 0x38BDF8u };
    s[m++] = (struct seg){ (float)(W - S(212)), (float)RELOJ_Y + SF(100), (float)(W - S(28)), (float)RELOJ_Y + SF(100), SF(24), 0.6f, 0xFFFFFFu };
    return m;
}

static void entrar_escritorio(void) {
    ocultar_capas_menu();
    ocultar(&s_crono); ocultar(&s_estado); ocultar(&s_pie); ocultar(&s_pan);
    estado = E_ESCRITORIO;
    preparar_escritorio();
    taek_mover(s_dfondo.id, 0, 0);
    taek_presentar(s_dfondo.id, 0, 0, W, H);
    for (int i = 0; i < NBURB; i++) taek_mover(s_burb[i].id, (int)burb[i].x, (int)burb[i].y);
    pintar_iconos();
    taek_mover(s_iconos.id, S(16), ICONOS_Y);
    pintar_reloj();
    taek_mover(s_reloj.id, W - l_reloj.an - S(20), RELOJ_Y);
    pintar_top(); taek_mover(s_top.id, 0, 0);
    pintar_gadgets(); taek_mover(s_gad.id, W - GAD_AN - S(20), RELOJ_Y + S(206));
    for (int i = 0; i < NVENT; i++) if (vent[i].abierta && !vent[i].minimizada) { taek_mover(vent[i].s.id, vent[i].x, vent[i].y); pintar_ventana(i); }
    pintar_barra();
    taek_mover(s_barra.id, 0, H - BARRA_AL - BARRA_EXTRA);
    taek_elevar(s_barra.id);
    if (esc_reabrir_videos) { esc_reabrir_videos = 0; ven_abrir(VT_VIDEOS); }
}

static void esc_animar(void) {
    esc_cuadro++;
    /* Las burbujas suben con un vaiven; cada una solo mueve su pequeño cuadro */
    if (!(esc_cuadro & 1)) for (int i = 0; i < NBURB; i++) {
        struct burbuja_mov *b = &burb[i];
        b->y -= b->v;
        if (b->y < -(float)l_burb[i].al) { b->y = (float)H; b->x = (float)(azar() % (uint32_t)W); }
        taek_mover(s_burb[i].id, (int)(b->x + sen((float)esc_cuadro * 0.02f + b->fase) * (float)S(26)), (int)b->y);
    }
    if (inicio_abierto) {
        muelle_paso(&m_ini, 0.18f, 0.66f);
        int y = H - BARRA_AL - INI_AL + S(6) + (int)m_ini.pos;
        if (y != m_ini.dibujado) { taek_mover(s_ini.id, S(8), y); m_ini.dibujado = y; }
    }
    for (int i = 0; i < NVENT; i++) {
        struct ventana *v = &vent[i];
        if (v->anim == 0) continue;
        v->anim *= 0.68f;
        if (v->anim < 1.0f) v->anim = 0;
        taek_mover(v->s.id, v->x, v->y + (int)v->anim);
    }
    apps_cada_cuadro();
    animar_onda();
    sonido_bombear();
}

static void esc_cada_segundo(int minuto_nuevo) {
    pintar_reloj();
    pintar_gadgets();
    if (minuto_nuevo) pintar_barra();
}

/* ----------------------------------------------------------------- entrada */

/* H151: el "Salto a QuickMenu" del prototipo: se vuelve al menu, directo a la opcion de esa aplicacion */
static int saltar_menu(int seccion, int opcion) {
    salir_escritorio();
    sel = seccion; nivel = 1; subsel = opcion;
    for (int i = 0; i < NFILAS; i++) ocultar(&s_fila[i]);
    pintar_nivel_modo(ENTRA_CASCADA);
    sonar(SND_OLEADA);
    return 0;
}

/* Devuelve una accion para el nucleo (TAEK_ACCION_APP si abre una aplicacion) o 0 */
static int esc_activar_icono(int i) {
    sonar(SND_TICK);
    if (iconos[i].dibujo == IC_CANGREJO) return orden_pantalla("cangrejo");
    if (modo_quickmenu) {
        switch (iconos[i].dibujo) {
        case IC_TERMINAL: return saltar_menu(1, 0);
        case IC_NAVEGADOR: return saltar_menu(1, 1);
        case IC_VIDEOS: return saltar_menu(1, 2);
        case IC_HUEVO: return saltar_menu(0, 3);
        case IC_ARCHIVOS: return saltar_menu(1, 3);
        case IC_STREAM: return saltar_menu(1, 4);
        case IC_RULETA: return saltar_menu(1, 6);
        default: break;
        }
    }
    switch (iconos[i].dibujo) {
    case IC_TERMINAL: ven_abrir(VT_TERMINAL); return 0;
    case IC_HUEVO: ven_abrir(VT_HUEVO); return 0;
    case IC_STREAM: sen_leer(0); ven_abrir(VT_STREAM); return 0;
    case IC_RULETA: ruleta_confirmar = 0; ven_abrir(VT_RULETA); return 0;
    case IC_EQUIPO: ven_abrir(VT_EQUIPO); return 0;
    case IC_BATERIA: ven_abrir(VT_BATERIA); return 0;
    case IC_ACERCA: ven_abrir(VT_ACERCA); return 0;
    /* H148: en el escritorio, Imagenes, Musica y Videos son ventanas; sus .elf de pantalla completa son los del menu */
    case IC_IMAGENES: ven_abrir(VT_IMAGENES); return 0;
    case IC_MUSICA: ven_abrir(VT_MUSICA); return 0;
    case IC_VIDEOS: ven_abrir(VT_VIDEOS); return 0;
    case IC_MENU: salir_escritorio(); return 0;
    default: return esc_lanzar(iconos[i].programa, iconos[i].argumento ? iconos[i].argumento : "");
    }
}

static int esc_tecla(uint32_t k) {
    if (inicio_abierto) {
        /* H151: como en el prototipo, escribir busca entre los programas */
        if (k == 9 || k == 27) { if (k == 27 && ini_nbusca) { ini_nbusca = 0; ini_busca[0] = 0; pintar_inicio(); return 0; } inicio_abrir(0); return 0; }
        if (k == 17 || k == 18) {
            int d = k == 17 ? NINICIO - 1 : 1;
            for (int n = 0; n < NINICIO; n++) { inicio_sel = (inicio_sel + d) % NINICIO; if (inicio_visible(inicio_sel)) break; }
            sonar(SND_TICK); pintar_inicio();
        } else if (k == '\n') return inicio_activar(inicio_sel);
        else if (k == 8) { if (ini_nbusca) ini_busca[--ini_nbusca] = 0; pintar_inicio(); }
        else if (k >= 32 && k < 127 && ini_nbusca < 30) {
            ini_busca[ini_nbusca++] = (char)k; ini_busca[ini_nbusca] = 0;
            if (!inicio_visible(inicio_sel)) for (int n = 0; n < INI_IZQ; n++) if (inicio_visible(n)) { inicio_sel = n; break; }
            pintar_inicio();
        }
        return 0;
    }
    if (k == 9) {
        int f_act = foco();
        if (f_act < 0 && !inicio_abierto) { salir_escritorio(); return 0; }
        inicio_abrir(!inicio_abierto);
        return 0;
    }
    /* La ventana con foco se queda con lo que entienda (buscar, flechas, Intro...) */
    int f = foco();
    if (f >= 0) {
        int a = app_tecla(f, k);
        if (a >= 0) return a;
    }
    if (k == 27 || (k == 8 && (f < 0 || vent[f].tipo < VT_IMAGENES))) {
        if (f >= 0) esc_cerrar(f);
        else if (k == 27) salir_escritorio();
        return 0;
    }
    if (f >= 0 && vent[f].tipo >= VT_IMAGENES) return 0;   /* Las teclas no se escapan a los iconos de atras */
    if (k == 17 || k == 'w') { icono_sel = (icono_sel + NICONOS - 1) % NICONOS; pintar_iconos(); }
    else if (k == 18 || k == 's') { icono_sel = (icono_sel + 1) % NICONOS; pintar_iconos(); }
    else if (k == '\n' || k == ' ') return esc_activar_icono(icono_sel);
    return 0;
}

static int esc_raton(const struct taek_evento *e, int subida) {
    int x = e->x, y = e->y;
    /* H148: escalar desde un borde o esquina. Se redibuja a unos 30 cuadros por segundo y al soltar. */
    if (arr_redim >= 0) {
        struct ventana *v = &vent[arr_redim];
        int suelto = !(e->botones & 1);
        long ahora = taek_ms();
        if (!suelto && ahora - rd_ultimo < 33) return 0;
        rd_ultimo = ahora;
        int dx = x - rd_x0, dy = y - rd_y0, nx = rd_vx, ny = rd_vy, nan = rd_an, nal = rd_al;
        int m = VENT_MARGEN, min_an = VMIN_AN + 2 * m, min_al = VMIN_AL + 2 * m;
        int max_an = W + 2 * m, max_al = H - BARRA_AL + 2 * m;
        if (redim_bordes & 1) { nx = rd_vx + dx; nan = rd_an - dx; }
        if (redim_bordes & 2) nan = rd_an + dx;
        if (redim_bordes & 4) { ny = rd_vy + dy; nal = rd_al - dy; }
        if (redim_bordes & 8) nal = rd_al + dy;
        if (nan < min_an) { if (redim_bordes & 1) nx -= min_an - nan; nan = min_an; }
        if (nal < min_al) { if (redim_bordes & 4) ny -= min_al - nal; nal = min_al; }
        if (nan > max_an) { if (redim_bordes & 1) nx += nan - max_an; nan = max_an; }
        if (nal > max_al) { if (redim_bordes & 4) ny += nal - max_al; nal = max_al; }
        if (ny < -m) { nal -= -m - ny; ny = -m; }
        if (nx != v->x || ny != v->y || nan != v->l.an || nal != v->l.al) geometria(arr_redim, nx, ny, nan, nal);
        if (suelto) arr_redim = -1;
        return 0;
    }
    /* La barra de una lista que se arrastra */
    {
        int f = foco();
        struct mlista *ls = f >= 0 ? lista_de(vent[f].tipo) : NULL;
        if (ls && ls->arr_barra) {
            if (!(e->botones & 1)) ls->arr_barra = 0;
            else if (ml_arrastrar(ls, y - vent[f].y)) pintar_ventana(f);
            return 0;
        }
    }
    /* Arrastre en curso */
    if (arrastre >= 0) {
        if (!(e->botones & 1)) {
            esc_mover_ventana(arrastre, vent[arrastre].x, vent[arrastre].y, 1);
            arrastre = -1;
            return 0;
        }
        if (x != arr_x || y != arr_y) {
            long ahora = taek_ms();
            int repintar = ahora - arr_ultimo > 120;
            if (repintar) arr_ultimo = ahora;
            arr_x = x; arr_y = y;
            esc_mover_ventana(arrastre, x - arr_dx, y - arr_dy, repintar);
        }
        return 0;
    }
    if (!(subida & 1)) return 0;
    /* Inicio abierto: elegir o cerrar */
    if (inicio_abierto) {
        int bx = S(8), by = H - BARRA_AL - INI_AL + S(6);
        if (x >= bx && x < bx + INI_AN && y >= by && y < by + INI_AL) {
            for (int i = 0; i < NINICIO; i++) {
                int ix, iy, iw, ih;
                inicio_rect(i, &ix, &iy, &iw, &ih);
                if (x >= bx + ix && x < bx + ix + iw && y >= by + iy && y < by + iy + ih) return inicio_activar(i);
            }
            return 0;
        }
        inicio_abrir(0);
    }
    /* H151: la barra superior: modo de los iconos y abrir QuickMenu */
    if (y < TOP_AL) {
        if (x >= top_x[0] && x < top_x[0] + top_an[0]) { modo_quickmenu = 0; sonar(SND_TICK); pintar_top(); }
        else if (x >= top_x[1] && x < top_x[1] + top_an[1]) { modo_quickmenu = 1; sonar(SND_TICK); pintar_top(); }
        else if (x >= top_x[2] && x < top_x[2] + top_an[2]) salir_escritorio();
        return 0;
    }
    /* Orbe y barra */
    if (y >= H - BARRA_AL - BARRA_EXTRA && x < S(90)) { inicio_abrir(!inicio_abierto); return 0; }
    if (y >= H - BARRA_AL && x >= W - S(290) && x < W - S(140)) { salir_escritorio(); return 0; }
    if (y >= H - BARRA_AL) {
        for (int i = 0, b = 0; i < NVENT; i++) {
            if (!vent[i].abierta) continue;
            int bx = boton_tarea_x(b++);
            /* Como en Windows: la activa se minimiza; las demas pasan al frente */
            if (x >= bx && x < bx + S(180)) { if (foco() == i) minimizar(i); else enfocar(i); return 0; }
        }
        return 0;
    }
    /* Ventanas, de la de delante a la de atras */
    for (int k = nz - 1; k >= 0; k--) {
        int i = z[k], bordes;
        struct ventana *v = &vent[i];
        if (v->minimizada) continue;
        int zn = zona(i, x, y, &bordes);
        if (zn == Z_FUERA) continue;
        if (foco() != i) enfocar(i);
        switch (zn) {
        case Z_CERRAR: esc_cerrar(i); return 0;
        case Z_MIN: minimizar(i); return 0;
        case Z_MAX: maximizar(i); return 0;
        case Z_BORDE:
            arr_redim = i; redim_bordes = bordes; rd_x0 = x; rd_y0 = y;
            rd_vx = v->x; rd_vy = v->y; rd_an = v->l.an; rd_al = v->l.al; rd_ultimo = 0;
            return 0;
        case Z_TITULO: {
            long ahora = taek_ms();
            if (tit_ultima == i && ahora - tit_ultimo_clic < 450) { tit_ultima = -1; maximizar(i); return 0; }
            tit_ultima = i; tit_ultimo_clic = ahora;
            if (!v->maximizada) { arrastre = i; arr_dx = x - v->x; arr_dy = y - v->y; arr_x = x; arr_y = y; }
            return 0;
        }
        default: return app_clic(i, x, y);
        }
    }
    /* Iconos, en columnas de ICONO_FILAS */
    if (x >= S(16) && x < S(16) + l_iconos.an && y >= ICONOS_Y && y < ICONOS_Y + ICONO_FILAS * ICONO_AL) {
        int i = ((x - S(16)) / ICONO_AN) * ICONO_FILAS + (y - ICONOS_Y) / ICONO_AL;
        if (i >= NICONOS) return 0;
        icono_sel = i;
        pintar_iconos();
        return esc_activar_icono(i);
    }
    return 0;
}
