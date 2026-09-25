#ifndef CONTROLADORES_AUDIO_HDA_H
#define CONTROLADORES_AUDIO_HDA_H

#include <stdint.h>
#include <stddef.h>

// Dirección virtual MMIO para el controlador Intel HDA
#define HDA_MMIO_VIRTUAL_BASE 0xFFFFFE0003000000ULL

// Número máximo de entradas en el Buffer Descriptor List (BDL)
#define HDA_MAX_BDL_ENTRADAS 32

// Estructura de entrada en la lista de descriptores de búfer (BDL)
struct __attribute__((packed)) hda_bdl_entrada {
    uint64_t dir_fisica;
    uint32_t longitud_bytes;
    uint32_t ioc; // Bit 0 = Interrupt on completion
};

// Estado público del subsistema Intel HDA
struct estado_hda {
    int      controlador_detectado;
    int      inicializado;
    uint8_t  bus;
    uint8_t  ranura;
    uint8_t  funcion;
    uint16_t id_proveedor;
    uint16_t id_dispositivo;
    uint64_t dir_fisica_mmio;
    uint64_t dir_virtual_mmio;
    uint32_t tamano_mmio;
    uint8_t  num_iss;
    uint8_t  num_oss;
    uint8_t  num_bss;
    uint16_t codecs_detectados;
    int      reproduciendo;
    uint32_t bytes_reproducidos;
    uint32_t bytes_totales;
};

// --- API PÚBLICA DEL CONTROLADOR INTEL HDA (ANILLO 0 EN ESPAÑOL) ---

// Inicializa el controlador Intel HDA, reinicia el silicio y configura anillos CORB/RIRB y streams DMA
int  audio_hda_iniciar(void);

// Inicia la reproducción de un búfer PCM (formato 44.1 kHz, 16 bits, estéreo)
int  audio_hda_reproducir_pcm(const void *datos_pcm, uint32_t tamano_bytes);

// Inicia la reproducción en bucle continuo de un búfer PCM
int  audio_hda_reproducir_pcm_bucle(const void *datos_pcm, uint32_t tamano_bytes);

// Actualiza el estado de reproducción y alimenta el búfer DMA si es necesario
void audio_hda_actualizar(void);

// Indica si el motor DMA del stream de audio se encuentra reproduciendo actualmente
int  audio_hda_esta_reproduciendo(void);

// Detiene inmediatamente la reproducción de audio y el motor DMA
void audio_hda_detener(void);

// Obtiene el estado actual del controlador Intel HDA
const struct estado_hda *audio_hda_obtener_estado(void);

#endif // CONTROLADORES_AUDIO_HDA_H
