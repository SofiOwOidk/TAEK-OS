#ifndef TAEK_UPSTREAM_TASKS_H
#define TAEK_UPSTREAM_TASKS_H
/* Frontera TaekOS de tareas kernel, no fork/kthread Linux. Tipos escalares para
 * el consumidor; los objetos y campos privados se compilan con headers Linux. */
#define TAEK_TASKS_VERSION 1u
#define TAEK_TASKS_SLOTS 24u
#define TAEK_TASKS_WAIT_VERSION 1u
#define TAEK_TASKS_MAX_PASOS 256u
struct taek_tasks_ops {
    unsigned bytes,version;
    void *(*asignar)(unsigned long);
    int (*liberar)(void *);
    int (*bloquear)(void);
    int (*yield)(void);
    int (*despertar)(unsigned long);
    unsigned long (*actual)(void);
    int (*estado)(unsigned long);
    int (*contexto)(void); /* 0 rechazado, 1 ordinario, 2 observer */
    void (*mensaje)(const char *);
    void (*warning)(const char *,unsigned,unsigned);
    /* Fallo de contrato interno: debe detener la ejecucion, no retornar. */
    void (*fatal)(int);
};
struct taek_tasks_stats {
    unsigned vivos,vinculados,esperando,referencias,fallos,avisos;
    unsigned long creadas,destruidas,bloqueos,yields,wakes,prewakes;
};
/* Extension separada: no cambia los 96 bytes ni version de taek_tasks_ops.
 * clock_ms es monotono/puro y no invoca APIs Task/fibra. ejecutar es el motor
 * real fibra_ejecutar: 0/1 activaciones o error; no reentrante. Los callbacks
 * y su backing viven hasta reset exitoso/parar, que requieren cero timers. */
struct taek_tasks_wait_ops {
    unsigned bytes,version;
    unsigned long (*clock_ms)(void);
    int (*ejecutar)(int);
};
struct taek_tasks_timer_stats {
    unsigned configurado,activos,bombeando;
    unsigned long registrados,vencidos,cancelados,bombeos,reloj_ms;
};
/* Solo raiz ordinaria sin timers ni pump. NULL retira la configuracion. */
int taek_tasks_configurar_esperas(const struct taek_tasks_wait_ops *);
/* Root pump bounded: timers antes/despues de cada activacion real. No espera
 * a que avance el reloj si no hay ejecutables. pasos=0 solo bombea una vez.
 * El loop principal debe volver a bombear: no hay timer/IRQ autonomo. */
int taek_tasks_ejecutar(int pasos);
int taek_tasks_contexto(void);
void taek_tasks_timer_stats(struct taek_tasks_timer_stats *);
int taek_tasks_iniciar(const struct taek_tasks_ops *);
int taek_tasks_parar(void);
int taek_tasks_crear(unsigned long *);
int taek_tasks_vincular(unsigned long,unsigned long);
int taek_tasks_desvincular(unsigned long);
int taek_tasks_destruir(unsigned long);
/* Solo observer: selector breve, sin asignar/liberar ni ejecutar el backend. */
int taek_tasks_seleccionar(unsigned long);
unsigned long taek_tasks_actual(void);
int taek_tasks_pid(unsigned long);
int taek_tasks_refs(unsigned long);
int taek_tasks_estado(unsigned long);
/* Task prestada solo BSP ordinario; NULL en observer/clock/AP/IRQ, inexistente
 * o retirando. Valida hasta destruir: quien la retenga debe tomar una ref real.
 * No autoriza mutar estados ajenos al perfil RUNNING/INTERRUPTIBLE/UNINT/IDLE.
 * Wake acepta flags0 y mascara no vacia solo con bits NORMAL|NOLOAD. */
void *taek_tasks_objeto(unsigned long);
int taek_tasks_mutex_lock(void);
int taek_tasks_mutex_unlock(void);
int taek_tasks_mutex_locked(void);
int taek_tasks_yield(void);
void taek_tasks_stats(struct taek_tasks_stats *);
/* Prueba acotada de wake_q original: insercion, duplicado y drain en tarea. */
int taek_tasks_wakeq(unsigned long,int);
int taek_tasks_publicar_espera(void);
/* Solo Task/fibra actual ordinaria. Estados Linux 1,2,0x402; rechaza RUNNING
 * y estados especiales antes de consumir token/publicar. No entrega senales. */
int taek_tasks_publicar_espera_estado(unsigned state);
int taek_tasks_schedule(void);
#endif
