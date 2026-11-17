#include "compositor.h"
#include "compositor_sse2.h"
#include "consola.h"
#include "pantalla.h"
#include "../arquitectura/x86_64/anillo3.h"
#include "../arquitectura/x86_64/anillo3_abi.h"
#include "../base/memoria.h"
#include "../base/tiempo.h"

#define MAX_SUPERFICIES 64u   /* H148: el escritorio suma ventanas de aplicacion */
#define PUNTERO_ESCALA 2
#define PUNTERO_ANCHO (8 * PUNTERO_ESCALA)
#define PUNTERO_ALTO (14 * PUNTERO_ESCALA)
#define NINGUNA 0xFFFFFFFFu

struct superficie { const uint32_t *px; uint32_t ancho, alto, banderas; int32_t x, y; int activa; uint64_t capacidad; };
struct rect { int64_t x0, y0, x1, y1; };

static struct superficie g_sup[MAX_SUPERFICIES];
static uint8_t g_orden[MAX_SUPERFICIES];  /* De atras hacia delante */
static unsigned g_num;
static uint32_t *g_atras;
static uint32_t g_ancho, g_alto, g_paso_fb;
static int g_abierto, g_rechazo;
static int32_t g_px, g_py;
static int g_pv;
static uint64_t g_rechazos, g_composiciones, g_ciclos_componer, g_ciclos_presentar;

/* --------------------------------------------------------------- capa CPU */

/* El memcpy del nucleo va byte a byte; para una pantalla de 8 MB eso es lo que mas pesa. */
static inline void copiar_fila(void *d, const void *o, size_t n) {
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(o), "+c"(n) :: "memory");
}
static int cpu_rellenar(uint32_t *d, uint32_t pd, uint32_t an, uint32_t al, uint32_t c) {
    for (uint32_t y = 0; y < al; y++, d += pd) {
        uint32_t *p = d;
        size_t n = an;
        __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    }
    return 0;
}
static int cpu_copiar(uint32_t *d, uint32_t pd, const uint32_t *o, uint32_t po, uint32_t an, uint32_t al) {
    for (uint32_t y = 0; y < al; y++, d += pd, o += po) copiar_fila(d, o, (size_t)an * 4);
    return 0;
}
static int cpu_mezclar(uint32_t *d, uint32_t pd, const uint32_t *o, uint32_t po, uint32_t an, uint32_t al) {
    comp_mezclar_sse2(d, pd, o, po, an, al);
    return 0;
}
static int cpu_presentar(const uint32_t *o, uint32_t po, uint32_t x, uint32_t y, uint32_t an, uint32_t al) {
    uint8_t *fb = (uint8_t *)pantalla_obtener_base() + (uint64_t)y * g_paso_fb + (uint64_t)x * 4;
    for (uint32_t f = 0; f < al; f++, fb += g_paso_fb, o += po) copiar_fila(fb, o, (size_t)an * 4);
    return 0;
}
static const struct comp_capa g_cpu = { "CPU", cpu_rellenar, cpu_copiar, cpu_mezclar, cpu_presentar };

/* Control negativo: una capa que lo rechaza todo debe dar los mismos pixeles que la CPU. */
static int rechaza_r(uint32_t *a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a;(void)b;(void)c;(void)d;(void)e; g_rechazos++; return -1; }
static int rechaza_c(uint32_t *a, uint32_t b, const uint32_t *c, uint32_t d, uint32_t e, uint32_t f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; g_rechazos++; return -1; }
static int rechaza_p(const uint32_t *a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; g_rechazos++; return -1; }
static const struct comp_capa g_rechazo_capa = { "PRUEBA-RECHAZO", rechaza_r, rechaza_c, rechaza_c, rechaza_p };

static const struct comp_capa *capa(void) { return g_rechazo ? &g_rechazo_capa : &g_cpu; }

static void op_rellenar(uint32_t *d, uint32_t pd, uint32_t an, uint32_t al, uint32_t c) {
    if (capa()->rellenar(d, pd, an, al, c)) cpu_rellenar(d, pd, an, al, c);
}
static void op_copiar(uint32_t *d, uint32_t pd, const uint32_t *o, uint32_t po, uint32_t an, uint32_t al) {
    if (capa()->copiar(d, pd, o, po, an, al)) cpu_copiar(d, pd, o, po, an, al);
}
static void op_mezclar(uint32_t *d, uint32_t pd, const uint32_t *o, uint32_t po, uint32_t an, uint32_t al) {
    if (capa()->mezclar(d, pd, o, po, an, al)) cpu_mezclar(d, pd, o, po, an, al);
}
static void op_presentar(const uint32_t *o, uint32_t po, uint32_t x, uint32_t y, uint32_t an, uint32_t al) {
    if (capa()->presentar(o, po, x, y, an, al)) cpu_presentar(o, po, x, y, an, al);
}

/* ----------------------------------------------------------- composicion */

static int recortar(struct rect *r, const struct rect *l) {
    if (r->x0 < l->x0) r->x0 = l->x0;
    if (r->y0 < l->y0) r->y0 = l->y0;
    if (r->x1 > l->x1) r->x1 = l->x1;
    if (r->y1 > l->y1) r->y1 = l->y1;
    return r->x0 < r->x1 && r->y0 < r->y1;
}

static void dibujar_puntero(const struct rect *r) {
    if (!g_pv) return;
    for (int py = 0; py < PUNTERO_ALTO; py++) for (int px = 0; px < PUNTERO_ANCHO; px++) {
        int x = px / PUNTERO_ESCALA, y = py / PUNTERO_ESCALA;
        int dentro = y < 9 ? x <= y / 2 : (x >= 2 && x <= 4);
        int64_t sx = (int64_t)g_px + px, sy = (int64_t)g_py + py;
        if (dentro && sx >= r->x0 && sx < r->x1 && sy >= r->y0 && sy < r->y1)
            g_atras[(uint64_t)sy * g_ancho + (uint64_t)sx] = (x == 0 || (y < 9 && x == y / 2)) ? 0x00000000u : 0x00FFFFFFu;
    }
}

/* Solo se recompone lo dañado. Los pixeles son memoria del programa: se leen como datos y nunca se confia en ellos. */
static void componer(struct rect r) {
    const struct rect pantalla = { 0, 0, g_ancho, g_alto };
    if (!recortar(&r, &pantalla)) return;
    uint64_t t0 = rdtsc();
    uint32_t an = (uint32_t)(r.x1 - r.x0), al = (uint32_t)(r.y1 - r.y0);
    uint32_t *d = g_atras + (uint64_t)r.y0 * g_ancho + (uint64_t)r.x0;
    /* Lo que queda bajo una superficie opaca que cubre toda la zona no se ve: se empieza por ella. */
    unsigned primera = 0;
    int cubierto = 0;
    for (unsigned k = g_num; k-- > 0;) {
        const struct superficie *s = &g_sup[g_orden[k]];
        if (!(s->banderas & TAEK_SUP_ALFA) && s->x <= r.x0 && s->y <= r.y0 &&
            (int64_t)s->x + s->ancho >= r.x1 && (int64_t)s->y + s->alto >= r.y1) { primera = k; cubierto = 1; break; }
    }
    if (!cubierto) op_rellenar(d, g_ancho, an, al, COMP_FONDO);
    anillo3_acceso_usuario(1);
    for (unsigned k = primera; k < g_num; k++) {
        const struct superficie *s = &g_sup[g_orden[k]];
        struct rect c = r;
        const struct rect sr = { s->x, s->y, (int64_t)s->x + s->ancho, (int64_t)s->y + s->alto };
        if (!recortar(&c, &sr)) continue;
        uint32_t *dd = g_atras + (uint64_t)c.y0 * g_ancho + (uint64_t)c.x0;
        const uint32_t *o = s->px + (uint64_t)(c.y0 - s->y) * s->ancho + (uint64_t)(c.x0 - s->x);
        uint32_t ca = (uint32_t)(c.x1 - c.x0), cl = (uint32_t)(c.y1 - c.y0);
        if (s->banderas & TAEK_SUP_ALFA) op_mezclar(dd, g_ancho, o, s->ancho, ca, cl);
        else op_copiar(dd, g_ancho, o, s->ancho, ca, cl);
    }
    anillo3_acceso_usuario(0);
    dibujar_puntero(&r);
    uint64_t t1 = rdtsc();
    op_presentar(d, g_ancho, (uint32_t)r.x0, (uint32_t)r.y0, an, al);
    g_ciclos_componer += t1 - t0;
    g_ciclos_presentar += rdtsc() - t1;
    g_composiciones++;
}

/* ------------------------------------------------------------------- API */

int compositor_abierto(void) { return g_abierto; }

/* H187: alguien dibujo directo en la pantalla (el video en flujo): se vuelve a pintar todo. */
void compositor_recomponer(void) {
    if (!g_abierto) return;
    struct rect todo = { 0, 0, g_ancho, g_alto };
    componer(todo);
}

int compositor_abrir(void) {
    if (g_abierto) return 0;
    if (!pantalla_obtener_base()) return -TAEK_ENODEV;
    g_ancho = (uint32_t)pantalla_obtener_ancho();
    g_alto = (uint32_t)pantalla_obtener_alto();
    g_paso_fb = (uint32_t)pantalla_obtener_stride_bytes();
    if (!g_ancho || !g_alto || g_paso_fb < g_ancho * 4) return -TAEK_ENODEV;
    g_atras = asignar_memoria((uint64_t)g_ancho * g_alto * 4);
    if (!g_atras) return -TAEK_ENOMEM;
    memset(g_sup, 0, sizeof(g_sup));
    g_num = 0;
    g_pv = 0;
    g_composiciones = g_ciclos_componer = g_ciclos_presentar = 0;
    g_abierto = 1;
    consola_ceder_pantalla(1);
    struct rect todo = { 0, 0, g_ancho, g_alto };
    componer(todo);
    return 0;
}

void compositor_cerrar(void) {
    if (!g_abierto) return;
    g_abierto = 0;
    liberar_memoria(g_atras);
    g_atras = NULL;
    memset(g_sup, 0, sizeof(g_sup));
    g_num = 0;
    consola_ceder_pantalla(0);
}

int compositor_crear(uint64_t pixeles, uint32_t ancho, uint32_t alto, int32_t x, int32_t y, uint32_t banderas) {
    if (!g_abierto) return -TAEK_ENODEV;
    if (g_num >= MAX_SUPERFICIES) return -TAEK_ENOMEM;
    unsigned i = g_num;
    g_sup[i] = (struct superficie){ (const uint32_t *)pixeles, ancho, alto, banderas, x, y, 1, (uint64_t)ancho * alto };
    g_orden[g_num++] = (uint8_t)i;
    return (int)i;
}

int compositor_elevar(uint32_t id) {
    if (!g_abierto || id >= g_num || !g_sup[id].activa) return -TAEK_EBADF;
    unsigned k = 0;
    while (k < g_num && g_orden[k] != id) k++;
    if (k >= g_num) return -TAEK_EBADF;
    for (; k + 1 < g_num; k++) g_orden[k] = g_orden[k + 1];
    g_orden[g_num - 1] = (uint8_t)id;
    const struct superficie *s = &g_sup[id];
    struct rect r = { s->x, s->y, (int64_t)s->x + s->ancho, (int64_t)s->y + s->alto };
    componer(r);
    return 0;
}

static struct superficie *buscar(uint32_t id) {
    return g_abierto && id < MAX_SUPERFICIES && g_sup[id].activa ? &g_sup[id] : NULL;
}

int compositor_presentar(uint32_t id, int64_t x, int64_t y, int64_t ancho, int64_t alto) {
    struct superficie *s = buscar(id);
    if (!s) return -TAEK_EBADF;
    if (x < 0 || y < 0 || ancho <= 0 || alto <= 0 || x >= s->ancho || y >= s->alto ||
        ancho > (int64_t)s->ancho - x || alto > (int64_t)s->alto - y) return -TAEK_EINVAL;
    struct rect r = { s->x + x, s->y + y, s->x + x + ancho, s->y + y + alto };
    componer(r);
    return 0;
}

int compositor_mover(uint32_t id, int32_t x, int32_t y) {
    struct superficie *s = buscar(id);
    if (!s) return -TAEK_EBADF;
    if (x < -32768 || y < -32768 || x > 32767 || y > 32767) return -TAEK_EINVAL;
    struct rect antes = { s->x, s->y, (int64_t)s->x + s->ancho, (int64_t)s->y + s->alto };
    s->x = x;
    s->y = y;
    struct rect ahora = { x, y, (int64_t)x + s->ancho, (int64_t)y + s->alto };
    /* Si se tocan, una sola pasada: componer dos veces lo comun es el doble de trabajo para el mismo resultado. */
    if (antes.x0 < ahora.x1 && ahora.x0 < antes.x1 && antes.y0 < ahora.y1 && ahora.y0 < antes.y1) {
        if (ahora.x0 < antes.x0) antes.x0 = ahora.x0;
        if (ahora.y0 < antes.y0) antes.y0 = ahora.y0;
        if (ahora.x1 > antes.x1) antes.x1 = ahora.x1;
        if (ahora.y1 > antes.y1) antes.y1 = ahora.y1;
        componer(antes);
        return 0;
    }
    componer(antes);
    componer(ahora);
    return 0;
}

/* H148: otro tamano visible dentro de los pixeles que ya tiene, y a la vez otra posicion (al escalar desde el borde
 * izquierdo o de arriba cambian las dos: en dos llamadas la pantalla veria un cuadro mezclado). Se recompone lo que
 * ocupaba y lo que ocupa. */
int compositor_tamano(uint32_t id, int32_t x, int32_t y, int64_t ancho, int64_t alto) {
    struct superficie *s = buscar(id);
    if (!s) return -TAEK_EBADF;
    if (ancho <= 0 || alto <= 0 || ancho > 8192 || alto > 8192 || (uint64_t)ancho * (uint64_t)alto > s->capacidad) return -TAEK_EINVAL;
    if (x < -32768 || y < -32768 || x > 32767 || y > 32767) return -TAEK_EINVAL;
    struct rect antes = { s->x, s->y, (int64_t)s->x + s->ancho, (int64_t)s->y + s->alto };
    s->x = x; s->y = y;
    s->ancho = (uint32_t)ancho;
    s->alto = (uint32_t)alto;
    struct rect ahora = { x, y, (int64_t)x + ancho, (int64_t)y + alto };
    if (antes.x0 < ahora.x1 && ahora.x0 < antes.x1 && antes.y0 < ahora.y1 && ahora.y0 < antes.y1) {
        if (ahora.x0 < antes.x0) antes.x0 = ahora.x0;
        if (ahora.y0 < antes.y0) antes.y0 = ahora.y0;
        if (ahora.x1 > antes.x1) antes.x1 = ahora.x1;
        if (ahora.y1 > antes.y1) antes.y1 = ahora.y1;
        componer(antes);
        return 0;
    }
    componer(antes);
    componer(ahora);
    return 0;
}

void compositor_puntero(int32_t x, int32_t y, int visible) {
    if (!g_abierto || (visible == g_pv && x == g_px && y == g_py)) return;
    struct rect antes = { g_px, g_py, (int64_t)g_px + PUNTERO_ANCHO, (int64_t)g_py + PUNTERO_ALTO };
    int habia = g_pv;
    g_px = x; g_py = y; g_pv = visible;
    if (habia) componer(antes);
    if (visible) {
        struct rect ahora = { x, y, (int64_t)x + PUNTERO_ANCHO, (int64_t)y + PUNTERO_ALTO };
        componer(ahora);
    }
}

const char *compositor_capa_nombre(void) { return capa()->nombre; }
void compositor_forzar_rechazo(int activo) { g_rechazo = activo; g_rechazos = 0; }
uint64_t compositor_rechazos(void) { return g_rechazos; }
void compositor_estadisticas(uint64_t *c, uint64_t *cc, uint64_t *cp) {
    *c = g_composiciones; *cc = g_ciclos_componer; *cp = g_ciclos_presentar;
}
uint32_t compositor_leer_pantalla(uint32_t x, uint32_t y) {
    const uint8_t *fb = (const uint8_t *)pantalla_obtener_base();
    return *(const volatile uint32_t *)(fb + (uint64_t)y * pantalla_obtener_stride_bytes() + (uint64_t)x * 4) & 0x00FFFFFFu;
}
