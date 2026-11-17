#include "paginacion.h"
#include "cerrojo.h"
#include "trabajos.h"
#include "memoria.h"
#include "huevo.h"
#include "../arquitectura/x86_64/serial.h"

// Desplazamiento Higher Half Direct Map (HHDM)
static uint64_t g_hhdm_offset = 0;

// Tabla PML4 soberana del Kernel de TAEK OS
static tabla_paginacion_t *g_pml4_kernel = NULL;
static uint64_t            g_pml4_kernel_fisico = 0;
static int                 g_paginacion_iniciada = 0;

#define FISICA_A_VIRTUAL(phys) ((void *)((uint64_t)(phys) + g_hhdm_offset))
#define VIRTUAL_A_FISICA(virt) ((uint64_t)(virt) - g_hhdm_offset)

uint64_t paginacion_obtener_cr3(void) {
    uint64_t valor_cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(valor_cr3));
    return valor_cr3;
}

uint64_t paginacion_obtener_pml4_activo(void) {
    return g_pml4_kernel_fisico;
}

void paginacion_invalidar_tlb(uint64_t dir_virtual) {
    __asm__ volatile ("invlpg (%0)" : : "r"(dir_virtual) : "memory");
}

/* ------------------------------------------------------------------ H205: cerrojo e invalidacion entre CPU
 * invlpg solo vale para la CPU que lo ejecuta. Hasta H204 las auxiliares recargaban CR3 al empezar cada lote y los
 * mapeos no cambiaban durante uno. Con hilos corriendo siempre eso no alcanza:
 *  - cada cambio de una entrada que ya estaba presente (remapear o desmapear) sube una epoca y deja la
 *    sincronizacion pendiente; una entrada nueva no la necesita (la TLB no guarda traducciones ausentes);
 *  - paginacion_tlb_atender() vacia la TLB de la CPU que la llama si su epoca quedo atras. La llaman el manejador
 *    de la IPI, la espera de un cerrojo y el arranque de cada lote;
 *  - paginacion_tlb_sincronizar() manda la IPI y espera a que todas las CPU hayan atendido. Se llama una vez al
 *    final de una tanda de cambios, sin cerrojos en la mano. */
static cerrojo_t g_cerrojo_paginas = CERROJO("paginas");
static volatile uint64_t g_tlb_epoca;
static volatile uint64_t g_tlb_vista[CERROJO_CPUS];
static volatile uint32_t g_tlb_pendiente;
static uint64_t g_tlb_rondas, g_tlb_tardias, g_tlb_espera_max_ciclos;

static void tlb_vaciar_local(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1u << 7)) {   /* PGE: apagarlo y encenderlo vacia tambien las entradas globales */
        uint64_t sin = cr4 & ~(UINT64_C(1) << 7);
        __asm__ volatile("mov %0, %%cr4; mov %1, %%cr4" :: "r"(sin), "r"(cr4) : "memory");
    } else {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=&r"(cr3) :: "memory");
    }
}

void paginacion_tlb_atender(void) {
    unsigned c = cpu_actual();
    uint64_t e = __atomic_load_n(&g_tlb_epoca, __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_tlb_vista[c], __ATOMIC_RELAXED) == e) return;
    tlb_vaciar_local();
    __atomic_store_n(&g_tlb_vista[c], e, __ATOMIC_RELEASE);
}

/* Un cambio de una entrada presente: la CPU que lo hace ya hizo su invlpg */
static void tlb_marcar(void) {
    __atomic_add_fetch(&g_tlb_epoca, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tlb_pendiente, 1, __ATOMIC_RELEASE);
}

int paginacion_tlb_sincronizar(void) {
    if (!__atomic_exchange_n(&g_tlb_pendiente, 0, __ATOMIC_ACQ_REL)) return 0;
    uint64_t e = __atomic_load_n(&g_tlb_epoca, __ATOMIC_ACQUIRE);
    unsigned yo = cpu_actual(), n = trabajos_cpu_arrancadas();
    tlb_vaciar_local();
    __atomic_store_n(&g_tlb_vista[yo], e, __ATOMIC_RELEASE);
    if (n < 2) return 0;
    if (n > CERROJO_CPUS) n = CERROJO_CPUS;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t t0 = ((uint64_t)hi << 32) | lo, ahora = t0;
    uint64_t limite = trabajos_ticks_por_ms(0) * 200u;   /* 200 ms: una CPU que no contesta esta colgada */
    trabajos_ipi_demas();
    int tarde = 0;
    for (;;) {
        unsigned faltan = 0;
        for (unsigned i = 0; i < n; i++)
            if (i != yo && __atomic_load_n(&g_tlb_vista[i], __ATOMIC_ACQUIRE) < e) faltan++;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        ahora = ((uint64_t)hi << 32) | lo;
        if (!faltan) break;
        if (limite && ahora - t0 > limite) { tarde = 1; break; }
        __asm__ volatile("pause");
    }
    g_tlb_rondas++;
    if (ahora - t0 > g_tlb_espera_max_ciclos) g_tlb_espera_max_ciclos = ahora - t0;
    if (tarde) {
        g_tlb_tardias++;
        serial_imprimir_linea("[TLB] Una CPU no atendio la invalidacion en 200 ms.");
        return -1;
    }
    return 0;
}

void paginacion_tlb_estadisticas(uint64_t *epoca, uint64_t *rondas, uint64_t *tardias, uint64_t *espera_max_ciclos) {
    if (epoca) *epoca = __atomic_load_n(&g_tlb_epoca, __ATOMIC_RELAXED);
    if (rondas) *rondas = g_tlb_rondas;
    if (tardias) *tardias = g_tlb_tardias;
    if (espera_max_ciclos) *espera_max_ciclos = g_tlb_espera_max_ciclos;
}
uint64_t paginacion_tlb_vista(unsigned cpu) { return cpu < CERROJO_CPUS ? __atomic_load_n(&g_tlb_vista[cpu], __ATOMIC_ACQUIRE) : 0; }

void paginacion_iniciar(void) {
    g_hhdm_offset = memoria_obtener_hhdm_offset();

    // 1. Obtener el CR3 inicial generado por el gestor de arranque Limine
    uint64_t cr3_limine = paginacion_obtener_cr3();
    uint64_t pml4_limine_fisico = cr3_limine & MASCARA_DIRECCION_FISICA;
    tabla_paginacion_t *pml4_limine_virt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pml4_limine_fisico);

    // 2. Asignar una página física de 4 KiB para el PML4 soberano de TAEK OS
    g_pml4_kernel = (tabla_paginacion_t *)pmm_asignar_pagina_virtual();
    if (g_pml4_kernel == NULL) {
        serial_imprimir_linea("[PAGINACIÓN FATAL] No fue posible asignar memoria para el PML4 del Kernel.");
        huevo_quebrar("Fallo crítico al asignar PML4 de TAEK OS con el PMM", 0, 0, 0);
    }
    g_pml4_kernel_fisico = VIRTUAL_A_FISICA(g_pml4_kernel);

    // 3. Clonar la mitad superior del espacio de direcciones virtual (entradas 256 a 511)
    // Esto preserva el HHDM (0xFFFF800000000000), el código del Kernel (0xFFFFFFFF80000000),
    // el Framebuffer GOP y los búferes de hardware.
    for (int i = 256; i < ENTRADAS_POR_TABLA; i++) {
        g_pml4_kernel->entradas[i] = pml4_limine_virt->entradas[i];
    }

    // 4. La mitad inferior (0 a 255) se inicializa en 0 (espacio limpio para mapeos dinámicos y usuario)
    for (int i = 0; i < 256; i++) {
        g_pml4_kernel->entradas[i] = 0;
    }

    // 5. Cargar el nuevo PML4 soberano en el registro de control CR3 de la CPU
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(g_pml4_kernel_fisico)
        : "memory"
    );

    g_paginacion_iniciada = 1;

    serial_imprimir("[CR3 Limine: ");
    serial_imprimir_hex(cr3_limine);
    serial_imprimir(" -> CR3 TAEK OS Soberano: ");
    serial_imprimir_hex(g_pml4_kernel_fisico);
    serial_imprimir_linea("]");
}

static int mapear_sin_cerrojo(uint64_t dir_virtual, uint64_t dir_fisica, uint64_t banderas) {
    if(trabajos_en_curso())return -1;
    if (!g_paginacion_iniciada || g_pml4_kernel == NULL) {
        return -1;
    }

    // Alinear direcciones a límites de página de 4 KiB
    dir_virtual &= ~0xFFFULL;
    dir_fisica  &= ~0xFFFULL;

    uint64_t idx_pml4 = (dir_virtual >> 39) & 0x1FFULL;
    uint64_t idx_pdpt = (dir_virtual >> 30) & 0x1FFULL;
    uint64_t idx_pd   = (dir_virtual >> 21) & 0x1FFULL;
    uint64_t idx_pt   = (dir_virtual >> 12) & 0x1FFULL;

    // Nivel 4: PML4 -> PDPT
    if (!(g_pml4_kernel->entradas[idx_pml4] & PAGINA_PRESENTE)) {
        tabla_paginacion_t *nueva_pdpt = (tabla_paginacion_t *)pmm_asignar_pagina_virtual();
        if (nueva_pdpt == NULL) return -2;
        uint64_t pdpt_fisica = VIRTUAL_A_FISICA(nueva_pdpt);
        g_pml4_kernel->entradas[idx_pml4] = pdpt_fisica | PAGINA_PRESENTE | PAGINA_ESCRITURA | (banderas & PAGINA_USUARIO);
    }
    if (g_pml4_kernel->entradas[idx_pml4] & PAGINA_GIGANTE) return -3;
    tabla_paginacion_t *pdpt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(g_pml4_kernel->entradas[idx_pml4] & MASCARA_DIRECCION_FISICA);

    // Nivel 3: PDPT -> PD
    if (!(pdpt->entradas[idx_pdpt] & PAGINA_PRESENTE)) {
        tabla_paginacion_t *nueva_pd = (tabla_paginacion_t *)pmm_asignar_pagina_virtual();
        if (nueva_pd == NULL) return -2;
        uint64_t pd_fisica = VIRTUAL_A_FISICA(nueva_pd);
        pdpt->entradas[idx_pdpt] = pd_fisica | PAGINA_PRESENTE | PAGINA_ESCRITURA | (banderas & PAGINA_USUARIO);
    }
    if (pdpt->entradas[idx_pdpt] & PAGINA_GIGANTE) return -3;
    tabla_paginacion_t *pd = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pdpt->entradas[idx_pdpt] & MASCARA_DIRECCION_FISICA);

    // Nivel 2: PD -> PT
    if (!(pd->entradas[idx_pd] & PAGINA_PRESENTE)) {
        tabla_paginacion_t *nueva_pt = (tabla_paginacion_t *)pmm_asignar_pagina_virtual();
        if (nueva_pt == NULL) return -2;
        uint64_t pt_fisica = VIRTUAL_A_FISICA(nueva_pt);
        pd->entradas[idx_pd] = pt_fisica | PAGINA_PRESENTE | PAGINA_ESCRITURA | (banderas & PAGINA_USUARIO);
    }
    if (pd->entradas[idx_pd] & PAGINA_GIGANTE) return -3;
    tabla_paginacion_t *pt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pd->entradas[idx_pd] & MASCARA_DIRECCION_FISICA);

    // Nivel 1: PT -> Marco Físico de 4 KiB
    uint64_t vieja = pt->entradas[idx_pt], nueva = dir_fisica | banderas | PAGINA_PRESENTE;
    pt->entradas[idx_pt] = nueva;

    // Invalidar TLB para que la CPU refresque la traducción inmediatamente
    paginacion_invalidar_tlb(dir_virtual);
    /* H205: si ya habia una traduccion distinta, las otras CPU pueden tenerla en su TLB */
    if ((vieja & PAGINA_PRESENTE) && vieja != nueva) tlb_marcar();

    return 0;
}

int paginacion_mapear(uint64_t dir_virtual, uint64_t dir_fisica, uint64_t banderas) {
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo_paginas);
    int r = mapear_sin_cerrojo(dir_virtual, dir_fisica, banderas);
    cerrojo_soltar_irq(&g_cerrojo_paginas, f);
    return r;
}

static int desmapear_sin_cerrojo(uint64_t dir_virtual) {
    if(trabajos_en_curso())return -1;
    if (!g_paginacion_iniciada || g_pml4_kernel == NULL) {
        return -1;
    }

    dir_virtual &= ~0xFFFULL;

    uint64_t idx_pml4 = (dir_virtual >> 39) & 0x1FFULL;
    uint64_t idx_pdpt = (dir_virtual >> 30) & 0x1FFULL;
    uint64_t idx_pd   = (dir_virtual >> 21) & 0x1FFULL;
    uint64_t idx_pt   = (dir_virtual >> 12) & 0x1FFULL;

    if (!(g_pml4_kernel->entradas[idx_pml4] & PAGINA_PRESENTE)) return -1;
    tabla_paginacion_t *pdpt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(g_pml4_kernel->entradas[idx_pml4] & MASCARA_DIRECCION_FISICA);

    if (!(pdpt->entradas[idx_pdpt] & PAGINA_PRESENTE)) return -1;
    tabla_paginacion_t *pd = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pdpt->entradas[idx_pdpt] & MASCARA_DIRECCION_FISICA);

    if (!(pd->entradas[idx_pd] & PAGINA_PRESENTE)) return -1;
    tabla_paginacion_t *pt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pd->entradas[idx_pd] & MASCARA_DIRECCION_FISICA);

    // Desmapear la entrada en la tabla final
    uint64_t vieja = pt->entradas[idx_pt];
    pt->entradas[idx_pt] = 0;
    paginacion_invalidar_tlb(dir_virtual);
    if (vieja & PAGINA_PRESENTE) tlb_marcar();   /* H205 */

    return 0;
}

int paginacion_desmapear(uint64_t dir_virtual) {
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo_paginas);
    int r = desmapear_sin_cerrojo(dir_virtual);
    cerrojo_soltar_irq(&g_cerrojo_paginas, f);
    return r;
}

#define PML4_ALIAS_USUARIO 192ULL
int paginacion_alias_pml4(uint64_t dir_destino, uint64_t dir_origen) {
    const uint64_t lim = (UINT64_C(1) << 39) - 1;
    if (trabajos_en_curso() || !g_paginacion_iniciada || g_pml4_kernel == NULL) return -1;
    if ((dir_destino & lim) || (dir_origen & lim)) return -1;
    uint64_t d = (dir_destino >> 39) & 0x1FFULL, o = (dir_origen >> 39) & 0x1FFULL;
    /* Mitad inferior: solo la ventana 0x6000_0000_0000, memoria "de usuario" del driver multimedia en el mismo Ring 0. */
    if ((d < 256 && d != PML4_ALIAS_USUARIO) || o < 256) return -1;
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo_paginas);
    uint64_t entrada = g_pml4_kernel->entradas[o];
    int r = 0;
    if (!(entrada & PAGINA_PRESENTE)) r = -2;
    else if (g_pml4_kernel->entradas[d] == entrada) r = 0;
    else if (g_pml4_kernel->entradas[d] & PAGINA_PRESENTE) r = -3;
    else g_pml4_kernel->entradas[d] = entrada;
    cerrojo_soltar_irq(&g_cerrojo_paginas, f);
    return r;
}

uint64_t paginacion_obtener_fisica(uint64_t dir_virtual) {
    if (!g_paginacion_iniciada || g_pml4_kernel == NULL) {
        return 0;
    }

    uint64_t offset_pagina = dir_virtual & 0xFFFULL;
    uint64_t idx_pml4 = (dir_virtual >> 39) & 0x1FFULL;
    uint64_t idx_pdpt = (dir_virtual >> 30) & 0x1FFULL;
    uint64_t idx_pd   = (dir_virtual >> 21) & 0x1FFULL;
    uint64_t idx_pt   = (dir_virtual >> 12) & 0x1FFULL;

    if (!(g_pml4_kernel->entradas[idx_pml4] & PAGINA_PRESENTE)) return 0;
    tabla_paginacion_t *pdpt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(g_pml4_kernel->entradas[idx_pml4] & MASCARA_DIRECCION_FISICA);

    if (!(pdpt->entradas[idx_pdpt] & PAGINA_PRESENTE)) return 0;
    // Comprobar página gigante de 1 GiB en PDPT
    if (pdpt->entradas[idx_pdpt] & PAGINA_GIGANTE) {
        return (pdpt->entradas[idx_pdpt] & 0x000FFFFFC0000000ULL) | (dir_virtual & 0x3FFFFFFFULL);
    }
    tabla_paginacion_t *pd = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pdpt->entradas[idx_pdpt] & MASCARA_DIRECCION_FISICA);

    if (!(pd->entradas[idx_pd] & PAGINA_PRESENTE)) return 0;
    // Comprobar página grande de 2 MiB en PD
    if (pd->entradas[idx_pd] & PAGINA_GIGANTE) {
        return (pd->entradas[idx_pd] & 0x000FFFFFFFE00000ULL) | (dir_virtual & 0x1FFFFFULL);
    }
    tabla_paginacion_t *pt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pd->entradas[idx_pd] & MASCARA_DIRECCION_FISICA);

    if (!(pt->entradas[idx_pt] & PAGINA_PRESENTE)) return 0;
    return (pt->entradas[idx_pt] & MASCARA_DIRECCION_FISICA) | offset_pagina;
}

int paginacion_ejecutar_autodiagnostico(void) {
    if (!g_paginacion_iniciada) return 0;

    // 1. Asignar un marco físico de 4 KiB del PMM
    uint64_t marco_fisico = pmm_asignar_pagina_fisica();
    if (marco_fisico == 0) return 0;

    // 2. Dirección virtual de prueba arbitraria en la mitad inferior
    uint64_t dir_virtual_prueba = 0x00007FFF00000000ULL;

    // 3. Mapear la dirección virtual hacia el marco físico con permisos de lectura/escritura
    int res = paginacion_mapear(dir_virtual_prueba, marco_fisico, PAGINA_ATRIBUTOS_KERNEL);
    if (res != 0) {
        pmm_liberar_pagina_fisica(marco_fisico);
        return 0;
    }

    // 4. Escribir una firma mágica de 64 bits ("TAEKOSVM" = 0x5441454B4F53564D) a través de la dirección virtual
    uint64_t *ptr_virtual = (uint64_t *)dir_virtual_prueba;
    *ptr_virtual = 0x5441454B4F53564DULL;

    // 5. Verificar lectura a través del mapeo directo HHDM sobre el marco físico real
    uint64_t *ptr_hhdm = (uint64_t *)FISICA_A_VIRTUAL(marco_fisico);
    if (*ptr_hhdm != 0x5441454B4F53564DULL) {
        paginacion_desmapear(dir_virtual_prueba);
        pmm_liberar_pagina_fisica(marco_fisico);
        return 0;
    }

    // 6. Verificar traducción inversa de virtual a física
    uint64_t fisica_resuelta = paginacion_obtener_fisica(dir_virtual_prueba);
    if (fisica_resuelta != marco_fisico) {
        paginacion_desmapear(dir_virtual_prueba);
        pmm_liberar_pagina_fisica(marco_fisico);
        return 0;
    }

    // 7. Desmapear la página y verificar que ya no esté presente
    paginacion_desmapear(dir_virtual_prueba);
    if (paginacion_obtener_fisica(dir_virtual_prueba) != 0) {
        pmm_liberar_pagina_fisica(marco_fisico);
        return 0;
    }

    // 8. Devolver el marco físico al PMM
    pmm_liberar_pagina_fisica(marco_fisico);

    return 1;
}
