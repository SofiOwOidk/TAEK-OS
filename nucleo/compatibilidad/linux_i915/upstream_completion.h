#ifndef TAEK_UPSTREAM_COMPLETION_H
#define TAEK_UPSTREAM_COMPLETION_H
/* Tres objetos de ensayo con layout Linux original; no servicio de workers. */
#define TAEK_COMPLETION_SLOTS 3u
int taek_completion_iniciar(void);
int taek_completion_parar(void);
int taek_completion_reset(unsigned);
int taek_completion_complete(unsigned,int);
long taek_completion_wait(unsigned,long); /* -1 infinito; >=0 jiffies HZ250 */
int taek_completion_try(unsigned);
int taek_completion_done(unsigned);
int taek_completion_waiters(unsigned);
unsigned long taek_completion_count(unsigned);
#endif
