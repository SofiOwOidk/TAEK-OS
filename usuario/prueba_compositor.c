/* Escena conocida para `anillo3 compositor`. El nucleo lee el framebuffer real al terminar y compara los pixeles;
 * el codigo de salida solo cuenta las llamadas que debian rechazarse. arg0 = 1: fallo con la escena en pantalla. */
#include "taek.h"

TAEK_PROGRAMA

static uint32_t *llenar(struct taek_superficie *s, uint32_t an, uint32_t al, int x, int y, uint32_t flags, uint32_t color, long *mal) {
    *s = (struct taek_superficie){ .ancho = an, .alto = al, .x = x, .y = y, .banderas = flags };
    if (taek_superficie(s) < 0) { (*mal)++; return NULL; }
    uint32_t *p = (uint32_t *)s->pixeles;
    for (uint64_t i = 0; i < (uint64_t)an * al; i++) p[i] = color;
    if (taek_presentar(s->id, 0, 0, an, al)) (*mal)++;
    return p;
}

long principal(uint64_t modo, uint64_t kernel) {
    long mal = 0;
    struct taek_pantalla pant;
    if (taek_pantalla(&pant)) return 1;
    struct taek_superficie s1, s2, s3, s4, s5;
    /* s5 nace debajo de nada, delante de s1: elevar s1 y las demas debe taparla. */
    uint32_t *p1 = llenar(&s1, 200, 100, 100, 100, 0, 0xFFFF0000u, &mal);
    llenar(&s2, 100, 50, 150, 120, TAEK_SUP_ALFA, 0x80000080u, &mal);
    llenar(&s3, 40, 40, (int)pant.ancho - 10, (int)pant.alto - 10, 0, 0xFF00FF00u, &mal);
    llenar(&s4, 10, 10, 200, 150, 0, 0xFFFFFFFFu, &mal);
    llenar(&s5, 10, 10, 250, 170, 0, 0xFF0000FFu, &mal);
    if (mal || !p1) return 2;
    if (sys1(SYS_ELEVAR, s1.id) || sys1(SYS_ELEVAR, s2.id) || sys1(SYS_ELEVAR, s3.id) || sys1(SYS_ELEVAR, s4.id)) return 4;
    if (sys1(SYS_ELEVAR, 99) != -TAEK_EBADF || sys1(SYS_ELEVAR, 0x100000000L) != -TAEK_EBADF) return 5;

    /* Mover debe reponer lo que tapaba. */
    if (taek_mover(s2.id, 170, 120)) return 3;
    /* Un pixel escrito sin presentar no debe llegar a la pantalla. */
    p1[90 * 200 + 190] = 0xFFFF00FFu;

    /* Llamadas invalidas: rechazadas sin efecto. */
    struct taek_superficie m = { .ancho = 0, .alto = 10 };
    if (taek_superficie(&m) != -TAEK_EINVAL) return 10;
    m = (struct taek_superficie){ .ancho = 65536, .alto = 65536 };
    if (taek_superficie(&m) != -TAEK_EINVAL) return 11;
    m = (struct taek_superficie){ .ancho = 16, .alto = 16, .banderas = 0x80 };
    if (taek_superficie(&m) != -TAEK_EINVAL) return 12;
    if (sys1(SYS_SUPERFICIE, (long)kernel) != -TAEK_EFAULT) return 13;
    if (sys1(SYS_EVENTO, (long)kernel) != -TAEK_EFAULT) return 14;
    if (sys1(SYS_PANTALLA, (long)kernel) != -TAEK_EFAULT) return 15;
    if (sys1(SYS_EVENTO, 0) != -TAEK_EFAULT) return 16;
    if (taek_presentar(99, 0, 0, 1, 1) != -TAEK_EBADF) return 17;
    if (sys5(SYS_PRESENTAR, 0x100000000L, 0, 0, 1, 1) != -TAEK_EBADF) return 18;
    if (taek_presentar(s1.id, 0, 0, 201, 1) != -TAEK_EINVAL) return 19;
    if (taek_presentar(s1.id, -1, 0, 1, 1) != -TAEK_EINVAL) return 20;
    if (taek_presentar(s1.id, 199, 99, 0x7FFFFFFFFFFFFFFFL, 1) != -TAEK_EINVAL) return 21;
    if (taek_mover(s1.id, 0x7FFFFFFFL, 0) != -TAEK_EINVAL) return 22;
    if (taek_mover(77, 0, 0) != -TAEK_EBADF) return 23;

    if (modo == 1) __asm__ volatile("ud2");
    return 77;
}
