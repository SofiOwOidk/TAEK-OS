#ifndef ARQUITECTURA_X86_64_THREAD_DIRECTOR_H
#define ARQUITECTURA_X86_64_THREAD_DIRECTOR_H
/* H215, CONGELADO (decision del usuario, 2026-11-17): no se sigue desarrollando hasta tener un equipo hibrido para
 * probarlo. Queda compilado, apagado por defecto y sin efecto si no se pide.
 *
 * Intel Thread Director y su tabla HFI (Hardware Feedback Interface).
 *
 * En procesadores hibridos (12.a generacion en adelante, nucleos P y E) el hardware publica en memoria una tabla con
 * la capacidad de rendimiento y de eficiencia de cada CPU logica. TAEK la usa para una sola cosa: elegir la CPU de un
 * hilo de programa (SYS_HILO CREAR sin CPU) cuando el usuario la enciende con 'thread director encender'.
 *
 * La i7-8650U de las pruebas no tiene HFI ni Thread Director: ahi solo se detecta y se dice "no disponible". El camino
 * que escribe MSR esta sin probar en hardware; nunca se activa solo. */
#include <stdint.h>

#define TD_CPUS 4

struct td_cpu {
    int visto;            /* La CPU contesto el CPUID */
    uint8_t tipo;         /* CPUID.1AH:EAX[31:24]: 0x40 nucleo P, 0x20 nucleo E, 0 sin dato */
    uint16_t fila;        /* CPUID.06H:EDX[31:16]: su fila en la tabla HFI */
};

struct td_estado {
    int detectado;
    uint32_t cpuid_max;
    int hibrido;          /* CPUID.07H:EDX[15] */
    int hfi;              /* CPUID.06H:EAX[19] */
    int itd;              /* CPUID.06H:EAX[23] */
    unsigned clases;      /* Con Thread Director, CPUID.06H:ECX[15:8]; sin el, 1 */
    uint8_t capacidades;  /* CPUID.06H:EDX[7:0]: bit 0 rendimiento, bit 1 eficiencia */
    unsigned paginas;     /* CPUID.06H:EDX[11:8] + 1 */
    int activo;
    uint64_t tabla_fisica;
    const uint8_t *tabla;
    struct td_cpu cpu[TD_CPUS];
};

const struct td_estado *thread_director_detectar(void);   /* CPUID en cada CPU (hilos cortos en las auxiliares) */
int  thread_director_encender(void);                      /* 0 bien; -1 sin HFI; -2 sin memoria */
void thread_director_apagar(void);
/* Capacidad de rendimiento (0-255) de cada CPU segun la tabla, en perf[0..n-1]. 0 si no esta activo. */
int  thread_director_rendimiento(uint8_t *perf, unsigned n);
/* Lectura de una tabla HFI con el formato de Linux (intel_hfi.c): rendimiento y eficiencia de una fila y clase */
void thread_director_leer_fila(const uint8_t *tabla, unsigned clases, unsigned fila, unsigned clase,
                               uint8_t *rendimiento, uint8_t *eficiencia);
/* Elige entre n CPU la de mejor rendimiento por hilo ya puesto en ella (desde la 1; la 0 es del principal) */
unsigned thread_director_elegir(const uint8_t *perf, const unsigned *hilos, unsigned n);
void thread_director_comando(const char *arg);

#endif
