#ifndef CONTROLADORES_USB_MSC_TELEMETRIA_H
#define CONTROLADORES_USB_MSC_TELEMETRIA_H

#include <stdint.h>

// ============================================================================
// TAEK OS - TELEMETRIA FINA DE READ(10) SOBRE USB MSC (BOT)
// Descompone cada comando en fases medidas con TSC y agrega percentiles para
// explicar la duracion real sin zonas temporales desconocidas.
// ============================================================================

// Fases del ciclo BOT de un READ(10). El orden del enum fija el orden temporal
// y la suma de las fases 0..4 es exactamente la fase TOTAL.
enum usb_msc_fase {
    USB_MSC_FASE_CBW = 0,       // submit CBW -> CBW completado
    USB_MSC_FASE_DATA_INICIO,   // CBW completado -> primer DATA recibido
    USB_MSC_FASE_DATA_RESTO,    // primer DATA -> DATA completado (incluye copia)
    USB_MSC_FASE_CSW,           // DATA completado -> CSW recibido
    USB_MSC_FASE_RETORNO,       // CSW recibido -> retorno al caller (validacion)
    USB_MSC_FASE_TOTAL,         // submit CBW -> retorno al caller
    USB_MSC_FASE_CANTIDAD
};

// Histograma de latencia total: USB_MSC_TELEM_CUBOS cubos de 50 us (204.8 ms).
#define USB_MSC_TELEM_CUBOS      4096
#define USB_MSC_TELEM_ANCHO_US   50

typedef struct {
    uint64_t comandos;              // READ(10) instrumentados
    uint64_t completados;           // transacciones BOT que retornaron 0
    uint64_t bytes;                 // bytes fisicos leidos (fase IN)
    uint32_t bytes_min;             // menor transferencia por comando
    uint32_t bytes_max;             // mayor transferencia por comando
    uint64_t ciclos_inicio;         // TSC de la primera muestra
    uint64_t ciclos_ultimo;         // TSC de la ultima muestra
    uint64_t ciclos_por_ms;         // calibracion TSC usada al registrar

    uint64_t sum_ciclos[USB_MSC_FASE_CANTIDAD];
    uint64_t max_ciclos[USB_MSC_FASE_CANTIDAD];

    // Atribucion ortogonal a las fases: espera pasiva del dispositivo (bucle
    // esperar_milisegundos -> CPU ociosa) frente a CPU util (sondeo MMIO,
    // timbre/doorbell y trabajo del resto del camino).
    uint64_t sum_ciclos_espera_usb;
    uint64_t max_ciclos_espera_usb;
    uint64_t sum_ciclos_cpu_sondeo;
    uint64_t sum_ciclos_cpu_timbre;

    uint32_t hist[USB_MSC_TELEM_CUBOS];
    uint64_t hist_total;
    uint32_t hist_desborde;         // muestras >= USB_MSC_TELEM_CUBOS*ancho
} usb_msc_telemetria;

static inline uint64_t usb_msc_telem_us(uint64_t ciclos, uint64_t c_ms) {
    if (c_ms == 0) return ciclos;
    return (ciclos * 1000ULL) / c_ms;
}

static inline void usb_msc_telem_reiniciar(usb_msc_telemetria *t) {
    if (!t) return;
    uint8_t *bytes = (uint8_t *)t;
    for (unsigned i = 0; i < (unsigned)sizeof(*t); i++) bytes[i] = 0;
}

// Registra una muestra. `ahora` es el TSC del retorno al caller.
static inline void usb_msc_telem_anotar(usb_msc_telemetria *t,
        const uint64_t *fases, uint32_t bytes, uint64_t espera_usb,
        uint64_t cpu_sondeo, uint64_t cpu_timbre, int ok, uint64_t c_ms,
        uint64_t ahora) {
    if (!t || !fases) return;
    if (t->comandos == 0) t->ciclos_inicio = ahora;
    t->ciclos_ultimo = ahora;
    t->comandos++;
    if (ok) t->completados++;
    t->bytes += bytes;
    if (t->comandos == 1) { t->bytes_min = bytes; t->bytes_max = bytes; }
    else { if (bytes < t->bytes_min) t->bytes_min = bytes;
           if (bytes > t->bytes_max) t->bytes_max = bytes; }
    t->ciclos_por_ms = c_ms;

    t->sum_ciclos_espera_usb += espera_usb;
    if (espera_usb > t->max_ciclos_espera_usb) t->max_ciclos_espera_usb = espera_usb;
    t->sum_ciclos_cpu_sondeo += cpu_sondeo;
    t->sum_ciclos_cpu_timbre += cpu_timbre;

    for (unsigned f = 0; f < USB_MSC_FASE_CANTIDAD; f++) {
        t->sum_ciclos[f] += fases[f];
        if (fases[f] > t->max_ciclos[f]) t->max_ciclos[f] = fases[f];
    }

    // El histograma describe la latencia percibida por el caller (fase TOTAL).
    uint64_t us = usb_msc_telem_us(fases[USB_MSC_FASE_TOTAL], c_ms);
    uint64_t cubo = us / USB_MSC_TELEM_ANCHO_US;
    if (cubo < USB_MSC_TELEM_CUBOS) t->hist[cubo]++;
    else t->hist_desborde++;
    t->hist_total++;
}

// Percentil p (0..100) en microsegundos, extremo superior del cubo hallado.
static inline uint64_t usb_msc_telem_percentil_us(const usb_msc_telemetria *t, unsigned p) {
    if (!t || t->hist_total == 0) return 0;
    if (p > 100) p = 100;
    uint64_t objetivo = (t->hist_total * p + 99) / 100;
    if (objetivo == 0) objetivo = 1;
    uint64_t acumulado = 0;
    for (uint32_t i = 0; i < USB_MSC_TELEM_CUBOS; i++) {
        acumulado += t->hist[i];
        if (acumulado >= objetivo) {
            return ((uint64_t)i + 1) * USB_MSC_TELEM_ANCHO_US;
        }
    }
    return (uint64_t)USB_MSC_TELEM_CUBOS * USB_MSC_TELEM_ANCHO_US;
}

// --- API PUBLICA ---

// Borra todos los agregados (llamar al comenzar una sesion de medicion).
void usb_msc_telemetria_reiniciar(void);

// Registra un READ(10) completo. `fases` tiene USB_MSC_FASE_CANTIDAD ciclos.
void usb_msc_telemetria_registrar(const uint64_t *fases, uint32_t bytes,
                                  uint64_t espera_usb_ciclos,
                                  uint64_t cpu_sondeo_ciclos,
                                  uint64_t cpu_timbre_ciclos,
                                  int ok);

const usb_msc_telemetria *usb_msc_telemetria_obtener(void);

// Vuelca el informe estructurado [USB_READ10] al puerto serial.
void usb_msc_telemetria_volcar_serial(void);

// Presenta el informe legible en la consola grafica interactiva.
void usb_msc_telemetria_imprimir_consola(void);

#endif // CONTROLADORES_USB_MSC_TELEMETRIA_H
