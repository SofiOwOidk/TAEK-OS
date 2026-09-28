#ifndef TAEK_VIDEO_H264_H
#define TAEK_VIDEO_H264_H

#include <stdint.h>
#include <stddef.h>

/* Decodificador AVC / H.264 para TAEK OS.
 * Freestanding para Ring 0, sin dependencias de libc. Compensación SSE2
 * optativa en una unidad separada; la ruta escalar permanece seleccionable.
 * Los callbacks pertenecen al anfitrión (heap del núcleo o arnés de pruebas).
 * Un fotograma permanece válido durante el callback presentar. */
typedef enum {
    H264_OK = 0,
    H264_DATOS_INVALIDOS = -1,
    H264_NO_SOPORTADO = -2,
    H264_SIN_MEMORIA = -3,
    H264_LIMITE_EXCEDIDO = -4,
    H264_CANCELADO = -5
} h264_resultado;

enum h264_etapa_inter {
    H264_INTER_ENTERO, H264_INTER_HORIZONTAL, H264_INTER_VERTICAL,
    H264_INTER_DIAGONAL, H264_INTER_CROMA, H264_INTER_SIMPLE,
    H264_INTER_DOBLE, H264_INTER_PONDERADA, H264_INTER_BORDES,
    H264_INTER_ETAPAS
};

typedef struct {
    const uint8_t *y, *u, *v;
    unsigned ancho, alto;
    unsigned paso_y, paso_c;
    int32_t orden;
    int64_t marca_tiempo;
    int rango_completo, matriz_color;
} h264_imagen;

typedef struct {
    void *usuario;
    void *(*asignar)(void *usuario, size_t bytes);
    void (*liberar)(void *usuario, void *memoria);
    int (*presentar)(void *usuario, const h264_imagen *imagen);
} h264_servicios;

typedef struct h264_decodificador h264_decodificador;
typedef void (*h264_region_fn)(void *,unsigned region,unsigned ejecutor);
/* Backend síncrono: retorna sólo después de que todos los consumidores hayan
 * abandonado los buffers. ejecutor está en [0,3]. Sólo el coordinador atiende
 * el callback servicio; éste no puede entrar otra vez al decodificador. */
typedef int (*h264_lote_fn)(void *,h264_region_fn,void *,unsigned);
int h264_configurar_trabajadores(h264_decodificador *,h264_lote_fn,void *,unsigned,
                                 int (*servicio)(void *));
void h264_cancelar_trabajadores(h264_decodificador *);

h264_decodificador *h264_crear(const h264_servicios *servicios);
/* Opt-in de XMM: el anfitrión garantiza estado FPU/SSE habilitado y preservado
 * por todas las IRQ/cambios de contexto del CPU que ejecuta este decodificador.
 * CPUID se verifica además aquí. Crear mantiene bloques escalares por defecto.
 * No llamar mientras una decodificación usa el mismo contexto. */
int h264_configurar_sse2(h264_decodificador *dec,int estado_xmm_disponible);
void h264_destruir(h264_decodificador *dec);

/* NAL incluye su byte de cabecera, sin start code ni longitud AVCC. */
h264_resultado h264_nal(h264_decodificador *dec, const uint8_t *datos,
                        size_t bytes, int64_t marca_tiempo);

/* Configura el decodificador H.264 a partir de un bloque avcC (SPS y PPS de ISO 14496-15) */
h264_resultado h264_configurar_avcc(h264_decodificador *dec, const uint8_t *avcc, size_t avcc_bytes);

/* Decodifica una muestra empaquetada en formato AVCC (longitud prefijada de NALs) */
h264_resultado h264_decodificar_muestra_avcc(h264_decodificador *dec, unsigned longitud_nal,
                                            const uint8_t *datos, size_t bytes, int64_t tiempo);

h264_resultado h264_finalizar(h264_decodificador *dec);
const char *h264_error(const h264_decodificador *dec);
uint64_t h264_huella(uint64_t anterior, const h264_imagen *imagen);
void h264_convertir_rgb(const h264_imagen *imagen, uint32_t *rgb, unsigned ancho, unsigned alto);
void h264_convertir_rgb_region(const h264_imagen *,uint32_t *,unsigned,unsigned,unsigned,unsigned,int);

typedef struct {
    uint64_t ciclos_sintaxis_reconstruccion;
    uint64_t max_ciclos_sintaxis;
    uint64_t ciclos_cabac_puro;
    uint64_t ciclos_inter;
    uint64_t ciclos_intra;
    /* Subetapas de compensación por bloques: exclusivas entre sí, incluidas
     * en ciclos_inter. Nunca sumarlas otra vez al total del reproductor.
     * ciclos_inter también contiene sintaxis MV/CABAC y residuo actualmente.
     * Activas sólo con H264_PERFIL_INTER=1; el reloj mide pared en el BSP. */
    uint64_t ciclos_inter_etapa[H264_INTER_ETAPAS];
    uint64_t bloques_inter_etapa[H264_INTER_ETAPAS];
    uint64_t pixeles_inter_etapa[H264_INTER_ETAPAS];
    uint64_t ciclos_desbloqueo;
    uint64_t max_ciclos_desbloqueo;
    uint64_t cuadros_decodificados;
    unsigned perfil, nivel, ancho_codificado, alto_codificado, ancho_visible, alto_visible;
    uint32_t num_tick, escala_tick;
    unsigned inter_sse2;
    unsigned etapas_sse2;
    uint64_t ciclos_transformadas, ciclos_suma_residuo, ciclos_hadamard;
    uint64_t ciclos_reconstruccion_cpu, ciclos_reconstruccion_pared;
    uint64_t ciclos_dependencias_cpu;
    unsigned trabajadores, presupuesto_reconstruccion_bytes;
} h264_telemetria;

void h264_obtener_telemetria(const h264_decodificador *dec, h264_telemetria *t);

#endif /* TAEK_VIDEO_H264_H */
