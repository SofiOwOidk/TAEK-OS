#include "gdt.h"

static struct entrada_gdt g_gdt[3];
static struct puntero_gdt g_puntero_gdt;

static void gdt_configurar_puerta(int num, uint32_t base, uint32_t limite, uint8_t acceso, uint8_t gran) {
    g_gdt[num].base_baja    = (base & 0xFFFF);
    g_gdt[num].base_media   = (base >> 16) & 0xFF;
    g_gdt[num].base_alta    = (base >> 24) & 0xFF;

    g_gdt[num].limite_bajo  = (limite & 0xFFFF);
    g_gdt[num].granularidad = (limite >> 16) & 0x0F;

    g_gdt[num].granularidad |= gran & 0xF0;
    g_gdt[num].acceso       = acceso;
}

void gdt_iniciar(void) {
    g_puntero_gdt.limite = (sizeof(struct entrada_gdt) * 3) - 1;
    g_puntero_gdt.base   = (uint64_t)&g_gdt;

    // 0: Descriptor nulo obligatorio
    gdt_configurar_puerta(0, 0, 0, 0, 0);

    // 1: Codigo de Nucleo en 64 bits (0x08): Acceso 0x9A, Bandera Modo Largo 0x20
    gdt_configurar_puerta(1, 0, 0, 0x9A, 0x20);

    // 2: Datos de Nucleo en 64 bits (0x10): Acceso 0x92, Banderas 0x00
    gdt_configurar_puerta(2, 0, 0, 0x92, 0x00);

    __asm__ volatile ("lgdt %0" : : "m"(g_puntero_gdt));

    __asm__ volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "pushq $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        : : : "rax"
    );
}
