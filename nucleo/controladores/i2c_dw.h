/* Controlador I2C del chipset Intel (LPSS, nucleo DesignWare), como amo del bus y por sondeo.
 *
 * En Sunrise Point (i7 de 8.a gen.) son las funciones PCI 8086:9D60..9D65, una por bus: I2C0 = 9D60, I2C1 = 9D61...
 * El trackpad de la laptop cuelga de I2C1. Nada aqui usa interrupciones ni DMA: cada transferencia empuja ordenes a
 * la cola del controlador y lee lo que vuelve, con limite de tiempo. */
#ifndef TAEK_I2C_DW_H
#define TAEK_I2C_DW_H
#include <stdint.h>

/* Busca el bus 'indice' (0..5), lo despierta (D0, fuera de reset) y lo deja como amo a 'khz' (100 o 400).
 * 0 bien; negativo si no esta, no responde o no es un DesignWare. */
int i2c_dw_iniciar(unsigned indice, unsigned khz);
int i2c_dw_listo(void);

/* Escribe 'nw' bytes y luego lee 'nr' (con condicion de reinicio entre medio) al dispositivo de 7 bits 'dir'.
 * 0 bien; -2 sin respuesta a tiempo; -3 abortada (p. ej. nadie contesta en esa direccion: ver i2c_dw_aborto). */
int i2c_dw_transferir(uint8_t dir, const uint8_t *w, unsigned nw, uint8_t *r, unsigned nr, unsigned limite_ms);
uint32_t i2c_dw_aborto(void);

/* Solo lectura: lista los buses, su estado de energia y su identificacion. No despierta ni cambia nada. */
void i2c_dw_sondear(void);
#endif
