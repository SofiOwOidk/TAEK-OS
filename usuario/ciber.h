/* H148: estilo neon del menu rapido (imagenes.elf, musica.elf, videos.elf). Diseno propio de NekoSaka, de la familia
 * "cyberpunk": negro azulado, amarillo, cian y rojo; paneles con esquinas cortadas, titulos con los canales separados,
 * barrido de escaneo, cortes de glitch y un cursor de neon que salta con resorte a lo elegido.
 *
 * Como el menu principal, sin GPU: lo quieto se pinta una vez y lo que se mueve son capas pequenas que el compositor
 * desplaza. El programa dibuja su contenido en una capa transparente encima del fondo (sup, L) y llama a cb_animar en
 * cada cuadro. Se incluye despues de definir AN, AL, S(), sup y L. */

#define CB_AMARILLO 0xFCEE0Au
#define CB_CIAN     0x00F0FFu
#define CB_ROJO     0xFF003Cu
#define CB_NEGRO    0x07070Cu
#define CB_TEXTO    0xD8DCE6u
#define CB_TENUE    0x6E7690u
#define CB_NGLITCH  5

static struct taek_superficie cb_sfondo, cb_sbarrido, cb_scursor, cb_sglitch[CB_NGLITCH];
static struct lienzo cb_lfondo, cb_lbarrido, cb_lcursor, cb_lglitch[CB_NGLITCH];
static const char *cb_titulo_txt, *cb_codigo_txt;
static uint32_t cb_azar_e = 0x2545F491u;
static uint32_t cb_azar(void) { cb_azar_e ^= cb_azar_e << 13; cb_azar_e ^= cb_azar_e >> 17; cb_azar_e ^= cb_azar_e << 5; return cb_azar_e; }

/* Resortes del cursor (x, y) y de la entrada del contenido */
struct cb_muelle { float p, v, o; };
static void cb_paso(struct cb_muelle *m, float k, float a) { m->v += (m->o - m->p) * k; m->v *= a; m->p += m->v; }
static int cb_quieto(const struct cb_muelle *m) { float d = m->o - m->p; return d < 0.5f && d > -0.5f && m->v < 0.5f && m->v > -0.5f; }
static struct cb_muelle cb_cx, cb_cy, cb_ent;
static int cb_cur_an, cb_cur_al, cb_cur_visible, cb_cur_dib_x = -99999, cb_cur_dib_y = -99999, cb_ent_dib = -99999;
static long cb_cuadro, cb_glitch_hasta[CB_NGLITCH], cb_proximo_glitch, cb_titulo_hasta;
static int cb_barrido_y;

/* ------------------------------------------------------------------ piezas de dibujo */

/* Titulo con el rojo y el cian corridos a los lados */
static void cb_titulo(struct lienzo *l, int x, int y, int px, int sep, const char *s) {
    texto(l, x - sep, y, px, CB_ROJO, 200, -12, s);
    texto(l, x + sep, y, px, CB_CIAN, 200, -12, s);
    texto(l, x, y, px, 0xFFFFFFu, 255, -12, s);
}
/* Panel oscuro translucido con la esquina de arriba a la derecha cortada y el filo de neon */
static void cb_panel(struct lienzo *l, int x, int y, int an, int al, uint32_t filo) {
    int c = S(22);
    chaflan(l, x, y, an, al, c, premul(205, 0x0A0C14u));
    rect(l, x, y, an - c, S(2), OPACO(filo));
    rect(l, x, y, S(2), al, premul(170, filo));
    linea(l, x + an - c, y, x + an, y + c, S(2) > 0 ? S(2) : 1, OPACO(filo));
    rect(l, x + an - S(2), y + c, S(2), al - c - S(26), premul(110, filo));
    rect(l, x + an - S(64), y + al - S(4), S(64), S(4), OPACO(filo));
}
/* Barra segmentada: bloques que se encienden */
static void cb_barra(struct lienzo *l, int x, int y, int an, int al, int permil, uint32_t c) {
    int paso = S(10), n = an / paso, encendidos = n * permil / 1000;
    for (int k = 0; k < n; k++) rect(l, x + k * paso, y, paso - S(3), al, k < encendidos ? OPACO(c) : premul(50, c));
}
/* Caja de busqueda de neon: lo escrito en amarillo con su cursor */
static void cb_buscador(struct lienzo *l, int x, int y, int an, const char *texto_buscado, int n) {
    int al = S(46);
    chaflan(l, x, y, an, al, S(12), premul(220, 0x10121Cu));
    rect(l, x, y + al - S(2), an, S(2), n ? OPACO(CB_AMARILLO) : premul(120, CB_CIAN));
    texto(l, x + S(14), y + S(19), S(12), n ? CB_AMARILLO : CB_TENUE, 255, 0, "BUSCAR //");
    texto(l, x + S(14), y + S(38), S(18), n ? 0xFFFFFFu : CB_TENUE, 255, 0, n ? texto_buscado : "escribe para filtrar");
    if (n && (cb_cuadro / 20) % 2 == 0) rect(l, x + S(16) + texto_ancho(S(18), texto_buscado), y + S(22), S(10), S(18), OPACO(CB_AMARILLO));
}

/* ------------------------------------------------------------------ fondo (una vez) */

static uint32_t *cb_bajo_titulo;   /* El fondo limpio bajo el titulo, para redibujarlo sin dejar caja */
static void cb_pintar_titulo(int glitch) {
    struct lienzo *l = &cb_lfondo;
    int x0 = S(40), y0 = S(18), an = S(760), al = S(110);
    if (!cb_bajo_titulo) {
        cb_bajo_titulo = malloc((size_t)an * (size_t)al * 4);
        if (cb_bajo_titulo) for (int f = 0; f < al; f++) memcpy(cb_bajo_titulo + (uint64_t)f * an, l->px + (uint64_t)(y0 + f) * l->an + x0 - S(10), (size_t)an * 4);
    } else for (int f = 0; f < al; f++) memcpy(l->px + (uint64_t)(y0 + f) * l->an + x0 - S(10), cb_bajo_titulo + (uint64_t)f * an, (size_t)an * 4);
    /* Etiqueta amarilla inclinada con el codigo */
    para(l, x0, y0 + S(10), S(250), S(26), S(14), OPACO(CB_AMARILLO));
    texto(l, x0 + S(20), y0 + S(29), S(14), CB_NEGRO, 255, -6, cb_codigo_txt);
    int sep = glitch ? S(6) + (int)(cb_azar() % (uint32_t)(S(10) + 1)) : S(2);
    cb_titulo(l, x0, y0 + S(90), S(48), sep, cb_titulo_txt);
    if (glitch)   /* Tiras del titulo corridas a los lados */
        for (int k = 0; k < 4; k++) {
            int fy = y0 + S(46) + (int)(cb_azar() % (uint32_t)S(50)), fh = S(3) + (int)(cb_azar() % (uint32_t)S(8));
            int d = (int)(cb_azar() % (uint32_t)S(40)) - S(20);
            for (int f = fy; f < fy + fh && f < l->al; f++) {
                uint32_t *p = l->px + (uint64_t)f * l->an;
                if (d > 0) for (int x = x0 + an - S(20); x >= x0 + d; x--) p[x] = p[x - d];
                else for (int x = x0; x < x0 + an - S(20) + d; x++) p[x] = p[x - d];
            }
        }
    rect(l, x0, y0 + al - S(6), S(120), S(4), OPACO(CB_ROJO));
    taek_presentar(cb_sfondo.id, x0 - S(10), y0, an, al);
}

static void cb_pintar_fondo(void) {
    struct lienzo *l = &cb_lfondo;
    for (int y = 0; y < AL; y++) {   /* Negro azulado, con un poco de purpura arriba */
        uint32_t t = (uint32_t)(y < AL / 2 ? (AL / 2 - y) * 18 / (AL / 2) : 0);
        uint32_t c = 0xFF000000u | (5 + t) << 16 | 6u << 8 | (10 + t / 2);
        uint32_t *p = l->px + (uint64_t)y * AN;
        for (int x = 0; x < AN; x++) p[x] = c;
    }
    /* Suelo en perspectiva: lineas que se van al horizonte */
    int hy = AL * 62 / 100, cx = AN / 2;
    for (int k = -24; k <= 24; k++) linea(l, cx + k * S(18), hy, cx + k * S(260), AL, 1, premul(38, CB_CIAN));
    for (int k = 0; k < 14; k++) {
        float f = (float)(k * k) / 196.0f;
        int y = hy + (int)(f * (float)(AL - hy));
        rect(l, 0, y, AN, 1, premul((uint32_t)(18 + 40 * f), CB_CIAN));
    }
    rect(l, 0, hy, AN, S(2), premul(120, CB_ROJO));
    /* Columnas de datos tenues a la derecha */
    static const char hex[] = "0123456789ABCDEF";
    for (int c = 0; c < 3; c++)
        for (int f = 0; f < 22; f++) {
            char s[9]; for (int i = 0; i < 8; i++) s[i] = hex[cb_azar() & 15]; s[8] = 0;
            texto(l, AN - S(130) - c * S(110), S(170) + f * S(24), S(12), CB_CIAN, 14 + (cb_azar() % 22), 0, s);
        }
    /* Marcos de visor en las esquinas de la pantalla y franjas de precaucion arriba a la derecha */
    esquinas(l, S(14), S(14), AN - S(28), AL - S(28), S(60), S(3), premul(200, CB_AMARILLO));
    peligro(l, AN - S(420), S(30), S(300), S(14), S(10), premul(220, CB_AMARILLO), premul(220, CB_NEGRO));
    texto(l, AN - S(420), S(70), S(12), CB_TENUE, 255, 0, "NEKOSAKA // MENU RAPIDO // TAEK OS");
    cb_pintar_titulo(0);
    taek_presentar(cb_sfondo.id, 0, 0, AN, AL);
}

/* ------------------------------------------------------------------ capas */

static int cb_crear(struct taek_superficie *s, struct lienzo *l, int an, int al, int x, int y, uint32_t b) {
    *s = (struct taek_superficie){ (uint32_t)an, (uint32_t)al, x, y, b, 0, 0 };
    if (taek_superficie(s)) return -1;
    *l = (struct lienzo){ (uint32_t *)(uintptr_t)s->pixeles, an, al };
    return 0;
}
/* Antes de crear la capa de contenido: el fondo */
static int cb_crear_fondo(const char *titulo, const char *codigo) {
    cb_titulo_txt = titulo; cb_codigo_txt = codigo;
    cb_azar_e ^= (uint32_t)taek_ms() * 2654435761u;
    if (cb_crear(&cb_sfondo, &cb_lfondo, AN, AL, 0, 0, 0)) return -1;
    cb_pintar_fondo();
    return 0;
}
static void cb_dibujar_cursor(void);
/* Despues de la capa de contenido: barrido, cortes y cursor, por encima */
static int cb_crear_capas(void) {
    if (cb_crear(&cb_sbarrido, &cb_lbarrido, AN, S(110), 0, -S(200), TAEK_SUP_ALFA)) return -1;
    struct lienzo *l = &cb_lbarrido;
    for (int y = 0; y < l->al; y++) {   /* Una banda que se desvanece hacia arriba y una linea brillante abajo */
        uint32_t a = (uint32_t)(y * 34 / l->al);
        rect(l, 0, y, AN, 1, premul(a, CB_CIAN));
    }
    rect(l, 0, l->al - S(2), AN, S(2), premul(150, CB_CIAN));
    taek_presentar(cb_sbarrido.id, 0, 0, AN, l->al);
    for (int i = 0; i < CB_NGLITCH; i++)
        if (cb_crear(&cb_sglitch[i], &cb_lglitch[i], S(700), S(60), 32000, 0, TAEK_SUP_ALFA)) return -1;
    if (cb_crear(&cb_scursor, &cb_lcursor, AN, S(400), 32000, 0, TAEK_SUP_ALFA)) return -1;
    return 0;
}

/* Un corte de glitch: bandas de color con ruido, a la vista unos cuadros */
static void cb_un_glitch(int i, int x, int y) {
    struct lienzo *l = &cb_lglitch[i];
    int an = S(80) + (int)(cb_azar() % (uint32_t)S(600)), al = S(4) + (int)(cb_azar() % (uint32_t)S(40));
    l->an = an; l->al = al;
    static const uint32_t tonos[] = { CB_CIAN, CB_ROJO, CB_AMARILLO, 0xFFFFFFu };
    limpiar(l, premul(60 + cb_azar() % 90, tonos[cb_azar() % 4]));
    for (int k = 0; k < 6; k++) rect(l, (int)(cb_azar() % (uint32_t)an), 0, S(4) + (int)(cb_azar() % (uint32_t)S(60)), al, premul(120 + cb_azar() % 120, tonos[cb_azar() % 4]));
    rect(l, 0, (int)(cb_azar() % (uint32_t)al), an, 1, premul(230, 0xFFFFFFu));
    taek_tamano(cb_sglitch[i].id, x, y, an, al);
    cb_glitch_hasta[i] = cb_cuadro + 2 + (long)(cb_azar() % 5);
}
/* Rafaga de cortes; fuerza 1 a 5 */
static void cb_glitch(int fuerza) {
    for (int i = 0; i < CB_NGLITCH && i < fuerza; i++)
        cb_un_glitch(i, (int)(cb_azar() % (uint32_t)AN) - S(100), (int)(cb_azar() % (uint32_t)AL));
    cb_titulo_hasta = cb_cuadro + 6;
    cb_pintar_titulo(1);
}

/* Cursor de neon: esquinas gruesas, filo fino y un brillo que late */
static void cb_dibujar_cursor(void) {
    struct lienzo *l = &cb_lcursor;
    int an = cb_cur_an + S(16), al = cb_cur_al + S(16);
    if ((uint64_t)an * (uint64_t)al > (uint64_t)AN * (uint64_t)S(400)) return;
    l->an = an; l->al = al;
    limpiar(l, 0);
    float lat = 0.5f + 0.5f * sen((float)cb_cuadro * 0.12f);
    rect(l, S(8), S(8), cb_cur_an, cb_cur_al, premul((uint32_t)(14 + 22 * lat), CB_AMARILLO));
    redondo_borde(l, S(7), S(7), cb_cur_an + S(2), cb_cur_al + S(2), 0, premul((uint32_t)(90 + 120 * lat), CB_AMARILLO));
    esquinas(l, 0, 0, an, al, S(22), S(4), OPACO(CB_AMARILLO));
    rect(l, 0, al / 2 - S(10), S(4), S(20), OPACO(CB_ROJO));
    para(l, an - S(70), 0, S(52), S(10), S(6), OPACO(CB_CIAN));
}
/* Lleva el cursor a ese rectangulo de pantalla (relativo al contenido); an = 0 lo esconde */
static void cb_cursor(int x, int y, int an, int al) {
    if (an <= 0) { cb_cur_visible = 0; taek_mover(cb_scursor.id, 32000, 0); return; }
    if (!cb_cur_visible) { cb_cx.p = (float)x; cb_cy.p = (float)y + S(30); cb_cx.v = cb_cy.v = 0; }
    cb_cur_visible = 1;
    cb_cx.o = (float)x; cb_cy.o = (float)y;
    if (an != cb_cur_an || al != cb_cur_al) { cb_cur_an = an; cb_cur_al = al; cb_dibujar_cursor(); cb_cur_dib_x = -99999; }
}
/* Entrada: el contenido llega deslizandose con una rafaga */
static void cb_entrar(int desde) {
    cb_ent.p = (float)desde; cb_ent.v = 0; cb_ent.o = 0;
    cb_glitch(5);
}

/* Cada cuadro */
static void cb_animar(void) {
    cb_cuadro++;
    /* Barrido de escaneo: baja y vuelve a empezar */
    cb_barrido_y += S(6) > 1 ? S(6) : 2;
    if (cb_barrido_y > AL + S(40)) cb_barrido_y = -S(110);
    taek_mover(cb_sbarrido.id, 0, cb_barrido_y);
    /* Cortes que vencen, y rafagas cada tanto */
    for (int i = 0; i < CB_NGLITCH; i++)
        if (cb_glitch_hasta[i] && cb_cuadro >= cb_glitch_hasta[i]) { cb_glitch_hasta[i] = 0; taek_mover(cb_sglitch[i].id, 32000, 0); }
    if (cb_cuadro >= cb_proximo_glitch) {
        cb_glitch(1 + (int)(cb_azar() % 3));
        cb_proximo_glitch = cb_cuadro + 90 + (long)(cb_azar() % 200);
    }
    if (cb_titulo_hasta && cb_cuadro >= cb_titulo_hasta) { cb_titulo_hasta = 0; cb_pintar_titulo(0); }
    /* Entrada del contenido */
    if (!cb_quieto(&cb_ent) || cb_ent_dib != (int)cb_ent.p) {
        cb_paso(&cb_ent, 0.16f, 0.62f);
        if (cb_quieto(&cb_ent)) cb_ent.p = 0;
        if ((int)cb_ent.p != cb_ent_dib) { cb_ent_dib = (int)cb_ent.p; taek_mover(sup.id, cb_ent_dib, 0); }
    }
    /* Cursor: resorte con rebote y su latido */
    if (cb_cur_visible) {
        cb_paso(&cb_cx, 0.22f, 0.64f); cb_paso(&cb_cy, 0.22f, 0.64f);
        if (cb_cuadro % 3 == 0) { cb_dibujar_cursor(); taek_presentar(cb_scursor.id, 0, 0, cb_lcursor.an, cb_lcursor.al); }
        int x = (int)cb_cx.p - S(8) + cb_ent_dib, y = (int)cb_cy.p - S(8);
        if (x != cb_cur_dib_x || y != cb_cur_dib_y) {
            cb_cur_dib_x = x; cb_cur_dib_y = y;
            taek_tamano(cb_scursor.id, x, y, cb_lcursor.an, cb_lcursor.al);
        }
    }
}
