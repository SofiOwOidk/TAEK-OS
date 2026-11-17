#ifndef TAEK_BASE_HILOS_H
#define TAEK_BASE_HILOS_H

/* H206, Fase A1 del plan del gestor de hilos (planes/PLAN_GESTOR_HILOS.md): hilos del nucleo con desalojo.
 *
 * Alcance de la A1 (lo que este archivo hace y lo que todavia no):
 *  - los hilos corren en las CPU 1 a 3, con afinidad fija; CPU 0 sigue como hasta ahora y no ejecuta hilos;
 *  - el bucle de cada CPU auxiliar (smp_trabajador) es su raiz: atiende primero los lotes de trabajos_ejecutar, que
 *    nunca se desalojan, y despues los hilos listos, por prioridad y por turnos;
 *  - cada CPU con hilos tiene un tictac de 1 ms (temporizador APIC periodico, vector 48). Sin hilos, lo apaga;
 *  - el tictac desaloja al hilo en curso al acabar su turno, o antes si hay listo uno de mas prioridad. El cambio se
 *    hace al final del despachador de interrupciones, ya enviado el EOI, y nunca si esa CPU tiene un cerrojo en la mano;
 *  - el cambio voluntario y el forzado usan el mismo i915_cambiar_contexto: la interrupcion ya dejo RAX-R15, RIP,
 *    RFLAGS y el estado FPU del hilo en su propia pila (trampa_comun_stub).
 *
 * Desde CPU 0 se puede crear hilos, senalar eventos y soltar mutex; esperar, dormir o tomar un mutex solo desde un
 * hilo (devuelven -1 si no). */

#include <stdint.h>
#include <stddef.h>
#include "cerrojo.h"

enum hilo_prioridad { HILO_TIEMPO_REAL = 0, HILO_NORMAL = 1, HILO_FONDO = 2, HILO_PRIORIDADES = 3 };
enum hilo_estado { HILO_LISTO = 1, HILO_EJECUTANDO, HILO_CEDIDO, HILO_DORMIDO, HILO_BLOQUEADO, HILO_MUERTO };

typedef struct hilo {
    void *rsp;                     /* Contexto guardado (i915_contexto_marco) */
    void *pila;
    size_t pila_bytes;
    void *tls;
    void (*fn)(void *);
    void *arg;
    const char *nombre;
    uint32_t id;
    uint8_t cpu, prioridad;
    volatile uint8_t estado;
    uint8_t reservado;
    uint64_t despertar_tsc;        /* DORMIDO: cuando vuelve */
    uint64_t turnos, desalojos;    /* Veces que corrio; veces que el tictac lo saco */
    struct hilo *siguiente;        /* Cola de listos, de dormidos o de un evento */
    /* H213, Fase D (al final, para no mover lo anterior): si el hilo baja a Anillo 3, la pila de nucleo de sus entradas
     * (RSP0 del TSS de su CPU). La raiz la carga al darle el turno. 0 en los hilos que solo corren en el nucleo. */
    uint64_t rsp0;
} hilo_t;

/* Un evento con memoria: una senal sin nadie esperando queda guardada para el proximo que espere. */
typedef struct evento {
    cerrojo_t cerrojo;
    hilo_t *esperando;
    uint32_t senales;
} evento_t;
#define EVENTO(nombre_) { .cerrojo = CERROJO(nombre_) }

typedef struct mutex_hilo {
    evento_t evento;
    hilo_t *dueno;
} mutex_t;
#define MUTEX(nombre_) { .evento = EVENTO(nombre_) }

/* Crea un hilo en la CPU indicada (1 a 3). La pila es de 64 KiB si pila_bytes es 0. NULL si no se pudo. */
hilo_t *hilo_crear(void (*fn)(void *), void *arg, const char *nombre, unsigned cpu, unsigned prioridad, size_t pila_bytes);
hilo_t *hilo_actual(void);          /* NULL fuera de un hilo (CPU 0, la raiz, una interrupcion) */
void hilo_ceder(void);
int  hilo_dormir_ms(uint32_t ms);
void hilo_salir(void) __attribute__((noreturn));
int  hilo_esperar(evento_t *e);
void evento_senalar(evento_t *e);
int  mutex_tomar(mutex_t *m);
void mutex_soltar(mutex_t *m);

/* Para la raiz de cada CPU auxiliar (smp.c), con las interrupciones apagadas. Ejecuta un turno de un hilo listo y
 * devuelve 1; 0 si esa CPU no tiene nada que hacer (entonces ya apago el tictac si no tiene hilos). */
int hilos_raiz(unsigned cpu);
/* Al final del despachador de interrupciones, con las interrupciones apagadas y la profundidad en cero. Desde H213
 * tambien cuando la interrupcion llego desde Anillo 3 (hilos de programa, Fase D): puede_cambiar = 1 en los dos casos. */
void hilos_tras_irq(int puede_cambiar);
/* Al arrancar cada CPU auxiliar */
void hilos_cpu_iniciar(unsigned cpu);

struct hilos_estadisticas {
    uint64_t tics, cambios, desalojos, vivos, creados;
    uint64_t tic_desvio_max_ciclos;   /* Mayor diferencia entre dos tictacs y el periodo nominal */
    uint64_t apic_por_ms;             /* Cuenta del temporizador APIC por ms, calibrada en esa CPU */
    int tictac;                       /* 1 si el tictac esta encendido */
};
void hilos_estadisticas(unsigned cpu, struct hilos_estadisticas *e);
void hilos_cero(void);

#endif
