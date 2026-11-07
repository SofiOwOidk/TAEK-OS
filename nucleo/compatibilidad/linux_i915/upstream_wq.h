#ifndef TAEK_UPSTREAM_WQ_H
#define TAEK_UPSTREAM_WQ_H
/* Frontera TaekOS bajo la workqueue original de Linux. Tipos escalares para el
 * consumidor; las estructuras Linux solo existen en las unidades que las compilan.
 * Perfil: UP, PREEMPT_NONE, BSP ordinario y cooperativo. No IRQ, no AP, no GPU. */
#define TAEK_WQ_VERSION 1u
struct taek_wq_ops {
    unsigned bytes,version;
    void *(*asignar)(unsigned long);
    int (*liberar)(void *);
    unsigned long (*clock_ms)(void);
    void (*mensaje)(const char *);
    void (*fatal)(int);
    /* Distinto de cero: kmalloc devuelve la direccion del mapa lineal de Linux para que __pa()
     * y virt_to_page() sean correctos. Cero: devuelve el puntero nativo del heap. */
    unsigned long delta_lineal;
};
struct taek_wq_stats {
    unsigned long heap_bloques,heap_bytes,heap_pico_bytes,heap_fallos;
    unsigned long timers_pendientes,timers_disparados;
    unsigned long rcu_pendientes,rcu_ejecutados;
    unsigned long caches,percpu_bloques;
};
/* Solo raiz ordinaria, antes de usar workqueue. Inicializa el arbol radix original. */
int taek_wq_frontera_iniciar(const struct taek_wq_ops *);
/* Solo raiz, entre activaciones de fibra: avanza jiffies, despacha los timer_list
 * vencidos y ejecuta RCU. Todas las fibras estan en un punto de schedule, y en este
 * perfil (PREEMPT_NONE, UP) un lector RCU no puede dormir: eso es un periodo de gracia. */
int taek_wq_bombear(void);
void taek_wq_estadisticas(struct taek_wq_stats *);
/* Control negativo: hace fallar la asignacion numero n contada desde ahora; 0 lo desarma. */
void taek_wq_fallo_en(unsigned long);
unsigned long taek_wq_fallos_inyectados(void);
void taek_wq_fallo_limpiar(void);
/* Actualiza jiffies desde el reloj de TaekOS. */
void taek_wq_bombear_jiffies(void);
/* Bytes utiles de un bloque de kmalloc (solo con su puntero de usuario). */
unsigned long taek_wq_bloque_bytes(const void *);
/* Solo fibra supervisora. Primera llamada: workqueue_init_early+workqueue_init. */
int taek_wq_fixture_probar(void);
/* Resultado de la ultima prueba: mascara de fallos y telemetria. */
struct taek_wq_resultado {
    unsigned long mascara,checks,rondas,workers_vivos_pico,ejecuciones;
};
void taek_wq_fixture_resultado(struct taek_wq_resultado *);
#endif
