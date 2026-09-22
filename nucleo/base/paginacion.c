#include "paginacion.h"
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

int paginacion_mapear(uint64_t dir_virtual, uint64_t dir_fisica, uint64_t banderas) {
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
    tabla_paginacion_t *pdpt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(g_pml4_kernel->entradas[idx_pml4] & MASCARA_DIRECCION_FISICA);

    // Nivel 3: PDPT -> PD
    if (!(pdpt->entradas[idx_pdpt] & PAGINA_PRESENTE)) {
        tabla_paginacion_t *nueva_pd = (tabla_paginacion_t *)pmm_asignar_pagina_virtual();
        if (nueva_pd == NULL) return -2;
        uint64_t pd_fisica = VIRTUAL_A_FISICA(nueva_pd);
        pdpt->entradas[idx_pdpt] = pd_fisica | PAGINA_PRESENTE | PAGINA_ESCRITURA | (banderas & PAGINA_USUARIO);
    }
    tabla_paginacion_t *pd = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pdpt->entradas[idx_pdpt] & MASCARA_DIRECCION_FISICA);

    // Nivel 2: PD -> PT
    if (!(pd->entradas[idx_pd] & PAGINA_PRESENTE)) {
        tabla_paginacion_t *nueva_pt = (tabla_paginacion_t *)pmm_asignar_pagina_virtual();
        if (nueva_pt == NULL) return -2;
        uint64_t pt_fisica = VIRTUAL_A_FISICA(nueva_pt);
        pd->entradas[idx_pd] = pt_fisica | PAGINA_PRESENTE | PAGINA_ESCRITURA | (banderas & PAGINA_USUARIO);
    }
    tabla_paginacion_t *pt = (tabla_paginacion_t *)FISICA_A_VIRTUAL(pd->entradas[idx_pd] & MASCARA_DIRECCION_FISICA);

    // Nivel 1: PT -> Marco Físico de 4 KiB
    pt->entradas[idx_pt] = dir_fisica | banderas | PAGINA_PRESENTE;

    // Invalidar TLB para que la CPU refresque la traducción inmediatamente
    paginacion_invalidar_tlb(dir_virtual);

    return 0;
}

int paginacion_desmapear(uint64_t dir_virtual) {
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
    pt->entradas[idx_pt] = 0;
    paginacion_invalidar_tlb(dir_virtual);

    return 0;
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
