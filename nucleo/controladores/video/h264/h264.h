#ifndef TAEK_VIDEO_H264_H
#define TAEK_VIDEO_H264_H

#include <stdint.h>
#include <stddef.h>

/* Decodificador AVC escrito para TAEK OS. Sin dependencias de libc.
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
h264_resultado h264_finalizar(h264_decodificador *dec);
const char *h264_error(const h264_decodificador *dec);
uint64_t h264_huella(uint64_t anterior, const h264_imagen *imagen);
void h264_convertir_rgb(const h264_imagen *imagen, uint32_t *rgb, unsigned ancho, unsigned alto);

/* Contenedor ISO BMFF no fragmentado, pista avc1/avc3. Vista del MP4:
 * los datos del archivo deben permanecer vivos durante toda la reproducción. */
typedef struct {
    const uint8_t *archivo;
    size_t bytes;
    const uint8_t *avcc, *stsz, *stsc, *stco, *stts, *ctts;
    size_t avcc_bytes, stsz_bytes, stsc_bytes, stco_bytes, stts_bytes, ctts_bytes;
    uint32_t muestras, escala_tiempo;
    unsigned longitud_nal, offsets_64;
    uint32_t indice, fragmento, muestra_fragmento, regla_fragmento;
    uint32_t regla_tiempo, restante_tiempo, regla_composicion, restante_composicion;
    uint64_t offset_muestra, tiempo_decodificacion;
} h264_mp4;
h264_resultado h264_mp4_abrir(h264_mp4 *mp4, const void *datos, size_t bytes);
h264_resultado h264_mp4_configurar(const h264_mp4 *mp4, h264_decodificador *dec);
/* Devuelve 1 con una muestra, 0 al terminar, o un h264_resultado negativo. */
int h264_mp4_siguiente(h264_mp4 *mp4, const uint8_t **datos, size_t *bytes,
                      int64_t *presentacion, uint32_t *duracion);
h264_resultado h264_mp4_muestra(const h264_mp4 *mp4, h264_decodificador *dec,
                               const uint8_t *datos, size_t bytes, int64_t tiempo);
#endif
