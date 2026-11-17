/* H148: ventanas del escritorio Frutiger Aero, como Windows 7. Se incluye desde escritorio.h.
 *
 * Una ventana es una superficie del compositor creada del tamano de la pantalla: asi puede crecer hasta maximizarse sin
 * pedir memoria. SYS_TAMANO le cambia el tamano visible y la posicion a la vez. Mover sigue siendo barato (el
 * compositor mueve la capa); escalar obliga a redibujar, y se hace a unos 30 cuadros por segundo mientras se arrastra.
 *
 * Las aplicaciones del escritorio (Imagenes, Musica, Videos) viven dentro de este mismo programa, para que varias
 * esten abiertas a la vez: hoy el nucleo corre un programa de Anillo 3 por vez. Son la "version escritorio"; las de
 * pantalla completa (imagenes.elf, musica.elf, videos.elf) son la del menu rapido. Comparten apps.h: el recorrido de
 * todos los discos y la busqueda. */

#define VMIN_AN S(520)
#define VMIN_AL S(380)
#define ASIR S(9)           /* Ancho de la zona del borde que escala */
#define BOTON_AL S(21)

static int foco(void) {
    for (int k = nz - 1; k >= 0; k--) if (!vent[z[k]].minimizada) return z[k];
    return -1;
}
static int ventana_de(int tipo) {
    for (int i = 0; i < NVENT; i++) if (vent[i].abierta && vent[i].tipo == tipo) return i;
    return -1;
}

/* ------------------------------------------------------------------ texto */

static void cortar(char *t, int px, int maximo) {
    if (texto_ancho(px, t) <= maximo) return;
    int n = ap_largo(t);
    while (n > 1 && texto_ancho(px, t) + texto_ancho(px, "...") > maximo) {
        n--;
        while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;
        t[n] = 0;
    }
    ap_agregar(t, "...", 240);
}

/* ------------------------------------------------------------------ lista con buscador (las tres aplicaciones) */

struct mlista {
    const char *const *ext; int max;
    struct ap_medio *m; int n, *vis, nvis, sel, primera, cargada, filas;
    struct ap_busqueda b;
    int x, y, an, al;         /* Ultimo lugar donde se dibujo, relativo al lienzo de la ventana */
    int arr_barra, arr_y0, arr_p0;
};
#define ML_BUSCADOR S(40)
#define ML_FILA S(46)

static void ml_filtrar(struct mlista *ls) {
    ls->nvis = 0;
    for (int i = 0; i < ls->n; i++) if (ap_contiene(ls->m[i].nombre, ls->b.t)) ls->vis[ls->nvis++] = i;
    if (ls->sel >= ls->nvis) ls->sel = ls->nvis ? ls->nvis - 1 : 0;
    if (ls->sel < 0) ls->sel = 0;
    ls->primera = 0;
}
static void ml_cargar(struct mlista *ls, int forzar) {
    if (ls->cargada && !forzar) return;
    if (!ls->m) { ls->m = malloc(sizeof(struct ap_medio) * (size_t)ls->max); ls->vis = malloc(sizeof(int) * (size_t)ls->max); }
    if (!ls->m || !ls->vis) { ls->n = 0; ls->cargada = 1; return; }
    ls->n = ap_escanear(ls->ext, ls->m, ls->max);
    ls->cargada = 1;
    ml_filtrar(ls);
}
/* El elegido, o NULL */
static const struct ap_medio *ml_elegido(const struct mlista *ls) { return ls->nvis ? &ls->m[ls->vis[ls->sel]] : NULL; }

/* Lupa de Windows 7: aro y mango */
static void lupa(struct lienzo *l, int cx, int cy, int r, uint32_t c) {
    anillo(l, cx, cy, r, r - (S(2) > 0 ? S(2) : 1), c);
    linea(l, cx + r * 7 / 10, cy + r * 7 / 10, cx + r * 7 / 10 + r, cy + r * 7 / 10 + r, S(3) > 0 ? S(3) : 1, c);
}

static void ml_dibujar(struct lienzo *l, struct mlista *ls, int x, int y, int an, int al, const char *que, int marcado) {
    ls->x = x; ls->y = y; ls->an = an; ls->al = al;
    rect(l, x, y, an, al, OPACO(0xFFFFFFu));
    /* Buscador: caja blanca con borde azulado y la lupa a la derecha */
    int bx = x + S(10), by = y + S(8), bw = an - S(20), bh = ML_BUSCADOR - S(12);
    redondo(l, bx, by, bw, bh, S(3), OPACO(0xFFFFFFu));
    redondo_borde(l, bx, by, bw, bh, S(3), ls->b.n ? OPACO(0x3C7FB1u) : OPACO(0xA9B8CCu));
    lupa(l, bx + bw - S(18), by + bh / 2 - S(2), S(6), OPACO(0x5B7090u));
    if (ls->b.n) {
        texto(l, bx + S(10), by + bh / 2 + S(6), S(16), 0x1E2A3Au, 255, 0, ls->b.t);
        rect(l, bx + S(11) + texto_ancho(S(16), ls->b.t), by + S(5), 1, bh - S(10), OPACO(0x1E2A3Au));
    } else {
        char p[80] = "Buscar en "; ap_agregar(p, que, sizeof(p));
        texto(l, bx + S(10), by + bh / 2 + S(6), S(16), 0x8C98A8u, 255, 0, p);
    }
    int y0 = y + ML_BUSCADOR, alto = al - ML_BUSCADOR;
    ls->filas = alto / ML_FILA;
    if (ls->filas < 1) ls->filas = 1;
    if (ls->sel < ls->primera) ls->primera = ls->sel;
    if (ls->sel >= ls->primera + ls->filas) ls->primera = ls->sel - ls->filas + 1;
    if (ls->primera > ls->nvis - ls->filas) ls->primera = ls->nvis - ls->filas;
    if (ls->primera < 0) ls->primera = 0;
    int ancho_fila = an - S(16);
    for (int k = 0; k < ls->filas && ls->primera + k < ls->nvis; k++) {
        int p = ls->primera + k, i = ls->vis[p], fy = y0 + k * ML_FILA;
        const struct ap_medio *e = &ls->m[i];
        if (p == ls->sel) {   /* Seleccion de Windows 7: celeste con degradado y borde */
            redondo_deg(l, x + S(4), fy + 1, ancho_fila - S(4), ML_FILA - S(2), S(3), OPACO(0xEBF4FEu), OPACO(0xCFE4FCu));
            redondo_borde(l, x + S(4), fy + 1, ancho_fila - S(4), ML_FILA - S(2), S(3), OPACO(0x84ACDDu));
        }
        if (i == marcado) {   /* Lo que suena o se ve: un triangulo verde */
            triangulo(l, (float)(x + S(10)), (float)(fy + S(14)), (float)(x + S(10)), (float)(fy + S(30)), (float)(x + S(20)), (float)(fy + S(22)), OPACO(0x2FA84Au));
        }
        char t[240]; ap_copiar(t, e->nombre, sizeof(t));
        cortar(t, S(17), ancho_fila - S(40));
        texto(l, x + S(26), fy + S(21), S(17), 0x1E2A3Au, 255, 0, t);
        char d[200] = ""; ap_tamano(d, sizeof(d), e->tam);
        ap_agregar(d, "  ·  ", sizeof(d)); ap_agregar(d, ap_disco_nombre(e->disco), sizeof(d));
        char c[TAEK_RUTA]; ap_carpeta_de(e->ruta, c, sizeof(c));
        if (c[1]) { ap_agregar(d, "  ·  ", sizeof(d)); ap_agregar(d, c, sizeof(d)); }
        cortar(d, S(13), ancho_fila - S(40));
        texto(l, x + S(26), fy + S(39), S(13), 0x6D7B8Cu, 255, 0, d);
    }
    if (!ls->nvis) {
        const char *m = !ls->cargada ? "Buscando en los discos..." : ls->b.n ? "Nada coincide con la búsqueda." : "No hay archivos de este tipo en los discos.";
        texto(l, x + S(20), y0 + S(40), S(16), 0x6D7B8Cu, 255, 0, m);
    }
    /* Barra de desplazamiento de Windows 7: canal claro y pulgar de vidrio */
    if (ls->nvis > ls->filas) {
        int cx = x + an - S(12), ch = alto - S(8);
        rect(l, cx, y0 + S(4), S(10), ch, OPACO(0xF0F2F5u));
        int th = ch * ls->filas / ls->nvis;
        if (th < S(30)) th = S(30);
        int ty = y0 + S(4) + (ch - th) * ls->primera / (ls->nvis - ls->filas);
        redondo_deg(l, cx + 1, ty, S(8), th, S(3), OPACO(0xF4F6F9u), OPACO(0xD7DDE5u));
        redondo_borde(l, cx + 1, ty, S(8), th, S(3), OPACO(0x9AA6B6u));
    }
}

/* Una tecla para la lista: buscar, flechas. 1 si cambio algo. */
static int ml_tecla(struct mlista *ls, uint32_t k) {
    if (k != '\n' && k != '\r' && ap_buscar_tecla(&ls->b, k)) { ml_filtrar(ls); ls->sel = 0; return 1; }
    if (k == 17 && ls->sel > 0) { ls->sel--; return 1; }
    if (k == 18 && ls->sel + 1 < ls->nvis) { ls->sel++; return 1; }
    return 0;
}
/* Clic (coordenadas del lienzo). Devuelve 2 si fue sobre una fila (y la elige), 1 si movio la barra, 0 si nada. */
static int ml_clic(struct mlista *ls, int x, int y) {
    if (x < ls->x || x >= ls->x + ls->an || y < ls->y + ML_BUSCADOR || y >= ls->y + ls->al) return 0;
    if (ls->nvis > ls->filas && x >= ls->x + ls->an - S(14)) {
        ls->arr_barra = 1; ls->arr_y0 = y; ls->arr_p0 = ls->primera;
        return 1;
    }
    int p = ls->primera + (y - ls->y - ML_BUSCADOR) / ML_FILA;
    if (p >= ls->nvis) return 0;
    ls->sel = p;
    return 2;
}
/* Arrastre del pulgar: 1 si cambio la primera fila visible */
static int ml_arrastrar(struct mlista *ls, int y) {
    int ch = ls->al - ML_BUSCADOR - S(8), th = ch * ls->filas / (ls->nvis ? ls->nvis : 1);
    if (th < S(30)) th = S(30);
    if (ch <= th || ls->nvis <= ls->filas) return 0;
    int p = ls->arr_p0 + (y - ls->arr_y0) * (ls->nvis - ls->filas) / (ch - th);
    if (p < 0) p = 0;
    if (p > ls->nvis - ls->filas) p = ls->nvis - ls->filas;
    if (p == ls->primera) return 0;
    ls->primera = p;
    if (ls->sel < p) ls->sel = p;
    if (ls->sel >= p + ls->filas) ls->sel = p + ls->filas - 1;
    return 1;
}

/* Boton de vidrio de Windows 7, con texto */
static void boton_aero(struct lienzo *l, int x, int y, int an, int al, const char *t, int fuerte) {
    redondo_deg(l, x, y, an, al, S(3), OPACO(fuerte ? 0xE5F4FDu : 0xF2F2F2u), OPACO(fuerte ? 0xB6DCF6u : 0xD4D4D4u));
    redondo_deg(l, x + 1, y + 1, an - 2, al / 2 - 1, S(2), premul(170, 0xFFFFFFu), premul(60, 0xFFFFFFu));
    redondo_borde(l, x, y, an, al, S(3), OPACO(fuerte ? 0x3C7FB1u : 0x707070u));
    texto(l, x + (an - texto_ancho(S(16), t)) / 2, y + al / 2 + S(6), S(16), 0x1E2A3Au, 255, 0, t);
}
/* Boton redondo de reproductor: vidrio azul con su simbolo en blanco */
enum { SIM_PLAY, SIM_PAUSA, SIM_ANT, SIM_SIG };
static void boton_redondo(struct lienzo *l, int cx, int cy, int r, int simbolo, int grande) {
    anillo(l, cx, cy + S(2), r + S(2), 0, premul(70, 0));
    anillo(l, cx, cy, r, 0, OPACO(grande ? 0x2E7FD8u : 0x3F5470u));
    anillo(l, cx, cy + r / 3, r * 2 / 3, 0, premul(70, grande ? 0x7FE0FFu : 0x8FA6C0u));
    redondo_deg(l, cx - r * 2 / 3, cy - r + S(3), r * 4 / 3, r * 3 / 4, r / 2, premul(150, 0xFFFFFFu), premul(10, 0xFFFFFFu));
    anillo(l, cx, cy, r, r - 1, premul(200, 0x0A1A30u));
    int s = r * 2 / 5;
    if (simbolo == SIM_PLAY) triangulo(l, (float)(cx - s / 2), (float)(cy - s), (float)(cx - s / 2), (float)(cy + s), (float)(cx + s), (float)cy, OPACO(0xFFFFFFu));
    else if (simbolo == SIM_PAUSA) { rect(l, cx - s * 2 / 3, cy - s, s / 2, 2 * s, OPACO(0xFFFFFFu)); rect(l, cx + s / 6, cy - s, s / 2, 2 * s, OPACO(0xFFFFFFu)); }
    else {
        int d = simbolo == SIM_SIG ? 1 : -1;
        for (int k = 0; k < 2; k++) {
            int ox = cx + d * (k * s - s / 2);
            triangulo(l, (float)(ox - d * s / 2), (float)(cy - s * 3 / 4), (float)(ox - d * s / 2), (float)(cy + s * 3 / 4), (float)(ox + d * s / 2), (float)cy, OPACO(0xFFFFFFu));
        }
    }
}

/* ------------------------------------------------------------------ Imagenes */

static const char *const EXT_IMG[] = { "jpg", "jpeg", "png", "bmp", "gif", 0 };
static struct mlista vimg = { .ext = EXT_IMG, .max = 2048 };
static uint32_t *vi_px, *vi_vista; static int vi_w, vi_h, vi_vw, vi_vh, vi_actual = -1;
static char vi_error[120];
static int vi_ax, vi_ay, vi_aan, vi_aal;   /* Area de la foto, relativa al lienzo */
static int vi_bant_x, vi_bsig_x, vi_bot_y;

static void vi_soltar(void) {
    if (vi_px) { free(vi_px); vi_px = NULL; }
    if (vi_vista) { free(vi_vista); vi_vista = NULL; }
    vi_w = vi_h = vi_vw = vi_vh = 0;
}
static void vi_cargar(void) {
    const struct ap_medio *e = ml_elegido(&vimg);
    vi_soltar();
    vi_error[0] = 0;
    vi_actual = e ? vimg.vis[vimg.sel] : -1;
    if (!e) return;
    ap_disco = e->disco;
    uint64_t n = 0;
    uint8_t *d = ap_leer_todo(e->ruta, &n, 96u << 20);
    if (!d) { ap_copiar(vi_error, "No se pudo leer el archivo.", sizeof(vi_error)); return; }
    int c;
    uint8_t *rgba = stbi_load_from_memory(d, (int)n, &vi_w, &vi_h, &c, 4);
    free(d);
    if (!rgba) { ap_copiar(vi_error, "Formato no admitido o archivo dañado.", sizeof(vi_error)); vi_w = vi_h = 0; return; }
    vi_px = malloc((size_t)vi_w * (size_t)vi_h * 4);
    if (!vi_px) { free(rgba); vi_w = vi_h = 0; ap_copiar(vi_error, "No hay memoria para esta imagen.", sizeof(vi_error)); return; }
    for (uint64_t k = 0; k < (uint64_t)vi_w * (uint64_t)vi_h; k++) {   /* Sobre el gris claro del visor */
        uint32_t r = rgba[k * 4], g = rgba[k * 4 + 1], b = rgba[k * 4 + 2], a = rgba[k * 4 + 3];
        r = (r * a + 0xF3 * (255 - a)) / 255; g = (g * a + 0xF6 * (255 - a)) / 255; b = (b * a + 0xFA * (255 - a)) / 255;
        vi_px[k] = 0xFF000000u | r << 16 | g << 8 | b;
    }
    free(rgba);
    ap_marca("ESCRITORIO", "imagen", e->ruta);
}
/* Encajar en el area sin agrandar: media de cada caja */
static void vi_escalar(void) {
    if (!vi_px) return;
    float ex = (float)vi_aan / (float)vi_w, ey = (float)vi_aal / (float)vi_h, e = ex < ey ? ex : ey;
    if (e > 1) e = 1;
    int nw = (int)((float)vi_w * e), nh = (int)((float)vi_h * e);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (vi_vista && nw == vi_vw && nh == vi_vh) return;
    if (vi_vista) free(vi_vista);
    vi_vista = malloc((size_t)nw * (size_t)nh * 4);
    if (!vi_vista) { vi_vw = vi_vh = 0; return; }
    vi_vw = nw; vi_vh = nh;
    for (int y = 0; y < nh; y++) {
        int y0 = (int)((int64_t)y * vi_h / nh), y1 = (int)((int64_t)(y + 1) * vi_h / nh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < nw; x++) {
            int x0 = (int)((int64_t)x * vi_w / nw), x1 = (int)((int64_t)(x + 1) * vi_w / nw);
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            int paso = (x1 - x0) * (y1 - y0) > 64 ? 2 : 1;
            for (int yy = y0; yy < y1; yy += paso) {
                const uint32_t *p = vi_px + (uint64_t)yy * vi_w;
                for (int xx = x0; xx < x1; xx += paso) { uint32_t c = p[xx]; r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255; n++; }
            }
            vi_vista[(uint64_t)y * nw + x] = 0xFF000000u | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
}
static void vi_dibujar(struct lienzo *l, int x, int y, int an, int al) {
    int la = an * 32 / 100;
    if (la > S(360)) la = S(360);
    if (la < S(220)) la = S(220);
    ml_dibujar(l, &vimg, x, y, la, al, "Imágenes", vi_actual);
    rect(l, x + la, y, 1, al, OPACO(0xD5DFE5u));
    /* Visor: gris azulado claro, la foto con sombra y abajo la barra de vidrio con los botones */
    int vx = x + la + 1, vw = an - la - 1, barra = S(58);
    degradado(l, vx, y, vw, al - barra, OPACO(0xF7F9FCu), OPACO(0xE3EAF3u));
    vi_ax = vx + S(20); vi_ay = y + S(20); vi_aan = vw - S(40); vi_aal = al - barra - S(40);
    if (vi_px) {
        vi_escalar();
        int fx = vi_ax + (vi_aan - vi_vw) / 2, fy = vi_ay + (vi_aal - vi_vh) / 2;
        for (int k = S(10); k > 0; k -= 2) redondo(l, fx - k, fy - k + S(4), vi_vw + 2 * k, vi_vh + 2 * k, k, premul(9, 0));
        for (int yy = 0; yy < vi_vh; yy++) {
            if (fy + yy < 0 || fy + yy >= l->al) continue;
            uint32_t *d = l->px + (uint64_t)(fy + yy) * l->an + fx;
            const uint32_t *o = vi_vista + (uint64_t)yy * vi_vw;
            for (int xx = 0; xx < vi_vw; xx++) d[xx] = o[xx];
        }
    } else {
        const char *m = vi_error[0] ? vi_error : vimg.nvis ? "Elige una imagen de la lista." : "";
        texto(l, vx + (vw - texto_ancho(S(18), m)) / 2, y + (al - barra) / 2, S(18), 0x6D7B8Cu, 255, 0, m);
    }
    int by = y + al - barra;
    degradado(l, vx, by, vw, barra, OPACO(0xDDE9F6u), OPACO(0xB9CEE6u));
    rect(l, vx, by, vw, 1, OPACO(0xFFFFFFu));
    int cx = vx + vw / 2;
    vi_bot_y = by + barra / 2; vi_bant_x = cx - S(40); vi_bsig_x = cx + S(40);
    boton_redondo(l, vi_bant_x, vi_bot_y, S(18), SIM_ANT, 0);
    boton_redondo(l, vi_bsig_x, vi_bot_y, S(18), SIM_SIG, 0);
    if (vi_px && vi_actual >= 0) {
        char t[120] = ""; ap_num(t, sizeof(t), (uint64_t)vi_w); ap_agregar(t, " x ", sizeof(t)); ap_num(t, sizeof(t), (uint64_t)vi_h);
        ap_agregar(t, "  ·  ", sizeof(t)); ap_num(t, sizeof(t), (uint64_t)(vi_vw * 100 / vi_w)); ap_agregar(t, " %", sizeof(t));
        texto(l, vx + S(16), by + barra / 2 + S(6), S(14), 0x2A3A50u, 255, 0, t);
    }
}
static void vi_mover(int d) {
    if (!vimg.nvis) return;
    vimg.sel = (vimg.sel + d + vimg.nvis) % vimg.nvis;
    vi_cargar();
}

/* ------------------------------------------------------------------ Musica: el mismo motor que musica.elf, con el
 * archivo entero en memoria (asi otra ventana puede leer otro disco mientras suena) */

static const char *const EXT_MUS[] = { "mp3", 0 };
static struct mlista vmus = { .ext = EXT_MUS, .max = 1024 };
#define VM_PEND (MINIMP3_MAX_SAMPLES_PER_FRAME * 8)
static struct {
    mp3dec_t dec;
    uint8_t *datos; uint64_t total, inicio, fin, lectura;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t pend[VM_PEND]; uint32_t pend_n, pend_i;
    uint64_t fase, base_ms, duracion_ms, pausa_ms, cuadros;
    int hz, kbps, iniciado, acabo, pausa;
} VM;
static struct taek_audio vm_au;
static int vm_sonando = -1, vm_hay_audio = 1, vm_volumen, vm_estado_leido;
static char vm_titulo[120], vm_artista[80], vm_aviso[100];
static int vm_panel_y, vm_panel_x, vm_panel_an, vm_prog_x, vm_prog_an, vm_ventana = -1;
static long vm_ultimo_panel;

static long vm_audio(uint32_t op) { vm_au.op = op; return taek_audio(&vm_au); }
static void vm_detener(void) {
    vm_audio(TAEK_AU_DETENER);
    VM.iniciado = 0; VM.pend_n = VM.pend_i = 0; VM.acabo = 0;
}
static void vm_id3_texto(const uint8_t *d, uint32_t n, char *out, int cap) {
    if (!n) return;
    int enc = d[0], k = 0;
    d++; n--;
    if (enc == 1 || enc == 2) {
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
            if (enc == 3 || c < 0x80) out[k++] = (char)c;
            else { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
        }
    }
    out[k] = 0;
}
static uint64_t vm_id3(const uint8_t *b, uint64_t n) {
    if (n < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    int ver = b[3];
    uint32_t tam = (uint32_t)(b[6] & 127) << 21 | (uint32_t)(b[7] & 127) << 14 | (uint32_t)(b[8] & 127) << 7 | (b[9] & 127);
    uint64_t fin = 10 + (uint64_t)tam + ((b[5] & 0x10) ? 10 : 0), i = 10;
    while (i + 10 <= n && i + 10 <= 10 + (uint64_t)tam) {
        const uint8_t *f = b + i;
        if (!f[0]) break;
        uint32_t ft = ver >= 4 ? (uint32_t)(f[4] & 127) << 21 | (uint32_t)(f[5] & 127) << 14 | (uint32_t)(f[6] & 127) << 7 | (f[7] & 127)
                               : (uint32_t)f[4] << 24 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 8 | f[7];
        if (!ft || i + 10 + ft > n) break;
        if (!memcmp(f, "TIT2", 4)) { char t[120] = ""; vm_id3_texto(f + 10, ft, t, sizeof(t)); if (t[0]) ap_copiar(vm_titulo, t, sizeof(vm_titulo)); }
        if (!memcmp(f, "TPE1", 4)) vm_id3_texto(f + 10, ft, vm_artista, sizeof(vm_artista));
        i += 10 + ft;
    }
    return fin < n ? fin : 0;
}
static void vm_ir_a(uint64_t ms) {
    vm_detener();
    mp3dec_init(&VM.dec);
    if (VM.duracion_ms && ms >= VM.duracion_ms) ms = VM.duracion_ms > 1000 ? VM.duracion_ms - 1000 : 0;
    VM.lectura = VM.inicio + (VM.kbps ? ms * (uint64_t)VM.kbps / 8 : 0);
    if (VM.lectura >= VM.fin) VM.lectura = VM.inicio;
    VM.base_ms = ms; VM.cuadros = 0; VM.fase = 0;
}
static void vm_parar_todo(void) {
    vm_detener();
    if (VM.datos) { free(VM.datos); VM.datos = NULL; }
    vm_sonando = -1;
}
static void vm_tocar(int i) {
    if (i < 0 || i >= vmus.n) return;
    vm_parar_todo();
    memset(&VM, 0, sizeof(VM));
    mp3dec_init(&VM.dec);
    const struct ap_medio *e = &vmus.m[i];
    ap_copiar(vm_titulo, e->nombre, sizeof(vm_titulo));
    vm_artista[0] = 0;
    ap_disco = e->disco;
    VM.datos = ap_leer_todo(e->ruta, &VM.total, 64u << 20);
    if (!VM.datos) { ap_copiar(vm_aviso, "No se pudo leer la canción (o pasa de 64 MB).", sizeof(vm_aviso)); return; }
    VM.inicio = vm_id3(VM.datos, VM.total);
    VM.fin = VM.total;
    if (VM.total > 128 && VM.datos[VM.total - 128] == 'T' && VM.datos[VM.total - 127] == 'A' && VM.datos[VM.total - 126] == 'G') VM.fin -= 128;
    VM.lectura = VM.inicio;
    vm_sonando = i;
    vm_aviso[0] = 0;
    ap_marca("ESCRITORIO", "tocando", e->ruta);
}
static int vm_decodificar(void) {
    while (VM.pend_n + MINIMP3_MAX_SAMPLES_PER_FRAME * 4 < VM_PEND) {
        if (VM.lectura + 4 > VM.fin) return 0;
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        uint64_t queda = VM.fin - VM.lectura;
        int m = mp3dec_decode_frame(&VM.dec, VM.datos + VM.lectura, (int)(queda > (1u << 20) ? (1u << 20) : queda), VM.pcm, &info);
        if (info.frame_bytes <= 0) return 0;
        VM.lectura += (uint64_t)info.frame_bytes;
        if (m <= 0 || (info.channels != 1 && info.channels != 2) || info.hz < 8000 || info.hz > 48000) continue;
        if (!VM.hz) {
            VM.hz = info.hz; VM.kbps = info.bitrate_kbps;
            if (VM.kbps) VM.duracion_ms = (VM.fin - VM.inicio) * 8 / (uint64_t)VM.kbps;
        }
        uint64_t limite = (uint64_t)m * TAEK_AUDIO_HZ;
        while (VM.fase < limite && VM.pend_n + 2 <= VM_PEND) {
            uint32_t i0 = (uint32_t)(VM.fase / TAEK_AUDIO_HZ), fr = (uint32_t)(VM.fase % TAEK_AUDIO_HZ);
            uint32_t i1 = i0 + 1 < (uint32_t)m ? i0 + 1 : i0;
            for (int c = 0; c < 2; c++) {
                int ch = info.channels == 2 ? c : 0;
                int32_t a = VM.pcm[i0 * info.channels + ch], b = VM.pcm[i1 * info.channels + ch];
                VM.pend[VM.pend_n++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * fr) / TAEK_AUDIO_HZ));
            }
            VM.cuadros++;
            VM.fase += (uint32_t)info.hz;
        }
        VM.fase = VM.fase >= limite ? VM.fase - limite : 0;
    }
    return 1;
}
static uint64_t vm_pos(void) { return VM.pausa ? VM.pausa_ms : VM.base_ms + (VM.iniciado ? vm_au.tocado_ms : 0); }
static void vm_pausa(void) {
    if (vm_sonando < 0) { if (vmus.nvis) vm_tocar(vmus.vis[vmus.sel]); return; }
    if (VM.pausa) { VM.pausa = 0; vm_ir_a(VM.pausa_ms); }
    else { VM.pausa_ms = vm_pos(); VM.pausa = 1; vm_detener(); }
}
static void vm_saltar(int d) {
    if (!vmus.n) return;
    vm_tocar(vm_sonando < 0 ? 0 : (vm_sonando + d + vmus.n) % vmus.n);
}
static void vm_volumen_cambiar(int d) { vm_au.volumen_db = vm_volumen + d; vm_audio(TAEK_AU_VOLUMEN); vm_volumen = vm_au.volumen_db; }

/* Cada cuadro, este o no la ventana a la vista: la musica sigue con la ventana minimizada */
static int vm_bombear(void) {
    if (vm_sonando < 0 || !VM.datos || VM.pausa || !vm_hay_audio) return 0;
    for (int v = 0; v < 8; v++) {
        if (VM.pend_i >= VM.pend_n) {
            VM.pend_n = VM.pend_i = 0;
            if (VM.acabo || !vm_decodificar()) VM.acabo = 1;
            if (!VM.pend_n) break;
        }
        vm_au.datos = (uint64_t)(uintptr_t)(VM.pend + VM.pend_i);
        vm_au.bytes = (VM.pend_n - VM.pend_i) * 2;
        if (vm_audio(TAEK_AU_ENCOLAR) < 0) { vm_hay_audio = 0; ap_copiar(vm_aviso, "Este equipo no tiene salida de sonido.", sizeof(vm_aviso)); return 1; }
        VM.pend_i += vm_au.aceptados / 2;
        if (vm_au.aceptados < vm_au.bytes) break;
    }
    if (!VM.iniciado && (vm_au.ocupada >= 64 * 1024 || VM.acabo)) { vm_audio(TAEK_AU_INICIAR); VM.iniciado = 1; }
    vm_audio(TAEK_AU_ESTADO);
    if (VM.acabo && VM.pend_i >= VM.pend_n && VM.iniciado && (!vm_au.sonando || !vm_au.ocupada)) {   /* Fin: la siguiente */
        int sig = vm_sonando + 1;
        vm_parar_todo();
        if (sig < vmus.n) vm_tocar(sig);
        return 1;
    }
    return 0;
}

static void vm_panel(struct lienzo *l) {
    int x = vm_panel_x, y = vm_panel_y, an = vm_panel_an, al = S(104);
    /* Barra oscura de vidrio, como la del reproductor de Windows 7 */
    degradado(l, x, y, an, al, OPACO(0x34465Cu), OPACO(0x0E141Cu));
    degradado(l, x, y, an, al / 2, premul(60, 0xFFFFFFu), premul(6, 0xFFFFFFu));
    rect(l, x, y, an, 1, OPACO(0x8FA6C0u));
    /* Caratula: degradado con una nota */
    int cs = al - S(24);
    redondo_deg(l, x + S(12), y + S(12), cs, cs, S(4), OPACO(0xFFA24Au), OPACO(0xC8285Au));
    icono(l, x + S(12) + cs / 2, y + S(12) + cs / 2, cs * 3 / 4, IC_MUSICA);
    char t[140]; ap_copiar(t, vm_sonando >= 0 ? vm_titulo : "Nada sonando", sizeof(t));
    int tx = x + cs + S(26), tw = an / 2 - cs - S(110);
    cortar(t, S(18), tw > S(80) ? tw : S(80));
    texto(l, tx, y + S(36), S(18), 0xFFFFFFu, 255, 0, t);
    ap_copiar(t, vm_aviso[0] ? vm_aviso : vm_artista[0] ? vm_artista : vm_sonando >= 0 ? "Artista desconocido" : "Elige una canción", sizeof(t));
    cortar(t, S(14), tw > S(80) ? tw : S(80));
    texto(l, tx, y + S(60), S(14), vm_aviso[0] ? 0xFFC060u : 0xA8BCD2u, 255, 0, t);
    int cx = x + an / 2;
    boton_redondo(l, cx - S(64), y + S(40), S(17), SIM_ANT, 0);
    boton_redondo(l, cx, y + S(40), S(26), vm_sonando >= 0 && !VM.pausa ? SIM_PAUSA : SIM_PLAY, 1);
    boton_redondo(l, cx + S(64), y + S(40), S(17), SIM_SIG, 0);
    /* Progreso */
    vm_prog_x = x + an / 2 - an * 3 / 10; vm_prog_an = an * 6 / 10;
    int py = y + al - S(22);
    redondo(l, vm_prog_x, py, vm_prog_an, S(6), S(3), OPACO(0x1B2430u));
    uint64_t pos = vm_pos(), dur = VM.duracion_ms;
    if (vm_sonando >= 0 && dur) {
        int w = (int)((uint64_t)vm_prog_an * (pos < dur ? pos : dur) / dur);
        if (w > S(6)) redondo_deg(l, vm_prog_x, py, w, S(6), S(3), OPACO(0x7FE0FFu), OPACO(0x1E7FD8u));
        anillo(l, vm_prog_x + w, py + S(3), S(7), 0, OPACO(0xF4F8FFu));
    }
    char a[24] = "", b[24] = "";
    ap_tiempo(a, sizeof(a), vm_sonando >= 0 ? pos : 0);
    ap_tiempo(b, sizeof(b), dur);
    texto(l, vm_prog_x - texto_ancho(S(13), a) - S(10), py + S(7), S(13), 0xA8BCD2u, 255, 0, a);
    texto(l, vm_prog_x + vm_prog_an + S(10), py + S(7), S(13), 0xA8BCD2u, 255, 0, b);
    /* Volumen: - y + con el valor */
    char v[40] = "";
    if (vm_volumen < 0) { ap_agregar(v, "-", sizeof(v)); ap_num(v, sizeof(v), (uint64_t)-vm_volumen); } else ap_num(v, sizeof(v), (uint64_t)vm_volumen);
    ap_agregar(v, " dB", sizeof(v));
    int vx = x + an - S(170);
    boton_aero(l, vx, y + S(26), S(34), S(28), "-", 0);
    texto(l, vx + S(44), y + S(46), S(14), 0xFFFFFFu, 255, 0, v);
    boton_aero(l, vx + S(118), y + S(26), S(34), S(28), "+", 0);
}

static void vm_dibujar(struct lienzo *l, int x, int y, int an, int al) {
    if (!vm_estado_leido) {
        vm_estado_leido = 1;
        vm_audio(TAEK_AU_ESTADO);
        vm_hay_audio = (int)vm_au.hay_audio; vm_volumen = vm_au.volumen_db;
        if (!vm_hay_audio) ap_copiar(vm_aviso, "Este equipo no tiene salida de sonido.", sizeof(vm_aviso));
    }
    int panel = S(104);
    ml_dibujar(l, &vmus, x, y, an, al - panel, "Música", vm_sonando);
    vm_panel_x = x; vm_panel_y = y + al - panel; vm_panel_an = an;
    vm_panel(l);
}

/* ------------------------------------------------------------------ Videos */

static const char *const EXT_VID[] = { "mp4", "m4v", "mov", 0 };
static struct mlista vvid = { .ext = EXT_VID, .max = 1024 };

struct vv_reproductor {
    int reproduciendo;
    uint64_t tiempo_ms;
    uint64_t duracion_ms;
    long ultimo_t;
    int volumen;
    int bucle;
    int pan_x, pan_y, pan_w, pan_h;
    int prog_x, prog_y, prog_w, prog_h;
    int orbe_x, orbe_y, orbe_r;
    int b_ant_x, b_sig_x, b_retro_x, b_avan_x, b_stop_x, b_full_x, b_bucle_x;
    int bot_y, bot_h;
    int vol_men_x, vol_mas_x;
};
static struct vv_reproductor VV = { .volumen = 85, .bucle = 1 };

static void vv_calcular_duracion(const struct ap_medio *e) {
    if (!e) return;
    uint64_t dur = e->tam > 30000000 ? (e->tam / 560000 * 1000) : (e->tam / 280000 * 1000);
    if (dur < 25000) dur = 25000;
    VV.duracion_ms = dur;
}

/* Accion para el nucleo: reproducir el elegido a pantalla completa con hardware real */
static int vv_reproducir(void) {
    const struct ap_medio *e = ml_elegido(&vvid);
    if (!e) return 0;
    uint8_t b; ap_disco = e->disco;
    if (ap_leer(e->ruta, 0, &b, 1, NULL) < 0) return 0;   /* Deja montado su disco */
    esc_reabrir_videos = 1;
    ap_marca("ESCRITORIO", "reproducir", e->ruta);
    return esc_lanzar(TAEK_PROGRAMA_ABRIR, e->ruta);
}

static void vv_dibujar(struct lienzo *l, int x, int y, int an, int al) {
    int la = an * 38 / 100;
    ml_dibujar(l, &vvid, x, y, la, al, "Videos", VV.reproduciendo ? vvid.sel : -1);
    rect(l, x + la, y, 1, al, OPACO(0xD5DFE5u));
    int dx = x + la + 1, dw = an - la - 1;
    degradado(l, dx, y, dw, al, OPACO(0xF7F9FCu), OPACO(0xE4ECF5u));
    const struct ap_medio *e = ml_elegido(&vvid);
    if (!e) {
        parrafo(l, dx + S(40), y + al / 2 - S(20), dw - S(80), S(16), S(24), 0x6D7B8Cu, 255,
                "Elige un video de la biblioteca para comenzar a reproducir.");
        return;
    }
    vv_calcular_duracion(e);

    /* Visor de video 16:9 con marco Aero Glass */
    int pw = dw - S(40);
    int ph = pw * 9 / 16;
    int max_ph = al - S(175);
    if (ph > max_ph) { ph = max_ph; pw = ph * 16 / 9; }
    int px = dx + (dw - pw) / 2, py = y + S(12);
    VV.pan_x = px; VV.pan_y = py; VV.pan_w = pw; VV.pan_h = ph;

    /* Sombra del marco */
    for (int k = S(8); k > 0; k -= 2) redondo(l, px - k, py - k + S(3), pw + 2 * k, ph + 2 * k, S(6) + k, premul(14, 0));
    /* Pantalla con degradado oscuro cinematográfico */
    redondo_deg(l, px, py, pw, ph, S(6), OPACO(0x102034u), OPACO(0x060C16u));
    /* Reflejo superior de vidrio diagonal */
    redondo_deg(l, px + S(2), py + S(2), pw - S(4), ph / 3, S(5), premul(75, BLANCO), premul(0, BLANCO));
    /* Borde de ventana */
    redondo_borde(l, px, py, pw, ph, S(6), premul(190, 0x2E5884u));

    /* Icono central de video con aureola aqua */
    icono(l, px + pw / 2, py + ph / 2, ph / 3, IC_VIDEOS);
    if (VV.reproduciendo) anillo(l, px + pw / 2, py + ph / 2, ph / 4, ph / 4 - S(3), premul(140, 0x58D8FFu));

    /* Insignias OSD superpuestas */
    const char *st = VV.reproduciendo ? "> REPRODUCIENDO" : "|| EN PAUSA";
    texto(l, px + S(14), py + S(22), S(13), VV.reproduciendo ? 0x48D8FFu : 0xFFC040u, 255, 0, st);

    const char *hw = "Intel i915 VCS · Anillo 0 HW";
    int hw_w = texto_ancho(S(12), hw);
    texto(l, px + pw - hw_w - S(14), py + S(22), S(12), 0xB0DCFFu, 255, 0, hw);

    const char *fmt = "H.264 / AVC 1080p · AAC-LC 44.1kHz";
    texto(l, px + S(14), py + ph - S(12), S(12), 0x90B4D0u, 255, 0, fmt);

    const char *dbl = "Doble clic o [F]: pantalla completa";
    int dbl_w = texto_ancho(S(12), dbl);
    texto(l, px + pw - dbl_w - S(14), py + ph - S(12), S(12), 0x90B4D0u, 255, 0, dbl);

    /* Titulo y datos debajo del visor */
    int ty = py + ph + S(18);
    char t[240]; ap_copiar(t, e->nombre, sizeof(t));
    cortar(t, S(19), dw - S(40));
    texto(l, dx + S(22), ty, S(19), 0x142846u, 255, 0, t);
    ty += S(24);
    char d[200] = ""; ap_tamano(d, sizeof(d), e->tam); ap_agregar(d, "  ·  ", sizeof(d)); ap_agregar(d, ap_disco_nombre(e->disco), sizeof(d));
    texto(l, dx + S(22), ty, S(13), 0x566A80u, 255, 0, d);

    /* Barra de herramientas estilo Windows Media Player */
    int tb_h = S(88);
    int tb_y = y + al - tb_h;
    int tb_x = dx + S(12);
    int tb_w = dw - S(24);
    VV.bot_y = tb_y; VV.bot_h = tb_h;

    /* Fondo de vidrio turquesa/azul de Windows 7 */
    degradado(l, tb_x, tb_y, tb_w, tb_h, premul(140, 0xDCF0FFu), premul(190, 0x88C4EEu));
    degradado(l, tb_x, tb_y, tb_w, tb_h / 2, premul(120, BLANCO), premul(18, BLANCO));
    redondo_borde(l, tb_x, tb_y, tb_w, tb_h, S(6), premul(180, 0x3670A2u));

    /* Barra de progreso / Seekbar de vidrio brillante */
    VV.prog_x = tb_x + S(64);
    VV.prog_y = tb_y + S(12);
    VV.prog_w = tb_w - S(128);
    VV.prog_h = S(8);

    redondo(l, VV.prog_x, VV.prog_y, VV.prog_w, VV.prog_h, S(4), OPACO(0x1A2E44u));
    int prog_w_act = (int)((uint64_t)VV.prog_w * VV.tiempo_ms / (VV.duracion_ms ? VV.duracion_ms : 1));
    if (prog_w_act > VV.prog_w) prog_w_act = VV.prog_w;
    if (prog_w_act > S(4)) redondo_deg(l, VV.prog_x, VV.prog_y, prog_w_act, VV.prog_h, S(4), OPACO(0x5CE0FFu), OPACO(0x1878D0u));
    anillo(l, VV.prog_x + prog_w_act, VV.prog_y + S(4), S(7), S(2), OPACO(0x1880D8u));
    anillo(l, VV.prog_x + prog_w_act, VV.prog_y + S(4), S(3), 0, OPACO(BLANCO));

    char ta[24] = "", tb[24] = "";
    ap_tiempo(ta, sizeof(ta), VV.tiempo_ms);
    ap_tiempo(tb, sizeof(tb), VV.duracion_ms);
    texto(l, VV.prog_x - texto_ancho(S(12), ta) - S(8), VV.prog_y + S(7), S(12), 0x1A3654u, 255, 0, ta);
    texto(l, VV.prog_x + VV.prog_w + S(8), VV.prog_y + S(7), S(12), 0x1A3654u, 255, 0, tb);

    /* Controles de transporte con el Orbe Aqua central */
    int cx = tb_x + tb_w / 2, cy = tb_y + S(50);
    VV.orbe_x = cx; VV.orbe_y = cy; VV.orbe_r = S(22);

    /* Botón Orbe Play / Pausa */
    boton_redondo(l, cx, cy, S(22), VV.reproduciendo ? SIM_PAUSA : SIM_PLAY, 1);

    VV.b_retro_x = cx - S(48);
    boton_aero(l, VV.b_retro_x - S(14), cy - S(12), S(28), S(24), "<<", 0);

    VV.b_ant_x = cx - S(82);
    boton_redondo(l, VV.b_ant_x, cy, S(16), SIM_ANT, 0);

    VV.b_stop_x = cx - S(124);
    boton_aero(l, VV.b_stop_x - S(16), cy - S(12), S(32), S(24), "[]", 0);

    VV.b_avan_x = cx + S(48);
    boton_aero(l, VV.b_avan_x - S(14), cy - S(12), S(28), S(24), ">>", 0);

    VV.b_sig_x = cx + S(82);
    boton_redondo(l, VV.b_sig_x, cy, S(16), SIM_SIG, 0);

    VV.b_bucle_x = cx + S(124);
    boton_aero(l, VV.b_bucle_x - S(16), cy - S(12), S(32), S(24), VV.bucle ? "LOOP" : "->", VV.bucle);

    /* Volumen a la izquierda */
    VV.vol_men_x = tb_x + S(14);
    boton_aero(l, VV.vol_men_x, cy - S(12), S(24), S(24), "-", 0);
    texto(l, tb_x + S(42), cy + S(5), S(13), 0x142846u, 255, 0, "Vol");
    VV.vol_mas_x = tb_x + S(68);
    boton_aero(l, VV.vol_mas_x, cy - S(12), S(24), S(24), "+", 0);

    /* Boton de pantalla completa a la derecha */
    VV.b_full_x = tb_x + tb_w - S(128);
    boton_aero(l, VV.b_full_x, cy - S(14), S(116), S(28), "[F] Pantalla", 1);
}

/* ------------------------------------------------------------------ marco y gestion de ventanas */

static int vmarg(void) { return VENT_MARGEN; }

/* Rectangulos de los tres botones del titulo, relativos al lienzo (como Windows 7: juntos arriba a la derecha) */
static void botones_titulo(const struct ventana *v, int *xmin, int *xmax, int *xcer, int *ancho_cer, int *ancho) {
    int m = vmarg(), an = v->l.an - 2 * m;
    *ancho = S(27); *ancho_cer = S(46);
    *xcer = m + an - *ancho_cer - S(8);
    *xmax = *xcer - *ancho;
    *xmin = *xmax - *ancho;
}

static void contenido(int i, struct lienzo *l, int x, int y, int an, int al) {
    switch (vent[i].tipo) {
    case VT_IMAGENES: vi_dibujar(l, x, y, an, al); break;
    case VT_MUSICA: vm_dibujar(l, x, y, an, al); break;
    case VT_VIDEOS: vv_dibujar(l, x, y, an, al); break;
    case VT_TERMINAL: case VT_HUEVO: case VT_STREAM: case VT_RULETA: contenido_oscuro(vent[i].tipo, l, x, y, an, al, foco() == i); break;   /* H151 */
    default: contenido_ventana(vent[i].tipo, l, x, y, an); break;
    }
}

static void pintar_marco(int i) {
    struct ventana *v = &vent[i];
    struct lienzo *l = &v->l;
    limpiar(l, 0);
    int m = vmarg(), an = l->an - 2 * m, al = l->al - 2 * m;
    int activa = foco() == i;
    int r = v->maximizada ? 0 : S(8);
    if (!v->maximizada)
        for (int k = m; k > 0; k -= S(3) > 0 ? S(3) : 1) redondo(l, m - k, m - k + S(2), an + 2 * k, al + 2 * k, S(8) + k, premul(activa ? 20 : 11, 0));
    vidrio(l, m, m, an, al, r, v->x + m, v->y + m, premul(activa ? 100 : 130, activa ? 0xD4F0FFu : 0xE8EEF2u));
    redondo_deg(l, m + 1, m + 1, an - 2, VENT_TITULO - 2, r ? S(7) : 0, premul(120, BLANCO), premul(18, BLANCO));
    if (r) {
        redondo_borde(l, m, m, an, al, r, premul(200, 0x18345Au));
        redondo_borde(l, m + 1, m + 1, an - 2, al - 2, r - 1, premul(120, BLANCO));
    }
    /* Icono pequeno y titulo con halo, como en Windows 7 */
    icono(l, m + S(22), m + S(19), S(22), vt_icono[v->tipo]);
    const char *t = vt_titulo[v->tipo];
    texto(l, m + S(40) - 1, m + S(26), S(17), BLANCO, 150, 0, t);
    texto(l, m + S(40) + 1, m + S(26) + 1, S(17), BLANCO, 150, 0, t);
    texto(l, m + S(40), m + S(26), S(17), 0x0A1A30u, 255, 0, t);
    /* Minimizar, maximizar y cerrar */
    int xmin, xmax, xcer, acer, a;
    botones_titulo(v, &xmin, &xmax, &xcer, &acer, &a);
    int by = m + (v->maximizada ? 0 : 1);
    boton_brillante(l, xmin, by, a, BOTON_AL, activa ? 0xE4F0FCu : 0xDCE4EC, activa ? 0x8EB0D6u : 0xA8B4C2u);
    boton_brillante(l, xmax, by, a, BOTON_AL, activa ? 0xE4F0FCu : 0xDCE4EC, activa ? 0x8EB0D6u : 0xA8B4C2u);
    boton_brillante(l, xcer, by, acer, BOTON_AL, activa ? 0xF08070u : 0xD0D8E0u, activa ? 0xC01818u : 0x98A4B0u);
    rect(l, xmin + a / 2 - S(5), by + BOTON_AL - S(7), S(10), S(3), OPACO(BLANCO));
    if (v->maximizada) {
        redondo_borde(l, xmax + a / 2 - S(3), by + S(5), S(9), S(7), 0, OPACO(BLANCO));
        redondo_borde(l, xmax + a / 2 - S(6), by + S(8), S(9), S(7), 0, OPACO(BLANCO));
    } else {
        redondo_borde(l, xmax + a / 2 - S(5), by + S(6), S(11), S(9), 0, OPACO(BLANCO));
        rect(l, xmax + a / 2 - S(5), by + S(6), S(11), S(2), OPACO(BLANCO));
    }
    texto_halo(l, xcer + acer / 2 - S(5), by + S(16), S(17), "x");
    /* Area de contenido */
    int cx = m + S(8), cy = m + VENT_TITULO, can = an - S(16), cal = al - VENT_TITULO - S(8);
    rect(l, cx, cy, can, cal, OPACO(0xFBFDFFu));
    rect(l, cx - 1, cy - 1, can + 2, 1, premul(150, 0x305878u));
    if (v->tipo < VT_IMAGENES) degradado(l, cx, cy + 1, can, S(26), premul(90, 0xDCEEFFu), premul(0, 0));
    contenido(i, l, cx, cy, can, cal);
}

static void pintar_ventana(int i) {
    struct ventana *v = &vent[i];
    if (!v->abierta || v->minimizada) return;
    pintar_marco(i);
    taek_presentar(v->s.id, 0, 0, v->l.an, v->l.al);
}
/* Nueva geometria (de la superficie, sombra incluida): se dibuja con la forma nueva y despues se cambia de una vez */
static void geometria(int i, int x, int y, int an, int al) {
    struct ventana *v = &vent[i];
    if ((uint64_t)an * (uint64_t)al > (uint64_t)W * (uint64_t)H) al = (int)((uint64_t)W * (uint64_t)H / (uint64_t)an);
    v->x = x; v->y = y; v->l.an = an; v->l.al = al;
    pintar_marco(i);
    taek_tamano(v->s.id, x, y, an, al);
}

static void traer_al_frente(int i) {
    int j = 0;
    for (int k = 0; k < nz; k++) if (z[k] != i) z[j++] = z[k];
    z[j] = i; nz = j + 1;
    taek_elevar(vent[i].s.id);
    taek_elevar(s_barra.id);
    if (inicio_abierto) taek_elevar(s_ini.id);
}

static void enfocar(int i) {
    int antes = foco();
    struct ventana *v = &vent[i];
    if (v->minimizada) {
        v->minimizada = 0;
        taek_mover(v->s.id, v->x, v->y);
    }
    traer_al_frente(i);
    if (antes >= 0 && antes != i) pintar_ventana(antes);
    pintar_ventana(i);
    pintar_barra();
}

static void ven_abrir(int tipo) {
    int i = ventana_de(tipo);
    if (i >= 0) { enfocar(i); return; }
    for (i = 0; i < NVENT && vent[i].abierta; i++) {}
    if (i >= NVENT) return;   /* Ocho a la vez: suficiente */
    struct ventana *v = &vent[i];
    if (!v->creada) {
        if (crear(&v->s, &v->l, W, H, OCULTO, 0, TAEK_SUP_ALFA)) return;
        v->creada = 1;
    }
    if (tipo < VT_IMAGENES) leer_equipo();
    int antes = foco();
    v->abierta = 1; v->minimizada = 0; v->maximizada = 0; v->tipo = tipo;
    int m = vmarg();
    int an = tipo < VT_IMAGENES ? S(780) : tipo == VT_MUSICA ? S(900) : tipo > VT_VIDEOS ? S(760) : S(1040);
    int al = tipo < VT_IMAGENES ? S(600) : tipo > VT_VIDEOS ? S(540) : S(680);
    if (an + 2 * m > W) an = W - 2 * m;
    if (al + 2 * m > H - BARRA_AL) al = H - BARRA_AL - 2 * m;
    int x = S(200) + nz * S(36) + tipo * S(18), y = S(50) + nz * S(30);
    if (x + an > W) x = W - an - 2 * m;
    if (y + al > H - BARRA_AL) y = 0;
    traer_al_frente(i);
    if (antes >= 0) pintar_ventana(antes);
    /* Lo que tarda (recorrer los discos) se hace despues de mostrar la ventana con "Buscando..." */
    geometria(i, x, y + S(46), an + 2 * m, al + 2 * m);
    v->anim = (float)S(46);
    v->y = y;
    pintar_barra();
    struct mlista *ls = tipo == VT_IMAGENES ? &vimg : tipo == VT_MUSICA ? &vmus : tipo == VT_VIDEOS ? &vvid : NULL;
    if (ls && !ls->cargada) {
        ml_cargar(ls, 0);
        if (tipo == VT_IMAGENES && vimg.nvis) vi_cargar();
        pintar_ventana(i);
    }
    if (tipo == VT_MUSICA) vm_ventana = i;
}

static void esc_cerrar(int i) {
    struct ventana *v = &vent[i];
    if (!v->abierta) return;
    v->abierta = 0; v->anim = 0; v->minimizada = 0;
    int j = 0;
    for (int k = 0; k < nz; k++) if (z[k] != i) z[j++] = z[k];
    nz = j;
    ocultar(&v->s);
    if (v->tipo == VT_MUSICA) { vm_parar_todo(); vm_ventana = -1; }
    if (v->tipo == VT_IMAGENES) { vi_soltar(); vi_actual = -1; }
    int f = foco();
    if (f >= 0) pintar_ventana(f);
    pintar_barra();
}

static void minimizar(int i) {
    struct ventana *v = &vent[i];
    v->minimizada = 1;
    ocultar(&v->s);
    int f = foco();
    if (f >= 0) pintar_ventana(f);
    pintar_barra();
}

static void maximizar(int i) {
    struct ventana *v = &vent[i];
    int m = vmarg();
    if (v->maximizada) {
        v->maximizada = 0;
        geometria(i, v->rx, v->ry, v->ran, v->ral);
    } else {
        v->rx = v->x; v->ry = v->y; v->ran = v->l.an; v->ral = v->l.al;
        v->maximizada = 1;
        geometria(i, -m, -m, W + 2 * m, H - BARRA_AL + 2 * m);
    }
}

static void esc_mover_ventana(int i, int x, int y, int repintar) {
    struct ventana *v = &vent[i];
    if (y < -vmarg()) y = -vmarg();
    if (y > H - BARRA_AL - S(40)) y = H - BARRA_AL - S(40);
    if (x < -v->l.an + S(120)) x = -v->l.an + S(120);
    if (x > W - S(120)) x = W - S(120);
    v->x = x; v->y = y;
    /* Mover es barato y repintar caro: el vidrio se actualiza de vez en cuando y al soltar */
    if (repintar) pintar_ventana(i);
    taek_mover(v->s.id, x, y);
}

/* Donde cae (x, y) de pantalla en la ventana i */
enum { Z_FUERA, Z_CONTENIDO, Z_TITULO, Z_MIN, Z_MAX, Z_CERRAR, Z_BORDE };
static int zona(int i, int x, int y, int *bordes) {
    struct ventana *v = &vent[i];
    int m = vmarg(), x0 = v->x + m, y0 = v->y + m, an = v->l.an - 2 * m, al = v->l.al - 2 * m;
    *bordes = 0;
    if (!v->maximizada) {
        if (x >= x0 - ASIR && x < x0 + an + ASIR && y >= y0 - ASIR && y < y0 + al + ASIR) {
            if (x < x0 + S(3)) *bordes |= 1;
            if (x >= x0 + an - S(3)) *bordes |= 2;
            if (y < y0 + S(3)) *bordes |= 4;
            if (y >= y0 + al - S(3)) *bordes |= 8;
            if (*bordes) return Z_BORDE;
        }
    }
    if (x < x0 || x >= x0 + an || y < y0 || y >= y0 + al) return Z_FUERA;
    if (y < y0 + VENT_TITULO) {
        int xmin, xmax, xcer, acer, a;
        botones_titulo(v, &xmin, &xmax, &xcer, &acer, &a);
        int lx = x - v->x, ly = y - v->y;
        if (ly < m + BOTON_AL + S(2)) {
            if (lx >= xcer && lx < xcer + acer) return Z_CERRAR;
            if (lx >= xmax && lx < xmax + a) return Z_MAX;
            if (lx >= xmin && lx < xmin + a) return Z_MIN;
        }
        return Z_TITULO;
    }
    return Z_CONTENIDO;
}

/* ------------------------------------------------------------------ entrada de las aplicaciones */

static struct mlista *lista_de(int tipo) {
    return tipo == VT_IMAGENES ? &vimg : tipo == VT_MUSICA ? &vmus : tipo == VT_VIDEOS ? &vvid : NULL;
}

/* Tecla para la ventana con foco. Devuelve accion del nucleo, 0, o -1 si no la quiso. */
static int app_tecla(int i, uint32_t k) {
    int tipo = vent[i].tipo;
    /* H151: Terminal, Stream, Huevo y Ruleta. Esc no lo quieren: la ventana se cierra. */
    if (tipo == VT_TERMINAL) {
        int soltar = 0, a = term_tecla(k, &soltar);
        if (soltar) return -1;
        if (k == '\n' || k == '\r') sonar(SND_TICK);
        pintar_ventana(i);
        return a;
    }
    if (tipo == VT_STREAM) { if (stm_tecla(k)) return -1; if (k == '\n') sonar(SND_OLEADA); pintar_ventana(i); return 0; }
    if (tipo == VT_HUEVO) {
        if (k == '\n' || k == '\r') { sonar(SND_BARRIDO); pan_huevo_correr(); sen_leer(0); pintar_ventana(i); return 0; }
        return -1;
    }
    if (tipo == VT_RULETA) {
        if (k == 27 && ruleta_confirmar) { ruleta_confirmar = 0; pintar_ventana(i); return 0; }
        if (k == '\n' || k == '\r') {
            if (!ruleta_confirmar) { ruleta_confirmar = 1; sonar(SND_TAMBOR); pintar_ventana(i); return 0; }
            ruleta_confirmar = 0;
            int a = orden_pantalla("ruleta");
            if (a) sonar(SND_DISPARO);
            return a;
        }
        return -1;
    }
    struct mlista *ls = lista_de(tipo);
    if (!ls) return -1;
    if (k == 27) {
        if (ls->b.n) { ls->b.n = 0; ls->b.t[0] = 0; ml_filtrar(ls); pintar_ventana(i); return 0; }
        return -1;
    }
    /* Musica: con la busqueda vacia, espacio y + / - son pausa y volumen */
    if (tipo == VT_MUSICA && !ls->b.n && (k == ' ' || k == '+' || k == '-')) {
        if (k == ' ') vm_pausa(); else vm_volumen_cambiar(k == '+' ? 3 : -3);
        pintar_ventana(i);
        return 0;
    }
    if (tipo == VT_MUSICA && (k == 28 || k == 29) && vm_sonando >= 0 && !VM.pausa) {
        uint64_t p = vm_pos();
        vm_ir_a(k == 29 ? p + 10000 : p > 10000 ? p - 10000 : 0);
        pintar_ventana(i);
        return 0;
    }
    if (tipo == VT_VIDEOS && (k == ' ' || k == 'k' || k == 'K')) {
        VV.reproduciendo = !VV.reproduciendo;
        VV.ultimo_t = taek_ms();
        pintar_ventana(i);
        return 0;
    }
    if (tipo == VT_VIDEOS && (k == 28 || k == 'j' || k == 'J')) {
        VV.tiempo_ms = VV.tiempo_ms > 10000 ? VV.tiempo_ms - 10000 : 0;
        pintar_ventana(i);
        return 0;
    }
    if (tipo == VT_VIDEOS && (k == 29 || k == 'l' || k == 'L')) {
        VV.tiempo_ms = VV.tiempo_ms + 10000 < VV.duracion_ms ? VV.tiempo_ms + 10000 : VV.duracion_ms;
        pintar_ventana(i);
        return 0;
    }
    if (tipo == VT_VIDEOS && (k == 'b' || k == 'B')) {
        VV.bucle = !VV.bucle;
        pintar_ventana(i);
        return 0;
    }
    if (tipo == VT_VIDEOS && (k == 's' || k == 'S')) {
        VV.reproduciendo = 0; VV.tiempo_ms = 0;
        pintar_ventana(i);
        return 0;
    }
    if (tipo == VT_VIDEOS && (k == 'f' || k == 'F')) return vv_reproducir();
    if (tipo == VT_IMAGENES && (k == 28 || k == 29)) { vi_mover(k == 29 ? 1 : -1); pintar_ventana(i); return 0; }
    if (k == '\n' || k == '\r') {
        if (tipo == VT_IMAGENES) { vi_cargar(); pintar_ventana(i); return 0; }
        if (tipo == VT_MUSICA && ls->nvis) { vm_tocar(ls->vis[ls->sel]); pintar_ventana(i); return 0; }
        if (tipo == VT_VIDEOS) {
            VV.reproduciendo = !VV.reproduciendo;
            VV.ultimo_t = taek_ms();
            pintar_ventana(i);
            return 0;
        }
        return 0;
    }
    if (ml_tecla(ls, k)) {
        if (tipo == VT_IMAGENES && (k == 17 || k == 18)) vi_cargar();
        pintar_ventana(i);
        return 0;
    }
    return -1;
}

static long app_ultimo_clic; static int app_ultima_fila = -1;
/* Clic en el contenido (coordenadas de pantalla). Devuelve accion del nucleo o 0. */
static int app_clic(int i, int x, int y) {
    struct ventana *v = &vent[i];
    int lx = x - v->x, ly = y - v->y, tipo = v->tipo;
    /* H151: en la Terminal, una sugerencia pulsada se ejecuta */
    if (tipo == VT_TERMINAL) { int hecho = 0, a = term_clic(lx, ly, &hecho); if (hecho) { sonar(SND_TICK); pintar_ventana(i); } return a; }
    struct mlista *ls = lista_de(tipo);
    if (!ls) return 0;
    int r = ml_clic(ls, lx, ly);
    if (r == 2) {
        long ahora = taek_ms();
        int doble = ls->sel == app_ultima_fila && ahora - app_ultimo_clic < 450;
        app_ultima_fila = ls->sel; app_ultimo_clic = ahora;
        if (tipo == VT_IMAGENES) vi_cargar();
        if (tipo == VT_MUSICA && doble) vm_tocar(ls->vis[ls->sel]);
        if (tipo == VT_VIDEOS && doble) return vv_reproducir();
        pintar_ventana(i);
        return 0;
    }
    if (r == 1) return 0;
    if (tipo == VT_IMAGENES && ly > vi_bot_y - S(20) && ly < vi_bot_y + S(20)) {
        if (lx > vi_bant_x - S(20) && lx < vi_bant_x + S(20)) { vi_mover(-1); pintar_ventana(i); }
        else if (lx > vi_bsig_x - S(20) && lx < vi_bsig_x + S(20)) { vi_mover(1); pintar_ventana(i); }
    }
    if (tipo == VT_MUSICA && ly >= vm_panel_y) {
        int cx = vm_panel_x + vm_panel_an / 2, cy = vm_panel_y + S(40);
        if (ly > cy - S(28) && ly < cy + S(28)) {
            if (lx > cx - S(28) && lx < cx + S(28)) vm_pausa();
            else if (lx > cx - S(84) && lx < cx - S(44)) vm_saltar(-1);
            else if (lx > cx + S(44) && lx < cx + S(84)) vm_saltar(1);
            int vx = vm_panel_x + vm_panel_an - S(170);
            if (lx >= vx && lx < vx + S(34)) vm_volumen_cambiar(-3);
            else if (lx >= vx + S(118) && lx < vx + S(152)) vm_volumen_cambiar(3);
        }
        int py = vm_panel_y + S(104) - S(22);
        if (ly > py - S(12) && ly < py + S(16) && lx >= vm_prog_x && lx < vm_prog_x + vm_prog_an && vm_sonando >= 0 && VM.duracion_ms && !VM.pausa)
            vm_ir_a(VM.duracion_ms * (uint64_t)(lx - vm_prog_x) / (uint64_t)vm_prog_an);
        pintar_ventana(i);
    }
    if (tipo == VT_VIDEOS) {
        /* Clic en pantalla de video o doble clic: conmutar reproduccion o lanzar pantalla completa */
        if (lx >= VV.pan_x && lx < VV.pan_x + VV.pan_w && ly >= VV.pan_y && ly < VV.pan_y + VV.pan_h) {
            long ahora = taek_ms();
            if (ahora - app_ultimo_clic < 400) return vv_reproducir();
            VV.reproduciendo = !VV.reproduciendo;
            VV.ultimo_t = ahora;
            app_ultimo_clic = ahora;
            pintar_ventana(i);
            return 0;
        }
        /* Barra de progreso */
        if (lx >= VV.prog_x && lx < VV.prog_x + VV.prog_w && ly >= VV.prog_y - S(6) && ly < VV.prog_y + VV.prog_h + S(8)) {
            VV.tiempo_ms = (uint64_t)(lx - VV.prog_x) * VV.duracion_ms / (uint64_t)VV.prog_w;
            pintar_ventana(i);
            return 0;
        }
        /* Orbe Play / Pausa */
        int dx = lx - VV.orbe_x, dy = ly - VV.orbe_y;
        if (dx * dx + dy * dy <= VV.orbe_r * VV.orbe_r) {
            VV.reproduciendo = !VV.reproduciendo;
            VV.ultimo_t = taek_ms();
            pintar_ventana(i);
            return 0;
        }
        /* Salto -10s */
        if (lx >= VV.b_retro_x - S(14) && lx < VV.b_retro_x + S(14) && ly >= VV.orbe_y - S(14) && ly < VV.orbe_y + S(14)) {
            VV.tiempo_ms = VV.tiempo_ms > 10000 ? VV.tiempo_ms - 10000 : 0;
            pintar_ventana(i);
            return 0;
        }
        /* Salto +10s */
        if (lx >= VV.b_avan_x - S(14) && lx < VV.b_avan_x + S(14) && ly >= VV.orbe_y - S(14) && ly < VV.orbe_y + S(14)) {
            VV.tiempo_ms = VV.tiempo_ms + 10000 < VV.duracion_ms ? VV.tiempo_ms + 10000 : VV.duracion_ms;
            pintar_ventana(i);
            return 0;
        }
        /* Anterior */
        if (lx >= VV.b_ant_x - S(16) && lx < VV.b_ant_x + S(16) && ly >= VV.orbe_y - S(16) && ly < VV.orbe_y + S(16)) {
            if (vvid.nvis) { vvid.sel = (vvid.sel + vvid.nvis - 1) % vvid.nvis; VV.tiempo_ms = 0; }
            pintar_ventana(i);
            return 0;
        }
        /* Siguiente */
        if (lx >= VV.b_sig_x - S(16) && lx < VV.b_sig_x + S(16) && ly >= VV.orbe_y - S(16) && ly < VV.orbe_y + S(16)) {
            if (vvid.nvis) { vvid.sel = (vvid.sel + 1) % vvid.nvis; VV.tiempo_ms = 0; }
            pintar_ventana(i);
            return 0;
        }
        /* Stop */
        if (lx >= VV.b_stop_x - S(16) && lx < VV.b_stop_x + S(16) && ly >= VV.orbe_y - S(14) && ly < VV.orbe_y + S(14)) {
            VV.reproduciendo = 0; VV.tiempo_ms = 0;
            pintar_ventana(i);
            return 0;
        }
        /* Bucle */
        if (lx >= VV.b_bucle_x - S(16) && lx < VV.b_bucle_x + S(16) && ly >= VV.orbe_y - S(14) && ly < VV.orbe_y + S(14)) {
            VV.bucle = !VV.bucle;
            pintar_ventana(i);
            return 0;
        }
        /* Boton pantalla completa HW */
        if (lx >= VV.b_full_x && lx < VV.b_full_x + S(116) && ly >= VV.orbe_y - S(16) && ly < VV.orbe_y + S(16)) {
            return vv_reproducir();
        }
        /* Volumen - / + */
        if (lx >= VV.vol_men_x && lx < VV.vol_men_x + S(24) && ly >= VV.orbe_y - S(14) && ly < VV.orbe_y + S(14)) {
            VV.volumen = VV.volumen > 5 ? VV.volumen - 5 : 0;
            pintar_ventana(i);
            return 0;
        }
        if (lx >= VV.vol_mas_x && lx < VV.vol_mas_x + S(24) && ly >= VV.orbe_y - S(14) && ly < VV.orbe_y + S(14)) {
            VV.volumen = VV.volumen + 5 < 100 ? VV.volumen + 5 : 100;
            pintar_ventana(i);
            return 0;
        }
    }
    return 0;
}

/* Cada cuadro: la musica y los videos avanzan */
static void apps_cada_cuadro(void) {
    int fin = vm_bombear();
    if (vm_ventana >= 0 && vent[vm_ventana].abierta && !vent[vm_ventana].minimizada) {
        long ahora = taek_ms();
        if (fin) pintar_ventana(vm_ventana);
        else if (vm_sonando >= 0 && ahora - vm_ultimo_panel > 500) {
            vm_ultimo_panel = ahora;
            struct ventana *v = &vent[vm_ventana];
            vm_panel(&v->l);
            taek_presentar(v->s.id, vm_panel_x, vm_panel_y, vm_panel_an, S(104));
        }
    }
    if (VV.reproduciendo) {
        long ahora = taek_ms();
        long dt = ahora - VV.ultimo_t;
        if (dt > 0) {
            VV.tiempo_ms += (uint64_t)dt;
            if (VV.tiempo_ms >= VV.duracion_ms) {
                if (VV.bucle) VV.tiempo_ms = 0;
                else { VV.tiempo_ms = VV.duracion_ms; VV.reproduciendo = 0; }
            }
            VV.ultimo_t = ahora;
        }
        int v_vid = ventana_de(VT_VIDEOS);
        if (v_vid >= 0 && vent[v_vid].abierta && !vent[v_vid].minimizada) {
            pintar_ventana(v_vid);
        }
    }
}
