/* Interfaz v3 de TAEK OS en Anillo 3: menu principal, submenus, pantallas de sistema, escaneo y escritorio.
 * Portada del prototipo "experimental/Interfaz grafica - Persona 5 v3": el gato NekoSaka en su hexagono, el riel inclinado,
 * la tarjeta inspectora y una transicion propia por opcion.
 *   INICIO         ola de luz por las facetas, rayos, ondas hexagonales y el sello NEKOSAKA que cae letra por letra
 *   APLICACIONES   el gato se separa en facetas y cada una dispara un rayo que enciende una opcion
 *   ESCRITORIO     las lineas del gato vuelan, forman NEKOSAKA y dibujan el escritorio Aero; al volver, el camino inverso
 *   CONFIGURACION  reticula de escaneo sobre el gato y datos reales del equipo que se descifran
 *   APAGAR         sacudida, linea plana sobre el gato y opciones que entran con falla de senal
 * Como siempre sin GPU: lo quieto son capas que se mueven; lo que vuela se dibuja solo en la zona que cambia.
 * Lo que muestra sale del sistema; lo que aun no existe lo dice. Lo que solo el nucleo puede hacer se pide terminando con un codigo. */
#include "v3.h"
/* H148: las aplicaciones del escritorio (ventanas.h) usan el monton, los discos y dos bibliotecas de un solo archivo.
 * Van antes que las macros de este archivo (S, W, H...), que no deben tocarlas. */
#include "memoria.h"
#include "apps.h"
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
#define MINIMP3_NO_STD_HEADERS
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wunused-function"
#include "nucleo/controladores/multimedia/imagen/stb_image.h"
#include "nucleo/controladores/multimedia/mp3/minimp3.h"
#pragma clang diagnostic pop

TAEK_PROGRAMA

#define ROJO 0xFF003Cu
#define CARMIN 0xC70014u
#define NEON 0xFCEE0Au
#define CIAN 0x00F0FFu
#define BLANCO 0xFFFFFFu
#define NEGRO 0x07070Cu
#define GRIS 0xA8A8B4u
#define OPACO(c) (0xFF000000u | (c))
#define OCULTO 32000

static int W, H;
#define S(v) ((int)((int64_t)(v) * H / 1080))
#define SF(v) ((float)(v) * (float)H / 1080.0f)
#define INCL 14  /* Inclinacion de los titulos */

/* H151: las cinco secciones y sus submenus son los del prototipo "experimental/Interfaz taek Os v1". Cada opcion tiene
 * un panel en la tarjeta inspectora y una accion con Intro. Todo lo que hacen es real: ordenes de la terminal del nucleo
 * (SYS_ORDEN o pantalla completa), aplicaciones del USB, las pantallas de sistema o la cabina MAGI. */
enum { A_NADA = 0, A_VOLVER = -1, A_ESPECIFICACIONES = -2, A_BATERIA = -3,
       A_PANEL = -4,     /* Intro le da el teclado al panel (Terminal, Stream) o lo vuelve a leer (Huevo, Oraculo...) */
       A_CABINA = -5,    /* La cabina MAGI en el modulo del panel */
       A_APP = -6,       /* Abrir un programa del USB (arg) y volver */
       A_ORDEN = -7,     /* Orden de pantalla completa (arg) */
       A_CONFIRMAR = -8  /* Orden de pantalla completa que pide Intro dos veces (arg) */ };
enum { P_NADA, P_TABLERO, P_CPU, P_GPU, P_HUEVO, P_PAGINAS, P_DISCOS, P_LAN, P_ESTRES, P_TERMINAL, P_NAVEGADOR, P_MEDIA,
       P_VFS, P_STREAM, P_ORACULO, P_RULETA, P_MOD0, P_MOD7 = P_MOD0 + 7, P_APAGAR, P_REINICIO, P_TRIPLE, P_CANCELAR };
struct opcion { const char *insignia, *titulo, *resumen, *detalle; int accion; int panel; const char *arg; };

static const struct opcion sub_inicio[] = {
    { "01-A", "TABLERO GENERAL", "Centro de control y rendimiento, en vivo", NULL, A_NADA, P_TABLERO, NULL },
    { "01-B", "RENDIMIENTO CPU Y NÚCLEOS", "Carga real de cada hilo y frecuencia efectiva", NULL, A_ORDEN, P_CPU, "stress" },
    { "01-C", "GPU // VIDEO POR HARDWARE", "Adaptador, video H.264 y prueba de velocidad", NULL, A_ORDEN, P_GPU, "h264 prueba 360p" },
    { "01-D", "EL HUEVO // GUARDIÁN RING 0", "Canarios 0x7AEECC05 y 'memoria probar'", NULL, A_PANEL, P_HUEVO, "memoria probar" },
    { "01-E", "PAGINACIÓN PML4", "Árbol de 4 niveles y traductor de direcciones", NULL, A_PANEL, P_PAGINAS, "paginacion" },
    { "01-F", "ALMACENAMIENTO VFS", "Discos USB, volumen y offset 0x6A", NULL, A_APP, P_DISCOS, "/archivos.elf" },
    { "01-G", "TELEMETRÍA LAN 9153", "Laptop y PC por la consola en red", NULL, A_PANEL, P_LAN, NULL },
    { "01-H", "TEST DE ESTRÉS", "Memoria, DMA, USB y audio a la vez", NULL, A_ORDEN, P_ESTRES, "stress" },
    { "01-I", "ESPECIFICACIONES", "Procesador, memoria, placa y discos", "Abre el panorama completo del equipo. Sin números de serie.", A_ESPECIFICACIONES, P_NADA, NULL },
    { "01-J", "BATERÍA", "Carga, salud y consumo", "Lee la batería y el cargador con los métodos ACPI del firmware.", A_BATERIA, P_NADA, NULL },
};
static const struct opcion sub_apps[] = {
    { "02-A", "TERMINAL TAEK", "Órdenes reales del núcleo, aquí mismo", NULL, A_PANEL, P_TERMINAL, NULL },
    { "02-B", "NAVEGADOR WEB", "HTTP y HTTPS con mbedTLS (navegador.elf)", NULL, A_APP, P_NAVEGADOR, "/navegador.elf" },
    { "02-C", "REPRODUCTOR MULTIMEDIA", "H.264 por GPU, MP3 y videos del USB", NULL, A_APP, P_MEDIA, "/videos.elf" },
    { "02-D", "EXPLORADOR VFS", "FAT32 y exFAT, con papelera", NULL, A_APP, P_VFS, "/archivos.elf" },
    { "02-E", "STREAMING LAN 9153", "Consola en red y mensajes al PC", NULL, A_PANEL, P_STREAM, NULL },
    { "02-F", "ORÁCULO SAGRADO", "Pasajes y God Words con entropía rdtsc", NULL, A_PANEL, P_ORACULO, "orar" },
    { "02-G", "RULETA RUSA", "El duelo de verdad: si pierdes, cae el sistema", NULL, A_CONFIRMAR, P_RULETA, "ruleta" },
};
static const struct opcion sub_config[] = {
    { "04-A", "PILA DE RED Y ARP", "Controlador, IP, contadores y caché ARP", NULL, A_CABINA, P_MOD0 + 0, NULL },
    { "04-B", "STREAMING LAN // TCP 9153", "Consola hacia el PC: estado y ritmo", NULL, A_CABINA, P_MOD0 + 1, NULL },
    { "04-C", "AUDIO // COLA PCM", "HDA o AC97, volumen y tono de prueba", NULL, A_CABINA, P_MOD0 + 2, NULL },
    { "04-D", "GPU Y PANTALLA", "Adaptador, compositor y cuadros", NULL, A_CABINA, P_MOD0 + 3, NULL },
    { "04-E", "GUARDIÁN RING 0 // EL HUEVO", "Mapa del montón y canarios", NULL, A_CABINA, P_MOD0 + 4, NULL },
    { "04-F", "ALMACENAMIENTO Y VOLUMEN", "Discos y bandera 0x6A del exFAT", NULL, A_CABINA, P_MOD0 + 5, NULL },
    { "04-G", "TÉRMICO // SENSOR DIGITAL", "Temperatura por MSR y alarma a 90 °C", NULL, A_CABINA, P_MOD0 + 6, NULL },
    { "04-H", "LATENCIA // ATENCIÓN DE CPU 0", "USB, red, audio y llamadas largas (H204)", NULL, A_CABINA, P_MOD0 + 7, NULL },
};
static const struct opcion sub_apagar[] = {
    { "05-A", "APAGAR EQUIPO AHORA", "Apagado limpio por ACPI", NULL, TAEK_ACCION_APAGAR, P_APAGAR, NULL },
    { "05-B", "REINICIO RÁPIDO // VENTOY", "Reinicia; Ventoy arranca lo que diga ventoy.json", NULL, TAEK_ACCION_REINICIAR, P_REINICIO, NULL },
    { "05-C", "TRIPLE FAULT // HARAKIRI", "Destrucción real: IDT, TSS y pila", NULL, A_CONFIRMAR, P_TRIPLE, "harakiri" },
    { "05-D", "CANCELAR Y SEGUIR", "Volver al menú", NULL, A_VOLVER, P_CANCELAR, NULL },
};

struct seccion { const char *titulo, *riel, *orden, *burbuja, *etiqueta, *pista; const struct opcion *sub; int nsub; };
static const struct seccion secciones[] = {
    { "INICIO", "CORE SILICON // i7 // EN VIVO", "CENTRO DE CONTROL // EN VIVO",
      "Centro de control general. El Huevo, el silicio Intel y la memoria, medidos ahora.", "[ NEKOSAKA KERNEL // TELEMETRÍA DE SILICIO ]", "[INTRO] DESPLIEGA EL CENTRO DE CONTROL", sub_inicio, 10 },
    { "APLICACIONES", "TERMINAL // RED // MEDIA // VFS", "TERMINAL, NAVEGADOR, MEDIA Y VFS",
      "Módulos reales de TAEK OS: terminal, navegador TLS, reproductor y archivos.", "[ MATRIZ DE APPS // NATIVAS ]", "[INTRO] EL GATO SE DESARMA Y ENCIENDE CADA OPCIÓN", sub_apps, 7 },
    { "ESCRITORIO", "FRUTIGER AERO // VENTANAS", "FUNDIDO A FRUTIGER AERO",
      "¿Deseas el entorno cristalino? Neko se desarma en líneas y dibuja el escritorio.", "[ DIMENSIONAL WARP // SELECTOR ]", "[INTRO] O [TAB] PARA SALTAR", NULL, 0 },
    { "CONFIGURACIÓN", "RED // AUDIO // RING 0 // MAGI", "AUDIO, RED, DRIVER Y RING 0",
      "Escaneo del equipo y los ocho módulos del sistema MAGI, con datos reales.", "[ MAGI SYSTEM // CONFIGURACIÓN ]", "[INTRO] ESCANEA Y DESPLIEGA LOS MÓDULOS", sub_config, 8 },
    { "APAGAR", "ACPI // REINICIO // HARAKIRI", "CONTROL DE ENERGÍA // ACPI Y REINICIO",
      "¿Detenemos el equipo? Nada ocurre hasta confirmar con Intro.", "[ ENERGÍA // ACPI RING 0 ]", "[INTRO] OPCIONES DE ENERGÍA", sub_apagar, 4 },
};
#define NSEC 5
#define NFILAS 10
#define FILAS_VISIBLES 7
#define NESQ 6

static struct taek_superficie s_fondo, s_heroe, s_scan, s_fila[NFILAS], s_esq[NESQ], s_insp, s_crono, s_estado, s_pie, s_pan, s_fx, s_barre;
static struct lienzo l_fondo, l_heroe, l_scan, l_fila[NFILAS], l_esq[NESQ], l_insp, l_crono, l_estado, l_pie, l_pan, l_fx, l_barre;
static int hay_barrido;

enum { E_MENU, E_ESPECIFICACIONES, E_BATERIA, E_ESCRITORIO, E_ESCANEO, E_CABINA };
static int estado = E_MENU;
static int nivel, sel, subsel;
static struct taek_info info;
static struct taek_equipo eq;
static int eq_leido, modo_prueba;
static uint32_t azar_estado = 0x9E3779B9u;
static uint32_t azar(void) { azar_estado ^= azar_estado << 13; azar_estado ^= azar_estado >> 17; azar_estado ^= azar_estado << 5; return azar_estado; }
static float azarf(void) { return (float)(azar() & 0xFFFFu) / 65535.0f; }

/* Un resorte: lo que da el rebote. Acelera hacia su objetivo y pierde energia en cada paso. */
struct muelle { float pos, vel, obj; int dibujado; };
static inline void muelle_paso(struct muelle *m, float k, float amort) {
    m->vel += (m->obj - m->pos) * k;
    m->vel *= amort;
    m->pos += m->vel;
}
static inline int muelle_quieto(const struct muelle *m) {
    float d = m->obj - m->pos, v = m->vel;
    return d < 0.5f && d > -0.5f && v < 0.5f && v > -0.5f;
}

/* Cada fila tiene su resorte y su manera de entrar: con retraso, encendida por un rayo o con falla de senal. */
struct fila { struct muelle m; int retraso, oculta, falla; long falla_t0; };
static struct fila fila[NFILAS];
static struct muelle m_insp, m_gato;
struct esquirla { float x, y, vx, vy; int vida; };
static struct esquirla esq[NESQ];
static long cuadro;
static long proximo_glitch;

/* El heroe: fragmentos y emblema en una capa que flota. Solo se repinta cuando el gato cambia de pose. */
static int hx, hy, hw, hh, heroe_visible = 1, heroe_dy, heroe_sucio;
static struct pose_gato pose;
static float sep_obj;
/* Mientras un efecto mueve las filas o el gato a mano, los resortes no los tocan. */
static int filas_manuales, heroe_manual;
static int panel_foco, confirmar;   /* H151: el teclado lo tiene el panel de la opcion; Intro pendiente de confirmar */

#include "sensores.h"   /* H150 */
#include "ordenes.h"    /* H151: ordenes reales del nucleo y el widget de terminal */

/* ------------------------------------------------------------- superficies */

static int crear(struct taek_superficie *s, struct lienzo *l, int an, int al, int x, int y, uint32_t banderas) {
    *s = (struct taek_superficie){ .ancho = (uint32_t)an, .alto = (uint32_t)al, .x = x, .y = y, .banderas = banderas };
    long r = taek_superficie(s);
    if (r < 0) return (int)r;
    *l = (struct lienzo){ (uint32_t *)s->pixeles, an, al };
    return 0;
}
static inline void ocultar(const struct taek_superficie *s) { taek_mover(s->id, OCULTO, 0); }

/* Titulo con los canales separados: rojo y cian asoman a los lados. */
static void titulo_rgb(struct lienzo *l, int x, int y, int px, uint32_t c, int sep, const char *s) {
    if (sep) {
        texto(l, x - sep, y, px, CIAN, 200, INCL, s);
        texto(l, x + sep, y, px, ROJO, 200, INCL, s);
    }
    texto(l, x, y, px, c, 255, INCL, s);
}

/* ------------------------------------------------------------------ fondo */

/* Cuatro paradas de la cuchilla clara: blanco, perla, gris azulado y casi negro en la punta. */
static uint32_t color_cuchilla(float t) {
    static const float pos[4] = { 0, 0.45f, 0.80f, 1.0f };
    static const uint32_t col[4] = { 0xFFFFFFFFu, 0xFFF6F6F9u, 0xFFE2E4EDu, 0xFF1A1C24u };
    t = limitar01(t);
    for (int i = 0; i < 3; i++)
        if (t <= pos[i + 1]) return entre(col[i], col[i + 1], (uint32_t)((t - pos[i]) / (pos[i + 1] - pos[i]) * 255.0f));
    return col[3];
}

static int borde_cuchilla(int y) { float bw = (float)W * 0.46f; return (int)(bw * 0.88f - bw * 0.40f * (float)y / (float)H); }

static void dibujar_fondo(void) {
    struct lienzo *l = &l_fondo;
    /* Noche: degradado radial centrado arriba a la derecha */
    float cx = (float)W * 0.75f, cy = (float)H * 0.25f, rx = (float)W * 0.95f, ry = (float)H * 0.65f;
    float rcx = (float)W * 0.6f, rcy = (float)H * 0.3f, rr = 0.5f * raizf(rcx * rcx + 0.49f * (float)H * (float)H);
    for (int y = 0; y < H; y++) {
        uint32_t *p = l->px + (uint64_t)y * W;
        float dy = ((float)y - cy) / ry, dy2 = ((float)y - rcy);
        for (int x = 0; x < W; x++) {
            float dx = ((float)x - cx) / rx, d = raizf(dx * dx + dy * dy);
            uint32_t c;
            if (d < 0.38f) c = entre(0xFF141622u, 0xFF0C0D14u, (uint32_t)(d / 0.38f * 255.0f));
            else if (d < 0.75f) c = entre(0xFF0C0D14u, 0xFF050609u, (uint32_t)((d - 0.38f) / 0.37f * 255.0f));
            else c = entre(0xFF050609u, 0xFF000000u, (uint32_t)(limitar01((d - 0.75f) / 0.25f) * 255.0f));
            float ex = (float)x - rcx, e = raizf(ex * ex + dy2 * dy2) / rr;
            if (e < 1) c = sobre(c, premul((uint32_t)((1 - e) * 31.0f), ROJO));
            p[x] = c;
        }
    }
    /* Reticula tenue y lineas de barrido, solo en la noche */
    for (int x = 0; x < W; x += S(60)) rect(l, x, 0, 1, H, premul(9, BLANCO));
    for (int y = 0; y < H; y += S(60)) rect(l, 0, y, W, 1, premul(9, BLANCO));
    /* Trama de puntos rojos arriba a la derecha */
    int t = S(22);
    for (int y = S(110), f = 0; y < S(560); y += t, f++)
        for (int x = W * 3 / 5 + (f & 1) * t / 2; x < W; x += t) {
            int r = S(6) - (y - S(110)) * S(6) / S(450);
            if (r > 0) rect(l, x, y, r, r, premul(40, ROJO));
        }
    /* La cuchilla clara, con su borde suavizado */
    float bw = (float)W * 0.46f;
    for (int y = 0; y < H; y++) {
        uint32_t *p = l->px + (uint64_t)y * W;
        float borde = bw * 0.88f - bw * 0.40f * (float)y / (float)H;
        int xb = (int)borde + 1;
        if (xb > W) xb = W;
        for (int x = 0; x < xb; x++) {
            uint32_t c = color_cuchilla(((float)x + (float)y) / (bw + (float)H));
            float cob = borde - (float)x;
            p[x] = cob >= 1 ? c : sobre(p[x], cubrir(c, (uint32_t)(limitar01(cob) * 255.0f)));
        }
    }
    /* Filo rojo y negro a lo largo de la cuchilla */
    trazo(l, bw * 0.88f + SF(16), 0, bw * 0.48f + SF(16), (float)H, SF(4), premul(220, ROJO), 0);
    trazo(l, bw * 0.88f + SF(30), 0, bw * 0.48f + SF(30), (float)H, SF(1.5f), premul(120, BLANCO), 0);
    /* Letrero vertical enorme y tenue: NEKOSAKA // TAEK OS, de abajo hacia arriba. La capa de efectos, aun vacia, presta su memoria */
    float esc = SF(157) / 240.0f;
    if ((int64_t)l_fx.an * l_fx.al >= 2400 * 300) {
        texto_girado(l, l_fx.px, S(-6), H - S(10), 240, esc, 0x022355u, 46, "TAEK OS");
        texto_girado(l, l_fx.px, S(178), H - S(10), 240, esc, 0x022355u, 46, "NEKOSAKA //");
        limpiar(&l_fx, 0);
    }
    /* Ondas de sonar alrededor del gato */
    float ex, ey;
    logo_punto(LOGO_CENTRO_X, LOGO_CENTRO_Y, &ex, &ey);
    anillo(l, (int)ex, (int)ey, S(420), S(418), premul(70, ROJO));
    anillo(l, (int)ex, (int)ey, S(500), S(499), premul(40, ROJO));
    anillo(l, (int)ex, (int)ey, S(360), S(357), premul(40, BLANCO));
    pintar_cables(l);
    for (int y = 0; y < H; y += 4) rect(l, borde_cuchilla(y), y, W, 1, premul(26, 0));
    taek_presentar(s_fondo.id, 0, 0, W, H);
}

/* ------------------------------------------------------------------ heroe */

static void pintar_heroe(void) {
    heroe_sucio = 0;
    limpiar(&l_heroe, 0);
    pintar_fragmentos(&l_heroe, (float)hx, (float)hy);
    pintar_emblema(&l_heroe, (float)hx, (float)hy, &pose);
    taek_presentar(s_heroe.id, 0, 0, hw, hh);
}
static void mover_heroe(int forzar) {
    if (!heroe_visible || (heroe_manual && !forzar)) return;
    int dy = (int)(sen((float)cuadro * 0.045f) * SF(9)) + (int)m_gato.pos;
    if (dy != heroe_dy || forzar) { heroe_dy = dy; taek_mover(s_heroe.id, hx, hy + dy); }
}
static void heroe_mostrar(int si) {
    heroe_visible = si;
    if (si && heroe_sucio) pintar_heroe();
    if (si) mover_heroe(1); else ocultar(&s_heroe);
}
/* Quita o pone el emblema dejando los fragmentos: lo que pasa mientras las lineas del gato vuelan. */
static void heroe_sin_gato(int si) {
    pose.sin_gato = (uint8_t)si;
    if (heroe_visible) pintar_heroe(); else heroe_sucio = 1;
}

/* ------------------------------------------------------------------- filas */

static int opciones(void) { return nivel ? secciones[sel].nsub : NSEC; }
/* H151: los submenus largos (INICIO tiene 10) muestran 7 filas y se desplazan con la elegida. desp: la primera a la vista. */
static int desp;
static int sub_largo(void) { return nivel && secciones[sel].nsub > 4; }
static int fila_visible(int i) { return !sub_largo() || (i >= desp && i < desp + FILAS_VISIBLES); }
static int fila_y(int i) {
    if (!nivel) return S(380) + i * S(76);
    if (!sub_largo()) return S(372) + i * S(88);
    return fila_visible(i) ? S(282) + (i - desp) * S(80) : OCULTO;
}
static int fila_x(int i) { return nivel ? S(690) - (sub_largo() ? i - desp : i) * S(10) : S(672) - i * S(13); }
static int fila_alto(void) { return !nivel ? S(76) : sub_largo() ? S(80) : S(88); }
#define FILA_AN S(900)
#define FILA_AL S(100)

/* Menu principal: titulo enorme en cursiva. La activa va sobre una franja roja con sombra negra. */
static void fila_principal(struct lienzo *l, int i, int activo) {
    const struct seccion *s = &secciones[i];
    int x = S(46);
    if (activo) {
        para(l, x + S(14), S(22), S(520), S(64), S(26), OPACO(NEGRO));
        para(l, x + S(470), S(14), S(300), S(64), S(26), premul(90, ROJO));
        para(l, x, S(14), S(520), S(64), S(26), OPACO(ROJO));
        para(l, x - S(26), S(8), S(12), S(80), S(6), OPACO(BLANCO));
        texto(l, x + S(26) + S(4), S(66) + S(4), S(48), NEGRO, 160, INCL, s->titulo);
        texto(l, x + S(26), S(66), S(48), BLANCO, 255, INCL, s->titulo);
        int w = texto_ancho(S(14), s->riel) + S(24);
        para(l, x + S(20), S(74), w, S(20), S(6), OPACO(NEGRO));
        texto(l, x + S(30), S(89), S(14), NEON, 255, 0, s->riel);
    } else {
        texto(l, x + S(3), S(58) + S(3), S(36), NEGRO, 210, INCL, s->titulo);
        texto(l, x, S(58), S(36), BLANCO, 255, INCL, s->titulo);
        texto(l, x + S(4), S(80), S(14), 0x8E94A8u, 255, 0, s->riel);
    }
}

/* Submenu: rombo con su insignia y una tira con borde; la activa es blanca con sombra roja y un cursor que apunta. */
static void fila_sub(struct lienzo *l, int i, int activo) {
    const struct opcion *o = &secciones[sel].sub[i];
    float bx = SF(70), by = SF(50), r = SF(activo ? 27 : 23);
    float rombo[8] = { bx, by - r, bx + r, by, bx, by + r, bx - r, by };
    poligono(l, rombo, 4, activo ? OPACO(ROJO) : OPACO(BLANCO));
    int wi = texto_ancho(S(14), o->insignia);
    texto(l, (int)bx - wi / 2, (int)by + S(5), S(14), activo ? BLANCO : NEGRO, 255, 0, o->insignia);
    if (activo) {
        float c[6] = { SF(4), by - SF(17), SF(30), by, SF(4), by + SF(17) };
        poligono(l, c, 3, OPACO(ROJO));
    }
    float x0 = SF(112), y0 = SF(18), an = SF(activo ? 640 : 590), al = SF(64);
    float tira[8] = { x0, y0, x0 + an, y0, x0 + an - SF(26), y0 + al, x0, y0 + al };
    if (activo) {
        float sombra[8];
        for (int k = 0; k < 8; k++) sombra[k] = tira[k] + SF(7);
        poligono(l, sombra, 4, OPACO(ROJO));
        poligono(l, tira, 4, OPACO(BLANCO));
    } else {
        poligono(l, tira, 4, OPACO(BLANCO));
        float in[8] = { x0 + SF(2), y0 + SF(2), x0 + an - SF(3), y0 + SF(2), x0 + an - SF(26) - SF(1), y0 + al - SF(2), x0 + SF(2), y0 + al - SF(2) };
        poligono(l, in, 4, OPACO(0x0B0B10u));
    }
    int xt = texto(l, (int)x0 + S(16), (int)y0 + S(30), S(22), ROJO, 255, INCL, "//");
    texto(l, xt + S(8), (int)y0 + S(30), S(22), activo ? NEGRO : BLANCO, 255, INCL, o->titulo);
    texto(l, (int)x0 + S(20), (int)y0 + S(52), S(14), activo ? ROJO : 0x94A3B8u, 255, 0, o->resumen);
}

static void dibujar_fila(int i) {
    struct lienzo *l = &l_fila[i];
    limpiar(l, 0);
    int activo = i == (nivel ? subsel : sel);
    if (nivel) fila_sub(l, i, activo); else fila_principal(l, i, activo);
    taek_presentar(s_fila[i].id, 0, 0, l->an, l->al);
}

static float fila_objetivo(int i) {
    return (float)(fila_x(i) - (i == (nivel ? subsel : sel) ? S(24) : 0));
}

/* -------------------------------------------------------------- inspector */

static int insp_an, insp_x;
#define INSP_AL S(680)
#define INSP_Y S(214)

static void cadena_bateria(struct txt *t) {
    t_ini(t);
    if (info.bateria_pct >= 0) {
        t_num(t, (uint64_t)info.bateria_pct); t_add(t, " %");
        if (info.bateria_estado & 2) t_add(t, ", cargando");
        else if (info.bateria_estado & 1) t_add(t, ", descargando");
        else if (info.cargador == 1) t_add(t, ", con cargador");
    } else t_add(t, "Sin dato");
}
static void cadena_ip(struct txt *t) {
    t_ini(t);
    if (!info.red_presente) t_add(t, "Sin controlador");
    else if (!info.red_enlace) t_add(t, "Sin cable");
    else if (info.red_ip_ok) { for (int i = 0; i < 4; i++) { if (i) t_add(t, "."); t_num(t, info.ip[i]); } t_add(t, ", "); t_num(t, info.red_mbps); t_add(t, " Mbps"); }
    else t_add(t, "Enlace, sin IP");
}
static void cadena_gpu(struct txt *t) {
    t_ini(t);
    if (info.gpu_proveedor_id) { t_add(t, info.gpu_proveedor); t_add(t, " "); t_hex4(t, info.gpu_proveedor_id); t_add(t, ":"); t_hex4(t, info.gpu_id); }
    else t_add(t, "No detectada");
}
static void cadena_cpu(struct txt *t, int maximo) {
    const char *cpu = info.cpu;
    while (*cpu == ' ') cpu++;
    t_ini(t);
    for (const char *p = cpu; *p && t->n < maximo; p++) { char c[2] = { *p, 0 }; t_add(t, c); }
}
/* El modelo corto del procesador: "i7-8650U" de "Intel(R) Core(TM) i7-8650U CPU @ 1.90GHz". */
static void cpu_corto(struct txt *t) {
    t_ini(t);
    for (const char *p = info.cpu; *p; p++)
        if ((p[0] == 'i' && p[1] >= '3' && p[1] <= '9' && p[2] == '-') || (p[0] == 'R' && p[1] == 'y' && p[2] == 'z')) {
            for (; *p && *p != ' ' && t->n < 20; p++) { char c[2] = { *p, 0 }; t_add(t, c); }
            return;
        }
    cadena_cpu(t, 14);
}
static int ram_pct(void) {
    uint64_t usada = info.ram_total > info.ram_libre ? info.ram_total - info.ram_libre : 0;
    return info.ram_total ? (int)(usada * 100 / info.ram_total) : 0;
}

static int fila_dato(struct lienzo *l, int x, int y, int ancho, const char *k, const char *v) {
    texto(l, x, y, S(18), GRIS, 255, 0, k);
    texto(l, x + S(170), y, S(18), BLANCO, 255, 0, v);
    rect(l, x, y + S(12), ancho, 1, premul(44, CIAN));
    return y + S(38);
}

/* Marco de la tarjeta: negra, filo rojo, una esquina cortada, insignia y titulo; visores rojos en las esquinas. */
static void marco_inspector(struct lienzo *l, const char *insignia, const char *titulo) {
    limpiar(l, 0);
    int an = l->an, al = l->al, corte = S(36), m = S(18);
    chaflan(l, m + S(10), m + S(10), an - 2 * m - S(10), al - 2 * m - S(10), corte, premul(110, ROJO));
    chaflan(l, m, m, an - 2 * m - S(10), al - 2 * m - S(10), corte, OPACO(ROJO));
    chaflan(l, m + S(3), m + S(3), an - 2 * m - S(16), al - 2 * m - S(16), corte - S(2), premul(244, 0x0A0B10u));
    esquinas(l, 0, 0, an, al, S(30), S(4), OPACO(ROJO));
    int w = texto_ancho(S(14), insignia) + S(28);
    para(l, m + S(22), m + S(20), w, S(26), S(8), OPACO(ROJO));
    texto(l, m + S(36), m + S(38), S(14), BLANCO, 255, 0, insignia);
    rect(l, an - m - S(90), m + S(22), S(24), S(24), OPACO(ROJO));
    rect(l, an - m - S(84), m + S(28), S(12), S(12), OPACO(NEGRO));
    titulo_rgb(l, m + S(22), m + S(92), S(28), BLANCO, S(2), titulo);
    rect(l, m + S(22), m + S(106), an - 2 * m - S(80), S(2), premul(150, ROJO));
}

static void baldosa(struct lienzo *l, int x, int y, int an, int al, const char *k, const char *v, const char *d, int pct, uint32_t tono) {
    rect(l, x, y, an, al, premul(220, 0x14161Eu));
    rect(l, x, y, S(3), al, OPACO(tono));
    rect(l, x, y, an, 1, premul(60, BLANCO));
    texto(l, x + S(14), y + S(22), S(14), CIAN, 255, 0, k);
    texto(l, x + S(14), y + S(54), S(26), NEON, 255, INCL, v);
    texto(l, x + S(14), y + S(75), S(13), GRIS, 255, 0, d);
    if (pct >= 0) {
        rect(l, x + S(14), y + al - S(8), an - S(28), S(4), premul(60, BLANCO));
        rect(l, x + S(14), y + al - S(8), (an - S(28)) * (pct > 100 ? 100 : pct) / 100, S(4), OPACO(tono));
    }
}

/* H150: el tablero general. Seis baldosas (las dos de abajo, de SYS_SENSORES) y el monton del nucleo; se redibuja
 * cada segundo mientras se ve. */
static void inspector_inicio(struct lienzo *l, int x, int y, int ancho) {
    struct txt v, d;
    int bw = (ancho - S(14)) / 2, bh = S(88);
    cpu_corto(&v);
    t_ini(&d); t_num(&d, info.cpus); t_add(&d, " hilos, "); t_num(&d, info.tsc_mhz); t_add(&d, " MHz");
    baldosa(l, x, y, bw, bh, "PROCESADOR", v.s, d.s, -1, CIAN);
    t_ini(&v);
    if (info.gpu_proveedor_id) { t_hex4(&v, info.gpu_proveedor_id); t_add(&v, ":"); t_hex4(&v, info.gpu_id); } else t_add(&v, "Ninguna");
    baldosa(l, x + bw + S(14), y, bw, bh, "GPU", v.s, info.gpu_video_probada ? "Video H.264 por GPU, probado" : "Video H.264 por software", -1, ROJO);
    uint64_t usada = info.ram_total > info.ram_libre ? info.ram_total - info.ram_libre : 0;
    t_ini(&v); t_num(&v, usada >> 20); t_add(&v, " MiB");
    t_ini(&d); t_add(&d, "en uso de "); t_num(&d, info.ram_total >> 20); t_add(&d, " MiB");
    baldosa(l, x, y + bh + S(14), bw, bh, "MEMORIA", v.s, d.s, ram_pct(), NEON);
    t_ini(&v);
    if (info.bateria_pct >= 0) { t_num(&v, (uint64_t)info.bateria_pct); t_add(&v, " %"); } else t_add(&v, "Sin dato");
    baldosa(l, x + bw + S(14), y + bh + S(14), bw, bh, "BATERÍA", v.s, (info.bateria_estado & 2) ? "Cargando" : (info.bateria_estado & 1) ? "Descargando" : info.cargador == 1 ? "Con cargador" : "Leída por ACPI",
            info.bateria_pct, info.bateria_pct >= 0 && info.bateria_pct < 20 ? ROJO : 0x2ECC71u);
    /* Fila 3: temperatura y red, en vivo. Sin SYS_SENSORES (nucleo anterior) dicen "sin dato". */
    int y3 = y + 2 * (bh + S(14));
    int c = sns.temp_paquete_ok ? sns.temp_paquete_c : sns.temp_nucleo_c;
    if (sen_hay() && (sns.temp_ok || sns.temp_paquete_ok)) {
        t_ini(&v); t_num(&v, (uint64_t)(c > 0 ? c : 0)); t_add(&v, " °C");
        t_ini(&d); t_add(&d, sns.temp_paquete_ok ? "paquete · TjMax " : "núcleo 0 · TjMax "); t_num(&d, (uint64_t)sns.tjmax_c); t_add(&d, " °C");
        baldosa(l, x, y3, bw, bh, "TEMPERATURA", v.s, d.s, sns.tjmax_c > 0 ? c * 100 / sns.tjmax_c : -1, tono_temp(c));
    } else baldosa(l, x, y3, bw, bh, "TEMPERATURA", "Sin dato", sen_hay() ? "El procesador no la informa" : "Núcleo anterior a H150", -1, GRIS);
    if (sen_hay() && sns.red_presente) {
        t_ini(&v); t_num(&v, (uint64_t)(sen_rx_s + 0.5f)); t_add(&v, " / "); t_num(&v, (uint64_t)(sen_tx_s + 0.5f));
        baldosa(l, x + bw + S(14), y3, bw, bh, "RED", v.s, sns.red_enlace ? "paquetes por segundo, RX / TX" : "Sin cable", -1, CIAN);
    } else baldosa(l, x + bw + S(14), y3, bw, bh, "RED", sen_hay() ? "Sin red" : "Sin dato", sen_hay() ? "No hay controlador" : "Núcleo anterior a H150", -1, GRIS);
    y = y3 + bh + S(34);
    /* El monton del nucleo: barra de uso y sus canarios */
    texto(l, x, y, S(14), NEON, 255, 0, "// MONTÓN DEL NÚCLEO:");
    struct txt t;
    t_ini(&t);
    if (sen_hay()) {
        t_num(&t, sns.heap_en_uso >> 20); t_add(&t, " de "); t_num(&t, sns.heap_capacidad >> 20); t_add(&t, " MiB · canarios ");
        t_add(&t, sns.heap_canarios_ok ? "0x7AEECC05 intactos" : "ROTOS");
    } else t_add(&t, "sin dato");
    texto(l, x + S(220), y, S(14), sen_hay() && !sns.heap_canarios_ok ? ROJO : BLANCO, 255, 0, t.s);
    rect(l, x, y + S(10), ancho, S(6), premul(60, BLANCO));
    if (sen_hay() && sns.heap_capacidad)
        rect(l, x, y + S(10), (int)((uint64_t)ancho * sns.heap_en_uso / sns.heap_capacidad), S(6), OPACO(sns.heap_canarios_ok ? 0x2ECC71u : ROJO));
    y += S(46);
    cadena_ip(&t); y = fila_dato(l, x, y, ancho, "Red", t.s);
    t_ini(&t); uint64_t m = info.activo_ms / 60000; t_num(&t, m / 60); t_add(&t, " h "); t_dos(&t, m % 60); t_add(&t, " min");
    fila_dato(l, x, y, ancho, "Encendido", t.s);
}

#include "paneles.h"   /* H151: el panel de cada opcion */

/* H151: tarjeta de ambiente del selector de ESCRITORIO, como en el prototipo */
static void tarjeta_ambiente(struct lienzo *l, int x, int y, int an, int al, int aero) {
    uint32_t borde = aero ? 0x38BDF8u : ROJO;
    if (aero) redondo_deg(l, x, y, an, al, S(14), premul(235, 0x0F3D5Cu), premul(235, 0x0B6E4Fu));
    else rect(l, x, y, an, al, premul(240, 0x0A0B10u));
    redondo_borde(l, x, y, an, al, aero ? S(14) : 0, OPACO(borde));
    int w = texto_ancho(S(12), aero ? "DISPONIBLE" : "ACTIVO") + S(18);
    rect(l, x + an - w - S(10), y + S(10), w, S(22), OPACO(aero ? 0x38BDF8u : ROJO));
    texto(l, x + an - w - S(1), y + S(26), S(12), aero ? NEGRO : BLANCO, 255, 0, aero ? "DISPONIBLE" : "ACTIVO");
    texto(l, x + S(16), y + S(44), S(26), aero ? BLANCO : NEON, 255, aero ? 0 : INCL, aero ? "AERO UI" : "NEURAL UI");
    texto(l, x + S(16), y + S(66), S(13), aero ? 0xBAE6FDu : GRIS, 255, 0, aero ? "Frutiger Aero (Windows Vista y 7)" : "Cyberpunk x Persona 3 Reload");
    parrafo(l, x + S(16), y + S(94), an - S(32), S(13), S(19), aero ? 0xE0F2FEu : 0xC8C8D4u, 255,
            aero ? "Cielo celeste, colinas, burbujas, barra de tareas traslúcida con reloj y ventanas que se arrastran y escalan."
                 : "Fondo obsidiana, HUD cinemático, telemetría real y el gato origami que se desarma.");
    if (!aero) texto(l, x + S(16), y + al - S(14), S(12), 0x2ECC71u, 255, 0, "// EN EJECUCIÓN");
    else texto(l, x + S(16), y + al - S(14), S(13), BLANCO, 255, 0, "[INTRO] ENTRAR AL ESCRITORIO >>");
}

static void dibujar_inspector(void) {
    struct lienzo *l = &l_insp;
    int m = S(18), x = m + S(26), y = m + S(146), ancho = l->an - 2 * m - S(80), y_max = l->al - m - S(34);
    uint32_t c = 0xE4E4EEu;
    const struct seccion *s = &secciones[sel];
    if (nivel) {
        const struct opcion *o = &s->sub[subsel];
        marco_inspector(l, s->etiqueta, o->titulo);
        if (!pan_dibujar(l, o, x, y, ancho, y_max) && o->detalle) {
            y = parrafo(l, x, y, ancho, S(22), S(34), c, 255, o->detalle);
            y += S(30);
            if (o->accion != A_NADA && o->accion != A_VOLVER) parrafo(l, x, y, ancho, S(18), S(28), NEON, 255, "[INTRO] PARA ABRIR");
        }
    } else {
        marco_inspector(l, s->etiqueta, s->orden);
        switch (sel) {
        case 0: inspector_inicio(l, x, y, ancho); break;
        case 1: {
            /* La matriz de aplicaciones del prototipo: las siete, con lo que son */
            for (int i = 0; i < secciones[1].nsub && y < y_max - S(40); i++) {
                const struct opcion *o = &secciones[1].sub[i];
                rect(l, x, y, ancho, S(54), premul(200, 0x14161Eu));
                rect(l, x, y, S(3), S(54), OPACO(i & 1 ? NEON : CIAN));
                texto(l, x + S(14), y + S(22), S(16), BLANCO, 255, INCL, o->titulo);
                texto(l, x + S(14), y + S(43), S(12), GRIS, 255, 0, o->resumen);
                texto(l, x + ancho - S(64), y + S(33), S(13), NEON, 255, 0, "RUN >>");
                y += S(62);
            }
            break;
        }
        case 2: {
            int al = (y_max - y - S(30)) / 2;
            tarjeta_ambiente(l, x, y, ancho, al, 0);
            tarjeta_ambiente(l, x, y + al + S(16), ancho, al, 1);
            break;
        }
        case 3: {
            struct txt t;
            t_ini(&t); t_num(&t, info.ancho); t_add(&t, " x "); t_num(&t, info.alto);
            y = fila_dato(l, x, y, ancho, "Pantalla", t.s);
            y = fila_dato(l, x, y, ancho, "Compositor", info.capa[0] ? info.capa : "CPU");
            t_ini(&t); t_add(&t, "SMEP "); t_add(&t, info.smep ? "sí" : "no"); t_add(&t, ", SMAP "); t_add(&t, info.smap ? "sí" : "no");
            y = fila_dato(l, x, y, ancho, "Protección", t.s);
            y = fila_dato(l, x, y, ancho, "Sensores", !sen_hay() ? "Núcleo anterior a H150" : sen_v3() ? "SYS_SENSORES v3" : sen_v2() ? "SYS_SENSORES v2" : "SYS_SENSORES v1");
            y += S(12);
            y = parrafo(l, x, y, ancho, S(15), S(23), 0xB8B8C4u, 255, "Primero el escaneo de Neko; después los ocho módulos MAGI: red, stream, audio, GPU, Ring 0, VFS, térmico y latencia. Cada uno abre su cabina a pantalla completa.");
            y += S(10);
            parrafo(l, x, y, ancho, S(13), S(20), 0x8E94A8u, 255, info.hito);
            break;
        }
        default:
            y = parrafo(l, x, y, ancho, S(22), S(34), c, 255, "Apagar, reiniciar hacia Ventoy o el harakiri del núcleo. Nada ocurre hasta confirmar con Intro.");
            break;
        }
        texto(l, x, l->al - m - S(30), S(14), NEON, 255, 0, s->pista);
    }
    taek_presentar(s_insp.id, 0, 0, l->an, l->al);
}
/* ------------------------------------------------------------------ barras */

/* Dia de la semana por el metodo de Sakamoto: 0 domingo. */
static int dia_semana(int a, int m, int d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) a--;
    return (a + a / 4 - a / 100 + a / 400 + t[m - 1] + d) % 7;
}

static void dibujar_crono(void) {
    static const char *const meses[] = { "ENE", "FEB", "MAR", "ABR", "MAY", "JUN", "JUL", "AGO", "SEP", "OCT", "NOV", "DIC" };
    static const char *const dias[] = { "DOM", "LUN", "MAR", "MIÉ", "JUE", "VIE", "SÁB" };
    struct lienzo *l = &l_crono;
    limpiar(l, 0);
    /* El atlas no baja de 14 px: en pantallas de menos de 1080 de alto el texto pesa mas y la tarjeta crece con el */
    struct txt dia_t;
    t_ini(&dia_t); t_add(&dia_t, "MIÉ 00 NOV  [MAÑANA]");
    int an = texto_ancho(S(14), dia_t.s) + S(24), al = S(90);
    if (an < S(214)) an = S(214);
    rect(l, S(6), S(6), an, al, OPACO(ROJO));
    rect(l, 0, 0, an, al, OPACO(0x111111u));
    rect(l, S(3), S(3), an - S(6), al - S(6), OPACO(BLANCO));
    para(l, S(8), S(8), texto_ancho(S(14), "CRONÓGRAFO") + S(16), S(18), S(5), OPACO(0x111111u));
    texto(l, S(14), S(22), S(14), BLANCO, 255, 0, "CRONÓGRAFO");
    struct txt t;
    if (info.fecha_ok) {
        t_ini(&t); t_num(&t, info.anio);
        texto(l, an - S(10) - texto_ancho(S(14), t.s), S(22), S(14), ROJO, 255, 0, t.s);
        t_ini(&t); t_dos(&t, info.hora); t_add(&t, ":"); t_dos(&t, info.minuto);
        int x = texto(l, S(10), S(60), S(36), 0x111111u, 255, INCL, t.s);
        t_ini(&t); t_add(&t, ":"); t_dos(&t, info.segundo);
        texto(l, x + S(4), S(60), S(18), ROJO, 255, 0, t.s);
        t_ini(&t); t_add(&t, dias[dia_semana(info.anio, info.mes, info.dia)]); t_add(&t, " "); t_dos(&t, info.dia); t_add(&t, " "); t_add(&t, meses[(info.mes + 11) % 12]);
        int h = info.hora;
        t_add(&t, h < 6 || h >= 20 ? "  [NOCHE]" : h < 12 ? "  [MAÑANA]" : "  [TARDE]");
        texto(l, S(10), S(80), S(14), 0x333333u, 255, 0, t.s);
    } else texto(l, S(10), S(60), S(22), 0x111111u, 255, INCL, "SIN RTC");
    /* Sello de la casa: hexagono rojo y el nombre */
    float cx = SF(30), cy = SF(124), r = SF(20), hexa[12];
    for (int i = 0; i < 6; i++) { float a = 1.0472f * (float)i - 1.5708f; hexa[2 * i] = cx + cosen(a) * r; hexa[2 * i + 1] = cy + sen(a) * r; }
    poligono(l, hexa, 6, OPACO(ROJO));
    for (int i = 0; i < 12; i++) hexa[i] = (i & 1) ? cy + (hexa[i] - cy) * 0.55f : cx + (hexa[i] - cx) * 0.55f;
    poligono(l, hexa, 6, OPACO(BLANCO));
    texto(l, S(58), S(126), S(22), 0x111111u, 255, INCL, "NEKOSAKA");
    texto(l, S(58), S(146), S(14), ROJO, 255, 0, "HECHO EN ESPAÑOL // SIN TUTORIALES GRINGOS");
    taek_presentar(s_crono.id, 0, 0, l->an, l->al);
}

/* Arriba a la derecha: tres tarjetas con lo que mas cambia. */
static void tarjeta_estado(struct lienzo *l, int y, uint32_t tono, const char *letra, const char *k, const char *v, int pct) {
    int an = l->an;
    rect(l, 0, y, an, S(60), premul(225, 0x0A0B10u));
    rect(l, 0, y, an, S(2), OPACO(tono));
    rect(l, 0, y + S(58), an, S(2), premul(120, tono));
    anillo(l, S(30), y + S(30), S(19), 0, OPACO(tono));
    anillo(l, S(30), y + S(30), S(15), 0, OPACO(0x0A0B10u));
    texto(l, S(30) - texto_ancho(S(18), letra) / 2, y + S(37), S(18), BLANCO, 255, 0, letra);
    texto(l, S(60), y + S(24), S(14), BLANCO, 255, 0, k);
    texto(l, an - S(10) - texto_ancho(S(14), v), y + S(24), S(14), NEON, 255, 0, v);
    if (pct >= 0) {
        rect(l, S(60), y + S(40), an - S(70), S(6), premul(50, BLANCO));
        rect(l, S(60), y + S(40), (an - S(70)) * (pct > 100 ? 100 : pct) / 100, S(6), OPACO(tono));
    }
}
static void dibujar_estado(void) {
    struct lienzo *l = &l_estado;
    limpiar(l, 0);
    struct txt t;
    t_ini(&t);
    if (info.bateria_pct >= 0) { t_num(&t, (uint64_t)info.bateria_pct); t_add(&t, (info.bateria_estado & 2) ? " % CARGA" : " %"); } else t_add(&t, "SIN DATO");
    tarjeta_estado(l, 0, ROJO, "B", "BATERÍA", t.s, info.bateria_pct);
    uint64_t usada = info.ram_total > info.ram_libre ? info.ram_total - info.ram_libre : 0;
    t_ini(&t); t_num(&t, usada >> 20); t_add(&t, " MiB");
    tarjeta_estado(l, S(70), 0x9B59FFu, "M", "MEMORIA", t.s, ram_pct());
    t_ini(&t);
    if (info.red_ip_ok) { for (int i = 0; i < 4; i++) { if (i) t_add(&t, "."); t_num(&t, info.ip[i]); } } else t_add(&t, info.red_presente ? "SIN IP" : "SIN RED");
    tarjeta_estado(l, S(140), CIAN, "R", "RED", t.s, -1);
    taek_presentar(s_estado.id, 0, 0, l->an, l->al);
}

static int pastilla(struct lienzo *l, int x, int y, const char *k, const char *v, int roja) {
    int w = texto_ancho(S(14), k) + texto_ancho(S(14), v) + S(30);
    rect(l, x, y - S(18), w, S(26), roja ? OPACO(ROJO) : premul(230, NEGRO));
    redondo_borde(l, x, y - S(18), w, S(26), 0, roja ? OPACO(ROJO) : premul(200, BLANCO));
    int xx = texto(l, x + S(10), y, S(14), roja ? BLANCO : NEON, 255, 0, k);
    texto(l, xx + S(10), y, S(14), BLANCO, 255, 0, v);
    return w;
}

#define PIE_AL S(176)
static void dibujar_pie(void) {
    struct lienzo *l = &l_pie;
    limpiar(l, 0);
    const struct seccion *s = &secciones[sel];
    if (estado == E_MENU || estado == E_ESCANEO) {
        /* Globo con la voz del sistema */
        int bx = S(350), by = S(6), bw = S(330), bh = S(98);
        rect(l, bx + S(7), by + S(7), bw, bh, premul(200, ROJO));
        rect(l, bx, by, bw, bh, OPACO(BLANCO));
        rect(l, bx + S(2), by + S(2), bw - S(4), bh - S(4), premul(240, NEGRO));
        float pico[6] = { (float)(bx + S(30)), (float)(by + bh - S(1)), (float)(bx + S(64)), (float)(by + bh - S(1)), (float)(bx + S(22)), (float)(by + bh + S(18)) };
        poligono(l, pico, 3, OPACO(BLANCO));
        texto(l, bx + S(14), by + S(24), S(14), NEON, 255, 0, s->etiqueta);
        char b[160];
        struct txt t; t_ini(&t); t_add(&t, "\""); t_add(&t, nivel ? s->sub[subsel].resumen : s->burbuja); t_add(&t, "\"");
        int n = 0; while (t.s[n] && n < 159) { b[n] = t.s[n]; n++; } b[n] = 0;
        parrafo(l, bx + S(14), by + S(48), bw - S(28), S(14), S(20), BLANCO, 255, b);
    }
    /* Orden en curso, sobre una franja oscura: abajo a la izquierda la cuchilla es clara */
    struct txt t;
    para(l, -S(20), PIE_AL - S(64), S(360), S(58), S(20), premul(225, 0x0A0B10u));
    rect(l, 0, PIE_AL - S(64), S(6), S(58), OPACO(ROJO));
    t_ini(&t); t_add(&t, "Subrutina: "); t_add(&t, nivel ? s->sub[subsel].titulo : s->titulo);
    texto(l, S(28), PIE_AL - S(40), S(18), NEON, 255, 0, t.s);
    t_ini(&t); t_add(&t, "Orden: "); t_add(&t, nivel ? s->sub[subsel].resumen : s->orden);
    texto(l, S(28), PIE_AL - S(16), S(14), GRIS, 255, 0, t.s);
    /* Atajos, de derecha a izquierda */
    static const char *const ks[] = { "[TAB]", "[ESC]", "[INTRO]", "[FLECHAS]" }, *const vs[] = { "ESCRITORIO", "VOLVER", "ENTRAR", "ELEGIR" };
    int x = W - S(24), y = PIE_AL - S(24);
    for (int i = 0; i < (estado == E_MENU || estado == E_ESCANEO ? 4 : 1); i++) {
        int k = estado == E_MENU || estado == E_ESCANEO ? i : 1;
        x -= texto_ancho(S(14), ks[k]) + texto_ancho(S(14), vs[k]) + S(30);
        pastilla(l, x, y, ks[k], vs[k], k == 2);
        x -= S(12);
    }
    const char *v = "TAEK OS v0.1.0 // EXPERIMENTAL // INTERFAZ V3";
    texto(l, W - texto_ancho(S(14), v) - S(24), PIE_AL - S(62), S(14), 0x6A6E7Cu, 255, 0, v);
    taek_presentar(s_pie.id, 0, 0, W, l->al);
}

/* ---------------------------------------------------------------- barrido */

static void preparar_barrido(void) {
    struct lienzo *l = &l_barre;
    limpiar(l, 0);
    int al = l->al, ext = l->an - W;
    para(l, 0, 0, S(80) + ext, al, ext, OPACO(BLANCO));
    para(l, S(50), 0, S(90) + ext, al, ext, OPACO(NEON));
    para(l, S(120), 0, S(170) + ext, al, ext, OPACO(ROJO));
    para(l, S(270), 0, W - S(290) + ext, al, ext, OPACO(NEGRO));
    para(l, W - S(30), 0, S(50) + ext, al, ext, OPACO(ROJO));
    int w = texto_ancho(S(240), "NEKOSAKA");
    texto(l, (l->an - w) / 2 + S(14), al / 2 + S(94), S(240), 0x000000u, 255, INCL, "NEKOSAKA");
    texto(l, (l->an - w) / 2, al / 2 + S(80), S(240), ROJO, 255, INCL, "NEKOSAKA");
    taek_presentar(s_barre.id, 0, 0, 1, 1);
}

/* Cubre la pantalla, cambia lo de debajo y sigue de largo, medido en tiempo real: da igual cuantos cuadros salgan. */
static long barrido_cuadros, barrido_ms;
static void barrido(void (*cambio)(void), const char *captura, int nc) {
    if (!hay_barrido) { if (cambio) cambio(); return; }
    taek_elevar(s_barre.id);
    long t0 = taek_ms(), dur = 300, inicio = t0, pausa = 0;
    int cambiado = 0, ext = l_barre.an - W;
    barrido_cuadros = 0;
    for (;;) {
        long t = taek_ms() - t0;
        if (t > dur) break;
        float f = (float)t / (float)dur, e = f < 0.5f ? 1.0f - (1.0f - 2.0f * f) * (1.0f - 2.0f * f) : 1.0f + (2.0f * f - 1.0f) * (2.0f * f - 1.0f);
        int x = W - (int)((float)(W + ext / 2) * e);
        if (!cambiado && t >= dur / 2) {
            if (cambio) cambio();
            cambiado = 1;
            if (captura) { long c0 = taek_ms(); taek_captura(captura, nc); pausa = taek_ms() - c0; t0 += pausa; }
        }
        taek_mover(s_barre.id, x, 0);
        if (x < W) barrido_cuadros++;
    }
    if (!cambiado && cambio) cambio();
    ocultar(&s_barre);
    barrido_ms = taek_ms() - inicio - pausa;
}

/* -------------------------------------------------------------- efectos */

/* Capa de efectos a pantalla completa: se borra y se presenta solo la caja de lo que se dibujo en este cuadro y el anterior. */
static int fx_activo, fxa_x0, fxa_y0, fxa_x1, fxa_y1, fxn_x0, fxn_y0, fxn_x1, fxn_y1;
static void fx_mostrar(int si) {
    if (si && !fx_activo) {
        fx_activo = 1;
        fxa_x0 = fxa_y0 = 0; fxa_x1 = fxa_y1 = 0;
        taek_elevar(s_fx.id);
        taek_mover(s_fx.id, 0, 0);
    } else if (!si && fx_activo) {
        fx_activo = 0;
        ocultar(&s_fx);
        for (int y = fxa_y0; y < fxa_y1; y++) memset(l_fx.px + (uint64_t)y * W + fxa_x0, 0, (size_t)(fxa_x1 - fxa_x0) * 4);
        fxa_x0 = fxa_y0 = fxa_x1 = fxa_y1 = 0;
    }
}
static void fx_empezar(void) {
    for (int y = fxa_y0; y < fxa_y1; y++) memset(l_fx.px + (uint64_t)y * W + fxa_x0, 0, (size_t)(fxa_x1 - fxa_x0) * 4);
    fxn_x0 = W; fxn_y0 = H; fxn_x1 = fxn_y1 = 0;
}
static void fx_marcar(float x0, float y0, float x1, float y1) {
    if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { float t = y0; y0 = y1; y1 = t; }
    int a = (int)x0 - 2, b = (int)y0 - 2, c = (int)x1 + 3, d = (int)y1 + 3;
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (c > W) c = W;
    if (d > H) d = H;
    if (a >= c || b >= d) return;
    if (a < fxn_x0) fxn_x0 = a;
    if (b < fxn_y0) fxn_y0 = b;
    if (c > fxn_x1) fxn_x1 = c;
    if (d > fxn_y1) fxn_y1 = d;
}
static void fx_terminar(void) {
    int x0 = fxn_x0, y0 = fxn_y0, x1 = fxn_x1, y1 = fxn_y1;
    if (fxa_x1 > fxa_x0) {
        if (x1 <= x0) { x0 = fxa_x0; y0 = fxa_y0; x1 = fxa_x1; y1 = fxa_y1; }
        else {
            if (fxa_x0 < x0) x0 = fxa_x0;
            if (fxa_y0 < y0) y0 = fxa_y0;
            if (fxa_x1 > x1) x1 = fxa_x1;
            if (fxa_y1 > y1) y1 = fxa_y1;
        }
    }
    if (x1 > x0 && y1 > y0) taek_presentar(s_fx.id, x0, y0, x1 - x0, y1 - y0);
    if (fxn_x1 > fxn_x0) { fxa_x0 = fxn_x0; fxa_y0 = fxn_y0; fxa_x1 = fxn_x1; fxa_y1 = fxn_y1; }
    else fxa_x0 = fxa_y0 = fxa_x1 = fxa_y1 = 0;
}
static void fx_trazo(float x0, float y0, float x1, float y1, float w, uint32_t c, int remate) {
    trazo(&l_fx, x0, y0, x1, y1, w, c, remate);
    fx_marcar((x0 < x1 ? x0 : x1) - w, (y0 < y1 ? y0 : y1) - w, (x0 > x1 ? x0 : x1) + w, (y0 > y1 ? y0 : y1) + w);
}
/* La caja se marca entera: una franja que cruza la pantalla puede tener todos sus vertices fuera de ella. */
static void fx_poligono(const float *p, int n, uint32_t c) {
    poligono(&l_fx, p, n, c);
    float x0 = p[0], y0 = p[1], x1 = p[0], y1 = p[1];
    for (int i = 1; i < n; i++) {
        if (p[2 * i] < x0) x0 = p[2 * i];
        if (p[2 * i] > x1) x1 = p[2 * i];
        if (p[2 * i + 1] < y0) y0 = p[2 * i + 1];
        if (p[2 * i + 1] > y1) y1 = p[2 * i + 1];
    }
    fx_marcar(x0, y0, x1, y1);
}

/* Capturas para `menu prueba` en mitad de un efecto: un nombre y el instante en que se toma. El tiempo de la captura no cuenta. */
static const char *cap_nombre[4];
static long cap_t[4];
static int cap_n;
static void capturar_en(const char *n, long t) { if (cap_n < 4) { cap_nombre[cap_n] = n; cap_t[cap_n++] = t; } }
static void capturas_pendientes(long t, long *t0) {
    for (int i = 0; i < cap_n; i++) {
        if (!cap_nombre[i] || t < cap_t[i]) continue;
        const char *n = cap_nombre[i];
        int k = 0; while (n[k]) k++;
        cap_nombre[i] = NULL;
        long c0 = taek_ms();
        taek_captura(n, k);
        *t0 += taek_ms() - c0;
    }
}
static void capturas_limpiar(void) { cap_n = 0; }

static void animar_menu(void);
/* Medida de los efectos: cuantos cuadros salen y el peor. */
static long ef_cuadros, ef_peor;
/* Un cuadro de efecto: espera lo que falte para 16 ms. */
static void ef_cuadro(long inicio) {
    long g = taek_ms() - inicio;
    ef_cuadros++;
    if (g > ef_peor) ef_peor = g;
    if (g < 16) taek_esperar(16 - g);
}

/* -- Sello NEKOSAKA: las piezas caen una por una, giradas, con sombra dura, sobre una franja blanca. */
static float sello_giro[LETRAS_PIEZAS];
static void sello_preparar(void) { for (int i = 0; i < LETRAS_PIEZAS; i++) sello_giro[i] = (i % 2 ? 1.0f : -1.0f) * (8.0f + azarf() * 10.0f) * 0.01745f; }

static void sello_pintar(long t, float ancho, float yc, long duracion) {
    if (t < 0 || t > duracion + 280) return;
    float vx = letras_vista[0], vy = letras_vista[1], vw = letras_vista[2], vh = letras_vista[3];
    float k = ancho / vw, ox = ((float)W - ancho) / 2, oy = yc - vh * k / 2;
    float salida = t > duracion ? limitar01((float)(t - duracion) / 260.0f) : 0;
    /* Franja blanca sesgada con sombra negra debajo */
    float fy = yc - vh * k * 0.75f, fh = vh * k * 1.5f, sesgo = fh * 0.364f;
    float ex = sale((float)t / 260.0f) * (1 + salida * 0.4f), fw = (float)W * 1.1f * ex, fx0 = -(float)W * 0.05f;
    uint32_t af = (uint32_t)((1 - salida) * 255.0f);
    if (af && fw > 1) {
        float sombra[8] = { fx0 + sesgo, fy + SF(10), fx0 + sesgo + fw, fy + SF(10), fx0 + fw, fy + fh + SF(10), fx0, fy + fh + SF(10) };
        fx_poligono(sombra, 4, premul(af, 0));
        float franja[8] = { fx0 + sesgo, fy, fx0 + sesgo + fw, fy, fx0 + fw, fy + fh, fx0, fy + fh };
        fx_poligono(franja, 4, premul(af, BLANCO));
    }
    /* Letras */
    static float pts[2 * 400];
    for (int pz = 0; pz < LETRAS_PIEZAS; pz++) {
        long tp = t - pz * 70;
        if (tp < 0) continue;
        float u = limitar01((float)tp / 360.0f), esc, giro, dy, op;
        float g = sello_giro[pz];
        if (u < 0.55f) { float e = sale(u / 0.55f); esc = mezclaf(2.6f, 0.92f, e); giro = mezclaf(g, -g / 4, e); dy = mezclaf(-60, 0, e); op = e; }
        else { float e = sale((u - 0.55f) / 0.45f); esc = mezclaf(0.92f, 1.0f, e); giro = mezclaf(-g / 4, 0, e); dy = 0; op = 1; }
        op *= 1 - salida;
        if (op <= 0.01f) continue;
        /* Centro de la pieza: caja de su contorno exterior */
        float cx0 = 1e9f, cy0 = 1e9f, cx1 = -1e9f, cy1 = -1e9f;
        for (int c = 0; c < (int)(sizeof(letras_contornos) / sizeof(letras_contornos[0])); c++) {
            if (letras_contornos[c].pieza != pz) continue;
            for (int j = 0; j < letras_contornos[c].n; j++) {
                float x = letras_pts[2 * (letras_contornos[c].ini + j)], y = letras_pts[2 * (letras_contornos[c].ini + j) + 1];
                if (x < cx0) cx0 = x;
                if (x > cx1) cx1 = x;
                if (y < cy0) cy0 = y;
                if (y > cy1) cy1 = y;
            }
            break;
        }
        float pcx = (cx0 + cx1) / 2, pcy = (cy0 + cy1) / 2, co = cosen(giro), si = sen(giro);
        for (int capa = 0; capa < 2; capa++) {
            rell_ini();
            float desp = capa == 0 ? 14 : 0;
            float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
            for (int c = 0; c < (int)(sizeof(letras_contornos) / sizeof(letras_contornos[0])); c++) {
                if (letras_contornos[c].pieza != pz) continue;
                int n = letras_contornos[c].n;
                for (int j = 0; j < n && j < 400; j++) {
                    float x = letras_pts[2 * (letras_contornos[c].ini + j)] - pcx, y = letras_pts[2 * (letras_contornos[c].ini + j) + 1] - pcy;
                    float rx = (x * co - y * si) * esc + pcx + desp, ry = (x * si + y * co) * esc + pcy + desp + dy;
                    float sx = ox + (rx - vx) * k, sy = oy + (ry - vy) * k;
                    /* Al salir: se corre a la derecha y se inclina */
                    sx += salida * SF(140) - (sy - yc) * salida * 0.32f;
                    pts[2 * j] = sx; pts[2 * j + 1] = sy;
                    if (sx < bx0) bx0 = sx;
                    if (sx > bx1) bx1 = sx;
                    if (sy < by0) by0 = sy;
                    if (sy > by1) by1 = sy;
                }
                rell_contorno(pts, n < 400 ? n : 400);
            }
            rell_pintar(&l_fx, capa == 0 ? premul((uint32_t)(op * 255.0f), 0) : premul((uint32_t)(op * 255.0f), ROJO), 1, NULL);
            fx_marcar(bx0, by0, bx1, by1);
        }
    }
}

/* -- Lineas que vuelan. Cada una gira y se curva de su sitio al de destino; mismo numero de lineas en las dos formas. */
#define MAXSEG 160
static struct seg v_seg[MAXSEG], v_dest[MAXSEG];
struct vuelo_dato { float aa, ab, d, cax, cay, cbx, cby, la, lb, vx, vy; };
static struct vuelo_dato v_dat[MAXSEG];
static int v_n;
static float v_dur;

static void vuelo_preparar(const struct seg *hasta, int n, float ms, float retraso, float remolino) {
    for (int i = 0; i < v_n; i++) {
        const struct seg *a = &v_seg[i], *b = &hasta[i % n];
        struct vuelo_dato *d = &v_dat[i];
        v_dest[i] = *b;
        d->aa = arco_tan2(a->y2 - a->y1, a->x2 - a->x1);
        float ab = arco_tan2(b->y2 - b->y1, b->x2 - b->x1);
        while (ab - d->aa > 3.14159265f) ab -= 6.2831853f;
        while (ab - d->aa < -3.14159265f) ab += 6.2831853f;
        /* Media vuelta o vuelta entera: una linea girada 180 grados es la misma */
        ab += (azarf() < 0.5f ? -1.0f : 1.0f) * 3.14159265f * (azarf() < 0.4f ? 2.0f : 1.0f);
        d->ab = ab;
        float ang = azarf() * 6.2831853f, mag = SF(remolino) * (0.35f + azarf() * 0.65f);
        d->d = azarf() * retraso;
        d->cax = (a->x1 + a->x2) / 2; d->cay = (a->y1 + a->y2) / 2;
        d->cbx = (b->x1 + b->x2) / 2; d->cby = (b->y1 + b->y2) / 2;
        float ax = a->x2 - a->x1, ay = a->y2 - a->y1, bx = b->x2 - b->x1, by = b->y2 - b->y1;
        d->la = raizf(ax * ax + ay * ay) / 2; d->lb = raizf(bx * bx + by * by) / 2;
        d->vx = cosen(ang) * mag; d->vy = sen(ang) * mag;
    }
    v_dur = ms - retraso;
}

/* Dibuja el vuelo en el instante t; brillo: un resplandor rojo debajo (el destello al formar la palabra). */
static int vuelo_pintar(long t, float brillo, float opacidad) {
    int vivos = 0;
    for (int i = 0; i < v_n; i++) {
        const struct vuelo_dato *d = &v_dat[i];
        const struct seg *a = &v_seg[i], *b = &v_dest[i];
        float u = limitar01(((float)t - d->d) / v_dur);
        if (u < 1) vivos = 1;
        float e = suave(u), arco = sen(3.14159265f * e);
        float cx = mezclaf(d->cax, d->cbx, e) + d->vx * arco, cy = mezclaf(d->cay, d->cby, e) + d->vy * arco;
        float L = mezclaf(d->la, d->lb, e), A = mezclaf(d->aa, d->ab, e);
        float dx = cosen(A) * L, dy = sen(A) * L;
        uint32_t col = entre(OPACO(a->c), OPACO(b->c), (uint32_t)(e * 255.0f));
        float w = mezclaf(a->w, b->w, e), o = mezclaf(a->o, b->o, e) * opacidad;
        if (brillo > 0 && w < SF(12)) fx_trazo(cx - dx, cy - dy, cx + dx, cy + dy, w + SF(10), premul((uint32_t)(brillo * 110.0f * opacidad), ROJO), 1);
        fx_trazo(cx - dx, cy - dy, cx + dx, cy + dy, w, cubrir(col, (uint32_t)(o * 255.0f)), w <= SF(12));
    }
    return vivos;
}
static void vuelo_fijar(void) { for (int i = 0; i < v_n; i++) v_seg[i] = v_dest[i]; }

/* Formas: el gato tal como esta en pantalla, la palabra NEKOSAKA y el escritorio Aero. */
static int nfinos;
static struct seg f_tmp[MAXSEG];

/* El logo se dibuja en la capa con su posicion de reposo; en pantalla solo cambia lo que flota (heroe_dy). */
static int segs_gato(struct seg *s) {
    float bob = (float)heroe_dy, k = logo_k();
    int n = 0;
    for (int i = 0; i < NFACETAS; i++) {
        const struct logo_faceta *f = &logo_facetas[i];
        float dx, dy;
        faceta_desplazamiento(i, pose.sep, &dx, &dy);
        for (int j = 0; j < f->n && n < MAXSEG - 6; j++) {
            int a = f->ini + j, b = f->ini + (j + 1) % f->n;
            float x1, y1, x2, y2;
            logo_punto(logo_faceta_pts[2 * a], logo_faceta_pts[2 * a + 1], &x1, &y1);
            logo_punto(logo_faceta_pts[2 * b], logo_faceta_pts[2 * b + 1], &x2, &y2);
            s[n++] = (struct seg){ x1 + dx, y1 + dy + bob, x2 + dx, y2 + dy + bob, 7 * k, 1, 0xFF003Cu };
        }
    }
    ordenar_segs(s, n, 0);
    nfinos = n;
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        float x1, y1, x2, y2;
        logo_punto(logo_hex[2 * i], logo_hex[2 * i + 1], &x1, &y1);
        logo_punto(logo_hex[2 * j], logo_hex[2 * j + 1], &x2, &y2);
        s[n++] = (struct seg){ x1, y1 + bob, x2, y2 + bob, LOGO_HEX_ANCHO * k, 1, 0xE0002Fu };
    }
    return n;
}

static float palabra_pts[2 * 800];
static int segs_palabra(struct seg *s, int n, float ancho, float yc) {
    float vx = letras_vista[0], vy = letras_vista[1], vw = letras_vista[2], vh = letras_vista[3];
    float k = ancho / vw, ox = ((float)W - vw * k) / 2, oy = yc - vh * k / 2;
    struct forma fm[16];
    int nf = 0, usados = 0, pieza_ant = -1;
    for (int c = 0; c < (int)(sizeof(letras_contornos) / sizeof(letras_contornos[0])) && nf < 16; c++) {
        const struct logo_contorno *lc = &letras_contornos[c];
        float *p = palabra_pts + usados;
        for (int j = 0; j < lc->n; j++) {
            p[2 * j] = ox + ((float)letras_pts[2 * (lc->ini + j)] - vx) * k;
            p[2 * j + 1] = oy + ((float)letras_pts[2 * (lc->ini + j) + 1] - vy) * k;
        }
        usados += 2 * lc->n;
        fm[nf++] = (struct forma){ p, lc->n, 1, lc->pieza != pieza_ant ? 4 : 3, perimetro(p, lc->n, 1), SF(4), 1, 0xFF1A40u };
        pieza_ant = lc->pieza;
    }
    int m = repartir(s, fm, nf, n);
    ordenar_segs(s, m, 1);
    /* Seis losas: subrayado amarillo y sobrerrayado blanco, al estilo Persona */
    float x0 = ox, x1 = ox + vw * k, arriba = yc - vh * k / 2 - SF(26), abajo = yc + vh * k / 2 + SF(26);
    float a0 = x0 - SF(60), a1 = x1 + SF(60), tA = (a1 - a0) / 3, b0 = x0 - SF(90), b1 = x1 + SF(90), tB = (b1 - b0) / 3;
    for (int i = 0; i < 3; i++) {
        s[m++] = (struct seg){ a0 + tA * (float)i + (i ? SF(10) : 0), arriba, a0 + tA * (float)(i + 1) - (i < 2 ? SF(10) : 0), arriba, SF(14), 1, 0xFFFFFFu };
        s[m++] = (struct seg){ b0 + tB * (float)i + (i ? SF(10) : 0), abajo, b0 + tB * (float)(i + 1) - (i < 2 ? SF(10) : 0), abajo, SF(22), 1, 0xFCEE0Au };
    }
    return m;
}

static void circulo_pts(float *p, int n, float cx, float cy, float r) {
    for (int i = 0; i < n; i++) { float a = 6.2831853f * (float)i / (float)n; p[2 * i] = cx + cosen(a) * r; p[2 * i + 1] = cy + sen(a) * r; }
}

static int segs_aero(struct seg *s, int n);  /* En escritorio.h: conoce la geometria del escritorio */

/* Vuelo bloqueante con su medida: mientras vuela, las filas y la tarjeta siguen sus resortes. */
static void (*vuelo_cada_cuadro)(long t);
static void volar(float ms, float retraso, float remolino, const struct seg *hasta, int n, float brillo, long *reloj) {
    vuelo_preparar(hasta, n, ms, retraso, remolino);
    long t0 = taek_ms();
    for (;;) {
        long ini = taek_ms(), t = ini - t0;
        capturas_pendientes(*reloj + t, &t0);
        t = taek_ms() - t0;
        fx_empezar();
        int vivo = vuelo_pintar(t, brillo, 1);
        fx_terminar();
        if (vuelo_cada_cuadro) vuelo_cada_cuadro(*reloj + t);
        ef_cuadro(ini);
        if (!vivo) break;
    }
    *reloj += taek_ms() - t0;
    vuelo_fijar();
}

/* Pausa con la palabra encendida mientras cae el sello encima. */
static void destello_y_sello(long ms, long dur_sello, long *reloj) {
    sello_preparar();
    long t0 = taek_ms();
    for (;;) {
        long ini = taek_ms(), t = ini - t0;
        capturas_pendientes(*reloj + t, &t0);
        t = taek_ms() - t0;
        if (t > ms) break;
        fx_empezar();
        float b = 1.0f - (float)t / (float)ms * 0.5f;
        /* Como en la v3: el sello cae debajo y las lineas encendidas quedan encima */
        sello_pintar(t, (float)W * 0.62f, (float)H * 0.46f, dur_sello);
        vuelo_pintar(1 << 20, b, 1);
        fx_terminar();
        if (vuelo_cada_cuadro) vuelo_cada_cuadro(*reloj + t);
        ef_cuadro(ini);
    }
    *reloj += taek_ms() - t0;
}

static void desvanecer(long ms, long *reloj) {
    long t0 = taek_ms();
    for (;;) {
        long ini = taek_ms(), t = ini - t0;
        capturas_pendientes(*reloj + t, &t0);
        t = taek_ms() - t0;
        if (t > ms) break;
        fx_empezar();
        vuelo_pintar(1 << 20, 0, 1.0f - (float)t / (float)ms);
        fx_terminar();
        ef_cuadro(ini);
    }
    fx_empezar();
    fx_terminar();
    *reloj += taek_ms() - t0;
}

/* ---------------------------------------------------------- menu: animacion */

enum { ENTRA_IZQ, ENTRA_CASCADA, ENTRA_RAYO, ENTRA_FALLA };
static void entrar_filas(int modo) {
    int n = opciones();
    long ahora = taek_ms();
    desp = sub_largo() && subsel >= FILAS_VISIBLES ? subsel - FILAS_VISIBLES + 1 : 0;
    for (int i = 0; i < NFILAS; i++) {
        struct fila *f = &fila[i];
        ocultar(&s_fila[i]);
        f->m.dibujado = OCULTO;
        f->oculta = 0; f->retraso = 0; f->falla = 0;
        if (i >= n) { f->oculta = 1; continue; }
        dibujar_fila(i);
        f->m.vel = 0;
        f->m.obj = fila_objetivo(i);
        switch (modo) {
        case ENTRA_IZQ: f->m.pos = (float)(-S(1000) - i * S(220)); break;
        case ENTRA_CASCADA: f->m.pos = f->m.obj + (float)S(220); f->retraso = i * 4; break;
        case ENTRA_RAYO: f->m.pos = f->m.obj - (float)S(60); f->oculta = 1; break;
        default: f->m.pos = f->m.obj; f->falla = 1; f->falla_t0 = ahora + i * 70; break;
        }
    }
}

/* Lo que vive cuando no pasa nada: filas con resorte, el gato que flota y un tiron de la tarjeta de vez en cuando. */
static void sonido_bombear(void);
static void animar_menu(void) {
    cuadro++;
    sonido_bombear();
    int n = opciones();
    long ahora = taek_ms();
    for (int i = 0; i < n && !filas_manuales; i++) {
        struct fila *f = &fila[i];
        struct muelle *m = &f->m;
        if (f->oculta) continue;
        if (f->retraso > 0) { f->retraso--; continue; }
        int x;
        if (f->falla) {
            static const int16_t salto[6] = { 40, -18, 12, -6, 3, 0 };
            long g = ahora - f->falla_t0;
            if (g < 0) continue;
            int paso = (int)(g * 6 / 500);
            if (paso >= 6) { f->falla = 0; x = (int)m->obj; m->pos = m->obj; m->vel = 0; }
            else x = (int)m->obj + S(salto[paso]);
            if (x != m->dibujado) { taek_mover(s_fila[i].id, x, fila_y(i) + (paso == 3 ? S(6) : 0)); m->dibujado = x; }
            continue;
        }
        if (muelle_quieto(m) && m->dibujado == (int)m->obj) continue;
        muelle_paso(m, 0.16f, 0.68f);
        x = (int)m->pos;
        if (muelle_quieto(m)) { x = (int)m->obj; m->pos = m->obj; m->vel = 0; }
        if (x != m->dibujado) { taek_mover(s_fila[i].id, x, fila_y(i)); m->dibujado = x; }
    }
    muelle_paso(&m_insp, 0.14f, 0.70f);
    int xi = (int)m_insp.pos;
    if (xi != m_insp.dibujado) { taek_mover(s_insp.id, xi, INSP_Y); m_insp.dibujado = xi; }
    /* El gato: se separa o se rearma hacia sep_obj, y flota */
    if (pose.sep != sep_obj) {
        float d = sep_obj - pose.sep;
        pose.sep = absf(d) < 0.5f ? sep_obj : pose.sep + d * 0.25f;
        if (heroe_visible) pintar_heroe(); else heroe_sucio = 1;
    }
    muelle_paso(&m_gato, 0.09f, 0.80f);
    mover_heroe(0);
    for (int i = 0; i < NESQ; i++) {
        struct esquirla *e = &esq[i];
        if (!e->vida) continue;
        if (--e->vida == 0) { ocultar(&s_esq[i]); continue; }
        e->x += e->vx; e->y += e->vy; e->vy += 0.9f; e->vx *= 0.97f;
        taek_mover(s_esq[i].id, (int)e->x, (int)e->y);
    }
    if (cuadro >= proximo_glitch) {
        m_insp.vel += (float)((int)(azar() % 13) - 6);
        m_gato.vel += (float)((int)(azar() % 9) - 4);
        proximo_glitch = cuadro + 120 + (long)(azar() % 240);
    }
}

static void golpe_de_seleccion(void) {
    int a = nivel ? subsel : sel;
    float cx = (float)(fila_x(a) + S(420)), cy = (float)(fila_y(a) + S(40));
    for (int i = 0; i < NESQ; i++) {
        struct esquirla *e = &esq[i];
        e->x = cx + (float)(azar() % (unsigned)(S(200) + 1));
        e->y = cy;
        e->vx = (float)((int)(azar() % 44) - 8) * (float)S(1);
        e->vy = -(float)(azar() % 16 + 4) * (float)S(1);
        e->vida = 14 + (int)(azar() % 10);
        taek_elevar(s_esq[i].id);
    }
    m_gato.vel -= (float)S(8);
    m_insp.pos = m_insp.obj + (float)S(40);
    m_insp.vel = 0;
}

/* H151: si la elegida sale de la ventana de 7, la lista se corre y cada fila va a su nuevo sitio */
static void reubicar_filas(void) {
    int n = opciones();
    for (int i = 0; i < n; i++) {
        fila[i].m.obj = fila_objetivo(i);
        if (fila[i].oculta) continue;
        int x = fila[i].m.dibujado == OCULTO ? (int)fila[i].m.obj : fila[i].m.dibujado;
        taek_mover(s_fila[i].id, x, fila_y(i));
    }
}
static void asegurar_visible(int i) {
    if (!sub_largo()) { desp = 0; return; }
    int d = desp;
    if (i < d) d = i;
    if (i >= d + FILAS_VISIBLES) d = i - FILAS_VISIBLES + 1;
    if (d != desp) { desp = d; reubicar_filas(); }
}

static void sonar(int e);
static void cambiar_seleccion(int nueva, int *cur) {
    int vieja = *cur;
    if (nueva == vieja) return;
    *cur = nueva;
    panel_foco = 0; confirmar = 0;
    if (nivel) asegurar_visible(nueva);
    dibujar_fila(vieja);
    dibujar_fila(nueva);
    fila[vieja].m.obj = fila_objetivo(vieja);
    fila[nueva].m.obj = fila_objetivo(nueva);
    fila[nueva].m.vel = -(float)S(16);
    dibujar_inspector();
    dibujar_pie();
    golpe_de_seleccion();
    sonar(0);   /* SND_TICK */
}

static void pintar_nivel_modo(int modo) {
    entrar_filas(modo);
    dibujar_inspector();
    dibujar_pie();
    m_insp.pos = m_insp.obj + (float)S(200);
    m_insp.vel = 0;
}
static void pintar_nivel(void) { pintar_nivel_modo(ENTRA_IZQ); }

static void mostrar_capas_menu(void) {
    taek_mover(s_insp.id, (int)m_insp.pos, INSP_Y);
    taek_mover(s_crono.id, S(24), S(20));
    taek_mover(s_estado.id, W - l_estado.an - S(24), S(20));
    taek_mover(s_pie.id, 0, H - PIE_AL);
}
static void ocultar_capas_menu(void) {
    ocultar(&s_insp); heroe_mostrar(0); ocultar(&s_scan);
    for (int i = 0; i < NFILAS; i++) { ocultar(&s_fila[i]); fila[i].m.dibujado = OCULTO; }
    for (int i = 0; i < NESQ; i++) { ocultar(&s_esq[i]); esq[i].vida = 0; }
}

/* Filas y tarjeta que huyen: unas en diagonal a la izquierda, la tarjeta a la derecha. */
static void huida(long t) {
    for (int i = 0; i < opciones(); i++) {
        float u = limitar01((float)(t - i * 40) / 450.0f), e = u * u * u;
        if (u <= 0) continue;
        int x = u >= 1 ? OCULTO : (int)fila[i].m.obj - (int)((float)W * 1.2f * e);
        if (x != fila[i].m.dibujado) { taek_mover(s_fila[i].id, x, fila_y(i)); fila[i].m.dibujado = x; }
    }
    float u = limitar01((float)t / 450.0f);
    int x = u >= 1 ? OCULTO : (int)m_insp.obj + (int)((float)W * 0.7f * u * u);
    if (x != m_insp.dibujado) { taek_mover(s_insp.id, x, INSP_Y); m_insp.dibujado = x; }
}

static void al_volver_menu(void);
static void salir_escritorio(void);
static void entrar_escritorio(void);
#include "sonido.h"   /* H151: los sonidos del prototipo (mira vm_sonando de ventanas.h) */
#include "pantallas.h"
#include "escritorio.h"

static void al_volver_menu(void) {
    esc_ocultar();
    ocultar(&s_pan);
    fx_mostrar(0);
    estado = E_MENU;
    mostrar_capas_menu();
    heroe_mostrar(1);
    pintar_nivel();
}

/* ------------------------------------------------------------ transiciones */

/* Bucle de un efecto bloqueante: el menu sigue animado debajo. paso(t) dibuja en la capa de efectos y dice si sigue. */
static long efecto(int (*paso)(long t), long *reloj_ext) {
    long t0 = taek_ms(), reloj = reloj_ext ? *reloj_ext : 0;
    ef_cuadros = 0; ef_peor = 0;
    for (;;) {
        long ini = taek_ms(), t = ini - t0;
        capturas_pendientes(reloj + t, &t0);
        t = taek_ms() - t0;
        fx_empezar();
        int sigue = paso(t);
        fx_terminar();
        animar_menu();
        ef_cuadro(ini);
        if (!sigue) break;
    }
    long dur = taek_ms() - t0;
    if (reloj_ext) *reloj_ext += dur;
    capturas_limpiar();
    return dur;
}

static int orden_onda[NFACETAS];
static void preparar_onda(void) {
    for (int i = 0; i < NFACETAS; i++) orden_onda[i] = i;
    for (int i = 1; i < NFACETAS; i++) {
        int v = orden_onda[i], j = i;
        while (j > 0 && logo_facetas[orden_onda[j - 1]].cy > logo_facetas[v].cy) { orden_onda[j] = orden_onda[j - 1]; j--; }
        orden_onda[j] = v;
    }
}

/* INICIO: rayos que giran, tres ondas hexagonales, una ola blanca por las facetas y el sello. */
static int inicio_abierto_sub;
static int paso_inicio(long t) {
    float ex, ey;
    logo_punto(LOGO_CENTRO_X, LOGO_CENTRO_Y, &ex, &ey);
    ey += (float)heroe_dy;
    if (t < 700) {
        float u = (float)t / 700.0f, e = sale(u), giro = 0.4887f * e, esc = 0.4f + 0.7f * e, R = SF(980) * esc, r0 = SF(50) * esc;
        uint32_t a = (uint32_t)(0.6f * (1 - u) * 255.0f);
        for (int i = 0; i < 12; i++) {
            float b = (float)i * 0.5236f + giro;
            float tri[6] = { ex + cosen(b - 0.07f) * r0, ey + sen(b - 0.07f) * r0, ex + cosen(b + 0.07f) * r0, ey + sen(b + 0.07f) * r0, ex + cosen(b) * R, ey + sen(b) * R };
            fx_poligono(tri, 3, premul(a, i & 1 ? NEGRO : ROJO));
        }
    }
    static const uint32_t tonos[3] = { BLANCO, ROJO, NEON };
    for (int i = 0; i < 3; i++) {
        long ti = t - i * 110;
        if (ti < 0 || ti > 900) continue;
        float u = (float)ti / 900.0f, e = sale(u), esc = 1 + (1.4f + 0.4f * (float)i) * e, giro = (float)(i - 1) * 0.1396f * e;
        float p[12], co = cosen(giro), si = sen(giro);
        for (int k = 0; k < 6; k++) {
            float x, y;
            logo_punto(logo_hex[2 * k], logo_hex[2 * k + 1], &x, &y);
            x -= ex; y += (float)heroe_dy - ey;
            p[2 * k] = ex + (x * co - y * si) * esc; p[2 * k + 1] = ey + (x * si + y * co) * esc;
        }
        uint32_t a = (uint32_t)((1 - u) * 255.0f);
        for (int k = 0; k < 6; k++) { int j = (k + 1) % 6; fx_trazo(p[2 * k], p[2 * k + 1], p[2 * j], p[2 * j + 1], SF(9) * esc * 0.6f, premul(a, tonos[i]), 1); }
    }
    /* La ola de luz: de la cabeza a la cola */
    int cambio = 0;
    for (int k = 0; k < NFACETAS; k++) {
        long tk = t - k * 35;
        uint8_t b = tk < 0 ? 0 : tk >= 550 ? 0 : (uint8_t)((1.0f - sale((float)tk / 550.0f)) * 255.0f);
        if (tk < 0) b = 0;
        if (pose.blanco[orden_onda[k]] != b) { pose.blanco[orden_onda[k]] = b; cambio = 1; }
    }
    if (cambio) pintar_heroe();
    sello_pintar(t, (float)W * 0.46f, (float)H * 0.5f, 900);
    if (t >= 260 && !inicio_abierto_sub) { inicio_abierto_sub = 1; nivel = 1; subsel = 0; pintar_nivel_modo(ENTRA_CASCADA); }
    return t < 1180;
}

/* APLICACIONES: el gato se separa y cada faceta dispara un rayo hacia una opcion, que se enciende al llegar. */
static const int faceta_rayo[] = { 11, 7, 4, 3, 5, 6, 0, 9, 10, 8 };
static int apps_abierto;
static int paso_apps(long t) {
    if (t >= 320 && !apps_abierto) { apps_abierto = 1; nivel = 1; subsel = 0; pintar_nivel_modo(ENTRA_RAYO); }
    if (!apps_abierto) return 1;
    int n = opciones(), vivos = 0;
    for (int i = 0; i < n; i++) {
        long ti = t - 320 - 120 - i * 85;
        if (ti < 0) { vivos = 1; continue; }
        float ax, ay;
        faceta_centro(faceta_rayo[i], pose.sep, 0, (float)heroe_dy, &ax, &ay);
        float bx = fila[i].m.obj + SF(60), by = (float)(fila_y(i) + S(50));
        float u = limitar01((float)ti / 240.0f), e = u < 1 ? 1 - (1 - u) * (1 - u) : 1;
        if (u >= 1 && fila[i].oculta) { fila[i].oculta = 0; fila[i].m.vel = (float)S(20); }
        long tf = ti - 240 - 120;
        float o = tf < 0 ? 1 : 1 - limitar01((float)tf / 350.0f);
        if (o <= 0) continue;
        vivos = 1;
        uint32_t c = i & 1 ? NEON : ROJO;
        fx_trazo(ax, ay, mezclaf(ax, bx, e), mezclaf(ay, by, e), SF(8), premul((uint32_t)(70.0f * o), c), 1);
        fx_trazo(ax, ay, mezclaf(ax, bx, e), mezclaf(ay, by, e), SF(3), premul((uint32_t)(255.0f * o), c), 1);
        if (u >= 1) anillo(&l_fx, (int)bx, (int)by, S(9), 0, premul((uint32_t)(255.0f * o), BLANCO));
    }
    return vivos;
}

/* APAGAR: la escena se sacude y queda una linea plana sobre el gato. */
static int apagar_abierto, apagar_paso;
static void dibujar_linea_plana(long t) {
    float x0, y0, x1, y1;
    logo_caja(&x0, &y0, &x1, &y1);
    float yc = (y0 + y1) / 2 + (float)heroe_dy;
    /* Parpadea al aparecer, como una senal que se pierde */
    if (t < 420 && ((t / 60) & 1)) return;
    fx_trazo(x0 - SF(60), yc, x1 + SF(60), yc, SF(16), premul(70, ROJO), 0);
    fx_trazo(x0 - SF(60), yc, x1 + SF(60), yc, SF(5), OPACO(ROJO), 0);
    fx_trazo(x0 - SF(60), yc, x1 + SF(60), yc, SF(1.5f), OPACO(BLANCO), 0);
    const char *s = "/// DESCONEXIÓN ACPI SOLICITADA ///";
    int w = texto_ancho(S(14), s) + S(30), x = (int)((x0 + x1) / 2) - w / 2, y = (int)yc + S(40);
    peligro(&l_fx, x, y, w, S(30), S(10), OPACO(NEON), OPACO(NEGRO));
    rect(&l_fx, x + S(8), y + S(5), w - S(16), S(20), OPACO(NEON));
    texto(&l_fx, x + S(15), y + S(21), S(14), NEGRO, 255, 0, s);
    fx_marcar((float)x, (float)y, (float)(x + w), (float)(y + S(30)));
}
static int paso_apagar(long t) {
    static const int16_t sx[6] = { 0, -14, 10, -6, 4, 0 }, sy[6] = { 0, 6, -8, 4, -2, 0 };
    int k = t < 380 ? (int)(t * 6 / 380) : 5;
    if (k != apagar_paso) {
        apagar_paso = k;
        heroe_manual = k < 5;
        taek_mover(s_fondo.id, S(sx[k]), S(sy[k]));
        if (heroe_visible) taek_mover(s_heroe.id, hx + S(sx[k]), hy + heroe_dy + S(sy[k]));
    }
    if (t >= 220 && !apagar_abierto) { apagar_abierto = 1; nivel = 1; subsel = 0; pintar_nivel_modo(ENTRA_FALLA); }
    dibujar_linea_plana(t);
    return t < 900;
}

/* Al salir de un submenu: las filas se van a la derecha, el gato se rearma y vuelve el menu. */
static int paso_salida(long t) {
    for (int i = 0; i < opciones(); i++) {
        float u = limitar01((float)(t - i * 25) / 260.0f);
        int x = u >= 1 ? OCULTO : (int)fila[i].m.obj + (int)(SF(260) * u * u);
        if (x != fila[i].m.dibujado) { taek_mover(s_fila[i].id, x, fila_y(i)); fila[i].m.dibujado = x; }
    }
    return t < 280;
}

static void entrar_seccion(int a) {
    long reloj = 0;
    static const int efecto_de[NSEC] = { SND_BARRIDO, SND_LASER, SND_WARP, SND_CAD, SND_LINEA };
    if (a == 0) sonar2(SND_BARRIDO, SND_OLEADA); else sonar(efecto_de[a]);
    if (a == 0) {
        preparar_onda();
        sello_preparar();
        inicio_abierto_sub = 0;
        fx_mostrar(1);
        efecto(paso_inicio, &reloj);
        memset(pose.blanco, 0, sizeof(pose.blanco));
        pintar_heroe();
        fx_mostrar(0);
    } else if (a == 1) {
        sep_obj = SF(42);
        apps_abierto = 0;
        fx_mostrar(1);
        efecto(paso_apps, &reloj);
        fx_mostrar(0);
    } else if (a == 4) {
        apagar_abierto = 0;
        apagar_paso = -1;
        fx_mostrar(1);
        efecto(paso_apagar, &reloj);
        heroe_manual = 0;
        taek_mover(s_fondo.id, 0, 0);
        mover_heroe(1);
        /* La linea plana se queda mientras se elige */
    }
}

static void cerrar_submenu(void) {
    long reloj = 0;
    if (!fx_activo) fx_mostrar(1);
    filas_manuales = 1;
    efecto(paso_salida, &reloj);
    filas_manuales = 0;
    fx_mostrar(0);
    nivel = 0;
    sep_obj = 0;
    pintar_nivel();
}

/* --------------------------------------------------------------- escaneo */

struct dato_escaneo { const char *t; char v[48]; int faceta, der; };
static struct dato_escaneo datos[7];
static long esc_t0;
static int esc_quieto;
static float ret_x0, ret_y0, ret_x1, ret_y1;

static void preparar_datos(void) {
    struct txt t;
    int k = 0;
#define DATO(T, F, D) do { datos[k].t = T; datos[k].faceta = F; datos[k].der = D; int _n = 0; while (t.s[_n] && _n < 47) { datos[k].v[_n] = t.s[_n]; _n++; } datos[k].v[_n] = 0; k++; } while (0)
    cpu_corto(&t); { struct txt c; t_ini(&c); t_add(&c, "Intel "); if (info.cpu[0] && t.s[0] == 'i') { t_add(&c, "Core "); t_add(&c, t.s); t = c; } }
    DATO("PROCESADOR", 11, 1);
    t_ini(&t); t_num(&t, info.cpus); t_add(&t, " hilos · "); t_num(&t, info.tsc_mhz); t_add(&t, " MHz");
    DATO("HILOS", 9, 1);
    t_ini(&t);
    if (info.gpu_proveedor_id) { t_add(&t, info.gpu_proveedor); t_add(&t, " · "); t_hex4(&t, info.gpu_proveedor_id); t_add(&t, ":"); t_hex4(&t, info.gpu_id); } else t_add(&t, "No detectada");
    DATO("GRÁFICOS", 7, 1);
    uint64_t usada = info.ram_total > info.ram_libre ? info.ram_total - info.ram_libre : 0;
    t_ini(&t); t_num(&t, info.ram_total >> 20); t_add(&t, " MiB · "); t_num(&t, usada >> 20); t_add(&t, " en uso");
    DATO("MEMORIA", 10, 0);
    t_ini(&t); t_num(&t, info.ancho); t_add(&t, " x "); t_num(&t, info.alto); t_add(&t, " · "); t_add(&t, info.capa[0] ? info.capa : "CPU");
    DATO("PANTALLA", 6, 0);
    cadena_bateria(&t);
    DATO("BATERÍA", 5, 0);
    t_ini(&t);
    if (info.red_ip_ok) { for (int i = 0; i < 4; i++) { if (i) t_add(&t, "."); t_num(&t, info.ip[i]); } t_add(&t, " · "); t_num(&t, info.red_mbps); t_add(&t, " Mb/s"); }
    else t_add(&t, info.red_presente ? (info.red_enlace ? "Enlace, sin IP" : "Sin cable") : "Sin controlador");
    DATO("RED", 0, 0);
#undef DATO
}

static void dato_caja(int i, float *x, float *y) {
    int k = 0;
    for (int j = 0; j < i; j++) if (datos[j].der == datos[i].der) k++;
    float cx0, cy0, cx1, cy1;
    logo_caja(&cx0, &cy0, &cx1, &cy1);
    if (datos[i].der) {
        float a = cx1 - SF(40), b = (float)(W - S(250) - S(20));
        *x = a < b ? a : b;
        *y = (float)H * (0.07f + (float)k * 0.085f);
    } else {
        *x = SF(18);
        *y = (float)H * (0.33f + (float)k * 0.13f);
    }
}

static void pintar_escaneo(long t) {
    float cx0, cy0, cx1, cy1, pad = SF(14);
    logo_caja(&cx0, &cy0, &cx1, &cy1);
    cy0 += (float)heroe_dy; cy1 += (float)heroe_dy;
    /* Reticula: de la pantalla entera al hexagono */
    float e = sale((float)t / 450.0f);
    ret_x0 = mezclaf(8, cx0 - pad, e); ret_y0 = mezclaf(8, cy0 - pad, e);
    ret_x1 = mezclaf((float)W - 8, cx1 + pad, e); ret_y1 = mezclaf((float)H - 8, cy1 + pad, e);
    int x0 = (int)ret_x0, y0 = (int)ret_y0, an = (int)(ret_x1 - ret_x0), al = (int)(ret_y1 - ret_y0);
    esquinas(&l_fx, x0 - S(3), y0 - S(3), an + S(6), al + S(6), S(48), S(10), premul(60, NEON));
    esquinas(&l_fx, x0, y0, an, al, S(42), S(5), OPACO(NEON));
    fx_marcar((float)x0 - SF(4), (float)y0 - SF(36), (float)(x0 + an) + SF(4), (float)(y0 + al) + SF(20));
    /* Titulo y barra */
    char b[64];
    if (t < 1200) descifrado(b, sizeof(b), "ESCANEANDO // NEKO-01", 1, 0);
    else descifrado(b, sizeof(b), "ANÁLISIS COMPLETO // [INTRO] CABINA MAGI", limitar01((float)(t - 1200) / 400.0f), (uint32_t)t * 2654435761u);
    int tw = texto_ancho(S(14), b) + S(20);
    rect(&l_fx, x0, y0 - S(30), tw, S(22), OPACO(NEON));
    texto(&l_fx, x0 + S(10), y0 - S(14), S(14), NEGRO, 255, 0, b);
    fx_marcar((float)x0, (float)y0 - SF(30), (float)(x0 + tw), (float)y0);
    rect(&l_fx, x0, y0 + al + S(9), an, S(5), premul(51, NEON));
    int pasos = t < 480 ? 0 : t > 1180 ? 14 : (int)((t - 480) * 14 / 700);
    rect(&l_fx, x0, y0 + al + S(9), an * pasos / 14, S(5), OPACO(NEON));
    /* Datos: linea con codo desde su faceta, punto que late y caja que se descubre */
    for (int i = 0; i < 7; i++) {
        long ti = t - 1200 - i * 140;
        if (ti < 0) continue;
        float x, y, ax, ay;
        dato_caja(i, &x, &y);
        faceta_centro(datos[i].faceta, pose.sep, 0, (float)heroe_dy, &ax, &ay);
        /* La caja crece con su valor: un nombre de GPU largo no debe salirse y dejar restos */
        float ancho = (float)texto_ancho(S(18), datos[i].v) + SF(28);
        if (ancho < SF(250)) ancho = SF(250);
        float medio = y + SF(22), codo = datos[i].der ? x - SF(34) : x + ancho + SF(34), borde = datos[i].der ? x : x + ancho;
        float l1 = raizf((codo - ax) * (codo - ax) + (medio - ay) * (medio - ay)), l2 = absf(borde - codo), L = l1 + l2;
        float u = limitar01((float)ti / 300.0f) * L;
        float px = u < l1 ? mezclaf(ax, codo, u / l1) : codo, py = u < l1 ? mezclaf(ay, medio, u / l1) : medio;
        fx_trazo(ax, ay, px, py, SF(5), premul(50, NEON), 1);
        fx_trazo(ax, ay, px, py, SF(1.6f), OPACO(NEON), 1);
        if (u > l1) {
            float qx = mezclaf(codo, borde, (u - l1) / l2);
            fx_trazo(codo, medio, qx, medio, SF(5), premul(50, NEON), 1);
            fx_trazo(codo, medio, qx, medio, SF(1.6f), OPACO(NEON), 1);
        }
        float pulso = esc_quieto ? 0.5f : 0.5f + 0.5f * sen((float)t * 0.0078f);
        int rp = S(5) + (int)(pulso * SF(3));
        anillo(&l_fx, (int)ax, (int)ay, rp + S(2), 0, OPACO(NEON));
        anillo(&l_fx, (int)ax, (int)ay, rp - S(1), 0, OPACO(0x07080Cu));
        fx_marcar(ax - SF(12), ay - SF(12), ax + SF(12), ay + SF(12));
        long tc = ti - 300;
        if (tc < 0) continue;
        float rev = sale((float)tc / 300.0f);
        int bx = (int)x, by = (int)y, bw = (int)(ancho * rev), bh = S(54);
        if (bw < 1) continue;
        rect(&l_fx, bx + S(6), by + S(6), bw, bh, premul(216, ROJO));
        rect(&l_fx, bx, by, bw, bh, premul(240, 0x05060Au));
        rect(&l_fx, bx, by, S(4), bh, OPACO(NEON));
        /* El texto se recorta con la caja: se dibuja en su sitio y lo que sobra lo tapa nada; basta con no dibujarlo antes */
        if (rev > 0.6f) {
            texto(&l_fx, bx + S(14), by + S(19), S(14), NEON, 255, 0, datos[i].t);
            descifrado(b, sizeof(b), datos[i].v, limitar01((float)tc / 520.0f), (uint32_t)(t / 30) * 2246822519u + (uint32_t)i);
            texto(&l_fx, bx + S(14), by + S(44), S(18), BLANCO, 255, 0, b);
        }
        /* Los glifos al azar del descifrado pueden ser mas anchos que el texto final: margen de sobra */
        fx_marcar((float)bx, (float)by, (float)(bx + (int)ancho + S(60)), (float)(by + bh + S(8)));
    }
}

static void entrar_escaneo(void) {
    preparar_datos();
    estado = E_ESCANEO;
    esc_t0 = taek_ms();
    esc_quieto = 0;
    fx_mostrar(1);
    float x0, y0, x1, y1;
    logo_caja(&x0, &y0, &x1, &y1);
    taek_mover(s_scan.id, (int)x0 - S(20), (int)y0);
    dibujar_pie();
}
static void salir_escaneo(void) {
    fx_mostrar(0);
    ocultar(&s_scan);
    estado = E_MENU;
    dibujar_pie();
}
/* H151: despues del escaneo se despliegan los siete modulos MAGI, como en el prototipo (solo, o antes con Intro) */
static void escaneo_a_modulos(void) {
    if (estado != E_ESCANEO) return;
    fx_mostrar(0);
    ocultar(&s_scan);
    estado = E_MENU;
    sel = 3; nivel = 1; subsel = 0;
    pintar_nivel_modo(ENTRA_CASCADA);
    sonar(SND_OLEADA);
}

static void animar_escaneo(void) {
    long t = taek_ms() - esc_t0;
    if (!esc_quieto) {
        fx_empezar();
        pintar_escaneo(t);
        fx_terminar();
        /* Una vez todo dicho, la capa queda quieta: solo se mueve la linea de escaneo */
        if (t > 1200 + 6 * 140 + 300 + 600) esc_quieto = 1;
    } else if (t > 1200 + 6 * 140 + 300 + 600 + 1200 && !modo_prueba) { escaneo_a_modulos(); return; }
    float x0, y0, x1, y1;
    logo_caja(&x0, &y0, &x1, &y1);
    float f = (float)(t % 1800) / 900.0f;
    if (f > 1) f = 2 - f;
    taek_mover(s_scan.id, (int)x0 - S(20), (int)(y0 + (y1 - y0 - (float)l_scan.al) * suave(f)) + heroe_dy);
}

static void preparar_linea_escaneo(void) {
    struct lienzo *l = &l_scan;
    limpiar(l, 0);
    int c = l->al - S(4);
    for (int y = 0; y < c; y++) rect(l, 0, y, l->an, 1, premul((uint32_t)(y * 60 / (c > 0 ? c : 1)), NEON));
    rect(l, 0, c, l->an, S(3), OPACO(NEON));
    rect(l, 0, c + S(1), l->an, S(1), OPACO(BLANCO));
    taek_presentar(s_scan.id, 0, 0, l->an, l->al);
}

/* H150: la cabina MAGI se abre desde el escaneo; mientras sus hojas bajan, el menu y el escaneo siguen vivos. */
static void animar_escaneo_y_menu(void) { animar_menu(); animar_escaneo(); }
#include "cabina.h"

/* ----------------------------------------------------- escritorio: el vuelo */

static long vuelo_ms, vuelo_cuadros, vuelo_peor;
static long ir_t_escritorio;
static int ir_escritorio_hecho;
static void cada_cuadro_ida(long t) {
    huida(t);
    if (!ir_escritorio_hecho && t >= ir_t_escritorio) {
        ir_escritorio_hecho = 1;
        entrar_escritorio();
        taek_elevar(s_fx.id);
    }
}

/* El gato se desarma: sus lineas forman NEKOSAKA, cae el sello y luego dibujan el escritorio, que aparece debajo. */
static void ir_escritorio(void) {
    long reloj = 0, t0 = taek_ms();
    /* Leer el equipo y la bateria puede tardar decenas de ms: antes de despegar, no en pleno vuelo */
    leer_equipo();
    taek_info(&info, 1);
    ef_cuadros = 0; ef_peor = 0;
    if (nivel) nivel = 0;
    v_n = segs_gato(v_seg);
    sep_obj = pose.sep = 0;
    fx_mostrar(1);
    heroe_sin_gato(1);
    ocultar(&s_scan);
    ir_escritorio_hecho = 0;
    ir_t_escritorio = 1 << 30;
    vuelo_cada_cuadro = cada_cuadro_ida;
    int m = segs_palabra(f_tmp, nfinos, (float)W * 0.62f, (float)H * 0.46f);
    (void)m;
    volar(850, 260, 300, f_tmp, v_n, 0, &reloj);
    destello_y_sello(420, 560, &reloj);
    segs_aero(f_tmp, nfinos);
    ir_t_escritorio = reloj + 250;
    volar(1000, 320, 260, f_tmp, v_n, 0, &reloj);
    if (!ir_escritorio_hecho) { ir_escritorio_hecho = 1; entrar_escritorio(); taek_elevar(s_fx.id); }
    vuelo_cada_cuadro = NULL;
    desvanecer(450, &reloj);
    fx_mostrar(0);
    heroe_sin_gato(0);
    capturas_limpiar();
    vuelo_ms = taek_ms() - t0; vuelo_cuadros = ef_cuadros; vuelo_peor = ef_peor;
}

static void cada_cuadro_vuelta(long t) { (void)t; animar_menu(); }

/* Y de vuelta: el escritorio se deshace en lineas que forman NEKOSAKA y rearman al gato. */
static void salir_escritorio(void) {
    long reloj = 0;
    ef_cuadros = 0; ef_peor = 0;
    sep_obj = pose.sep = 0;
    v_n = segs_gato(f_tmp);
    int ng = v_n;
    segs_aero(v_seg, nfinos);
    v_n = ng;
    fx_mostrar(1);
    esc_ocultar();
    ocultar(&s_pan);
    estado = E_MENU;
    nivel = 0;
    sel = 2;
    for (int i = 0; i < NSEC; i++) dibujar_fila(i);
    mostrar_capas_menu();
    pose.sin_gato = 1;
    heroe_sucio = 1;
    heroe_mostrar(1);
    pintar_nivel();
    vuelo_cada_cuadro = cada_cuadro_vuelta;
    static struct seg gato[MAXSEG];
    for (int i = 0; i < ng; i++) gato[i] = f_tmp[i];
    segs_palabra(f_tmp, nfinos, (float)W * 0.62f, (float)H * 0.46f);
    volar(800, 240, 260, f_tmp, v_n, 0, &reloj);
    destello_y_sello(400, 520, &reloj);
    volar(900, 280, 300, gato, v_n, 0, &reloj);
    vuelo_cada_cuadro = NULL;
    heroe_sin_gato(0);
    fx_empezar();
    fx_terminar();
    fx_mostrar(0);
    capturas_limpiar();
}

/* ----------------------------------------------------------------- entrada */

static long posicion(int accion) {
    /* H146: si se sale desde el escritorio (para abrir una aplicacion), se vuelve a el */
    long escritorio = estado == E_ESCRITORIO ? TAEK_POS_ESCRITORIO | (long)icono_sel << 10 : 0;   /* Y en que icono */
    if (estado == E_ESCRITORIO && esc_reabrir_videos) escritorio |= 1L << 14;   /* H148: volver a la ventana de Videos */
    return (long)accion | ((long)(sel | subsel << 4 | nivel << 8) | escritorio) << 8;
}

static void atras(void) {
    if (estado == E_ESCANEO) { salir_escaneo(); sonar(SND_REVERSO); return; }
    if (nivel) { cerrar_submenu(); sonar(SND_REVERSO); }
}

/* H151: Intro sobre una opcion de submenu. Devuelve una accion para el nucleo, o 0. */
static int activar_opcion(void) {
    const struct opcion *o = &secciones[sel].sub[subsel];
    int a = o->accion;
    if (a > 0) { sonar(SND_LINEA); return a; }
    switch (a) {
    case A_VOLVER: cerrar_submenu(); sonar(SND_REVERSO); break;
    case A_ESPECIFICACIONES: fx_mostrar(0); sonar(SND_BARRIDO); barrido(entrar_especificaciones, NULL, 0); break;
    case A_BATERIA: fx_mostrar(0); sonar(SND_BARRIDO); barrido(entrar_bateria, NULL, 0); break;
    case A_PANEL:
        switch (o->panel) {
        case P_TERMINAL: case P_STREAM: panel_foco = 1; sonar(SND_TICK); break;
        case P_HUEVO: sonar(SND_BARRIDO); pan_huevo_correr(); break;
        case P_PAGINAS: sonar(SND_CAD); pan_paginas_correr(); break;
        case P_ORACULO: sonar(SND_OLEADA); ora_correr(); break;
        default: sen_leer(0); sonar(SND_TICK); break;
        }
        dibujar_inspector();
        break;
    case A_CABINA: entrar_cabina(o->panel - P_MOD0); break;
    case A_APP: { sonar(SND_WARP); return esc_lanzar(o->arg, NULL); }
    case A_ORDEN: {
        int r = orden_pantalla(o->arg);
        if (r) { sonar(SND_OLEADA); return r; }
        break;
    }
    case A_CONFIRMAR:
        if (!confirmar) { confirmar = 1; sonar(o->panel == P_RULETA ? SND_TAMBOR : SND_GLITCH); dibujar_inspector(); break; }
        confirmar = 0;
        { int r = orden_pantalla(o->arg); if (r) { sonar(o->panel == P_RULETA ? SND_DISPARO : SND_GLITCH); return r; } }
        break;
    default: break;
    }
    return 0;
}

/* Devuelve una accion para el nucleo, o 0. */
static int tecla_menu(uint32_t k) {
    if (estado == E_ESCANEO) {
        if (k == 27 || k == 8) atras();
        else if (k == '\n' || k == '\r' || k == ' ') escaneo_a_modulos();   /* H151: los modulos MAGI */
        else if (k >= '1' && k < '1' + NMOD) { escaneo_a_modulos(); subsel = (int)(k - '1'); entrar_cabina(subsel); }
        else if (k == 9) { salir_escaneo(); ir_escritorio(); }
        return 0;
    }
    /* H151: con el panel enfocado, las teclas son suyas (Esc lo suelta) */
    if (nivel && panel_foco) {
        const struct opcion *o = &secciones[sel].sub[subsel];
        if (o->panel == P_TERMINAL) {
            int soltar = 0, a = term_tecla(k, &soltar);
            if (soltar) panel_foco = 0;
            if (k == '\n' || k == '\r') sonar(SND_TICK);
            dibujar_inspector();
            return a;
        }
        if (o->panel == P_STREAM) { if (stm_tecla(k)) panel_foco = 0; if (k == '\n') sonar(SND_OLEADA); dibujar_inspector(); return 0; }
        panel_foco = 0;
    }
    int n = opciones();
    int *cur = nivel ? &subsel : &sel;
    if (k == 17 || k == 'w' || k == 'W' || k == 'k') cambiar_seleccion((*cur + n - 1) % n, cur);
    else if (k == 18 || k == 's' || k == 'S' || k == 'j') cambiar_seleccion((*cur + 1) % n, cur);
    else if (!nivel && k >= '1' && k <= '5') { cambiar_seleccion((int)(k - '1'), &sel); return tecla_menu('\n'); }
    else if (nivel && sel == 3 && k >= '1' && k < '1' + NMOD) { cambiar_seleccion((int)(k - '1'), &subsel); entrar_cabina(subsel); }
    else if (k == 9) { if (nivel) { nivel = 0; sep_obj = 0; fx_mostrar(0); } sel = 2; ir_escritorio(); }
    else if (k == '\n' || k == '\r' || k == ' ') {
        if (!nivel) {
            if (sel == 2) { sonar(SND_WARP); ir_escritorio(); return 0; }
            if (sel == 3) { sonar2(SND_CAD, SND_TICK); entrar_escaneo(); return 0; }
            entrar_seccion(sel);
        } else return activar_opcion();
    } else if (k == 27 || k == 8) {
        if (confirmar) { confirmar = 0; dibujar_inspector(); return 0; }
        atras();
    }
    return 0;
}
/* Ctrl+C siempre lleva a la consola: la salida de emergencia, desde cualquier sitio. */
static int tecla(uint32_t k) {
    if (k == 3) return TAEK_ACCION_TERMINAL;
    switch (estado) {
    case E_MENU: case E_ESCANEO: return tecla_menu(k);
    case E_ESCRITORIO: return esc_tecla(k);
    case E_CABINA: return cab_tecla(k);
    default:
        if (k == 27 || k == 8 || k == '\n' || k == ' ') barrido(al_volver_menu, NULL, 0);
        return 0;
    }
}

/* Las flechas por serie llegan como ESC [ A. Un Esc solo es Esc si no sigue nada enseguida. */
static int esc_paso;
static long esc_t;
static uint32_t filtrar(uint32_t k) {
    if (esc_paso == 1) {
        if (k == '[') { esc_paso = 2; return 0; }
        esc_paso = 0;
    } else if (esc_paso == 2) {
        esc_paso = 0;
        return k == 'A' ? 17 : k == 'B' ? 18 : k == 'C' ? TAEK_TECLA_DERECHA : k == 'D' ? TAEK_TECLA_IZQUIERDA : 0;
    } else if (k == 27) {
        esc_paso = 1; esc_t = taek_ms();
        return 0;
    }
    return k;
}

static int botones_antes;
static int ultimo_item = -1;
static int raton(const struct taek_evento *e) {
    if (!e->hay_puntero) return 0;
    int subida = (int)e->botones & ~botones_antes;
    botones_antes = (int)e->botones;
    if (estado == E_ESCRITORIO) return esc_raton(e, subida);
    if (estado == E_CABINA) return cab_raton(e, subida);
    if (estado == E_ESCANEO) { if (subida & 1) escaneo_a_modulos(); else if (subida) atras(); return 0; }
    if (estado != E_MENU) { if (subida) barrido(al_volver_menu, NULL, 0); return 0; }
    /* H151: clic en la tarjeta: la Terminal ejecuta la sugerencia pulsada; Terminal y Stream toman el teclado */
    if ((subida & 1) && nivel) {
        int lx = e->x - (int)m_insp.pos, ly = e->y - INSP_Y;
        if (lx >= 0 && ly >= 0 && lx < l_insp.an && ly < l_insp.al) {
            int p = secciones[sel].sub[subsel].panel;
            if (p == P_TERMINAL) {
                int hecho = 0, a = term_clic(lx, ly, &hecho);
                panel_foco = 1;
                if (hecho) sonar(SND_TICK);
                dibujar_inspector();
                return a;
            }
            if (p == P_STREAM) { panel_foco = 1; dibujar_inspector(); return 0; }
        }
    }
    int n = opciones(), item = -1;
    for (int i = 0; i < n; i++)
        if (fila_visible(i) && e->y >= fila_y(i) && e->y < fila_y(i) + fila_alto() && e->x >= fila_x(i) && e->x < fila_x(i) + S(700)) item = i;
    if (item >= 0 && item != ultimo_item) cambiar_seleccion(item, nivel ? &subsel : &sel);
    ultimo_item = item;
    if ((subida & 1) && item >= 0) return tecla_menu('\n');
    if (subida & 2) return tecla_menu(27);
    return 0;
}

static void animar(void) {
    switch (estado) {
    case E_MENU: animar_menu(); break;
    case E_ESCANEO: animar_menu(); animar_escaneo(); break;
    case E_ESCRITORIO: esc_animar(); break;
    case E_CABINA: cab_animar(); break;
    case E_ESPECIFICACIONES: pantalla_especificaciones(taek_ms() - pan_t0); break;
    case E_BATERIA: pantalla_bateria(taek_ms() - pan_t0); break;
    }
}

/* Un cuadro cada 16 ms, midiendo lo que ya se gasto. */
static void cuadros(int n) {
    for (int i = 0; i < n; i++) {
        long t = taek_ms();
        animar();
        long g = taek_ms() - t;
        if (g < 16) taek_esperar(16 - g);
    }
}

/* ------------------------------------------------------------------ medida */

static inline uint64_t tsc(void) {
    uint32_t a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    return (uint64_t)d << 32 | a;
}
#define US(c) ((long)((c) / (info.tsc_mhz ? info.tsc_mhz : 1)))

static void medida(const char *k, long v) {
    struct txt t; t_ini(&t); t_add(&t, "[MEDIR] "); t_add(&t, k); t_add(&t, "="); t_num(&t, v < 0 ? 0 : (uint64_t)v); t_add(&t, "\n");
    taek_escribir(t.s, t.n);
}

static void medir_estado(const char *nombre, int n) {
    uint64_t suma = 0, max = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t0 = tsc();
        animar();
        uint64_t d = tsc() - t0;
        suma += d;
        if (d > max) max = d;
        taek_esperar(4);
    }
    struct txt t; t_ini(&t); t_add(&t, nombre); t_add(&t, "_cuadro_us_medio"); medida(t.s, US(suma / (uint64_t)n));
    t_ini(&t); t_add(&t, nombre); t_add(&t, "_cuadro_us_max"); medida(t.s, US(max));
}

static void medir_efecto(const char *nombre, long ms) {
    struct txt t;
    t_ini(&t); t_add(&t, nombre); t_add(&t, "_ms"); medida(t.s, ms);
    t_ini(&t); t_add(&t, nombre); t_add(&t, "_cuadros"); medida(t.s, ef_cuadros);
    t_ini(&t); t_add(&t, nombre); t_add(&t, "_peor_cuadro_ms"); medida(t.s, ef_peor);
}

static long medir(long abrir_us, long equipo_us, long cielo_us) {
    medida("abrir_us", abrir_us);
    medida("equipo_us", equipo_us);
    medida("preparar_escritorio_us", cielo_us);
    medir_estado("menu_quieto", 120);
    uint64_t t0 = tsc(), peor = 0;
    for (int r = 0; r < 20; r++) {
        uint64_t c = tsc();
        tecla(18);
        uint64_t d = tsc() - c;
        if (d > peor) peor = d;
        cuadros(6);
    }
    medida("cambiar_opcion_us_max", US(peor));
    medida("cambiar_opcion_con_animacion_ms", US((tsc() - t0) / 20) / 1000);
    medir_estado("menu_tras_teclas", 60);
    barrido(NULL, NULL, 0);
    medida("barrido_ms", barrido_ms);
    cambiar_seleccion(0, &sel); cuadros(30);
    long t = taek_ms(); tecla('\n'); medir_efecto("inicio", taek_ms() - t);
    tecla(27); cuadros(30);
    cambiar_seleccion(1, &sel); cuadros(30);
    t = taek_ms(); tecla('\n'); medir_efecto("aplicaciones", taek_ms() - t);
    tecla(27); cuadros(30);
    cambiar_seleccion(4, &sel); cuadros(30);
    t = taek_ms(); tecla('\n'); medir_efecto("apagar", taek_ms() - t);
    tecla(27); cuadros(30);
    cambiar_seleccion(3, &sel); cuadros(30);
    tecla('\n');
    uint64_t suma = 0, max = 0;
    for (int i = 0; i < 180; i++) {
        uint64_t c = tsc(); animar(); uint64_t d = tsc() - c;
        suma += d; if (d > max) max = d;
        taek_esperar(12);
    }
    medida("escaneo_cuadro_us_medio", US(suma / 180));
    medida("escaneo_cuadro_us_max", US(max));
    /* H150: la cabina. Su cuadro incluye leer los sensores cuatro veces por segundo. */
    tecla('\n'); cuadros(30);   /* H151: el escaneo despliega los modulos; Intro abre el primero */
    t = taek_ms(); tecla('\n'); medida("cabina_abrir_ms", taek_ms() - t);
    medir_estado("cabina", 120);
    tecla('5'); medir_estado("cabina_nucleo", 60);
    tecla('h'); medir_estado("cabina_hex", 60);
    tecla('h'); tecla('7'); medir_estado("cabina_termico", 60);
    t0 = tsc(); sen_leer(0); medida("sensores_us", US(tsc() - t0));
    t0 = tsc(); sen_leer(TAEK_SEN_LEER_VOLUMEN); medida("sensores_volumen_us", US(tsc() - t0));
    t = taek_ms(); tecla(27); medida("cabina_cerrar_ms", taek_ms() - t);
    tecla(27); cuadros(20);
    cambiar_seleccion(2, &sel); cuadros(30);
    tecla('\n');
    medida("vuelo_ida_ms", vuelo_ms);
    medida("vuelo_ida_cuadros", vuelo_cuadros);
    medida("vuelo_ida_peor_cuadro_ms", vuelo_peor);
    medir_estado("escritorio_quieto", 120);
    t0 = tsc();
    ven_abrir(VT_EQUIPO);
    medida("abrir_ventana_us", US(tsc() - t0));
    medir_estado("escritorio_ventana", 60);
    t0 = tsc();
    for (int i = 0; i < 20; i++) esc_mover_ventana(0, S(200) + i * S(10), S(120) + i * S(4), 0);
    medida("arrastrar_ventana_us", US((tsc() - t0) / 20));
    t0 = tsc();   /* H148: escalar redibuja la ventana entera con su vidrio */
    for (int i = 0; i < 10; i++) geometria(0, vent[0].x, vent[0].y, S(800) + i * S(24), S(620) + i * S(12));
    medida("escalar_ventana_us", US((tsc() - t0) / 10));
    esc_cerrar(0);
    t = taek_ms();
    salir_escritorio();
    medida("vuelo_vuelta_ms", taek_ms() - t);
    medida("vuelo_vuelta_cuadros", ef_cuadros);
    medida("vuelo_vuelta_peor_cuadro_ms", ef_peor);
    taek_escribir("[MEDIR] FIN\n", 12);
    return TAEK_ACCION_TERMINAL;
}

/* ---------------------------------------------------------------- prueba */

static void informe(const char *k, long v) {
    struct txt t; t_ini(&t); t_add(&t, "[UI] "); t_add(&t, k); t_add(&t, "="); t_num(&t, v < 0 ? 0 : (uint64_t)v); t_add(&t, "\n");
    taek_escribir(t.s, t.n);
}

/* Recorre todo lo que se puede ver, captura el framebuffer real en mitad de cada transicion y golpea con teclas al azar. */
static long prueba(void) {
#define CAP(n) taek_captura(n, sizeof(n) - 1)
    cuadros(90);
    CAP("menu-inicio");
    tecla(18); cuadros(3); CAP("menu-golpe");
    cuadros(60); CAP("menu-aplicaciones");
    tecla(18); cuadros(60); CAP("menu-escritorio");
    tecla(18); cuadros(60); CAP("menu-configuracion");
    tecla(18); cuadros(60); CAP("menu-apagar");
    tecla(18); cuadros(60);
    /* INICIO: los rayos, el sello a medio caer y el submenu de 10 (H151) */
    capturar_en("inicio-rayos", 160);
    capturar_en("inicio-sello", 520);
    tecla('\n');
    informe("INICIO_CUADROS", ef_cuadros);
    informe("INICIO_PEOR_MS", ef_peor);
    cuadros(40); CAP("sub-inicio");
    informe("SENSORES", sen_hay());
    informe("SENSORES_V2", sen_v2());
    sen_leer(0); cuadros(30); sen_leer(0);   /* Dos lecturas: la carga sale de la diferencia */
    cambiar_seleccion(1, &subsel); cuadros(20); CAP("inicio-cpu");
    if (sen_carga_ok) { informe("CARGA_C0_PCT", (long)(sen_carga[0] * 100)); informe("CARGA_C1_PCT", (long)(sen_carga[1] * 100)); informe("GHZ_X100", (long)(sen_ghz * 100)); }
    cambiar_seleccion(2, &subsel); cuadros(20); CAP("inicio-gpu");
    cambiar_seleccion(3, &subsel); cuadros(10); tecla('\n'); cuadros(20); CAP("inicio-huevo");
    informe("ORDEN_MEMORIA_PROBAR", ord_r);
    cambiar_seleccion(4, &subsel); cuadros(10); tecla('\n'); cuadros(20); CAP("inicio-paginacion");
    informe("ORDEN_PAGINACION", ord_r);
    cambiar_seleccion(5, &subsel); cuadros(20); CAP("inicio-discos");
    cambiar_seleccion(6, &subsel); cuadros(20); CAP("inicio-lan");
    cambiar_seleccion(7, &subsel); cuadros(20); CAP("inicio-estres");
    cambiar_seleccion(8, &subsel); cuadros(10);
    tecla('\n'); cuadros(70); CAP("especificaciones");
    tecla(27); cuadros(60);
    cambiar_seleccion(9, &subsel); cuadros(10);
    tecla('\n'); cuadros(80); CAP("bateria");
    tecla(27); cuadros(60);
    tecla(27); cuadros(40); CAP("menu-de-vuelta");
    /* APLICACIONES: rayos desde las facetas; la Terminal, el Stream, el Oraculo y la Ruleta (sin dispararla) */
    cambiar_seleccion(1, &sel); cuadros(30);
    capturar_en("apps-rayos", 600);
    tecla('\n');
    informe("APPS_CUADROS", ef_cuadros);
    informe("APPS_PEOR_MS", ef_peor);
    cuadros(30); CAP("sub-aplicaciones");
    tecla('\n'); cuadros(5);
    { static const char escribir[] = "ayuda\n"; for (int i = 0; escribir[i]; i++) tecla((uint8_t)escribir[i]); }
    informe("ORDEN_AYUDA", ord_r);
    cuadros(10); CAP("app-terminal");
    { static const char escribir[] = "huevo\n"; for (int i = 0; escribir[i]; i++) tecla((uint8_t)escribir[i]); }
    informe("ORDEN_HUEVO", ord_r);
    { static const char escribir[] = "zzz-no-existe\n"; for (int i = 0; escribir[i]; i++) tecla((uint8_t)escribir[i]); }
    informe("ORDEN_NO_PERMITIDA", -ord_r);
    cuadros(10); CAP("app-terminal-2");
    tecla(27); cuadros(5);
    cambiar_seleccion(4, &subsel); tecla('\n');
    { static const char escribir[] = "Hola PC desde menu prueba\n"; for (int i = 0; escribir[i]; i++) tecla((uint8_t)escribir[i]); }
    informe("ORDEN_TRANSMITIR", ord_r);
    cuadros(10); CAP("app-stream");
    tecla(27);
    cambiar_seleccion(5, &subsel); tecla('\n'); cuadros(10); CAP("app-oraculo");
    informe("ORDEN_ORAR", ord_r);
    cambiar_seleccion(6, &subsel); tecla('\n'); cuadros(20); CAP("app-ruleta-confirmar");
    tecla(27); cuadros(5);
    cambiar_seleccion(1, &subsel); cuadros(20); CAP("app-navegador");
    tecla(27); cuadros(40);
    /* ESCRITORIO: las dos tarjetas de ambiente */
    cambiar_seleccion(2, &sel); cuadros(30); CAP("menu-ambientes");
    /* CONFIGURACION: escaneo y despues los siete modulos */
    cambiar_seleccion(3, &sel); cuadros(30);
    tecla('\n'); cuadros(40); CAP("escaneo-reticula");
    cuadros(150); CAP("escaneo-datos");
    tecla('\n'); cuadros(40); CAP("config-modulos");
    /* H150: la cabina MAGI, sus modulos y el modo hexadecimal */
    tecla('\n');
    informe("CABINA_ABRIR_PEOR_MS", ef_peor);
    if (sen_hay()) {
        informe("TEMP_OK", (long)sns.temp_ok); informe("TEMP_C", sns.temp_nucleo_c); informe("TEMP_PAQUETE_C", sns.temp_paquete_c);
        informe("TJMAX_C", sns.tjmax_c); informe("RED_RX", (long)sns.rx_paquetes); informe("STREAM_CONECTADO", (long)sns.stream_conectado);
        informe("HEAP_CANARIOS_OK", (long)sns.heap_canarios_ok); informe("HEAP_BLOQUES", (long)sns.heap_total_bloques);
    }
    informe("ARP_VECINOS", cab_narp);
    cuadros(30); CAP("cabina-red");
    tecla('2'); cuadros(20); CAP("cabina-stream");
    tecla('5'); cuadros(20); CAP("cabina-nucleo");
    tecla('h'); cuadros(10); tecla(TAEK_TECLA_DERECHA); tecla(TAEK_TECLA_ABAJO); cuadros(5); CAP("cabina-hex");
    tecla('h'); tecla('6'); tecla('l'); cuadros(20); CAP("cabina-vfs");
    informe("VOL_LEIDO", (long)vol_ult.leido); informe("VOL_BANDERAS", (long)vol_ult.banderas);
    tecla('7'); cuadros(20); CAP("cabina-termico");
    tecla('p'); cuadros(20); CAP("cabina-alarma-prueba");
    tecla('3'); tecla('t'); cuadros(20); CAP("cabina-audio");
    tecla('4'); cuadros(20); CAP("cabina-gpu");
    /* H204: el octavo modulo, LATENCIA (Fase 0 del gestor de hilos), y su vista hexadecimal */
    tecla('8'); cuadros(40); CAP("cabina-latencia");
    informe("SENSORES_V3", sen_v3());
    if (sen_v3()) { informe("USB_MAX_MS", (long)sns.usb_max_ms); informe("USB_ANIDADAS", (long)sns.usb_reentradas); informe("RED_MAX_MS", (long)sns.red_max_ms); }
    tecla('h'); cuadros(10); CAP("cabina-latencia-hex");
    tecla('h'); cuadros(5);
    informe("CABINA_CUADRO_MS", (long)cab_cuadro_ms);
    tecla(27); cuadros(20); CAP("cabina-de-vuelta");
    tecla(27); cuadros(30);
    /* APAGAR: sacudida, linea plana y el harakiri pidiendo confirmacion (nunca se confirma aqui) */
    cambiar_seleccion(4, &sel); cuadros(30);
    capturar_en("apagar-sacudida", 120);
    tecla('\n');
    cuadros(20); CAP("sub-apagar");
    cambiar_seleccion(2, &subsel); tecla('\n'); cuadros(10); CAP("apagar-confirmar");
    tecla(27); cuadros(5);
    tecla(27); cuadros(40);
    informe("SONIDO_SIN_TARJETA", snd_sin_tarjeta);    /* ESCRITORIO: el vuelo de ida y vuelta */
    cambiar_seleccion(2, &sel); cuadros(30);
    capturas_limpiar();
    capturar_en("vuelo-gato", 300);
    capturar_en("vuelo-palabra", 1100);
    capturar_en("vuelo-aero", 1900);
    ir_escritorio();
    informe("VUELO_MS", vuelo_ms);
    informe("VUELO_CUADROS", vuelo_cuadros);
    informe("VUELO_PEOR_MS", vuelo_peor);
    cuadros(30); CAP("escritorio");
    tecla('\n'); cuadros(30); CAP("ventana-equipo");
    tecla(18); tecla('\n'); cuadros(30); CAP("dos-ventanas");
    tecla(27); tecla(27); cuadros(10);
    tecla(9); cuadros(20); CAP("inicio-aero");
    tecla(9); cuadros(10);
    /* H148: las aplicaciones del escritorio en ventanas; buscar, escalar, maximizar y minimizar */
    if (estado != E_ESCRITORIO) { ir_escritorio(); cuadros(10); }
    if (inicio_abierto) inicio_abrir(0);
    ven_abrir(VT_IMAGENES); cuadros(20); CAP("ventana-imagenes");
    ven_abrir(VT_MUSICA); cuadros(20); CAP("ventana-musica");
    tecla('d'); tecla('u'); cuadros(5); CAP("ventana-buscar");
    tecla(27); cuadros(5);
    {
        int f = foco();
        if (f >= 0) geometria(f, vent[f].x - S(160), vent[f].y - S(30), vent[f].l.an + S(320), vent[f].l.al + S(120));
    }
    cuadros(5); CAP("ventana-escalada");
    ven_abrir(VT_VIDEOS); cuadros(20);
    if (foco() >= 0) maximizar(foco());
    cuadros(5); CAP("ventana-maximizada");
    if (foco() >= 0) minimizar(foco());
    cuadros(5); CAP("ventana-minimizada");
    /* H151: el menu Inicio del prototipo, buscando "na" (Navegador), y las ventanas: Terminal (con una orden real), Huevo,
     * Stream y Ruleta (sin dispararla) */
    inicio_abrir(1); cuadros(20); CAP("inicio-aero-prototipo");
    tecla('n'); tecla('a'); cuadros(5); CAP("inicio-aero-buscar");
    inicio_abrir(0); cuadros(5);
    ven_abrir(VT_TERMINAL); cuadros(5);
    { static const char escribir[] = "version\n"; for (int i = 0; escribir[i]; i++) tecla((uint8_t)escribir[i]); }
    informe("ORDEN_VERSION_ESCRITORIO", ord_r);
    cuadros(5); CAP("ventana-terminal");
    ven_abrir(VT_HUEVO); tecla('\n'); cuadros(5); CAP("ventana-huevo");
    ven_abrir(VT_STREAM); cuadros(5); CAP("ventana-stream");
    ven_abrir(VT_RULETA); tecla('\n'); cuadros(5); CAP("ventana-ruleta");
    tecla(27); cuadros(2);
    modo_quickmenu = 1; pintar_top(); cuadros(5); CAP("escritorio-modo-quickmenu");
    modo_quickmenu = 0; pintar_top();
    while (nz) esc_cerrar(z[nz - 1]);
    cuadros(5);
    capturas_limpiar();
    capturar_en("vuelta-palabra", 1000);
    tecla(27);
    informe("VUELTA_CUADROS", ef_cuadros);
    cuadros(40); CAP("menu-tras-escritorio");
    /* Teclas al azar en todos los estados; Tab y Enter disparan vuelos y efectos enteros */
    static const uint8_t juego[] = { 17, 18, '\n', 27, 8, 9, 'w', 's', ' ', 'x', 200, 0xFF, 1, 17, 18, 27 };
    long t0 = taek_ms(), nt = 0;
    capturas_limpiar();
    while (taek_ms() - t0 < 12000 && nt < 500) {
        int ac = tecla(juego[azar() % sizeof(juego)]);
        (void)ac;
        cuadros(2);
        nt++;
    }
    informe("TECLAS_AZAR", nt);
    if (estado == E_CABINA) salir_cabina();
    if (estado == E_ESCRITORIO) salir_escritorio();
    if (estado != E_MENU) al_volver_menu();
    nivel = 0; sel = 0; subsel = 0; sep_obj = 0; fx_mostrar(0);
    for (int i = 0; i < NSEC; i++) dibujar_fila(i);
    pintar_nivel();
    cuadros(70);
    CAP("final");
    taek_escribir("[UI] RESULTADO=PASS\n", 20);
    return TAEK_ACCION_TERMINAL;
}

/* ------------------------------------------------------------------ inicio */

long principal(uint64_t pos, uint64_t modo) {
    uint64_t t_abrir = tsc();
    modo_prueba = (int)modo;
    struct taek_pantalla p;
    if (taek_pantalla(&p)) return TAEK_ACCION_TERMINAL;
    W = (int)p.ancho; H = (int)p.alto;
    logo_esc = (float)H / 1080.0f;
    sel = (int)(pos & 15) % NSEC;
    nivel = (int)((pos >> 8) & 1);
    if (!secciones[sel].nsub) nivel = 0;
    subsel = nivel ? (int)((pos >> 4) & 15) % secciones[sel].nsub : 0;
    int volver_escritorio = (pos & TAEK_POS_ESCRITORIO) && !modo;   /* H146: de vuelta de una aplicacion */
    if (volver_escritorio && (int)((pos >> 10) & 15) < NICONOS) icono_sel = (int)((pos >> 10) & 15);
    if (volver_escritorio && ((pos >> 14) & 1)) esc_reabrir_videos = 1;
    taek_info(&info, 0);

    /* La capa del heroe abarca el hexagono y los fragmentos, con margen para flotar y separarse */
    float x0, y0, x1, y1;
    logo_caja(&x0, &y0, &x1, &y1);
    for (unsigned i = 0; i < sizeof(logo_fragmentos) / sizeof(logo_fragmentos[0]); i++)
        for (int k = 0; k < 3; k++) {
            float fx = logo_fragmentos[i].x[k] * logo_esc, fy = logo_fragmentos[i].y[k] * logo_esc;
            if (fx < x0) x0 = fx;
            if (fx > x1) x1 = fx;
            if (fy < y0) y0 = fy;
            if (fy > y1) y1 = fy;
        }
    hx = (int)x0 - S(70); hy = (int)y0 - S(70);
    if (hx < -S(40)) hx = -S(40);
    hw = (int)x1 + S(70) - hx; hh = (int)y1 + S(70) - hy;
    if (hy + hh > H + S(40)) hh = H + S(40) - hy;

    insp_an = S(640);
    if (W - S(1180) < insp_an) insp_an = W - S(1180);
    if (insp_an < S(440)) insp_an = S(440);
    insp_x = W - insp_an - S(40);

    /* Orden de creacion: de atras hacia delante. */
    if (crear(&s_fondo, &l_fondo, W, H, 0, 0, 0) ||
        crear(&s_heroe, &l_heroe, hw, hh, OCULTO, 0, TAEK_SUP_ALFA) ||
        crear(&s_scan, &l_scan, (int)(x1 - x0) + S(40), S(26), OCULTO, 0, TAEK_SUP_ALFA))
        return TAEK_ACCION_TERMINAL;
    for (int i = 0; i < NFILAS; i++)
        if (crear(&s_fila[i], &l_fila[i], FILA_AN, FILA_AL, OCULTO, 0, TAEK_SUP_ALFA)) return TAEK_ACCION_TERMINAL;
    for (int i = 0; i < NESQ; i++) {
        if (crear(&s_esq[i], &l_esq[i], S(90), S(90), OCULTO, 0, TAEK_SUP_ALFA)) return TAEK_ACCION_TERMINAL;
        static const uint32_t tonos[] = { ROJO, NEON, BLANCO, ROJO, NEGRO, NEON };
        int t = S(90);
        triangulo(&l_esq[i], (float)(azar() % (unsigned)(t / 3 + 1)), 0, (float)t, (float)(azar() % (unsigned)(t / 2 + 1)), (float)(azar() % (unsigned)(t / 2 + 1)), (float)t, OPACO(tonos[i]));
        taek_presentar(s_esq[i].id, 0, 0, t, t);
    }
    if (crear(&s_insp, &l_insp, insp_an, INSP_AL, OCULTO, INSP_Y, TAEK_SUP_ALFA) ||
        crear(&s_crono, &l_crono, texto_ancho(S(14), "HECHO EN ESPAÑOL // SIN TUTORIALES GRINGOS") + S(80), S(160), OCULTO, 0, TAEK_SUP_ALFA) ||
        crear(&s_estado, &l_estado, S(250), S(200), OCULTO, 0, TAEK_SUP_ALFA) ||
        crear(&s_pie, &l_pie, W, PIE_AL, OCULTO, 0, TAEK_SUP_ALFA) ||
        crear(&s_pan, &l_pan, W, H - S(102) - S(56), OCULTO, S(102), TAEK_SUP_ALFA) ||
        crear(&s_fx, &l_fx, W, H, OCULTO, 0, TAEK_SUP_ALFA))
        return TAEK_ACCION_TERMINAL;
    limpiar(&l_fx, 0);
    if (esc_crear()) return TAEK_ACCION_TERMINAL;
    /* El barrido es adorno: en pantallas enormes puede no haber memoria y la interfaz sigue sin el. */
    hay_barrido = crear(&s_barre, &l_barre, W + S(300), H, OCULTO, 0, TAEK_SUP_ALFA) == 0;
    if (hay_barrido) preparar_barrido();

    m_insp.obj = (float)insp_x;
    dibujar_fondo();
    pintar_heroe();
    preparar_linea_escaneo();
    dibujar_crono();
    dibujar_estado();
    pintar_nivel();
    mostrar_capas_menu();
    heroe_mostrar(1);
    if (!volver_escritorio) cuadros(40);
    long abrir_us = US(tsc() - t_abrir);
    taek_escribir("[UI] ABIERTO\n", 13);

    /* Primera lectura del firmware y el cielo del escritorio: tardan, por eso despues de dibujar. */
    uint64_t b0 = tsc();
    taek_info(&info, 1);
    long equipo_us = US(tsc() - b0);
    sen_leer(0);
    dibujar_crono();
    dibujar_estado();
    dibujar_inspector();
    b0 = tsc();
    preparar_escritorio();
    long cielo_us = US(tsc() - b0);

    if (modo_prueba == 2) return medir(abrir_us, equipo_us, cielo_us);
    if (modo_prueba) return prueba();
    if (volver_escritorio) entrar_escritorio();

    long ultimo_seg = -1, ultima_bat = taek_ms();
    struct taek_evento e;
    for (;;) {
        long inicio_cuadro = taek_ms();
        uint32_t botones = 0;   /* Cada evento trae los clics desde el anterior: se juntan los de todo el cuadro */
        for (int i = 0; i < 16; i++) {
            long r = taek_evento(&e);
            if (r < 0) return posicion(TAEK_ACCION_TERMINAL);
            botones |= e.botones;
            if (!r) break;
            uint32_t k = filtrar(e.tecla);
            if (k) { int a = tecla(k); if (a) return posicion(a); }
        }
        e.botones = botones;
        if (esc_paso == 1 && taek_ms() - esc_t > 40) {
            esc_paso = 0;
            int a = tecla(27);
            if (a) return posicion(a);
        }
        int a = raton(&e);
        if (a) return posicion(a);
        animar();
        long ahora = taek_ms();
        if (ahora / 1000 != ultimo_seg) {
            ultimo_seg = ahora / 1000;
            int leer_bat = ahora - ultima_bat > 30000;
            if (leer_bat) ultima_bat = ahora;
            int32_t pct = info.bateria_pct, est = info.bateria_estado, car = info.cargador;
            uint8_t min = info.minuto;
            taek_info(&info, leer_bat);
            if (!leer_bat) { info.bateria_pct = pct; info.bateria_estado = est; info.cargador = car; }
            if (estado != E_CABINA) sen_leer(0);   /* H150: la cabina lee 4 veces por segundo por su cuenta */
            if (estado == E_ESCRITORIO) esc_cada_segundo(info.minuto != min || leer_bat);
            else if (estado != E_CABINA) {
                dibujar_crono();
                if (info.minuto != min || leer_bat) dibujar_estado();
                /* H150-H151: los paneles que cambian solos (tablero, CPU, LAN, modulos...) estan en vivo */
                if (estado == E_MENU && ((!nivel && sel == 0) || (nivel && pan_vivo(secciones[sel].sub[subsel].panel)))) dibujar_inspector();
            }
        }
        /* H151: el cursor de texto de la Terminal y del Stream parpadea; la ruleta gira */
        static long ultimo_parpadeo;
        if (estado == E_MENU && nivel && ahora - ultimo_parpadeo >= (secciones[sel].sub[subsel].panel == P_RULETA ? 90 : 500) &&
            (panel_foco || secciones[sel].sub[subsel].panel == P_RULETA)) {
            ultimo_parpadeo = ahora;
            dibujar_inspector();
        }
        long g = taek_ms() - inicio_cuadro;
        if (g < 16) taek_esperar(16 - g);
    }
}
