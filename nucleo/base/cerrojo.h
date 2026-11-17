#ifndef TAEK_BASE_CERROJO_H
#define TAEK_BASE_CERROJO_H

/* H205, Fase 0.5 del plan del gestor de hilos (planes/PLAN_GESTOR_HILOS.md): el cerrojo del nucleo.
 *
 * Hasta H204 lo compartido se protegia negandose a trabajar fuera de CPU 0 (trabajos_es_coordinador) o durante un
 * lote (trabajos_en_curso). Para que otras CPU corran codigo del nucleo hace falta un cerrojo de verdad.
 *
 * Reglas (regla 6 del plan):
 *  - corto: solo para tocar punteros de colas, listas o mapas. Con uno tomado no se duerme ni se espera hardware;
 *  - es recursivo por CPU: quien ya lo tiene puede volver a tomarlo (el monton se llama a si mismo al crecer, y la
 *    consola puede imprimir dentro de una impresion). Solo cuenta el nivel exterior;
 *  - la variante _irq apaga las interrupciones de esa CPU mientras dura; la simple no las toca, y una interrupcion
 *    de la misma CPU puede volver a entrar (asi imprime hoy el manejador de la GPU);
 *  - quien espera un cerrojo atiende la invalidacion de TLB pendiente: no puede quedarse sordo a ella.
 *
 * Orden de toma, de fuera hacia dentro: paginas -> dma -> memoria -> consola. La consola es hoja. */

#include <stdint.h>

#define CERROJO_CPUS 4

/* Indice de la CPU que ejecuta: 0 el BSP, 1..3 las auxiliares. Barato (rdtscp) si el procesador lo tiene. */
unsigned cpu_actual(void);
/* Cada CPU se identifica una vez al arrancar, antes de tomar ningun cerrojo. */
void cpu_identidad_iniciar(unsigned indice);

typedef struct cerrojo {
    volatile uint32_t dueno;       /* 0 libre; indice de CPU + 1 */
    uint32_t profundidad;
    uint64_t t_tomado;             /* TSC al tomarlo (nivel exterior) */
    uint64_t max_ciclos;           /* Lo mas que estuvo tomado, en ciclos de TSC */
    uint64_t desde, max_desde;     /* H206: quien lo tomo (direccion de retorno) y quien lo tuvo mas tiempo */
    uint64_t tomas, disputas;      /* Veces tomado; veces que hubo que esperar a otra CPU */
    const char *nombre;
    struct cerrojo *siguiente;     /* Registro para la orden 'cerrojos' */
    uint32_t registrado;
} cerrojo_t;
#define CERROJO(texto) { .nombre = texto }

void cerrojo_tomar(cerrojo_t *c);
void cerrojo_soltar(cerrojo_t *c);
uint64_t cerrojo_tomar_irq(cerrojo_t *c);              /* Devuelve los RFLAGS de antes */
int cerrojo_intentar(cerrojo_t *c);                    /* H208: 1 si lo tomo; 0 si lo tiene otra CPU (no espera) */
void cerrojo_forzar(cerrojo_t *c, uint32_t ms);        /* H211: solo para el colapso; espera ms y se lo queda igual */
static inline int cerrojo_es_mio(const cerrojo_t *c) {  /* H211: 1 si esta CPU lo tiene tomado */
    return __atomic_load_n(&c->dueno, __ATOMIC_ACQUIRE) == cpu_actual() + 1;
}
void cerrojo_soltar_irq(cerrojo_t *c, uint64_t rflags);

/* Cuantos cerrojos tiene tomados una CPU. El dueno es la CPU, no el hilo: el planificador no cambia de hilo en una
 * CPU que tiene alguno en la mano. */
unsigned cerrojo_en_mano(unsigned cpu);

/* Para 'cerrojos': recorre los cerrojos que se usaron alguna vez. */
const cerrojo_t *cerrojo_primero(void);
/* Pone a cero los maximos y contadores (no toca quien los tiene). */
void cerrojo_cero(void);

#endif
