#include "idt.h"
#include "../../base/huevo.h"
#include "serial.h"

extern void *tabla_trampas[256];
extern void apic_despachar_irq(struct marco_interrupcion *marco);

static const char *g_nombres_excepciones[32] = {
    "Division por Cero (#DE)",
    "Depuracion (#DB)",
    "Interrupcion No Enmascarable (#NMI)",
    "Punto de Interrupcion (#BP)",
    "Desbordamiento (#OF)",
    "Rango de Limites Excedido (#BR)",
    "Instruccion / Opcode Invalido (#UD)",
    "Dispositivo No Disponible (#NM)",
    "Doble Fallo (#DF)",
    "Segmento de Coprocesador Sobrepasado",
    "TSS Invalido (#TS)",
    "Segmento No Presente (#NP)",
    "Fallo de Segmento de Pila (#SS)",
    "Fallo de Proteccion General (#GP)",
    "Fallo de Pagina (#PF)",
    "Reservado",
    "Excepcion de Coma Flotante x87 (#MF)",
    "Comprobacion de Alineacion (#AC)",
    "Comprobacion de Maquina (#MC)",
    "Excepcion de Punto Flotante SIMD (#XM)",
    "Excepcion de Virtualizacion (#VE)",
    "Excepcion de Proteccion de Control (#CP)",
    "Reservado", "Reservado", "Reservado", "Reservado", "Reservado", "Reservado",
    "Excepcion de Inyeccion de Hipervisor (#HV)",
    "Excepcion de Comunicacion VMM (#VC)",
    "Excepcion de Seguridad (#SX)",
    "Reservado"
};

static struct entrada_idt g_idt[256];
static struct puntero_idt g_puntero_idt;
static manejador_irq_fn   g_manejadores_irq[256];

static void idt_configurar_puerta(int num, uint64_t dir_manejador) {
    g_idt[num].manejador_bajo  = (uint16_t)(dir_manejador & 0xFFFF);
    g_idt[num].selector_cs     = 0x08;
    g_idt[num].ist             = 0;
    g_idt[num].atributos       = 0x8E; // Presente, Anillo 0, Interrupt Gate 64-bit
    g_idt[num].manejador_medio = (uint16_t)((dir_manejador >> 16) & 0xFFFF);
    g_idt[num].manejador_alto  = (uint32_t)((dir_manejador >> 32) & 0xFFFFFFFF);
    g_idt[num].reservado       = 0;
}

void idt_iniciar(void) {
    g_puntero_idt.limite = sizeof(g_idt) - 1;
    g_puntero_idt.base   = (uint64_t)&g_idt;

    for (int i = 0; i < 256; i++) {
        g_manejadores_irq[i] = 0;
        idt_configurar_puerta(i, (uint64_t)tabla_trampas[i]);
    }

    __asm__ volatile ("lidt %0" : : "m"(g_puntero_idt));
}

void idt_registrar_manejador(uint8_t vector, manejador_irq_fn manejador) {
    g_manejadores_irq[vector] = manejador;
}

void despachador_interrupciones(struct marco_interrupcion *marco) {
    if (marco->num_interrupcion < 32) {
        // Excepción de CPU: invocar autopsia forense de El Huevo
        const char *nombre = g_nombres_excepciones[marco->num_interrupcion];
        huevo_quebrar(nombre, marco->rip, marco->rsp, marco->codigo_error);
        return;
    }

    // Interrupción de hardware o APIC (vector >= 32)
    if (g_manejadores_irq[marco->num_interrupcion]) {
        g_manejadores_irq[marco->num_interrupcion](marco);
    }

    // Notificar al subsistema APIC para estadísticas y EOI
    apic_despachar_irq(marco);
}
