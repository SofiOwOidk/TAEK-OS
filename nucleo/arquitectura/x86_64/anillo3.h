#ifndef ARQUITECTURA_X86_64_ANILLO3_H
#define ARQUITECTURA_X86_64_ANILLO3_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

/* Un programa a la vez, en la CPU coordinadora. La mitad baja del espacio de direcciones
 * compartido usa la entrada 1 del PML4; el resto de la memoria no lleva el bit de usuario. */
#define ANILLO3_VECTOR_LLAMADA 0x80u
#define ANILLO3_BASE    UINT64_C(0x0000008000000000)
#define ANILLO3_DATOS   (ANILLO3_BASE + UINT64_C(0x100000))
#define ANILLO3_PILA_FIN (ANILLO3_BASE + UINT64_C(0x400000))
#define ANILLO3_PILA_PAGINAS 16u
#define ANILLO3_DATOS_PAGINAS 16u

enum anillo3_fin { ANILLO3_SALIO = 1, ANILLO3_FALLO = 2, ANILLO3_RELOJ = 3 };

struct anillo3_resultado {
    int fin;
    int64_t codigo;
    uint64_t vector, error, rip, rsp, cr2;
    uint64_t ms, ticks, llamadas;
    uint64_t fs_alterado;      /* Entradas al nucleo con el FS_BASE cambiado por el programa */
    uint64_t ac_en_nucleo;     /* Entradas donde RFLAGS.AC siguio activo tras limpiarlo */
    uint64_t composiciones, ciclos_componer, ciclos_presentar;
    uint64_t monton_pico;      /* H144: lo mas que llego a pedir con SYS_MEMORIA, en bytes */
};

/* limite_ms: sin llamadas al sistema. limite_total_ms: de toda la ejecucion; 0 sin limite. */
int anillo3_ejecutar(const void *imagen, size_t bytes, uint64_t arg0, uint64_t arg1,
                     uint32_t limite_ms, uint32_t limite_total_ms, struct anillo3_resultado *res);
void anillo3_comando(const char *arg);
/* Corre el menu principal. posicion: lo que el menu devolvio la ultima vez, para volver al mismo sitio. */
int anillo3_menu(uint32_t posicion, int modo_prueba, struct anillo3_resultado *r);
/* H149: busca /menu.elf en los discos y lo deja listo para anillo3_menu; si no hay (o solo_iso), se usa el de la ISO. */
void anillo3_menu_cargar(int solo_iso);
/* H143: lee un ELF del disco montado y lo ejecuta en Anillo 3, sin reiniciar. */
void anillo3_ejecutar_archivo(const char *ruta);
/* H146: corre lo que el menu pidio con SYS_LANZAR. -1 si no pidio nada. */
int anillo3_lanzar_pendiente(void);
int anillo3_orden_pendiente(char *b, unsigned cap);   /* H151: orden de pantalla completa pedida por el menu */
/* Permite o cierra el acceso del nucleo a paginas de usuario (SMAP). */
void anillo3_acceso_usuario(int abrir);

/* Enganches de despachador_interrupciones, solo con marco->cs en Ring 3. */
void anillo3_al_entrar(void);
void anillo3_llamada(struct marco_interrupcion *marco);
void anillo3_fallo(struct marco_interrupcion *marco) __attribute__((noreturn));
void anillo3_tras_irq(void);
/* H213, Fase D: justo antes de volver a Anillo 3 (llamada o interrupcion), con las interrupciones apagadas al salir */
void anillo3_al_salir(void);
unsigned anillo3_hilos_vivos(void);   /* H213: hilos de programa en marcha (deberia ser 0 sin programa) */

#endif
