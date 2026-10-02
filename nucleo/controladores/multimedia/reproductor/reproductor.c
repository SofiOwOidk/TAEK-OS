#include "reproductor.h"
#include "cola_muestras.h"
#include "traza.h"
#include "cache_fuente.h"
#include "anillo_almacenamiento.h"
#include "arquitectura/x86_64/pci.h"
#include "controladores/usb_msc.h"
#include "controladores/usb_msc_telemetria.h"
#include "controladores/multimedia/mp4/mp4.h"
#include "controladores/multimedia/h264/h264.h"
#include "controladores/multimedia/aac/aac.h"
#include "boot/limine/limine.h"
#include "base/memoria.h"
#include "base/tiempo.h"
#include "base/trabajos.h"
#include "base/version.h"
#include "arquitectura/x86_64/serial.h"
#include "arquitectura/x86_64/fpu.h"
#include "controladores/consola.h"
#include "controladores/pantalla.h"
#include "controladores/teclado.h"
#include "controladores/xhci.h"
#include "controladores/audio_ac97.h"
#include "controladores/audio_hda.h"
#include "controladores/vfs.h"
#include "controladores/video/intel/intel_info.h"
#include "controladores/video/intel/intel_diagnostico_ring0.h"
#include "controladores/video/intel/i915_drv.h"
#include "controladores/video/intel/i915_va.h"

#ifndef TAEK_REVISION_CODIGO
#define TAEK_REVISION_CODIGO "sin identificar"
#endif
#ifndef H264_INTER_SSE2
#define H264_INTER_SSE2 0
#endif

// ============================================================================
// TAEK OS - REPRODUCTOR MULTIMEDIA H.264 & AAC (Hito A: Telemetría y Benchmark)
// Contrato Común de Medición: Ciclos, Tiempos, Percentiles y Desglose por Etapa
// ============================================================================

__attribute__((used, section(".requests")))
static volatile struct limine_module_request peticion_video = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};
/* Sólo el token de arranque smoke1080 activa este límite funcional de QEMU. */
static unsigned muestras_funcionales;
static unsigned diagnostico_cpu=4;
static int diagnostico_coordinador;
static unsigned diagnostico_lectura_kib=4;

// Modos de ejecución soportados definidos en reproductor.h

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
    uint64_t ciclos_pared_sesion;

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

    // Avance DMA y audio (A4)
    uint64_t pcm_producido_bytes;
    uint64_t pcm_aceptado_bytes;
    uint64_t avance_dma_bytes;
    uint64_t silencio_insertado_bytes;
    uint32_t vaciados_audio;

    // Registro de latencia por cuadro para cálculo de percentiles
    uint32_t *tiempos_cuadro_us;
    uint32_t total_tiempos_grabados;
    uint32_t capacidad_tiempos;
    uint64_t cuadros_tiempo_observados;
    uint32_t semilla_muestreo;
} telemetria_reproductor;

typedef struct {
    unsigned cuadros, ancho, alto, escala;
    const char *fuente;
    enum modo_reproduccion modo;
    int cancelado, error;
    int64_t primer_pts;
    uint64_t inicio, huella;
    size_t memoria_usada, memoria_maxima;
    uint32_t *rgb;
    unsigned presentaciones_cabeza,presentaciones_pendientes;
    int64_t presentaciones_pts[2];
    uint64_t reloj_maestro_ultimo, servicio_ultimo, servicio_max_ms;
    uint32_t vaciados_en_pista,vaciados_eof;
    uint64_t silencio_eof_inicio;
    uint64_t silencio_en_pista,silencio_despues_eof;
    int audio_eof;
    h264_decodificador *dec_video;
    telemetria_reproductor telem;
    uint64_t t_inicio_cuadro_ciclos;
    uint64_t ciclos_espera_pts_ultimo_cuadro;
    aac_decodificador *dec_aac;
    mp4_contenedor    *mp4;
    int descriptor_vfs_video;
    int descriptor_vfs_audio;
    cola_muestras *video_comprimido,*audio_comprimido;
    traza_video *traza;
    unsigned muestra_actual;
    int64_t pts_actual;
    mp4_lectura_posicional lector_original;
    void *lector_contexto;
    uint64_t ciclos_lectura, bytes_lectura, lecturas;
    unsigned cuadros_tardios;
    uint64_t ultimo_sondeo;
    cache_fuente *cache;
    anillo_almacenamiento *anillo;
    uint64_t usb_comandos_inicio,usb_bytes_inicio;
    unsigned muestra_lectura;
    int64_t pts_lectura;
    int pista_lectura;
    uint64_t ultima_presentacion_ms;
    int ya_presentado;
    traza_video *pausa;
    uint64_t pausa_inicio,pausa_fin;
    uint64_t heap_inicial;
    i915_va_sesion_t *va_sesion;
    int usando_intel_va;
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

static void registrar(reproductor *p,unsigned etapa,uint64_t ini,uint64_t fin,uint64_t detalle) {
    traza_registrar(p->traza,0,etapa,p->muestra_actual,p->pts_actual,ini,fin,detalle,
                    audio_ac97_cola_ocupada(),p->presentaciones_pendientes);
}
static void observar_h264(void *u,unsigned cpu,unsigned etapa,int64_t pts,uint64_t ini,uint64_t fin,uint64_t detalle) {
    reproductor *p=u;
    /* AP no consulta hardware ni campos modificados por servicio BSP. */
    traza_registrar(p->traza,cpu,etapa,p->muestra_actual,pts,ini,fin,detalle,0,0);
}
static void observar_indice(void *u,int pista,unsigned muestra,int64_t pts,uint64_t ini,uint64_t fin) {
    reproductor *p=u;
    p->muestra_lectura=muestra;p->pts_lectura=pts;p->pista_lectura=pista;
    p->telem.ciclos_demux+=fin-ini;
    traza_registrar(p->traza,0,TR_DEMUX,muestra,pts,ini,fin,pista,audio_ac97_cola_ocupada(),p->presentaciones_pendientes);
}
static int64_t leer_medido(void *u,int pista,uint64_t off,void *dst,size_t n) {
    reproductor *p=u;uint64_t t=rdtsc();
    /* Contrato verificable: no iniciar I/O si un lote posee filas/superficies. */
    if(trabajos_en_curso()){p->error=1;return MP4_ERROR_LECTURA;}
    int64_t r=p->lector_original(p->lector_contexto,pista,off,dst,n);
    uint64_t fin=rdtsc();p->ciclos_lectura+=fin-t;p->lecturas++;
    if(r>0)p->bytes_lectura+=(uint64_t)r;
    traza_registrar(p->traza,0,TR_LECTURA,p->muestra_lectura,p->pts_lectura,t,fin,n,
        audio_ac97_cola_ocupada(),p->presentaciones_pendientes);
    return r;
}

static int atender(reproductor *p) {
    uint64_t t0 = rdtsc();
    xhci_sondeo();
    audio_ac97_actualizar();
    if(p && p->descriptor_vfs_video>=0) {
        struct vfs_descriptor_archivo *fd=vfs_obtener_descriptor(p->descriptor_vfs_video);
        const struct usb_msc_dispositivo *d=fd?usb_msc_obtener_dispositivo(fd->unidad_msc):NULL;
        if(!fd || fd->cancelado || !d || !d->activo || !d->listo || d->generacion!=fd->generacion_msc) {
            int desconectado=!fd || !d || !d->activo || !d->listo || d->generacion!=fd->generacion_msc;
            p->error=1;
            if(!p->mp4->ultimo_error_lectura || desconectado)
                p->mp4->ultimo_error_lectura=desconectado?VOLUMEN_DESCONECTADO:VOLUMEN_CANCELADO;
            if(p->video_comprimido)p->video_comprimido->error=MP4_ERROR_LECTURA;
            if(p->audio_comprimido)p->audio_comprimido->error=MP4_ERROR_LECTURA;
        }
    }
    while (teclado_hay_datos()) {
        char c = teclado_leer_caracter();
        if (c == 27 || c == 3 || c == 'q' || c == 'Q') {
            if (p) p->cancelado = 1;
            if (p && p->descriptor_vfs_video >= 0) vfs_cancelar(p->descriptor_vfs_video);
            if (p && p->descriptor_vfs_audio >= 0) vfs_cancelar(p->descriptor_vfs_audio);
        }
    }
    char cc;
    while ((cc = consola_leer_caracter()) != 0) {
        if (cc == 27 || cc == 3 || cc == 'q' || cc == 'Q') {
            if (p) p->cancelado = 1;
            if (p && p->descriptor_vfs_video >= 0) vfs_cancelar(p->descriptor_vfs_video);
            if (p && p->descriptor_vfs_audio >= 0) vfs_cancelar(p->descriptor_vfs_audio);
        }
    }
    uint64_t dt = rdtsc() - t0;
    if (p) {
        p->telem.ciclos_hda_usb += dt;
        if (dt > p->telem.max_ciclos_hda_usb) p->telem.max_ciclos_hda_usb = dt;
        registrar(p,TR_SERVICIO,t0,t0+dt,0);
    }
    return p ? p->cancelado : 0;
}

static int servicio_recarga(void *u){reproductor *p=u;return atender(p)||p->error;}
static int validar_cache(void *u) {
    reproductor *p=u;
    if(servicio_recarga(p))return p->mp4->ultimo_error_lectura?(int)p->mp4->ultimo_error_lectura:VOLUMEN_CANCELADO;
    return 0;
}
static void guardar_pausa(reproductor *p,uint64_t ini,uint64_t fin) {
    uint64_t c_ms=tiempo_ciclos_por_ms();
    if(p->pausa && p->traza && c_ms && fin-ini>=150*c_ms && fin-ini>p->pausa_fin-p->pausa_inicio) {
        /* Sólo entre lotes, después de adquirir las terminaciones de los AP. */
        memcpy(p->pausa,p->traza,sizeof(*p->pausa));p->pausa_inicio=ini;p->pausa_fin=fin;
    }
}
static int recargar(reproductor *p,int video) {
    cola_muestras *q=video?p->video_comprimido:p->audio_comprimido;
    if(!q)return 1;
    if(q->cantidad>=(video?2u:32u))return 1;
    uint64_t ini=rdtsc();
    int r=cola_muestras_recargar(q,video?4:64,servicio_recarga,p);
    guardar_pausa(p,ini,rdtsc());
    if(r<0){p->error=1;if(q->cursor.ultimo_error_lectura)p->mp4->ultimo_error_lectura=q->cursor.ultimo_error_lectura;}
    return r;
}
static int siguiente_audio(reproductor *p,const uint8_t **d,size_t *n,int64_t *pts) {
    if(!p->audio_comprimido)return mp4_siguiente_audio(p->mp4,d,n,pts);
    int r=cola_muestras_tomar(p->audio_comprimido,d,n,pts,NULL);
    if(r>0)p->mp4->a_indice++;
    return r;
}

// PCM estéreo S16 44.1 kHz del dispositivo: objetivo 750 ms, servicio prioritario.
static void reproductor_alimentar_audio(reproductor *p) {
    if (!p || !p->dec_aac || !p->mp4 || !p->mp4->tiene_audio || p->modo != MODO_INTERACTIVO) return;
    if(p->audio_eof)return;
    uint64_t ahora_servicio=tiempo_obtener_milisegundos();
    if(p->servicio_ultimo && ahora_servicio-p->servicio_ultimo>p->servicio_max_ms)
        p->servicio_max_ms=ahora_servicio-p->servicio_ultimo;
    p->servicio_ultimo=ahora_servicio;
    if(p->mp4->a_indice<p->mp4->a_muestras && audio_ac97_cola_ocupada()>=44100u*4u*450u/1000u)return;

    while (!p->error && !p->cancelado &&
           p->mp4->a_indice < p->mp4->a_muestras && audio_ac97_cola_ocupada() < (44100u * 4u * 750u / 1000u)) {
        const uint8_t *datos_audio;
        size_t bytes_audio;
        int64_t pts_audio;
        uint64_t t_da0 = rdtsc();
        int res_a = siguiente_audio(p, &datos_audio, &bytes_audio, &pts_audio);
        uint64_t dt_da = rdtsc() - t_da0;
        p->telem.ciclos_demux += dt_da;
        if (dt_da > p->telem.max_ciclos_demux) p->telem.max_ciclos_demux = dt_da;
        if (res_a < 0) { p->error = 1; break; }
        if (res_a == 0) break;

        const uint8_t *ptr_aac = datos_audio;
        int rem_aac = (int)bytes_audio;
        int16_t pcm_buf[2048];

        uint64_t t_aac0 = rdtsc();
        int s = aac_decodificar(p->dec_aac, &ptr_aac, &rem_aac, pcm_buf);
        uint64_t dt_aac = rdtsc() - t_aac0;
        p->telem.ciclos_aac += dt_aac;
        if (dt_aac > p->telem.max_ciclos_aac) p->telem.max_ciclos_aac = dt_aac;
        registrar(p,TR_AAC,t_aac0,t_aac0+dt_aac,bytes_audio);

        if (s > 0) {
            uint32_t pcm_bytes = (uint32_t)(s * sizeof(int16_t));
            const uint8_t *ptr_pcm = (const uint8_t *)pcm_buf;
            p->telem.pcm_producido_bytes += pcm_bytes;

            uint64_t t_snd0 = rdtsc();
            while (pcm_bytes > 0) {
                int esc = audio_ac97_encolar_pcm(ptr_pcm, pcm_bytes);
                if (esc > 0) {
                    ptr_pcm += esc;
                    pcm_bytes -= (uint32_t)esc;
                    p->telem.pcm_aceptado_bytes += (uint32_t)esc;
                } else {
                    if (atender(p)) break;
                    esperar_microsegundos(200);
                }
            }
            p->telem.ciclos_hda_usb += (rdtsc() - t_snd0);
        }
    }
    if(!p->audio_eof && !p->cancelado && !p->error && p->mp4->a_indice==p->mp4->a_muestras) {
        p->audio_eof=1;p->vaciados_en_pista=audio_ac97_obtener_vaciados();
        if(audio_es_intel_hda())p->silencio_eof_inicio=audio_hda_obtener_estado()->silencio_insertado_bytes;
        audio_ac97_drenar();
    }
}

// Detección de CPU y conteo de hilos lógicos vía CPUID
static void obtener_info_cpu(char *marca_cpu, uint32_t tam_marca, uint32_t *hilos_detectados,
                             uint32_t *nucleos_detectados) {
    uint32_t eax, ebx, ecx, edx;

    uint32_t max_basico, logicos=0, nucleos=0;
    __asm__ volatile ("cpuid" : "=a"(max_basico), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    int intel=ebx==0x756e6547&&edx==0x49656e69&&ecx==0x6c65746e;
    /* CPUID(1).EBX[23:16] reserva IDs APIC: no es el número de hilos.
     * Intel SDM, CPUID: preferir 1F, luego B. Son datos del paquete del BSP,
     * no un inventario de CPUs presentes/arrancadas. No extrapolar a SMP. */
    if (intel) for (int opcion=0;opcion<2&&!logicos;opcion++) {
        unsigned hoja=opcion?0xb:0x1f,smt=0,paquete=0;
        if(max_basico<hoja)continue;
        for(unsigned nivel=0;nivel<8;nivel++) {
            __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(hoja), "c"(nivel));
            unsigned tipo=(ecx>>8)&255,cantidad=ebx&65535;
            if(!tipo||!cantidad)break;
            if(tipo==1)smt=cantidad;
            paquete=cantidad;
        }
        logicos=paquete;
        if(smt&&paquete&&paquete%smt==0)nucleos=paquete/smt;
    }
    if (intel&&max_basico>=7) {
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
        /* En paquetes híbridos SMT no es uniforme. Sin enumerar los APs,
         * dividir por hilos/core inventaría un conteo físico. */
        if(edx&(1u<<15))nucleos=0;
    }
    if(hilos_detectados)*hilos_detectados=logicos;
    if(nucleos_detectados)*nucleos_detectados=nucleos;

    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000000));
    if (eax >= 0x80000004 && tam_marca >= 49) {
        uint32_t marca[12];uint32_t *dst = marca;
        for (uint32_t i = 0; i < 3; i++) {
            __asm__ volatile ("cpuid" : "=a"(dst[0]), "=b"(dst[1]), "=c"(dst[2]), "=d"(dst[3]) : "a"(0x80000002 + i));
            dst += 4;
        }
        for(unsigned i=0;i<48;i++)marca_cpu[i]=((const char *)marca)[i];
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

static uint64_t reloj_maestro(reproductor *p) {
    uint64_t t=audio_ac97_esta_reproduciendo()?audio_ac97_obtener_tiempo_ms():tiempo_obtener_milisegundos()-p->inicio;
    if(t<p->reloj_maestro_ultimo)t=p->reloj_maestro_ultimo;
    p->reloj_maestro_ultimo=t;return t;
}
static int presentar_pendiente(reproductor *p,int esperar) {
    if(!p->presentaciones_pendientes)return 0;
    // 1. Espera de sincronización PTS (solo en MODO_INTERACTIVO)
    if (p->modo == MODO_INTERACTIVO) {
        int64_t delta = p->presentaciones_pts[p->presentaciones_cabeza] - p->primer_pts;
        if (delta < 0 || (uint64_t)delta / p->escala > 86400) {
            p->error = 1;
            return 1;
        }
        uint64_t destino = (uint64_t)delta * 1000 / p->escala;
        uint64_t ahora = reloj_maestro(p);

        if (ahora < destino && !esperar)return 0;
        if (ahora < destino) {
            uint64_t c_ms_pts = tiempo_ciclos_por_ms();
            if (c_ms_pts == 0) c_ms_pts = 2500000;
            uint64_t max_ciclos_espera = c_ms_pts * 500ULL; // 500 ms máximo de seguridad
            uint64_t t_pts0 = rdtsc();
            uint64_t trabajo_pts=0;

            while (1) {
                uint64_t t_act = reloj_maestro(p);
                if (t_act >= destino) break;
                if ((rdtsc() - t_pts0) >= max_ciclos_espera) break;
                uint64_t t_trabajo=rdtsc();
                if (atender(p)) return 1;
                if(recargar(p,0)<0)return 1;
                reproductor_alimentar_audio(p);
                trabajo_pts+=rdtsc()-t_trabajo;
                esperar_microsegundos(200);
            }
            uint64_t fin_pts=rdtsc();
            uint64_t dt_pts = fin_pts-t_pts0>trabajo_pts?fin_pts-t_pts0-trabajo_pts:0;
            registrar(p,TR_PTS,t_pts0,fin_pts,trabajo_pts);
            p->telem.ciclos_espera_pts += dt_pts;
            if (dt_pts > p->telem.max_ciclos_espera_pts) p->telem.max_ciclos_espera_pts = dt_pts;
            p->ciclos_espera_pts_ultimo_cuadro = dt_pts;
        } else if (ahora > destino) {
            uint64_t retraso_us = (ahora - destino) * 1000;
            p->telem.retraso_pts_acumulado_us += retraso_us;
            if (retraso_us > p->telem.retraso_pts_max_us) p->telem.retraso_pts_max_us = retraso_us;
            /* Caduca una presentación, jamás la decodificación/referencias.
             * Tolerancia dos períodos nominales de la pista. */
            uint64_t periodo=p->mp4->v_muestras?
                p->mp4->v_duracion_ticks*1000/p->escala/p->mp4->v_muestras:42;
            if(!periodo)periodo=42;
            if(ahora-destino>2*periodo &&
               (p->presentaciones_pendientes>1 || (p->ya_presentado && ahora-p->ultima_presentacion_ms<periodo))) {
                registrar(p,TR_DESCARTADO,rdtsc(),rdtsc(),ahora-destino);
                p->cuadros_tardios++;p->presentaciones_cabeza^=1;p->presentaciones_pendientes--;
                return 0;
            }
        }
    }


    uint64_t t=rdtsc();
    pantalla_dibujar_imagen_centrada((int)p->ancho,(int)p->alto,p->rgb+(size_t)p->presentaciones_cabeza*p->ancho*p->alto);
    uint64_t dt=rdtsc()-t;p->telem.ciclos_copia_fb+=dt;
    traza_registrar(p->traza,0,TR_FRAMEBUFFER,p->muestra_actual,p->presentaciones_pts[p->presentaciones_cabeza],
        t,t+dt,0,audio_ac97_cola_ocupada(),p->presentaciones_pendientes);
    p->ultima_presentacion_ms=reloj_maestro(p);p->ya_presentado=1;
    if(dt>p->telem.max_ciclos_copia_fb)p->telem.max_ciclos_copia_fb=dt;
    p->telem.cuadros_presentados++;p->presentaciones_cabeza^=1;p->presentaciones_pendientes--;
    return 0;
}
static int servir_coordinador(void *u) {
    reproductor *p=u;
    /* Callback dentro de filas: sólo sondeo y cancelación. No leer, AAC,
     * esperar PCM ni copiar framebuffer mientras se posee una dependencia. */
    uint64_t ahora=rdtsc(),c_ms=tiempo_ciclos_por_ms();
    if(ahora-p->ultimo_sondeo>=(c_ms?c_ms:2500000)) {
        xhci_configurar_sondeo_breve(1);atender(p);xhci_configurar_sondeo_breve(0);p->ultimo_sondeo=ahora;
    }
    if(p->cancelado || p->error){h264_cancelar_trabajadores(p->dec_video);trabajos_cancelar_actual();}
    return p->cancelado || p->error;
}
static void servir_lote(void *u){servir_coordinador(u);}
static int ejecutar_lote(void *u,h264_region_fn f,void *ctx,unsigned n) {
    reproductor *p=u;
    int ok=trabajos_ejecutar(f,ctx,n,servir_lote,p);
    return ok && !p->cancelado && !p->error;
}
typedef struct {const h264_imagen *im;uint32_t *rgb;unsigned w,h,n;int sse2;reproductor *p;} rgb_lote;
static void convertir_region(void *u,unsigned i,unsigned cpu) {
    rgb_lote *r=u;uint64_t t=rdtsc();
    h264_convertir_rgb_region(r->im,r->rgb,r->w,r->h,r->h*i/r->n,r->h*(i+1)/r->n,r->sse2);
    observar_h264(r->p,cpu,TR_CONVERSION,r->im->marca_tiempo,t,rdtsc(),i);
}

static int presentar(void *usuario, const h264_imagen *im) {
    reproductor *p = (reproductor *)usuario;
    if (atender(p)) return 1;

    if (!p->cuadros) {
        p->primer_pts = im->marca_tiempo;
        p->inicio = tiempo_obtener_milisegundos();
    }

    p->ciclos_espera_pts_ultimo_cuadro = 0;

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
        if(p->presentaciones_pendientes==2 && presentar_pendiente(p,1))return 1;
        unsigned w = im->ancho, h = im->alto;
        uint64_t sw = pantalla_obtener_ancho(), sh = pantalla_obtener_alto();
        if (!sw || !sh) { p->error = 1; return 1; }
        if (w > sw) { h = (unsigned)((uint64_t)h * sw / w); w = (unsigned)sw; }
        if (h > sh) { w = (unsigned)((uint64_t)w * sh / h); h = (unsigned)sh; }
        if (!w || !h) { p->error = 1; return 1; }

        if (p->ancho != w || p->alto != h) {
            while(p->presentaciones_pendientes)if(presentar_pendiente(p,1))return 1;
            p->presentaciones_cabeza=0;
            if (p->rgb) soltar(p, p->rgb);
            p->rgb = reservar(p, (size_t)w * h * 4 * 2);
            p->ancho = w;
            p->alto = h;
            if (!p->rgb) { p->error = 1; return 1; }
            pantalla_limpiar(0);
        }

        // Medición de conversión YUV -> RGB
        uint64_t t_rgb0 = rdtsc();
        unsigned slot=(p->presentaciones_cabeza+p->presentaciones_pendientes)&1;
        uint32_t *destino_rgb=p->rgb+(size_t)slot*w*h;
        unsigned n=trabajos_cpu_activas();
        rgb_lote r={im,destino_rgb,w,h,n,H264_INTER_SSE2&&x86_64_fpu_sse2_lista(),p};
        if(!ejecutar_lote(p,convertir_region,&r,n)){p->error=1;return 1;}
        p->presentaciones_pts[slot]=im->marca_tiempo;p->presentaciones_pendientes++;
        uint64_t dt_rgb = rdtsc() - t_rgb0;
        traza_registrar(p->traza,0,TR_CONVERSION,p->muestra_actual,im->marca_tiempo,t_rgb0,t_rgb0+dt_rgb,0,audio_ac97_cola_ocupada(),p->presentaciones_pendientes);
        traza_registrar(p->traza,0,TR_LISTO,p->muestra_actual,im->marca_tiempo,t_rgb0+dt_rgb,t_rgb0+dt_rgb,0,audio_ac97_cola_ocupada(),p->presentaciones_pendientes);
        p->telem.ciclos_yuv_rgb += dt_rgb;
        if (dt_rgb > p->telem.max_ciclos_yuv_rgb) p->telem.max_ciclos_yuv_rgb = dt_rgb;

        if(presentar_pendiente(p,0))return 1;
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
        if (p->telem.tiempos_cuadro_us && p->telem.capacidad_tiempos) {
            uint64_t n = ++p->telem.cuadros_tiempo_observados;
            if (p->telem.total_tiempos_grabados < p->telem.capacidad_tiempos) {
                p->telem.tiempos_cuadro_us[p->telem.total_tiempos_grabados++] = dur_us;
            } else {
                /* Reservoir sampling: muestra acotada y representativa de toda la sesión. */
                p->telem.semilla_muestreo = p->telem.semilla_muestreo * 1664525U + 1013904223U;
                uint64_t aleatorio = ((uint64_t)p->telem.semilla_muestreo << 32) | (uint32_t)rdtsc();
                uint64_t indice = aleatorio % n;
                if (indice < p->telem.capacidad_tiempos)
                    p->telem.tiempos_cuadro_us[indice] = dur_us;
            }
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
    const char *perfil_nombre="desconocido";
    uint32_t hilos_cpu = 1;
    uint32_t nucleos_cpu = 1;
    obtener_info_cpu(cpu_str, sizeof(cpu_str), &hilos_cpu, &nucleos_cpu);

    // Obtener telemetría de reconstrucción y desbloqueo del decodificador H.264
    h264_telemetria ht;
    h264_obtener_telemetria(dec, &ht);
    p->telem.ciclos_cabac_reconstruccion = ht.ciclos_sintaxis_reconstruccion+ht.ciclos_reconstruccion_pared;
    p->telem.max_ciclos_cabac = ht.max_ciclos_sintaxis;
    p->telem.ciclos_cabac_puro = ht.ciclos_cabac_puro;
    p->telem.ciclos_inter = ht.ciclos_inter;
    p->telem.ciclos_intra = ht.ciclos_intra;
    p->telem.ciclos_desbloqueo = ht.ciclos_desbloqueo;
    p->telem.max_ciclos_desbloqueo = ht.max_ciclos_desbloqueo;
    p->telem.cuadros_decodificados = (uint32_t)ht.cuadros_decodificados;
    if(ht.perfil==66)perfil_nombre="Baseline";
    else if(ht.perfil==77)perfil_nombre="Main";
    else if(ht.perfil==88)perfil_nombre="Extended";
    else if(ht.perfil==100)perfil_nombre="High";

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
    /* El servicio AAC/USB puede ejecutarse dentro de la pared de un lote.
     * Usar pared observada; sumar esas subetapas contaría tiempo dos veces. */
    uint64_t ciclos_activos = p->telem.ciclos_pared_sesion>p->telem.ciclos_espera_pts?
        p->telem.ciclos_pared_sesion-p->telem.ciclos_espera_pts:0;

    // Emisión del reporte estructurado tanto a pantalla como a serial
    telem_println("");
    telem_println("================== INFORME DE TELEMETRÍA Y RENDIMIENTO TAEK OS ==================");
    telem_print("  Versión del Kernel    : TAEK OS ");
    telem_print(taek_obtener_version());
    telem_print(" / ");
    telem_print(taek_obtener_hito());
    telem_print(" (Compilado: ");
    telem_print(taek_obtener_fecha_compilacion());
    telem_print(" ");
    telem_print(taek_obtener_hora_compilacion());
    telem_println(")");
    telem_print("  Revisión de fuentes   : ");telem_println(TAEK_REVISION_CODIGO);

    telem_print("  Procesador Detectado  : ");
    telem_print(cpu_str);
    telem_print(" (");
    if(hilos_cpu)telem_print_dec(hilos_cpu);else telem_print("desconocidos");
    telem_print(" lógicos del paquete BSP; ");
    if(nucleos_cpu)telem_print_dec(nucleos_cpu);else telem_print("desconocidos");
    telem_println(" núcleos; topología CPUID, sin enumeración de APs)");
    telem_print("  Topología activa      : ");telem_print_dec(trabajos_cpu_arrancadas());
    telem_print(" procesadores arrancados; ");telem_print_dec(trabajos_cpu_activas()-1);telem_println(" trabajadores y BSP coordinador");
    telem_println("  Medición              : pared BSP; subetapas de servicio pueden superponerse con lotes");

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

    telem_print("  Archivo / fuente      : ");
    telem_println(p->fuente ? p->fuente : "(sin nombre)");
    telem_print("  Video H.264           : ");
    if (ht.ancho_visible && ht.alto_visible) {
        telem_print_dec(ht.ancho_visible);telem_print("x");telem_print_dec(ht.alto_visible);
        telem_print(" visible; codificada ");telem_print_dec(ht.ancho_codificado);telem_print("x");telem_print_dec(ht.alto_codificado);
        telem_print("; perfil ");telem_print(perfil_nombre);telem_print(" (IDC ");telem_print_dec(ht.perfil);
        telem_print("); nivel ");telem_print_dec(ht.nivel);
    } else telem_print("SPS aún no disponible");
    telem_println("");
    telem_print("  Duración / frecuencia: ");
    if (p->mp4 && p->mp4->v_escala_tiempo && p->mp4->v_duracion_ticks && p->mp4->v_muestras) {
        uint64_t segundos=p->mp4->v_duracion_ticks/p->mp4->v_escala_tiempo;
        uint64_t resto=p->mp4->v_duracion_ticks%p->mp4->v_escala_tiempo;
        uint64_t ms=segundos*1000+(resto*1000)/p->mp4->v_escala_tiempo;
        uint64_t duracion_us=segundos*1000000+(resto*1000000)/p->mp4->v_escala_tiempo;
        uint64_t presupuesto_us=duracion_us/p->mp4->v_muestras;
        uint64_t fps_mil=presupuesto_us?UINT64_C(1000000000)/presupuesto_us:0;
        telem_print_dec(ms);telem_print(" ms, ");telem_print_dec(fps_mil/1000);telem_print(".");
        if ((fps_mil%1000)<100) telem_print("0");
        if ((fps_mil%1000)<10) telem_print("0");
        telem_print_dec(fps_mil%1000);telem_println(" FPS promedio de la pista");
    } else telem_println("sin tabla de tiempos válida");
    telem_print("  Presupuesto por cuadro: ");
    if (p->mp4 && p->mp4->v_escala_tiempo && p->mp4->v_duracion_ticks && p->mp4->v_muestras) {
        uint64_t q=p->mp4->v_duracion_ticks/p->mp4->v_escala_tiempo;
        uint64_t r=p->mp4->v_duracion_ticks%p->mp4->v_escala_tiempo;
        uint64_t us=(q*1000000+(r*1000000)/p->mp4->v_escala_tiempo)/p->mp4->v_muestras;
        telem_print_dec(us/1000);telem_print(".");
        if ((us%1000)<100) telem_print("0");
        if ((us%1000)<10) telem_print("0");
        telem_print_dec(us%1000);telem_println(" ms/frame según stts");
    } else telem_println("no disponible");
    telem_print("  Ruta de compensación : ");
#ifdef H264_INTER_ESCALAR
    telem_println("ESCALAR DE REFERENCIA (H264_INTER_ESCALAR)");
#else
    telem_println(ht.inter_sse2?"SSE2 POR BLOQUE":"KERNELS ESCALARES POR BLOQUE");
#endif

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
    telem_println(" del total decodificado)");

    telem_print("  Descartes por Tardanza : ");
    telem_print_dec(p->cuadros_tardios);
    telem_println(" (LATE_DROPS; sólo presentación, jamás decodificación)");

    telem_print("  Omitidos sin Presentar : ");
    telem_print_dec(p->telem.cuadros_omitidos);
    telem_println(" (otros: p. ej. presentación selectiva forense)");

    if (duracion_total_ms > 0) {
        uint64_t fpsd_ent = (uint64_t)p->telem.cuadros_decodificados * 1000 / duracion_total_ms;
        uint64_t fpsd_dec = ((uint64_t)p->telem.cuadros_decodificados * 100000 / duracion_total_ms) % 100;
        uint64_t fpsp_ent = (uint64_t)p->telem.cuadros_presentados * 1000 / duracion_total_ms;
        uint64_t fpsp_dec = ((uint64_t)p->telem.cuadros_presentados * 100000 / duracion_total_ms) % 100;
        telem_print("  FPS Decodificados     : ");
        telem_print_dec(fpsd_ent);telem_print(".");
        if (fpsd_dec < 10) telem_print("0");
        telem_print_dec(fpsd_dec);
        telem_println(" FPS");
        telem_print("  FPS Presentados       : ");
        telem_print_dec(fpsp_ent);telem_print(".");
        if (fpsp_dec < 10) telem_print("0");
        telem_print_dec(fpsp_dec);
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

    telem_println("DESGLOSE DE TIEMPOS POR ETAPA (INTERVALOS INCLUSIVOS; NO SUMAR ENTRE SÍ):");
    telem_println("  Etapa                     |  Ciclos Totales   |  Tiempo (ms)  |  % Pared   |  Máx Cuadro");
    telem_println("  --------------------------+-------------------+---------------+------------+------------");
    telem_print_fila_etapa("1. Demux MP4", p->telem.ciclos_demux, p->telem.max_ciclos_demux, ciclos_activos, c_ms);
    telem_print_fila_etapa("2. Decodificación AAC", p->telem.ciclos_aac, p->telem.max_ciclos_aac, ciclos_activos, c_ms);
    telem_print_fila_etapa("3. Pared sintaxis+recon", p->telem.ciclos_cabac_reconstruccion, p->telem.max_ciclos_cabac, ciclos_activos, c_ms);
    telem_print_fila_etapa("   - CABAC Sintaxis pura", p->telem.ciclos_cabac_puro, 0, ciclos_activos, c_ms);
    telem_print_fila_etapa("   - Reconstrucción pared", ht.ciclos_reconstruccion_pared, 0, ciclos_activos, c_ms);
    telem_print_fila_etapa("   - Cómputo trabajadores", ht.ciclos_reconstruccion_cpu, 0, ciclos_activos, c_ms);
    telem_print_fila_etapa("   - Dependencias AP", ht.ciclos_dependencias_cpu, 0, 0, c_ms);
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

    telem_println("DESBLOQUEO POR COMPONENTE (C1):");
    telem_print("  Segmentos evaluados/filtrados/desc.: ");
    telem_print_dec(ht.deblock_bordes_evaluados);telem_print(" / ");
    telem_print_dec(ht.deblock_bordes_filtrados);telem_print(" / ");
    telem_print_dec(ht.deblock_bordes_descartados);telem_println("");
    telem_print("  Filtrados H / V ; Luma / Croma    : ");
    telem_print_dec(ht.deblock_filtrados_h);telem_print(" / ");
    telem_print_dec(ht.deblock_filtrados_v);telem_print(" ; ");
    telem_print_dec(ht.deblock_luma);telem_print(" / ");
    telem_print_dec(ht.deblock_croma);telem_println("");
    telem_print("  Fuerza tipo / nz / movimiento     : ");
    telem_print_dec(ht.deblock_fuerza_tipo);telem_print(" / ");
    telem_print_dec(ht.deblock_fuerza_nz);telem_print(" / ");
    telem_print_dec(ht.deblock_fuerza_movimiento);telem_println("");
    telem_print("  Saltos MB filtro/marco/8x8/slice  : ");
    telem_print_dec(ht.deblock_skip_filtro);telem_print(" / ");
    telem_print_dec(ht.deblock_skip_marco);telem_print(" / ");
    telem_print_dec(ht.deblock_skip_8x8);telem_print(" / ");
    telem_print_dec(ht.deblock_skip_slice);telem_println("");
    telem_print("  Kernel SSE2 / escalar             : ");
    telem_print_dec(ht.deblock_kernel_sse2);telem_print(" / ");
    telem_print_dec(ht.deblock_kernel_escalar);telem_println("");
    telem_print("  Ciclos fuerza / kernel (H / V)    : ");
    telem_print_ms(ht.ciclos_deblock_fuerza, c_ms);telem_print(" / ");
    telem_print_ms(ht.ciclos_deblock_kernel, c_ms);telem_print(" (");
    telem_print_ms(ht.ciclos_deblock_h, c_ms);telem_print(" / ");
    telem_print_ms(ht.ciclos_deblock_v, c_ms);
    telem_println(") ms");
    telem_println("  (ciclos por componente sólo con telemetría detallada; los recuentos siempre)");
    telem_println("---------------------------------------------------------------------------------");

    telem_println("MÉTRICAS DE CAMINO CRÍTICO Y LATENCIA:");
    uint64_t presupuesto_cuadro_us=33333;
    if (p->mp4 && p->mp4->v_escala_tiempo && p->mp4->v_duracion_ticks && p->mp4->v_muestras) {
        uint64_t q=p->mp4->v_duracion_ticks/p->mp4->v_escala_tiempo;
        uint64_t r=p->mp4->v_duracion_ticks%p->mp4->v_escala_tiempo;
        presupuesto_cuadro_us=(q*1000000+(r*1000000)/p->mp4->v_escala_tiempo)/p->mp4->v_muestras;
    }
    telem_print("  Presupuesto de Cuadro (pista)  : ");
    telem_print_dec(presupuesto_cuadro_us/1000);telem_print(".");
    if ((presupuesto_cuadro_us%1000)<100) telem_print("0");
    if ((presupuesto_cuadro_us%1000)<10) telem_print("0");
    telem_print_dec(presupuesto_cuadro_us%1000);telem_println(" ms");

    telem_print("  Camino Crítico Promedio        : ");
    telem_print_dec(prom_us / 1000);
    telem_print(".");
    if ((prom_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((prom_us % 1000) / 10);
    telem_print(" ms (");
    telem_print_dec(presupuesto_cuadro_us ? (uint64_t)prom_us * 100 / presupuesto_cuadro_us : 0);
    telem_println("% del presupuesto)");

    telem_print("  Percentil p50 (Mediana, aprox.) : ");
    telem_print_dec(p50_us / 1000);
    telem_print(".");
    if ((p50_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((p50_us % 1000) / 10);
    telem_println(" ms");

    telem_print("  Percentil p95 (aprox.)         : ");
    telem_print_dec(p95_us / 1000);
    telem_print(".");
    if ((p95_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((p95_us % 1000) / 10);
    telem_print(" ms");
    if (p95_us < presupuesto_cuadro_us) {
        telem_println("  [p95 bajo el presupuesto temporal de la pista]");
    } else {
        telem_println("  [EXCEDE PRESUPUESTO]");
    }

    telem_print("  Percentil p99 (aprox.)         : ");
    telem_print_dec(p99_us / 1000);
    telem_print(".");
    if ((p99_us % 1000) / 10 < 10) telem_print("0");
    telem_print_dec((p99_us % 1000) / 10);
    telem_println(" ms");

    telem_print("  Cuadros medidos / muestra      : ");
    telem_print_dec(p->telem.cuadros_tiempo_observados);
    telem_print(" / ");
    telem_print_dec(p->telem.total_tiempos_grabados);
    telem_println("");

    telem_print("  Retraso Acumulado vs PTS       : ");
    telem_print_dec(p->telem.retraso_pts_acumulado_us / 1000);
    telem_print(" ms (Retraso Máximo: ");
    telem_print_dec(p->telem.retraso_pts_max_us / 1000);
    telem_println(" ms)");
    telem_println("---------------------------------------------------------------------------------");

    telem_println("SUBSISTEMA DE AUDIO (A4):");
    telem_print("  PCM Producido por AAC          : ");
    telem_print_dec(p->telem.pcm_producido_bytes);
    telem_println(" bytes generados");

    telem_print("  PCM Aceptado en Cola           : ");
    telem_print_dec(p->telem.pcm_aceptado_bytes);
    telem_println(" bytes encolados");

    telem_print("  Avance DMA Hardware Total      : ");
    telem_print_dec(p->telem.avance_dma_bytes);
    telem_println(" bytes procesados por silicio");

    telem_print("  Silencio Insertado (Underrun)  : ");
    telem_print_dec(p->telem.silencio_insertado_bytes);
    telem_println(" bytes de relleno");

    telem_print("  Silencio en Pista / Tras EOF   : ");
    telem_print_dec(p->silencio_en_pista);
    telem_print(" bytes / ");
    telem_print_dec(p->silencio_despues_eof);
    telem_println(" bytes");

    telem_print("  Vaciados de Búfer Registrados  : ");
    telem_print_dec(p->telem.vaciados_audio);
    telem_print(" (En pista: ");
    telem_print_dec(p->vaciados_en_pista);
    telem_print(", Tras EOF: ");
    telem_print_dec(p->vaciados_eof);
    telem_println(")");
    telem_println("---------------------------------------------------------------------------------");

    telem_println("PIPELINE I/O Y ALMACENAMIENTO:");
    telem_print("  Origen de la Fuente            : ");
    telem_println(p->lector_original ? "USB / VFS Progresivo" : "RAM (Memoria Directa)");

    telem_print("  Tiempo de Lectura a la Fuente  : ");
    telem_print_ms(p->ciclos_lectura, c_ms);
    telem_print(" (");
    telem_print_dec(p->bytes_lectura);
    telem_print(" bytes en ");
    telem_print_dec(p->lecturas);
    telem_println(" llamadas)");

    uint64_t cmd_usb = 0, bytes_usb = 0;
    usb_msc_obtener_lecturas(&cmd_usb, &bytes_usb);
    uint64_t cmd_sesion = cmd_usb - p->usb_comandos_inicio;
    uint64_t b_sesion = bytes_usb - p->usb_bytes_inicio;
    telem_print("  Comandos USB READ(10)          : ");
    telem_print_dec(cmd_sesion);
    telem_print(" comandos (");
    telem_print_dec(b_sesion);
    telem_println(" bytes fisicos)");

    if (p->anillo) {
        telem_print("  Anillo almac. (productor/consumidor): ");
        telem_print_dec(p->anillo->aciertos);
        telem_print(" aciertos; ");
        telem_print_dec(p->anillo->starvations);
        telem_print(" starvations; ");
        telem_print_dec(p->anillo->productor_paradas);
        telem_print(" prod. paradas; ");
        telem_print_dec(p->anillo->bloques_leidos);
        telem_println(" bloques prefetched");
        telem_print("    - margen prefetch min/media/max  : ");
        telem_print_dec(p->anillo->margen_muestras ? p->anillo->margen_min : 0);
        telem_print(" / ");
        telem_print_dec(p->anillo->margen_muestras ? p->anillo->margen_suma / p->anillo->margen_muestras : 0);
        telem_print(" / ");
        telem_print_dec(p->anillo->margen_max);
        telem_println(" bytes");
        telem_print("    - bytes prefetched utiles/inutiles: ");
        telem_print_dec(p->anillo->bytes_utiles);
        telem_print(" / ");
        telem_print_dec(p->anillo->bytes_inutiles);
        telem_println(" bytes");
    }

    telem_print("  Ventana I/O / Modo BSP         : ");
    telem_print_dec(diagnostico_lectura_kib);
    telem_print(" KiB");
    if (diagnostico_lectura_kib == 4) telem_print(" (referencia de comparación)");
    telem_print(" / ");
    telem_println((!diagnostico_coordinador || trabajos_cpu_activas() == 1) ? "BSP Calcula" : "BSP Coordinador");

    telem_print("  Fallos de Disponibilidad (Miss): Video: ");
    telem_print_dec(p->video_comprimido ? p->video_comprimido->faltantes : 0);
    telem_print(" | Audio: ");
    telem_print_dec(p->audio_comprimido ? p->audio_comprimido->faltantes : 0);
    telem_println("");

    telem_print("  Cuadros Tardios Descartados    : ");
    telem_print_dec(p->cuadros_tardios);
    telem_println(" (LATE_DROPS)");
    telem_println("---------------------------------------------------------------------------------");

    telem_println("PARALELISMO Y COORDINACIÓN DE RECONSTRUCCIÓN:");
    telem_print("  Trabajadores Activos           : ");
    telem_print_dec(trabajos_cpu_activas());
    telem_print(" CPUs (");
    telem_println((!diagnostico_coordinador || trabajos_cpu_activas() == 1) ? "BSP participando en cálculo" : "BSP exclusivo coordinador");

    telem_print("  Pared de Reconstrucción (Wall) : ");
    telem_print_ms(ht.ciclos_reconstruccion_pared, c_ms);
    telem_print(" (");
    telem_print_dec(ht.lotes_reconstruccion);
    telem_print(" lotes, ");
    telem_print_dec(ht.regiones_reconstruccion);
    telem_print(" filas)");
    telem_println("");

    telem_print("  Cómputo Total de CPUs en Filas : ");
    telem_print_ms(ht.ciclos_reconstruccion_cpu, c_ms);
    telem_println(" (suma entre trabajadores; usar su reloj)");

    telem_print("  Espera por Dependencias (AP)   : ");
    telem_print_ms(ht.ciclos_dependencias_cpu, c_ms);
    telem_println("");

    telem_print("  Regiones OK / Despachadas      : ");
    telem_print_dec(ht.regiones_ok_reconstruccion);
    telem_print(" / ");
    telem_print_dec(ht.regiones_reconstruccion);
    if (ht.regiones_ok_reconstruccion == ht.regiones_reconstruccion) {
        telem_println(" (exactamente una vez)");
    } else {
        telem_println(" [ANOMALÍA: región perdida o duplicada]");
    }

    telem_print("  Regiones Duplicadas / Faltantes: ");
    telem_print_dec(ht.regiones_duplicadas);
    telem_print(" / ");
    telem_print_dec(ht.regiones_faltantes);
    telem_println("");

    telem_print("  Relación Cómputo/Pared (peor)  : ");
    telem_print_dec(ht.ratio_worst_miles / 1000);
    telem_print(".");
    telem_print_dec(ht.ratio_worst_miles % 1000);
    telem_print(" x; límite ");
    telem_print_dec(ht.trabajadores);
    telem_print(" x; lotes incoherentes: ");
    telem_print_dec(ht.lotes_incoherentes);
    telem_println(ht.lotes_incoherentes ? " [REVISAR RELOJES POR CPU]" : " [coherente]");

    for (unsigned i = 0; i < ht.trabajadores && i < 4; i++) {
        telem_print("    Ejecutor ");
        telem_print_dec(i);
        telem_print(": regiones=");
        telem_print_dec(ht.regiones_ejecutor[i]);
        telem_print(" pared=");
        telem_print_ms(ht.pared_ejecutor[i], c_ms);
        telem_print(" ms cómputo=");
        telem_print_ms(ht.computo_ejecutor[i], c_ms);
        telem_print(" ms espera=");
        telem_print_ms(ht.espera_ejecutor[i], c_ms);
        telem_print(" ms; TSC=");
        telem_print_dec(trabajos_ticks_por_ms(i));
        telem_println(" ciclos/ms");
    }
    telem_println("---------------------------------------------------------------------------------");

    telem_println("MEMORIA DEL SISTEMA:");
    telem_print("  Heap antes de comenzar         : ");
    telem_print_dec(p->heap_inicial);
    telem_println(" bytes (referencia base de la sesión)");

    telem_print("  Memoria Máxima Dinámica en Uso : ");
    telem_print_dec(p->memoria_maxima / (1024 * 1024));
    telem_print(" MB (");
    telem_print_dec(p->memoria_maxima);
    telem_println(" bytes / Límite 256 MB)");

    telem_print("  Memoria Remanente sin Liberar  : ");
    telem_print_dec(p->memoria_usada);
    telem_println(" bytes");

    memoria_estadisticas_t mem_actual;
    memoria_obtener_estadisticas(&mem_actual);
    uint64_t heap_delta = mem_actual.heap_bytes_en_uso >= p->heap_inicial ?
                          mem_actual.heap_bytes_en_uso - p->heap_inicial : 0;
    telem_print("  Heap al terminar la sesión     : ");
    telem_print_dec(mem_actual.heap_bytes_en_uso);
    telem_println(" bytes (antes de liberar buffers)");

    telem_print("  Delta de Heap durante Sesión   : ");
    telem_print_dec(heap_delta);
    telem_println(" bytes (HEAP_DELTA en el informe final tras liberar)");

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
    serial_imprimir("[BENCHMARK] SOURCE_REVISION=");serial_imprimir_linea(TAEK_REVISION_CODIGO);
    serial_imprimir("[BENCHMARK] FUNCTIONAL_SAMPLE_LIMIT=");serial_imprimir_dec(muestras_funcionales);serial_imprimir_linea("");
    x86_64_capacidades_simd simd_cpu=x86_64_fpu_capacidades_cpu();
    serial_imprimir("[BENCHMARK] AVX2_CPUID=");serial_imprimir_dec(simd_cpu.avx2);
    serial_imprimir(" YMM_ENABLED=");serial_imprimir_dec(simd_cpu.ymm_habilitado);serial_imprimir_linea(" AVX2_SELECTED=0");
    serial_imprimir("[BENCHMARK] TELEMETRIA_DETALLADA=");serial_imprimir_dec(H264_TELEMETRIA_DETALLADA);serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] CLOCK_SCOPE=BSP_WALL_CYCLES WORKER_CPU_CYCLES=");
    serial_imprimir_dec(ht.ciclos_reconstruccion_cpu);serial_imprimir(" RECON_WALL_CYCLES=");
    serial_imprimir_dec(ht.ciclos_reconstruccion_pared);serial_imprimir(" DEPENDENCY_CPU_CYCLES=");
    serial_imprimir_dec(ht.ciclos_dependencias_cpu);serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] RECON_LOTES=");serial_imprimir_dec(ht.lotes_reconstruccion);
    serial_imprimir(" RECON_REGIONES=");serial_imprimir_dec(ht.regiones_reconstruccion);
    serial_imprimir(" RECON_REGIONES_OK=");serial_imprimir_dec(ht.regiones_ok_reconstruccion);
    serial_imprimir(" RECON_REGIONES_DUP=");serial_imprimir_dec(ht.regiones_duplicadas);
    serial_imprimir(" RECON_REGIONES_FALTA=");serial_imprimir_dec(ht.regiones_faltantes);
    serial_imprimir(" RECON_PARTICIPANTES=");serial_imprimir_dec(ht.trabajadores);
    serial_imprimir(" RECON_RATIO_MILES=");serial_imprimir_dec(ht.ratio_worst_miles);
    serial_imprimir(" RECON_INCOHERENTES=");serial_imprimir_dec(ht.lotes_incoherentes);
    serial_imprimir_linea("");
    for(unsigned i=0;i<ht.trabajadores && i<4;i++){
        serial_imprimir("[BENCHMARK] RECON_EJECUTOR=");serial_imprimir_dec(i);
        serial_imprimir(" REGIONES=");serial_imprimir_dec(ht.regiones_ejecutor[i]);
        serial_imprimir(" PARED=");serial_imprimir_dec(ht.pared_ejecutor[i]);
        serial_imprimir(" COMPUTO=");serial_imprimir_dec(ht.computo_ejecutor[i]);
        serial_imprimir(" ESPERA=");serial_imprimir_dec(ht.espera_ejecutor[i]);
        serial_imprimir(" TICKS_PER_MS=");serial_imprimir_dec(trabajos_ticks_por_ms(i));
        serial_imprimir_linea("");
    }
    serial_imprimir("[BENCHMARK] FPS_DECODIFICADOS_MILES=");
    serial_imprimir_dec(duracion_total_ms?((uint64_t)p->telem.cuadros_decodificados*100000/duracion_total_ms):0);
    serial_imprimir(" FPS_PRESENTADOS_MILES=");
    serial_imprimir_dec(duracion_total_ms?((uint64_t)p->telem.cuadros_presentados*100000/duracion_total_ms):0);
    serial_imprimir(" LATE_DROPS=");serial_imprimir_dec(p->cuadros_tardios);
    serial_imprimir(" OMITIDOS=");serial_imprimir_dec(p->telem.cuadros_omitidos);
    serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] SESSION_WALL_CYCLES=");serial_imprimir_dec(p->telem.ciclos_pared_sesion);
    serial_imprimir(" ACTIVE_WALL_CYCLES=");serial_imprimir_dec(ciclos_activos);serial_imprimir_linea(" SERVICE_OVERLAP=1");
    serial_imprimir("[BENCHMARK] TRANSFORM_CYCLES=");serial_imprimir_dec(ht.ciclos_transformadas);
    serial_imprimir(" SSE2_STAGE_MASK=");serial_imprimir_dec(ht.etapas_sse2);
    serial_imprimir(" RESIDUAL_ADD_CYCLES=");serial_imprimir_dec(ht.ciclos_suma_residuo);
    serial_imprimir(" HADAMARD_CYCLES=");serial_imprimir_dec(ht.ciclos_hadamard);
    serial_imprimir(" RECON_MEMORY_BYTES=");serial_imprimir_dec(ht.presupuesto_reconstruccion_bytes);
    serial_imprimir(" AUDIO_SERVICE_MAX_MS=");serial_imprimir_dec(p->servicio_max_ms);
    serial_imprimir(" AUDIO_UNDERRUN_TRACK=");serial_imprimir_dec(p->vaciados_en_pista);
    serial_imprimir(" AUDIO_EOF_EVENTS=");serial_imprimir_dec(p->vaciados_eof);
    serial_imprimir(" SILENCE_TRACK_BYTES=");serial_imprimir_dec(p->silencio_en_pista);
    serial_imprimir(" SILENCE_EOF_BYTES=");serial_imprimir_dec(p->silencio_despues_eof);
    serial_imprimir(" AUDIO_TARGET_MS=750 AUDIO_LOW_MS=450 LATE_POLICY=");
    serial_imprimir_linea(p->modo==MODO_INTERACTIVO?"DROP_STALE RGB_QUEUE=2":"PRESENT_ALL RGB_QUEUE=2");
#if H264_PERFIL_INTER
    static const char *etapas_inter[H264_INTER_ETAPAS]={"ENTERO","HORIZONTAL","VERTICAL","DIAGONAL_CRUZADA",
        "CROMA","SIMPLE","DOBLE","PONDERADA","BORDES"};
    serial_imprimir_linea("[BENCHMARK] INTER_PROFILE=1 NESTED_IN_INTER=1");
    for(int etapa=0;etapa<H264_INTER_ETAPAS;etapa++) {
        serial_imprimir("[INTER] ETAPA=");serial_imprimir(etapas_inter[etapa]);
        serial_imprimir(" CICLOS=");serial_imprimir_dec(ht.ciclos_inter_etapa[etapa]);
        serial_imprimir(" BLOQUES=");serial_imprimir_dec(ht.bloques_inter_etapa[etapa]);
        serial_imprimir(" PIXELES=");serial_imprimir_dec(ht.pixeles_inter_etapa[etapa]);serial_imprimir_linea("");
    }
#else
    serial_imprimir_linea("[BENCHMARK] INTER_PROFILE=0");
#endif
    serial_imprimir("[BENCHMARK] CPU=\""); serial_imprimir(cpu_str);
    serial_imprimir("\" LOGICAL_CPUS="); serial_imprimir_dec(hilos_cpu);
    serial_imprimir(" PHYSICAL_CORES=");serial_imprimir_dec(nucleos_cpu);
    serial_imprimir(" CPUS_DETECTED=");serial_imprimir_dec(trabajos_cpu_detectadas());
    serial_imprimir(" CPUS_STARTED=");serial_imprimir_dec(trabajos_cpu_arrancadas());
    serial_imprimir(" WORKERS_ACTIVE=");serial_imprimir_dec(trabajos_cpu_activas()-1);
    serial_imprimir(" TSC_MHZ="); serial_imprimir_dec(tsc_mhz); serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] SOURCE=\"");serial_imprimir(p->fuente?p->fuente:"(sin nombre)");serial_imprimir("\" FRAMES="); serial_imprimir_dec(p->telem.cuadros_decodificados);
    serial_imprimir(" DURACION_MS="); serial_imprimir_dec(duracion_total_ms);
    serial_imprimir(" TRACK_TICKS=");serial_imprimir_dec(p->mp4?p->mp4->v_duracion_ticks:0);
    serial_imprimir(" TIMESCALE=");serial_imprimir_dec(p->mp4?p->mp4->v_escala_tiempo:0);
    serial_imprimir(" VISIBLE=");serial_imprimir_dec(ht.ancho_visible);serial_imprimir("x");serial_imprimir_dec(ht.alto_visible);
    serial_imprimir(" CODED=");serial_imprimir_dec(ht.ancho_codificado);serial_imprimir("x");serial_imprimir_dec(ht.alto_codificado);
    serial_imprimir(" PROFILE_IDC=");serial_imprimir_dec(ht.perfil);
#ifdef H264_INTER_ESCALAR
    serial_imprimir(" INTER_PATH=SCALAR_REFERENCE");
#else
    serial_imprimir(ht.inter_sse2?" INTER_PATH=BLOCK_SSE2":" INTER_PATH=BLOCK_SCALAR");
#endif
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
    serial_imprimir("[BENCHMARK] DEBLOCK_SEG_EVAL=");serial_imprimir_dec(ht.deblock_bordes_evaluados);
    serial_imprimir(" DEBLOCK_SEG_FILT=");serial_imprimir_dec(ht.deblock_bordes_filtrados);
    serial_imprimir(" DEBLOCK_SEG_DESC=");serial_imprimir_dec(ht.deblock_bordes_descartados);
    serial_imprimir(" DEBLOCK_H=");serial_imprimir_dec(ht.deblock_filtrados_h);
    serial_imprimir(" DEBLOCK_V=");serial_imprimir_dec(ht.deblock_filtrados_v);
    serial_imprimir(" DEBLOCK_LUMA=");serial_imprimir_dec(ht.deblock_luma);
    serial_imprimir(" DEBLOCK_CROMA=");serial_imprimir_dec(ht.deblock_croma);
    serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] DEBLOCK_FUERZA_TIPO=");serial_imprimir_dec(ht.deblock_fuerza_tipo);
    serial_imprimir(" DEBLOCK_FUERZA_NZ=");serial_imprimir_dec(ht.deblock_fuerza_nz);
    serial_imprimir(" DEBLOCK_FUERZA_MOV=");serial_imprimir_dec(ht.deblock_fuerza_movimiento);
    serial_imprimir(" DEBLOCK_SKIP_FILTRO=");serial_imprimir_dec(ht.deblock_skip_filtro);
    serial_imprimir(" DEBLOCK_SKIP_MARCO=");serial_imprimir_dec(ht.deblock_skip_marco);
    serial_imprimir(" DEBLOCK_SKIP_8X8=");serial_imprimir_dec(ht.deblock_skip_8x8);
    serial_imprimir(" DEBLOCK_SKIP_SLICE=");serial_imprimir_dec(ht.deblock_skip_slice);
    serial_imprimir_linea("");
    serial_imprimir("[BENCHMARK] DEBLOCK_KERNEL_SSE2=");serial_imprimir_dec(ht.deblock_kernel_sse2);
    serial_imprimir(" DEBLOCK_KERNEL_ESCALAR=");serial_imprimir_dec(ht.deblock_kernel_escalar);
    serial_imprimir(" DEBLOCK_CICLOS_FUERZA=");serial_imprimir_dec(ht.ciclos_deblock_fuerza);
    serial_imprimir(" DEBLOCK_CICLOS_KERNEL=");serial_imprimir_dec(ht.ciclos_deblock_kernel);
    serial_imprimir(" DEBLOCK_CICLOS_H=");serial_imprimir_dec(ht.ciclos_deblock_h);
    serial_imprimir(" DEBLOCK_CICLOS_V=");serial_imprimir_dec(ht.ciclos_deblock_v);
    serial_imprimir_linea("");
}

static int reproductor_ejecutar(mp4_contenedor *mp4, const char *nombre, enum modo_reproduccion modo,
                                int descriptor_vfs_video, int descriptor_vfs_audio) {
    if (!mp4 || !nombre) return -1;
    if (!mp4_tiene_video(mp4)) {
        consola_imprimir_linea("El archivo MP4 no contiene pista de video compatible.");
        return -3;
    }

    reproductor p = {
        .modo = modo,
        .fuente = nombre,
        .descriptor_vfs_video = descriptor_vfs_video,
        .descriptor_vfs_audio = descriptor_vfs_audio,
        .escala = mp4->v_escala_tiempo ? mp4->v_escala_tiempo : 90000,
        .huella = UINT64_C(14695981039346656037)
    };

    memoria_estadisticas_t antes, despues;
    memoria_obtener_estadisticas(&antes);
    p.heap_inicial = antes.heap_bytes_en_uso;
    // Reservar búfer para percentiles de latencia (hasta 5000 cuadros)
    p.telem.capacidad_tiempos = 5000;
    p.telem.tiempos_cuadro_us = (uint32_t *)reservar(&p, sizeof(uint32_t) * p.telem.capacidad_tiempos);

    h264_servicios servicios = { &p, reservar, soltar, presentar };

    h264_decodificador *dec = h264_crear(&servicios);
    h264_configurar_sse2(dec,H264_INTER_SSE2&&x86_64_fpu_sse2_lista());
    if (!dec) {
        consola_imprimir_linea("Memoria insuficiente para H.264.");
        if (p.telem.tiempos_cuadro_us) soltar(&p, p.telem.tiempos_cuadro_us);
        return -4;
    }

    aac_decodificador *dec_aac = NULL;
    if (mp4_tiene_audio(mp4) && modo == MODO_INTERACTIVO) {
        aac_servicios serv_aac = {
            .usuario = &p,
            .asignar = reservar,
            .liberar = soltar
        };
        dec_aac = aac_crear(&serv_aac);
        if (dec_aac) {
            aac_configurar(dec_aac, (int)mp4->a_canales, (int)mp4->a_frecuencia);
            serial_imprimir_linea("[AAC] Decodificador AAC inicializado.");
        }
    }

    p.dec_aac = dec_aac;
    p.mp4 = mp4;
    p.dec_video=dec;
    p.traza=reservar(&p,sizeof(*p.traza));
    if(p.traza)memset(p.traza,0,sizeof(*p.traza));
    p.pausa=reservar(&p,sizeof(*p.pausa));
    h264_configurar_traza(dec,observar_h264,&p);
    p.lector_original=mp4->leer_fuente;p.lector_contexto=mp4->fuente_contexto;
    if(p.lector_original) {
        p.anillo=reservar(&p,sizeof(*p.anillo));
        if(p.anillo) {
            anillo_iniciar(p.anillo,leer_medido,&p,validar_cache,&p,mp4->bytes);
            mp4->leer_fuente=anillo_leer;mp4->fuente_contexto=p.anillo;
        } else {mp4->leer_fuente=leer_medido;mp4->fuente_contexto=&p;}
    }
    usb_msc_obtener_lecturas(&p.usb_comandos_inicio,&p.usb_bytes_inicio);
    usb_msc_telemetria_reiniciar();
    p.video_comprimido=reservar(&p,sizeof(cola_muestras));
    p.audio_comprimido=dec_aac?reservar(&p,sizeof(cola_muestras)):NULL;
    uint8_t *datos_video=reservar(&p,8u*1024*1024);
    uint8_t *datos_audio=dec_aac?reservar(&p,2u*1024*1024):NULL;
    if(!p.video_comprimido || !datos_video || (dec_aac && (!p.audio_comprimido || !datos_audio)))p.error=1;
    if(p.video_comprimido)cola_muestras_iniciar(p.video_comprimido,mp4,MP4_FUENTE_VIDEO,datos_video,datos_video?8u*1024*1024:0);
    if(p.audio_comprimido)cola_muestras_iniciar(p.audio_comprimido,mp4,MP4_FUENTE_AUDIO,datos_audio,datos_audio?2u*1024*1024:0);
    if(p.video_comprimido){p.video_comprimido->observar_indice=observar_indice;p.video_comprimido->usuario_observador=&p;}
    if(p.audio_comprimido){p.audio_comprimido->observar_indice=observar_indice;p.audio_comprimido->usuario_observador=&p;}
    /* También con una CPU: misma ruta de sintaxis/reconstrucción SSE2. */
    h264_configurar_trabajadores(dec,ejecutar_lote,&p,trabajos_cpu_activas(),servir_coordinador);

    // Prebuffer inicial de audio para cebar el pipeline DMA (128 KiB = ambos bloques DMA de 64 KiB + margen)
    if (dec_aac && mp4->tiene_audio && modo == MODO_INTERACTIVO) {
        serial_imprimir_linea("[MULTIMEDIA] Precargando buffer de audio AAC (lookahead)...");
        // Precargar al menos 160 KiB de PCM (~900 ms de audio) o hasta agotar la pista de audio
        while (!p.error && !p.cancelado &&
               mp4->a_indice < mp4->a_muestras && audio_ac97_cola_ocupada() < (160 * 1024)) {
            const uint8_t *datos_audio;
            size_t bytes_audio;
            int64_t pts_audio;
            uint64_t t_da0 = rdtsc();
            if(p.anillo)anillo_rellenar(p.anillo,ANILLO_BLOQUES);
            if(recargar(&p,0)<0)break;
            int res_a = siguiente_audio(&p, &datos_audio, &bytes_audio, &pts_audio);
            p.telem.ciclos_demux += (rdtsc() - t_da0);
            if (res_a < 0) { p.error = 1; break; }
            if (res_a == 0) break;

            const uint8_t *ptr_aac = datos_audio;
            int rem_aac = (int)bytes_audio;
            int16_t pcm_buf[2048];

            uint64_t t_aac0 = rdtsc();
            int s = aac_decodificar(dec_aac, &ptr_aac, &rem_aac, pcm_buf);
            p.telem.ciclos_aac += (rdtsc() - t_aac0);

            if (s > 0) {
                uint32_t pcm_bytes = (uint32_t)(s * sizeof(int16_t));
                const uint8_t *ptr_pcm = (const uint8_t *)pcm_buf;
                p.telem.pcm_producido_bytes += pcm_bytes;

                uint64_t t_snd0 = rdtsc();
                while (pcm_bytes > 0) {
                    int esc = audio_ac97_encolar_pcm(ptr_pcm, pcm_bytes);
                    if (esc > 0) {
                        ptr_pcm += esc;
                        pcm_bytes -= (uint32_t)esc;
                        p.telem.pcm_aceptado_bytes += (uint32_t)esc;
                    } else {
                        if (atender(&p)) break;
                        break;
                    }
                }
                p.telem.ciclos_hda_usb += (rdtsc() - t_snd0);
            }
        }
        // Iniciar hardware DMA con datos 100% reales
        audio_ac97_iniciar_stream();
    }

    consola_imprimir("Reproductor Multimedia TAEK OS. Presiona ESC o Ctrl+C para salir...");
    if (modo == MODO_PRUEBA_FORENSE) {
        consola_imprimir_linea_color(" [MODO PRUEBA FORENSE FNV-1a]", COLOR_AVISO_DEFAULT);
    } else if (modo == MODO_BENCHMARK) {
        consola_imprimir_linea_color(" [MODO BENCHMARK VELOCIDAD PICO]", COLOR_PROMPT_DEFAULT);
    } else {
        consola_imprimir_linea_color(mp4_tiene_audio(mp4) ? " [H.264 + AAC]" : " [Solo H.264]", COLOR_EXITO_DEFAULT);
    }

    serial_imprimir("[MULTIMEDIA] INICIO "); serial_imprimir_linea(nombre);
    h264_resultado resultado = h264_configurar_avcc(dec, mp4->avcc, mp4->avcc_bytes);

    i915_va_sesion_t *va_ses = NULL;
#ifdef TAEK_I915_HW
    // [EXPERIMENTAL / NO VALIDADO] Decodificacion VA-API por GPU. Excluida del
    // build estable: no hay evidencia de M16/M19 (ver plan maestro, M17-M22).
    if (i915_va_disponible()) {
        uint32_t w = 1920;
        uint32_t h = 1080;
        if (i915_va_crear_sesion(w, h, &va_ses) == 0) {
            p.va_sesion = va_ses;
            p.usando_intel_va = 1;
            serial_imprimir_linea("[MULTIMEDIA] Acelerador Intel VA-API activado con exito.");
            consola_imprimir_linea_color(" [ACELERACION INTEL VA-API GPU ACTIVA]", COLOR_EXITO_DEFAULT);
        } else {
            serial_imprimir_linea("[MULTIMEDIA] Intel VA-API no pudo inicializar sesion; usando software.");
        }
    }
#else
    (void)va_ses;
#endif

    uint64_t t_inicio_reproduccion = tiempo_obtener_milisegundos();
    uint64_t t_inicio_sesion_ciclos=rdtsc();

    while (!resultado) {
        if(muestras_funcionales && mp4->v_indice>=muestras_funcionales)break;
        const uint8_t *datos_v;
        size_t bytes_v;
        int64_t pts_v;
        uint32_t duracion_v;
        if (servir_coordinador(&p)) { resultado = H264_CANCELADO; break; }
        if(presentar_pendiente(&p,0)){resultado=H264_CANCELADO;break;}
        /* Productor: mantener el anillo lleno para que el consumidor no espere. */
        if(p.anillo)anillo_rellenar(p.anillo,ANILLO_BLOQUES);
        if(recargar(&p,0)<0 || recargar(&p,1)<0){resultado=H264_DATOS_INVALIDOS;break;}

        if (p.t_inicio_cuadro_ciclos == 0) p.t_inicio_cuadro_ciclos = rdtsc();

        // 1. Demux de video
        uint64_t t_dv0 = rdtsc();
        int siguiente = cola_muestras_tomar(p.video_comprimido, &datos_v, &bytes_v, &pts_v, &duracion_v);
        if(siguiente>0){p.muestra_actual=mp4->v_indice++;p.pts_actual=pts_v;}
        uint64_t dt_dv = rdtsc() - t_dv0;
        p.telem.ciclos_demux += dt_dv;
        if (dt_dv > p.telem.max_ciclos_demux) p.telem.max_ciclos_demux = dt_dv;
        if (siguiente == 0) break;
        if (siguiente < 0) {
            p.error = 1;
            resultado = siguiente == MP4_LIMITE_EXCEDIDO ? H264_LIMITE_EXCEDIDO : H264_DATOS_INVALIDOS;
            break;
        }

        // 2. Audio AAC: mantener alimentación continua de la cola circular
        reproductor_alimentar_audio(&p);

        // 3. Decodificación de muestra H.264
        uint64_t t_decode=rdtsc();
#ifdef TAEK_I915_HW
        if (p.va_sesion) {
            int ret_va = i915_va_decodificar_cuadro(p.va_sesion, datos_v, bytes_v, pts_v);
            if (ret_va == 0) {
                h264_imagen im;
                if (i915_va_obtener_imagen(p.va_sesion, &im) == 0) {
                    presentar(&p, &im);
                    resultado = H264_OK;
                } else {
                    resultado = h264_decodificar_muestra_avcc(dec, mp4->v_longitud_nal, datos_v, bytes_v, pts_v);
                }
            } else {
                resultado = h264_decodificar_muestra_avcc(dec, mp4->v_longitud_nal, datos_v, bytes_v, pts_v);
            }
        } else
#endif
        {
            resultado = h264_decodificar_muestra_avcc(dec, mp4->v_longitud_nal, datos_v, bytes_v, pts_v);
        }
        registrar(&p,TR_DECODIFICAR,t_decode,rdtsc(),bytes_v);
        guardar_pausa(&p,t_decode,rdtsc());
    }
    if (!resultado) resultado = h264_finalizar(dec);
    while(!resultado && !p.cancelado && p.presentaciones_pendientes)
        if(presentar_pendiente(&p,1))resultado=H264_CANCELADO;

    uint64_t duracion_real_ms = tiempo_obtener_milisegundos() - t_inicio_reproduccion;
    p.telem.ciclos_pared_sesion=rdtsc()-t_inicio_sesion_ciclos;

    if (dec_aac && !p.cancelado && !p.error) {
        audio_ac97_drenar();
        uint64_t t_drenar_ini = tiempo_obtener_milisegundos();
        while (audio_ac97_esta_reproduciendo() && (tiempo_obtener_milisegundos() - t_drenar_ini < 1500)) {
            atender(&p);
            esperar_milisegundos(10);
        }
    }

    if (dec_aac) {
        uint32_t total_vaciados=audio_ac97_obtener_vaciados();
        if(!p.audio_eof)p.vaciados_en_pista=total_vaciados;
        else p.vaciados_eof=total_vaciados-p.vaciados_en_pista;
        if (audio_es_intel_hda()) {
            const struct estado_hda *st = audio_hda_obtener_estado();
            p.telem.avance_dma_bytes = st->bytes_dma_totales;
            p.telem.silencio_insertado_bytes = st->silencio_insertado_bytes;
            p.silencio_en_pista=p.audio_eof?p.silencio_eof_inicio:st->silencio_insertado_bytes;
            p.silencio_despues_eof=st->silencio_insertado_bytes-p.silencio_en_pista;
            p.telem.vaciados_audio = st->vaciados_audio;
        } else {
            p.telem.vaciados_audio = audio_ac97_obtener_vaciados();
        }
        audio_ac97_detener();
        aac_destruir(dec_aac);
    }

    consola_limpiar();
    consola_imprimir(p.cancelado ? "Reproducción cancelada por usuario. " :
                     resultado || p.error ? "Error durante reproducción: " :
                     "Reproducción finalizada con éxito. ");
    if(p.mp4->ultimo_error_lectura){int64_t e=p.mp4->ultimo_error_lectura;consola_imprimir(e<=-20 && e>=-30?volumen_error((int)e):"Lectura MP4 incompleta");}
    else if (resultado && !p.cancelado) consola_imprimir(h264_error(dec));
    consola_imprimir_linea("");

    // Generar e imprimir informe integral de telemetría y benchmark
    imprimir_informe_benchmark(&p, dec, duracion_real_ms);
    serial_imprimir("[PIPELINE] READ_CYCLES=");serial_imprimir_dec(p.ciclos_lectura);
    serial_imprimir(" READ_BYTES=");serial_imprimir_dec(p.bytes_lectura);
    serial_imprimir(" READ_CALLS=");serial_imprimir_dec(p.lecturas);
    serial_imprimir(" LATE_DROPS=");serial_imprimir_dec(p.cuadros_tardios);
    serial_imprimir_linea(" GOP_SCANOUT_VERIFIED=0");
    uint64_t comandos_usb,bytes_usb;usb_msc_obtener_lecturas(&comandos_usb,&bytes_usb);
    serial_imprimir("[PIPELINE] USB_READ_COMMANDS=");serial_imprimir_dec(comandos_usb-p.usb_comandos_inicio);
    serial_imprimir(" USB_READ_BYTES=");serial_imprimir_dec(bytes_usb-p.usb_bytes_inicio);
    serial_imprimir(" VIDEO_MISSES=");serial_imprimir_dec(p.video_comprimido?p.video_comprimido->faltantes:0);
    serial_imprimir(" AUDIO_MISSES=");serial_imprimir_dec(p.audio_comprimido?p.audio_comprimido->faltantes:0);
    serial_imprimir(" SOURCE=");serial_imprimir(p.lector_original?"USB":"RAM");
    serial_imprimir(" BSP_COMPUTE=");serial_imprimir_dec(!diagnostico_coordinador || trabajos_cpu_activas()==1);
    serial_imprimir(" IO_KIB=");serial_imprimir_dec(diagnostico_lectura_kib);
    serial_imprimir(" RING_HITS=");serial_imprimir_dec(p.anillo?p.anillo->aciertos:0);
    serial_imprimir(" RING_STALLS=");serial_imprimir_dec(p.anillo?p.anillo->fallos:0);
    serial_imprimir(" RING_SEEKS=");serial_imprimir_dec(p.anillo?p.anillo->reposiciones:0);
    serial_imprimir(" RING_BLOCKS=");serial_imprimir_dec(p.anillo?p.anillo->bloques_leidos:0);
    serial_imprimir(" RING_BYTES=");serial_imprimir_dec(p.anillo?p.anillo->bytes_leidos:0);
    serial_imprimir(" RING_STARVATIONS=");serial_imprimir_dec(p.anillo?p.anillo->starvations:0);
    serial_imprimir(" RING_PROD_PARADAS=");serial_imprimir_dec(p.anillo?p.anillo->productor_paradas:0);
    serial_imprimir(" RING_MARGEN_MIN=");serial_imprimir_dec(p.anillo&&p.anillo->margen_muestras?p.anillo->margen_min:0);
    serial_imprimir(" RING_MARGEN_MAX=");serial_imprimir_dec(p.anillo?p.anillo->margen_max:0);
    serial_imprimir(" RING_MARGEN_MEDIA=");
    serial_imprimir_dec(p.anillo&&p.anillo->margen_muestras?p.anillo->margen_suma/p.anillo->margen_muestras:0);
    serial_imprimir(" RING_BYTES_UTILES=");serial_imprimir_dec(p.anillo?p.anillo->bytes_utiles:0);
    serial_imprimir(" RING_BYTES_INUTILES=");serial_imprimir_dec(p.anillo?p.anillo->bytes_inutiles:0);
    serial_imprimir_linea("");
    usb_msc_telemetria_volcar_serial();
    {
        memoria_estadisticas_t mem_sesion;
        memoria_obtener_estadisticas(&mem_sesion);
        serial_imprimir("[PIPELINE] HEAP_INICIAL=");serial_imprimir_dec(p.heap_inicial);
        serial_imprimir(" HEAP_PICO_SESION=");serial_imprimir_dec(p.memoria_maxima);
        serial_imprimir(" HEAP_SESION=");serial_imprimir_dec(mem_sesion.heap_bytes_en_uso);
        serial_imprimir(" SILENCIO_EN_PISTA=");serial_imprimir_dec(p.silencio_en_pista);
        serial_imprimir(" SILENCIO_TRAS_EOF=");serial_imprimir_dec(p.silencio_despues_eof);
        serial_imprimir_linea("");
    }
    for(unsigned archivo_traza=0;archivo_traza<2;archivo_traza++) {
        traza_video *tr=archivo_traza?p.pausa:p.traza;
        if(!tr || (archivo_traza && !p.pausa_fin))continue;
        if(archivo_traza) {
            serial_imprimir("[TRACE_PAUSE] START=");serial_imprimir_dec(p.pausa_inicio);
            serial_imprimir(" END=");serial_imprimir_dec(p.pausa_fin);serial_imprimir_linea("");
        }
        serial_imprimir("[TRACE] CYCLES_PER_MS=");serial_imprimir_dec(tiempo_ciclos_por_ms());
        serial_imprimir_linea(" columns=cpu,stage,sample,pts,start,end,detail,pcm_bytes,ready_frames");
        for(unsigned cpu=0;cpu<TRAZA_CPU;cpu++) {
            for(unsigned etapa=1;etapa<14;etapa++) {
                traza_evento *e=&tr->maximos[cpu][etapa];
                if(!e->fin)continue;
                serial_imprimir("[TRACE_MAX] ");serial_imprimir_dec(cpu);
                uint64_t campos[]={e->etapa,e->cuadro,(uint64_t)e->pts,e->inicio,e->fin,e->detalle,e->audio,e->video};
                for(unsigned j=0;j<8;j++){serial_imprimir(",");serial_imprimir_dec(campos[j]);}serial_imprimir_linea("");
            }
            uint64_t total=tr->escritos[cpu],ini=total>TRAZA_EVENTOS?total-TRAZA_EVENTOS:0;
            for(uint64_t i=ini;i<total;i++) {
                traza_evento *e=&tr->eventos[cpu][i%TRAZA_EVENTOS];
                serial_imprimir(archivo_traza?"[TRACE_PAUSE_EVENT] ":"[TRACE_EVENT] ");serial_imprimir_dec(cpu);
                uint64_t campos[]={e->etapa,e->cuadro,(uint64_t)e->pts,e->inicio,e->fin,e->detalle,e->audio,e->video};
                for(unsigned j=0;j<8;j++){serial_imprimir(",");serial_imprimir_dec(campos[j]);}
                serial_imprimir_linea("");
            }
        }
    }

#ifdef TAEK_I915_HW
    if (p.va_sesion) {
        i915_va_destruir_sesion(p.va_sesion);
        p.va_sesion = NULL;
    }
#endif
    h264_destruir(dec);
    if (p.rgb) soltar(&p, p.rgb);
    if (p.telem.tiempos_cuadro_us) soltar(&p, p.telem.tiempos_cuadro_us);
    soltar(&p,datos_audio);soltar(&p,datos_video);
    soltar(&p,p.audio_comprimido);soltar(&p,p.video_comprimido);soltar(&p,p.traza);
    soltar(&p,p.cache);soltar(&p,p.anillo);
    soltar(&p,p.pausa);
    mp4->leer_fuente=p.lector_original;mp4->fuente_contexto=p.lector_contexto;

    memoria_obtener_estadisticas(&despues);
    serial_imprimir("[MULTIMEDIA] HEAP_TRAS_LIBERAR=");serial_imprimir_dec(despues.heap_bytes_en_uso);
    serial_imprimir(" HEAP_INICIAL=");serial_imprimir_dec(antes.heap_bytes_en_uso);
    serial_imprimir_linea("");
    serial_imprimir("[MULTIMEDIA] HEAP_DELTA=");
    serial_imprimir_dec((uint64_t)(despues.heap_bytes_en_uso >= antes.heap_bytes_en_uso ?
                        despues.heap_bytes_en_uso - antes.heap_bytes_en_uso : 0));
    serial_imprimir_linea("");

    if(p.mp4->ultimo_error_lectura)return (int)p.mp4->ultimo_error_lectura;
    return p.cancelado ? 1 : resultado || p.error ? -1 : 0;
}

int reproductor_reproducir_memoria(const void *datos, size_t tamano, const char *nombre, enum modo_reproduccion modo) {
    if (!datos || !tamano) return -1;
    mp4_contenedor mp4;
    mp4_resultado r = mp4_abrir(&mp4, datos, tamano);
    if (r != MP4_OK) {
        consola_imprimir_linea("MP4 inválido o contenedor no soportado.");
        serial_imprimir_linea("[MULTIMEDIA] ERROR MP4");
        return -2;
    }
    return reproductor_ejecutar(&mp4, nombre, modo, -1, -1);
}

int reproductor_reproducir_fuente(mp4_lectura_posicional leer, void *contexto,
                                   uint64_t tamano, const char *nombre,
                                   enum modo_reproduccion modo,
                                   int descriptor_vfs_video, int descriptor_vfs_audio) {
    if (!leer || !tamano) return -1;
    const size_t tam_metadatos = 2u * 1024u * 1024u;
    const size_t tam_muestra = 3u * 1024u * 1024u;
    uint8_t *metadatos = asignar_memoria(tam_metadatos);
    uint8_t *muestra_video = asignar_memoria(tam_muestra);
    uint8_t *muestra_audio = asignar_memoria(tam_muestra);
    if (!metadatos || !muestra_video || !muestra_audio) {
        if (metadatos) liberar_memoria(metadatos);
        if (muestra_video) liberar_memoria(muestra_video);
        if (muestra_audio) liberar_memoria(muestra_audio);
        return -2;
    }
    mp4_contenedor mp4;
    mp4_resultado r = mp4_abrir_fuente(&mp4, tamano, leer, contexto,
                                       metadatos, tam_metadatos,
                                       muestra_video, tam_muestra,
                                       muestra_audio, tam_muestra);
    int resultado = r == MP4_OK ? reproductor_ejecutar(&mp4, nombre, modo,
                                                       descriptor_vfs_video, descriptor_vfs_audio) : -3;
    if (r != MP4_OK) {
        int64_t e=mp4.ultimo_error_lectura;
        consola_imprimir_linea(e?(e<=-20 && e>=-30?volumen_error((int)e):"Lectura MP4 incompleta"):
            r == MP4_LIMITE_EXCEDIDO ?
            "El índice MP4 excede el límite de memoria configurado." :
            "MP4 inválido o contenedor no soportado.");
        serial_imprimir_linea("[MULTIMEDIA] ERROR MP4 STREAM");
    }
    if(mp4.ultimo_error_lectura)resultado=(int)mp4.ultimo_error_lectura;
    liberar_memoria(muestra_audio);
    liberar_memoria(muestra_video);
    liberar_memoria(metadatos);
    return resultado;
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

    reproductor_reproducir_memoria(archivo->address, (size_t)archivo->size, nombre, modo);
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
    if(iguales(arg,"io 4") || iguales(arg,"io 16") || iguales(arg,"io 32") || iguales(arg,"io 64") ||
       iguales(arg,"io 128") || iguales(arg,"io 256")) {
        unsigned n=0;for(const char *c=arg+3;*c;c++)n=n*10+(unsigned)(*c-'0');
        if(particiones_configurar_lectura(n))diagnostico_lectura_kib=n;
        serial_imprimir("[PIPELINE_CONFIG] IO_KIB=");serial_imprimir_dec(diagnostico_lectura_kib);
        if(n==4)serial_imprimir(" REFERENCIA=1");
        serial_imprimir_linea("");
    } else if(iguales(arg,"intel h0")) {
        intel_inventario_imprimir_h0();
    } else if(iguales(arg,"intel probar") || iguales(arg,"intel test") || iguales(arg,"intel r0") || iguales(arg,"intel h1")) {
        intel_diagnostico_ring0_ejecutar();
    } else if(iguales(arg,"intel estado") || iguales(arg,"intel drv")) {
        i915_imprimir_estado();
    } else if(iguales(arg,"intel vcs") || iguales(arg,"intel trabajo") || iguales(arg,"intel batch")) {
        uint32_t res = 0;
        i915_vcs_primer_trabajo(0x5441454BU, &res);
        consola_imprimir_linea_color("==> [ AVISO ] Envío de trabajos VCS deshabilitado temporalmente.", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("Pendiente de resolver: ownership formal GGTT, direccionamiento DMA y recuperación.");
    } else if(iguales(arg,"cpu 1") || iguales(arg,"cpu 2") || iguales(arg,"cpu 4")) {
        unsigned n=(unsigned)(arg[4]-'0');
        if(trabajos_configurar_distribucion(n,!diagnostico_coordinador)) {
            diagnostico_cpu=n;serial_imprimir("[PIPELINE_CONFIG] CPUS=");serial_imprimir_dec(n);serial_imprimir_linea("");
        }
        else consola_imprimir_linea("No hay tantos procesadores activos o existe un lote en curso.");
    } else if(iguales(arg,"bsp coordinador") || iguales(arg,"bsp calcula")) {
        diagnostico_cpu=trabajos_cpu_activas();
        int coordinador=iguales(arg,"bsp coordinador") && diagnostico_cpu>1;
        if(trabajos_configurar_distribucion(diagnostico_cpu,!coordinador))diagnostico_coordinador=coordinador;
        serial_imprimir("[PIPELINE_CONFIG] BSP_COMPUTE=");serial_imprimir_dec(!diagnostico_coordinador);serial_imprimir_linea("");
    } else if (iguales(arg, "360p") || iguales(arg, "1")) {
        reproducir("h264:360p", MODO_INTERACTIVO);
    } else if (iguales(arg, "1080p") || iguales(arg, "2")) {
        reproducir("h264:1080p", MODO_INTERACTIVO);
    } else if (iguales(arg, "prueba 360p") || iguales(arg, "test 360p") || iguales(arg, "prueba")) {
        reproducir("h264:360p", MODO_PRUEBA_FORENSE);
    } else if (iguales(arg, "prueba 1080p") || iguales(arg, "test 1080p")) {
        reproducir("h264:1080p", MODO_PRUEBA_FORENSE);
    } else if (iguales(arg, "bench 360p") || iguales(arg, "benchmark 360p") || iguales(arg, "bench")) {
        reproducir("h264:360p", MODO_BENCHMARK);
    } else if (iguales(arg, "bench 1080p") || iguales(arg, "benchmark 1080p")) {
        reproducir("h264:1080p", MODO_BENCHMARK);
    } else if (iguales(arg, "aac 360p") || iguales(arg, "audio 360p")) {
        audio_aac_probar("h264:360p");
    } else if (iguales(arg, "aac 1080p") || iguales(arg, "audio 1080p")) {
        audio_aac_probar("h264:1080p");
    } else if (iguales(arg, "aac") || iguales(arg, "audio")) {
        audio_aac_probar("h264:360p");
    } else {
        consola_imprimir_linea_color("--- REPRODUCTOR MULTIMEDIA H.264 & AAC (TELEMETRÍA) ---", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("  h264 cpu 1|2|4; h264 bsp coordinador|calcula: comparación de distribución.");
        consola_imprimir_linea("  reproducir [ram|usb] [bench|prueba] archivo.mp4: diagnóstico de la misma fuente.");
        consola_imprimir_color("  h264 360p          ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Reproducción interactiva A/V completa (H.264 + AAC + PTS).");
        consola_imprimir_color("  h264 bench 360p    ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Benchmark a velocidad pico de hardware (sin esperas de PTS).");
        consola_imprimir_color("  h264 bench 1080p   ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Mide capacidad 1080p sin hash, AAC ni espera PTS.");
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
        if (iguales(token, "h264=bench1080")) reproducir("h264:1080p", MODO_BENCHMARK);
        if (iguales(token, "h264=smoke1080")) {
            muestras_funcionales=32;reproducir("h264:1080p",MODO_BENCHMARK);muestras_funcionales=0;
        }
        if (iguales(token, "h264=play360")) reproducir("h264:360p", MODO_INTERACTIVO);
        if (iguales(token, "h264=play1080") || iguales(token, "h264=1080p")) reproducir("h264:1080p", MODO_INTERACTIVO);
        if (iguales(token, "h264=prueba1080")) reproducir("h264:1080p", MODO_PRUEBA_FORENSE);
    }
}
