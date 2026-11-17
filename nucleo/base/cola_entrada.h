#ifndef TAEK_BASE_COLA_ENTRADA_H
#define TAEK_BASE_COLA_ENTRADA_H

/* H205, Fase 0.5 del plan del gestor de hilos: cola de teclas de varios productores y un consumidor (MPSC).
 *
 * La entrada tiene cuatro fuentes (teclado USB por xHCI, teclado interno PS/2, serie y LAN). Hoy cada una tiene su
 * bufer y CPU 0 las sondea una por una. Cuando cada fuente tenga su hilo (fases B y C), todas dejaran aqui sus
 * caracteres y CPU 0 solo sacara de esta cola. El ABI de SYS_EVENTO no cambia: sigue entregando un caracter.
 *
 * Un cerrojo corto protege los indices; encolar y tomar no esperan nada. Si la cola esta llena, la tecla se pierde y
 * se cuenta: nunca se bloquea a un productor. */

#include <stdint.h>

enum { ENTRADA_USB = 1, ENTRADA_PS2 = 2, ENTRADA_SERIE = 3, ENTRADA_LAN = 4, ENTRADA_FUENTES = 5 };
#define COLA_ENTRADA_TAM 256u

/* 0 si se encolo, -1 si la cola estaba llena (la tecla se cuenta como perdida) */
int cola_entrada_encolar(uint8_t fuente, char c);
/* El caracter mas antiguo, o 0 si no hay; fuente puede ser NULL */
char cola_entrada_tomar(uint8_t *fuente);
unsigned cola_entrada_pendientes(void);
uint64_t cola_entrada_perdidas(void);
uint64_t cola_entrada_encoladas(uint8_t fuente);

#endif
