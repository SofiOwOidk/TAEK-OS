#ifndef CONTROLADORES_ENTRADA_H
#define CONTROLADORES_ENTRADA_H

/* H210, Fase B2 del gestor de hilos: el teclado interno (PS/2, 8042) y el trackpad (HID por I2C) los sondea un hilo de
 * CPU 2 cada 4 ms, y CPU 0 deja de hacerlo. Un cerrojo protege lo que comparten: los puertos del 8042, el controlador
 * I2C, la cola de teclas del PS/2 y el estado del puntero. Las funciones publicas de ps2.c, hid_i2c.c y puntero.c lo
 * toman al entrar (ENTRADA_EXCLUSIVA); es recursivo por CPU. Ninguna interrupcion toca estas cosas: no apaga IRQ. */

#include <stdint.h>
#include "../base/cerrojo.h"

extern cerrojo_t g_cerrojo_entrada;
static inline void entrada_soltar_exclusiva(int *p) { (void)p; cerrojo_soltar(&g_cerrojo_entrada); }
#define ENTRADA_EXCLUSIVA() \
    int entrada_exclusiva_ __attribute__((cleanup(entrada_soltar_exclusiva), unused)) = (cerrojo_tomar(&g_cerrojo_entrada), 1)

/* 1 si el hilo de entrada esta encendido y quien llama no es el: el sondeo de CPU 0 vuelve sin hacer nada */
int entrada_cede_el_sondeo(void);
/* 1 enciende, 0 apaga; devuelve el modo, o -1 si no se pudo crear el hilo */
int entrada_hilo(int encender);
uint64_t entrada_hilo_pasadas(void);

#endif
