#include "animacion_cangrejo.h"
#include "pantalla.h"
#include "audio_ac97.h"
#include "../base/tiempo.h"
#include "../arquitectura/x86_64/serial.h"

extern const uint8_t _binary_cangrejo_video_bin_start[];
extern const uint8_t _binary_cangrejo_video_bin_end[];

extern const uint8_t _binary_cangrejo_audio_bin_start[];
extern const uint8_t _binary_cangrejo_audio_bin_end[];

void animacion_don_cangrejo_explotar(int bucles) {
    serial_imprimir_linea("");
    serial_imprimir_linea("==============================================================");
    serial_imprimir_linea("  [ HIPERVISOR RING -1 ] ¡INTERCEPCIÓN DE TRIPLE FAULT ACTIVA! ");
    serial_imprimir_linea("  [ HIPERVISOR RING -1 ] Tomando control del Framebuffer...    ");
    serial_imprimir_linea("  [ HIPERVISOR RING -1 ] Ejecutando protocolo Don Cangrejo...  ");
    serial_imprimir_linea("==============================================================");

    const uint32_t *frames = (const uint32_t *)_binary_cangrejo_video_bin_start;
    uint32_t tamano_audio = (uint32_t)(_binary_cangrejo_audio_bin_end - _binary_cangrejo_audio_bin_start);
    uint32_t total_pixeles_frame = CANGREJO_ANCHO * CANGREJO_ALTO;

    if (bucles < 1) bucles = 1;

    for (int b = 0; b < bucles; b++) {
        serial_imprimir("  [ DON CANGREJO ] Bucle de explosión ");
        serial_imprimir_dec(b + 1);
        serial_imprimir(" de ");
        serial_imprimir_dec(bucles);
        serial_imprimir_linea("...");

        // Iniciar audio de la explosion
        if (tamano_audio > 0) {
            audio_ac97_reproducir_pcm(_binary_cangrejo_audio_bin_start, tamano_audio);
        }

        // Reproducir los 49 fotogramas a 30 FPS (33 ms por fotograma)
        for (int f = 0; f < CANGREJO_FRAMES; f++) {
            const uint32_t *frame_actual = &frames[f * total_pixeles_frame];
            pantalla_dibujar_imagen_centrada(CANGREJO_ANCHO, CANGREJO_ALTO, frame_actual);
            esperar_milisegundos(33);
        }
    }

    serial_imprimir_linea("==============================================================");
    serial_imprimir_linea("  [ HIPERVISOR RING -1 ] Secuencia de explosión concluida.     ");
    serial_imprimir_linea("  [ HIPERVISOR RING -1 ] Apagando equipo ahora. ¡Adiós!        ");
    serial_imprimir_linea("==============================================================");
}
