#ifndef TAEK_BASE_ATENCION_H
#define TAEK_BASE_ATENCION_H

/* H204, Fase 0 del plan del gestor de hilos (planes/PLAN_GESTOR_HILOS.md): cuanto tarda CPU 0 en volver a atender a
 * cada subsistema que hoy va por sondeo, y cuanto dura cada llamada al sistema. Solo mide: no cambia el flujo.
 * Todo esto corre en CPU 0 (el BSP), asi que no lleva cerrojos. */

#include <stdint.h>

#define ATENCION_SIN_SYS 0xFFu   /* Ninguna llamada al sistema en curso: corre el programa o la consola del nucleo */
#define ATENCION_SYS_MAX 32u
enum { ATENCION_USB = 1, ATENCION_RED = 2 };

struct atencion_medida {
    uint64_t ultimo_ms;       /* Ultima pasada */
    uint64_t pasadas;
    uint32_t max_ms;          /* Mayor hueco entre dos pasadas seguidas */
    uint16_t max_sys_ini;     /* SYS_* en curso en la pasada que abrio ese hueco, o ATENCION_SIN_SYS */
    uint16_t max_sys_fin;     /* ... y en la que lo cerro */
    uint64_t max_cuando_ms;   /* Milisegundos desde el arranque en que se cerro */
    uint32_t ultimo_sys;      /* SYS_* en curso en la ultima pasada */
    uint32_t que;             /* ATENCION_USB o ATENCION_RED */
};

/* Bitacora de los ultimos huecos grandes, de cualquier subsistema, para cruzarlos con la consola */
#define ATENCION_HUECO_MIN_MS 250u
#define ATENCION_HUECOS 8u
struct atencion_hueco { uint64_t fin_ms; uint32_t ms; uint8_t que, sys_ini, sys_fin, reservado; };
extern struct atencion_hueco atencion_huecos[ATENCION_HUECOS];
extern uint32_t atencion_huecos_total;
/* Copia los ultimos huecos del mas viejo al mas nuevo; devuelve cuantos */
unsigned atencion_huecos_copiar(struct atencion_hueco *out, unsigned max);

extern struct atencion_medida atencion_usb;   /* xhci_bombear_eventos: el anillo de eventos del xHCI */
extern struct atencion_medida atencion_red;   /* red_pila_sondeo: WiFi, e1000, TCP y TLS */
extern volatile uint32_t atencion_sys_actual;
extern uint32_t atencion_sys_max_us[ATENCION_SYS_MAX];

/* Una pasada del subsistema: cierra el hueco desde la anterior. */
void atencion_marcar(struct atencion_medida *m);
/* Al terminar una llamada al sistema: su duracion en ciclos del TSC de CPU 0. */
void atencion_sys_terminar(uint32_t numero, uint64_t ciclos);
/* Pone a cero los maximos y las pasadas. El hueco que esta abierto se sigue midiendo desde su inicio real.
 * La bitacora de huecos no se borra: cada uno lleva su hora. */
void atencion_cero(void);
extern uint64_t atencion_cero_ms;   /* Cuando se puso a cero por ultima vez (0 = nunca: cuenta desde el arranque) */
/* Nombre de una SYS_* sin el prefijo ("FLUJO"), "sin SYS" para ATENCION_SIN_SYS, "?" si no existe */
const char *atencion_sys_nombre(uint32_t numero);

/* H215, Fase E1 (fluidez): la carga real de cada CPU.
 *  - CPU 0 no duerme: gira esperando. Las esperas conocidas (esperar_*, SYS_ESPERAR, futex y unir del programa, la
 *    consola sin teclas) suman aqui su tiempo; lo demas es trabajo. Solo CPU 0 llama a estas funciones.
 *  - Las CPU 1 a 3 cuentan su tiempo en hlt (smp.c); trabajos_ocupado_ciclos devuelve lo que no durmieron.
 * Una espera dentro de otra no suma dos veces: solo cuenta la de afuera. */
extern volatile uint64_t atencion_cpu0_espera;   /* Ciclos del TSC */
extern uint32_t atencion_espera_anidada;
static inline uint64_t atencion_tsc(void) { uint32_t a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return ((uint64_t)d << 32) | a; }
static inline uint64_t atencion_espera_inicio(void) { return atencion_espera_anidada++ ? 0 : atencion_tsc(); }
/* sumar = 0: no era espera (por ejemplo, la consola encontro una tecla) */
static inline void atencion_espera_fin(uint64_t t0, int sumar) {
    if (atencion_espera_anidada) atencion_espera_anidada--;
    if (t0 && sumar) atencion_cpu0_espera += atencion_tsc() - t0;
}
/* Carga desde atencion_cero: en milesimas (0 a 1000) para no usar coma flotante en el nucleo */
struct atencion_carga { uint32_t cpus; uint32_t cpu0_trabajo, cpu0_espera; uint32_t ocupado[4]; uint64_t ms; };
void atencion_carga(struct atencion_carga *c);

#endif
