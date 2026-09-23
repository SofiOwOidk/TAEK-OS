#include "apic.h"
#include "puertos.h"
#include "serial.h"
#include "../../base/paginacion.h"
#include "../../base/tiempo.h"
#include "../../base/huevo.h"

#define MSR_IA32_APIC_BASE 0x0000001B

// Offsets estándar de registros Local APIC (xAPIC)
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ESR           0x280
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370

static struct estado_apic g_apic;

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(0));
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

void pic_desactivar(void) {
    // Desactivar PIC 8259 Maestro y Esclavo enmascarando todas las líneas
    escribir_puerto_b(0x21, 0xFF);
    esperar_io();
    escribir_puerto_b(0xA1, 0xFF);
    esperar_io();
    serial_imprimir_linea("[PIC 8259 Legacy Desactivado] ");
}

uint32_t apic_leer(uint32_t reg) {
    if (g_apic.es_x2apic) {
        uint32_t msr = 0x800 + (reg >> 4);
        return (uint32_t)rdmsr(msr);
    } else {
        if (!g_apic.dir_virtual_base) return 0;
        volatile uint32_t *addr = (volatile uint32_t *)(g_apic.dir_virtual_base + reg);
        return *addr;
    }
}

void apic_escribir(uint32_t reg, uint32_t val) {
    if (g_apic.es_x2apic) {
        uint32_t msr = 0x800 + (reg >> 4);
        wrmsr(msr, (uint64_t)val);
    } else {
        if (!g_apic.dir_virtual_base) return;
        volatile uint32_t *addr = (volatile uint32_t *)(g_apic.dir_virtual_base + reg);
        *addr = val;
    }
}

void apic_enviar_eoi(void) {
    if (g_apic.es_x2apic) {
        wrmsr(0x80B, 0); // x2APIC EOI MSR
    } else {
        apic_escribir(LAPIC_EOI, 0);
    }
}

void apic_enviar_self_ipi(uint8_t vector) {
    if (g_apic.es_x2apic) {
        wrmsr(0x83F, vector); // x2APIC Self-IPI MSR
    } else {
        // En xAPIC: ICR Low con Dest Shorthand = Self (bit 18)
        apic_escribir(LAPIC_ICR_LOW, (1 << 18) | vector);
    }
}

void apic_despachar_irq(struct marco_interrupcion *marco) {
    g_apic.interrupciones_recibidas++;
    if (marco->num_interrupcion == APIC_VECTOR_PRUEBA_IPI) {
        g_apic.ipi_recibidos++;
    }

    if (marco->num_interrupcion != APIC_VECTOR_ESPURIO) {
        apic_enviar_eoi();
    }
}

int apic_iniciar(void) {
    if (g_apic.activo) return 0;

    pic_desactivar();

    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(edx & (1 << 9))) {
        serial_imprimir_linea("[ERROR FATAL] CPU sin soporte para Local APIC.");
        return 1;
    }

    int soporta_x2apic = (ecx & (1 << 21)) != 0;
    uint64_t apic_base = rdmsr(MSR_IA32_APIC_BASE);
    g_apic.dir_fisica_base = apic_base & 0xFFFFF000ULL;

    if (soporta_x2apic) {
        // Habilitar x2APIC (Bit 11: Global Enable, Bit 10: x2APIC Enable)
        apic_base |= (1ULL << 11) | (1ULL << 10);
        wrmsr(MSR_IA32_APIC_BASE, apic_base);
        g_apic.es_x2apic = 1;
        g_apic.dir_virtual_base = 0; // Se accede por MSRs directos
    } else {
        // Modo xAPIC estándar con mapeo MMIO
        apic_base |= (1ULL << 11);
        wrmsr(MSR_IA32_APIC_BASE, apic_base);
        g_apic.es_x2apic = 0;
        g_apic.dir_virtual_base = APIC_MMIO_VIRTUAL_BASE;

        // Mapear la página física de 4 KiB del APIC sin caché (PCD)
        paginacion_mapear(g_apic.dir_virtual_base, g_apic.dir_fisica_base, PAGINA_ATRIBUTOS_MMIO);
    }

    // Configuración del Spurious Interrupt Vector y habilitación por software (bit 8)
    apic_escribir(LAPIC_SVR, 0x100 | APIC_VECTOR_ESPURIO);

    // Permitir todas las interrupciones (Task Priority Register = 0)
    apic_escribir(LAPIC_TPR, 0);

    // Enmascarar LINT0 (línea legacy PIC) y configurar LINT1 como NMI
    apic_escribir(LAPIC_LVT_LINT0, 0x10000);
    apic_escribir(LAPIC_LVT_LINT1, 0x400);

    // Limpiar errores
    apic_escribir(LAPIC_ESR, 0);
    apic_escribir(LAPIC_ESR, 0);

    // Enviar EOI por si había algo pendiente
    apic_enviar_eoi();

    // Leer ID y Versión
    if (g_apic.es_x2apic) {
        g_apic.id      = (uint32_t)rdmsr(0x802);
        g_apic.version = (uint32_t)rdmsr(0x803) & 0xFF;
    } else {
        g_apic.id      = (apic_leer(LAPIC_ID) >> 24) & 0xFF;
        g_apic.version = apic_leer(LAPIC_VERSION) & 0xFF;
    }

    g_apic.activo = 1;

    // Habilitar interrupciones por hardware a nivel de CPU
    __asm__ volatile ("sti");

    return 0;
}

const struct estado_apic *apic_obtener_estado(void) {
    if (!g_apic.activo) apic_iniciar();
    return &g_apic;
}

int apic_ejecutar_autodiagnostico(void) {
    if (!g_apic.activo) {
        if (apic_iniciar() != 0) return 1;
    }

    uint32_t ipis_antes = g_apic.ipi_recibidos;

    // Disparar un Self-IPI al vector de prueba 80
    apic_enviar_self_ipi(APIC_VECTOR_PRUEBA_IPI);

    // Esperar unos pocos ciclos para que la CPU atienda la interrupción
    int intentos = 1000;
    while (intentos > 0 && g_apic.ipi_recibidos == ipis_antes) {
        __asm__ volatile ("pause");
        intentos--;
    }

    if (g_apic.ipi_recibidos <= ipis_antes) {
        return 2; // No se recibió el Self-IPI
    }

    return 0;
}
