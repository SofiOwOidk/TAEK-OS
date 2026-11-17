#ifndef TAEK_APPS_H
#define TAEK_APPS_H
/* H146: lo que comparten las aplicaciones de NekoSaka (imagenes, musica, videos...): teclas, cadenas, rutas y archivos.
 * Se incluye despues de ui.h y memoria.h. Todo static: cada aplicacion es un solo archivo .c. */
#pragma clang diagnostic ignored "-Wunused-function"

#ifndef OPACO
#define OPACO(c) (0xFF000000u | (c))
#endif

/* Lo que las bibliotecas de un solo archivo (stb_image, minimp3) esperan de una biblioteca C.
 * En el banco del PC (tests/menu_anfitrion.h) estan las de la libc. */
#ifndef TAEK_ANFITRION
static void *memmove(void *d, const void *o, size_t n) {
    unsigned char *a = d; const unsigned char *b = o;
    if (a < b) for (size_t i = 0; i < n; i++) a[i] = b[i];
    else for (size_t i = n; i > 0; i--) a[i - 1] = b[i - 1];
    return d;
}
static int abs(int v) { return v < 0 ? -v : v; }
static int memcmp(const void *x, const void *y, size_t n) {
    const unsigned char *a = x, *b = y;
    for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}
#endif

/* ------------------------------------------------------------------ cadenas */

static int ap_largo(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void ap_copiar(char *d, const char *o, int cap) { int i = 0; while (o[i] && i + 1 < cap) { d[i] = o[i]; i++; } d[i] = 0; }
static void ap_agregar(char *d, const char *o, int cap) { int n = ap_largo(d); ap_copiar(d + n, o, cap - n); }
static char ap_minus(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int ap_comparar(const char *a, const char *b) {
    while (*a && ap_minus(*a) == ap_minus(*b)) { a++; b++; }
    return (unsigned char)ap_minus(*a) - (unsigned char)ap_minus(*b);
}
static void ap_num(char *d, int cap, uint64_t v) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    int n = ap_largo(d);
    while (i && n + 1 < cap) d[n++] = b[--i];
    d[n] = 0;
}
static void ap_dos(char *d, int cap, uint64_t v) { if (v < 10) ap_agregar(d, "0", cap); ap_num(d, cap, v); }
static void ap_tamano(char *d, int cap, uint64_t b) {
    static const char *u[] = { " B", " KB", " MB", " GB" };
    int k = 0; uint64_t ent = b, dec = 0;
    while (ent >= 1024 && k < 3) { dec = (ent % 1024) * 10 / 1024; ent /= 1024; k++; }
    d[0] = 0; ap_num(d, cap, ent);
    if (k && ent < 100) { ap_agregar(d, ",", cap); ap_num(d, cap, dec); }
    ap_agregar(d, u[k], cap);
}
/* Minutos y segundos: 3:07 */
static void ap_tiempo(char *d, int cap, uint64_t ms) {
    uint64_t s = ms / 1000;
    ap_num(d, cap, s / 60); ap_agregar(d, ":", cap); ap_dos(d, cap, s % 60);
}

/* ------------------------------------------------------------------ rutas */

static void ap_unir(char *out, int cap, const char *carpeta, const char *nombre) {
    ap_copiar(out, carpeta, cap);
    if (ap_largo(out) > 1 || (out[0] && out[0] != '/')) ap_agregar(out, "/", cap);
    else if (!out[0]) ap_copiar(out, "/", cap);
    ap_agregar(out, nombre, cap);
}
static void ap_carpeta_de(const char *ruta, char *out, int cap) {
    ap_copiar(out, ruta, cap);
    int n = ap_largo(out);
    while (n > 0 && out[n - 1] != '/') n--;
    if (n > 1) n--;
    if (n < 1) n = 1;
    out[n] = 0;
    if (!out[0]) ap_copiar(out, "/", cap);
}
static const char *ap_nombre_de(const char *ruta) {
    const char *n = ruta;
    for (const char *p = ruta; *p; p++) if (*p == '/') n = p + 1;
    return n;
}
/* 1 si la extension (sin distinguir mayusculas) esta en la lista terminada en 0 */
static int ap_extension(const char *nombre, const char *const *lista) {
    const char *punto = 0;
    for (const char *p = nombre; *p; p++) if (*p == '.') punto = p;
    if (!punto) return 0;
    for (int i = 0; lista[i]; i++) if (!ap_comparar(punto + 1, lista[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------------ archivos */

static struct taek_archivos ap_ar;
/* H148: disco de las lecturas que siguen: 0 el abierto, N + 1 el disco N (el nucleo lo monta si hace falta) */
static uint32_t ap_disco;

/* Entradas de una carpeta en un arreglo nuevo (malloc). Devuelve cuantas, o negativo. */
static int ap_listar(const char *ruta, struct taek_entrada **out, int max) {
    struct taek_entrada *e = malloc(sizeof(*e) * (size_t)max);
    if (!e) return -TAEK_ENOMEM;
    int n = 0;
    for (;;) {
        memset(&ap_ar, 0, sizeof(ap_ar));
        ap_ar.op = TAEK_AR_LISTAR; ap_ar.disco = ap_disco; ap_copiar(ap_ar.ruta, ruta, sizeof(ap_ar.ruta));
        ap_ar.destino = (uint64_t)(uintptr_t)(e + n); ap_ar.desde = (uint64_t)n;
        ap_ar.capacidad = max - n > 512 ? 512 : (uint64_t)(max - n);
        long r = taek_archivos(&ap_ar);
        if (r) { free(e); return (int)r; }
        n += (int)ap_ar.n;
        if (!ap_ar.n || (uint64_t)n >= ap_ar.total || n >= max) break;
    }
    *out = e;
    return n;
}

/* Un trozo de archivo. Devuelve los bytes leidos y deja el tamano total en *total. */
static long ap_leer(const char *ruta, uint64_t desde, void *destino, uint64_t cap, uint64_t *total) {
    memset(&ap_ar, 0, sizeof(ap_ar));
    ap_ar.op = TAEK_AR_LEER; ap_ar.disco = ap_disco; ap_copiar(ap_ar.ruta, ruta, sizeof(ap_ar.ruta));
    ap_ar.destino = (uint64_t)(uintptr_t)destino; ap_ar.capacidad = cap; ap_ar.desde = desde;
    long r = taek_archivos(&ap_ar);
    if (total) *total = ap_ar.total;
    return r ? r : (long)ap_ar.n;
}

/* El archivo entero en un bufer nuevo (malloc), si no pasa de max bytes */
static uint8_t *ap_leer_todo(const char *ruta, uint64_t *n, uint64_t max) {
    uint64_t total = 0;
    uint8_t prueba;
    if (ap_leer(ruta, 0, &prueba, 1, &total) < 0 || !total || total > max) return NULL;
    uint8_t *b = malloc((size_t)total + 1);
    if (!b) return NULL;
    uint64_t hecho = 0;
    while (hecho < total) {
        uint64_t k = total - hecho > (8u << 20) ? (8u << 20) : total - hecho;
        long r = ap_leer(ruta, hecho, b + hecho, k, NULL);
        if (r <= 0) { free(b); return NULL; }
        hecho += (uint64_t)r;
    }
    b[total] = 0;
    *n = total;
    return b;
}

/* Abre otra aplicacion al salir; al cerrarla, el nucleo vuelve a abrir esta con 'al_volver' */
static int ap_lanzar(const char *programa, const char *argumento, const char *al_volver) {
    static struct taek_lanzar l;
    memset(&l, 0, sizeof(l));
    ap_copiar(l.programa, programa, sizeof(l.programa));
    if (argumento) ap_copiar(l.argumento, argumento, sizeof(l.argumento));
    if (al_volver) ap_copiar(l.argumento_al_volver, al_volver, sizeof(l.argumento_al_volver));
    return (int)taek_lanzar(&l);
}

/* ------------------------------------------------------------------ medios en todos los discos (H148) */

/* Un archivo encontrado al recorrer los discos. disco va como en ap_disco: N + 1. */
struct ap_medio { char ruta[TAEK_RUTA]; char nombre[160]; uint64_t tam; uint32_t disco; };
#define AP_MAX_DISCOS 8
static struct taek_disco_usb ap_discos[AP_MAX_DISCOS];
static int ap_ndiscos;
/* Los que de verdad se pudieron leer: un lector de tarjetas vacio aparece como disco listo y no aporta nada */
static int ap_leidos = 1;

/* Los discos USB que el nucleo tiene listos */
static int ap_leer_discos(void) {
    memset(&ap_ar, 0, sizeof(ap_ar));
    ap_ar.op = TAEK_AR_DISCOS; ap_ar.destino = (uint64_t)(uintptr_t)ap_discos; ap_ar.capacidad = AP_MAX_DISCOS;
    ap_ndiscos = taek_archivos(&ap_ar) ? 0 : (int)ap_ar.n;
    return ap_ndiscos;
}
/* Nombre corto de un disco (N + 1) para mostrar junto al archivo */
static const char *ap_disco_nombre(uint32_t disco) {
    static char con_numero[AP_MAX_DISCOS][56];
    for (int i = 0; i < ap_ndiscos; i++) {
        if (ap_discos[i].numero + 1 != disco) continue;
        /* Dos pendrives del mismo modelo se llaman igual: entonces va tambien el numero de disco */
        int repetido = 0;
        for (int j = 0; j < ap_ndiscos; j++) if (j != i && !ap_comparar(ap_discos[j].nombre, ap_discos[i].nombre)) repetido = 1;
        if (!repetido) return ap_discos[i].nombre;
        ap_copiar(con_numero[i], ap_discos[i].nombre, sizeof(con_numero[i]));
        ap_agregar(con_numero[i], " #", sizeof(con_numero[i]));
        ap_num(con_numero[i], sizeof(con_numero[i]), (uint64_t)ap_discos[i].numero);
        return con_numero[i];
    }
    return disco ? "Disco" : "Disco abierto";
}
/* Carpetas que no se recorren: el arranque, la papelera y lo que deja Windows */
static int ap_carpeta_ignorada(const char *n) {
    return n[0] == '.' || !ap_comparar(n, "ventoy") || !ap_comparar(n, "papelera") || !ap_comparar(n, "System Volume Information") ||
           !ap_comparar(n, "$RECYCLE.BIN");
}
static void ap_recorrer(const char *const *ext, struct ap_medio *m, int *n, int max, const char *carpeta, int nivel, int *carpetas) {
    struct taek_entrada *e;
    int k = ap_listar(carpeta, &e, 4096);
    if (k <= 0) return;
    (*carpetas)++;
    for (int i = 0; i < k && *n < max; i++) {
        if (e[i].carpeta || !ap_extension(e[i].nombre, ext)) continue;
        struct ap_medio *x = &m[(*n)++];
        ap_unir(x->ruta, sizeof(x->ruta), carpeta, e[i].nombre);
        ap_copiar(x->nombre, e[i].nombre, sizeof(x->nombre));
        int c = ap_largo(x->nombre);
        while (c > 0 && x->nombre[c - 1] != '.') c--;
        if (c > 1) x->nombre[c - 1] = 0;   /* Sin la extension */
        x->tam = e[i].tam;
        x->disco = ap_disco;
    }
    if (nivel < 2)
        for (int i = 0; i < k && *n < max && *carpetas < 96; i++) {
            if (!e[i].carpeta || ap_carpeta_ignorada(e[i].nombre)) continue;
            char sub[TAEK_RUTA]; ap_unir(sub, sizeof(sub), carpeta, e[i].nombre);
            ap_recorrer(ext, m, n, max, sub, nivel + 1, carpetas);
        }
    free(e);
}
static void ap_marca(const char *app, const char *que, const char *detalle);
/* Todos los archivos con esas extensiones en la raiz y dos niveles de carpetas de cada disco listo, por nombre.
 * Sin discos que listar (por ejemplo, en el banco del PC), recorre el disco abierto. */
static int ap_escanear(const char *const *ext, struct ap_medio *m, int max) {
    int n = 0;
    ap_leer_discos();
    uint32_t antes = ap_disco;
    int recorridos = 0;
    for (int d = 0; d < ap_ndiscos && n < max; d++) {
        if (!ap_discos[d].listo) continue;
        ap_disco = ap_discos[d].numero + 1;
        /* La primera lectura de un USB que nadie toco desde el arranque puede salir rechazada (en la laptop, el de
         * 128 GB: "Comando SCSI rechazado"). Se prueba la raiz hasta tres veces antes de darlo por perdido. */
        int legible = 0;
        for (int intento = 0; intento < 3 && !legible; intento++) {
            struct taek_entrada *e;
            int r = ap_listar("/", &e, 1);
            if (r >= 0) { free(e); legible = 1; }
            else taek_esperar(80);
        }
        if (!legible) { ap_marca("MEDIOS", "no se pudo leer el disco", ap_discos[d].nombre); continue; }
        int carpetas = 0;
        ap_recorrer(ext, m, &n, max, "/", 0, &carpetas);
        if (carpetas) recorridos++;   /* Se pudo listar su raiz */
    }
    if (!recorridos) { ap_disco = 0; int carpetas = 0; ap_recorrer(ext, m, &n, max, "/", 0, &carpetas); }
    ap_leidos = recorridos ? recorridos : 1;
    ap_disco = antes;
    for (int i = 1; i < n; i++) {   /* Por nombre; con el mismo nombre, por disco */
        struct ap_medio t = m[i];
        int j = i;
        while (j > 0 && (ap_comparar(m[j - 1].nombre, t.nombre) > 0 || (!ap_comparar(m[j - 1].nombre, t.nombre) && m[j - 1].disco > t.disco))) { m[j] = m[j - 1]; j--; }
        m[j] = t;
    }
    return n;
}

/* ------------------------------------------------------------------ busqueda (H148) */

/* 1 si 'filtro' aparece dentro de 's' sin distinguir mayusculas (las tildes, tal cual) */
static int ap_contiene(const char *s, const char *filtro) {
    if (!filtro[0]) return 1;
    for (; *s; s++) {
        int i = 0;
        while (filtro[i] && s[i] && ap_minus(s[i]) == ap_minus(filtro[i])) i++;
        if (!filtro[i]) return 1;
    }
    return 0;
}
/* Lo que se escribe para buscar. Devuelve 1 si la tecla cambio el filtro. */
struct ap_busqueda { char t[64]; int n; };
static int ap_buscar_tecla(struct ap_busqueda *b, uint32_t k) {
    if ((k == 8 || k == 127) && b->n) { b->n--; while (b->n && ((unsigned char)b->t[b->n] & 0xC0) == 0x80) b->n--; b->t[b->n] = 0; return 1; }
    if (k >= 32 && k < 127 && b->n + 1 < (int)sizeof(b->t)) { b->t[b->n++] = (char)k; b->t[b->n] = 0; return 1; }
    return 0;
}

/* Una linea en la consola del nucleo, para quien prueba por LAN */
static void ap_marca(const char *app, const char *que, const char *detalle) {
    char m[TAEK_RUTA + 80] = "[";
    ap_agregar(m, app, sizeof(m)); ap_agregar(m, "] ", sizeof(m)); ap_agregar(m, que, sizeof(m));
    if (detalle && detalle[0]) { ap_agregar(m, " ", sizeof(m)); ap_agregar(m, detalle, sizeof(m)); }
    ap_agregar(m, "\n", sizeof(m));
    taek_escribir(m, ap_largo(m));
}

/* ------------------------------------------------------------------ teclas */

/* Esc suelto o secuencias ANSI de la terminal por LAN (ESC [ A/B/C/D) a los codigos de TAEK */
static int ap_esc_paso; static long ap_esc_t;
static uint32_t ap_filtrar(uint32_t k) {
    if (ap_esc_paso == 1) { if (k == '[') { ap_esc_paso = 2; return 0; } if (k == 'O') { ap_esc_paso = 4; return 0; } ap_esc_paso = 0; }
    else if (ap_esc_paso == 4) { ap_esc_paso = 0; return k == 'Q' ? TAEK_TECLA_F2 : 0; }   /* H188: ESC O Q = F2 */
    else if (ap_esc_paso == 2) {
        ap_esc_paso = 0;
        if (k == '3') { ap_esc_paso = 3; return 0; }   /* ESC [ 3 ~ : Supr */
        return k == 'A' ? TAEK_TECLA_ARRIBA : k == 'B' ? TAEK_TECLA_ABAJO : k == 'C' ? TAEK_TECLA_DERECHA : k == 'D' ? TAEK_TECLA_IZQUIERDA : 0;
    }
    else if (ap_esc_paso == 3) { ap_esc_paso = 0; return k == '~' ? TAEK_TECLA_SUPRIMIR : 0; }
    else if (k == 27) { ap_esc_paso = 1; ap_esc_t = taek_ms(); return 0; }
    return k;
}
/* Un Esc que no fue el principio de una secuencia: llamarla una vez por cuadro */
static int ap_esc_suelto(void) {
    if (ap_esc_paso == 1 && taek_ms() - ap_esc_t > 40) { ap_esc_paso = 0; return 1; }
    return 0;
}

#endif
