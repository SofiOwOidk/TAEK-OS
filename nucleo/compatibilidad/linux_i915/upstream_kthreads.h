#ifndef TAEK_UPSTREAM_KTHREADS_H
#define TAEK_UPSTREAM_KTHREADS_H
/* Constructor/salida TaekOS con payload y worker Linux originales. No fork,
 * kthreadd, señales, park, afinidad SMP ni constructor printf Linux. */
#define TAEK_KTHREADS_VERSION 1u
struct taek_kthreads_ops {
    unsigned bytes,version;
    void *(*asignar)(unsigned long);
    int (*liberar)(void *);
    int (*crear)(void (*)(void *),void *,unsigned long *);
    int (*retirar)(unsigned long);
    int (*estado)(unsigned long);
    void (*fatal)(int);
};
struct taek_kthreads_stats {
    unsigned vivos,pendientes,detenidos,salidos,joins,retenidos;
    unsigned long creados,terminados,recolectados,fallos;
};
int taek_kthreads_iniciar(const struct taek_kthreads_ops *);
int taek_kthreads_parar(void);
/* Solo hijo ordinario IF=1; nombre literal de 1..63 bytes. Encola un pedido
 * real a raíz y espera el handshake detenido antes de publicar id. */
int taek_kthread_crear(int (*)(void *),void *,const char *,unsigned long *);
/* Solo raíz ordinaria, antes/después de cada activación real de fibra.
 * No ejecuta fibras ni espera: procesa pedidos y confirma handshakes. */
int taek_kthreads_servir(void);
int taek_kthread_despertar(unsigned long);
/* Solo hijo IF=1 distinto del target; ejecuta kthread_stop original una vez.
 * Segundo join devuelve -EALREADY antes de consumir otra completion. */
int taek_kthread_detener(unsigned long,int *);
/* Solo raíz. Rechaza hilo vivo, join/ref extra o binding pendiente.
 * Retiene cada recurso cuya liberación falla; admite retry por el mismo id. */
int taek_kthread_recolectar(unsigned long);
void taek_kthreads_estadisticas(struct taek_kthreads_stats *);
#endif
