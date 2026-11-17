/* Ver ps2.h. Referencias: la interfaz del 8042 (puertos 0x60 datos y 0x64 estado/ordenes) y el protocolo del mouse PS/2
 * (paquetes de 3 bytes), como los usa Linux en drivers/input/serio/i8042.c y drivers/input/mouse/psmouse-base.c.
 *   Estado (0x64): bit 0 hay un byte para leer, bit 1 el 8042 aun no tomo lo ultimo escrito, bit 5 el byte es del AUX.
 *   Ordenes: 0x20 leer configuracion, 0x60 escribirla, 0xAD/0xAE apagar/encender teclado, 0xA7/0xA8 lo mismo con el AUX,
 *            0xD4 el siguiente byte de datos va al mouse.
 *   Mouse: 0xFF reinicio (contesta FA, AA, id), 0xF6 valores por omision, 0xF4 empezar a mandar paquetes. FA = recibido.
 *   Paquete: byte 0 = botones (bits 0-2), siempre 1 (bit 3), signos de X e Y (bits 4-5), desbordes (bits 6-7); luego X, Y.
 * Todo por sondeo: se apagan las interrupciones del 8042 y nada aqui espera mas de un segundo. */
#include "ps2.h"
#include "teclado.h"
#include "puntero.h"
#include "consola.h"
#include "../arquitectura/x86_64/puertos.h"
#include "../base/tiempo.h"
#include "entrada.h"   /* H210: cerrojo de la entrada (Fase B2) */

#define DATOS  0x60
#define ESTADO 0x64
#define HAY_SALIDA  0x01
#define OCUPADO     0x02
#define ES_AUX      0x20

static struct {
    int intentado, activo, raton, paso_fallido;
    int config_antes, config_despues;
    int reinicio[3];
    uint64_t bytes_teclado, bytes_aux, teclas, paquetes, desincronizados, desbordes, otros_puertos;
    uint8_t paquete[3]; unsigned n; uint64_t ultimo_ms;
    uint8_t ultimo[3]; int hay_ultimo;
    int32_t resto_x, resto_y;
} P;

static char g_cola[64];
static unsigned g_ini, g_total;

int ps2_activo(void) { return P.activo; }
int ps2_raton_activo(void) { return P.raton; }

static uint64_t ms(void) { return tiempo_obtener_milisegundos(); }

static int escribir(uint16_t puerto, uint8_t v) {
    uint64_t t = ms();
    while (leer_puerto_b(ESTADO) & OCUPADO) {
        if (ms() - t > 20) return -1;
        __asm__ volatile("pause");
    }
    escribir_puerto_b(puerto, v);
    return 0;
}

/* Un byte del puerto pedido (0 teclado, 1 AUX, -1 cualquiera); los del otro se cuentan y se tiran. -1 si no llega. */
static int leer(int aux, unsigned espera_ms) {
    uint64_t t = ms();
    for (;;) {
        uint8_t e = leer_puerto_b(ESTADO);
        if (e & HAY_SALIDA) {
            uint8_t b = leer_puerto_b(DATOS);
            if (aux < 0 || ((e & ES_AUX) != 0) == aux) return b;
            P.otros_puertos++;
            continue;
        }
        if (ms() - t >= espera_ms) return -1;
        __asm__ volatile("pause");
    }
}

/* Una orden al mouse: su respuesta (FA si la acepto) o -1. Si pide repetir (FE), se repite una vez. */
static int al_raton(uint8_t orden) {
    for (int i = 0; i < 2; i++) {
        if (escribir(ESTADO, 0xD4) || escribir(DATOS, orden)) return -1;
        int r = leer(1, 100);
        if (r != 0xFE) return r;
    }
    return 0xFE;
}

static void vaciar(void) {
    for (int i = 0; i < 64 && (leer_puerto_b(ESTADO) & HAY_SALIDA); i++) (void)leer_puerto_b(DATOS);
}

static void hex8(int b) {
    static const char h[] = "0123456789ABCDEF";
    if (b < 0) { consola_imprimir("--"); return; }
    char s[3] = { h[(b >> 4) & 15], h[b & 15], 0 };
    consola_imprimir(s);
}

static int fallar(int verbose, int paso, const char *texto) {
    P.paso_fallido = paso;
    if (verbose) { consola_imprimir("  [PS2] "); consola_imprimir_linea(texto); }
    return P.activo ? 1 : -paso;
}

int ps2_iniciar(int verbose) { ENTRADA_EXCLUSIVA();
    P = (__typeof__(P)){0};
    P.config_antes = P.config_despues = -1;
    P.reinicio[0] = P.reinicio[1] = P.reinicio[2] = -1;
    P.intentado = 1;
    g_ini = g_total = 0;
    if (leer_puerto_b(ESTADO) == 0xFF) return fallar(verbose, 1, "El puerto 0x64 devuelve 0xFF: no hay 8042.");

    /* Apagar los dos puertos mientras se cambia la configuracion, y tirar lo que hubiera */
    if (escribir(ESTADO, 0xAD) || escribir(ESTADO, 0xA7)) return fallar(verbose, 2, "El 8042 no acepta ordenes.");
    vaciar();
    if (escribir(ESTADO, 0x20) || (P.config_antes = leer(-1, 50)) < 0) return fallar(verbose, 3, "No se pudo leer la configuracion.");
    /* Sin interrupciones (se sondea) y con los dos relojes encendidos. La traduccion (bit 6) queda como la dejo el
     * firmware: el teclado ya habla el juego 1 que entiende teclado.c. */
    uint8_t cfg = (uint8_t)P.config_antes & (uint8_t)~(0x01 | 0x02 | 0x10 | 0x20);
    if (escribir(ESTADO, 0x60) || escribir(DATOS, cfg)) return fallar(verbose, 4, "No se pudo escribir la configuracion.");
    if (escribir(ESTADO, 0xAE) || escribir(ESTADO, 0xA8)) return fallar(verbose, 5, "No se pudieron encender los puertos.");
    if (escribir(ESTADO, 0x20) == 0) P.config_despues = leer(-1, 50);
    if (!teclado_es_modo_nativo()) P.activo = 1;   /* En modo=ps2 el teclado ya lo lee teclado.c */
    if (verbose) {
        consola_imprimir("  [PS2] Configuracion "); hex8(P.config_antes); consola_imprimir(" -> "); hex8(P.config_despues);
        consola_imprimir_linea(P.activo ? ". Teclado interno leido junto a xHCI." : ".");
    }
    if (P.config_despues >= 0 && (P.config_despues & 0x20)) return fallar(verbose, 6, "El puerto AUX no se encendio: no hay mouse PS/2.");

    /* Reinicio del mouse: FA, luego AA (autoprueba superada) e id (00 = mouse normal). Puede tardar casi un segundo. */
    P.reinicio[0] = al_raton(0xFF);
    if (P.reinicio[0] == 0xFA) { P.reinicio[1] = leer(1, 1000); P.reinicio[2] = leer(1, 100); }
    if (verbose) {
        consola_imprimir("  [PS2] Reinicio del mouse:"); for (int i = 0; i < 3; i++) { consola_imprimir(" "); hex8(P.reinicio[i]); }
        consola_imprimir_linea("");
    }
    if (P.reinicio[0] != 0xFA || P.reinicio[1] != 0xAA) return fallar(verbose, 7, "Nadie contesto en el puerto AUX.");
    if (al_raton(0xF6) != 0xFA) return fallar(verbose, 8, "El mouse no acepto los valores por omision (F6).");
    if (al_raton(0xF4) != 0xFA) return fallar(verbose, 9, "El mouse no acepto empezar a mandar (F4).");
    P.raton = 1;
    if (verbose) consola_imprimir_linea("  [PS2] Mouse listo: paquetes de 3 bytes.");
    return 0;
}

void ps2_raton_byte(uint8_t b) { ENTRADA_EXCLUSIVA();
    P.bytes_aux++;
    if (!P.raton) return;
    uint64_t ahora = ms();
    if (P.n && ahora - P.ultimo_ms > 50) { P.n = 0; P.desincronizados++; }   /* Paquete a medias: se empieza de nuevo */
    P.ultimo_ms = ahora;
    if (!P.n && !(b & 0x08)) { P.desincronizados++; return; }                /* El primer byte siempre trae el bit 3 */
    P.paquete[P.n++] = b;
    if (P.n < 3) return;
    P.n = 0;
    for (int i = 0; i < 3; i++) P.ultimo[i] = P.paquete[i];
    P.hay_ultimo = 1;
    uint8_t s = P.paquete[0];
    if (s & 0xC0) { P.desbordes++; return; }
    int32_t dx = (int32_t)P.paquete[1] - ((s << 4) & 0x100);
    int32_t dy = -((int32_t)P.paquete[2] - ((s << 3) & 0x100));   /* En PS/2, Y positivo es hacia arriba */
    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy, v = ax > ay ? ax : ay;
    dx = puntero_acelerar(dx, v, &P.resto_x);
    dy = puntero_acelerar(dy, v, &P.resto_y);
    if (!puntero_publicar(dx, dy, s & 7u, PS2_DISPOSITIVO)) P.paquetes++;
}

void ps2_sondear(void) { if (entrada_cede_el_sondeo()) return; ENTRADA_EXCLUSIVA();
    if (!P.activo) return;
    for (int i = 0; i < 32; i++) {
        uint8_t e = leer_puerto_b(ESTADO);
        if (!(e & HAY_SALIDA)) return;
        uint8_t b = leer_puerto_b(DATOS);
        if (e & ES_AUX) { ps2_raton_byte(b); continue; }
        P.bytes_teclado++;
        char c = teclado_traducir(b);
        if (c && g_total < sizeof(g_cola)) { g_cola[(g_ini + g_total) % sizeof(g_cola)] = c; g_total++; P.teclas++; }
    }
}

char ps2_leer_caracter(void) {
    /* H211: la terminal lo llama en cada vuelta. Con la cola vacia y el 8042 apagado (o sondeado por el hilo de entrada)
     * no hay nada que hacer: no se toma el cerrojo. Asi CPU 0 no lo toma millones de veces (leccion de H210). */
    if (!__atomic_load_n(&g_total, __ATOMIC_RELAXED) && (!P.activo || entrada_cede_el_sondeo())) return 0;
    ENTRADA_EXCLUSIVA();
    ps2_sondear();
    if (!g_total) return 0;
    char c = g_cola[g_ini];
    g_ini = (g_ini + 1) % sizeof(g_cola);
    g_total--;
    return c;
}

static void estado(void) {
    consola_imprimir("[PS2] "); consola_imprimir(!P.intentado ? "sin iniciar" : P.activo ? "activo" : "inactivo");
    consola_imprimir(P.raton ? " mouse=listo" : " mouse=no");
    if (P.paso_fallido) { consola_imprimir(" paso_fallido="); consola_imprimir_dec((uint64_t)P.paso_fallido); }
    consola_imprimir(" config="); hex8(P.config_antes); consola_imprimir("->"); hex8(P.config_despues);
    consola_imprimir(" reinicio="); for (int i = 0; i < 3; i++) { hex8(P.reinicio[i]); if (i < 2) consola_imprimir(","); }
    consola_imprimir_linea("");
    consola_imprimir("[PS2] bytes_teclado="); consola_imprimir_dec(P.bytes_teclado);
    consola_imprimir(" teclas="); consola_imprimir_dec(P.teclas);
    consola_imprimir(" bytes_aux="); consola_imprimir_dec(P.bytes_aux);
    consola_imprimir(" paquetes="); consola_imprimir_dec(P.paquetes);
    consola_imprimir(" desincronizados="); consola_imprimir_dec(P.desincronizados);
    consola_imprimir(" desbordes="); consola_imprimir_dec(P.desbordes);
    consola_imprimir(" otros_puertos="); consola_imprimir_dec(P.otros_puertos);
    if (P.hay_ultimo) { consola_imprimir(" ultimo:"); for (int i = 0; i < 3; i++) { consola_imprimir(" "); hex8(P.ultimo[i]); } }
    consola_imprimir_linea("");
}

static int igual(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

void ps2_comando(const char *arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg || igual(arg, "estado")) { estado(); return; }
    if (igual(arg, "iniciar")) {
        int r = ps2_iniciar(1);
        consola_imprimir_linea(r == 0 ? "[PS2] RESULTADO=MOUSE" : r > 0 ? "[PS2] RESULTADO=SOLO_TECLADO" : "[PS2] RESULTADO=FALLO");
        estado();
        return;
    }
    if (igual(arg, "probar")) {
        if (!P.raton && ps2_iniciar(1) != 0) { consola_imprimir_linea("[PS2] Sin mouse PS/2."); estado(); return; }
        consola_imprimir_linea("[PS2] Mueve el dedo o el mouse y pulsa. q / Esc / Ctrl+C: salir (20 s como maximo).");
        const struct puntero_estado *p = puntero_obtener_estado();
        uint64_t t0 = ms(), sec = p->secuencia, visto = 0;
        while (ms() - t0 < 20000) {
            char c = consola_leer_caracter();   /* Vacia el 8042: en modo xHCI por ps2_leer_caracter, en modo=ps2 por teclado.c */
            if (c == 'q' || c == 27 || c == 3) break;
            if (p->secuencia != sec) {
                sec = p->secuencia;
                if (++visto % 4 == 1) {
                    consola_imprimir("[PS2] x="); consola_imprimir_dec((uint32_t)p->x); consola_imprimir(" y="); consola_imprimir_dec((uint32_t)p->y);
                    consola_imprimir(" botones="); consola_imprimir_dec(p->botones); consola_imprimir_linea("");
                }
            }
            esperar_microsegundos(500);
        }
        estado();
        return;
    }
    consola_imprimir_linea("ps2 [estado|iniciar|probar]: teclado interno y mouse PS/2 (8042) junto a xHCI.");
}
