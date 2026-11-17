/* H151: ordenes reales de la terminal del nucleo, pedidas desde la interfaz.
 *  - orden_texto(): las de solo texto (SYS_ORDEN). Lo que imprimen vuelve aqui, con los colores marcados 0x01 R G B.
 *  - orden_pantalla(): las que necesitan la pantalla (ruleta, cangrejo, estres...). Se piden con SYS_LANZAR y el menu
 *    termina con TAEK_ACCION_ORDEN; el nucleo las corre en la consola y luego vuelve al menu donde estaba.
 * Y un widget de terminal (term_*) que usan el submenu de APLICACIONES y la ventana Terminal del escritorio. */

static char ord_buf[64 * 1024];
static struct taek_orden ord_o;
static long ord_r;

/* Devuelve los bytes impresos, o -TAEK_EPANTALLA, -TAEK_ENOPERMITIDA, -TAEK_ENOSYS (nucleo anterior a H151). */
static long orden_texto(const char *linea) {
    memset(&ord_o, 0, sizeof(ord_o));
    int n = 0;
    while (linea[n] && n < TAEK_ORDEN_LINEA - 1) { ord_o.linea[n] = linea[n]; n++; }
    ord_o.destino = (uint64_t)(uintptr_t)ord_buf;
    ord_o.capacidad = sizeof(ord_buf) - 1;
    ord_r = taek_orden(&ord_o);
    ord_buf[ord_r > 0 ? ord_r : 0] = 0;
    return ord_r;
}
/* 0 si no se pudo pedir; si no, la accion con la que el menu debe terminar */
static int orden_pantalla(const char *linea) {
    static struct taek_lanzar l;
    memset(&l, 0, sizeof(l));
    memcpy(l.programa, TAEK_PROGRAMA_ORDEN, sizeof(TAEK_PROGRAMA_ORDEN));
    for (int n = 0; linea[n] && n < TAEK_RUTA - 1; n++) l.argumento[n] = linea[n];
    return taek_lanzar(&l) == 0 ? TAEK_ACCION_ORDEN : 0;
}
static const char *orden_error(long r) {
    return r == -TAEK_ENOSYS ? "El núcleo que corre es anterior a H151: no tiene SYS_ORDEN." :
           r == -TAEK_ENOPERMITIDA ? "Esa orden no se puede pedir desde la interfaz. Para todas, la consola (Ctrl+C)." :
           r == -TAEK_EPANTALLA ? "Esa orden necesita la pantalla completa." : "La orden no respondió.";
}

/* Recorre la salida linea a linea, sin marcas de color. *pos empieza en 0; devuelve 0 al terminar. */
static int orden_linea(int *pos, char *d, int cap) {
    int p = *pos, n = 0;
    if (!ord_buf[p]) return 0;
    while (ord_buf[p] && ord_buf[p] != '\n') {
        if (ord_buf[p] == 1) { p += ord_buf[p + 1] && ord_buf[p + 2] && ord_buf[p + 3] ? 4 : 1; if (p > (int)sizeof(ord_buf)) break; continue; }
        if (n < cap - 1) d[n++] = ord_buf[p];
        p++;
    }
    d[n] = 0;
    *pos = ord_buf[p] == '\n' ? p + 1 : p;
    return 1;
}
/* El primer texto despues de una marca (por ejemplo "[ Pasaje: "): copia lo que sigue hasta el fin de linea o "stop". */
static int orden_buscar(const char *marca, char *d, int cap, char stop) {
    int pos = 0;
    char lin[400];
    while (orden_linea(&pos, lin, sizeof(lin))) {
        for (int i = 0; lin[i]; i++) {
            int k = 0;
            while (marca[k] && lin[i + k] == marca[k]) k++;
            if (marca[k]) continue;
            int n = 0, j = i + k;
            while (lin[j] && lin[j] != stop && n < cap - 1) d[n++] = lin[j++];
            d[n] = 0;
            return 1;
        }
    }
    d[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ widget de terminal */

#define TERM_LINEAS 300
#define TERM_COL 220
static char term_lin[TERM_LINEAS][TERM_COL];
static int term_total;                      /* Lineas agregadas desde el principio (la ultima es term_total - 1) */
static char term_ent[200];
static int term_len;
static char term_hist[16][200];
static int term_nhist, term_ihist = -1;
static int term_chip_x[16], term_chip_y, term_chip_an[16], term_chip_al, term_nchips;
static const char *const term_chips[] = { "ayuda", "gpu", "lspci", "memoria probar", "paginacion", "calc 42 * 2 + 10", "orar",
                                          "bateria", "dmesg", "stream", "red arp", "huevo", "ruleta", "cangrejo", "limpiar", "consola" };
#define TERM_NCHIPS 16
static int term_chip_sel = -1;

static char *term_nueva(void) { char *l = term_lin[term_total % TERM_LINEAS]; l[0] = 0; term_total++; return l; }
static void term_poner(const char *s, uint32_t color) {
    char *l = term_nueva();
    int n = 0;
    uint8_t r = (uint8_t)(color >> 16), v = (uint8_t)(color >> 8), a = (uint8_t)color;   /* Sin bytes 0, como el nucleo */
    l[n++] = 1; l[n++] = (char)(r ? r : 1); l[n++] = (char)(v ? v : 1); l[n++] = (char)(a ? a : 1);
    while (*s && n < TERM_COL - 1) l[n++] = *s++;
    l[n] = 0;
}
/* Pega la salida capturada: corta en lineas y arrastra el color vigente a cada linea nueva */
static void term_pegar(const char *b) {
    char col[4] = { 1, (char)0xE0, (char)0xE0, (char)0xE0 };
    while (*b) {
        char *l = term_nueva();
        int n = 0;
        memcpy(l, col, 4); n = 4;
        while (*b && *b != '\n') {
            if (*b == 1 && b[1] && b[2] && b[3]) { memcpy(col, b, 4); if (n + 4 < TERM_COL - 1) { memcpy(l + n, b, 4); n += 4; } b += 4; continue; }
            if (n < TERM_COL - 1) l[n++] = *b;
            b++;
        }
        l[n] = 0;
        if (*b == '\n') b++;
    }
}
static void term_bienvenida(void) {
    if (term_total) return;
    term_poner("==============================================================", 0xFFD700);
    term_poner("  TERMINAL TAEK EN LA INTERFAZ // órdenes reales del núcleo", 0xFFFFFF);
    term_poner("  Las de texto se ven aquí; las de pantalla completa (ruleta, cangrejo,", 0xA8A8B4);
    term_poner("  stress...) cierran la interfaz un momento y vuelven al terminar.", 0xA8A8B4);
    term_poner("==============================================================", 0xFFD700);
    term_poner("Escribe 'ayuda'. [TAB] elige una sugerencia, [ESC] suelta el teclado.", 0x00F0FF);
}

/* Ejecuta lo escrito. Devuelve una accion para el nucleo (orden de pantalla, consola) o 0. */
static int term_ejecutar(const char *cmd) {
    while (*cmd == ' ') cmd++;
    if (!*cmd) return 0;
    if (term_nhist < 16) { memcpy(term_hist[term_nhist++], cmd, sizeof(term_hist[0])); }
    else { for (int i = 1; i < 16; i++) memcpy(term_hist[i - 1], term_hist[i], sizeof(term_hist[0])); memcpy(term_hist[15], cmd, sizeof(term_hist[0])); }
    term_ihist = -1;
    struct txt t; t_ini(&t); t_add(&t, "sudo@taek-os:~# "); t_add(&t, cmd);
    term_poner(t.s, NEON);
    if (!ap_comparar(cmd, "limpiar") || !ap_comparar(cmd, "cls") || !ap_comparar(cmd, "clear")) { term_total = 0; term_bienvenida(); return 0; }
    if (!ap_comparar(cmd, "consola") || !ap_comparar(cmd, "salir")) return TAEK_ACCION_TERMINAL;
    long r = orden_texto(cmd);
    if (r == -TAEK_EPANTALLA) {
        int a = orden_pantalla(cmd);
        if (a) { term_poner("Pantalla completa: la interfaz vuelve cuando termine.", 0xFFD700); return a; }
        term_poner("El núcleo no aceptó pedirla.", ROJO);
        return 0;
    }
    if (r < 0) { term_poner(orden_error(r), ROJO); return 0; }
    term_pegar(ord_buf);
    if (ord_o.truncado) term_poner("(la salida era más larga y se cortó)", 0xFFD700);
    return 0;
}

/* Un caracter UTF-8 de la linea, con su ancho; avanza p */
static int term_glifo(const char **p, char *g) {
    const unsigned char *u = (const unsigned char *)*p;
    int n = u[0] < 0x80 ? 1 : (u[0] & 0xE0) == 0xC0 ? 2 : (u[0] & 0xF0) == 0xE0 ? 3 : 4;
    for (int i = 0; i < n; i++) { if (!u[i]) { n = i; break; } g[i] = (char)u[i]; }
    g[n] = 0;
    *p += n ? n : 1;
    return n;
}
/* Cuantas filas ocupa una linea al ancho dado (dibujar = 0) o la dibuja hacia abajo desde y (dibujar = 1) */
static int term_linea(struct lienzo *l, const char *s, int x, int y, int an, int px, int alto, int dibujar) {
    uint32_t col = 0xE0E0E0u;
    int cx = 0, filas = 1;
    char g[5];
    while (*s) {
        if (*s == 1 && s[1] && s[2] && s[3]) { col = (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3]; s += 4; continue; }
        if ((unsigned char)*s < 32) { s++; continue; }
        term_glifo(&s, g);
        if ((unsigned char)g[0] >= 0xE0) { g[0] = '-'; g[1] = 0; }   /* La fuente no tiene simbolos de 3 bytes (cajas) */
        int w = texto_ancho(px, g);
        if (cx + w > an) { cx = 0; filas++; }
        if (dibujar) texto(l, x + cx, y + (filas - 1) * alto, px, col, 255, 0, g);
        cx += w;
    }
    return filas;
}

/* Area de salida, linea de orden y sugerencias. El texto se dibuja de abajo hacia arriba. */
static void term_dibujar(struct lienzo *l, int x, int y, int an, int al, int con_foco) {
    int px = S(13), alto = S(18);
    int chips_al = S(62), prompt_al = S(36);
    int sal_al = al - chips_al - prompt_al;
    rect(l, x, y, an, al, premul(235, 0x050608u));
    rect(l, x, y, an, 1, premul(120, CIAN));
    /* Salida */
    int yb = y + sal_al - S(6), ini = term_total > TERM_LINEAS ? term_total - TERM_LINEAS : 0;
    for (int i = term_total - 1; i >= ini; i--) {
        const char *s = term_lin[i % TERM_LINEAS];
        int f = term_linea(l, s, 0, 0, an - S(24), px, alto, 0);
        int y0 = yb - f * alto;
        if (y0 < y + S(4)) break;
        term_linea(l, s, x + S(12), y0 + alto - S(4), an - S(24), px, alto, 1);
        yb = y0;
    }
    /* Orden */
    int py = y + sal_al;
    rect(l, x, py, an, prompt_al, con_foco ? premul(255, 0x101820u) : premul(255, 0x0B0C10u));
    rect(l, x, py, an, 1, premul(con_foco ? 220 : 90, NEON));
    int xx = texto(l, x + S(10), py + S(24), S(15), NEON, 255, 0, "sudo@taek-os:~#");
    xx = texto(l, xx + S(8), py + S(24), S(15), BLANCO, 255, 0, term_ent);
    if (con_foco && (taek_ms() / 500) % 2) rect(l, xx + S(2), py + S(10), S(9), S(17), OPACO(NEON));
    if (!con_foco && !term_len) texto(l, xx + S(8), py + S(24), S(13), 0x6A6E7Cu, 255, 0, "[INTRO] para escribir");
    /* Sugerencias */
    int cx = x + S(8), cy = py + prompt_al + S(8);
    term_chip_y = cy; term_chip_al = S(22); term_nchips = 0;
    for (int i = 0; i < TERM_NCHIPS; i++) {
        int w = texto_ancho(S(12), term_chips[i]) + S(16);
        if (cx + w > x + an - S(8)) { cx = x + S(8); cy += S(26); if (cy + S(22) > y + al) break; }
        int sel = i == term_chip_sel;
        rect(l, cx, cy, w, S(22), sel ? OPACO(NEON) : premul(200, 0x1A1D26u));
        redondo_borde(l, cx, cy, w, S(22), 0, premul(160, i >= 12 ? ROJO : CIAN));
        texto(l, cx + S(8), cy + S(16), S(12), sel ? NEGRO : (i >= 12 ? 0xFF8A9Au : 0xC8F6FFu), 255, 0, term_chips[i]);
        term_chip_x[i] = cx; term_chip_an[i] = w;
        if (i == 0) term_chip_y = cy;
        term_nchips = i + 1;
        cx += w + S(6);
    }
}

/* Tecla con el teclado tomado por la terminal. Devuelve una accion o 0; *soltar = 1 si se suelta (Esc). */
static int term_tecla(uint32_t k, int *soltar) {
    *soltar = 0;
    if (k == 27) { *soltar = 1; return 0; }
    if (k == '\n' || k == '\r') {
        char c[200]; memcpy(c, term_ent, sizeof(c));
        term_len = 0; term_ent[0] = 0; term_chip_sel = -1;
        return term_ejecutar(c);
    }
    if (k == 8 || k == 127) { if (term_len) { do term_len--; while (term_len && ((unsigned char)term_ent[term_len] & 0xC0) == 0x80); term_ent[term_len] = 0; } return 0; }
    if (k == 9) {   /* Tab: la siguiente sugerencia */
        term_chip_sel = (term_chip_sel + 1) % TERM_NCHIPS;
        int n = 0; while (term_chips[term_chip_sel][n] && n < 198) { term_ent[n] = term_chips[term_chip_sel][n]; n++; }
        term_ent[n] = 0; term_len = n;
        return 0;
    }
    if (k == TAEK_TECLA_ARRIBA || k == TAEK_TECLA_ABAJO) {
        if (!term_nhist) return 0;
        if (k == TAEK_TECLA_ARRIBA) term_ihist = term_ihist < 0 ? term_nhist - 1 : (term_ihist > 0 ? term_ihist - 1 : 0);
        else term_ihist = term_ihist < 0 ? -1 : (term_ihist + 1 < term_nhist ? term_ihist + 1 : -1);
        if (term_ihist < 0) { term_ent[0] = 0; term_len = 0; }
        else { memcpy(term_ent, term_hist[term_ihist], sizeof(term_ent)); term_len = 0; while (term_ent[term_len]) term_len++; }
        return 0;
    }
    if (k >= 32 && k < 127 && term_len < 190) { term_ent[term_len++] = (char)k; term_ent[term_len] = 0; }
    else if (k >= 0xA0 && k < 0x100 && term_len < 188) { term_ent[term_len++] = (char)(0xC0 | k >> 6); term_ent[term_len++] = (char)(0x80 | (k & 0x3F)); term_ent[term_len] = 0; }
    return 0;
}
/* Clic en una sugerencia: se ejecuta */
static int term_clic(int x, int y, int *hecho) {
    *hecho = 0;
    for (int i = 0; i < term_nchips; i++) {
        /* La fila de cada sugerencia: se recalcula igual que al dibujar */
        if (x >= term_chip_x[i] && x < term_chip_x[i] + term_chip_an[i]) {
            int fila = 0;
            for (int j = 1; j <= i; j++) if (term_chip_x[j] < term_chip_x[j - 1]) fila++;
            int cy = term_chip_y + fila * S(26);
            if (y >= cy && y < cy + S(22)) { *hecho = 1; return term_ejecutar(term_chips[i]); }
        }
    }
    return 0;
}
