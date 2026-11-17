/* Lectura minima de descriptores de reporte HID (HID 1.11, seccion 6.2.2): encuentra el raton.
 *
 * Un trackpad por I2C o un mouse USB en modo reporte no mandan los tres bytes del modo boot: mandan lo que su
 * descriptor dice. Aqui se busca la coleccion de aplicacion Raton (pagina 1, uso 2) y dentro de ella los botones,
 * X, Y y la rueda, con su posicion en bits. Sin dependencias del kernel: se prueba en el PC. */
#ifndef TAEK_HID_DESCRIPTOR_H
#define TAEK_HID_DESCRIPTOR_H
#include <stdint.h>

struct hid_campo {
    uint16_t bit;        /* Posicion del primer bit dentro del reporte, sin contar el byte de ID */
    uint8_t tam;         /* Bits */
    uint8_t con_signo;   /* Minimo logico negativo */
    uint8_t presente;
};

struct hid_raton {
    uint8_t id;              /* ID del reporte; 0 si el descriptor no usa IDs */
    uint8_t n_botones;
    uint8_t relativo;        /* X e Y relativos (lo normal en un raton) */
    uint16_t bits;           /* Largo del reporte de entrada de ese ID, en bits */
    struct hid_campo botones, x, y, rueda;
};

/* 0 si encontro un raton con X e Y; negativo si no (-1 descriptor roto, -2 sin raton). */
int hid_buscar_raton(const uint8_t *d, unsigned n, struct hid_raton *r);

/* rep: reporte tal como llega (con su byte de ID si lo hay). 0 bien; -1 corto; -2 es de otro ID. */
int hid_decodificar_raton(const struct hid_raton *r, const uint8_t *rep, unsigned n,
                          int32_t *dx, int32_t *dy, uint32_t *botones, int32_t *rueda);
#endif
