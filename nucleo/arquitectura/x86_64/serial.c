#include "serial.h"
#include "puertos.h"

static int  g_serial_listo = 0;
static char g_kernel_log[KERNEL_LOG_TAMANO];
static uint32_t g_log_cursor = 0;
static uint64_t g_log_total_bytes = 0;
static unsigned char g_log_lock;
static char g_uart_cola[65536];
static uint32_t g_uart_inicio,g_uart_total;
static uint64_t g_uart_perdidos;

static uint64_t serial_log_bloquear(void) {
    uint64_t flags = 0;
#ifndef TAEK_SERIAL_HOST_TEST
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#endif
    while (__atomic_test_and_set(&g_log_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    return flags;
}

static void serial_log_desbloquear(uint64_t flags) {
    __atomic_clear(&g_log_lock, __ATOMIC_RELEASE);
#ifndef TAEK_SERIAL_HOST_TEST
    __asm__ volatile ("pushq %0; popfq" :: "r"(flags) : "memory", "cc");
#else
    (void)flags;
#endif
}

int serial_iniciar(void) {
    escribir_puerto_b(PUERTO_COM1 + 1, 0x00);    // Desactivar todas las interrupciones
    escribir_puerto_b(PUERTO_COM1 + 3, 0x80);    // Activar DLAB (divisor de velocidad)
    escribir_puerto_b(PUERTO_COM1 + 0, 0x01);    // Divisor 1 (115200 baudios) byte bajo
    escribir_puerto_b(PUERTO_COM1 + 1, 0x00);    // byte alto
    escribir_puerto_b(PUERTO_COM1 + 3, 0x03);    // 8 bits, sin paridad, un bit de parada (8N1)
    escribir_puerto_b(PUERTO_COM1 + 2, 0xC7);    // Activar colas FIFO, limpiar búferes
    escribir_puerto_b(PUERTO_COM1 + 4, 0x0B);    // Interrupciones activadas, RTS/DSR listos
    escribir_puerto_b(PUERTO_COM1 + 4, 0x1E);    // Modo bucle invertido para autoprueba
    escribir_puerto_b(PUERTO_COM1 + 0, 0xAE);    // Byte de prueba

    // Verificar si el chip UART responde correctamente
    if (leer_puerto_b(PUERTO_COM1 + 0) != 0xAE) {
        g_serial_listo = 0;
        return 1;
    }

    // Configurar en modo de operación normal
    escribir_puerto_b(PUERTO_COM1 + 4, 0x0F);
    g_serial_listo = 1;
    return 0;
}

int serial_esta_activo(void) {
    return g_serial_listo;
}

static inline int serial_transmisor_vacio(void) {
    return leer_puerto_b(PUERTO_COM1 + 5) & 0x20;
}
// FIFO 16550 habilitada por serial_iniciar: vaciar hasta 16 bytes sin espera.
// El log RAM conserva toda la salida aun cuando una UART lenta pierda bytes.
static void uart_atender_bloqueado(void){
    if(!g_serial_listo || !g_uart_total || !serial_transmisor_vacio())return;
    unsigned n=g_uart_total>16?16:g_uart_total;
    while(n--){escribir_puerto_b(PUERTO_COM1,(uint8_t)g_uart_cola[g_uart_inicio]);g_uart_inicio=(g_uart_inicio+1)%sizeof(g_uart_cola);g_uart_total--;}
}
void serial_atender(void){uint64_t flags=serial_log_bloquear();uart_atender_bloqueado();serial_log_desbloquear(flags);}
uint64_t serial_uart_perdidos(void){uint64_t flags=serial_log_bloquear();uint64_t n=g_uart_perdidos;serial_log_desbloquear(flags);return n;}

int serial_hay_datos(void) {
    if (!g_serial_listo) return 0;
    return leer_puerto_b(PUERTO_COM1 + 5) & 0x01;
}

char serial_leer_caracter(void) {
    if (!g_serial_listo) return 0;
    while (serial_hay_datos() == 0);
    return (char)leer_puerto_b(PUERTO_COM1);
}

void serial_escribir_caracter(char c) {
    // 1. Guardar siempre en el búfer circular de log en memoria (dmesg)
    uint64_t flags = serial_log_bloquear();
    g_kernel_log[g_log_cursor] = c;
    g_log_cursor = (g_log_cursor + 1) % (KERNEL_LOG_TAMANO - 1);
    g_log_total_bytes++;
    // No escribir un terminador sobre bytes retenidos cuando el anillo esta lleno.
    if (g_log_total_bytes < KERNEL_LOG_TAMANO - 1) g_kernel_log[g_log_cursor] = '\0';
    uart_atender_bloqueado();
    if(g_serial_listo){
        if(g_uart_total<sizeof(g_uart_cola)){g_uart_cola[(g_uart_inicio+g_uart_total)%sizeof(g_uart_cola)]=c;g_uart_total++;}
        else g_uart_perdidos++;
        uart_atender_bloqueado();
    }
    serial_log_desbloquear(flags);
}

void serial_imprimir(const char *texto) {
    if (!texto) return;
    for (int i = 0; texto[i] != '\0'; i++) {
        if (texto[i] == '\n') {
            serial_escribir_caracter('\r');
        }
        serial_escribir_caracter(texto[i]);
    }
}

void serial_imprimir_linea(const char *texto) {
    serial_imprimir(texto);
    serial_imprimir("\n");
}

void serial_imprimir_hex(uint64_t valor) {
    const char hex[] = "0123456789ABCDEF";
    serial_imprimir("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t cuarteto = (valor >> i) & 0xF;
        serial_escribir_caracter(hex[cuarteto]);
    }
}

void serial_imprimir_dec(uint64_t valor) {
    if (valor == 0) {
        serial_escribir_caracter('0');
        return;
    }
    char buf[24];
    int idx = 0;
    while (valor > 0) {
        buf[idx++] = '0' + (valor % 10);
        valor /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) {
        serial_escribir_caracter(buf[i]);
    }
}

const char *serial_obtener_log_buffer(uint32_t *tamano_out, uint32_t *cursor_out) {
    if (tamano_out) *tamano_out = g_log_total_bytes >= KERNEL_LOG_TAMANO - 1 ? KERNEL_LOG_TAMANO - 1 : (uint32_t)g_log_total_bytes;
    if (cursor_out) *cursor_out = g_log_cursor;
    return g_kernel_log;
}

uint64_t serial_log_posicion(void) {
    uint64_t flags = serial_log_bloquear();
    uint64_t total = g_log_total_bytes;
    serial_log_desbloquear(flags);
    return total;
}

uint32_t serial_leer_desde(uint64_t *cursor, void *salida, uint32_t capacidad, uint64_t *perdidos) {
    if (!cursor || !salida || !capacidad) return 0;
    uint64_t flags = serial_log_bloquear();
    uint64_t primero = g_log_total_bytes > KERNEL_LOG_TAMANO - 1 ?
        g_log_total_bytes - (KERNEL_LOG_TAMANO - 1) : 0;
    uint64_t perdido = *cursor < primero ? primero - *cursor : 0;
    if (*cursor < primero) *cursor = primero;
    if (*cursor > g_log_total_bytes) *cursor = g_log_total_bytes;
    uint64_t disponibles = g_log_total_bytes - *cursor;
    uint32_t n = disponibles < capacidad ? (uint32_t)disponibles : capacidad;
    for (uint32_t i = 0; i < n; ++i)
        ((char *)salida)[i] = g_kernel_log[(*cursor + i) % (KERNEL_LOG_TAMANO - 1)];
    *cursor += n;
    if (perdidos) *perdidos = perdido;
    serial_log_desbloquear(flags);
    return n;
}

int serial_copiar_log(char *salida, uint32_t capacidad, uint64_t *total, uint64_t *perdidos) {
    if (!salida) return -1;
    uint64_t flags = serial_log_bloquear();
    uint32_t retenidos = g_log_total_bytes < KERNEL_LOG_TAMANO - 1 ?
        (uint32_t)g_log_total_bytes : KERNEL_LOG_TAMANO - 1;
    if (capacidad <= retenidos) {
        serial_log_desbloquear(flags);
        return -1;
    }
    uint32_t inicio = g_log_total_bytes >= KERNEL_LOG_TAMANO - 1 ? g_log_cursor : 0;
    for (uint32_t i = 0; i < retenidos; i++)
        salida[i] = g_kernel_log[(inicio + i) % (KERNEL_LOG_TAMANO - 1)];
    salida[retenidos] = '\0';
    if (total) *total = g_log_total_bytes;
    if (perdidos) *perdidos = g_log_total_bytes - retenidos;
    serial_log_desbloquear(flags);
    return (int)retenidos;
}
