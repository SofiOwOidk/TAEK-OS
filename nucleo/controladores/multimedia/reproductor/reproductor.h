#ifndef TAEK_H264_REPRODUCTOR_H
#define TAEK_H264_REPRODUCTOR_H

#include <stdint.h>
#include <stddef.h>
#include "../mp4/mp4.h"

// Modos de ejecución soportados
enum modo_reproduccion {
    MODO_INTERACTIVO    = 0, // Video + Audio + Presentación 100% + Sincronización PTS
    MODO_PRUEBA_FORENSE = 1, // Hash FNV-1a en 100% de cuadros, presentación 1/100, sin espera PTS
    MODO_BENCHMARK      = 2  // Rendimiento pico de hardware: presentación 100%, sin espera PTS
};

/* H155: modulo de Limine por su cmdline exacta (p. ej. "rtw88:fw"). La unica
 * limine_module_request del nucleo vive en reproductor.c. NULL si no esta. */
const void *taek_modulo_arranque(const char *cmdline, uint64_t *tamano);

void video_h264_comando(const char *argumento);
void video_h264_arranque(const char *cmdline);
void audio_aac_comando(const char *argumento);

// Reproduce un archivo MP4 en memoria (buffer binario desde Limine o USB VFS)
int reproductor_reproducir_memoria(const void *datos, size_t tamano, const char *nombre, enum modo_reproduccion modo);

// Reproduce un MP4 desde una fuente de lectura aleatoria con buffers acotados.
int reproductor_reproducir_fuente(mp4_lectura_posicional leer, void *contexto,
                                  uint64_t tamano, const char *nombre,
                                  enum modo_reproduccion modo,
                                  int descriptor_vfs_video, int descriptor_vfs_audio);

/* H187: reproduccion en flujo para SYS_FLUJO. Las muestras llegan en memoria del nucleo. 0 bien, 1 cancelado por el
 * usuario, negativo error. Una sesion a la vez. */
int reproductor_flujo_abrir(const uint8_t *avcc, size_t n, unsigned canales, unsigned frecuencia, unsigned escala,
                            int hay_desde, int64_t desde);
unsigned reproductor_flujo_teclas(uint8_t *destino, unsigned capacidad);   /* H188 */
int reproductor_flujo_osd(const uint32_t *pixeles, unsigned x, unsigned y, unsigned ancho, unsigned alto, unsigned ms);
int reproductor_flujo_video(const uint8_t *datos, size_t n, int64_t pts);
int reproductor_flujo_audio(const uint8_t *datos, size_t n, int64_t pts);
int reproductor_flujo_cerrar(int drenar);
int reproductor_flujo_abierto(void);
void reproductor_flujo_estado(int64_t *reloj_ms, uint32_t *cola_ms, uint32_t *cuadros, uint32_t *tardios, uint32_t *motor);
/* H201: video en marco. Se pide antes de abrir (la sesion presenta por Iris); ancho 0 lo quita. 0 bien, -1 fuera de pantalla. */
int reproductor_flujo_marco(int x, int y, int ancho, int alto);
int reproductor_flujo_iris(void);   /* 1 si la sesion abierta presenta por Iris */

// Reproduce una pista MP3 desde un descriptor VFS por ventanas progresivas.
int reproductor_mp3_reproducir_vfs(int descriptor_vfs, const char *nombre);

#endif
