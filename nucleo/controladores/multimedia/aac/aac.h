#ifndef TAEK_AUDIO_AAC_H
#define TAEK_AUDIO_AAC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aac_decodificador aac_decodificador;

typedef struct {
    void *usuario;
    void *(*asignar)(void *usuario, size_t bytes);
    void (*liberar)(void *usuario, void *memoria);
} aac_servicios;

aac_decodificador *aac_crear(const aac_servicios *servicios);
void aac_destruir(aac_decodificador *dec);
int aac_configurar(aac_decodificador *dec, int canales, int frecuencia_muestreo);

/* Decodifica un cuadro AAC a PCM int16_t estéreo entrelazado.
 * Retorna la cantidad de muestras int16_t escritas (típicamente 2048 = 1024 x 2),
 * o un valor menor o igual a 0 si ocurrió un error. */
int aac_decodificar(aac_decodificador *dec, const uint8_t **datos, int *bytes_restantes, int16_t *pcm_salida);

#ifdef __cplusplus
}
#endif

#endif
