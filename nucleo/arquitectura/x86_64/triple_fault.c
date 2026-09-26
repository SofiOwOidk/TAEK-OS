#include "triple_fault.h"
#include "idt.h"
#include "gdt.h"
#include "serial.h"
#include "vmx.h"
#include "../../base/paginacion.h"
#include "../../base/memoria.h"
#include "../../base/tiempo.h"
#include "../../controladores/consola.h"

// Tablas de paginación sacrificiales para Seppuku (Tier 3)
// Alineadas estrictamente a 4096 bytes para x86_64 Long Mode
static uint8_t g_sac_pml4[4096] __attribute__((aligned(4096)));
static uint8_t g_sac_pdpt[4096] __attribute__((aligned(4096)));
static uint8_t g_sac_pd[4096]   __attribute__((aligned(4096)));
static uint8_t g_sac_pt[4096]   __attribute__((aligned(4096)));

const char *triple_fault_obtener_nombre_tier(triple_fault_tier_t tier) {
    switch (tier) {
        case TRIPLE_FAULT_TIER1_LIMPIO:
            return "Tier 1 (Triple Fault Limpio y Determinista)";
        case TRIPLE_FAULT_TIER2_HARAKIRI:
            return "Tier 2 (Harakiri - Excepciones/TSS Destruidos)";
        case TRIPLE_FAULT_TIER3_SEPPUKU:
            return "Tier 3 (Seppuku - CR3 Sacrificial Aislado)";
        default:
            return "Desconocido";
    }
}

// ==============================================================================
//  TIER 1: TRIPLE FAULT LIMPIO Y DETERMINISTA
// ==============================================================================
//  cli
//  sub rsp, 16
//  sidt [rsp]
//  xor eax, eax
//  mov [rsp], ax
//  mov qword [rsp + 2], 0
//  lidt [rsp]
//  int3
//
//  Cascada:
//  INT3 -> #BP (IDT invalida) -> #GP (IDT invalida) -> #DF (IDT invalida) -> TRIPLE FAULT -> VM EXIT 2
// ==============================================================================
__attribute__((noreturn)) void triple_fault_tier1_limpio(void) {
    serial_imprimir_linea("[ TIER 1 - TRIPLE FAULT ] Ejecutando: cli; sidt; lidt [lim=0, base=0]; int3");

    __asm__ volatile (
        "cli\n\t"
        "subq $16, %%rsp\n\t"
        "sidt (%%rsp)\n\t"
        "xorl %%eax, %%eax\n\t"
        "movw %%ax, (%%rsp)\n\t"
        "movq $0, 2(%%rsp)\n\t"
        "lidt (%%rsp)\n\t"
        "int3\n\t"
        :
        :
        : "rax", "memory"
    );

    __builtin_unreachable();
}

// ==============================================================================
//  TIER 2: HARAKIRI
// ==============================================================================
//  Matar IDT + stack de excepciones / TSS / IST del guest y disparar UD2.
//  Comprueba que el hipervisor no dependa de las estructuras del guest.
//
//  Cascada:
//  UD2 -> #UD (compuerta ausente en IDT) -> #NP / #GP -> #DF (sin IST válido) -> TRIPLE FAULT
// ==============================================================================
__attribute__((noreturn)) void triple_fault_tier2_harakiri(void) {
    serial_imprimir_linea("[ TIER 2 - HARAKIRI ] Inutilizando IDT, TSS64 e IST del guest...");

    // 1. Destruir TSS e IST (GDT / hardware task state)
    gdt_destruir_tss_e_ist();

    // 2. Destruir la IDT activa
    idt_corromper_para_harakiri();

    serial_imprimir_linea("[ TIER 2 - HARAKIRI ] Anulando RSP del guest y detonando UD2...");

    // 3. Destruir RSP, desactivar interrupciones y disparar UD2
    __asm__ volatile (
        "cli\n\t"
        "movq $0, %%rsp\n\t"
        "ud2\n\t"
        :
        :
        : "memory"
    );

    __builtin_unreachable();
}

// ==============================================================================
//  TIER 3: SEPPUKU
// ==============================================================================
//  Construye una tabla de páginas sacrificial válida que mapee ÚNICAMENTE
//  la página que contiene el stub de prueba, y deliberadamente NO mapee:
//  - IDT
//  - Stack del guest
//  - Heap
//  - TSS / IST
//  - Resto del kernel
//
//  mov cr3, cr3_sacrificial
//  ud2
//
//  Cascada:
//  UD2 -> #UD -> intento de acceso a IDT -> #PF en entrega de #UD -> #DF ->
//  intento de entrega de #DF (IDT sigue sin mapear) -> TRIPLE FAULT -> VM EXIT 2
// ==============================================================================

// Stub de ejecución: debe residir en código y ocupar menos de 64 bytes
__attribute__((noinline, aligned(64)))
static void seppuku_ejecutar_stub(uint64_t cr3_sacrificial_fisico) {
    __asm__ volatile (
        "cli\n\t"
        "movq $0, %%rsp\n\t"
        "movq %0, %%cr3\n\t"
        "ud2\n\t"
        :
        : "r"(cr3_sacrificial_fisico)
        : "memory"
    );
}

__attribute__((noreturn)) void triple_fault_tier3_seppuku(void) {
    serial_imprimir_linea("[ TIER 3 - SEPPUKU ] Iniciando construcción de tabla de páginas sacrificial...");

    // 1. Limpiar completamente las 4 tablas sacrificiales (512 entradas en 0 = NO PRESENTE)
    memset(g_sac_pml4, 0, 4096);
    memset(g_sac_pdpt, 0, 4096);
    memset(g_sac_pd,   0, 4096);
    memset(g_sac_pt,   0, 4096);

    // 2. Obtener direcciones físicas de las tablas sacrificiales
    uint64_t phys_pml4 = paginacion_obtener_fisica((uint64_t)g_sac_pml4);
    uint64_t phys_pdpt = paginacion_obtener_fisica((uint64_t)g_sac_pdpt);
    uint64_t phys_pd   = paginacion_obtener_fisica((uint64_t)g_sac_pd);
    uint64_t phys_pt   = paginacion_obtener_fisica((uint64_t)g_sac_pt);

    if (phys_pml4 == 0 || phys_pdpt == 0 || phys_pd == 0 || phys_pt == 0) {
        serial_imprimir_linea("[ TIER 3 - SEPPUKU ERROR ] No se pudieron resolver direcciones físicas de tablas sacrificiales.");
        // Caída de seguridad al Tier 1 si fallara el VMM
        triple_fault_tier1_limpio();
    }

    // 3. Obtener dirección virtual y física de la página de código del stub
    uint64_t vaddr_stub = (uint64_t)&seppuku_ejecutar_stub;
    uint64_t phys_stub  = paginacion_obtener_fisica(vaddr_stub);

    if (phys_stub == 0) {
        serial_imprimir_linea("[ TIER 3 - SEPPUKU ERROR ] No se pudo resolver la dirección física del stub ejecutable.");
        triple_fault_tier1_limpio();
    }

    uint64_t idx_pml4 = (vaddr_stub >> 39) & 0x1FFULL;
    uint64_t idx_pdpt = (vaddr_stub >> 30) & 0x1FFULL;
    uint64_t idx_pd   = (vaddr_stub >> 21) & 0x1FFULL;
    uint64_t idx_pt   = (vaddr_stub >> 12) & 0x1FFULL;

    // 4. Enlazar ÚNICAMENTE la ruta de 4 niveles hacia la página del stub
    ((uint64_t *)g_sac_pml4)[idx_pml4] = phys_pdpt | PAGINA_PRESENTE | PAGINA_ESCRITURA;
    ((uint64_t *)g_sac_pdpt)[idx_pdpt] = phys_pd   | PAGINA_PRESENTE | PAGINA_ESCRITURA;
    ((uint64_t *)g_sac_pd)[idx_pd]     = phys_pt   | PAGINA_PRESENTE | PAGINA_ESCRITURA;
    ((uint64_t *)g_sac_pt)[idx_pt]     = (phys_stub & ~0xFFFULL) | PAGINA_PRESENTE | PAGINA_ESCRITURA;

    // IDT, Stack del Guest, Heap, TSS y MMIO deliberadamente NO MAPEADOS (entradas 0)

    serial_imprimir("[ TIER 3 - SEPPUKU ] PML4 Sacrificial armado en CR3 físico: 0x");
    serial_imprimir_hex(phys_pml4);
    serial_imprimir_linea("");
    serial_imprimir("[ TIER 3 - SEPPUKU ] Única página mapeada: 0x");
    serial_imprimir_hex(vaddr_stub & ~0xFFFULL);
    serial_imprimir(" -> Físico 0x");
    serial_imprimir_hex(phys_stub & ~0xFFFULL);
    serial_imprimir_linea("");

    // 5. Corrupción no destructiva de objetos no esenciales del guest (sin tocar PCI MMIO ni Hipervisor)
    // Se desactivan las banderas de páginas globales para asegurar purga total de TLB
    uint64_t cr4_val;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4_val));
    cr4_val &= ~(1ULL << 7); // Desactivar CR4.PGE
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4_val));

    serial_imprimir_linea("[ TIER 3 - SEPPUKU ] Cargando CR3 sacrificial y detonando UD2...");

    // 6. Invocar el stub sacrificial
    seppuku_ejecutar_stub(phys_pml4);

    __builtin_unreachable();
}

// ==============================================================================
//  DESPACHADOR GENERAL Y TELEMETRÍA
// ==============================================================================
void triple_fault_ejecutar(triple_fault_tier_t tier) {
    if (!vmx_intercepcion_triple_fault_disponible()) {
        consola_imprimir_linea_color(
            "Triple Fault bloqueado: VMXON no configura guest ni manejador VM-exit.",
            COLOR_ERROR_DEFAULT);
        serial_imprimir_linea("[TRIPLE FAULT] Bloqueado: no hay guest VMX ni handler VM-exit.");
        return;
    }
    consola_imprimir_linea("");

    switch (tier) {
        case TRIPLE_FAULT_TIER1_LIMPIO:
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  [ TIER 1 ] PROVOCAR TRIPLE FAULT - LIMPIO Y DETERMINISTA                      ", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  Arquitectura : x86_64 Long Mode (Anillo 0 / VMX)", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("  Secuencia    : cli -> sidt [rsp] -> idtr.lim=0, idtr.base=0 -> lidt -> int3", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  Cascada      : INT3 -> #BP (IDT invalida) -> #GP -> #DF -> TRIPLE FAULT", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  Misión       : Verificar únicamente la ruta VMX VM-Exit reason 2.", COLOR_EXITO_DEFAULT);
            consola_imprimir_linea_color("  Diagnóstico  : Si Don Cangrejo aparece: Hipervisor VMX interceptó el colapso.", COLOR_EXITO_DEFAULT);
            consola_imprimir_linea_color("                 Si el equipo se reinicia: CPU colapsó fuera del host VMX.", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            break;

        case TRIPLE_FAULT_TIER2_HARAKIRI:
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  [ TIER 2 ] HARAKIRI - DESTRUCCIÓN DE ESTRUCTURAS DE EXCEPCIÓN GUEST           ", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  Arquitectura : x86_64 Long Mode (Anillo 0 / VMX)", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("  Secuencia    : Corromper IDT + anular TSS64/IST + stack a 0 + ud2", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  Cascada      : UD2 -> #UD -> fallo compuerta -> #DF -> sin pila IST -> TRIPLE FAULT", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  Misión       : Comprobar independencia de excepciones respecto al guest.", COLOR_EXITO_DEFAULT);
            consola_imprimir_linea_color("  Diagnóstico  : Verifica que el host no dependa de IDT/TSS/stack del guest.", COLOR_EXITO_DEFAULT);
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            break;

        case TRIPLE_FAULT_TIER3_SEPPUKU:
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  [ TIER 3 ] SEPPUKU - COLAPSO DE MEMORIA VIRTUAL (CR3 SACRIFICIAL)             ", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  Arquitectura : x86_64 Long Mode (Anillo 0 / VMX)", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("  Secuencia    : PML4 sacrificial aislado -> mov cr3 -> ud2", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  Mapeo        : SOLO mapea pagina del stub. NO mapea IDT, stack, heap ni TSS.", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  Cascada      : UD2 -> #UD -> acceso IDT (#PF) -> #DF (sin IDT) -> TRIPLE FAULT", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  Misión       : Prueba de aislamiento total: supervisor host inmune al colapso virtual.", COLOR_EXITO_DEFAULT);
            consola_imprimir_linea_color("  Diagnóstico  : Demostración suprema de contingencia fuera del contexto destruido.", COLOR_EXITO_DEFAULT);
            consola_imprimir_linea_color("================================================================================", COLOR_ERROR_DEFAULT);
            break;
    }

    consola_imprimir_linea("");
    consola_imprimir_color("  [!] Detonando Triple Fault de hardware en: ", COLOR_AVISO_DEFAULT);

    for (int seg = 3; seg > 0; seg--) {
        consola_imprimir_dec(seg);
        consola_imprimir("... ");
        serial_imprimir("[TRIPLE FAULT] Detonación en: ");
        serial_imprimir_dec(seg);
        serial_imprimir_linea("...");
        esperar_milisegundos(700);
    }

    consola_imprimir_linea_color("¡¡¡COLAPSO INICIADO!!!", COLOR_ERROR_DEFAULT);
    serial_imprimir_linea("[TRIPLE FAULT] ¡¡¡DISPARANDO TRIPLE FAULT REAL!!!");

    switch (tier) {
        case TRIPLE_FAULT_TIER1_LIMPIO:
            triple_fault_tier1_limpio();
            break;
        case TRIPLE_FAULT_TIER2_HARAKIRI:
            triple_fault_tier2_harakiri();
            break;
        case TRIPLE_FAULT_TIER3_SEPPUKU:
            triple_fault_tier3_seppuku();
            break;
    }
}
