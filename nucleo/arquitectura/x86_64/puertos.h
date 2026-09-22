#ifndef ARQUITECTURA_X86_64_PUERTOS_H
#define ARQUITECTURA_X86_64_PUERTOS_H

#include <stdint.h>

static inline void escribir_puerto_b(uint16_t puerto, uint8_t valor) {
    __asm__ volatile ("outb %0, %1" : : "a"(valor), "Nd"(puerto));
}

static inline uint8_t leer_puerto_b(uint16_t puerto) {
    uint8_t retorno;
    __asm__ volatile ("inb %1, %0" : "=a"(retorno) : "Nd"(puerto));
    return retorno;
}

static inline void escribir_puerto_w(uint16_t puerto, uint16_t valor) {
    __asm__ volatile ("outw %0, %1" : : "a"(valor), "Nd"(puerto));
}

static inline uint16_t leer_puerto_w(uint16_t puerto) {
    uint16_t retorno;
    __asm__ volatile ("inw %1, %0" : "=a"(retorno) : "Nd"(puerto));
    return retorno;
}

static inline void escribir_puerto_l(uint16_t puerto, uint32_t valor) {
    __asm__ volatile ("outl %0, %1" : : "a"(valor), "Nd"(puerto));
}

static inline uint32_t leer_puerto_l(uint16_t puerto) {
    uint32_t retorno;
    __asm__ volatile ("inl %1, %0" : "=a"(retorno) : "Nd"(puerto));
    return retorno;
}

static inline void esperar_io(void) {
    escribir_puerto_b(0x80, 0);
}

#endif // ARQUITECTURA_X86_64_PUERTOS_H
