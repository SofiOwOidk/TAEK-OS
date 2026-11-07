#ifndef TAEK_UPSTREAM_CURRENT_H
#define TAEK_UPSTREAM_CURRENT_H
/* Frontera escalar para probar consumidores originales sobre fixtures de
 * task_struct Linux. No crea tareas Linux ni sustituye sched/DRM/stack. */
int taek_current_fixture_iniciar(void);
int taek_current_fixture_seleccionar(unsigned indice);
int taek_current_fixture_verificar(unsigned indice,int pid,unsigned long estado);
int taek_current_fixture_estado(unsigned long estado);
int taek_current_fixture_trylock(void);
int taek_current_fixture_unlock(void);
int taek_current_fixture_locked(void);
unsigned long taek_current_fixture_owner(void);
unsigned long taek_current_fixture_identidad(unsigned indice);
unsigned long taek_current_fixture_task_bytes(void);
unsigned long taek_current_fixture_task_align(void);
#endif
