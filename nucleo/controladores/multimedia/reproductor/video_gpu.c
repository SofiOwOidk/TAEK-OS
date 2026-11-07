/* Eleccion del motor de video. La decodificacion por GPU solo se intenta en las GPU ya probadas: en cualquier otra el fallo podria
 * colgar la maquina en vez de devolver un error, asi que el video va por software. Anadir una GPU a la lista exige probarla antes. */
#include "video_gpu.h"
#include "arquitectura/x86_64/pci.h"
#include "arquitectura/x86_64/serial.h"
#include "controladores/consola.h"
#include "controladores/driver_ram.h"

static const uint16_t gpu_probadas[] = {0x5917};   /* i7-8650U */

enum { MOTOR_AUTO, MOTOR_SOFTWARE };
static int motor = MOTOR_AUTO, lista, fuera;
static const char *por_que_fuera;
static int fallar_preparar;
static uint32_t inyectar_tipo, inyectar_muestra;

static int gpu_en_lista(void) {
    if (pci_leer_config_16(0, 2, 0, 0) != 0x8086) return 0;
    uint16_t id = pci_leer_config_16(0, 2, 0, 2);
    for (unsigned i = 0; i < sizeof(gpu_probadas) / sizeof(gpu_probadas[0]); i++)
        if (gpu_probadas[i] == id) return 1;
    return 0;
}

int video_gpu_preparar(const void *modulo, size_t bytes, const char **motivo) {
    if (motor == MOTOR_SOFTWARE) { *motivo = "elegido con 'h264 motor software'"; return -1; }
    if (fuera) { *motivo = por_que_fuera; return -1; }
    if (!gpu_en_lista()) { *motivo = "GPU sin probar con el shim"; return -1; }
    if (lista) {
        /* Sigue el modulo que hizo la preparacion: nadie puede descargarlo con el dispositivo retenido. */
        if (driver_ram_tiene_video()) return 0;
        lista = 0;
    }
    if (fallar_preparar) {
        fallar_preparar = 0;
        *motivo = "fallo inyectado al preparar la GPU";
        return -1;
    }
    if (!driver_ram_tiene_video()) {
        if (!modulo || !bytes) { *motivo = "esta imagen no incluye el modulo i915"; return -1; }
        int r = driver_ram_cargar_bytes(modulo, bytes);
        if (r == -2) { *motivo = "hay otro modulo cargado"; return -1; }
        if (r || !driver_ram_tiene_video()) { *motivo = "no se pudo cargar el modulo i915"; return -1; }
    }
    consola_imprimir_linea("[VIDEO] Iniciando la GPU Intel...");
    int r = driver_ram_video(TAEK_VIDEO_PREPARAR, NULL);
    if (r) {
        fuera = 1;
        por_que_fuera = "la GPU no se pudo iniciar; hace falta reiniciar para reintentar";
        *motivo = por_que_fuera;
        return -1;
    }
    lista = 1;
    return 0;
}

int video_gpu_orden(struct taek_video_peticion *p, uint32_t orden) {
    p->orden = orden;
    p->motivo[0] = 0;
    int r = driver_ram_video(orden, p);
    if (r <= -1000) { const char *m = "modulo i915 no disponible"; unsigned i = 0; while (m[i]) { p->motivo[i] = m[i]; i++; } p->motivo[i] = 0; return TAEK_VIDEO_FALLO; }
    if (r < 0 && r != TAEK_VIDEO_INCOMPATIBLE && !p->motivo[0]) {
        /* Sin texto del cliente: la orden no llego a terminar y el modulo da la GPU por perdida. */
        const char *m = "la GPU dejo de responder";
        unsigned i = 0; while (m[i]) { p->motivo[i] = m[i]; i++; } p->motivo[i] = 0;
        fuera = 1; por_que_fuera = "la GPU dejo de responder; hace falta reiniciar para reintentar";
    }
    return r;
}

void video_gpu_inyeccion(struct taek_video_peticion *p) {
    p->inyectar_tipo = inyectar_tipo;
    p->inyectar_muestra = inyectar_muestra;
    inyectar_tipo = 0;
    inyectar_muestra = 0;
}

static int empieza(const char *a, const char *p) { while (*p) if (*a++ != *p++) return 0; return 1; }
static int igual(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void estado(void) {
    consola_imprimir("[VIDEO] Motor: ");
    consola_imprimir_linea(motor == MOTOR_SOFTWARE ? "software (forzado)" : "auto (GPU si esta probada; si no, software)");
    consola_imprimir("[VIDEO] GPU en la lista de probadas: ");
    consola_imprimir_linea(gpu_en_lista() ? "si" : "no");
    consola_imprimir("[VIDEO] Estado de la GPU: ");
    consola_imprimir_linea(fuera ? por_que_fuera : lista ? "iniciada" : "sin iniciar");
}

int video_gpu_comando(const char *a) {
    if (igual(a, "motor")) { estado(); return 1; }
    if (igual(a, "motor auto")) { motor = MOTOR_AUTO; estado(); return 1; }
    if (igual(a, "motor software")) { motor = MOTOR_SOFTWARE; estado(); return 1; }
    /* Solo para probar el respaldo: afectan al siguiente video. */
    if (igual(a, "gpu fallo preparar")) { fallar_preparar = 1; consola_imprimir_linea("[VIDEO] El siguiente video fallara al preparar la GPU."); return 1; }
    if (empieza(a, "gpu fallo ") || empieza(a, "gpu bloqueo ")) {
        int bloqueo = a[4] == 'b';
        const char *c = a + (bloqueo ? 12 : 10);
        uint32_t n = 0;
        if (*c < '0' || *c > '9') return 0;
        while (*c >= '0' && *c <= '9') n = n * 10 + (uint32_t)(*c++ - '0');
        if (*c || !n) return 0;
        inyectar_tipo = bloqueo ? TAEK_VIDEO_INYECTAR_BLOQUEO : TAEK_VIDEO_INYECTAR_ERROR;
        inyectar_muestra = n;
        consola_imprimir("[VIDEO] El siguiente video fallara en la muestra ");
        consola_imprimir_dec(n);
        consola_imprimir_linea("");
        return 1;
    }
    return 0;
}
