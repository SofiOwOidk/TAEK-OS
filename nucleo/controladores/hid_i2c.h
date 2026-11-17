/* Trackpad por HID sobre I2C (especificacion de Microsoft "HID over I2C", v1.0).
 *
 * El firmware dice donde esta (dispositivos con _CID PNP0C50: bus, direccion y registro del descriptor). Se lee el
 * descriptor HID, el descriptor de reporte, se enciende y se reinicia. Hasta que alguien le pida el modo "trackpad de
 * precision", el dispositivo manda reportes de raton relativos, que es lo que se usa aqui.
 * Saber cuando hay datos: la linea de interrupcion va a un pin GPIO; se lee su estado (solo lectura). Sin pin, se
 * pregunta cada 8 ms. */
#ifndef TAEK_HID_I2C_H
#define TAEK_HID_I2C_H
#include <stdint.h>

#define TACTIL_DISPOSITIVO 0x7AC7u   /* Identificador del trackpad ante puntero_publicar */

/* 0 si encontro, encendio y entendio el trackpad. verbose: imprimir cada paso. */
int tactil_iniciar(int verbose);
int tactil_listo(void);
/* Lee un reporte si hay. 1 si movio el puntero, 0 nada, negativo error. Barato si el pin dice que no hay datos. */
int tactil_sondear(void);
void tactil_comando(const char *arg);
void i2c_comando(const char *arg);
#endif
