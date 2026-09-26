#include "audio_hda.h"
#include "../arquitectura/x86_64/pci.h"
#include "../arquitectura/x86_64/serial.h"
#include "../base/paginacion.h"
#include "../base/dma.h"
#include "../base/tiempo.h"
#include "../base/memoria.h"
#include "../base/huevo.h"

// --- REGISTROS MMIO DE INTEL HIGH DEFINITION AUDIO ---
#define REG_GCAP           0x00 // 16 bits: Global Capabilities
#define REG_VMIN           0x02 // 8 bits: Minor Version
#define REG_VMAJ           0x03 // 8 bits: Major Version
#define REG_GCTL           0x08 // 32 bits: Global Control
#define REG_WAKEEN         0x0C // 16 bits: Wake Enable
#define REG_STATESTS       0x0E // 16 bits: State Change Status
#define REG_GSTS           0x10 // 16 bits: Global Status
#define REG_INTCTL         0x20 // 32 bits: Interrupt Control
#define REG_INTSTS         0x24 // 32 bits: Interrupt Status
#define REG_CORBLBASE      0x40 // 32 bits: CORB Lower Base
#define REG_CORBUBASE      0x44 // 32 bits: CORB Upper Base
#define REG_CORBWP         0x48 // 16 bits: CORB Write Pointer
#define REG_CORBRP         0x4A // 16 bits: CORB Read Pointer
#define REG_CORBCTL        0x4C // 8 bits: CORB Control
#define REG_CORBSIZE       0x4E // 8 bits: CORB Size
#define REG_RIRBLBASE      0x50 // 32 bits: RIRB Lower Base
#define REG_RIRBUBASE      0x54 // 32 bits: RIRB Upper Base
#define REG_RIRBWP         0x58 // 16 bits: RIRB Write Pointer
#define REG_RINTCNT        0x5A // 16 bits: Response Interrupt Count
#define REG_RIRBCTL        0x5C // 8 bits: RIRB Control
#define REG_RIRBSTS        0x5D // 8 bits: RIRB Status
#define REG_RIRBSIZE       0x5E // 8 bits: RIRB Size

// Registros de Descriptor de Stream (relativos a stream_base = 0x80 + stream_idx * 0x20)
#define SD_REG_CTL         0x00 // 24 bits: Stream Control
#define SD_REG_STS         0x03 // 8 bits: Stream Status
#define SD_REG_LPIB        0x04 // 32 bits: Link Position in Buffer
#define SD_REG_CBL         0x08 // 32 bits: Cyclic Buffer Length
#define SD_REG_LVI         0x0C // 16 bits: Last Valid Index
#define SD_REG_FIFOS       0x10 // 16 bits: FIFO Size
#define SD_REG_FMT         0x12 // 16 bits: Stream Format
#define SD_REG_BDLPL       0x18 // 32 bits: BDL Lower Base
#define SD_REG_BDLPU       0x1C // 32 bits: BDL Upper Base

// Banderas de GCTL
#define GCTL_CRST          (1U << 0) // Controller Reset (1 = Operativo, 0 = En reset)

// Banderas de CORBCTL y RIRBCTL
#define CORBCTL_RUN        (1U << 1)
#define RIRBCTL_RUN        ((1U << 1) | (1U << 0)) // bit 1 = DMA Enable, bit 0 = Response Interrupt Enable

// Banderas de Stream Control (SD_CTL)
#define SD_CTL_SRST        (1U << 1) // Stream Reset
#define SD_CTL_RUN         (1U << 2) // Stream Run
#define SD_CTL_IOCE        (1U << 3) // Interrupt On Completion Enable
#define SD_CTL_STREAM_TAG  (1U << 20)// Stream ID = 1 en bits 23:20

// Formato de Stream para 44.1 kHz, 16 bits estéreo
// Bit 14 = 1 (Base 44.1 kHz), Bits 6:4 = 001 (16 bits/muestra), Bits 3:0 = 0001 (2 canales estéreo)
#define HDA_FORMATO_44K_16B_STEREO 0x4011

// Tamaño del búfer DMA ping-pong (2 bloques de 64 KiB = 128 KiB)
#define HDA_TAMANO_BLOQUE_DMA (64 * 1024)
#define HDA_NUM_BLOQUES       2
#define HDA_TAMANO_TOTAL_DMA  (HDA_TAMANO_BLOQUE_DMA * HDA_NUM_BLOQUES)

// Estructura de respuesta de RIRB (8 bytes)
struct __attribute__((packed)) rirb_entrada {
    uint32_t respuesta;
    uint32_t respuesta_ex;
};

// --- ESTADO GLOBAL DEL CONTROLADOR INTEL HDA ---
static struct estado_hda g_hda_estado = {0};

static uint64_t g_mmio_base   = 0;
static uint64_t g_stream_base = 0;

// Anillos de comandos y respuestas
static uint32_t            *g_corb = NULL;
static uint64_t             g_corb_fisica = 0;
static uint16_t             g_corb_wp = 0;

static struct rirb_entrada *g_rirb = NULL;
static uint64_t             g_rirb_fisica = 0;
static uint16_t             g_rirb_rp = 0;

// BDL y búfer DMA cíclico ping-pong
static struct hda_bdl_entrada *g_bdl = NULL;
static uint64_t                g_bdl_fisica = 0;

static uint8_t  *g_dma_pcm_buffer = NULL;
static uint64_t  g_dma_pcm_fisica = 0;

// Estado del reproductor
static const uint8_t *g_audio_fuente = NULL;
static uint32_t       g_audio_tamano = 0;
static uint32_t       g_audio_cursor = 0;
static int            g_audio_en_bucle = 0;
static uint8_t        g_bloque_activo = 0;

// --- FUNCIONES DE LECTURA/ESCRITURA MMIO ---
static inline uint8_t __attribute__((unused)) mmio_leer8(uint64_t dir) {
    return *(volatile uint8_t *)dir;
}

static inline void mmio_escribir8(uint64_t dir, uint8_t val) {
    *(volatile uint8_t *)dir = val;
}

static inline uint16_t mmio_leer16(uint64_t dir) {
    return *(volatile uint16_t *)dir;
}

static inline void mmio_escribir16(uint64_t dir, uint16_t val) {
    *(volatile uint16_t *)dir = val;
}

static inline uint32_t mmio_leer32(uint64_t dir) {
    return *(volatile uint32_t *)dir;
}

static inline void mmio_escribir32(uint64_t dir, uint32_t val) {
    *(volatile uint32_t *)dir = val;
}

// --- COMUNICACIÓN CON EL CÓDEC VÍA CORB/RIRB ---
static uint32_t hda_enviar_verbo(uint8_t codec, uint8_t nodo, uint32_t payload) {
    if (!g_corb || !g_rirb) return 0;

    // Asegurar que banderas residuales de RIRB estén limpias antes de despachar
    mmio_escribir8(g_mmio_base + REG_RIRBSTS, 0x05);

    uint32_t verbo = ((uint32_t)(codec & 0x0F) << 28) |
                     ((uint32_t)(nodo  & 0xFF) << 20) |
                     (payload & 0x000FFFFF);

    g_corb_wp = (g_corb_wp + 1) & 0xFF;
    g_corb[g_corb_wp] = verbo;
    dma_sincronizar_cpu_a_dispositivo(&g_corb[g_corb_wp], sizeof(g_corb[g_corb_wp]));
    mmio_escribir16(g_mmio_base + REG_CORBWP, g_corb_wp);

    // Esperar respuesta en RIRB: 50 µs × 400 iteraciones = 20 ms máximo por verbo
    int timeout = 400;
    while (timeout > 0) {
        uint16_t rirb_wp = mmio_leer16(g_mmio_base + REG_RIRBWP) & 0xFF;
        if (rirb_wp != g_rirb_rp) {
            g_rirb_rp = (g_rirb_rp + 1) & 0xFF;
            dma_sincronizar_dispositivo_a_cpu(&g_rirb[g_rirb_rp], sizeof(g_rirb[g_rirb_rp]));
            uint32_t resp = g_rirb[g_rirb_rp].respuesta;
            // CRÍTICO: Limpiar RIRBSTS (bit 0 = RINTFL, bit 2 = ERIS) para reiniciar rirb_count a 0
            // y permitir que el hardware/QEMU procese el siguiente verbo en CORB sin bloquearse.
            mmio_escribir8(g_mmio_base + REG_RIRBSTS, 0x05);
            return resp;
        }
        esperar_microsegundos(50);
        timeout--;
    }

    return 0; // Timeout: nodo no existe o códec no responde
}

static uint8_t hda_encontrar_grupo_audio(uint8_t codec) {
    // El nodo raíz enumera los Function Groups. No todos los códecs sitúan
    // el Audio Function Group en el nodo 1, especialmente los códecs HDMI.
    uint32_t sub_count = hda_enviar_verbo(codec, 0, 0xF0004);
    uint8_t inicio = (sub_count >> 16) & 0xFF;
    uint8_t cantidad = sub_count & 0xFF;

    if (cantidad == 0 || cantidad > 32) {
        // Si el nodo 0 no reporta subnodos válidos, probar nodo 1 por defecto (estándar en la mayoría de códecs)
        uint32_t tipo1 = hda_enviar_verbo(codec, 1, 0xF0005);
        if ((tipo1 & 0xFF) == 0x01) return 1;
        return 0;
    }

    for (uint8_t i = 0; i < cantidad; i++) {
        uint8_t nodo = inicio + i;
        uint32_t tipo = hda_enviar_verbo(codec, nodo, 0xF0005);
        if ((tipo & 0xFF) == 0x01) { // Audio Function Group
            return nodo;
        }
    }

    return 0;
}

// Configura los nodos DAC y Pin de salida del códec consultando el tipo real de cada nodo
static void hda_configurar_nodos_codec(uint8_t codec) {
    serial_imprimir("[HDA] Configurando nodos de audio del códec ");
    serial_imprimir_dec(codec);
    serial_imprimir_linea("...");

    // Tiempo límite global: 800 ms máximo
    uint64_t t_limite = tiempo_obtener_milisegundos() + 800;

    // 1. Localizar y despertar el Audio Function Group real del códec.
    uint8_t afg = hda_encontrar_grupo_audio(codec);
    if (afg == 0) {
        serial_imprimir_linea("[HDA AVISO] El códec no expuso un Audio Function Group utilizable.");
        return;
    }

    serial_imprimir("[HDA] AFG detectado en nodo ");
    serial_imprimir_dec(afg);
    serial_imprimir_linea(" — Activando estado de energía D0.");
    hda_enviar_verbo(codec, afg, 0x70500); // SET_POWER_STATE D0
    esperar_milisegundos(2);

    // 2. Consultar rango real de nodos hijo del AFG
    //    GET_PARAMETER(0x04) = Sub-Node Count sobre el AFG
    //    Respuesta: bits [23:16] = nodo inicio, bits [7:0] = conteo
    uint32_t sub_count = hda_enviar_verbo(codec, afg, 0xF0004);
    uint8_t nodo_inicio = (sub_count >> 16) & 0xFF;
    uint8_t num_nodos   = (uint8_t)(sub_count & 0xFF);

    if (nodo_inicio < 2 || nodo_inicio > 100 || num_nodos == 0 || num_nodos > 64) {
        nodo_inicio = 2;
        num_nodos   = 10;
        serial_imprimir_linea("[HDA AVISO] No se obtuvo rango de nodos válido. Usando rango de seguridad 2-11.");
    }
    uint8_t nodo_fin = nodo_inicio + num_nodos - 1;

    serial_imprimir("[HDA] Rango de nodos AFG: ");
    serial_imprimir_dec(nodo_inicio);
    serial_imprimir(" a ");
    serial_imprimir_dec(nodo_fin);
    serial_imprimir_linea("");

    // 3. Recorrer nodos y configurar DACs y Pines de salida
    for (uint8_t n = nodo_inicio; n <= nodo_fin; n++) {
        if (tiempo_obtener_milisegundos() >= t_limite) {
            serial_imprimir_linea("[HDA AVISO] Timeout en configuración de nodos.");
            break;
        }

        // GET_PARAMETER(0x09) = Audio Widget Capabilities
        // Bits [23:20] = Widget Type:
        //   0x0 = Audio Output (DAC)
        //   0x1 = Audio Input (ADC)
        //   0x2 = Audio Mixer
        //   0x3 = Audio Selector
        //   0x4 = Pin Complex
        uint32_t widget_cap = hda_enviar_verbo(codec, n, 0xF0009);
        uint8_t widget_type = (widget_cap >> 20) & 0x0F;

        if (widget_type == 0x0) {
            // --- Audio Output (DAC) ---
            serial_imprimir("  [HDA] Nodo ");
            serial_imprimir_dec(n);
            serial_imprimir_linea(": DAC — Configurando Stream 1, Formato 44.1k/16b estéreo");

            hda_enviar_verbo(codec, n, 0x70500);                                // Power State D0
            hda_enviar_verbo(codec, n, 0x20000 | HDA_FORMATO_44K_16B_STEREO); // Converter Format
            hda_enviar_verbo(codec, n, 0x70610);                                // Stream=1, Channel=0
            // SET_AMP_GAIN_MUTE: Output, Left+Right, Index=0, Mute=0, Gain=0x77 (-3dB aprox)
            hda_enviar_verbo(codec, n, 0x3B077); // Amp-Out Both, Unmute, Gain=0x77
            hda_enviar_verbo(codec, n, 0x3B777); // Amp-Out alternativo
            hda_enviar_verbo(codec, n, 0x39077);

        } else if (widget_type == 0x4) {
            // --- Pin Complex ---
            uint32_t cfg_default = hda_enviar_verbo(codec, n, 0xF1C00);
            uint8_t port_conn = (cfg_default >> 30) & 0x3; // 0=Jack, 1=NoPhysical, 2=Fixed, 3=Both
            uint8_t default_dev = (cfg_default >> 20) & 0xF; // 0=LineOut, 1=Speaker, 2=HP...
            uint8_t location = (cfg_default >> 24) & 0x3F;

            // Configurar si es salida física o fija
            if (port_conn != 1) {
                serial_imprimir("  [HDA] Nodo ");
                serial_imprimir_dec(n);
                serial_imprimir(": Pin Complex — Dev=");
                serial_imprimir_dec(default_dev);
                serial_imprimir(" Loc=");
                serial_imprimir_dec(location);
                serial_imprimir_linea(" — Habilitando salida física y amplificador");

                hda_enviar_verbo(codec, n, 0x70500); // D0
                hda_enviar_verbo(codec, n, 0x70100); // Conexión índice 0

                // PIN_WIDGET_CONTROL: Out Enable (bit 6 = 0x40) + HP Drive (bit 7 = 0x80)
                uint8_t pin_ctrl = 0x40; // Out Enable
                if (default_dev <= 2) pin_ctrl |= 0x80; // HP Enable para LineOut, Speaker y Auriculares
                hda_enviar_verbo(codec, n, 0x70700 | pin_ctrl);

                // EAPD Enable (External Amplifier Power Down: bit 1 = Enable), vital en portátiles y MoDT
                hda_enviar_verbo(codec, n, 0x70C02);

                // Amplificador de salida: Desmutear ambos canales, ganancia máxima
                hda_enviar_verbo(codec, n, 0x3B077);
                hda_enviar_verbo(codec, n, 0x39077);
            }

        } else if (widget_type == 0x2) {
            // --- Audio Mixer: desmutear entradas ---
            uint32_t in_cap = hda_enviar_verbo(codec, n, 0xF0012);
            uint8_t num_inputs = (in_cap >> 16) & 0x7F;
            if (num_inputs > 8) num_inputs = 8;
            for (uint8_t i = 0; i < num_inputs; i++) {
                hda_enviar_verbo(codec, n, 0x3A000 | (i << 8) | 0x77);
            }
        } else if (widget_type == 0x3) {
            // Audio Selector: elegir la primera ruta
            hda_enviar_verbo(codec, n, 0x70500);
            hda_enviar_verbo(codec, n, 0x70100);
        }
        // Nodos tipo Input, Selector, Power, VolumeKnob, Beep: ignorar
    }

    serial_imprimir_linea("[HDA] Configuración de códec completada.");
}

// --- GESTIÓN DE BÚFERES Y REPRODUCCIÓN DMA ---
static void hda_llenar_bloque_dma(uint8_t bloque) {
    if (!g_dma_pcm_buffer || !g_audio_fuente) return;

    uint8_t *destino = g_dma_pcm_buffer + (bloque * HDA_TAMANO_BLOQUE_DMA);

    if (g_audio_cursor < g_audio_tamano) {
        uint32_t restante = g_audio_tamano - g_audio_cursor;
        uint32_t copiar = (restante > HDA_TAMANO_BLOQUE_DMA) ? HDA_TAMANO_BLOQUE_DMA : restante;
        copiar &= ~3; // Múltiplo de muestra de 4 bytes

        for (uint32_t i = 0; i < copiar; i++) {
            destino[i] = g_audio_fuente[g_audio_cursor + i];
        }

        // Si el bloque no se llenó por completo, rellenar con silencio
        for (uint32_t i = copiar; i < HDA_TAMANO_BLOQUE_DMA; i++) {
            destino[i] = 0;
        }

        g_audio_cursor += copiar;
        g_hda_estado.bytes_reproducidos = g_audio_cursor;

        if (g_audio_cursor >= g_audio_tamano) {
            if (g_audio_en_bucle) {
                g_audio_cursor = 0;
            }
        }
    } else {
        // Silencio si no hay más datos
        for (uint32_t i = 0; i < HDA_TAMANO_BLOQUE_DMA; i++) {
            destino[i] = 0;
        }
    }

    dma_sincronizar_cpu_a_dispositivo(destino, HDA_TAMANO_BLOQUE_DMA);
}

void audio_hda_actualizar(void) {
    if (!g_hda_estado.inicializado || !g_hda_estado.reproduciendo) return;

    // Leer la posición actual del puntero DMA de hardware en el búfer cíclico
    uint32_t lpib = mmio_leer32(g_stream_base + SD_REG_LPIB);
    uint8_t bloque_actual = (lpib >= HDA_TAMANO_BLOQUE_DMA) ? 1 : 0;

    // Si el hardware cambió de bloque, rellenar el bloque que acaba de quedar libre
    if (bloque_actual != g_bloque_activo) {
        hda_llenar_bloque_dma(g_bloque_activo);
        g_bloque_activo = bloque_actual;

        // Si se terminó el audio y no está en bucle, detener
        if (g_audio_cursor >= g_audio_tamano && !g_audio_en_bucle) {
            audio_hda_detener();
        }
    }
}

int audio_hda_esta_reproduciendo(void) {
    audio_hda_actualizar();
    return g_hda_estado.reproduciendo;
}

void audio_hda_detener(void) {
    if (!g_hda_estado.inicializado) return;

    // Detener el motor DMA del Stream 1
    uint32_t ctl = mmio_leer32(g_stream_base + SD_REG_CTL);
    mmio_escribir32(g_stream_base + SD_REG_CTL, ctl & ~SD_CTL_RUN);

    g_hda_estado.reproduciendo = 0;
    g_audio_fuente = NULL;
    g_audio_tamano = 0;
    g_audio_cursor = 0;
}

static int hda_iniciar_reproduccion(const void *datos_pcm, uint32_t tamano_bytes, int bucle) {
    if (!g_hda_estado.inicializado || !datos_pcm || tamano_bytes == 0) {
        return -1;
    }

    // Detener si estaba reproduciendo
    audio_hda_detener();

    g_audio_fuente   = (const uint8_t *)datos_pcm;
    g_audio_tamano   = tamano_bytes;
    g_audio_cursor   = 0;
    g_audio_en_bucle = bucle;
    g_hda_estado.bytes_totales = tamano_bytes;
    g_hda_estado.bytes_reproducidos = 0;
    g_bloque_activo  = 0;

    // Llenar ambos bloques iniciales con los primeros datos
    hda_llenar_bloque_dma(0);
    hda_llenar_bloque_dma(1);

    // --- Reset del Stream (SD_CTL bit 0 = SRST) ---
    // 1. Poner SRST=1 para entrar en reset
    uint32_t ctl = mmio_leer32(g_stream_base + SD_REG_CTL);
    ctl &= ~SD_CTL_RUN;       // Asegurar que RUN=0 antes de reset
    ctl |= SD_CTL_SRST;
    mmio_escribir32(g_stream_base + SD_REG_CTL, ctl);

    // 2. Esperar que el hardware confirme reset (SRST permanece en 1)
    int timeout = 100;
    while (!(mmio_leer32(g_stream_base + SD_REG_CTL) & SD_CTL_SRST) && timeout > 0) {
        esperar_milisegundos(1);
        timeout--;
    }

    // 3. Limpiar SRST=0 para salir del reset
    ctl = mmio_leer32(g_stream_base + SD_REG_CTL);
    ctl &= ~SD_CTL_SRST;
    mmio_escribir32(g_stream_base + SD_REG_CTL, ctl);

    // 4. Esperar que el hardware confirme que salió del reset (SRST=0)
    timeout = 100;
    while ((mmio_leer32(g_stream_base + SD_REG_CTL) & SD_CTL_SRST) && timeout > 0) {
        esperar_milisegundos(1);
        timeout--;
    }

    // --- Programar el Stream Descriptor ---
    // El Stream Tag se ubica en bits [23:20] del SD_CTL: Tag=1 → 0x00100000
    // Limpiar cualquier status pendiente en el stream
    mmio_escribir8(g_stream_base + SD_REG_STS, 0x1C); // W1C: Clear BCIS/FIFOE/DESE

    // Formato de audio (debe programarse antes del BDL y CBL)
    mmio_escribir16(g_stream_base + SD_REG_FMT, HDA_FORMATO_44K_16B_STEREO);

    // BDL: dirección física en dos registros de 32 bits
    mmio_escribir32(g_stream_base + SD_REG_BDLPL, (uint32_t)(g_bdl_fisica & 0xFFFFFFFF));
    mmio_escribir32(g_stream_base + SD_REG_BDLPU, (uint32_t)(g_bdl_fisica >> 32));

    // Longitud cíclica total del búfer DMA
    mmio_escribir32(g_stream_base + SD_REG_CBL, HDA_TAMANO_TOTAL_DMA);

    // Último Índice Válido del BDL (0-indexed, tenemos 2 entradas → LVI=1)
    mmio_escribir16(g_stream_base + SD_REG_LVI, HDA_NUM_BLOQUES - 1);
    dma_sincronizar_cpu_a_dispositivo(g_bdl, HDA_NUM_BLOQUES * sizeof(*g_bdl));

    // --- Iniciar reproducción DMA ---
    // Stream Tag en bits [23:20]: Tag=1 → bits 23:20 = 0001 → valor 0x00100000
    // RUN en bit 1 (SD_CTL_RUN = (1U<<2) es incorrecto — corrección: ver spec)
    // HDA SD_CTL: bit 1 = SRST, bit 2 = RUN (Run/Stop)
    uint32_t nuevo_ctl = (1U << 20) | SD_CTL_RUN | SD_CTL_IOCE;
    mmio_escribir32(g_stream_base + SD_REG_CTL, nuevo_ctl);

    g_hda_estado.reproduciendo = 1;
    return 0;
}

int audio_hda_reproducir_pcm(const void *datos_pcm, uint32_t tamano_bytes) {
    return hda_iniciar_reproduccion(datos_pcm, tamano_bytes, 0);
}

int audio_hda_reproducir_pcm_bucle(const void *datos_pcm, uint32_t tamano_bytes) {
    return hda_iniciar_reproduccion(datos_pcm, tamano_bytes, 1);
}

const struct estado_hda *audio_hda_obtener_estado(void) {
    return &g_hda_estado;
}

int audio_hda_esta_operativo(void) {
    return g_hda_estado.inicializado;
}

// --- INICIALIZACIÓN DEL CONTROLADOR INTEL HDA ---
int audio_hda_iniciar(void) {
    if (g_hda_estado.inicializado) return 0;

    serial_imprimir_linea("[HDA] Escaneando bus PCI en busca de controladores de audio (Intel HDA / Multimedia)...");

    int total_devs = pci_obtener_conteo();
    #define MAX_CANDIDATOS_HDA 8
    const struct dispositivo_pci *candidatos[MAX_CANDIDATOS_HDA];
    int num_candidatos = 0;

    // Prioridad 1: Controladores de Audio Intel (Vendor 0x8086)
    // Coincide con Subclase 0x03 (HDA) o Subclase 0x01 (Audio Controller / cAVS en portátiles)
    for (int i = 0; i < total_devs && num_candidatos < MAX_CANDIDATOS_HDA; i++) {
        const struct dispositivo_pci *d = pci_obtener_dispositivo(i);
        if (d && d->id_proveedor == 0x8086 && d->clase == 0x04 &&
            (d->subclase == 0x03 || d->subclase == 0x01)) {
            // Evitar confundir con legacy AC97 ICH si estuviera presente
            if (d->id_dispositivo != 0x2415) {
                candidatos[num_candidatos++] = d;
            }
        }
    }

    // Prioridad 2: Otros controladores HDA (NVIDIA 0x10DE, AMD 0x1002, Realtek 0x10EC, QEMU/RedHat)
    for (int i = 0; i < total_devs && num_candidatos < MAX_CANDIDATOS_HDA; i++) {
        const struct dispositivo_pci *d = pci_obtener_dispositivo(i);
        if (d && d->id_proveedor != 0x8086 && d->clase == 0x04 &&
            (d->subclase == 0x03 || d->subclase == 0x01)) {
            candidatos[num_candidatos++] = d;
        }
    }

    if (num_candidatos == 0) {
        serial_imprimir_linea("[HDA AVISO] No se detectó ningún controlador Intel HDA / Audio en el bus PCI.");
        return -1;
    }

    serial_imprimir("[HDA] Se encontraron ");
    serial_imprimir_dec(num_candidatos);
    serial_imprimir_linea(" candidato(s) de audio PCI. Evaluando silicio...");

    for (int cand = 0; cand < num_candidatos; cand++) {
        const struct dispositivo_pci *pci_hda = candidatos[cand];
        if (!pci_hda->barras[0].valida || pci_hda->barras[0].es_io || pci_hda->barras[0].dir_base == 0) {
            continue;
        }

        serial_imprimir("  [HDA Intento ");
        serial_imprimir_dec(cand + 1);
        serial_imprimir("] Dispositivo ");
        serial_imprimir_hex(pci_hda->bus);
        serial_imprimir(":");
        serial_imprimir_hex(pci_hda->ranura);
        serial_imprimir(".");
        serial_imprimir_hex(pci_hda->funcion);
        serial_imprimir(" [Vendor: 0x");
        serial_imprimir_hex(pci_hda->id_proveedor);
        serial_imprimir(" Dev: 0x");
        serial_imprimir_hex(pci_hda->id_dispositivo);
        serial_imprimir(" | BAR0: 0x");
        serial_imprimir_hex(pci_hda->barras[0].dir_base);
        serial_imprimir_linea("]");

        // 1. Activar Bus Master y Memory Space en PCI Command
        pci_activar_bus_master(pci_hda);

        // 2. Mapear BAR0 MMIO en el espacio virtual soberano del kernel (0xFFFFFE0005000000ULL)
        uint64_t mmio_virt = HDA_MMIO_VIRTUAL_BASE;
        uint32_t tamano_mmio = (uint32_t)pci_hda->barras[0].tamano;
        uint32_t paginas = (tamano_mmio + TAMANO_PAGINA - 1) / TAMANO_PAGINA;
        if (paginas == 0) paginas = 4; // Mínimo 16 KiB

        for (uint32_t p = 0; p < paginas; p++) {
            paginacion_mapear(mmio_virt + (p * TAMANO_PAGINA),
                              pci_hda->barras[0].dir_base + (p * TAMANO_PAGINA),
                              PAGINA_ATRIBUTOS_MMIO);
        }
        g_mmio_base = mmio_virt;

        // 3. Reset del controlador HDA (GCTL.CRST)
        // Paso A: Entrar en reset (CRST = 0)
        uint32_t gctl = mmio_leer32(g_mmio_base + REG_GCTL);
        mmio_escribir32(g_mmio_base + REG_GCTL, gctl & ~GCTL_CRST);

        int timeout = 100;
        while ((mmio_leer32(g_mmio_base + REG_GCTL) & GCTL_CRST) && timeout > 0) {
            esperar_milisegundos(1);
            timeout--;
        }
        esperar_milisegundos(10);

        // Paso B: Salir de reset (CRST = 1)
        mmio_escribir32(g_mmio_base + REG_GCTL, GCTL_CRST);
        timeout = 100;
        while (!(mmio_leer32(g_mmio_base + REG_GCTL) & GCTL_CRST) && timeout > 0) {
            esperar_milisegundos(1);
            timeout--;
        }

        if (timeout == 0) {
            serial_imprimir_linea("  [HDA AVISO] Timeout saliendo del reset del silicio. Descartando controlador.");
            continue;
        }

        // Esperar que los códecs físicos señalen presencia en STATESTS (mínimo 25 ms según spec)
        esperar_milisegundos(50);

        uint16_t statests = mmio_leer16(g_mmio_base + REG_STATESTS);
        for (int intento = 0; statests == 0 && intento < 20; intento++) {
            esperar_milisegundos(10);
            statests = mmio_leer16(g_mmio_base + REG_STATESTS);
        }

        if (statests == 0) {
            serial_imprimir_linea("  [HDA AVISO] Ningún códec respondió (STATESTS=0). Buscando alternativa...");
            continue;
        }

        // STATESTS es W1C: limpiar bits detectados
        mmio_escribir16(g_mmio_base + REG_STATESTS, statests);

        // Deshabilitar interrupciones
        mmio_escribir32(g_mmio_base + REG_INTCTL, 0x00000000);

        // 4. Leer capacidades globales
        uint16_t gcap = mmio_leer16(g_mmio_base + REG_GCAP);
        uint8_t num_iss = (gcap >> 8) & 0x0F;
        uint8_t num_oss = (gcap >> 12) & 0x0F;
        uint8_t num_bss = (gcap >> 3) & 0x1F;

        if (num_oss == 0) {
            serial_imprimir_linea("  [HDA AVISO] El controlador no tiene streams de salida de audio.");
            continue;
        }

        // Registrar datos del controlador exitoso en estado global
        g_hda_estado.controlador_detectado = 1;
        g_hda_estado.bus            = pci_hda->bus;
        g_hda_estado.ranura         = pci_hda->ranura;
        g_hda_estado.funcion        = pci_hda->funcion;
        g_hda_estado.id_proveedor   = pci_hda->id_proveedor;
        g_hda_estado.id_dispositivo = pci_hda->id_dispositivo;
        g_hda_estado.dir_fisica_mmio= pci_hda->barras[0].dir_base;
        g_hda_estado.dir_virtual_mmio = g_mmio_base;
        g_hda_estado.tamano_mmio    = tamano_mmio;
        g_hda_estado.num_iss        = num_iss;
        g_hda_estado.num_oss        = num_oss;
        g_hda_estado.num_bss        = num_bss;
        g_hda_estado.codecs_detectados = statests;

        serial_imprimir("[HDA] Seleccionado con éxito. Input Streams: ");
        serial_imprimir_dec(num_iss);
        serial_imprimir(" | Output Streams: ");
        serial_imprimir_dec(num_oss);
        serial_imprimir(" | Códecs Detectados (STATESTS): 0x");
        serial_imprimir_hex(statests);
        serial_imprimir_linea("");

        // 5. Asignar estructuras DMA de CORB y RIRB si aún no están asignadas
        if (!g_corb) {
            g_corb = (uint32_t *)dma_asignar_bufer_contiguo(1024, 128, &g_corb_fisica);
        }
        if (!g_rirb) {
            g_rirb = (struct rirb_entrada *)dma_asignar_bufer_contiguo(2048, 128, &g_rirb_fisica);
        }
        if (!g_corb || !g_rirb) {
            serial_imprimir_linea("[HDA ERROR] Falló asignación de memoria DMA para CORB/RIRB.");
            return -3;
        }

        // --- Configuración y Handshake Robusto de CORB ---
        // 1. Detener motor CORB
        mmio_escribir8(g_mmio_base + REG_CORBCTL, 0);
        for (int t = 0; (mmio_leer8(g_mmio_base + REG_CORBCTL) & CORBCTL_RUN) && t < 100; t++) {
            esperar_microsegundos(50);
        }
        // 2. Programar dirección base y tamaño (0x02 = 256 entradas)
        mmio_escribir32(g_mmio_base + REG_CORBLBASE, (uint32_t)g_corb_fisica);
        mmio_escribir32(g_mmio_base + REG_CORBUBASE, (uint32_t)(g_corb_fisica >> 32));
        mmio_escribir8(g_mmio_base + REG_CORBSIZE, 0x02);
        // 3. Reset del puntero de lectura CORBRP (bit 15: escribir 1, esperar 1, escribir 0, esperar 0)
        mmio_escribir16(g_mmio_base + REG_CORBRP, 0x8000);
        for (int t = 0; !(mmio_leer16(g_mmio_base + REG_CORBRP) & 0x8000) && t < 100; t++) {
            esperar_microsegundos(50);
        }
        mmio_escribir16(g_mmio_base + REG_CORBRP, 0x0000);
        for (int t = 0; (mmio_leer16(g_mmio_base + REG_CORBRP) & 0x8000) && t < 100; t++) {
            esperar_microsegundos(50);
        }
        // 4. Reset del puntero de escritura CORBWP
        mmio_escribir16(g_mmio_base + REG_CORBWP, 0x0000);
        g_corb_wp = 0;
        // 5. Iniciar motor CORB
        mmio_escribir8(g_mmio_base + REG_CORBCTL, CORBCTL_RUN);
        for (int t = 0; !(mmio_leer8(g_mmio_base + REG_CORBCTL) & CORBCTL_RUN) && t < 100; t++) {
            esperar_microsegundos(50);
        }

        // --- Configuración y Handshake Robusto de RIRB ---
        // 1. Detener motor RIRB
        mmio_escribir8(g_mmio_base + REG_RIRBCTL, 0);
        for (int t = 0; (mmio_leer8(g_mmio_base + REG_RIRBCTL) & RIRBCTL_RUN) && t < 100; t++) {
            esperar_microsegundos(50);
        }
        // 2. Programar dirección base y tamaño (0x02 = 256 entradas)
        mmio_escribir32(g_mmio_base + REG_RIRBLBASE, (uint32_t)g_rirb_fisica);
        mmio_escribir32(g_mmio_base + REG_RIRBUBASE, (uint32_t)(g_rirb_fisica >> 32));
        mmio_escribir8(g_mmio_base + REG_RIRBSIZE, 0x02);
        // 3. Reset del puntero de escritura RIRBWP (bit 15: escribir 1, esperar 1, escribir 0, esperar 0)
        mmio_escribir16(g_mmio_base + REG_RIRBWP, 0x8000);
        for (int t = 0; !(mmio_leer16(g_mmio_base + REG_RIRBWP) & 0x8000) && t < 100; t++) {
            esperar_microsegundos(50);
        }
        mmio_escribir16(g_mmio_base + REG_RIRBWP, 0x0000);
        for (int t = 0; (mmio_leer16(g_mmio_base + REG_RIRBWP) & 0x8000) && t < 100; t++) {
            esperar_microsegundos(50);
        }
        // 4. Limpiar estado de interrupciones RIRB
        mmio_escribir8(g_mmio_base + REG_RIRBSTS, 0x05); // W1C: RINTFL y ERIS
        mmio_escribir16(g_mmio_base + REG_RINTCNT, 1);
        g_rirb_rp = 0;
        // 5. Iniciar motor RIRB
        mmio_escribir8(g_mmio_base + REG_RIRBCTL, RIRBCTL_RUN);
        for (int t = 0; !(mmio_leer8(g_mmio_base + REG_RIRBCTL) & RIRBCTL_RUN) && t < 100; t++) {
            esperar_microsegundos(50);
        }

        serial_imprimir_linea("[HDA] Anillos CORB/RIRB activos y sincronizados con el silicio.");

        // 6. Asignar BDL y búfer DMA ping-pong
        if (!g_bdl) {
            g_bdl = (struct hda_bdl_entrada *)dma_asignar_bufer_contiguo(HDA_MAX_BDL_ENTRADAS * sizeof(struct hda_bdl_entrada), 128, &g_bdl_fisica);
        }
        if (!g_dma_pcm_buffer) {
            g_dma_pcm_buffer = (uint8_t *)dma_asignar_bufer_contiguo(HDA_TAMANO_TOTAL_DMA, 128, &g_dma_pcm_fisica);
        }
        if (!g_bdl || !g_dma_pcm_buffer) {
            serial_imprimir_linea("[HDA ERROR] Falló asignación de memoria DMA para BDL y PCM.");
            return -4;
        }

        // Configurar las 2 entradas del BDL
        g_bdl[0].dir_fisica      = g_dma_pcm_fisica;
        g_bdl[0].longitud_bytes  = HDA_TAMANO_BLOQUE_DMA;
        g_bdl[0].ioc             = 1;

        g_bdl[1].dir_fisica      = g_dma_pcm_fisica + HDA_TAMANO_BLOQUE_DMA;
        g_bdl[1].longitud_bytes  = HDA_TAMANO_BLOQUE_DMA;
        g_bdl[1].ioc             = 1;

        // 7. Base del primer Descriptor de Stream de Salida
        g_stream_base = g_mmio_base + 0x80 + (g_hda_estado.num_iss * 0x20);

        // 8. Configurar códecs detectados
        uint64_t t_inicio_codecs = tiempo_obtener_milisegundos();
        uint64_t t_limite_codecs = t_inicio_codecs + 3000;

        for (uint8_t c = 0; c < 15; c++) {
            if (statests & (1U << c)) {
                if (tiempo_obtener_milisegundos() >= t_limite_codecs) {
                    serial_imprimir_linea("[HDA AVISO] Tiempo máximo de configuración de códecs alcanzado.");
                    break;
                }
                hda_configurar_nodos_codec(c);
            }
        }

        g_hda_estado.inicializado = 1;
        serial_imprimir_linea("[HDA EXITOSO] ¡Controlador Intel HDA inicializado y listo para reproducir!");
        return 0;
    }

    serial_imprimir_linea("[HDA AVISO] Ningún controlador Intel HDA / Audio completó la inicialización.");
    return -1;
}
