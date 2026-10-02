#ifndef CONTROLADORES_VIDEO_INTEL_DIAGNOSTICO_RING0_H
#define CONTROLADORES_VIDEO_INTEL_DIAGNOSTICO_RING0_H

#include <stdint.h>

// Ejecuta el autodiagnóstico explícito del runtime i915 en Ring 0
// Prueba: IRQ spinlocks, conmutación de contexto x86_64, workqueues con reencolado,
// ciclo de vida de páginas con unpin diferido, SG table y contrato DMA con desbordamiento.
int intel_diagnostico_ring0_ejecutar(void);

#endif // CONTROLADORES_VIDEO_INTEL_DIAGNOSTICO_RING0_H
