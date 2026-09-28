#include "../reproductor/reproductor.h"
#include "minimp3.h"
#include "../../vfs.h"
#include "../../audio_ac97.h"
#include "../../teclado.h"
#include "../../consola.h"
#include "../../xhci.h"
#include "../../../base/memoria.h"
#include "../../../base/tiempo.h"

#define MP3_ENTRADA_BYTES (16u * 1024u)
#define MP3_MUESTRAS_SALIDA 8192u
#define MP3_TASA_SALIDA 44100u

struct reproductor_mp3_estado {
    mp3dec_t decodificador;
    uint8_t entrada[MP3_ENTRADA_BYTES];
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t salida[MP3_MUESTRAS_SALIDA * 2];
};

static int mp3_atender(int fd) {
    xhci_sondeo();
    audio_ac97_actualizar();
    int cancelar = 0;
    while (teclado_hay_datos()) {
        char c = teclado_leer_caracter();
        if (c == 27 || c == 3 || c == 'q' || c == 'Q') cancelar = 1;
    }
    char c;
    while ((c = consola_leer_caracter()) != 0) {
        if (c == 27 || c == 3 || c == 'q' || c == 'Q') cancelar = 1;
    }
    if (cancelar) vfs_cancelar(fd);
    return cancelar;
}

static int16_t mp3_interpolar(int16_t a, int16_t b, uint32_t fraccion) {
    int32_t delta = (int32_t)b - a;
    return (int16_t)(a + (int32_t)(((int64_t)delta * fraccion) / MP3_TASA_SALIDA));
}

static int mp3_encolar(int fd, const int16_t *pcm, size_t bytes) {
    const uint8_t *p = (const uint8_t *)pcm;
    while (bytes) {
        if (mp3_atender(fd)) return -1;
        int aceptados = audio_ac97_encolar_pcm(p, (uint32_t)bytes);
        if (aceptados > 0) {
            p += aceptados;
            bytes -= (size_t)aceptados;
        } else {
            esperar_microsegundos(200);
        }
    }
    return 0;
}

int reproductor_mp3_reproducir_vfs(int fd, const char *nombre) {
    if (fd < 0 || !nombre || !audio_esta_iniciado()) return -1;
    uint64_t tamano = vfs_tamano_fd(fd);
    if (tamano < 4) return -2;

    uint64_t inicio = 0, fin = tamano;
    uint8_t cabecera[10];
    int64_t n = vfs_leer_en(fd, 0, cabecera, sizeof(cabecera));
    if (n == (int64_t)sizeof(cabecera) && cabecera[0] == 'I' &&
        cabecera[1] == 'D' && cabecera[2] == '3') {
        uint32_t tag = 0;
        for (int i = 6; i < 10; i++) {
            if (cabecera[i] & 0x80) return -3;
            tag = (tag << 7) | cabecera[i];
        }
        uint64_t extra = (cabecera[5] & 0x10) ? 10 : 0;
        if ((uint64_t)tag + 10 + extra > tamano) return -3;
        inicio = 10 + (uint64_t)tag + extra;
    }

    if (tamano >= 128) {
        uint8_t id3v1[3];
        if (vfs_leer_en(fd, tamano - 128, id3v1, sizeof(id3v1)) == 3 &&
            id3v1[0] == 'T' && id3v1[1] == 'A' && id3v1[2] == 'G') fin -= 128;
    }
    if (inicio >= fin) return -3;

    struct reproductor_mp3_estado *st = asignar_memoria(sizeof(*st));
    if (!st) return -4;
    mp3dec_init(&st->decodificador);
    uint64_t lectura_pos = inicio, fase = 0;
    size_t entrada_bytes = 0;
    uint32_t frames_sin_sync = 0;
    int iniciado = 0, cancelado = 0, resultado = 0;

    consola_imprimir("MP3 progresivo: ");
    consola_imprimir(nombre);
    consola_imprimir_linea(". ESC o Ctrl+C para detener.");

    while (lectura_pos < fin || entrada_bytes) {
        if (mp3_atender(fd)) { cancelado = 1; break; }

        if (entrada_bytes < MP3_ENTRADA_BYTES && lectura_pos < fin) {
            size_t espacio = MP3_ENTRADA_BYTES - entrada_bytes;
            uint64_t restante = fin - lectura_pos;
            if ((uint64_t)espacio > restante) espacio = (size_t)restante;
            int64_t leidos = vfs_leer_en(fd, lectura_pos, st->entrada + entrada_bytes, espacio);
            if (leidos < 0) { resultado = -5; break; }
            if (!leidos) lectura_pos = fin;
            else { entrada_bytes += (size_t)leidos; lectura_pos += (uint64_t)leidos; }
        }
        if (entrada_bytes < 4) break;

        mp3dec_frame_info_t info = {0};
        int muestras = mp3dec_decode_frame(&st->decodificador, st->entrada,
                                            (int)entrada_bytes, st->pcm, &info);
        if (info.frame_bytes > 0 && (size_t)info.frame_bytes <= entrada_bytes) {
            entrada_bytes -= (size_t)info.frame_bytes;
            memmove(st->entrada, st->entrada + info.frame_bytes, entrada_bytes);
            frames_sin_sync = 0;
        } else if (muestras > 0) {
            resultado = -9;
            break;
        } else {
            if (lectura_pos >= fin) break;
            if (++frames_sin_sync > 65536) { resultado = -6; break; }
            memmove(st->entrada, st->entrada + 1, --entrada_bytes);
        }
        if (muestras <= 0 || info.frame_bytes <= 0) continue;
        if ((info.channels != 1 && info.channels != 2) || info.hz < 8000 || info.hz > 48000 ||
            muestras > MINIMP3_MAX_SAMPLES_PER_FRAME / info.channels) {
            resultado = -7;
            break;
        }

        uint64_t limite = (uint64_t)muestras * MP3_TASA_SALIDA;
        size_t cuadros_salida = 0;
        while (fase < limite && cuadros_salida < MP3_MUESTRAS_SALIDA) {
            uint32_t indice = (uint32_t)(fase / MP3_TASA_SALIDA);
            uint32_t fraccion = (uint32_t)(fase % MP3_TASA_SALIDA);
            uint32_t siguiente = indice + 1 < (uint32_t)muestras ? indice + 1 : indice;
            int16_t izq0 = st->pcm[indice * info.channels];
            int16_t der0 = info.channels == 2 ? st->pcm[indice * 2 + 1] : izq0;
            int16_t izq1 = st->pcm[siguiente * info.channels];
            int16_t der1 = info.channels == 2 ? st->pcm[siguiente * 2 + 1] : izq1;
            st->salida[cuadros_salida * 2] = mp3_interpolar(izq0, izq1, fraccion);
            st->salida[cuadros_salida * 2 + 1] = mp3_interpolar(der0, der1, fraccion);
            cuadros_salida++;
            fase += (uint32_t)info.hz;
        }
        fase = fase >= limite ? fase - limite : 0;

        if (mp3_encolar(fd, st->salida, cuadros_salida * 2 * sizeof(int16_t)) != 0) {
            cancelado = 1;
            break;
        }
        if (!iniciado && audio_ac97_cola_ocupada() >= (128 * 1024)) {
            if (audio_ac97_iniciar_stream() != 0) { resultado = -8; break; }
            iniciado = 1;
        }
    }

    if (!cancelado && !resultado && !iniciado && audio_ac97_cola_ocupada()) {
        iniciado = audio_ac97_iniciar_stream() == 0;
        if (!iniciado) resultado = -8;
    }
    if (iniciado && !cancelado && !resultado) {
        audio_ac97_drenar();
        uint64_t t0 = tiempo_obtener_milisegundos();
        while (audio_ac97_esta_reproduciendo() && tiempo_obtener_milisegundos() - t0 < 2000) {
            if (mp3_atender(fd)) { cancelado = 1; break; }
            esperar_milisegundos(10);
        }
    }
    audio_ac97_detener();
    liberar_memoria(st);
    return cancelado ? 1 : resultado;
}
