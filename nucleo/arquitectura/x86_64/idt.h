#ifndef ARQUITECTURA_X86_64_IDT_H
#define ARQUITECTURA_X86_64_IDT_H

#include <stdint.h>

struct entrada_idt {
    uint16_t manejador_bajo;
    uint16_t selector_cs;
    uint8_t  ist;
    uint8_t  atributos;
    uint16_t manejador_medio;
    uint32_t manejador_alto;
    uint32_t reservado;
} __attribute__((packed));

struct puntero_idt {
    uint16_t limite;
    uint64_t base;
} __attribute__((packed));

struct marco_interrupcion {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t num_interrupcion;
    uint64_t codigo_error;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed));

void idt_iniciar(void);
void manejador_excepciones(struct marco_interrupcion *marco);

#endif // ARQUITECTURA_X86_64_IDT_H
