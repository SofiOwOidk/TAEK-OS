#include "idt.h"
#include "../../base/huevo.h"
#include "serial.h"

extern void trampa0(void);
extern void trampa1(void);
extern void trampa2(void);
extern void trampa3(void);
extern void trampa4(void);
extern void trampa5(void);
extern void trampa6(void);
extern void trampa7(void);
extern void trampa8(void);
extern void trampa9(void);
extern void trampa10(void);
extern void trampa11(void);
extern void trampa12(void);
extern void trampa13(void);
extern void trampa14(void);
extern void trampa15(void);
extern void trampa16(void);
extern void trampa17(void);
extern void trampa18(void);
extern void trampa19(void);
extern void trampa20(void);
extern void trampa21(void);
extern void trampa22(void);
extern void trampa23(void);
extern void trampa24(void);
extern void trampa25(void);
extern void trampa26(void);
extern void trampa27(void);
extern void trampa28(void);
extern void trampa29(void);
extern void trampa30(void);
extern void trampa31(void);

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

static void idt_configurar_puerta(int num, uint64_t dir_manejador) {
    g_idt[num].manejador_bajo  = (uint16_t)(dir_manejador & 0xFFFF);
    g_idt[num].selector_cs     = 0x08;
    g_idt[num].ist             = 0;
    g_idt[num].atributos       = 0x8E; // Puerta de Interrupcion 64-bit (Presente, Anillo 0)
    g_idt[num].manejador_medio = (uint16_t)((dir_manejador >> 16) & 0xFFFF);
    g_idt[num].manejador_alto  = (uint32_t)((dir_manejador >> 32) & 0xFFFFFFFF);
    g_idt[num].reservado       = 0;
}

void idt_iniciar(void) {
    g_puntero_idt.limite = sizeof(g_idt) - 1;
    g_puntero_idt.base   = (uint64_t)&g_idt;

    void *trampas[32] = {
        trampa0,  trampa1,  trampa2,  trampa3,  trampa4,  trampa5,  trampa6,  trampa7,
        trampa8,  trampa9,  trampa10, trampa11, trampa12, trampa13, trampa14, trampa15,
        trampa16, trampa17, trampa18, trampa19, trampa20, trampa21, trampa22, trampa23,
        trampa24, trampa25, trampa26, trampa27, trampa28, trampa29, trampa30, trampa31
    };

    for (int i = 0; i < 32; i++) {
        idt_configurar_puerta(i, (uint64_t)trampas[i]);
    }

    __asm__ volatile ("lidt %0" : : "m"(g_puntero_idt));
}

void manejador_excepciones(struct marco_interrupcion *marco) {
    const char *nombre = "Excepcion Desconocida";
    if (marco->num_interrupcion < 32) {
        nombre = g_nombres_excepciones[marco->num_interrupcion];
    }

    // Transferir control a El Huevo de la Estabilidad para autopsia y apagado
    huevo_quebrar(nombre, marco->rip, marco->rsp, marco->codigo_error);
}
