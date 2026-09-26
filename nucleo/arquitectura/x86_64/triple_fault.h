#ifndef ARQUITECTURA_X86_64_TRIPLE_FAULT_H
#define ARQUITECTURA_X86_64_TRIPLE_FAULT_H

#include <stdint.h>

// ==============================================================================
//  BATERÍA DE PRUEBAS DE RESILIENCIA Y TRIPLE FAULT PARA TAEK OS / VMX
// ==============================================================================
//  Tier 1: Triple fault limpio y determinista (IDTR nulo + INT3)
//  Tier 2: Harakiri (Destrucción de IDT + TSS/IST + stack de excepciones + UD2)
//  Tier 3: Seppuku (CR3 sacrificial aislado + IDT/stack/heap ausentes + UD2)
// ==============================================================================

typedef enum {
    TRIPLE_FAULT_TIER1_LIMPIO   = 1,
    TRIPLE_FAULT_TIER2_HARAKIRI = 2,
    TRIPLE_FAULT_TIER3_SEPPUKU  = 3
} triple_fault_tier_t;

// Test 1 (Tier 1): Triple fault limpio y determinista
// Valida la ruta VMX VM-Exit reason 2 sin corromper memoria previa.
void triple_fault_tier1_limpio(void) __attribute__((noreturn));

// Test 2 (Tier 2): Harakiri
// Inutiliza IDT, TSS/IST y stack de excepciones; luego dispara UD2.
// Comprueba que el hipervisor no depende de las estructuras de interrupción del guest.
void triple_fault_tier2_harakiri(void) __attribute__((noreturn));

// Test 3 (Tier 3): Seppuku
// Construye un PML4 sacrificial que solo mapea el stub de ejecución y no mapea
// IDT, stack, heap ni TSS; carga CR3 sacrificial y ejecuta UD2.
// Prueba extrema de aislamiento de memoria virtual e independencia de páginas del guest.
void triple_fault_tier3_seppuku(void) __attribute__((noreturn));

// Despachador interactivo de telemetría y ejecución con cuenta atrás
void triple_fault_ejecutar(triple_fault_tier_t tier);

// Obtener nombre legible del Tier
const char *triple_fault_obtener_nombre_tier(triple_fault_tier_t tier);

#endif // ARQUITECTURA_X86_64_TRIPLE_FAULT_H
