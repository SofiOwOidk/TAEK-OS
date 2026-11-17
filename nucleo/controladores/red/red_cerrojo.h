#ifndef CONTROLADORES_RED_CERROJO_H
#define CONTROLADORES_RED_CERROJO_H

/* H211, Fase C del gestor de hilos: un cerrojo para toda la red (pila TCP/IP, stream de la consola, controladores
 * e1000 y WiFi). Lo toma cada funcion publica de pila.c, stream.c, remoto.c, mensajes.c, archivos.c, e1000.c,
 * wifi_rtw88.c y wifi_wpa.c al entrar (RED_EXCLUSIVA) y lo suelta al salir; es recursivo por CPU.
 *
 * TLS, HTTP y el navegador estan por encima y no lo toman: entre llamada y llamada a la pila el cerrojo queda libre, y
 * el hilo de red (CPU 3) atiende ARP, ACK, el stream de la consola y las teclas del PC mientras CPU 0 calcula un apreton
 * de manos TLS o abre un video.
 *
 * Orden: red -> consola -> serial. La consola nunca llama a la red con su cerrojo tomado; red_remoto_capturar, que
 * corre dentro del cerrojo de la consola, no toma este. */

#include <stdint.h>
#include "../../base/cerrojo.h"

extern cerrojo_t g_cerrojo_red;
static inline void red_soltar_exclusiva(int *p) { (void)p; cerrojo_soltar(&g_cerrojo_red); }
#define RED_EXCLUSIVA() \
    int red_exclusiva_ __attribute__((cleanup(red_soltar_exclusiva), unused)) = (cerrojo_tomar(&g_cerrojo_red), 1)

/* 1 si el hilo de red atiende la pila y quien llama no tiene el cerrojo: el sondeo de CPU 0 vuelve sin hacer nada */
int red_cede_el_sondeo(void);
int red_hilo(int encender);          /* 1 enciende, 0 apaga; devuelve el modo o -1 */
int red_hilo_activo(void);
uint64_t red_hilo_pasadas(void);
/* Para el informe de un colapso: se queda con el cerrojo aunque otra CPU lo tenga (espera hasta 200 ms) */
void red_cerrojo_forzar(void);

#endif
