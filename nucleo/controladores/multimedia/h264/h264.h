#ifndef TAEK_VIDEO_H264_H
#define TAEK_VIDEO_H264_H

#include <stdint.h>
#include <stddef.h>

/* Decodificador AVC / H.264 para TAEK OS.
 * Freestanding para Ring 0, sin dependencias de libc ni aceleración FPU/SSE.
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

h264_decodificador *h264_crear(const h264_servicios *servicios);
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

typedef struct {
    uint64_t ciclos_sintaxis_reconstruccion;
    uint64_t max_ciclos_sintaxis;
    uint64_t ciclos_cabac_puro;
    uint64_t ciclos_inter;
    uint64_t ciclos_intra;
    uint64_t ciclos_desbloqueo;
    uint64_t max_ciclos_desbloqueo;
    uint64_t cuadros_decodificados;
} h264_telemetria;

void h264_obtener_telemetria(const h264_decodificador *dec, h264_telemetria *t);

#endif /* TAEK_VIDEO_H264_H */
