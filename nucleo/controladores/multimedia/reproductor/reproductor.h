#ifndef TAEK_H264_REPRODUCTOR_H
#define TAEK_H264_REPRODUCTOR_H

#include <stdint.h>
#include <stddef.h>

// Modos de ejecución soportados
enum modo_reproduccion {
    MODO_INTERACTIVO    = 0, // Video + Audio + Presentación 100% + Sincronización PTS
    MODO_PRUEBA_FORENSE = 1, // Hash FNV-1a en 100% de cuadros, presentación 1/100, sin espera PTS
    MODO_BENCHMARK      = 2  // Rendimiento pico de hardware: presentación 100%, sin espera PTS
};

void video_h264_comando(const char *argumento);
void video_h264_arranque(const char *cmdline);
void audio_aac_comando(const char *argumento);

// Reproduce un archivo MP4 en memoria (buffer binario desde Limine o USB VFS)
int reproductor_reproducir_memoria(const void *datos, size_t tamano, const char *nombre, enum modo_reproduccion modo);

#endif
