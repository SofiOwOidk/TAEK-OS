#include "red.h"
#include "../../base/memoria.h"

// Sesiones TCP cortas; recibir consulta una cola del PC y confirma su consumo.
static uint8_t g_destino[4] = {192, 168, 18, 146};
static uint16_t g_puerto = 9151;
static unsigned char g_ocupada;
static int g_pendiente;
static uint8_t g_id_pendiente[8];
static char g_texto_pendiente[RED_MENSAJE_MAX + 1];

static void be32(uint8_t *p, uint32_t v) {
    for (int i = 3; i >= 0; --i) { p[i] = (uint8_t)v; v >>= 8; }
}
static uint32_t leer32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void red_mensajes_destino(const uint8_t ip[4], uint16_t puerto) {
    if (ip && puerto) {
        // No mezclar confirmaciones entre receptores diferentes.
        if (memcmp(ip, g_destino, 4) || puerto != g_puerto) g_pendiente = 0;
        memcpy(g_destino, ip, 4); g_puerto = puerto;
    }
}
void red_mensajes_obtener_destino(uint8_t ip[4], uint16_t *puerto) {
    if (ip) memcpy(ip, g_destino, 4);
    if (puerto) *puerto = g_puerto;
}

static int intercambiar(uint32_t op, const char *texto, uint32_t n,
                       const uint8_t *id, uint8_t respuesta[264]) {
    uint8_t cabecera[24] = {0}, datos[RED_MENSAJE_MAX] = {0};
    memcpy(cabecera, "TAEKMSG1", 8);
    be32(cabecera + 8, op); be32(cabecera + 12, n);
    if (id) memcpy(cabecera + 16, id, 8);
    if (n) memcpy(datos, texto, n);
    int r = red_tcp_intercambiar(g_destino, g_puerto, cabecera, sizeof(cabecera),
                                datos, sizeof(datos), respuesta, 264, 5000);
    if (r) return r;
    if (memcmp(respuesta, "TAEKRSP1", 8) || leer32(respuesta + 12) > RED_MENSAJE_MAX)
        return -4;
    return 0;
}

int red_mensaje_transmitir(const char *mensaje) {
    if (!mensaje) return -1;
    uint32_t n = 0;
    while (mensaje[n] && n <= RED_MENSAJE_MAX) {
        if ((uint8_t)mensaje[n] < 32 || (uint8_t)mensaje[n] == 127) return -1;
        ++n;
    }
    if (!n || n > RED_MENSAJE_MAX) return -1;
    if (__atomic_test_and_set(&g_ocupada, __ATOMIC_ACQUIRE)) return -16;
    uint8_t respuesta[264];
    int r = intercambiar(1, mensaje, n, 0, respuesta);
    if (!r && (leer32(respuesta + 8) != 1 || leer32(respuesta + 12))) r = -4;
    __atomic_clear(&g_ocupada, __ATOMIC_RELEASE);
    return r;
}

int red_mensaje_recibir(char *salida, uint32_t capacidad) {
    if (!salida || capacidad < RED_MENSAJE_MAX + 1) return -1;
    salida[0] = 0;
    if (__atomic_test_and_set(&g_ocupada, __ATOMIC_ACQUIRE)) return -16;
    uint8_t respuesta[264], ack[264];
    int r = 0;
    // Un ACK de aplicacion perdido no debe descartar texto ya recibido.
    if (g_pendiente) goto confirmar;
    r = intercambiar(2, 0, 0, 0, respuesta);
    if (r) goto salir;
    uint32_t estado = leer32(respuesta + 8), n = leer32(respuesta + 12);
    if (estado == 2 && !n) goto salir;
    if (estado != 3 || !n) { r = -4; goto salir; }
    for (uint32_t i = 0; i < n; ++i) {
        if (respuesta[24 + i] < 32 || respuesta[24 + i] == 127) { r = -4; goto salir; }
    }
    memcpy(g_texto_pendiente, respuesta + 24, n); g_texto_pendiente[n] = 0;
    memcpy(g_id_pendiente, respuesta + 16, 8); g_pendiente = 1;
confirmar:
    r = intercambiar(3, 0, 0, g_id_pendiente, ack);
    if (!r && (leer32(ack + 8) != 4 || leer32(ack + 12) ||
               memcmp(ack + 16, g_id_pendiente, 8))) r = -4;
    if (!r) {
        memcpy(salida, g_texto_pendiente, sizeof(g_texto_pendiente));
        g_pendiente = 0; r = 1;
    }
salir:
    __atomic_clear(&g_ocupada, __ATOMIC_RELEASE);
    return r;
}
