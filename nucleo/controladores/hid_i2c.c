/* Ver hid_i2c.h. Protocolo (HID over I2C v1.0, secciones 5 a 7):
 *   - Descriptor HID: escribir el numero de registro (2 bytes, little endian) y leer 30 bytes.
 *   - Ordenes: escribir [registro de ordenes (2 bytes), argumento, codigo]. RESET = 1, SET_POWER = 8 (argumento 0 = encendido).
 *   - Reportes de entrada: una lectura sin escritura previa; los 2 primeros bytes son el largo total (0 = nada).
 * Nada de esto escribe en memoria del chipset: solo el bus I2C y la lectura del estado del pin. */
#include "hid_i2c.h"
#include "i2c_dw.h"
#include "hid_descriptor.h"
#include "puntero.h"
#include "consola.h"
#include "iommu.h"
#include "acpi/bateria.h"
#include "../base/tiempo.h"
#include "entrada.h"   /* H210: cerrojo de la entrada (Fase B2) */

#define EISA_PNP0C50 UINT32_C(0x500CD041)

struct candidato { uint8_t bus, dir; uint16_t khz; uint16_t reg_desc; char ruta[48]; };

static struct {
    int listo, intentado;
    uint8_t bus, dir;
    uint16_t reg_desc, lon_rep, reg_rep, reg_entrada, max_entrada, reg_orden, vid, pid, version;
    unsigned max_lectura;
    struct hid_raton raton;
    int hay_raton;
    const volatile uint32_t *pin;
    uint64_t pin_fisica;
    uint64_t reportes, otros, vacios, errores, ultimo_ms;
    uint8_t ultimo[16]; unsigned ultimo_n;
    /* Ultimo error: codigo de la transferencia (-2 tiempo, -3 aborto con su causa) o largo invalido con sus bytes */
    int err_codigo; uint32_t err_aborto; unsigned err_lon; uint8_t err_bytes[8];
    uint64_t err_i2c, err_largo, err_decodificar;
    int32_t resto_x, resto_y;   /* Fraccion de pixel que dejo la aceleracion, en 1/256 */
    char ruta[48];
} T;
static uint8_t g_desc_rep[4096];
static uint8_t g_entrada[256];

int tactil_listo(void) { return T.listo; }

static int32_t absoluto(int32_t v) { return v < 0 ? -v : v; }

static void hex8(uint8_t b) { static const char h[] = "0123456789ABCDEF"; char s[3] = { h[b >> 4], h[b & 15], 0 }; consola_imprimir(s); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int leer_registro(uint16_t reg, uint8_t *buf, unsigned n) {
    uint8_t w[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) };
    return i2c_dw_transferir(T.dir, w, 2, buf, n, 200);
}
static int orden(uint8_t codigo, uint8_t argumento) {
    uint8_t w[4] = { (uint8_t)T.reg_orden, (uint8_t)(T.reg_orden >> 8), argumento, codigo };
    return i2c_dw_transferir(T.dir, w, 4, 0, 0, 200);
}

/* ------------------------------------------------------------------ lo que dice el firmware */

/* Descriptor I2cSerialBus (ACPI 6.4.3.8.2.1): etiqueta 0x8E, tipo de bus 1. Velocidad en el byte 12, direccion en el
 * 16, y despues el nombre del controlador ("\\_SB.PCI0.I2C1"): su ultima cifra es el numero de bus. */
static int recurso_i2c(const uint8_t *b, uint32_t n, struct candidato *c) {
    for (uint32_t i = 0; i + 3 <= n;) {
        uint8_t tag = b[i];
        if (tag == 0x79) break;
        if (!(tag & 0x80)) { i += 1u + (tag & 7); continue; }
        uint32_t lon = (uint32_t)b[i + 1] | (uint32_t)b[i + 2] << 8;
        if (i + 3 + lon > n) break;
        if (tag == 0x8E && lon >= 15 && b[i + 5] == 1) {
            uint32_t vel = (uint32_t)b[i + 12] | (uint32_t)b[i + 13] << 8 | (uint32_t)b[i + 14] << 16 | (uint32_t)b[i + 15] << 24;
            uint16_t dir = le16(b + i + 16);
            uint16_t datos = le16(b + i + 10);
            uint32_t s = i + 12 + datos, fin = i + 3 + lon;
            int bus = -1;
            for (uint32_t k = s; k < fin && b[k]; k++) if (b[k] >= '0' && b[k] <= '9') bus = b[k] - '0';
            if (dir > 0x7F || bus < 0 || bus > 5) return -1;
            c->dir = (uint8_t)dir;
            c->bus = (uint8_t)bus;
            c->khz = vel >= 400000 ? 400 : 100;
            return 0;
        }
        i += 3 + lon;
    }
    return -1;
}

static int es_hid_i2c(const aml_valor *v) {
    if (v->tipo == AML_ENTERO) return (uint32_t)v->v.entero == EISA_PNP0C50;
    char id[16];
    if (!acpi_id_texto(v, id, sizeof(id))) return 0;
    const char *p = "PNP0C50";
    for (unsigned i = 0; p[i]; i++) if (id[i] != p[i]) return 0;
    return id[7] == 0;
}

static unsigned candidatos_acpi(struct candidato *c, unsigned max, int verbose) {
    aml *a = acpi_interprete();
    if (!a) { if (verbose) consola_imprimir_linea("  [TACTIL] Sin interprete ACPI: se prueba la direccion tipica."); return 0; }
    unsigned n = 0;
    static const char *const nombres[] = { "_CID", "_HID" };
    for (unsigned k = 0; k < 2; k++) {
        for (int d = aml_siguiente_nombre(a, -1, nombres[k]); d >= 0 && n < max; d = aml_siguiente_nombre(a, d, nombres[k])) {
            aml_valor v;
            if (aml_evaluar(a, d, 0, 0, &v) || !es_hid_i2c(&v)) continue;
            char ruta[96];
            aml_ruta(a, d, ruta, sizeof(ruta));
            unsigned largo = 0;
            while (ruta[largo]) largo++;
            if (largo <= 5) continue;
            ruta[largo - 5] = 0;                     /* Sin ".._CID": el dispositivo */
            int dev = aml_buscar(a, ruta);
            if (dev < 0) continue;
            int repetido = 0;
            for (unsigned j = 0; j < n; j++) { unsigned q = 0; while (c[j].ruta[q] && c[j].ruta[q] == ruta[q]) q++; if (!c[j].ruta[q] && !ruta[q]) repetido = 1; }
            if (repetido) continue;

            struct candidato x = {0};
            for (unsigned q = 0; ruta[q] && q + 1 < sizeof(x.ruta); q++) x.ruta[q] = ruta[q];
            /* Recursos: primero la plantilla con nombre (no depende de variables), luego _CRS */
            int ok = -1;
            static const char *const fuentes[] = { "SBFB", "_CRS" };
            for (unsigned f = 0; f < 2 && ok; f++) {
                int o = aml_hijo(a, dev, fuentes[f]);
                if (o >= 0 && !aml_evaluar(a, o, 0, 0, &v) && v.tipo == AML_BUFFER) ok = recurso_i2c(v.v.buffer, v.n, &x);
            }
            /* El registro del descriptor sale del _DSM de HID; muchas Dell lo dejan en HID2 tras _INI. _INI escribe en
             * registros GPIO: el interprete lo deja en sombra, sin tocarlos. */
            int ini = aml_hijo(a, dev, "_INI");
            if (ini >= 0) (void)aml_evaluar(a, ini, 0, 0, &v);
            int h2 = aml_hijo(a, dev, "HID2");
            if (h2 >= 0 && !aml_evaluar(a, h2, 0, 0, &v) && v.tipo == AML_ENTERO && v.v.entero && v.v.entero < 0x10000) x.reg_desc = (uint16_t)v.v.entero;
            if (verbose) {
                consola_imprimir("  [TACTIL] Firmware: "); consola_imprimir(x.ruta);
                if (ok) consola_imprimir_linea(" sin recurso I2C legible");
                else {
                    consola_imprimir(" bus I2C"); consola_imprimir_dec(x.bus);
                    consola_imprimir(" direccion 0x"); hex8(x.dir);
                    consola_imprimir(" "); consola_imprimir_dec(x.khz); consola_imprimir(" kHz");
                    consola_imprimir(" registro descriptor="); consola_imprimir_hex(x.reg_desc);
                    consola_imprimir_linea(x.reg_desc ? "" : " (desconocido: se prueban 0x20 y 0x01)");
                }
            }
            if (!ok) c[n++] = x;
        }
    }
    return n;
}

/* Estado del pin de interrupcion: \GPDI = grupo << 16 | pin, y \_SB.GADR(grupo, 2) da la base de su configuracion;
 * cada pin ocupa 8 bytes y el bit 1 de la primera palabra es el estado de la linea (0 = el trackpad tiene datos). */
static void preparar_pin(int verbose) {
    T.pin = 0;
    aml *a = acpi_interprete();
    if (!a) return;
    aml_valor v, args[2];
    int g = aml_buscar(a, "\\GPDI"), f = aml_buscar(a, "\\_SB.GADR");
    if (g < 0 || f < 0 || aml_evaluar(a, g, 0, 0, &v) || v.tipo != AML_ENTERO) return;
    uint32_t gpdi = (uint32_t)v.v.entero;
    for (unsigned i = 0; i < 2; i++) { args[i].tipo = AML_ENTERO; args[i].n = 0; args[i].ruta_ambito = 0; }
    args[0].v.entero = (gpdi >> 16) & 0xFF;
    args[1].v.entero = 2;
    if (aml_evaluar(a, f, args, 2, &v) || v.tipo != AML_ENTERO || !v.v.entero) return;
    uint64_t dir = v.v.entero + (uint64_t)(gpdi & 0xFFFF) * 8;
    const volatile uint32_t *p = (const volatile uint32_t *)acpi_mapear_lectura(dir);
    if (!p || *p == 0xFFFFFFFFu) return;
    T.pin = p;
    T.pin_fisica = dir;
    if (verbose) {
        consola_imprimir("  [TACTIL] Pin de interrupcion: GPDI="); consola_imprimir_hex(gpdi);
        consola_imprimir(" registro "); consola_imprimir_hex(dir);
        consola_imprimir(" valor "); consola_imprimir_hex(*p);
        consola_imprimir_linea((*p >> 1) & 1 ? " (linea alta: sin datos)" : " (linea baja: datos pendientes)");
    }
}

/* ------------------------------------------------------------------ puesta en marcha */

static int probar(const struct candidato *c, int verbose) {
    int r = i2c_dw_iniciar(c->bus, c->khz);
    if (r) {
        if (verbose) { consola_imprimir("  [TACTIL] Bus I2C"); consola_imprimir_dec(c->bus); consola_imprimir(" no arranca, codigo "); consola_imprimir_dec((uint64_t)-r); consola_imprimir_linea(""); }
        return -1;
    }
    T.bus = c->bus; T.dir = c->dir;
    uint16_t regs[3] = { c->reg_desc, 0x20, 0x01 };
    for (unsigned k = 0; k < 3; k++) {
        if (!regs[k] || (k && regs[k] == c->reg_desc)) continue;
        uint8_t d[30];
        r = leer_registro(regs[k], d, sizeof(d));
        if (verbose) {
            consola_imprimir("  [TACTIL] Descriptor en 0x"); hex8(c->dir); consola_imprimir(" registro "); consola_imprimir_hex(regs[k]);
            if (r) { consola_imprimir(": sin respuesta, codigo "); consola_imprimir_dec((uint64_t)-r); consola_imprimir(" aborto="); consola_imprimir_hex(i2c_dw_aborto()); consola_imprimir_linea(""); }
            else { consola_imprimir(":"); for (unsigned i = 0; i < 30; i++) { consola_imprimir(" "); hex8(d[i]); } consola_imprimir_linea(""); }
        }
        if (r) { if (r == -3 && (i2c_dw_aborto() & 1)) return -1; continue; }   /* Bit 0: nadie en esa direccion */
        if (le16(d) != 30 || le16(d + 2) != 0x0100) continue;
        T.reg_desc = regs[k];
        T.lon_rep = le16(d + 4); T.reg_rep = le16(d + 6);
        T.reg_entrada = le16(d + 8); T.max_entrada = le16(d + 10);
        T.reg_orden = le16(d + 16);
        T.vid = le16(d + 20); T.pid = le16(d + 22); T.version = le16(d + 24);
        for (unsigned q = 0; c->ruta[q] && q + 1 < sizeof(T.ruta); q++) T.ruta[q] = c->ruta[q];
        return 0;
    }
    return -1;
}

int tactil_iniciar(int verbose) { ENTRADA_EXCLUSIVA();
    T = (__typeof__(T)){0};
    T.intentado = 1;
    struct candidato c[6];
    unsigned n = candidatos_acpi(c, 6, verbose);
    if (!n) { c[0] = (struct candidato){1, 0x2C, 400, 0x20, "(sin firmware: tipica de Dell)"}; n = 1; }
    /* Primero los que el firmware confirma: su _INI les dio registro de descriptor. Los demas suelen ser variantes de
     * otros modelos de la misma placa (aqui TPD0 en el bus 0, que no existe y deja el bus colgado). */
    for (unsigned i = 1; i < n; i++)
        for (unsigned j = i; j > 0 && !c[j - 1].reg_desc && c[j].reg_desc; j--) { struct candidato t = c[j]; c[j] = c[j - 1]; c[j - 1] = t; }
    int elegido = -1;
    for (unsigned i = 0; i < n && elegido < 0; i++) if (!probar(&c[i], verbose)) elegido = (int)i;
    if (elegido < 0) { if (verbose) consola_imprimir_linea("  [TACTIL] Ningun dispositivo HID sobre I2C respondio."); return -1; }

    if (verbose) {
        consola_imprimir("  [TACTIL] Encontrado "); consola_imprimir(T.ruta);
        consola_imprimir(" VID="); consola_imprimir_hex(T.vid); consola_imprimir(" PID="); consola_imprimir_hex(T.pid);
        consola_imprimir(" version="); consola_imprimir_hex(T.version);
        consola_imprimir(" descriptor de reporte "); consola_imprimir_dec(T.lon_rep); consola_imprimir(" bytes");
        consola_imprimir(" entrada maxima "); consola_imprimir_dec(T.max_entrada); consola_imprimir_linea(" bytes");
    }
    if (!T.lon_rep || T.lon_rep > sizeof(g_desc_rep) || T.max_entrada < 3) { if (verbose) consola_imprimir_linea("  [TACTIL] Descriptor fuera de rango."); return -2; }
    T.max_lectura = T.max_entrada < sizeof(g_entrada) ? T.max_entrada : sizeof(g_entrada);

    int r = leer_registro(T.reg_rep, g_desc_rep, T.lon_rep);
    if (r) { if (verbose) consola_imprimir_linea("  [TACTIL] No se pudo leer el descriptor de reporte."); return -3; }
    r = hid_buscar_raton(g_desc_rep, T.lon_rep, &T.raton);
    T.hay_raton = r == 0;
    if (verbose) {
        if (T.hay_raton) {
            consola_imprimir("  [TACTIL] Raton en el reporte ID "); consola_imprimir_dec(T.raton.id);
            consola_imprimir(": "); consola_imprimir_dec(T.raton.n_botones); consola_imprimir(" botones, X "); consola_imprimir_dec(T.raton.x.tam);
            consola_imprimir(" bits en el bit "); consola_imprimir_dec(T.raton.x.bit);
            consola_imprimir(", Y "); consola_imprimir_dec(T.raton.y.tam); consola_imprimir(" bits en el bit "); consola_imprimir_dec(T.raton.y.bit);
            consola_imprimir_linea(T.raton.relativo ? ", relativo" : ", ABSOLUTO (no se usa)");
        } else {
            consola_imprimir("  [TACTIL] El descriptor no declara un raton (codigo "); consola_imprimir_dec((uint64_t)-r); consola_imprimir_linea("). Primeros bytes:");
            consola_imprimir("   ");
            for (unsigned i = 0; i < T.lon_rep && i < 48; i++) { consola_imprimir(" "); hex8(g_desc_rep[i]); }
            consola_imprimir_linea("");
        }
    }
    preparar_pin(verbose);

    /* Encender y reiniciar (Linux: i2c_hid_hwreset). El reinicio termina cuando el dispositivo deja un reporte de largo 0. */
    if (orden(8, 0)) { if (verbose) consola_imprimir_linea("  [TACTIL] SET_POWER no fue aceptado."); return -4; }
    esperar_milisegundos(1);
    if (orden(1, 0)) { if (verbose) consola_imprimir_linea("  [TACTIL] RESET no fue aceptado."); return -5; }
    uint64_t t = tiempo_obtener_milisegundos();
    int confirmado = 0;
    while (tiempo_obtener_milisegundos() - t < 1000) {
        if (T.pin && ((*T.pin >> 1) & 1)) { esperar_milisegundos(5); continue; }
        if (!T.pin) esperar_milisegundos(50);
        if (!i2c_dw_transferir(T.dir, 0, 0, g_entrada, 2, 50) && le16(g_entrada) == 0) { confirmado = 1; break; }
    }
    if (verbose) consola_imprimir_linea(confirmado ? "  [TACTIL] Reinicio confirmado por el dispositivo." :
                                                     "  [TACTIL] Sin confirmacion del reinicio en 1 s; se sigue de todos modos.");
    T.listo = 1;
    if (verbose) consola_imprimir_linea(T.pin ? "  [TACTIL] Listo: se lee cuando el pin avisa." : "  [TACTIL] Listo: sin pin, se pregunta cada 8 ms.");
    return 0;
}

/* ------------------------------------------------------------------ reportes */

int tactil_sondear(void) { if (entrada_cede_el_sondeo()) return 0; ENTRADA_EXCLUSIVA();
    if (!T.listo) return 0;
    /* Se lee si el pin avisa y, ademas, cada 10 ms aunque no avise: en la laptop se perdian reportes de clic (la
     * liberacion del boton no llegaba) confiando solo en el pin. Sin datos, el dispositivo responde largo 0. */
    uint64_t ahora = tiempo_obtener_milisegundos();
    int avisa = T.pin && !((*T.pin >> 1) & 1);
    /* H142: aunque el pin avise, al menos 2 ms entre lecturas. En H141 se leia en un bucle cerrado y, justo despues de
     * un reporte, el pin aun parecia avisar: 2987 lecturas caducaron en una prueba, cada una de hasta 20 ms. */
    if (ahora - T.ultimo_ms < 2u) return 0;
    if (!avisa && ahora - T.ultimo_ms < (T.pin ? 10u : 8u)) return 0;
    T.ultimo_ms = ahora;
    int k = i2c_dw_transferir(T.dir, 0, 0, g_entrada, T.max_lectura, 20);
    if (k) { T.errores++; T.err_i2c++; T.err_codigo = k; T.err_aborto = i2c_dw_aborto(); return -1; }
    unsigned lon = le16(g_entrada);
    if (lon <= 2) { T.vacios++; return 0; }
    if (lon > T.max_lectura) {
        T.errores++; T.err_largo++; T.err_lon = lon;
        for (unsigned i = 0; i < sizeof(T.err_bytes); i++) T.err_bytes[i] = g_entrada[i];
        return -1;
    }
    T.ultimo_n = lon - 2 < sizeof(T.ultimo) ? lon - 2 : sizeof(T.ultimo);
    for (unsigned i = 0; i < T.ultimo_n; i++) T.ultimo[i] = g_entrada[2 + i];
    if (!T.hay_raton || !T.raton.relativo) { T.otros++; return 0; }
    int32_t dx, dy, rueda; uint32_t b;
    int r = hid_decodificar_raton(&T.raton, g_entrada + 2, lon - 2, &dx, &dy, &b, &rueda);
    if (r == -2) { T.otros++; return 0; }
    if (r) { T.errores++; T.err_decodificar++; T.err_lon = lon; return -1; }
    /* H141: aceleracion. En modo mouse el Alps da unas pocas cuentas por reporte y a 1:1 cruzar 1920 pixeles pedia mas
     * recorrido que el que tiene el trackpad. Despacio se queda casi en 1:1 para apuntar; rapido llega a 2,5x. */
    int32_t v = absoluto(dx) > absoluto(dy) ? absoluto(dx) : absoluto(dy);
    dx = puntero_acelerar(dx, v, &T.resto_x);
    dy = puntero_acelerar(dy, v, &T.resto_y);
    if (puntero_publicar(dx, dy, b & 7u, TACTIL_DISPOSITIVO)) { T.errores++; return -1; }
    T.reportes++;
    return 1;
}

static void estado(void) {
    consola_imprimir("[TACTIL] "); consola_imprimir(T.listo ? "listo" : T.intentado ? "no encontrado" : "sin iniciar");
    consola_imprimir(" reportes="); consola_imprimir_dec(T.reportes);
    consola_imprimir(" otros="); consola_imprimir_dec(T.otros);
    consola_imprimir(" vacios="); consola_imprimir_dec(T.vacios);
    consola_imprimir(" errores="); consola_imprimir_dec(T.errores);
    if (T.ultimo_n) { consola_imprimir(" ultimo:"); for (unsigned i = 0; i < T.ultimo_n; i++) { consola_imprimir(" "); hex8(T.ultimo[i]); } }
    consola_imprimir_linea("");
    if (T.errores) {
        consola_imprimir("[TACTIL] errores: i2c="); consola_imprimir_dec(T.err_i2c);
        consola_imprimir(" (ultimo codigo "); consola_imprimir_dec((uint64_t)(uint32_t)-T.err_codigo);
        consola_imprimir(" aborto="); consola_imprimir_hex(T.err_aborto);
        consola_imprimir(") largo="); consola_imprimir_dec(T.err_largo);
        consola_imprimir(" (ultimo "); consola_imprimir_dec(T.err_lon); consola_imprimir(":");
        for (unsigned i = 0; i < sizeof(T.err_bytes); i++) { consola_imprimir(" "); hex8(T.err_bytes[i]); }
        consola_imprimir(") decodificar="); consola_imprimir_dec(T.err_decodificar);
        consola_imprimir_linea("");
    }
}

static int igual(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

void tactil_comando(const char *arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg || igual(arg, "estado")) { estado(); return; }
    if (igual(arg, "iniciar")) { int r = tactil_iniciar(1); consola_imprimir_linea(r ? "[TACTIL] RESULTADO=FALLO" : "[TACTIL] RESULTADO=LISTO"); return; }
    if (igual(arg, "probar") || igual(arg, "crudo")) {
        int crudo = arg[0] == 'c';
        if (!T.listo && tactil_iniciar(1)) { consola_imprimir_linea("[TACTIL] RESULTADO=FALLO"); return; }
        consola_imprimir_linea(crudo ? "[TACTIL] Reportes crudos durante 20 s. q / Esc / Ctrl+C: salir." :
                                       "[TACTIL] Mueve el dedo y pulsa. q / Esc / Ctrl+C: salir (20 s como maximo).");
        uint64_t t0 = tiempo_obtener_milisegundos(), visto = 0, otros = T.otros;
        const struct puntero_estado *p = puntero_obtener_estado();
        uint64_t sec = p->secuencia;
        while (tiempo_obtener_milisegundos() - t0 < 20000) {
            char c = consola_leer_caracter();
            if (c == 'q' || c == 27 || c == 3) break;
            uint64_t errores_antes = T.errores;
            int r = tactil_sondear();
            if (T.errores != errores_antes && T.errores <= 6) {
                consola_imprimir("[TACTIL] error: codigo "); consola_imprimir_dec((uint64_t)(uint32_t)-T.err_codigo);
                consola_imprimir(" aborto="); consola_imprimir_hex(T.err_aborto);
                consola_imprimir(" largo="); consola_imprimir_dec(T.err_lon); consola_imprimir(" bytes:");
                for (unsigned i = 0; i < sizeof(T.err_bytes); i++) { consola_imprimir(" "); hex8(T.err_bytes[i]); }
                consola_imprimir_linea("");
            }
            if (crudo && (r == 1 || T.otros != otros) && T.ultimo_n) {
                otros = T.otros;
                consola_imprimir("[TACTIL] crudo:"); for (unsigned i = 0; i < T.ultimo_n; i++) { consola_imprimir(" "); hex8(T.ultimo[i]); } consola_imprimir_linea("");
            }
            if (!crudo && p->secuencia != sec) {
                sec = p->secuencia;
                if (++visto % 4 == 1) {
                    consola_imprimir("[TACTIL] x="); consola_imprimir_dec((uint32_t)p->x); consola_imprimir(" y="); consola_imprimir_dec((uint32_t)p->y);
                    consola_imprimir(" botones="); consola_imprimir_dec(p->botones); consola_imprimir_linea("");
                }
            }
            esperar_microsegundos(500);
        }
        estado();
        return;
    }
    consola_imprimir_linea("tactil [estado|iniciar|probar|crudo]");
}

/* ------------------------------------------------------------------ comando i2c */

static uint32_t num(const char **p) {
    const char *s = *p; uint32_t v = 0;
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (s += 2;; s++) {
            int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + (uint32_t)d;
        }
    } else while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    *p = s;
    return v;
}

void i2c_comando(const char *arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg || igual(arg, "sondear")) { i2c_dw_sondear(); return; }
    if (arg[0] == 'l') {   /* i2c leer <bus> <dir> <registro de 16 bits> <n>: escribe el registro y lee */
        const char *p = arg; while (*p && *p != ' ') p++;
        uint32_t bus = num(&p), dir = num(&p), reg = num(&p), n = num(&p);
        if (bus > 5 || dir > 0x7F || !n || n > 64) { consola_imprimir_linea("  Uso: i2c leer <bus 0-5> <direccion> <registro> <bytes 1-64>"); return; }
        if (i2c_dw_iniciar(bus, 400)) { consola_imprimir_linea("  [I2C] El bus no arranca."); return; }
        uint8_t w[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) }, r[64];
        int k = i2c_dw_transferir((uint8_t)dir, w, 2, r, n, 200);
        if (k) { consola_imprimir("  [I2C] Sin respuesta, codigo "); consola_imprimir_dec((uint64_t)-k); consola_imprimir(" aborto="); consola_imprimir_hex(i2c_dw_aborto()); consola_imprimir_linea(""); return; }
        consola_imprimir("  [I2C]"); for (uint32_t i = 0; i < n; i++) { consola_imprimir(" "); hex8(r[i]); } consola_imprimir_linea("");
        return;
    }
    consola_imprimir_linea("i2c [sondear] | i2c leer <bus> <direccion> <registro> <bytes>");
}
