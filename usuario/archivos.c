/* Explorador de archivos de NekoSaka para TAEK OS (H145). Aplicacion de Anillo 3 aparte: 'ejecutar /archivos.elf'.
 *
 * Tres columnas: los discos USB a la izquierda, la carpeta en el centro y una vista previa a la derecha.
 * Todo por SYS_ARCHIVOS, de solo lectura: el explorador no puede borrar ni escribir nada.
 *   - Carpeta: Intro (o doble clic) entra; Retroceso o clic derecho sube.
 *   - .elf: Intro la abre con SYS_LANZAR; al cerrarse, el nucleo vuelve a abrir el explorador en esta misma carpeta.
 *   - Texto: la vista previa muestra el principio; Intro lo abre a pantalla completa para leerlo entero (hasta 1 MB).
 * Teclas: flechas, Ctrl+D / Ctrl+U media pagina, una letra salta al primer nombre que empieza por ella, Esc sale. */
#pragma clang diagnostic ignored "-Wunused-function"
#include "ui.h"
#include "memoria.h"
#include "apps.h"

TAEK_PROGRAMA

#define FONDO   0xFF0B0E14u
#define PANEL   0xFF10141Du
#define BARRA   0xFF141924u
#define CAMPO   0xFF1E2533u
#define FILA_SEL 0xFF232C3Fu
#define FILA_SOBRE 0xFF171D2Au
#define ROJO    0xFF003Cu
#define NEON    0xFCEE0Au
#define CIAN    0x5BE0FFu
#define TEXTO   0xD8DCE6u
#define TENUE   0x8A93A6u
#define BLANCO  0xFFFFFFu
#define VERDE   0x9BE8A0u

static int AN, AL;
static int S(int v) { return v * AL / 1080; }
static struct taek_superficie sup;
static struct lienzo L;
static int T0, T1;   /* Tamanos de letra: normal y chico */

/* ------------------------------------------------------------------ cadenas */

static int largo_c(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void copiar(char *d, const char *o, int cap) { int i = 0; while (o[i] && i + 1 < cap) { d[i] = o[i]; i++; } d[i] = 0; }
static void agregar(char *d, const char *o, int cap) { int n = largo_c(d); copiar(d + n, o, cap - n); }
static char minus(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int comparar(const char *a, const char *b) {
    while (*a && minus(*a) == minus(*b)) { a++; b++; }
    return (unsigned char)minus(*a) - (unsigned char)minus(*b);
}
static void num(char *d, int cap, uint64_t v) {
    char b[24]; int i = 0;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    int n = largo_c(d);
    while (i && n + 1 < cap) d[n++] = b[--i];
    d[n] = 0;
}
static void tamano_txt(char *d, int cap, uint64_t b) {
    static const char *u[] = { " B", " KB", " MB", " GB", " TB" };
    int k = 0; uint64_t ent = b, dec = 0;
    while (ent >= 1024 && k < 4) { dec = (ent % 1024) * 10 / 1024; ent /= 1024; k++; }
    d[0] = 0; num(d, cap, ent);
    if (k && ent < 100) { agregar(d, ",", cap); num(d, cap, dec); }
    agregar(d, u[k], cap);
}

/* ------------------------------------------------------------------ tipos por extension */

enum { T_CARPETA, T_TEXTO, T_ELF, T_VIDEO, T_AUDIO, T_IMAGEN, T_ISO, T_OTRO };
static const char *const ETIQ[] = { "DIR", "TXT", "APP", "MP4", "MP3", "IMG", "ISO", "BIN" };
static const uint32_t COLOR_T[] = { 0xFFFF003Cu, 0xFF2E7DD7u, 0xFFFCEE0Au, 0xFF9B4DFFu, 0xFF1FB57Fu, 0xFF2BB5C9u, 0xFFE8742Cu, 0xFF4A5366u };

static int tipo_de(const char *nombre, int carpeta) {
    if (carpeta) return T_CARPETA;
    const char *p = nombre, *punto = 0;
    for (; *p; p++) if (*p == '.') punto = p;
    if (!punto) return T_OTRO;
    char e[8]; int n = 0;
    for (p = punto + 1; *p && n < 7; p++) e[n++] = minus(*p);
    e[n] = 0;
    static const char *const txt[] = { "txt", "md", "c", "h", "json", "cfg", "ini", "log", "py", "sh", "html", "htm", "css",
                                       "js", "conf", "csv", "xml", "asm", "ld", "s", "toml", "yaml", "yml", 0 };
    for (int i = 0; txt[i]; i++) if (!comparar(e, txt[i])) return T_TEXTO;
    if (!comparar(e, "elf")) return T_ELF;
    if (!comparar(e, "mp4") || !comparar(e, "mkv") || !comparar(e, "webm")) return T_VIDEO;
    if (!comparar(e, "mp3") || !comparar(e, "wav") || !comparar(e, "flac")) return T_AUDIO;
    if (!comparar(e, "png") || !comparar(e, "jpg") || !comparar(e, "jpeg") || !comparar(e, "bmp")) return T_IMAGEN;
    if (!comparar(e, "iso") || !comparar(e, "img")) return T_ISO;
    return T_OTRO;
}

/* ------------------------------------------------------------------ estado */

#define MAX_ENT 4096
static struct taek_entrada *ent;   /* malloc: MAX_ENT */
static int *orden;
static int nent, sel, primera, fila_sobre = -1;
static char carpeta[TAEK_RUTA] = "/";
static struct taek_disco_usb discos[4];
static int ndiscos, disco_abierto = -1;
static char fs[16];
static char aviso[160];
static int papelera_y = -1;   /* Donde quedo dibujada la entrada Papelera de la columna izquierda */

/* Vista previa / lectura de texto */
static char *txt;           /* malloc: TXT_MAX + 1 */
#define TXT_MAX (1024 * 1024)
static uint64_t txt_n, txt_total;
static int *lineas;         /* malloc: MAX_LINEAS, comienzo de cada linea visual */
#define MAX_LINEAS 60000
static int nlineas, txt_scroll, leyendo, txt_ancho_hecho = -1;
static char txt_de[TAEK_RUTA];

static struct taek_archivos ar;
static void marca_hecho(const char *que, const char *ruta);

static void pedir_discos(void) {
    memset(&ar, 0, sizeof(ar));
    ar.op = TAEK_AR_DISCOS; ar.destino = (uint64_t)(uintptr_t)discos; ar.capacidad = 4;
    ndiscos = taek_archivos(&ar) == 0 ? (int)ar.n : 0;
    disco_abierto = ar.disco_abierto;
    copiar(fs, ar.fs, sizeof(fs));
}

static int montar(uint32_t d) {
    memset(&ar, 0, sizeof(ar));
    ar.op = TAEK_AR_MONTAR; ar.disco = d;
    long r = taek_archivos(&ar);
    pedir_discos();
    return (int)r;
}

static void ruta_de(int i, char *out, int cap) {
    copiar(out, carpeta, cap);
    if (largo_c(out) > 1) agregar(out, "/", cap);
    agregar(out, ent[orden[i]].nombre, cap);
}

static void ordenar(void) {
    for (int i = 0; i < nent; i++) orden[i] = i;
    /* Shell sort: carpetas primero, luego por nombre sin mayusculas */
    for (int h = nent / 2; h > 0; h /= 2)
        for (int i = h; i < nent; i++) {
            int t = orden[i], j = i;
            for (; j >= h; j -= h) {
                const struct taek_entrada *a = &ent[orden[j - h]], *b = &ent[t];
                int mayor = a->carpeta != b->carpeta ? b->carpeta > a->carpeta : comparar(a->nombre, b->nombre) > 0;
                if (!mayor) break;
                orden[j] = orden[j - h];
            }
            orden[j] = t;
        }
}

/* ------------------------------------------------------------------ escritura (H147) */

#define PAPELERA "/papelera"
#define INDICE "/papelera/.indice"   /* Una linea por archivo: nombre en la papelera, tabulador, ruta original */
static int en_papelera(void) { return !comparar(carpeta, PAPELERA); }

/* Lo que devuelve el nucleo, dicho para una persona */
static const char *error_de(long r) {
    switch (r) {
    case -TAEK_EPROTEGIDO: return "Esa zona es de Ventoy (lo que hace arrancar el equipo): TAEK no la toca.";
    case -31: return "Ya existe algo con ese nombre.";
    case -26: return "No se encontr\xC3\xB3.";
    case -33: return "No queda espacio en el disco.";
    case -34: return "Ese nombre no vale: sin / \\ : * ? \" < > | y sin terminar en punto o espacio.";
    case -21: return "Eso todav\xC3\xAD" "a no se puede: por ahora solo archivos, no carpetas.";
    case -29: return "Este disco es de solo lectura para TAEK (NTFS o ext4).";
    case -32: return "El disco est\xC3\xA1 ocupado con otra operaci\xC3\xB3n.";
    case -22: return "El disco no pas\xC3\xB3 la revisi\xC3\xB3n previa: no se escribi\xC3\xB3 nada.";
    default: return "No se pudo.";
    }
}

static void trabajando(const char *que);
static long escribir_op(uint32_t op, const char *ruta, const char *ruta2, const void *datos, uint64_t n) {
    trabajando(op == TAEK_AR_BORRAR ? "Borrando..." : op == TAEK_AR_MOVER ? "Moviendo..." : "Escribiendo...");
    memset(&ar, 0, sizeof(ar));
    ar.op = op; copiar(ar.ruta, ruta, sizeof(ar.ruta));
    if (ruta2) copiar(ar.ruta2, ruta2, sizeof(ar.ruta2));
    ar.destino = (uint64_t)(uintptr_t)datos; ar.capacidad = n;
    return taek_archivos(&ar);
}

/* El indice de la papelera, entero en memoria */
static char *indice; static uint64_t indice_n;
static void indice_leer(void) {
    if (indice) { free(indice); indice = NULL; }
    indice_n = 0;
    uint64_t total = 0;
    long r = ap_leer(INDICE, 0, txt, 1, &total);   /* Solo para saber el tamano */
    if (r < 0 || !total) return;
    indice = malloc(total + 1);
    if (!indice) return;
    if (ap_leer(INDICE, 0, indice, total, NULL) != (long)total) { free(indice); indice = NULL; return; }
    indice[total] = 0; indice_n = total;
}
/* Ruta original de un archivo de la papelera, o "" */
static void indice_buscar(const char *nombre, char *out, int cap) {
    out[0] = 0;
    for (uint64_t i = 0; indice && i < indice_n;) {
        uint64_t fin = i; while (fin < indice_n && indice[fin] != '\n') fin++;
        uint64_t tab = i; while (tab < fin && indice[tab] != '\t') tab++;
        int largo = (int)(tab - i);
        if (tab < fin && largo == largo_c(nombre) && !memcmp(indice + i, nombre, (size_t)largo)) {
            int k = 0; for (uint64_t j = tab + 1; j < fin && k + 1 < cap; j++) out[k++] = indice[j];
            out[k] = 0; return;
        }
        i = fin + 1;
    }
}
/* El indice sin la linea de 'nombre' (o con una linea mas, si 'agregar_linea'), escrito de vuelta */
static long indice_guardar(const char *quitar, const char *agregar_linea) {
    uint64_t extra = agregar_linea ? (uint64_t)largo_c(agregar_linea) + 1 : 0;
    char *n = malloc(indice_n + extra + 1);
    if (!n) return -12;
    uint64_t m = 0;
    for (uint64_t i = 0; indice && i < indice_n;) {
        uint64_t fin = i; while (fin < indice_n && indice[fin] != '\n') fin++;
        uint64_t tab = i; while (tab < fin && indice[tab] != '\t') tab++;
        int fuera = quitar && (int)(tab - i) == largo_c(quitar) && !memcmp(indice + i, quitar, tab - i);
        if (!fuera && fin > i) { memcpy(n + m, indice + i, fin - i); m += fin - i; n[m++] = '\n'; }
        i = fin + 1;
    }
    if (agregar_linea) { memcpy(n + m, agregar_linea, (size_t)largo_c(agregar_linea)); m += (uint64_t)largo_c(agregar_linea); n[m++] = '\n'; }
    long r = m ? escribir_op(TAEK_AR_ESCRIBIR, INDICE, 0, n, m) : escribir_op(TAEK_AR_BORRAR, INDICE, 0, 0, 0);
    if (!m && r == -26) r = 0;
    free(n);
    indice_leer();
    return r;
}

static int listar(const char *ruta);

static void a_la_papelera(int i) {
    const struct taek_entrada *e = &ent[orden[i]];
    if (e->carpeta) { copiar(aviso, "Por ahora a la papelera solo van archivos, no carpetas.", sizeof(aviso)); return; }
    char r[TAEK_RUTA]; ruta_de(i, r, sizeof(r));
    long x = escribir_op(TAEK_AR_CREAR_CARPETA, PAPELERA, 0, 0, 0);
    if (x && x != -31) { copiar(aviso, error_de(x), sizeof(aviso)); return; }
    indice_leer();
    /* Nombre unico en la papelera: un numero que nadie use todavia, delante del nombre */
    uint64_t numero = 1;
    for (uint64_t k = 0; indice && k < indice_n; k++)
        if (!k || indice[k - 1] == '\n') { uint64_t v = 0; while (k < indice_n && indice[k] >= '0' && indice[k] <= '9') v = v * 10 + (uint64_t)(indice[k++] - '0'); if (v >= numero) numero = v + 1; }
    char nombre[300] = ""; num(nombre, sizeof(nombre), numero); agregar(nombre, "_", sizeof(nombre)); agregar(nombre, e->nombre, sizeof(nombre));
    char destino[TAEK_RUTA]; copiar(destino, PAPELERA "/", sizeof(destino)); agregar(destino, nombre, sizeof(destino));
    x = escribir_op(TAEK_AR_MOVER, r, destino, 0, 0);
    if (x) { copiar(aviso, error_de(x), sizeof(aviso)); return; }
    char linea[TAEK_RUTA + 300]; copiar(linea, nombre, sizeof(linea)); agregar(linea, "\t", sizeof(linea)); agregar(linea, r, sizeof(linea));
    indice_guardar(NULL, linea);
    copiar(aviso, "Enviado a la papelera.", sizeof(aviso));
    marca_hecho("papelera", r);
}

static void restaurar(int i) {
    const struct taek_entrada *e = &ent[orden[i]];
    if (e->carpeta) return;
    indice_leer();
    char origen[TAEK_RUTA]; copiar(origen, PAPELERA "/", sizeof(origen)); agregar(origen, e->nombre, sizeof(origen));
    char destino[TAEK_RUTA]; indice_buscar(e->nombre, destino, sizeof(destino));
    if (!destino[0]) {   /* Sin linea en el indice: a la raiz, sin el numero delante */
        const char *n = e->nombre; while (*n >= '0' && *n <= '9') n++; if (*n == '_') n++;
        copiar(destino, "/", sizeof(destino)); agregar(destino, *n ? n : e->nombre, sizeof(destino));
    }
    long x = escribir_op(TAEK_AR_MOVER, origen, destino, 0, 0);
    if (x == -31 || x == -26) {
        /* Ya hay otro con ese nombre, o su carpeta ya no existe: a la raiz como "nombre (restaurado)" */
        char alterno[TAEK_RUTA] = "/"; const char *n = destino + largo_c(destino);
        while (n > destino && n[-1] != '/') n--;
        char base[260]; copiar(base, n, sizeof(base));
        char ext[40] = ""; int p = largo_c(base); while (p > 0 && base[p - 1] != '.') p--;
        if (p > 1) { copiar(ext, base + p - 1, sizeof(ext)); base[p - 1] = 0; }
        agregar(alterno, base, sizeof(alterno)); agregar(alterno, " (restaurado)", sizeof(alterno)); agregar(alterno, ext, sizeof(alterno));
        x = escribir_op(TAEK_AR_MOVER, origen, alterno, 0, 0);
        if (!x) copiar(destino, alterno, sizeof(destino));
    }
    if (x) { copiar(aviso, error_de(x), sizeof(aviso)); return; }
    indice_guardar(e->nombre, NULL);
    copiar(aviso, "Restaurado en ", sizeof(aviso)); agregar(aviso, destino, sizeof(aviso));
    marca_hecho("restaurado", destino);
}

static void eliminar_definitivo(int i) {
    const struct taek_entrada *e = &ent[orden[i]];
    if (e->carpeta) return;
    char r[TAEK_RUTA]; ruta_de(i, r, sizeof(r));
    long x = escribir_op(TAEK_AR_BORRAR, r, 0, 0, 0);
    if (x) { copiar(aviso, error_de(x), sizeof(aviso)); return; }
    indice_leer();
    indice_guardar(e->nombre, NULL);
    copiar(aviso, "Eliminado para siempre.", sizeof(aviso));
    marca_hecho("eliminado", r);
}

static void vaciar_papelera(void) {
    int hechos = 0, fallos = 0;
    for (int i = 0; i < nent; i++) {
        if (ent[orden[i]].carpeta) continue;
        char r[TAEK_RUTA]; ruta_de(i, r, sizeof(r));
        if (escribir_op(TAEK_AR_BORRAR, r, 0, 0, 0)) fallos++; else hechos++;
    }
    escribir_op(TAEK_AR_BORRAR, INDICE, 0, 0, 0);
    indice_leer();
    aviso[0] = 0; num(aviso, sizeof(aviso), (uint64_t)hechos); agregar(aviso, hechos == 1 ? " archivo eliminado" : " archivos eliminados", sizeof(aviso));
    if (fallos) { agregar(aviso, "; no se pudieron ", sizeof(aviso)); num(aviso, sizeof(aviso), (uint64_t)fallos); }
    marca_hecho("vaciada", aviso);
}

static void renombrar(int i, const char *nuevo) {
    char r[TAEK_RUTA]; ruta_de(i, r, sizeof(r));
    char d[TAEK_RUTA]; copiar(d, carpeta, sizeof(d)); if (largo_c(d) > 1) agregar(d, "/", sizeof(d)); agregar(d, nuevo, sizeof(d));
    long x = escribir_op(TAEK_AR_MOVER, r, d, 0, 0);
    copiar(aviso, x ? error_de(x) : "Renombrado.", sizeof(aviso));
    if (!x) marca_hecho("renombrado", d);
}

static void nueva_carpeta(const char *nombre) {
    char d[TAEK_RUTA]; copiar(d, carpeta, sizeof(d)); if (largo_c(d) > 1) agregar(d, "/", sizeof(d)); agregar(d, nombre, sizeof(d));
    long x = escribir_op(TAEK_AR_CREAR_CARPETA, d, 0, 0, 0);
    copiar(aviso, x ? error_de(x) : "Carpeta creada.", sizeof(aviso));
    if (!x) marca_hecho("carpeta creada", d);
}

/* Vuelve a listar la carpeta actual dejando seleccionado 'nombre' si sigue ahi (o la fila de antes) */
static void refrescar(const char *nombre) {
    int antes = sel;
    char guardado[160]; copiar(guardado, aviso, sizeof(guardado));
    char c[TAEK_RUTA]; copiar(c, carpeta, sizeof(c));
    listar(c);
    copiar(aviso, guardado, sizeof(aviso));
    sel = antes < nent ? antes : nent - 1;
    if (sel < 0) sel = 0;
    for (int i = 0; nombre && i < nent; i++) if (!comparar(ent[orden[i]].nombre, nombre)) { sel = i; break; }
}

static int listar(const char *ruta) {
    int n = 0;
    aviso[0] = 0;
    for (;;) {
        memset(&ar, 0, sizeof(ar));
        ar.op = TAEK_AR_LISTAR; copiar(ar.ruta, ruta, sizeof(ar.ruta));
        ar.destino = (uint64_t)(uintptr_t)(ent + n); ar.desde = (uint64_t)n;
        ar.capacidad = MAX_ENT - n > 512 ? 512 : (uint64_t)(MAX_ENT - n);
        long r = taek_archivos(&ar);
        if (r == -26 && !comparar(ruta, PAPELERA)) { n = 0; break; }   /* La papelera aun no existe: esta vacia */
        if (r) {
            copiar(aviso, "No se pudo abrir la carpeta (codigo ", sizeof(aviso)); num(aviso, sizeof(aviso), (uint64_t)-r); agregar(aviso, ")", sizeof(aviso));
            return (int)r;
        }
        n += (int)ar.n;
        if (!ar.n || (uint64_t)n >= ar.total || n >= MAX_ENT) break;
    }
    /* El indice de la papelera no se muestra */
    if (!comparar(ruta, PAPELERA)) for (int i = 0; i < n; i++) if (!comparar(ent[i].nombre, ".indice")) { ent[i] = ent[n - 1]; n--; break; }
    copiar(fs, ar.fs, sizeof(fs));
    disco_abierto = ar.disco_abierto;
    copiar(carpeta, ruta, sizeof(carpeta));
    nent = n; sel = 0; primera = 0; fila_sobre = -1;
    ordenar();
    if (n >= MAX_ENT) copiar(aviso, "Solo se muestran las primeras 4096 entradas", sizeof(aviso));
    return 0;
}

static void subir(void) {
    if (largo_c(carpeta) <= 1) return;
    char antes[TAEK_RUTA]; copiar(antes, carpeta, sizeof(antes));
    char p[TAEK_RUTA]; copiar(p, carpeta, sizeof(p));
    int n = largo_c(p);
    while (n > 1 && p[n - 1] != '/') n--;
    if (n > 1) n--;
    p[n] = 0;
    if (listar(p)) return;
    /* Seleccionar la carpeta de la que se vino */
    const char *nom = antes + largo_c(antes);
    while (nom > antes && nom[-1] != '/') nom--;
    for (int i = 0; i < nent; i++) if (!comparar(ent[orden[i]].nombre, nom)) { sel = i; break; }
}

/* ------------------------------------------------------------------ texto */

static int avance(int px, uint32_t cp) { const struct f_glifo *g = fuente_glifo(fuente_tam(px), cp); return g ? g->avance : 0; }

/* Lo leido a algo que la fuente sabe dibujar: UTF-8 de 1 o 2 bytes; tabuladores a 4 espacios; lo demas, '?' */
static void cargar_texto(const char *ruta, uint64_t cuanto) {
    if (!comparar(ruta, txt_de) && txt_n >= cuanto) return;
    static char crudo[64 * 1024];
    txt_n = 0; txt_total = 0; nlineas = 0; txt_scroll = 0; txt_ancho_hecho = -1;
    copiar(txt_de, ruta, sizeof(txt_de));
    uint64_t pos = 0;
    while (pos < cuanto && txt_n + 8 < TXT_MAX) {
        memset(&ar, 0, sizeof(ar));
        ar.op = TAEK_AR_LEER; copiar(ar.ruta, ruta, sizeof(ar.ruta));
        ar.destino = (uint64_t)(uintptr_t)crudo; ar.capacidad = sizeof(crudo); ar.desde = pos;
        if (taek_archivos(&ar) || !ar.n) break;
        txt_total = ar.total;
        for (uint64_t i = 0; i < ar.n && txt_n + 8 < TXT_MAX; ) {
            unsigned char c = (unsigned char)crudo[i];
            if (c == '\r') { i++; continue; }
            if (c == '\t') { for (int k = 0; k < 4; k++) txt[txt_n++] = ' '; i++; continue; }
            if (c == '\n' || (c >= 32 && c < 127)) { txt[txt_n++] = (char)c; i++; continue; }
            if ((c & 0xE0) == 0xC0 && i + 1 < ar.n && ((unsigned char)crudo[i + 1] & 0xC0) == 0x80) {
                uint32_t cp = (uint32_t)(c & 0x1F) << 6 | ((unsigned char)crudo[i + 1] & 0x3F);
                if (fuente_glifo(fuente_tam(T1), cp)) { txt[txt_n++] = (char)c; txt[txt_n++] = crudo[i + 1]; }
                else txt[txt_n++] = '?';
                i += 2; continue;
            }
            int extra = (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
            txt[txt_n++] = '?';
            i += 1 + (uint64_t)extra;
        }
        pos += ar.n;
        if (pos >= ar.total) break;
    }
    txt[txt_n] = 0;
}

/* Reparte el texto en lineas del ancho dado (cortando en espacios si se puede) */
static void partir(int ancho, int px) {
    if (txt_ancho_hecho == ancho) return;
    txt_ancho_hecho = ancho;
    nlineas = 0;
    int i = 0;
    while (i < (int)txt_n && nlineas < MAX_LINEAS) {
        lineas[nlineas++] = i;
        int w = 0, corte = -1, j = i;
        while (j < (int)txt_n && txt[j] != '\n') {
            const char *q = txt + j;
            uint32_t cp = utf8(&q);
            int a = avance(px, cp);
            if (w + a > ancho && j > i) break;
            w += a;
            if (txt[j] == ' ') corte = (int)(q - txt);
            j = (int)(q - txt);
        }
        if (j < (int)txt_n && txt[j] != '\n' && corte > i) j = corte;
        i = j < (int)txt_n && txt[j] == '\n' ? j + 1 : j;
    }
}

static void dibujar_lineas(int x, int y, int ancho, int alto, int px, int desde) {
    const struct f_tam *ft = fuente_tam(px);
    int paso = ft->alto + S(4);
    partir(ancho, px);
    char b[600];
    for (int k = desde; k < nlineas && y + paso <= alto; k++) {
        int ini = lineas[k], fin = k + 1 < nlineas ? lineas[k + 1] : (int)txt_n;
        int n = 0;
        for (int i = ini; i < fin && n < (int)sizeof(b) - 1; i++) if (txt[i] != '\n') b[n++] = txt[i];
        b[n] = 0;
        texto(&L, x, y + ft->asc, px, TEXTO, 255, 0, b);
        y += paso;
    }
}

/* ------------------------------------------------------------------ dibujo */

static int BARRA_AL, PIE_AL, IZQ_AN, DER_AN, LISTA_X, LISTA_AN, LISTA_Y, LISTA_AL, FILA_AL;

static void etiqueta_tipo(int x, int y, int t) {
    int an = S(54), al = S(28);
    redondo(&L, x, y, an, al, S(5), COLOR_T[t]);
    uint32_t c = t == T_ELF ? 0x000000u : BLANCO;
    texto(&L, x + (an - texto_ancho(T1 - 4, ETIQ[t])) / 2, y + al / 2 + S(6), T1 - 4, c, 255, 0, ETIQ[t]);
}

static void recortar(char *s, int cap, int px, int ancho) {
    if (texto_ancho(px, s) <= ancho) return;
    int n = largo_c(s);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s[n] = 0;
        char t[300]; copiar(t, s, sizeof(t) - 4); agregar(t, "...", sizeof(t));
        if (texto_ancho(px, t) <= ancho) { copiar(s, t, cap); return; }
    }
}

static void dibujar_barra(void) {
    rect(&L, 0, 0, AN, BARRA_AL, BARRA);
    rect(&L, 0, BARRA_AL - S(3), AN, S(3), 0xFF000000u | ROJO);
    /* Marca: rombo rojo y el nombre */
    int cy = BARRA_AL / 2;
    for (int k = 0; k <= S(14); k++) {
        int medio = S(14) - k;   /* Ancho a media altura, angosto hacia las puntas */
        rect(&L, S(42) - medio, cy - k, 2 * medio + 1, 1, 0xFF000000u | ROJO);
        rect(&L, S(42) - medio, cy + k, 2 * medio + 1, 1, 0xFF000000u | ROJO);
    }
    texto(&L, S(64), cy + S(10), 28, BLANCO, 255, -12, en_papelera() ? "PAPELERA" : "ARCHIVOS");
    /* Ruta */
    int rx = IZQ_AN + S(24);
    redondo(&L, rx, S(16), AN - rx - S(24), BARRA_AL - S(35), S(6), CAMPO);
    char r[TAEK_RUTA + 40];
    copiar(r, disco_abierto >= 0 ? "USB " : "", sizeof(r));
    if (disco_abierto >= 0) { num(r, sizeof(r), (uint64_t)disco_abierto); agregar(r, "  \xC2\xB7  ", sizeof(r)); }
    agregar(r, leyendo ? txt_de : carpeta, sizeof(r));
    recortar(r, (int)sizeof(r), T0, AN - rx - S(60));
    texto(&L, rx + S(16), cy + S(8), T0, BLANCO, 255, 0, r);
}

static void dibujar_discos(void) {
    rect(&L, 0, BARRA_AL, IZQ_AN, AL - BARRA_AL - PIE_AL, PANEL);
    texto(&L, S(28), BARRA_AL + S(46), T1, TENUE, 255, 0, "DISCOS");
    int y = BARRA_AL + S(64);
    for (int i = 0; i < ndiscos; i++) {
        const struct taek_disco_usb *d = &discos[i];
        int alto = S(76), activo = (int)d->numero == disco_abierto;
        redondo(&L, S(16), y, IZQ_AN - S(32), alto, S(8), activo ? FILA_SEL : 0xFF151A25u);
        if (activo) rect(&L, S(16), y + S(10), S(4), alto - S(20), 0xFF000000u | ROJO);
        char n[48]; copiar(n, d->nombre[0] ? d->nombre : "Disco USB", sizeof(n));
        recortar(n, (int)sizeof(n), T1, IZQ_AN - S(80));
        texto(&L, S(36), y + S(32), T1, BLANCO, 255, 0, n);
        char t[64] = "USB "; num(t, sizeof(t), d->numero); agregar(t, "  \xC2\xB7  ", sizeof(t));
        char b[24]; tamano_txt(b, sizeof(b), d->bytes); agregar(t, b, sizeof(t));
        if (!d->listo) agregar(t, "  \xC2\xB7  no listo", sizeof(t));
        texto(&L, S(36), y + S(60), T1 - 4, activo ? CIAN : TENUE, 255, 0, t);
        y += alto + S(12);
    }
    if (!ndiscos) texto(&L, S(28), y + S(20), T1, TENUE, 255, 0, "Sin discos USB");
    /* H147: la papelera, como un lugar mas */
    if (disco_abierto >= 0) {
        y += S(14);
        papelera_y = y;
        int activo = en_papelera();
        redondo(&L, S(16), y, IZQ_AN - S(32), S(56), S(8), activo ? FILA_SEL : 0xFF151A25u);
        if (activo) rect(&L, S(16), y + S(10), S(4), S(36), 0xFF000000u | ROJO);
        /* Cubo: cuerpo, tapa y lineas */
        int cx = S(48), cy = y + S(28);
        redondo(&L, cx - S(10), cy - S(8), S(20), S(22), S(3), OPACO(0xB8C2D4u));
        rect(&L, cx - S(13), cy - S(13), S(26), S(4), OPACO(0xE8ECF4u));
        rect(&L, cx - S(4), cy - S(16), S(8), S(3), OPACO(0xE8ECF4u));
        for (int k = -1; k <= 1; k++) rect(&L, cx + k * S(5), cy - S(4), 1, S(15), OPACO(0x5A6478u));
        texto(&L, S(76), y + S(36), T1, BLANCO, 255, 0, "Papelera");
    }
    /* Ayuda al pie de la columna */
    int ay = AL - PIE_AL - S(210);
    static const char *const ayuda[] = { "Intro: abrir", "Retroceso: subir", "Clic derecho: m\xC3\xA1s", "Supr: a la papelera",
                                         "F2: renombrar", "Ctrl+N: nueva carpeta", "Esc: salir", 0 };
    for (int i = 0; ayuda[i]; i++) texto(&L, S(28), ay + i * S(30), T1 - 4, TENUE, 255, 0, ayuda[i]);
}

static void dibujar_lista(void) {
    rect(&L, LISTA_X, LISTA_Y, LISTA_AN, LISTA_AL, FONDO);
    int filas = LISTA_AL / FILA_AL;
    if (sel < primera) primera = sel;
    if (sel >= primera + filas) primera = sel - filas + 1;
    if (primera < 0) primera = 0;
    for (int k = 0; k < filas && primera + k < nent; k++) {
        int i = primera + k, y = LISTA_Y + k * FILA_AL;
        const struct taek_entrada *e = &ent[orden[i]];
        if (i == sel) { rect(&L, LISTA_X, y, LISTA_AN - S(16), FILA_AL, FILA_SEL); rect(&L, LISTA_X, y, S(4), FILA_AL, 0xFF000000u | ROJO); }
        else if (i == fila_sobre) rect(&L, LISTA_X, y, LISTA_AN - S(16), FILA_AL, FILA_SOBRE);
        int t = tipo_de(e->nombre, (int)e->carpeta);
        etiqueta_tipo(LISTA_X + S(20), y + (FILA_AL - S(28)) / 2, t);
        char n[300]; copiar(n, e->nombre, sizeof(n));
        if (e->carpeta) agregar(n, "/", sizeof(n));
        recortar(n, (int)sizeof(n), T0, LISTA_AN - S(260));
        texto(&L, LISTA_X + S(90), y + FILA_AL / 2 + S(8), T0, e->carpeta ? BLANCO : TEXTO, 255, 0, n);
        if (!e->carpeta) {
            char b[24]; tamano_txt(b, sizeof(b), e->tam);
            texto(&L, LISTA_X + LISTA_AN - S(40) - texto_ancho(T1, b), y + FILA_AL / 2 + S(7), T1, TENUE, 255, 0, b);
        }
    }
    if (!nent) texto(&L, LISTA_X + S(30), LISTA_Y + S(50), T0, TENUE, 255, 0, aviso[0] ? aviso : en_papelera() ? "La papelera est\xC3\xA1 vac\xC3\xAD" "a." : "Carpeta vac\xC3\xAD" "a");
    if (nent > filas) {
        int pista = LISTA_AL - S(16), alto = pista * filas / nent;
        if (alto < S(30)) alto = S(30);
        int y = LISTA_Y + S(8) + (pista - alto) * primera / (nent - filas);
        redondo(&L, LISTA_X + LISTA_AN - S(12), LISTA_Y + S(8), S(6), pista, S(3), 0xFF1A2030u);
        redondo(&L, LISTA_X + LISTA_AN - S(12), y, S(6), alto, S(3), 0xFF000000u | ROJO);
    }
}

static void dibujar_previa(void) {
    int x = AN - DER_AN, y = BARRA_AL;
    rect(&L, x, y, DER_AN, AL - BARRA_AL - PIE_AL, PANEL);
    rect(&L, x, y, S(1), AL - BARRA_AL - PIE_AL, 0xFF1E2533u);
    if (!nent) return;
    const struct taek_entrada *e = &ent[orden[sel]];
    int t = tipo_de(e->nombre, (int)e->carpeta);
    etiqueta_tipo(x + S(28), y + S(30), t);
    char n[300]; copiar(n, e->nombre, sizeof(n)); recortar(n, (int)sizeof(n), T0, DER_AN - S(130));
    texto(&L, x + S(98), y + S(52), T0, BLANCO, 255, 0, n);
    char info[120] = "";
    static const char *const que[] = { "Carpeta", "Texto", "Aplicaci\xC3\xB3n de Anillo 3", "Video", "Audio", "Imagen", "Imagen de disco", "Archivo" };
    copiar(info, que[t], sizeof(info));
    if (!e->carpeta) { agregar(info, "  \xC2\xB7  ", sizeof(info)); char b[24]; tamano_txt(b, sizeof(b), e->tam); agregar(info, b, sizeof(info)); }
    texto(&L, x + S(28), y + S(100), T1, CIAN, 255, 0, info);
    rect(&L, x + S(28), y + S(118), DER_AN - S(56), S(1), 0xFF2A3346u);
    int cy = y + S(132), ancho = DER_AN - S(56);
    if (t == T_TEXTO) {
        char r[TAEK_RUTA]; ruta_de(sel, r, sizeof(r));
        cargar_texto(r, 16 * 1024);
        dibujar_lineas(x + S(28), cy, ancho, AL - PIE_AL - S(20), T1, 0);
        texto(&L, x + S(28), AL - PIE_AL - S(16), T1 - 4, TENUE, 255, 0, "Intro: leerlo entero");
    } else {
        const char *msg = t == T_CARPETA ? "Intro o doble clic para entrar." :
                          t == T_ELF ? "Intro para abrirla. Al cerrarla, el explorador vuelve a esta carpeta." :
                          t == T_VIDEO ? "Intro para verlo a pantalla completa con el reproductor del n\xC3\xBA" "cleo. Esc vuelve aqu\xC3\xAD." :
                          t == T_IMAGEN ? "Intro para abrirla en el visor de im\xC3\xA1genes." :
                          t == T_AUDIO ? "Intro para escucharla en el reproductor de m\xC3\xBA" "sica." :
                          t == T_ISO ? "Imagen de disco: Ventoy la puede arrancar." :
                          "Sin vista previa para este tipo de archivo.";
        parrafo(&L, x + S(28), cy + S(24), ancho, T1, S(30), TEXTO, 255, msg);
    }
}

static void dibujar_pie(void) {
    int y = AL - PIE_AL;
    rect(&L, 0, y, AN, PIE_AL, BARRA);
    char t[200] = "";
    if (leyendo) {
        copiar(t, "Leyendo  \xC2\xB7  ", sizeof(t)); char b[24]; tamano_txt(b, sizeof(b), txt_total); agregar(t, b, sizeof(t));
        if (txt_total > TXT_MAX) agregar(t, " (se muestra el primer MB)", sizeof(t));
        agregar(t, "  \xC2\xB7  Flechas y Ctrl+D/U: desplazar  \xC2\xB7  Esc o Retroceso: volver", sizeof(t));
    } else {
        num(t, sizeof(t), (uint64_t)nent); agregar(t, nent == 1 ? " elemento" : " elementos", sizeof(t));
        if (fs[0]) { agregar(t, "  \xC2\xB7  ", sizeof(t)); agregar(t, fs, sizeof(t)); }
        if (en_papelera()) agregar(t, "  \xC2\xB7  Intro: restaurar   Supr: eliminar para siempre", sizeof(t));
        if (aviso[0] && nent) { agregar(t, "  \xC2\xB7  ", sizeof(t)); agregar(t, aviso, sizeof(t)); }
    }
    texto(&L, S(28), y + PIE_AL / 2 + S(7), T1 - 4, TENUE, 255, 0, t);
}

static void dibujar_lectura(void) {
    rect(&L, 0, BARRA_AL, AN, AL - BARRA_AL - PIE_AL, FONDO);
    int ancho = AN - S(240) > S(1200) ? S(1200) : AN - S(240), x = (AN - ancho) / 2;
    partir(ancho, T0);
    int paso = fuente_tam(T0)->alto + S(4), visibles = (AL - BARRA_AL - PIE_AL - S(40)) / paso;
    if (txt_scroll > nlineas - visibles) txt_scroll = nlineas - visibles;
    if (txt_scroll < 0) txt_scroll = 0;
    dibujar_lineas(x, BARRA_AL + S(24), ancho, AL - PIE_AL - S(16), T0, txt_scroll);
    if (nlineas > visibles) {
        int pista = AL - BARRA_AL - PIE_AL - S(32), alto = pista * visibles / nlineas;
        if (alto < S(30)) alto = S(30);
        int y = BARRA_AL + S(16) + (pista - alto) * txt_scroll / (nlineas - visibles);
        redondo(&L, AN - S(24), BARRA_AL + S(16), S(6), pista, S(3), 0xFF1A2030u);
        redondo(&L, AN - S(24), y, S(6), alto, S(3), 0xFF000000u | ROJO);
    }
}

/* ------------------------------------------------------------------ dialogos (H147) */

enum { M_NADA, M_MENU, M_NOMBRE, M_CONFIRMAR };
enum { A_ABRIR, A_RENOMBRAR, A_PAPELERA, A_NUEVA, A_RESTAURAR, A_ELIMINAR, A_VACIAR, A_SUBIR };
static const char *const ACCION_TXT[] = { "Abrir", "Renombrar", "Mover a la papelera", "Nueva carpeta", "Restaurar",
                                          "Eliminar para siempre", "Vaciar la papelera", "Subir una carpeta" };
static int modo, menu_n, menu_sel, menu_x, menu_y, pendiente;
static int menu_acc[8];
static char campo[232]; static int campo_n;
static char pregunta[300];
#define MENU_AN S(330)
#define MENU_FILA S(46)

static void abrir_menu(int x, int y, int sobre_item) {
    menu_n = 0;
    if (sobre_item && nent) {
        if (en_papelera()) { menu_acc[menu_n++] = A_RESTAURAR; menu_acc[menu_n++] = A_ELIMINAR; }
        else { menu_acc[menu_n++] = A_ABRIR; menu_acc[menu_n++] = A_RENOMBRAR; menu_acc[menu_n++] = A_PAPELERA; }
    }
    if (en_papelera()) { if (nent) menu_acc[menu_n++] = A_VACIAR; }
    else menu_acc[menu_n++] = A_NUEVA;
    if (largo_c(carpeta) > 1) menu_acc[menu_n++] = A_SUBIR;
    menu_sel = 0; modo = M_MENU;
    menu_x = x + MENU_AN > AN ? AN - MENU_AN - S(8) : x;
    menu_y = y + menu_n * MENU_FILA + S(16) > AL ? AL - menu_n * MENU_FILA - S(24) : y;
}

static void pedir_nombre(int accion, const char *inicial) {
    pendiente = accion; modo = M_NOMBRE;
    copiar(campo, inicial, sizeof(campo)); campo_n = largo_c(campo);
}
static void confirmar(int accion, const char *texto_pregunta) { pendiente = accion; modo = M_CONFIRMAR; copiar(pregunta, texto_pregunta, sizeof(pregunta)); }

static void ejecutar_accion(int a);

static void panel_modal(int x, int y, int an, int al) {
    for (int k = S(16); k > 0; k -= 2) redondo(&L, x - k, y - k + S(6), an + 2 * k, al + 2 * k, S(12) + k, premul(10, 0));
    redondo(&L, x, y, an, al, S(10), 0xFF161B27u);
    redondo_borde(&L, x, y, an, al, S(10), 0xFF000000u | ROJO);
}

static void dibujar_modal(void) {
    if (modo == M_MENU) {
        int al = menu_n * MENU_FILA + S(16);
        panel_modal(menu_x, menu_y, MENU_AN, al);
        for (int i = 0; i < menu_n; i++) {
            int y = menu_y + S(8) + i * MENU_FILA;
            if (i == menu_sel) { redondo(&L, menu_x + S(6), y, MENU_AN - S(12), MENU_FILA, S(6), FILA_SEL); rect(&L, menu_x + S(6), y + S(8), S(4), MENU_FILA - S(16), 0xFF000000u | ROJO); }
            int peligro = menu_acc[i] == A_ELIMINAR || menu_acc[i] == A_VACIAR;
            texto(&L, menu_x + S(24), y + MENU_FILA / 2 + S(7), T1, peligro ? 0xFF6A80u : BLANCO, 255, 0, ACCION_TXT[menu_acc[i]]);
        }
    } else if (modo == M_NOMBRE || modo == M_CONFIRMAR) {
        int an = S(720), al = modo == M_NOMBRE ? S(210) : S(230), x = (AN - an) / 2, y = (AL - al) / 2;
        rect(&L, 0, 0, AN, AL, premul(110, 0));
        panel_modal(x, y, an, al);
        if (modo == M_NOMBRE) {
            texto(&L, x + S(32), y + S(52), T0, BLANCO, 255, 0, pendiente == A_NUEVA ? "Nueva carpeta" : "Renombrar");
            redondo(&L, x + S(32), y + S(78), an - S(64), S(54), S(6), CAMPO);
            char v[300]; copiar(v, campo, sizeof(v));
            const char *ver = v; while (*ver && texto_ancho(T0, ver) > an - S(110)) ver++;
            int fx = texto(&L, x + S(48), y + S(114), T0, BLANCO, 255, 0, ver);
            rect(&L, fx + S(2), y + S(92), S(2), S(28), 0xFF000000u | NEON);
            texto(&L, x + S(32), y + S(176), T1 - 4, TENUE, 255, 0, "Intro: aceptar   \xC2\xB7   Esc: cancelar");
        } else {
            texto(&L, x + S(32), y + S(52), T0, 0xFF6A80u, 255, 0, "\xC2\xBF" "Seguro?");
            parrafo(&L, x + S(32), y + S(96), an - S(64), T1, S(28), TEXTO, 255, pregunta);
            texto(&L, x + S(32), y + al - S(28), T1 - 4, TENUE, 255, 0, "Intro: s\xC3\xAD, hacerlo   \xC2\xB7   Esc: no");
        }
    }
}

/* Mientras el nucleo escribe (revisa el volumen entero antes): un aviso sobre lo que haya */
static void trabajando(const char *que) {
    int an = S(360), al = S(84), x = (AN - an) / 2, y = (AL - al) / 2;
    panel_modal(x, y, an, al);
    texto(&L, x + (an - texto_ancho(T0, que)) / 2, y + al / 2 + S(8), T0, NEON, 255, 0, que);
    taek_presentar(sup.id, x - S(20), y - S(20), an + S(40), al + S(40));
}

static void dibujar(void) {
    dibujar_barra();
    if (leyendo) dibujar_lectura();
    else { dibujar_discos(); dibujar_lista(); dibujar_previa(); }
    dibujar_pie();
    dibujar_modal();
    taek_presentar(sup.id, 0, 0, AN, AL);
}

/* ------------------------------------------------------------------ acciones */

static void marca(const char *que, const char *ruta) {
    char m[TAEK_RUTA + 40] = "[ARCHIVOS] "; agregar(m, que, sizeof(m)); agregar(m, " ", sizeof(m)); agregar(m, ruta, sizeof(m));
    agregar(m, "\n", sizeof(m));
    taek_escribir(m, largo_c(m));
}
static void marca_hecho(const char *que, const char *ruta) { marca(que, ruta); }

static int abrir_sel(void);
static void subir(void);

/* H147: lo que se eligio en el menu o en un atajo */
static void ejecutar_accion(int a) {
    modo = M_NADA;
    if (!nent && a != A_NUEVA && a != A_SUBIR) return;
    const struct taek_entrada *e = nent ? &ent[orden[sel]] : NULL;
    char nombre[232]; copiar(nombre, e ? e->nombre : "", sizeof(nombre));
    switch (a) {
    case A_ABRIR: break;   /* Lo hace quien llama: puede tener que salir para abrir otra aplicacion */
    case A_RENOMBRAR:
        if (e->carpeta) { copiar(aviso, "Por ahora solo se renombran archivos.", sizeof(aviso)); return; }
        pedir_nombre(A_RENOMBRAR, e->nombre); return;
    case A_NUEVA: pedir_nombre(A_NUEVA, "Nueva carpeta"); return;
    case A_PAPELERA: a_la_papelera(sel); refrescar(NULL); return;
    case A_RESTAURAR: restaurar(sel); refrescar(NULL); return;
    case A_ELIMINAR: {
        char p[300] = "Se eliminar\xC3\xA1 \""; agregar(p, nombre, sizeof(p)); agregar(p, "\" para siempre. No se puede deshacer.", sizeof(p));
        confirmar(A_ELIMINAR, p); return;
    }
    case A_VACIAR: {
        char p[200] = "Se eliminar\xC3\xA1n para siempre los "; num(p, sizeof(p), (uint64_t)nent); agregar(p, " elementos de la papelera. No se puede deshacer.", sizeof(p));
        confirmar(A_VACIAR, p); return;
    }
    case A_SUBIR: subir(); return;
    }
}

/* Intro en un dialogo */
static void aceptar_dialogo(void) {
    int a = pendiente;
    if (modo == M_NOMBRE) {
        modo = M_NADA;
        while (campo_n && campo[campo_n - 1] == ' ') campo[--campo_n] = 0;
        if (!campo_n) return;
        if (a == A_NUEVA) { nueva_carpeta(campo); char n[232]; copiar(n, campo, sizeof(n)); refrescar(n); }
        else if (a == A_RENOMBRAR && comparar(campo, ent[orden[sel]].nombre)) { renombrar(sel, campo); char n[232]; copiar(n, campo, sizeof(n)); refrescar(n); }
    } else if (modo == M_CONFIRMAR) {
        modo = M_NADA;
        if (a == A_ELIMINAR) { eliminar_definitivo(sel); refrescar(NULL); }
        else if (a == A_VACIAR) { vaciar_papelera(); refrescar(NULL); }
    }
}

/* Teclas con un dialogo abierto. 1 si la tecla se uso. */
static int tecla_modal(uint32_t k) {
    if (modo == M_NADA) return 0;
    if (k == 27) { modo = M_NADA; return 1; }
    if (modo == M_MENU) {
        if (k == 17 && menu_sel > 0) menu_sel--;
        else if (k == 18 && menu_sel + 1 < menu_n) menu_sel++;
        else if (k == '\n' || k == '\r') { int a = menu_acc[menu_sel]; if (a == A_ABRIR) { modo = M_NADA; return 2; } ejecutar_accion(a); }
        return 1;
    }
    if (modo == M_CONFIRMAR) { if (k == '\n' || k == '\r' || k == 's' || k == 'S') aceptar_dialogo(); return 1; }
    /* M_NOMBRE: escribir el nombre */
    if (k == '\n' || k == '\r') aceptar_dialogo();
    else if (k == 8 || k == 127) { if (campo_n) { campo_n--; while (campo_n && ((unsigned char)campo[campo_n] & 0xC0) == 0x80) campo_n--; campo[campo_n] = 0; } }
    else if (k >= 32 && k < 256 && campo_n + 3 < (int)sizeof(campo)) {
        if (k < 0x80) campo[campo_n++] = (char)k;
        else { campo[campo_n++] = (char)(0xC0 | (k >> 6)); campo[campo_n++] = (char)(0x80 | (k & 0x3F)); }
        campo[campo_n] = 0;
    }
    return 1;
}

/* 1 si hay que salir (para que el nucleo abra otra aplicacion) */
static int abrir_sel(void) {
    if (!nent) return 0;
    const struct taek_entrada *e = &ent[orden[sel]];
    char r[TAEK_RUTA]; ruta_de(sel, r, sizeof(r));
    int t = tipo_de(e->nombre, (int)e->carpeta);
    if (t == T_CARPETA) { listar(r); marca("carpeta", carpeta); char n[16] = ""; num(n, sizeof(n), (uint64_t)nent); marca("elementos", n); return 0; }
    if (t == T_TEXTO) { cargar_texto(r, TXT_MAX); txt_ancho_hecho = -1; leyendo = 1; txt_scroll = 0; marca("leyendo", r); return 0; }
    /* H146: cada tipo con su aplicacion; al cerrarla, el nucleo vuelve a abrir el explorador en esta carpeta */
    const char *prog = t == T_ELF ? r : t == T_IMAGEN ? "/imagenes.elf" : t == T_AUDIO ? "/musica.elf" : t == T_VIDEO ? TAEK_PROGRAMA_ABRIR : 0;
    if (prog) {
        static struct taek_lanzar l;
        memset(&l, 0, sizeof(l));
        copiar(l.programa, prog, sizeof(l.programa));
        if (t != T_ELF) copiar(l.argumento, r, sizeof(l.argumento));
        copiar(l.argumento_al_volver, carpeta, sizeof(l.argumento_al_volver));
        if (taek_lanzar(&l) == 0) { marca("lanzar", r); return 1; }
    }
    return 0;
}

static void cambiar_disco(int i) {
    if (i < 0 || i >= ndiscos) return;
    if (montar(discos[i].numero)) { copiar(aviso, "No se pudo abrir ese disco", sizeof(aviso)); nent = 0; return; }
    listar("/");
    marca("disco", discos[i].nombre);
}

static int esc_paso; static long esc_t;
static uint32_t filtrar(uint32_t k) {
    if (esc_paso == 1) { if (k == '[') { esc_paso = 2; return 0; } if (k == 'O') { esc_paso = 4; return 0; } esc_paso = 0; }
    else if (esc_paso == 4) { esc_paso = 0; return k == 'Q' ? TAEK_TECLA_F2 : 0; }   /* ESC O Q: F2 desde una terminal */
    else if (esc_paso == 2) { esc_paso = k == '3' ? 3 : 0; return k == 'A' ? 17 : k == 'B' ? 18 : 0; }
    else if (esc_paso == 3) { esc_paso = 0; return k == '~' ? TAEK_TECLA_SUPRIMIR : 0; }   /* ESC [ 3 ~ desde la terminal LAN */
    else if (k == 27) { esc_paso = 1; esc_t = taek_ms(); return 0; }
    return k;
}

/* 1 salir, 0 seguir. Esc (27) llega aqui ya filtrado. */
static int tecla(uint32_t k) {
    int m = tecla_modal(k);
    if (m == 2) return abrir_sel();
    if (m) return 0;
    if (!leyendo && nent) {
        if (k == TAEK_TECLA_SUPRIMIR) { ejecutar_accion(en_papelera() ? A_ELIMINAR : A_PAPELERA); return 0; }
        if (k == TAEK_TECLA_F2 && !en_papelera()) { ejecutar_accion(A_RENOMBRAR); return 0; }
        if ((k == '\n' || k == '\r') && en_papelera()) { ejecutar_accion(A_RESTAURAR); return 0; }
    }
    if (!leyendo && k == 14 && !en_papelera()) { ejecutar_accion(A_NUEVA); return 0; }   /* Ctrl+N */
    if (leyendo) {
        int paso = 3;
        if (k == 27 || k == 8 || k == 127) { leyendo = 0; return 0; }
        if (k == 17) txt_scroll -= paso;
        else if (k == 18) txt_scroll += paso;
        else if (k == 4 || k == ' ') txt_scroll += 15;
        else if (k == 21) txt_scroll -= 15;
        return 0;
    }
    if (k == 27) return 1;
    int filas = LISTA_AL / FILA_AL;
    if (k == 17 && sel > 0) sel--;
    else if (k == 18 && sel + 1 < nent) sel++;
    else if (k == 4) { sel += filas / 2; if (sel >= nent) sel = nent - 1; }
    else if (k == 21) { sel -= filas / 2; if (sel < 0) sel = 0; }
    else if (k == '\n' || k == '\r') return abrir_sel();
    else if (k == 8 || k == 127) subir();
    else if (k > ' ' && k < 127) {
        /* Saltar al siguiente nombre que empieza por esa letra */
        for (int d = 1; d <= nent; d++) {
            int i = (sel + d) % nent;
            if (minus(ent[orden[i]].nombre[0]) == minus((char)k)) { sel = i; break; }
        }
    }
    if (sel < 0) sel = 0;
    return 0;
}

static uint32_t botones_antes;
static long ultimo_clic; static int ultimo_clic_fila = -1;
static int raton(const struct taek_evento *e, int *redibujar) {
    if (!e->hay_puntero) return 0;
    uint32_t subida = e->botones & ~botones_antes;
    botones_antes = e->botones;
    if (leyendo) { if (subida & 2) { leyendo = 0; *redibujar = 1; } return 0; }
    /* H147: dialogos abiertos */
    if (modo == M_MENU) {
        int dentro = e->x >= menu_x && e->x < menu_x + MENU_AN && e->y >= menu_y + S(8) && e->y < menu_y + S(8) + menu_n * MENU_FILA;
        int i = dentro ? (e->y - menu_y - S(8)) / MENU_FILA : -1;
        if (i >= 0 && i != menu_sel) { menu_sel = i; *redibujar = 1; }
        if (subida & 1) {
            *redibujar = 1;
            if (i < 0) { modo = M_NADA; return 0; }
            int a = menu_acc[i];
            if (a == A_ABRIR) { modo = M_NADA; return abrir_sel(); }
            ejecutar_accion(a);
        } else if (subida & 2) { modo = M_NADA; *redibujar = 1; }
        return 0;
    }
    if (modo != M_NADA) { if (subida & 2) { modo = M_NADA; *redibujar = 1; } return 0; }
    int fila = -1;
    if (e->x >= LISTA_X && e->x < LISTA_X + LISTA_AN - S(16) && e->y >= LISTA_Y && e->y < LISTA_Y + LISTA_AL) {
        int k = primera + (e->y - LISTA_Y) / FILA_AL;
        if (k < nent) fila = k;
    }
    if (fila != fila_sobre) { fila_sobre = fila; *redibujar = 1; }
    /* Clic derecho: menu de lo que se puede hacer ahi (sobre un elemento, o sobre la carpeta) */
    if (subida & 2) {
        if (fila >= 0) sel = fila;
        abrir_menu(e->x, e->y, fila >= 0);
        *redibujar = 1;
        return 0;
    }
    if (!(subida & 1)) return 0;
    /* La papelera, en la columna de los discos */
    if (e->x < IZQ_AN && papelera_y >= 0 && e->y >= papelera_y && e->y < papelera_y + S(56)) {
        listar(PAPELERA); indice_leer(); marca("carpeta", carpeta); *redibujar = 1;
        return 0;
    }
    /* Discos */
    if (e->x < IZQ_AN && e->y > BARRA_AL + S(64)) {
        int i = (e->y - BARRA_AL - S(64)) / (S(76) + S(12));
        if (i < ndiscos) { cambiar_disco(i); *redibujar = 1; }
        return 0;
    }
    if (fila < 0) return 0;
    long ahora = taek_ms();
    int doble = fila == ultimo_clic_fila && ahora - ultimo_clic < 450;
    ultimo_clic = ahora; ultimo_clic_fila = fila;
    sel = fila;
    *redibujar = 1;
    if (doble) { ultimo_clic_fila = -1; if (en_papelera()) { ejecutar_accion(A_RESTAURAR); return 0; } return abrir_sel(); }
    return 0;
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
    ent = malloc(sizeof(struct taek_entrada) * MAX_ENT);
    orden = malloc(sizeof(int) * MAX_ENT);
    txt = malloc(TXT_MAX + 8);
    lineas = malloc(sizeof(int) * MAX_LINEAS);
    if (!ent || !orden || !txt || !lineas) return 4;
    T0 = AL >= 1000 ? 22 : 18; T1 = AL >= 1000 ? 18 : 14;
    BARRA_AL = S(84); PIE_AL = S(44); IZQ_AN = S(340); DER_AN = AN / 3;
    LISTA_X = IZQ_AN; LISTA_AN = AN - IZQ_AN - DER_AN; LISTA_Y = BARRA_AL + S(8); LISTA_AL = AL - BARRA_AL - PIE_AL - S(16);
    FILA_AL = S(46);

    /* Donde empezar: la carpeta del argumento (al volver de otra aplicacion) o la raiz del disco abierto */
    char arg[TAEK_RUTA] = "";
    taek_argumento(arg, sizeof(arg));
    pedir_discos();
    if (disco_abierto < 0) for (int i = 0; i < ndiscos; i++) if (discos[i].listo && !montar(discos[i].numero)) break;
    if (disco_abierto >= 0 && (!arg[0] || listar(arg))) listar("/");
    if (disco_abierto < 0) copiar(aviso, "No hay ning\xC3\xBAn disco USB listo", sizeof(aviso));
    marca("ABIERTO", carpeta);

    dibujar();
    struct taek_evento e;
    for (;;) {
        long t = taek_ms();
        uint32_t botones = 0;
        int redibujar = 0;
        for (int i = 0; i < 16; i++) {
            long r = taek_evento(&e);
            if (r < 0) return 1;
            botones |= e.botones;
            if (!r) break;
            uint32_t k = filtrar(e.tecla);
            if (k == 3) return 0;
            if (k) { redibujar = 1; if (tecla(k)) return 0; }
        }
        if (esc_paso == 1 && taek_ms() - esc_t > 40) { esc_paso = 0; redibujar = 1; if (tecla(27)) return 0; }
        e.botones = botones;
        if (raton(&e, &redibujar)) return 0;
        if (redibujar) dibujar();
        long g = taek_ms() - t;
        if (g < 16) taek_esperar(16 - g);
    }
}
