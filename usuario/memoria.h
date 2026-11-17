#ifndef TAEK_MEMORIA_H
#define TAEK_MEMORIA_H
/* H144: malloc, free, realloc y calloc para programas de Anillo 3, sobre SYS_MEMORIA.
 *
 * Como funciona (el asignador clasico de Kernighan y Ritchie, con dos agregados):
 *   - Cada bloque lleva delante una cabecera de 16 bytes con su tamano. Todo va alineado a 16 (lo pide SSE).
 *   - Los bloques libres forman una lista ordenada por direccion. malloc toma el primero que alcance y, si sobra,
 *     lo parte; free lo devuelve a su sitio y lo une con el vecino de antes y el de despues si se tocan.
 *   - Si no hay hueco, se le pide al nucleo al menos 1 MiB mas. El monton es contiguo, asi que lo nuevo queda pegado
 *     a lo anterior y tambien se puede unir.
 *   - Si al liberar queda un bloque libre grande justo al final del monton, se devuelve al nucleo (menos 1 MiB que se
 *     guarda para la proxima vez): una aplicacion que abre y cierra algo pesado no se queda con esa RAM.
 * Un solo hilo: no hay cerrojos. */
#include "taek.h"

struct taek_bloque { size_t tam; struct taek_bloque *sig; };   /* tam: bytes con la cabecera, multiplo de 16 */

#define TAEK_CABECERA      sizeof(struct taek_bloque)
#define TAEK_PEDIR_MIN     (1024u * 1024u)
#define TAEK_DEVOLVER_DESDE (4u * 1024u * 1024u)
#define TAEK_GUARDAR       (1024u * 1024u)

static struct taek_bloque *taek_libres;
static uint64_t taek_monton_fin;     /* Final actual segun el nucleo; 0 hasta la primera peticion */

/* Mete un bloque en la lista, en su sitio, y lo une con sus vecinos. */
static void taek_poner(struct taek_bloque *b) {
    struct taek_bloque *ant = NULL, *p = taek_libres;
    while (p && p < b) { ant = p; p = p->sig; }
    if (p && (char *)b + b->tam == (char *)p) { b->tam += p->tam; b->sig = p->sig; }
    else b->sig = p;
    if (ant && (char *)ant + ant->tam == (char *)b) { ant->tam += b->tam; ant->sig = b->sig; }
    else if (ant) ant->sig = b;
    else taek_libres = b;
}

static int taek_crecer(size_t tam) {
    size_t n = tam < TAEK_PEDIR_MIN ? TAEK_PEDIR_MIN : (tam + 4095) & ~(size_t)4095;
    long p = taek_memoria((long)n);
    if (p < 0) return -1;
    taek_monton_fin = (uint64_t)p + n;
    struct taek_bloque *b = (struct taek_bloque *)(uintptr_t)p;
    b->tam = n;
    taek_poner(b);
    return 0;
}

/* Si el ultimo libre llega hasta el final del monton y es grande, la mayor parte vuelve al nucleo. */
static void taek_recortar(void) {
    struct taek_bloque *p = taek_libres;
    while (p && p->sig) p = p->sig;
    if (!p || (uint64_t)(uintptr_t)((char *)p + p->tam) != taek_monton_fin || p->tam < TAEK_DEVOLVER_DESDE) return;
    size_t devolver = (p->tam - TAEK_GUARDAR) & ~(size_t)4095;
    if (!devolver || taek_memoria(-(long)devolver) < 0) return;
    taek_monton_fin -= devolver;
    p->tam -= devolver;
}

__attribute__((unused)) static void *malloc(size_t n) {
    if (!n || n > ((size_t)1 << 40)) return NULL;
    size_t t = (n + TAEK_CABECERA + 15) & ~(size_t)15;
    for (int intento = 0; intento < 2; intento++) {
        struct taek_bloque *ant = NULL, *p = taek_libres;
        for (; p; ant = p, p = p->sig) {
            if (p->tam < t) continue;
            if (p->tam - t >= 2 * TAEK_CABECERA) {
                /* Se entrega el final del bloque: el principio sigue en la lista sin moverlo */
                p->tam -= t;
                struct taek_bloque *b = (struct taek_bloque *)((char *)p + p->tam);
                b->tam = t;
                return b + 1;
            }
            if (ant) ant->sig = p->sig; else taek_libres = p->sig;
            return p + 1;
        }
        if (intento || taek_crecer(t)) return NULL;
    }
    return NULL;
}

__attribute__((unused)) static void free(void *q) {
    if (!q) return;
    taek_poner((struct taek_bloque *)q - 1);
    taek_recortar();
}

__attribute__((unused)) static void *calloc(size_t n, size_t tam) {
    if (tam && n > ((size_t)1 << 40) / tam) return NULL;
    void *p = malloc(n * tam);
    if (p) memset(p, 0, n * tam);
    return p;
}

__attribute__((unused)) static void *realloc(void *q, size_t n) {
    if (!q) return malloc(n);
    if (!n) { free(q); return NULL; }
    struct taek_bloque *b = (struct taek_bloque *)q - 1;
    if (b->tam - TAEK_CABECERA >= n) return q;
    void *r = malloc(n);
    if (!r) return NULL;
    memcpy(r, q, b->tam - TAEK_CABECERA);
    free(q);
    return r;
}

/* Bytes que el programa tiene pedidos al nucleo ahora mismo */
__attribute__((unused)) static uint64_t taek_monton_usado(void) {
    long fin = taek_memoria(0);
    return fin < 0 ? 0 : (uint64_t)fin - TAEK_MONTON;
}

#endif
