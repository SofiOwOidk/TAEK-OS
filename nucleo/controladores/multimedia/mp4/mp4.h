#ifndef TAEK_MULTIMEDIA_MP4_H
#define TAEK_MULTIMEDIA_MP4_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    MP4_OK = 0,
    MP4_DATOS_INVALIDOS = -1,
    MP4_NO_SOPORTADO = -2,
    MP4_SIN_MEMORIA = -3,
    MP4_LIMITE_EXCEDIDO = -4
} mp4_resultado;

/* Contenedor ISO Base Media File Format (MP4 / ISO 14496-12).
 * Demuxer freestanding para Ring 0 sin dependencias de codecs ni de libc. */
typedef struct {
    const uint8_t *archivo;
    size_t bytes;

    /* Pista de Video (AVC / H.264) */
    int tiene_video;
    const uint8_t *avcc;
    size_t avcc_bytes;
    const uint8_t *v_stsz, *v_stsc, *v_stco, *v_stts, *v_ctts;
    size_t v_stsz_bytes, v_stsc_bytes, v_stco_bytes, v_stts_bytes, v_ctts_bytes;
    uint32_t v_muestras, v_escala_tiempo;
    unsigned v_longitud_nal, v_offsets_64;
    uint32_t v_indice, v_fragmento, v_muestra_fragmento, v_regla_fragmento;
    uint32_t v_regla_tiempo, v_restante_tiempo, v_regla_composicion, v_restante_composicion;
    uint64_t v_offset_muestra, v_tiempo_decodificacion;

    /* Pista de Audio (AAC / MP4A) */
    int tiene_audio;
    const uint8_t *a_stsz, *a_stsc, *a_stco, *a_stts;
    size_t a_stsz_bytes, a_stsc_bytes, a_stco_bytes, a_stts_bytes;
    uint32_t a_muestras, a_escala_tiempo, a_canales, a_frecuencia;
    unsigned a_offsets_64;
    uint32_t a_indice, a_fragmento, a_muestra_fragmento, a_regla_fragmento;
    uint32_t a_regla_tiempo, a_restante_tiempo;
    uint64_t a_offset_muestra, a_tiempo;
} mp4_contenedor;

mp4_resultado mp4_abrir(mp4_contenedor *m, const void *datos, size_t bytes);

int mp4_tiene_video(const mp4_contenedor *m);
int mp4_tiene_audio(const mp4_contenedor *m);

/* Extrae la siguiente muestra de video (paquete NAL / AVCC).
 * Devuelve 1 con muestra, 0 al finalizar, o negativo en error. */
int mp4_siguiente_video(mp4_contenedor *m, const uint8_t **datos, size_t *bytes,
                        int64_t *pts, uint32_t *duracion);

/* Extrae el siguiente paquete de audio (AAC).
 * Devuelve 1 con muestra, 0 al finalizar, o negativo en error. */
int mp4_siguiente_audio(mp4_contenedor *m, const uint8_t **datos, size_t *bytes,
                        int64_t *pts);

void mp4_rebobinar_video(mp4_contenedor *m);
void mp4_rebobinar_audio(mp4_contenedor *m);

#endif /* TAEK_MULTIMEDIA_MP4_H */
