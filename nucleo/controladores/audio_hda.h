#ifndef CONTROLADORES_AUDIO_HDA_H
#define CONTROLADORES_AUDIO_HDA_H

#include <stdint.h>
#include <stddef.h>

// Dirección virtual MMIO para el controlador Intel HDA (aislado de GPU, APIC, IOMMU, ACPI y xHCI)
#define HDA_MMIO_VIRTUAL_BASE 0xFFFFFE0005000000ULL

// Número máximo de entradas en el Buffer Descriptor List (BDL)
#define HDA_MAX_BDL_ENTRADAS 32

// Estructura de entrada en la lista de descriptores de búfer (BDL)
struct __attribute__((packed)) hda_bdl_entrada {
    uint64_t dir_fisica;
    uint32_t longitud_bytes;
    uint32_t ioc; // Bit 0 = Interrupt on completion
};

// Estados explícitos de la máquina de estados de reproducción (A2)
enum hda_estado_reproductor {
    HDA_ESTADO_DETENIDO      = 0,
    HDA_ESTADO_PREPARANDO    = 1,
    HDA_ESTADO_REPRODUCIENDO = 2,
    HDA_ESTADO_DRENANDO      = 3,
    HDA_ESTADO_ERROR         = 4
};

// Fuentes de posición validadas del DMA
enum hda_fuente_pos {
    HDA_POS_NINGUNA = 0,
    HDA_POS_DPIB    = 1, // Reporte de hardware en RAM (DMA Position-in-Buffer)
    HDA_POS_LPIB    = 2, // Registro MMIO Link Position in Buffer
    HDA_POS_BCIS    = 3  // Fallback por finalización de bloque
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
    enum hda_estado_reproductor estado_reproductor;
    enum hda_fuente_pos         fuente_pos_activa;
    uint32_t reproduccion_id;
    uint64_t bytes_reproducidos;
    uint64_t bytes_dma_totales; // Contador monotónico de posición DMA observada
    uint64_t bytes_en_cola;     // Bytes de la fuente ya copiados al ring
    uint32_t eventos_bcis;     // Finalizaciones BDL observadas y reconocidas
    uint32_t errores_stream;   // FIFO/Descriptor errors observados
    uint32_t vaciados_audio;   // Underruns de búfer observados
    uint64_t silencio_insertado_bytes; // Bytes de ceros insertados por falta de muestras
    uint32_t pcm_aceptado_bytes;       // Total de bytes PCM aceptados en cola
    uint32_t pcm_rechazado_bytes;      // Bytes que no cupieron por backpressure
    uint32_t saltos_posicion_rechazados; // Saltos no coherentes filtrados
    uint32_t max_intervalo_sin_atencion_ms; // Máxima latencia entre llamadas a actualizar
    uint64_t t_ultimo_servicio_ms;
    uint32_t bytes_totales;
};

// --- API PÚBLICA DEL CONTROLADOR INTEL HDA (ANILLO 0 EN ESPAÑOL) ---

// Inicializa el controlador Intel HDA, reinicia el silicio y configura anillos CORB/RIRB y streams DMA
int  audio_hda_iniciar(void);

// Inicia la reproducción de un búfer PCM (formato 44.1 kHz, 16 bits, estéreo)
int  audio_hda_reproducir_pcm(const void *datos_pcm, uint32_t tamano_bytes);

// Inicia la reproducción en bucle continuo de un búfer PCM
int  audio_hda_reproducir_pcm_bucle(const void *datos_pcm, uint32_t tamano_bytes);

// Encola datos PCM (44.1 kHz, 16 bits estéreo) en la cola persistente para streaming A/V continuo.
// Retorna la cantidad exacta de bytes aceptados (>= 0), o código negativo en caso de error.
int  audio_hda_encolar_pcm(const void *datos_pcm, uint32_t tamano_bytes);

// Inicia explícitamente el stream DMA de audio continuo tras la precarga inicial
int  audio_hda_iniciar_stream(void);

// Solicita el drenado limpio de las muestras remanentes en cola antes de detener
int  audio_hda_drenar(void);

// Devuelve el número de bytes ocupados en la cola circular de streaming
uint32_t audio_hda_cola_ocupada(void);

// Devuelve el número de bytes libres en la cola circular de streaming
uint32_t audio_hda_cola_disponible(void);

// Obtiene el tiempo de audio transcurrido según el DMA de hardware en milisegundos
uint64_t audio_hda_obtener_tiempo_ms(void);

// Reinicia el reloj de reproducción de audio y los contadores asociados
void audio_hda_reiniciar_reloj(void);

// Obtiene el número total de vaciados (underruns) ocurridos durante el streaming
uint32_t audio_hda_obtener_vaciados(void);

// Actualiza el estado de reproducción y alimenta el búfer DMA si es necesario
void audio_hda_actualizar(void);

// Indica si el motor DMA del stream de audio se encuentra reproduciendo actualmente
int  audio_hda_esta_reproduciendo(void);

// Detiene inmediatamente la reproducción de audio y el motor DMA de forma segura
void audio_hda_detener(void);

// Vuelca el buffer circular de trazas diagnósticas en RAM a la salida serial
void audio_hda_volcar_trazas(void);

// Obtiene el estado actual del controlador Intel HDA
const struct estado_hda *audio_hda_obtener_estado(void);

// Indica si el controlador Intel HDA fue detectado e inicializado exitosamente
int  audio_hda_esta_operativo(void);

// Control de volumen en hardware para códecs Intel HDA (en dB)
int  audio_hda_obtener_volumen_db(void);
int  audio_hda_fijar_volumen_db(int db);
int  audio_hda_ajustar_volumen_db(int delta_db);
int  audio_hda_esta_silenciado(void);
void audio_hda_fijar_silencio(int silenciar);

#endif // CONTROLADORES_AUDIO_HDA_H
