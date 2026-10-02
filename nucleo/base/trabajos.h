#ifndef TAEK_TRABAJOS_H
#define TAEK_TRABAJOS_H
#include <stdint.h>
/* Lote síncrono, como máximo 256 regiones y cuatro ejecutores. El índice de
 * ejecutor identifica exclusivamente el contexto privado de un trabajador.
 * El coordinador es el único que publica/cancela; nadie asigna en el lote. */
#define TRABAJOS_MAX_CPU 4
#define TRABAJOS_MAX_REGIONES 256
typedef void (*trabajo_fn)(void *, unsigned region, unsigned ejecutor);
typedef struct {
    trabajo_fn funcion;
    void *contexto;
    unsigned total, siguiente, completados, cancelado, activo;
    unsigned ejecutados[TRABAJOS_MAX_CPU];
    uint64_t ciclos[TRABAJOS_MAX_CPU];
} trabajos_lote;
void trabajos_preparar(trabajos_lote *, trabajo_fn, void *, unsigned);
int trabajos_tomar(trabajos_lote *, unsigned ejecutor);
void trabajos_cancelar(trabajos_lote *);
int trabajos_terminados(const trabajos_lote *);
/* Backend kernel: no reentrante, sólo BSP. servicio nunca se ejecuta en AP. */
int trabajos_ejecutar(trabajo_fn, void *, unsigned, void (*servicio)(void *), void *);
unsigned trabajos_cpu_activas(void);
unsigned trabajos_cpu_arrancadas(void);
unsigned trabajos_cpu_detectadas(void);
/* Reloj calibrado del ejecutor: 0 = BSP, 1..3 = AP. 0 si no hay dato. */
uint64_t trabajos_ticks_por_ms(unsigned cpu);
int trabajos_en_curso(void);
void trabajos_cancelar_actual(void);
int trabajos_iniciar(void);
void trabajos_limitar_cpu(unsigned);
int trabajos_configurar_distribucion(unsigned cpus,int bsp_calcula);
int trabajos_es_coordinador(void);
int trabajos_autoprueba(void);
void trabajos_parar(void);
#endif
