#include "red.h"
#include "../../arquitectura/x86_64/serial.h"
#include "../../base/version.h"
#include "../../base/memoria.h"

#ifndef TAEK_REVISION_CODIGO
#define TAEK_REVISION_CODIGO "NO_IDENTIFICADA"
#endif

static unsigned char g_ocupada;
static uint64_t g_boot_id, g_secuencia;
static char g_log_snapshot[KERNEL_LOG_TAMANO];
static char g_expediente[KERNEL_LOG_TAMANO + 1024];

static void texto(uint32_t *n, const char *s) {
    while (*s && *n < sizeof(g_expediente)) g_expediente[(*n)++] = *s++;
}

static void decimal(uint32_t *n, uint64_t v) {
    char digits[24];
    int count = 0;
    do { digits[count++] = '0' + v % 10; v /= 10; } while (v);
    while (count && *n < sizeof(g_expediente)) g_expediente[(*n)++] = digits[--count];
}

static void be32(uint8_t *p, uint32_t v) {
    for (int i = 3; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static void be64(uint8_t *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xEDB88320u);
    }
    return ~crc;
}

int red_telemetria_enviar(const uint8_t ip[4], uint16_t puerto,
                         uint32_t *bytes_enviados, uint64_t *bytes_perdidos) {
    if (!ip || !puerto) return -1;
    if (__atomic_test_and_set(&g_ocupada, __ATOMIC_ACQUIRE)) return -16;
    if (bytes_enviados) *bytes_enviados = 0;
    uint64_t total = 0, perdidos = 0;
    int nlog = serial_copiar_log(g_log_snapshot, sizeof(g_log_snapshot), &total, &perdidos);
    int resultado = -1;
    if (nlog < 0) goto salir;
    if (bytes_perdidos) *bytes_perdidos = perdidos;
    if (!g_boot_id) {
        uint32_t lo, hi;
        __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
        g_boot_id = ((uint64_t)hi << 32) | lo;
        if (!g_boot_id) g_boot_id = 1;
    }
    uint32_t n = 0;
    texto(&n, "TAEK_TELEMETRIA_V1\nBUILD_REV="); texto(&n, TAEK_REVISION_CODIGO);
    texto(&n, "\nBUILD_DATE="); texto(&n, taek_obtener_fecha_compilacion());
    texto(&n, "\nBUILD_TIME="); texto(&n, taek_obtener_hora_compilacion());
    texto(&n, "\nVERSION="); texto(&n, taek_obtener_version());
    texto(&n, "\nLOG_TOTAL_BYTES="); decimal(&n, total);
    texto(&n, "\nLOG_LOST_BYTES="); decimal(&n, perdidos);
    texto(&n, "\nLOG_RETAINED_BYTES="); decimal(&n, (uint64_t)nlog);
    texto(&n, "\n[LOG]\n");
    if ((uint32_t)nlog > sizeof(g_expediente) - n) goto salir;
    memcpy(g_expediente + n, g_log_snapshot, (uint32_t)nlog);
    n += (uint32_t)nlog;
    uint8_t header[32], ack[32];
    memcpy(header, "TAEKLOG1", 8);
    be32(header + 8, n);
    be32(header + 12, crc32((const uint8_t *)g_expediente, n));
    be64(header + 16, g_boot_id);
    be64(header + 24, ++g_secuencia);
    resultado = red_tcp_intercambiar(ip, puerto, header, sizeof(header), g_expediente, n,
                                     ack, sizeof(ack), 20000);
    if (resultado == 0 && (memcmp(ack, "TAEKACK1", 8) != 0 ||
                           memcmp(ack + 8, header + 8, 24) != 0)) resultado = -4;
    if (resultado == 0 && bytes_enviados) *bytes_enviados = n;
salir:
    __atomic_clear(&g_ocupada, __ATOMIC_RELEASE);
    return resultado;
}
