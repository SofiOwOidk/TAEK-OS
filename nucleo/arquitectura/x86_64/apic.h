#ifndef ARQUITECTURA_X86_64_APIC_H
#define ARQUITECTURA_X86_64_APIC_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

// ============================================================================
// TAEK OS - CONTROLADOR LOCAL APIC & ENRUTADOR DE INTERRUPCIONES (H16)
// Soporte xAPIC (MMIO) y x2APIC (MSRs de alta velocidad)
// ============================================================================

#define APIC_MMIO_VIRTUAL_BASE   0xFFFFFE0001000000ULL
#define APIC_VECTOR_TEMPORIZADOR 32
#define APIC_VECTOR_TECLADO      33
#define APIC_VECTOR_MSI_GPU      40
#define APIC_VECTOR_PRUEBA_IPI   80
#define APIC_VECTOR_ESPURIO      255

struct estado_apic {
    uint8_t  activo;
    uint8_t  es_x2apic;
    uint32_t id;
    uint32_t version;
    uint64_t dir_fisica_base;
    uint64_t dir_virtual_base;
    uint32_t interrupciones_recibidas;
    uint32_t ipi_recibidos;
};

void pic_desactivar(void);

int  apic_iniciar(void);
const struct estado_apic *apic_obtener_estado(void);

void apic_enviar_eoi(void);
void apic_enviar_self_ipi(uint8_t vector);

uint32_t apic_leer(uint32_t reg);
void     apic_escribir(uint32_t reg, uint32_t val);

void apic_despachar_irq(struct marco_interrupcion *marco);
int  apic_ejecutar_autodiagnostico(void);

#endif // ARQUITECTURA_X86_64_APIC_H
