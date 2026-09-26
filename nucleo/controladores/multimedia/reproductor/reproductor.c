#include "reproductor.h"
#include "controladores/multimedia/mp4/mp4.h"
#include "controladores/multimedia/h264/h264.h"
#include "controladores/multimedia/aac/aac.h"
#include "boot/limine/limine.h"
#include "base/memoria.h"
#include "base/tiempo.h"
#include "base/version.h"
#include "arquitectura/x86_64/serial.h"
#include "controladores/consola.h"
#include "controladores/pantalla.h"
#include "controladores/teclado.h"
#include "controladores/xhci.h"
#include "controladores/audio_ac97.h"

// ============================================================================
// TAEK OS - REPRODUCTOR MULTIMEDIA H.264 & AAC (Hito A: Telemetría y Benchmark)
// Contrato Común de Medición: Ciclos, Tiempos, Percentiles y Desglose por Etapa
// ============================================================================

__attribute__((used, section(".requests")))
static volatile struct limine_module_request peticion_video = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

// Modos de ejecución soportados
enum modo_reproduccion {
    MODO_INTERACTIVO   = 0, // Video + Audio + Presentación 100% + Sincronización PTS
    MODO_PRUEBA_FORENSE = 1, // Hash FNV-1a en 100% de cuadros, presentación 1/100, sin espera PTS
    MODO_BENCHMARK     = 2  // Rendimiento pico de hardware: presentación 100%, sin espera PTS
};

typedef struct {
    // Ciclos acumulados por etapa
    uint64_t ciclos_demux;
    uint64_t ciclos_aac;
    uint64_t ciclos_cabac_reconstruccion;
    uint64_t ciclos_cabac_puro;
    uint64_t ciclos_inter;
    uint64_t ciclos_intra;
    uint64_t ciclos_desbloqueo;
    uint64_t ciclos_yuv_rgb;
    uint64_t ciclos_copia_fb;
    uint64_t ciclos_espera_pts;
    uint64_t ciclos_hda_usb;
    uint64_t ciclos_hash;

    // Máximos ciclos individuales por cuadro
    uint64_t max_ciclos_demux;
    uint64_t max_ciclos_aac;
    uint64_t max_ciclos_cabac;
    uint64_t max_ciclos_desbloqueo;
    uint64_t max_ciclos_yuv_rgb;
    uint64_t max_ciclos_copia_fb;
    uint64_t max_ciclos_espera_pts;
    uint64_t max_ciclos_hda_usb;
    uint64_t max_ciclos_hash;

    // Métricas de flujo y cuadros
    uint32_t cuadros_decodificados;
    uint32_t cuadros_presentados;
    uint32_t cuadros_omitidos;

    // Retraso frente a PTS
    uint64_t retraso_pts_acumulado_us;
    uint64_t retraso_pts_max_us;

    // Avance DMA y audio
    uint64_t avance_dma_bytes;
    uint32_t vaciados_audio;

    // Registro de latencia por cuadro para cálculo de percentiles
    uint32_t *tiempos_cuadro_us;
    uint32_t total_tiempos_grabados;
    uint32_t capacidad_tiempos;
} telemetria_reproductor;

typedef struct {
    unsigned cuadros, ancho, alto, escala;
    enum modo_reproduccion modo;
    int cancelado, error;
    int64_t primer_pts;
    uint64_t inicio, huella;
    size_t memoria_usada, memoria_maxima;
    uint32_t *rgb;
    telemetria_reproductor telem;
    uint64_t t_inicio_cuadro_ciclos;
    uint64_t ciclos_espera_pts_ultimo_cuadro;
} reproductor;

typedef struct {
    size_t bytes;
    uint64_t reservado;
} cabecera_reserva;

static int iguales(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void *reservar(void *usuario, size_t bytes) {
    reproductor *p = (reproductor *)usuario;
    if (p && bytes > 256u * 1024 * 1024 - p->memoria_usada) return NULL;
    cabecera_reserva *c = asignar_memoria(sizeof(*c) + bytes);
    if (!c) return NULL;
    c->bytes = bytes;
    c->reservado = 0;
    if (p) {
        p->memoria_usada += bytes;
        if (p->memoria_usada > p->memoria_maxima) p->memoria_maxima = p->memoria_usada;
    }
    return c + 1;
}

static void soltar(void *usuario, void *memoria) {
    if (!memoria) return;
    reproductor *p = (reproductor *)usuario;
    cabecera_reserva *c = (cabecera_reserva *)memoria - 1;
    if (p) p->memoria_usada -= c->bytes;
    liberar_memoria(c);
}

static int atender(reproductor *p) {
    uint64_t t0 = rdtsc();
    xhci_sondeo();
    audio_ac97_actualizar();
    while (teclado_hay_datos()) {
        if (teclado_leer_caracter() == 27) {
            if (p) p->cancelado = 1;
        }
    }
    uint64_t dt = rdtsc() - t0;
    if (p) {
        p->telem.ciclos_hda_usb += dt;
        if (dt > p->telem.max_ciclos_hda_usb) p->telem.max_ciclos_hda_usb = dt;
    }
    return p ? p->cancelado : 0;
}

// Detección de CPU y conteo de hilos lógicos vía CPUID
static void obtener_info_cpu(char *marca_cpu, uint32_t tam_marca, uint32_t *hilos_detectados) {
    uint32_t eax, ebx, ecx, edx;

    if (hilos_detectados) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        *hilos_detectados = (ebx >> 16) & 0xFF;
        if (*hilos_detectados == 0) *hilos_detectados = 1;
    }

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000));
    if (eax >= 0x80000004 && tam_marca >= 48) {
        uint32_t *dst = (uint32_t *)marca_cpu;
        for (uint32_t i = 0; i < 3; i++) {
            __asm__ volatile ("cpuid" : "=a"(dst[0]), "=b"(dst[1]), "=c"(dst[2]), "=d"(dst[3]) : "a"(0x80000002 + i));
            dst += 4;
        }
        marca_cpu[48] = '\0';
        char *src = marca_cpu;
        while (*src == ' ') src++;
        if (src != marca_cpu) {
            int k = 0;
            while (src[k]) { marca_cpu[k] = src[k]; k++; }
            marca_cpu[k] = '\0';
        }
    } else {
        const char *def = "x86_64 Compatible CPU";
        int k = 0;
        while (def[k] && k < (int)tam_marca - 1) { marca_cpu[k] = def[k]; k++; }
        marca_cpu[k] = '\0';
    }
}

// Algoritmo Quicksort in-place para cálculo exacto de percentiles (p50, p95, p99)
static void ordenar_tiempos_cuadro(uint32_t *arr, int izq, int der) {
    if (izq >= der) return;
    uint32_t pivote = arr[(izq + der) / 2];
    int i = izq, j = der;
    while (i <= j) {
        while (arr[i] < pivote) i++;
        while (arr[j] > pivote) j--;
        if (i <= j) {
            uint32_t tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
            i++;
            j--;
        }
    }
    if (izq < j) ordenar_tiempos_cuadro(arr, izq, j);
    if (i < der) ordenar_tiempos_cuadro(arr, i, der);
}

// Utilidades de impresión dual (Pantalla GOP + Salida Serial COM1 vía consola)
static void telem_print(const char *txt) {
    consola_imprimir(txt);
}

static void telem_println(const char *txt) {
    consola_imprimir_linea(txt);
}

static void telem_print_dec(uint64_t val) {
    consola_imprimir_dec(val);
}

static void telem_print_hex(uint64_t val) {
    consola_imprimir_hex(val);
}

static void telem_print_ms(uint64_t ciclos, uint64_t c_ms) {
    if (c_ms == 0) c_ms = 2500000;
    uint64_t ms = ciclos / c_ms;
    uint64_t dec = ((ciclos % c_ms) * 100) / c_ms;
    telem_print_dec(ms);
    telem_print(".");
    if (dec < 10) telem_print("0");
    telem_print_dec(dec);
}

static void telem_print_porcentaje(uint64_t parte, uint64_t total) {
    if (total == 0) { telem_print("  0.00%"); return; }
    uint64_t p_int = (parte * 100) / total;
    uint64_t p_dec = ((parte * 10000) / total) % 100;
    if (p_int < 10) telem_print(" ");
    telem_print_dec(p_int);
    telem_print(".");
    if (p_dec < 10) telem_print("0");
    telem_print_dec(p_dec);
    telem_print("%");
}

static void telem_print_fila_etapa(const char *etiqueta, uint64_t ciclos, uint64_t max_ciclos,
                                  uint64_t total_ciclos_activos, uint64_t c_ms) {
    telem_print("  ");
    telem_print(etiqueta);
    // Relleno hasta columna 28
    int len = 0;
    while (etiqueta[len]) len++;
    for (int k = len; k < 26; k++) telem_print(" ");
    telem_print("|  ");

    // Ciclos (en millones si es grande)
    uint64_t c_millones = ciclos / 1000000ULL;
    if (c_millones > 0) {
        if (c_millones < 1000) telem_print("    ");
        else if (c_millones < 10000) telem_print("   ");
        else if (c_millones < 100000) telem_print("  ");
        telem_print_dec(c_millones);
        telem_print(" M");
    } else {
        telem_print("      <1 M");
    }
    telem_print(" | ");

    // Tiempo ms
    telem_print_ms(ciclos, c_ms);
    telem_print(" ms | ");

    // Porcentaje del camino activo
    if (total_ciclos_activos > 0) {
        telem_print_porcentaje(ciclos, total_ciclos_activos);
    } else {
        telem_print("   ---  ");
    }
    telem_print(" | ");

    // Máximo en un cuadro
    telem_print_ms(max_ciclos, c_ms);
    telem_println(" ms");
}

static int presentar(void *usuario, const h264_imagen *im) {
    reproductor *p = (reproductor *)usuario;
    if (atender(p)) return 1;

    if (!p->cuadros) {
        p->primer_pts = im->marca_tiempo;
        p->inicio = tiempo_obtener_milisegundos();
        audio_ac97_reiniciar_reloj();
    }

    p->ciclos_espera_pts_ultimo_cuadro = 0;

    // 1. Espera de sincronización PTS (solo en MODO_INTERACTIVO)
    if (p->modo == MODO_INTERACTIVO) {
        int64_t delta = im->marca_tiempo - p->primer_pts;
        if (delta < 0 || (uint64_t)delta / p->escala > 86400) {
            p->error = 1;
            return 1;
        }
        uint64_t destino = (uint64_t)delta * 1000 / p->escala;
        uint64_t ahora = audio_ac97_esta_reproduciendo() ?
                         audio_ac97_obtener_tiempo_ms() :
                         (tiempo_obtener_milisegundos() - p->inicio);

        if (ahora < destino) {
            uint64_t t_pts0 = rdtsc();
            while (1) {
                uint64_t t_act = audio_ac97_esta_reproduciendo() ?
                                 audio_ac97_obtener_tiempo_ms() :
                                 (tiempo_obtener_milisegundos() - p->inicio);
                if (t_act >= destino) break;
                if (atender(p)) return 1;
                esperar_microsegundos(200);
            }
            uint64_t dt_pts = rdtsc() - t_pts0;
            p->telem.ciclos_espera_pts += dt_pts;
            if (dt_pts > p->telem.max_ciclos_espera_pts) p->telem.max_ciclos_espera_pts = dt_pts;
            p->ciclos_espera_pts_ultimo_cuadro = dt_pts;
        } else if (ahora > destino) {
            uint64_t retraso_us = (ahora - destino) * 1000;
            p->telem.retraso_pts_acumulado_us += retraso_us;
            if (retraso_us > p->telem.retraso_pts_max_us) p->telem.retraso_pts_max_us = retraso_us;
        }
    }

    // 2. Cálculo de huella FNV-1a (solo en MODO_PRUEBA_FORENSE)
    if (p->modo == MODO_PRUEBA_FORENSE) {
        uint64_t t_h0 = rdtsc();
        p->huella = h264_huella(p->huella, im);
        uint64_t dt_h = rdtsc() - t_h0;
        p->telem.ciclos_hash += dt_h;
        if (dt_h > p->telem.max_ciclos_hash) p->telem.max_ciclos_hash = dt_h;
    }

    // 3. Conversión de Color YUV->RGB y Copia al Framebuffer
    int debe_dibujar = (p->modo != MODO_PRUEBA_FORENSE) || (p->cuadros % 100 == 0);

    if (debe_dibujar) {
        unsigned w = im->ancho, h = im->alto;
        uint64_t sw = pantalla_obtener_ancho(), sh = pantalla_obtener_alto();
        if (!sw || !sh) { p->error = 1; return 1; }
        if (w > sw) { h = (unsigned)((uint64_t)h * sw / w); w = (unsigned)sw; }
        if (h > sh) { w = (unsigned)((uint64_t)w * sh / h); h = (unsigned)sh; }
        if (!w || !h) { p->error = 1; return 1; }

        if (p->ancho != w || p->alto != h) {
            if (p->rgb) soltar(p, p->rgb);
            p->rgb = reservar(p, (size_t)w * h * 4);
            p->ancho = w;
            p->alto = h;
            if (!p->rgb) { p->error = 1; return 1; }
            pantalla_limpiar(0);
        }

        // Medición de conversión YUV -> RGB
        uint64_t t_rgb0 = rdtsc();
        h264_convertir_rgb(im, p->rgb, w, h);
        uint64_t dt_rgb = rdtsc() - t_rgb0;
        p->telem.ciclos_yuv_rgb += dt_rgb;
        if (dt_rgb > p->telem.max_ciclos_yuv_rgb) p->telem.max_ciclos_yuv_rgb = dt_rgb;

        // Medición de copia al framebuffer
        uint64_t t_fb0 = rdtsc();
        pantalla_dibujar_imagen_centrada((int)w, (int)h, p->rgb);
        uint64_t dt_fb = rdtsc() - t_fb0;
        p->telem.ciclos_copia_fb += dt_fb;
        if (dt_fb > p->telem.max_ciclos_copia_fb) p->telem.max_ciclos_copia_fb = dt_fb;

        p->telem.cuadros_presentados++;
    } else {
        p->telem.cuadros_omitidos++;
    }

    // 4. Registro del camino crítico del cuadro (en microsegundos)
    uint64_t t_ahora = rdtsc();
    if (p->t_inicio_cuadro_ciclos > 0 && t_ahora > p->t_inicio_cuadro_ciclos) {
        uint64_t dur_ciclos = t_ahora - p->t_inicio_cuadro_ciclos;
        if (dur_ciclos > p->ciclos_espera_pts_ultimo_cuadro) {
            dur_ciclos -= p->ciclos_espera_pts_ultimo_cuadro;
        }
        uint64_t c_ms = tiempo_ciclos_por_ms();
        if (c_ms == 0) c_ms = 2500000;
        uint32_t dur_us = (uint32_t)((dur_ciclos * 1000) / c_ms);
        if (p->telem.tiempos_cuadro_us && p->telem.total_tiempos_grabados < p->telem.capacidad_tiempos) {
            p->telem.tiempos_cuadro_us[p->telem.total_tiempos_grabados++] = dur_us;
        }
    }
    p->t_inicio_cuadro_ciclos = rdtsc();
    p->cuadros++;

    return 0;
}

static void imprimir_informe_benchmark(reproductor *p, h264_decodificador *dec, uint64_t duracion_total_ms) {
    uint64_t c_ms = tiempo_ciclos_por_ms();
    if (c_ms == 0) c_ms = 2500000;
    uint64_t tsc_mhz = c_ms / 1000;

    char cpu_str[64];
    uint32_t hilos_cpu = 1;
    obtener_info_cpu(cpu_str, sizeof(cpu_str), &hilos_cpu);

    // Obtener telemetría de reconstrucción y desbloqueo del decodificador H.264
    h264_telemetria ht;
    h264_obtener_telemetria(dec, &ht);
    p->telem.ciclos_cabac_reconstruccion = ht.ciclos_sintaxis_reconstruccion;
    p->telem.max_ciclos_cabac = ht.max_ciclos_sintaxis;
    p->telem.ciclos_cabac_puro = ht.ciclos_cabac_puro;
    p->telem.ciclos_inter = ht.ciclos_inter;
    p->telem.ciclos_intra = ht.ciclos_intra;
    p->telem.ciclos_desbloqueo = ht.ciclos_desbloqueo;
    p->telem.max_ciclos_desbloqueo = ht.max_ciclos_desbloqueo;
    p->telem.cuadros_decodificados = (uint32_t)ht.cuadros_decodificados;

    // Calcular percentiles de camino crítico
    uint32_t p50_us = 0, p95_us = 0, p99_us = 0, prom_us = 0;
    if (p->telem.total_tiempos_grabados > 0) {
        ordenar_tiempos_cuadro(p->telem.tiempos_cuadro_us, 0, (int)p->telem.total_tiempos_grabados - 1);
        p50_us = p->telem.tiempos_cuadro_us[p->telem.total_tiempos_grabados * 50 / 100];
        p95_us = p->telem.tiempos_cuadro_us[p->telem.total_tiempos_grabados * 95 / 100];
        p99_us = p->telem.tiempos_cuadro_us[p->telem.total_tiempos_grabados * 99 / 100];
        uint64_t suma = 0;
        for (uint32_t k = 0; k < p->telem.total_tiempos_grabados; k++) suma += p->telem.tiempos_cuadro_us[k];
        prom_us = (uint32_t)(suma / p->telem.total_tiempos_grabados);
    }

    // Calcular suma total de ciclos activos (excluyendo espera pasiva de PTS)
    uint64_t ciclos_activos = p->telem.ciclos_demux +
                              p->telem.ciclos_aac +
                              p->telem.ciclos_cabac_reconstruccion +
                              p->telem.ciclos_desbloqueo +
                              p->telem.ciclos_yuv_rgb +
                              p->telem.ciclos_copia_fb +
                              p->telem.ciclos_hda_usb +
                              p->telem.ciclos_hash;

    // Emisión del reporte estructurado tanto a pantalla como a serial
    telem_println("");
    telem_println("================== INFORME DE TELEMETRÍA Y RENDIMIENTO TAEK OS ==================");
    telem_print("  Versión del Kernel    : TAEK OS 0.5.0-x86_64 (Compilado: ");
    telem_print(taek_obtener_fecha_compilacion());
    telem_print(" ");
    telem_print(taek_obtener_hora_compilacion());
    telem_println(")");

    telem_print("  Procesador Detectado  : ");
    telem_print(cpu_str);
    telem_print(" (");
    telem_print_dec(hilos_cpu);
    telem_println(" hilos lógicos detectados)");

    telem_print("  Contador TSC          : ");
    telem_print_dec(tsc_mhz);
    telem_print(" MHz calibrado (");
    telem_print_dec(c_ms);
    telem_println(" ciclos/ms)");

    telem_print("  Resolución Pantalla   : ");
    telem_print_dec(pantalla_obtener_ancho());
    telem_print("x");
    telem_print_dec(pantalla_obtener_alto());
    telem_println(" (Framebuffer lineal GOP)");

    telem_print("  Resolución del Video  : 640x360 @ 30 fps (Formato: H.264 Main + AAC)");
    telem_println("");

    telem_print("  Modo de Ejecución     : ");
    if (p->modo == MODO_PRUEBA_FORENSE) {
        telem_println("[MODO FORENSE: HASH YUV 100% + PRESENTACIÓN CADA 100 CUADROS]");
    } else if (p->modo == MODO_BENCHMARK) {
        telem_println("[MODO BENCHMARK: RENDIMIENTO MÁXIMO DE HARDWARE (SIN ESPERA POR PTS)]");
    } else {
        telem_println("[MODO INTERACTIVO: VIDEO H.264 + AUDIO AAC + SINCRONIZACIÓN PTS]");
    }
    telem_println("---------------------------------------------------------------------------------");

    telem_println("RESUMEN DE TIEMPO Y CUADROS:");
    telem_print("  Duración Real         : ");
    telem_print_dec(duracion_total_ms);
    telem_println(" ms");

    telem_print("  Cuadros Decodificados : ");
    telem_print_dec(p->telem.cuadros_decodificados);
    telem_println("");

    telem_print("  Cuadros Presentados   : ");
    telem_print_dec(p->telem.cuadros_presentados);
    telem_print(" (");
    if (p->telem.cuadros_decodificados > 0) {
        telem_print_porcentaje(p->telem.cuadros_presentados, p->telem.cuadros_decodificados);
    } else {
        telem_print("0.00%");
    }
    telem_println(")");

    telem_print("  Cuadros Omitidos      : ");
    telem_print_dec(p->telem.cuadros_omitidos);
    telem_println("");

    if (duracion_total_ms > 0) {
        uint64_t fps_ent = (uint64_t)p->telem.cuadros_decodificados * 1000 / duracion_total_ms;
        uint64_t fps_dec = ((uint64_t)p->telem.cuadros_decodificados * 100000 / duracion_total_ms) % 100;
        telem_print("  Rendimiento Efectivo  : ");
        telem_print_dec(fps_ent);
        telem_print(".");
        if (fps_dec < 10) telem_print("0");
        telem_print_dec(fps_dec);
        telem_println(" FPS");
    }

    if (p->modo == MODO_PRUEBA_FORENSE) {
        telem_print("  Huella YUV FNV-1a     : ");
        telem_print_hex(p->huella);
        if (p->huella == UINT64_C(0x984a4460415d1b1c)) {
            telem_println(" [COINCIDENCIA EXACTA CON REFERENCIA ITU-T / FFMPEG]");
        } else {
            telem_println(" [AVISO: DISCREPANCIA CON REFERENCIA 0x984A4460415D1B1C]");
        }
    }
    telem_println("---------------------------------------------------------------------------------");

    telem_println("DESGLOSE DE TIEMPOS POR ETAPA (NO ANIDADOS):");
    telem_println("  Etapa                     |  Ciclos Totales   |  Tiempo (ms)  |  % Tiempo  |  Máx Cuadro");
    telem_println("  --------------------------+-------------------+---------------+------------+------------");
    telem_print_fila_etapa("1. Demux MP4", p->telem.ciclos_demux, p->telem.max_ciclos_demux, ciclos_activos, c_ms);
    telem_print_fila_etapa("2. Decodificación AAC", p->telem.ciclos_aac, p->telem.max_ciclos_aac, ciclos_activos, c_ms);
    telem_print_fila_etapa("3. CABAC / Sintaxis / Rec", p->telem.ciclos_cabac_reconstruccion, p->telem.max_ciclos_cabac, ciclos_activos, c_ms);
    telem_print_fila_etapa("   - CABAC Sintaxis Pura", p->telem.ciclos_cabac_puro, 0, ciclos_activos, c_ms);
    telem_print_fila_etapa("   - Inter / Compensación", p->telem.ciclos_inter, 0, ciclos_activos, c_ms);
    telem_print_fila_etapa("   - Intra / Predicción", p->telem.ciclos_intra, 0, ciclos_activos, c_ms);
    telem_print_fila_etapa("4. Desbloqueo (Deblocking)", p->telem.ciclos_desbloqueo, p->telem.max_ciclos_desbloqueo, ciclos_activos, c_ms);
    telem_print_fila_etapa("5. Conversión YUV -> RGB", p->telem.ciclos_yuv_rgb, p->telem.max_ciclos_yuv_rgb, ciclos_activos, c_ms);
    telem_print_fila_etapa("6. Copia al Framebuffer", p->telem.ciclos_copia_fb, p->telem.max_ciclos_copia_fb, ciclos_activos, c_ms);
    telem_print_fila_etapa("7. Espera por PTS (Sinc)", p->telem.ciclos_espera_pts, p->telem.max_ciclos_espera_pts, 0, c_ms);
    telem_print_fila_etapa("8. Servicio HDA / USB", p->telem.ciclos_hda_usb, p->telem.max_ciclos_hda_usb, ciclos_activos, c_ms);
    if (p->modo == MODO_PRUEBA_FORENSE) {
        telem_print_fila_etapa("9. Hash FNV-1a (Forense)", p->telem.ciclos_hash, p->telem.max_ciclos_hash, ciclos_activos, c_ms);
    }
    telem_println("---------------------------------------------------------------------------------");

    telem_println("MÉTRICAS DE CAMINO CRÍTICO Y LATENCIA:");
    telem_println("  Presupuesto de Cuadro (30 fps) : 33.33 ms (33,333 us)");

    telem_print("  Camino Crítico Promedio        : ");
    telem_print_dec(prom_us / 1000);
    telem_print(".");
    if ((prom_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((prom_us % 1000) / 10);
    telem_print(" ms (");
    telem_print_dec((uint64_t)prom_us * 100 / 33333);
    telem_println("% del presupuesto)");

    telem_print("  Percentil p50 (Mediana)        : ");
    telem_print_dec(p50_us / 1000);
    telem_print(".");
    if ((p50_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((p50_us % 1000) / 10);
    telem_println(" ms");

    telem_print("  Percentil p95                  : ");
    telem_print_dec(p95_us / 1000);
    telem_print(".");
    if ((p95_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((p95_us % 1000) / 10);
    telem_print(" ms");
    if (p95_us <= 33333) {
        telem_println("  [OBJETIVO CUMPLIDO: p95 < 33.33 ms]");
    } else {
        telem_println("  [EXCEDE PRESUPUESTO]");
    }

    telem_print("  Percentil p99                  : ");
    telem_print_dec(p99_us / 1000);
    telem_print(".");
    if ((p99_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((p99_us % 1000) / 10);
    telem_println(" ms");

    telem_print("  Retraso Acumulado vs PTS       : ");
    telem_print_dec(p->telem.retraso_pts_acumulado_us / 1000);
    telem_print(" ms (Retraso Máximo: ");
    telem_print_dec(p->telem.retraso_pts_max_us / 1000);
    telem_println(" ms)");
    telem_println("---------------------------------------------------------------------------------");

    telem_println("SUBSISTEMA DE AUDIO:");
    telem_print("  Avance DMA Total               : ");
    telem_print_dec(p->telem.avance_dma_bytes);
    telem_println(" bytes enviados al controlador");

    telem_print("  Vaciados / Underruns de Audio  : ");
    telem_print_dec(p->telem.vaciados_audio);
    telem_println("");
    telem_println("---------------------------------------------------------------------------------");

    telem_println("MEMORIA DEL SISTEMA:");
    telem_print("  Memoria Máxima Dinámica en Uso : ");
    telem_print_dec(p->memoria_maxima / (1024 * 1024));
    telem_print(" MB (");
    telem_print_dec(p->memoria_maxima);
    telem_println(" bytes / Límite 256 MB)");

    telem_print("  Memoria Remanente sin Liberar  : ");
    telem_print_dec(p->memoria_usada);
    telem_println(" bytes");

    telem_print("  Integridad de Canarios de Heap : ");
    if (memoria_verificar_integridad()) {
        telem_println("[CORRECTA / CANARIOS INTACTOS]");
    } else {
        telem_println("[CORRUPCIÓN DETECTADA EN CANARIOS]");
    }
    telem_println("=================================================================================");
    telem_println("");

    // Serial estructurado [BENCHMARK] para oráculos automáticos y captura de datos
    serial_imprimir("[BENCHMARK] VERSION=\""); serial_imprimir(taek_obtener_version()); serial_imprimir_linea("\"");
    serial_imprimir("[BENCHMARK] CPU=\""); serial_imprimir(cpu_str);
    serial_imprimir("\" THREADS="); serial_imprimir_dec(hilos_cpu);
    serial_imprimir(" TSC_MHZ="); serial_imprimir_dec(tsc_mhz); serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] ARCHIVO=\"h264:360p\" FRAMES="); serial_imprimir_dec(p->telem.cuadros_decodificados);
    serial_imprimir(" DURACION_MS="); serial_imprimir_dec(duracion_total_ms);
    serial_imprimir(" HUELLA_FNV="); serial_imprimir_hex(p->huella); serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] P50_US="); serial_imprimir_dec(p50_us);
    serial_imprimir(" P95_US="); serial_imprimir_dec(p95_us);
    serial_imprimir(" P99_US="); serial_imprimir_dec(p99_us);
    serial_imprimir(" PROMEDIO_US="); serial_imprimir_dec(prom_us); serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] DEMUX_CICLOS="); serial_imprimir_dec(p->telem.ciclos_demux);
    serial_imprimir(" AAC_CICLOS="); serial_imprimir_dec(p->telem.ciclos_aac);
    serial_imprimir(" CABAC_CICLOS="); serial_imprimir_dec(p->telem.ciclos_cabac_reconstruccion);
    serial_imprimir(" CABAC_PURO_CICLOS="); serial_imprimir_dec(p->telem.ciclos_cabac_puro);
    serial_imprimir(" INTER_CICLOS="); serial_imprimir_dec(p->telem.ciclos_inter);
    serial_imprimir(" INTRA_CICLOS="); serial_imprimir_dec(p->telem.ciclos_intra);
    serial_imprimir(" DEBLOCK_CICLOS="); serial_imprimir_dec(p->telem.ciclos_desbloqueo);
    serial_imprimir(" YUV_RGB_CICLOS="); serial_imprimir_dec(p->telem.ciclos_yuv_rgb);
    serial_imprimir(" COPIA_FB_CICLOS="); serial_imprimir_dec(p->telem.ciclos_copia_fb);
    serial_imprimir(" ESPERA_PTS_CICLOS="); serial_imprimir_dec(p->telem.ciclos_espera_pts);
    serial_imprimir(" HDA_USB_CICLOS="); serial_imprimir_dec(p->telem.ciclos_hda_usb);
    serial_imprimir_linea("");
}

static void reproducir(const char *nombre, enum modo_reproduccion modo) {
    struct limine_file *archivo = NULL;
    struct limine_module_response *r = peticion_video.response;
    if (r) for (uint64_t i = 0; i < r->module_count; i++) {
        if (iguales(r->modules[i]->cmdline, nombre)) { archivo = r->modules[i]; break; }
    }
    if (!archivo) {
        if (iguales(nombre, "h264:1080p")) {
            consola_imprimir_linea_color("El video 1080p fue descartado del arranque para reducir el tamaño de TAEK OS.", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea("Puedes grabarlo en un pendrive USB para probar la lectura con 'disco' y 'cat'.");
            consola_imprimir_linea("Para probar la decodificación interna, ejecuta: 'h264 360p'.");
        } else {
            consola_imprimir_linea_color("No está cargado ese módulo MP4 en memoria.", COLOR_ERROR_DEFAULT);
            consola_imprimir("Módulo buscado: "); consola_imprimir_linea_color(nombre, COLOR_AVISO_DEFAULT);
        }
        serial_imprimir_linea("[MULTIMEDIA] ERROR: módulo MP4 ausente");
        return;
    }

    mp4_contenedor mp4;
    mp4_resultado res_mp4 = mp4_abrir(&mp4, archivo->address, (size_t)archivo->size);
    if (res_mp4 != MP4_OK) {
        consola_imprimir_linea("MP4 inválido o contenedor no soportado.");
        serial_imprimir_linea("[MULTIMEDIA] ERROR MP4");
        return;
    }
    if (!mp4_tiene_video(&mp4)) {
        consola_imprimir_linea("El archivo MP4 no contiene pista de video compatible.");
        return;
    }

    reproductor p = {
        .modo = modo,
        .escala = mp4.v_escala_tiempo ? mp4.v_escala_tiempo : 90000,
        .huella = UINT64_C(14695981039346656037)
    };

    // Reservar búfer para percentiles de latencia (hasta 5000 cuadros)
    p.telem.capacidad_tiempos = 5000;
    p.telem.tiempos_cuadro_us = (uint32_t *)reservar(&p, sizeof(uint32_t) * p.telem.capacidad_tiempos);

    h264_servicios servicios = { &p, reservar, soltar, presentar };
    memoria_estadisticas_t antes, despues;
    memoria_obtener_estadisticas(&antes);

    h264_decodificador *dec = h264_crear(&servicios);
    if (!dec) {
        consola_imprimir_linea("Memoria insuficiente para H.264.");
        if (p.telem.tiempos_cuadro_us) soltar(&p, p.telem.tiempos_cuadro_us);
        return;
    }

    aac_decodificador *dec_aac = NULL;
    if (mp4_tiene_audio(&mp4) && modo == MODO_INTERACTIVO) {
        aac_servicios serv_aac = {
            .usuario = &p,
            .asignar = reservar,
            .liberar = soltar
        };
        dec_aac = aac_crear(&serv_aac);
        if (dec_aac) {
            aac_configurar(dec_aac, (int)mp4.a_canales, (int)mp4.a_frecuencia);
            serial_imprimir_linea("[AAC] Decodificador AAC inicializado.");
        }
    }

    consola_imprimir("Reproductor Multimedia TAEK OS. Presiona ESC para salir...");
    if (modo == MODO_PRUEBA_FORENSE) {
        consola_imprimir_linea_color(" [MODO PRUEBA FORENSE FNV-1a]", COLOR_AVISO_DEFAULT);
    } else if (modo == MODO_BENCHMARK) {
        consola_imprimir_linea_color(" [MODO BENCHMARK VELOCIDAD PICO]", COLOR_PROMPT_DEFAULT);
    } else {
        consola_imprimir_linea_color(mp4_tiene_audio(&mp4) ? " [H.264 + AAC]" : " [Solo H.264]", COLOR_EXITO_DEFAULT);
    }

    serial_imprimir("[MULTIMEDIA] INICIO "); serial_imprimir_linea(nombre);
    h264_resultado resultado = h264_configurar_avcc(dec, mp4.avcc, mp4.avcc_bytes);

    uint64_t t_inicio_reproduccion = tiempo_obtener_milisegundos();

    while (!resultado) {
        const uint8_t *datos;
        size_t bytes;
        int64_t pts;
        uint32_t duracion;
        if (atender(&p)) { resultado = H264_CANCELADO; break; }

        if (p.t_inicio_cuadro_ciclos == 0) p.t_inicio_cuadro_ciclos = rdtsc();

        // 1. Demux de video
        uint64_t t_dv0 = rdtsc();
        int siguiente = mp4_siguiente_video(&mp4, &datos, &bytes, &pts, &duracion);
        uint64_t dt_dv = rdtsc() - t_dv0;
        p.telem.ciclos_demux += dt_dv;
        if (dt_dv > p.telem.max_ciclos_demux) p.telem.max_ciclos_demux = dt_dv;
        if (siguiente <= 0) { resultado = (h264_resultado)siguiente; break; }

        // 2. Audio AAC (solo en MODO_INTERACTIVO)
        if (dec_aac && mp4.tiene_audio && modo == MODO_INTERACTIVO) {
            const uint8_t *datos_audio;
            size_t bytes_audio;
            int64_t pts_audio;
            uint32_t escala_a = mp4.a_escala_tiempo ? mp4.a_escala_tiempo : 44100;
            while (mp4.a_indice < mp4.a_muestras &&
                   (int64_t)(mp4.a_tiempo * p.escala / escala_a) <= pts) {
                uint64_t t_da0 = rdtsc();
                int res_a = mp4_siguiente_audio(&mp4, &datos_audio, &bytes_audio, &pts_audio);
                uint64_t dt_da = rdtsc() - t_da0;
                p.telem.ciclos_demux += dt_da;
                if (dt_da > p.telem.max_ciclos_demux) p.telem.max_ciclos_demux = dt_da;
                if (res_a <= 0) break;

                const uint8_t *ptr_aac = datos_audio;
                int rem_aac = (int)bytes_audio;
                int16_t pcm_buf[2048];

                uint64_t t_aac0 = rdtsc();
                int s = aac_decodificar(dec_aac, &ptr_aac, &rem_aac, pcm_buf);
                uint64_t dt_aac = rdtsc() - t_aac0;
                p.telem.ciclos_aac += dt_aac;
                if (dt_aac > p.telem.max_ciclos_aac) p.telem.max_ciclos_aac = dt_aac;

                if (s > 0) {
                    uint64_t t_snd0 = rdtsc();
                    int res_snd = audio_ac97_encolar_pcm(pcm_buf, (uint32_t)(s * sizeof(int16_t)));
                    p.telem.ciclos_hda_usb += (rdtsc() - t_snd0);
                    p.telem.avance_dma_bytes += (uint32_t)(s * sizeof(int16_t));
                    if (res_snd < 0) p.telem.vaciados_audio++;
                }
            }
        }

        // 3. Decodificación de muestra H.264
        resultado = h264_decodificar_muestra_avcc(dec, mp4.v_longitud_nal, datos, bytes, pts);
    }
    if (!resultado) resultado = h264_finalizar(dec);

    uint64_t duracion_real_ms = tiempo_obtener_milisegundos() - t_inicio_reproduccion;

    if (dec_aac) {
        audio_ac97_detener();
        aac_destruir(dec_aac);
    }

    consola_limpiar();
    consola_imprimir(p.cancelado ? "Reproducción cancelada por usuario. " :
                     resultado || p.error ? "Error durante reproducción: " :
                     "Reproducción finalizada con éxito. ");
    if (resultado && !p.cancelado) consola_imprimir(h264_error(dec));
    consola_imprimir_linea("");

    // Generar e imprimir informe integral de telemetría y benchmark
    imprimir_informe_benchmark(&p, dec, duracion_real_ms);

    h264_destruir(dec);
    if (p.rgb) soltar(&p, p.rgb);
    if (p.telem.tiempos_cuadro_us) soltar(&p, p.telem.tiempos_cuadro_us);

    memoria_obtener_estadisticas(&despues);
    serial_imprimir("[MULTIMEDIA] HEAP_DELTA=");
    serial_imprimir_dec((uint64_t)(despues.heap_bytes_en_uso >= antes.heap_bytes_en_uso ?
                        despues.heap_bytes_en_uso - antes.heap_bytes_en_uso : 0));
    serial_imprimir_linea("");
}

static void audio_aac_probar(const char *nombre) {
    struct limine_file *archivo = NULL;
    struct limine_module_response *r = peticion_video.response;
    if (r) for (uint64_t i = 0; i < r->module_count; i++) {
        if (iguales(r->modules[i]->cmdline, nombre)) { archivo = r->modules[i]; break; }
    }
    if (!archivo) {
        if (iguales(nombre, "h264:1080p")) {
            consola_imprimir_linea_color("[AAC] El video 1080p fue descartado del arranque para aligerar el sistema.", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea("      Usa 'aac' o 'aac 360p' para auditar el códec con el video integrado.");
        } else {
            consola_imprimir_linea_color("[AAC] ERROR: Módulo MP4 no cargado en memoria.", COLOR_ERROR_DEFAULT);
            consola_imprimir("Módulo buscado: "); consola_imprimir_linea_color(nombre, COLOR_AVISO_DEFAULT);
        }
        serial_imprimir_linea("[AAC] ERROR: Módulo no encontrado");
        return;
    }

    mp4_contenedor mp4;
    mp4_resultado res_mp4 = mp4_abrir(&mp4, archivo->address, (size_t)archivo->size);
    if (res_mp4 != MP4_OK) {
        consola_imprimir_linea_color("[AAC] ERROR: Contenedor MP4 inválido.", COLOR_ERROR_DEFAULT);
        return;
    }

    if (!mp4_tiene_audio(&mp4)) {
        consola_imprimir_linea_color("[AAC] El archivo MP4 no tiene pista de audio AAC.", COLOR_AVISO_DEFAULT);
        return;
    }

    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_color("  AUDITORÍA Y DECODIFICACIÓN AUDIO AAC - ", COLOR_EXITO_DEFAULT);
    consola_imprimir_linea_color(nombre, COLOR_USUARIO_DEFAULT);
    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Canales de audio   : "); consola_imprimir_dec(mp4.a_canales);
    consola_imprimir_linea(mp4.a_canales == 2 ? " (Estéreo)" : " (Mono)");
    consola_imprimir("  Frecuencia         : "); consola_imprimir_dec(mp4.a_frecuencia); consola_imprimir_linea(" Hz");
    consola_imprimir("  Total paquetes AAC : "); consola_imprimir_dec(mp4.a_muestras); consola_imprimir_linea("");
    consola_imprimir_linea("------------------------------------------------------------------");

    reproductor p = {0};
    aac_servicios serv_aac = {
        .usuario = &p,
        .asignar = reservar,
        .liberar = soltar
    };

    aac_decodificador *dec_aac = aac_crear(&serv_aac);
    if (!dec_aac) {
        consola_imprimir_linea_color("[AAC] Memoria insuficiente para inicializar decodificador.", COLOR_ERROR_DEFAULT);
        return;
    }

    if (aac_configurar(dec_aac, (int)mp4.a_canales, (int)mp4.a_frecuencia) != 0) {
        consola_imprimir_linea_color("[AAC] Falló la configuración de parámetros crudos AAC.", COLOR_ERROR_DEFAULT);
        aac_destruir(dec_aac);
        return;
    }

    uint32_t paquetes_ok = 0;
    uint32_t paquetes_err = 0;
    uint64_t total_muestras_pcm = 0;
    uint64_t ciclos_aac_total = 0;
    uint64_t t_inicio = tiempo_obtener_milisegundos();
    int16_t pcm_salida[2048];

    const uint8_t *datos_aac;
    size_t bytes_aac;
    int64_t pts_aac;

    serial_imprimir("[AAC] Iniciando decodificación exhaustiva de ");
    serial_imprimir_linea(nombre);

    while (mp4_siguiente_audio(&mp4, &datos_aac, &bytes_aac, &pts_aac) > 0) {
        atender(&p);
        if (p.cancelado) {
            consola_imprimir_linea_color("  [!] Prueba cancelada por el usuario (ESC).", COLOR_AVISO_DEFAULT);
            break;
        }

        const uint8_t *inptr = datos_aac;
        int rem = (int)bytes_aac;

        uint64_t t0 = rdtsc();
        int s = aac_decodificar(dec_aac, &inptr, &rem, pcm_salida);
        ciclos_aac_total += (rdtsc() - t0);

        if (s > 0) {
            paquetes_ok++;
            total_muestras_pcm += s;
        } else {
            paquetes_err++;
        }
    }

    uint64_t duracion_ms = tiempo_obtener_milisegundos() - t_inicio;
    aac_destruir(dec_aac);

    uint64_t c_ms = tiempo_ciclos_por_ms();
    if (c_ms == 0) c_ms = 2500000;

    consola_imprimir_linea("");
    consola_imprimir("  Paquetes exitosos  : "); consola_imprimir_dec(paquetes_ok); consola_imprimir_linea("");
    consola_imprimir("  Paquetes erróneos  : "); consola_imprimir_dec(paquetes_err); consola_imprimir_linea("");
    consola_imprimir("  Muestras PCM gener.: "); consola_imprimir_dec(total_muestras_pcm); consola_imprimir_linea("");
    consola_imprimir("  Tiempo total       : "); consola_imprimir_dec(duracion_ms); consola_imprimir_linea(" ms");
    consola_imprimir("  Ciclos CPU activos : "); consola_imprimir_dec(ciclos_aac_total / 1000000ULL); consola_imprimir_linea(" M");
    consola_imprimir("  Tiempo CPU núcleo  : ");
    consola_imprimir_dec(ciclos_aac_total / c_ms);
    consola_imprimir_linea(" ms");

    if (paquetes_err == 0 && paquetes_ok > 0) {
        consola_imprimir_linea_color("  [ OK ] Decodificación AAC 100% exitosa sin errores.", COLOR_EXITO_DEFAULT);
        serial_imprimir_linea("[AAC] OK: Decodificación completa verificada.");
    } else if (paquetes_ok > 0) {
        consola_imprimir_linea_color("  [ AVISO ] Decodificación completada con advertencias.", COLOR_AVISO_DEFAULT);
    } else {
        consola_imprimir_linea_color("  [ ERROR ] No se pudo decodificar ningún paquete AAC.", COLOR_ERROR_DEFAULT);
    }
    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);

    serial_imprimir("[BENCHMARK] AAC_PAQUETES="); serial_imprimir_dec(paquetes_ok);
    serial_imprimir(" AAC_MUESTRAS="); serial_imprimir_dec(total_muestras_pcm);
    serial_imprimir(" AAC_TIEMPO_MS="); serial_imprimir_dec(duracion_ms);
    serial_imprimir(" AAC_CICLOS="); serial_imprimir_dec(ciclos_aac_total);
    serial_imprimir_linea("");
}

void audio_aac_comando(const char *arg) {
    if (iguales(arg, "360p") || iguales(arg, "1")) audio_aac_probar("h264:360p");
    else if (iguales(arg, "1080p") || iguales(arg, "2")) audio_aac_probar("h264:1080p");
    else if (iguales(arg, "probar") || iguales(arg, "test") || iguales(arg, "todo") || iguales(arg, "")) {
        audio_aac_probar("h264:360p");
    } else {
        consola_imprimir_linea_color("--- AUDITORÍA DE DECODIFICADOR AAC (RING 0) ---", COLOR_AVISO_DEFAULT);
        consola_imprimir_color("  aac                ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Audita y decodifica la pista AAC de 'Video 360p.mp4'.");
        consola_imprimir_color("  aac 360p           ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Audita la pista de audio AAC de 'Video 360p.mp4'.");
        consola_imprimir_color("  aac 1080p          ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": [USB] Requiere video 1080p en memoria o pendrive externo.");
    }
}

void video_h264_comando(const char *arg) {
    if (iguales(arg, "360p") || iguales(arg, "1")) {
        reproducir("h264:360p", MODO_INTERACTIVO);
    } else if (iguales(arg, "1080p") || iguales(arg, "2")) {
        reproducir("h264:1080p", MODO_INTERACTIVO);
    } else if (iguales(arg, "prueba 360p") || iguales(arg, "test 360p") || iguales(arg, "prueba")) {
        reproducir("h264:360p", MODO_PRUEBA_FORENSE);
    } else if (iguales(arg, "prueba 1080p") || iguales(arg, "test 1080p")) {
        reproducir("h264:1080p", MODO_PRUEBA_FORENSE);
    } else if (iguales(arg, "bench 360p") || iguales(arg, "benchmark 360p") || iguales(arg, "bench")) {
        reproducir("h264:360p", MODO_BENCHMARK);
    } else if (iguales(arg, "aac 360p") || iguales(arg, "audio 360p")) {
        audio_aac_probar("h264:360p");
    } else if (iguales(arg, "aac 1080p") || iguales(arg, "audio 1080p")) {
        audio_aac_probar("h264:1080p");
    } else if (iguales(arg, "aac") || iguales(arg, "audio")) {
        audio_aac_probar("h264:360p");
    } else {
        consola_imprimir_linea_color("--- REPRODUCTOR MULTIMEDIA H.264 & AAC (TELEMETRÍA) ---", COLOR_AVISO_DEFAULT);
        consola_imprimir_color("  h264 360p          ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Reproducción interactiva A/V completa (H.264 + AAC + PTS).");
        consola_imprimir_color("  h264 bench 360p    ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Benchmark a velocidad pico de hardware (sin esperas de PTS).");
        consola_imprimir_color("  h264 prueba 360p   ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Prueba forense H.264 (huella FNV-1a de 100% de cuadros).");
        consola_imprimir_color("  aac [360p]         ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Diagnóstico y auditoría exhaustiva del códec AAC.");
    }
}

void video_h264_arranque(const char *cmdline) {
    if (!cmdline) return;
    while (*cmdline) {
        while (*cmdline == ' ') cmdline++;
        char token[32];
        unsigned n = 0;
        while (*cmdline && *cmdline != ' ') {
            if (n + 1 < sizeof(token)) token[n++] = *cmdline;
            cmdline++;
        }
        token[n] = 0;
        if (iguales(token, "h264=prueba360")) reproducir("h264:360p", MODO_PRUEBA_FORENSE);
        if (iguales(token, "h264=bench360")) reproducir("h264:360p", MODO_BENCHMARK);
        if (iguales(token, "h264=play360")) reproducir("h264:360p", MODO_INTERACTIVO);
        if (iguales(token, "h264=prueba1080")) reproducir("h264:1080p", MODO_PRUEBA_FORENSE);
    }
}
