#include "vmx.h"
#include "serial.h"
#include "../../base/paginacion.h"

#define MSR_IA32_FEATURE_CONTROL 0x0000003A
#define MSR_IA32_VMX_BASIC       0x00000480

static int g_vmx_activo = 0;
static uint8_t g_vmxon_region[4096] __attribute__((aligned(4096)));

static inline void cpuid(uint32_t hoja, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(hoja), "c"(0));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

__attribute__((unused))
static inline void wrmsr(uint32_t msr, uint64_t valor) {
    uint32_t lo = (uint32_t)valor;
    uint32_t hi = (uint32_t)(valor >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

int vmx_soportado(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    // Bit 5 de ECX indica Intel VMX
    return (ecx & (1 << 5)) != 0;
}

int vmx_iniciar(uint64_t base_fisica_kernel, uint64_t base_virtual_kernel) {
    (void)base_fisica_kernel;
    (void)base_virtual_kernel;
    if (!vmx_soportado()) {
        serial_imprimir_linea("[ VMX ] CPU no soporta extensiones de virtualización Intel VMX.");
        return 1;
    }

    serial_imprimir_linea("[ VMX ] Hardware Intel VMX detectado en el procesador.");

    // Verificar MSR IA32_FEATURE_CONTROL (0x3A)
    uint64_t feature_control = rdmsr(MSR_IA32_FEATURE_CONTROL);

    // Bit 0: Lock bit, Bit 2: VMX outside SMX
    if ((feature_control & 0x05) != 0x05) {
        // Si el lock bit ya está activo (bit 0), el firmware bloqueó la configuración.
        // NUNCA ejecutar wrmsr sobre 0x3A si el lock bit está activo (causa #GP inmediato).
        if (feature_control & 0x01) {
            serial_imprimir_linea("[ VMX ] VMX bloqueado o desactivado en BIOS (Lock bit activo). Continuando en Ring 0.");
            return 1;
        }

        // Si el lock bit está libre, intentar activar VMX fuera de SMX (bit 2) y bloquear (bit 0)
        // Forzamos explícitamente EDX (hi) en 0 para no tocar bits reservados 63:20
        uint32_t lo = (uint32_t)((feature_control & 0xFFFC) | 0x05);
        uint32_t hi = 0;
        __asm__ volatile (
            "wrmsr"
            :
            : "c"(MSR_IA32_FEATURE_CONTROL), "a"(lo), "d"(hi)
        );

        feature_control = rdmsr(MSR_IA32_FEATURE_CONTROL);
        if ((feature_control & 0x05) != 0x05) {
            serial_imprimir_linea("[ VMX ] BIOS no permitió activar VMX en MSR 0x3A. Continuando en Ring 0.");
            return 1;
        }
    }

    // 2. Configurar bits obligatorios de CR0 y CR4 para VMX según MSRs FIXED0 / FIXED1
    // (Intel SDM Vol 3C, Cap. 23.8 / 31.1.1: CR0.NE, CR0.PE, CR0.PG y CR4.VMXE deben coincidir estrictamente)
    uint64_t cr0_fixed0 = rdmsr(0x486); // IA32_VMX_CR0_FIXED0
    uint64_t cr0_fixed1 = rdmsr(0x487); // IA32_VMX_CR0_FIXED1
    uint64_t cr4_fixed0 = rdmsr(0x488); // IA32_VMX_CR4_FIXED0
    uint64_t cr4_fixed1 = rdmsr(0x489); // IA32_VMX_CR4_FIXED1

    uint64_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= cr0_fixed0;
    cr0 &= cr0_fixed1;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 13); // CR4.VMXE
    cr4 |= cr4_fixed0;
    cr4 &= cr4_fixed1;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    // 3. Obtener VMCS Revision ID de MSR IA32_VMX_BASIC (0x480)
    uint64_t vmx_basic = rdmsr(MSR_IA32_VMX_BASIC);
    uint32_t vmcs_rev = (uint32_t)(vmx_basic & 0x7FFFFFFF);

    // Limpiar la región VMXON (4096 bytes) y escribir VMCS Revision ID en los primeros 4 bytes
    for (int i = 0; i < 4096; i++) {
        g_vmxon_region[i] = 0;
    }
    *(uint32_t *)g_vmxon_region = vmcs_rev;

    // 4. Calcular y verificar dirección física de la región VMXON
    uint64_t dir_fisica_vmxon = paginacion_obtener_fisica((uint64_t)g_vmxon_region);
    if (dir_fisica_vmxon == 0 || (dir_fisica_vmxon & 0xFFFULL) != 0) {
        serial_imprimir_linea("[ VMX ] Dirección física inválida o no alineada para VMXON. Continuando en Ring 0.");
        return 1;
    }

    // Si el bit 48 de VMX_BASIC está activo, la dirección física debe caber en 32 bits (< 4 GiB)
    if ((vmx_basic & (1ULL << 48)) && (dir_fisica_vmxon > 0xFFFFFFFFULL)) {
        serial_imprimir_linea("[ VMX ] Dirección física de VMXON excede límite de 32 bits. Continuando en Ring 0.");
        return 1;
    }

    // 5. Ejecutar instrucción VMXON
    uint8_t error = 0;
    __asm__ volatile (
        "vmxon %1\n\t"
        "setbe %0\n\t"
        : "=r"(error)
        : "m"(dir_fisica_vmxon)
        : "memory", "cc"
    );

    if (error) {
        serial_imprimir_linea("[ VMX ] Falló la instrucción VMXON (CF/ZF activos). Continuando en modo Ring 0.");
        return 1;
    }

    g_vmx_activo = 1;
    serial_imprimir_linea("[ VMX ] CPU en VMX Root Operation (Ring -1). Guardián activo.");
    return 0;
}

int vmx_esta_activo(void) {
    return g_vmx_activo;
}
