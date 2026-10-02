#include "usb_msc_telemetria.h"
#include "consola.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/tiempo.h"

// ============================================================================
// TAEK OS - TELEMETRIA FINA DE READ(10)
// ============================================================================

static usb_msc_telemetria g_usb_msc_telemetria;

void usb_msc_telemetria_reiniciar(void) {
    usb_msc_telem_reiniciar(&g_usb_msc_telemetria);
}

void usb_msc_telemetria_registrar(const uint64_t *fases, uint32_t bytes,
                                  uint64_t espera_usb_ciclos,
                                  uint64_t cpu_sondeo_ciclos,
                                  uint64_t cpu_timbre_ciclos,
                                  int ok) {
    uint64_t c_ms = tiempo_ciclos_por_ms();
    usb_msc_telem_anotar(&g_usb_msc_telemetria, fases, bytes,
                         espera_usb_ciclos, cpu_sondeo_ciclos,
                         cpu_timbre_ciclos, ok, c_ms, rdtsc());
}

const usb_msc_telemetria *usb_msc_telemetria_obtener(void) {
    return &g_usb_msc_telemetria;
}

// --- Emision dual (serial o consola) -----------------------------------------

static void emitir(const char *texto, int serial) {
    if (serial) serial_imprimir(texto);
    else consola_imprimir(texto);
}

static void emitir_dec(uint64_t valor, int serial) {
    if (serial) serial_imprimir_dec(valor);
    else consola_imprimir_dec(valor);
}

static void emitir_linea(const char *texto, int serial) {
    emitir(texto, serial);
    emitir("\n", serial);
}

// Imprime "entero.3decimales" a partir de microsegundos (es decir, ms).
static void emitir_ms(uint64_t us, int serial) {
    char buf[32];
    int n = 0;
    uint64_t entero = us / 1000;
    uint64_t frac = us % 1000;
    char tmp[24];
    int ti = 0;
    if (entero == 0) tmp[ti++] = '0';
    while (entero > 0) { tmp[ti++] = (char)('0' + (entero % 10)); entero /= 10; }
    for (int i = ti - 1; i >= 0; i--) buf[n++] = tmp[i];
    buf[n++] = '.';
    buf[n++] = (char)('0' + (frac / 100));
    buf[n++] = (char)('0' + ((frac / 10) % 10));
    buf[n++] = (char)('0' + (frac % 10));
    buf[n] = '\0';
    emitir(buf, serial);
}

// Imprime "entero.2decimales".
static void emitir_2dec(uint64_t valor_x100, int serial) {
    char buf[32];
    int n = 0;
    uint64_t entero = valor_x100 / 100;
    uint64_t frac = valor_x100 % 100;
    char tmp[24];
    int ti = 0;
    if (entero == 0) tmp[ti++] = '0';
    while (entero > 0) { tmp[ti++] = (char)('0' + (entero % 10)); entero /= 10; }
    for (int i = ti - 1; i >= 0; i--) buf[n++] = tmp[i];
    buf[n++] = '.';
    buf[n++] = (char)('0' + (frac / 10));
    buf[n++] = (char)('0' + (frac % 10));
    buf[n] = '\0';
    emitir(buf, serial);
}

static const char *g_nombres_fase[USB_MSC_FASE_CANTIDAD] = {
    "CBW", "DATA_INICIO", "DATA_RESTO", "CSW", "RETORNO", "TOTAL"
};

static void emitir_informe(int serial) {
    const usb_msc_telemetria *t = &g_usb_msc_telemetria;
    uint64_t c_ms = t->ciclos_por_ms ? t->ciclos_por_ms : tiempo_ciclos_por_ms();

    emitir_linea("=== TELEMETRIA FINA READ(10) [USB MSC BOT] ===", serial);

    if (t->comandos == 0) {
        emitir_linea("  Sin muestras registradas.", serial);
        emitir_linea("==============================================", serial);
        return;
    }

    emitir("  Muestras / OK / Desborde Histograma : ", serial);
    emitir_dec(t->comandos, serial);
    emitir(" / ", serial);
    emitir_dec(t->completados, serial);
    emitir(" / ", serial);
    emitir_dec(t->hist_desborde, serial);
    emitir_linea("", serial);

    emitir("  Bytes fisicos leidos                : ", serial);
    emitir_dec(t->bytes, serial);
    emitir(" (por comando min/medio/max ", serial);
    emitir_dec(t->bytes_min, serial);
    emitir(" / ", serial);
    emitir_dec(t->comandos ? t->bytes / t->comandos : 0, serial);
    emitir(" / ", serial);
    emitir_dec(t->bytes_max, serial);
    emitir_linea(")", serial);

    // Duracion del stream de muestras.
    uint64_t elapsed = (t->ciclos_ultimo > t->ciclos_inicio) ?
                       (t->ciclos_ultimo - t->ciclos_inicio) : 0;
    if (elapsed && c_ms) {
        uint64_t ms = elapsed / c_ms;
        emitir("  Ventana de muestreo                 : ", serial);
        emitir_ms(ms * 1000ULL, serial);
        emitir(" (comandos/s ", serial);
        emitir_2dec(ms ? (t->comandos * 100000ULL) / ms : 0, serial);
        emitir("; ", serial);
        emitir_dec(ms ? (t->bytes * 1000ULL) / ms : 0, serial);
        emitir_linea(" bytes/s)", serial);
    }

    emitir_linea("", serial);
    emitir_linea("  Descomposicion por fase (media / max / % del total):", serial);
    uint64_t media_total = usb_msc_telem_us(t->sum_ciclos[USB_MSC_FASE_TOTAL], c_ms) / t->comandos;
    for (unsigned f = 0; f < USB_MSC_FASE_CANTIDAD; f++) {
        uint64_t sum_us = usb_msc_telem_us(t->sum_ciclos[f], c_ms);
        uint64_t media_us = sum_us / t->comandos;
        uint64_t max_us = usb_msc_telem_us(t->max_ciclos[f], c_ms);
        emitir("    ", serial);
        emitir(g_nombres_fase[f], serial);
        emitir(": media ", serial);
        emitir_ms(media_us, serial);
        emitir("  max ", serial);
        emitir_ms(max_us, serial);
        emitir("  ", serial);
        emitir_2dec(media_total ? (media_us * 10000ULL) / media_total : 0, serial);
        emitir_linea(" %", serial);
    }

    emitir_linea("", serial);
    emitir_linea("  Latencia total (histograma 50 us):", serial);
    uint64_t p50 = usb_msc_telem_percentil_us(t, 50);
    uint64_t p95 = usb_msc_telem_percentil_us(t, 95);
    uint64_t p99 = usb_msc_telem_percentil_us(t, 99);
    emitir("    media ", serial);
    emitir_ms(media_total, serial);
    emitir("  p50 ", serial);
    emitir_ms(p50, serial);
    emitir("  p95 ", serial);
    emitir_ms(p95, serial);
    emitir("  p99 ", serial);
    emitir_ms(p99, serial);
    emitir("  max ", serial);
    emitir_ms(usb_msc_telem_us(t->max_ciclos[USB_MSC_FASE_TOTAL], c_ms), serial);
    emitir_linea("", serial);

    emitir_linea("", serial);
    emitir_linea("  Atribucion CPU vs espera USB:", serial);
    uint64_t espera_us = usb_msc_telem_us(t->sum_ciclos_espera_usb, c_ms);
    uint64_t total_us = usb_msc_telem_us(t->sum_ciclos[USB_MSC_FASE_TOTAL], c_ms);
    uint64_t cpu_us = (total_us > espera_us) ? (total_us - espera_us) : 0;
    uint64_t sondeo_us = usb_msc_telem_us(t->sum_ciclos_cpu_sondeo, c_ms);
    uint64_t timbre_us = usb_msc_telem_us(t->sum_ciclos_cpu_timbre, c_ms);
    uint64_t resto_us = (cpu_us > sondeo_us + timbre_us) ?
                        (cpu_us - sondeo_us - timbre_us) : 0;
    emitir("    espera USB (CPU ociosa): media ", serial);
    emitir_ms(t->comandos ? espera_us / t->comandos : 0, serial);
    emitir("  ", serial);
    emitir_2dec(total_us ? (espera_us * 10000ULL) / total_us : 0, serial);
    emitir_linea(" %", serial);
    emitir("    CPU util               : media ", serial);
    emitir_ms(t->comandos ? cpu_us / t->comandos : 0, serial);
    emitir("  ", serial);
    emitir_2dec(total_us ? (cpu_us * 10000ULL) / total_us : 0, serial);
    emitir_linea(" %", serial);
    emitir("      - sondeo xHCI         : media ", serial);
    emitir_ms(t->comandos ? sondeo_us / t->comandos : 0, serial);
    emitir_linea("", serial);
    emitir("      - timbre/doorbell     : media ", serial);
    emitir_ms(t->comandos ? timbre_us / t->comandos : 0, serial);
    emitir_linea("", serial);
    emitir("      - resto (copias/valid): media ", serial);
    emitir_ms(t->comandos ? resto_us / t->comandos : 0, serial);
    emitir_linea("", serial);

    // Criterio de cierre: la suma de fases en ciclos debe igualar la fase TOTAL.
    uint64_t suma_fases = 0;
    for (unsigned f = 0; f < USB_MSC_FASE_TOTAL; f++) suma_fases += t->sum_ciclos[f];
    uint64_t total_ciclos = t->sum_ciclos[USB_MSC_FASE_TOTAL];
    uint64_t faltante = (total_ciclos > suma_fases) ? (total_ciclos - suma_fases) : 0;
    emitir_linea("", serial);
    emitir("  Cobertura de fases (suma vs total): ", serial);
    emitir_dec(suma_fases, serial);
    emitir(" / ", serial);
    emitir_dec(total_ciclos, serial);
    emitir(" ciclos; faltante ", serial);
    emitir_dec(faltante, serial);
    emitir_linea(faltante == 0 ? " [SIN ZONAS DESCONOCIDAS]" : " [REVISAR]", serial);

    emitir_linea("==============================================", serial);
}

void usb_msc_telemetria_volcar_serial(void) {
    const usb_msc_telemetria *t = &g_usb_msc_telemetria;
    uint64_t c_ms = t->ciclos_por_ms ? t->ciclos_por_ms : tiempo_ciclos_por_ms();

    // Cabecera con recuentos crudos.
    serial_imprimir("[USB_READ10] MUESTRAS=");   serial_imprimir_dec(t->comandos);
    serial_imprimir(" OK=");                      serial_imprimir_dec(t->completados);
    serial_imprimir(" BYTES=");                   serial_imprimir_dec(t->bytes);
    serial_imprimir(" MIN_BYTES=");               serial_imprimir_dec(t->bytes_min);
    serial_imprimir(" MAX_BYTES=");               serial_imprimir_dec(t->bytes_max);
    serial_imprimir(" DESBORDE=");                serial_imprimir_dec(t->hist_desborde);
    serial_imprimir(" TSC_CICLOS_MS=");           serial_imprimir_dec(c_ms);
    uint64_t elapsed = (t->ciclos_ultimo > t->ciclos_inicio) ?
                       (t->ciclos_ultimo - t->ciclos_inicio) : 0;
    serial_imprimir(" DURACION_MS=");             serial_imprimir_dec(c_ms ? elapsed / c_ms : 0);
    serial_imprimir_linea("");

    if (t->comandos == 0) return;

    // Fase por fase en microsegundos (para dividir por MUESTRAS).
    for (unsigned f = 0; f < USB_MSC_FASE_CANTIDAD; f++) {
        serial_imprimir("[USB_READ10] FASE=");    serial_imprimir(g_nombres_fase[f]);
        serial_imprimir(" MEDIA_US=");            serial_imprimir_dec(usb_msc_telem_us(t->sum_ciclos[f], c_ms) / t->comandos);
        serial_imprimir(" MAX_US=");              serial_imprimir_dec(usb_msc_telem_us(t->max_ciclos[f], c_ms));
        serial_imprimir(" SUM_US=");              serial_imprimir_dec(usb_msc_telem_us(t->sum_ciclos[f], c_ms));
        serial_imprimir_linea("");
    }

    uint64_t total_us = usb_msc_telem_us(t->sum_ciclos[USB_MSC_FASE_TOTAL], c_ms);
    uint64_t media_us = total_us / t->comandos;
    serial_imprimir("[USB_READ10] LATENCIA_US MEDIA="); serial_imprimir_dec(media_us);
    serial_imprimir(" P50=");   serial_imprimir_dec(usb_msc_telem_percentil_us(t, 50));
    serial_imprimir(" P95=");   serial_imprimir_dec(usb_msc_telem_percentil_us(t, 95));
    serial_imprimir(" P99=");   serial_imprimir_dec(usb_msc_telem_percentil_us(t, 99));
    serial_imprimir(" MAX=");   serial_imprimir_dec(usb_msc_telem_us(t->max_ciclos[USB_MSC_FASE_TOTAL], c_ms));
    serial_imprimir_linea("");

    uint64_t ms = c_ms ? elapsed / c_ms : 0;
    serial_imprimir("[USB_READ10] TASA COMANDOS_S_MILES=");
    serial_imprimir_dec(ms ? (t->comandos * 1000000ULL) / ms : 0);
    serial_imprimir(" BYTES_S=");           serial_imprimir_dec(ms ? (t->bytes * 1000ULL) / ms : 0);
    serial_imprimir(" BYTES_COMANDO=");     serial_imprimir_dec(t->bytes / t->comandos);
    serial_imprimir_linea("");

    uint64_t espera_us = usb_msc_telem_us(t->sum_ciclos_espera_usb, c_ms);
    uint64_t cpu_us = (total_us > espera_us) ? (total_us - espera_us) : 0;
    uint64_t sondeo_us = usb_msc_telem_us(t->sum_ciclos_cpu_sondeo, c_ms);
    uint64_t timbre_us = usb_msc_telem_us(t->sum_ciclos_cpu_timbre, c_ms);
    uint64_t resto_us = (cpu_us > sondeo_us + timbre_us) ?
                        (cpu_us - sondeo_us - timbre_us) : 0;
    serial_imprimir("[USB_READ10] ATRIBUCION ESPERA_USB_US="); serial_imprimir_dec(espera_us);
    serial_imprimir(" CPU_US=");        serial_imprimir_dec(cpu_us);
    serial_imprimir(" CPU_SONDEO_US="); serial_imprimir_dec(sondeo_us);
    serial_imprimir(" CPU_TIMBRE_US="); serial_imprimir_dec(timbre_us);
    serial_imprimir(" CPU_RESTO_US=");  serial_imprimir_dec(resto_us);
    serial_imprimir_linea("");

    uint64_t suma_fases = 0;
    for (unsigned f = 0; f < USB_MSC_FASE_TOTAL; f++) suma_fases += t->sum_ciclos[f];
    uint64_t total_ciclos = t->sum_ciclos[USB_MSC_FASE_TOTAL];
    serial_imprimir("[USB_READ10] COBERTURA SUMA_CICLOS="); serial_imprimir_dec(suma_fases);
    serial_imprimir(" TOTAL_CICLOS="); serial_imprimir_dec(total_ciclos);
    serial_imprimir(" FALTANTE_CICLOS=");
    serial_imprimir_dec(total_ciclos > suma_fases ? total_ciclos - suma_fases : 0);
    serial_imprimir_linea("");
}

void usb_msc_telemetria_imprimir_consola(void) {
    emitir_informe(0);
}
