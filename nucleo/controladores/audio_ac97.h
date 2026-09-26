#ifndef CONTROLADORES_AUDIO_AC97_H
#define CONTROLADORES_AUDIO_AC97_H

#include <stdint.h>
#include <stddef.h>

int  audio_ac97_iniciar(uint64_t base_fisica_kernel, uint64_t base_virtual_kernel);
int  audio_ac97_reproducir_pcm(const void *datos_pcm, uint32_t tamano_bytes);
int  audio_ac97_reproducir_pcm_bucle(const void *datos_pcm, uint32_t tamano_bytes);
void audio_ac97_actualizar(void);
int  audio_ac97_esta_reproduciendo(void);
void audio_ac97_detener(void);
int  audio_es_intel_hda(void);
int  audio_esta_iniciado(void);

#endif // CONTROLADORES_AUDIO_AC97_H
