#include "gdt.h"

static struct entrada_gdt g_gdt[5];
static struct puntero_gdt g_puntero_gdt;

struct tss64 {
    uint32_t reservado0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reservado1;
    uint64_t ist[7];
    uint64_t reservado2;
    uint16_t reservado3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss64 g_tss;
static uint8_t g_pila_rsp0[16384] __attribute__((aligned(16)));
static uint8_t g_pila_df[16384] __attribute__((aligned(16)));
static uint8_t g_pila_nmi[16384] __attribute__((aligned(16)));
static uint8_t g_pila_mc[16384] __attribute__((aligned(16)));

/* TSS y tres IST privados: nunca compartir una pila de excepción entre CPUs. */
static struct entrada_gdt g_ap_gdt[3][5];
static struct tss64 g_ap_tss[3];
static uint8_t g_ap_pilas[3][4][16384] __attribute__((aligned(16)));
void gdt_iniciar_cpu(unsigned indice) {
    if(!indice || indice>3)return;
    unsigned i=indice-1;
    __builtin_memcpy(g_ap_gdt[i],g_gdt,sizeof(g_gdt));
    struct tss64 *t=&g_ap_tss[i];
    t->rsp0=(uint64_t)(g_ap_pilas[i][0]+16384);
    for(unsigned j=0;j<3;j++)t->ist[j]=(uint64_t)(g_ap_pilas[i][j+1]+16384);
    t->iomap_base=sizeof(*t);
    uint64_t base=(uint64_t)t, bajo=(sizeof(*t)-1)|((base&0xffffff)<<16)|
        (UINT64_C(0x89)<<40)|(((base>>24)&255)<<56),alto=base>>32;
    __builtin_memcpy(&g_ap_gdt[i][3],&bajo,8);
    __builtin_memcpy(&g_ap_gdt[i][4],&alto,8);
    struct puntero_gdt p={sizeof(g_ap_gdt[i])-1,(uint64_t)g_ap_gdt[i]};
    __asm__ volatile("lgdt %0"::"m"(p):"memory");
    __asm__ volatile(
        "mov $0x10,%%ax;mov %%ax,%%ds;mov %%ax,%%es;mov %%ax,%%ss;mov %%ax,%%fs;mov %%ax,%%gs;"
        "pushq $0x08;lea 1f(%%rip),%%rax;pushq %%rax;lretq;1:"
        :::"rax","memory");
    uint16_t selector=0x18;
    __asm__ volatile("ltr %0"::"r"(selector):"memory");
}

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
    g_puntero_gdt.limite = sizeof(g_gdt) - 1;
    g_puntero_gdt.base   = (uint64_t)&g_gdt;

    // 0: Descriptor nulo obligatorio
    gdt_configurar_puerta(0, 0, 0, 0, 0);

    // 1: Codigo de Nucleo en 64 bits (0x08): Acceso 0x9A, Bandera Modo Largo 0x20
    gdt_configurar_puerta(1, 0, 0, 0x9A, 0x20);

    // 2: Datos de Nucleo en 64 bits (0x10): Acceso 0x92, Banderas 0x00
    gdt_configurar_puerta(2, 0, 0, 0x92, 0x00);

    g_tss.rsp0 = (uint64_t)(g_pila_rsp0 + sizeof(g_pila_rsp0));
    g_tss.ist[0] = (uint64_t)(g_pila_df + sizeof(g_pila_df));
    g_tss.ist[1] = (uint64_t)(g_pila_nmi + sizeof(g_pila_nmi));
    g_tss.ist[2] = (uint64_t)(g_pila_mc + sizeof(g_pila_mc));
    g_tss.iomap_base = sizeof(g_tss);
    uint64_t base_tss = (uint64_t)&g_tss;
    gdt_configurar_puerta(3, (uint32_t)base_tss, sizeof(g_tss) - 1, 0x89, 0x00);
    uint64_t mitad_alta = base_tss >> 32;
    __builtin_memcpy(&g_gdt[4], &mitad_alta, sizeof(mitad_alta));

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
    uint16_t selector_tss = 0x18;
    __asm__ volatile ("ltr %0" : : "r"(selector_tss));
}

void gdt_destruir_tss_e_ist(void) {
    g_tss.rsp0 = 0;
    g_tss.rsp1 = 0;
    g_tss.rsp2 = 0;
    for (int i = 0; i < 7; i++) {
        g_tss.ist[i] = 0;
    }
    for (uint64_t i = 0; i < sizeof(g_pila_df); i++) {
        g_pila_df[i] = 0;
    }
    for (uint64_t i = 0; i < sizeof(g_pila_nmi); i++) {
        g_pila_nmi[i] = 0;
    }
    for (uint64_t i = 0; i < sizeof(g_pila_mc); i++) {
        g_pila_mc[i] = 0;
    }
}
