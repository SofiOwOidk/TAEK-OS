#ifndef I915_CPU_RUNTIME_H
#define I915_CPU_RUNTIME_H
/* Frontera de arranque sin importar los tipos del shim Linux al kernel
 * antiguo, que tiene otros tipos spinlock/atomic con el mismo nombre. */
#define I915_MAX_CPU 4
#define I915_CPU_INVALIDA I915_MAX_CPU
void i915_tareas_iniciar(void);
int i915_scheduler_cpu(unsigned cpu);
unsigned i915_scheduler_cpu_actual(void);
int i915_scheduler_registrar_cpu(unsigned slot,unsigned apic_id);
#endif
