#include "vmx.h"
#include "serial.h"

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
    if (!vmx_soportado()) {
        serial_imprimir_linea("[ VMX ] CPU no soporta extensiones de virtualización Intel VMX.");
        return 1;
    }

    serial_imprimir_linea("[ VMX ] Hardware Intel VMX detectado en el procesador.");

    // Verificar MSR IA32_FEATURE_CONTROL
    uint64_t feature_control = rdmsr(MSR_IA32_FEATURE_CONTROL);
    if ((feature_control & 0x01) == 0) {
        // No esta bloqueado: activar bit 0 (bloqueo) y bit 2 (VMX fuera de SMX)
        feature_control |= 0x05;
        wrmsr(MSR_IA32_FEATURE_CONTROL, feature_control);
    } else if ((feature_control & 0x04) == 0) {
        serial_imprimir_linea("[ VMX ] VMX bloqueado por el firmware/BIOS.");
        return 1;
    }

    // Activar VMXE en CR4 (Bit 13)
    uint64_t cr4;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1 << 13);
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    // Obtener VMCS Revision ID de MSR IA32_VMX_BASIC
    uint64_t vmx_basic = rdmsr(MSR_IA32_VMX_BASIC);
    uint32_t vmcs_rev = (uint32_t)(vmx_basic & 0x7FFFFFFF);

    // Escribir Revision ID en los primeros 4 bytes de la region VMXON
    *(uint32_t *)g_vmxon_region = vmcs_rev;

    // Calcular direccion fisica de la region VMXON
    uint64_t dir_fisica_vmxon = (uint64_t)g_vmxon_region - base_virtual_kernel + base_fisica_kernel;

    // Ejecutar instruccion VMXON
    uint8_t error = 0;
    __asm__ volatile (
        "vmxon %1\n\t"
        "setc %0\n\t"
        : "=r"(error)
        : "m"(dir_fisica_vmxon)
        : "memory", "cc"
    );

    if (error) {
        serial_imprimir_linea("[ VMX ] Falló la instrucción VMXON. Continuando en modo Ring 0.");
        return 1;
    }

    g_vmx_activo = 1;
    serial_imprimir_linea("[ VMX ] CPU en VMX Root Operation (Ring -1). Guardián activo.");
    return 0;
}

int vmx_esta_activo(void) {
    return g_vmx_activo;
}
