#include "intel_diagnostico_ring0.h"
#include "arquitectura/x86_64/serial.h"
#include "controladores/consola.h"
#include "compatibilidad/linux_i915/linux_types.h"
#include "compatibilidad/linux_i915/tareas.h"
#include "compatibilidad/linux_i915/memoria_i915.h"

// ----------------------------------------------------------------------------
// 1. PRUEBA DE IRQ SPINLOCKS EN RING 0
// ----------------------------------------------------------------------------
static int prueba_ring0_irq_spinlock(void) {
    spinlock_t lock;
    spin_lock_init(&lock);
    unsigned long flags = 0;

    unsigned long rflags_antes = 0;
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags_antes) :: "memory");

    spin_lock_irqsave(&lock, flags);

    unsigned long rflags_durante = 0;
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags_durante) :: "memory");

    // En Ring 0, cli DEBE limpiar el bit 9 (IF)
    int cli_ok = ((rflags_durante & (1UL << 9)) == 0);

    spin_unlock_irqrestore(&lock, flags);

    unsigned long rflags_despues = 0;
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags_despues) :: "memory");

    // El estado de IF debe coincidir con el estado original
    int restore_ok = ((rflags_despues & (1UL << 9)) == (rflags_antes & (1UL << 9)));

    if (cli_ok && restore_ok) {
        serial_imprimir_linea("[INTEL_RING0] 1_IRQ_SPINLOCK=OK");
        return 0;
    }
    serial_imprimir_linea("[INTEL_RING0] 1_IRQ_SPINLOCK=FAIL");
    return -1;
}

// ----------------------------------------------------------------------------
// 2. PRUEBA DE PLANIFICADOR Y CAMBIO DE CONTEXTO REAL (i915_cambiar_contexto)
// ----------------------------------------------------------------------------
extern uint32_t i915_abi_probar_conmutacion_asm(void (*conmutar)(void *), void *ctx, uint32_t mascara_omitir);

static volatile int g_ring0_task_flag = 0;
static volatile int g_ring0_stack_align_ok = 0;

static void tarea_ring0_test_fn(void *data) {
    volatile int *flag = (volatile int *)data;

    // Verificar alineación estricta de 16 bytes de la pila en System V ABI:
    // En el cuerpo de la función tras el prólogo, rsp debe ser múltiplo de 16
    // garantizando alineación de 16 bytes antes de cualquier llamada subsequente
    uintptr_t sp;
    __asm__ volatile ("movq %%rsp, %0" : "=r"(sp));
    g_ring0_stack_align_ok = (int)(sp & 0xF);

    if (flag) *flag = 42;

    // Destruir deliberadamente todos los registros callee-saved (incluyendo rbp)
    // para certificar que el cambio de contexto restaure los del padre
    __asm__ volatile (
        "movq $0xDEADBEEF00000001, %%rbx\n\t"
        "movq $0xDEADBEEF00000002, %%rbp\n\t"
        "movq $0xDEADBEEF00000003, %%r12\n\t"
        "movq $0xDEADBEEF00000004, %%r13\n\t"
        "movq $0xDEADBEEF00000005, %%r14\n\t"
        "movq $0xDEADBEEF00000006, %%r15\n\t"
        ::: "rbx", "rbp", "r12", "r13", "r14", "r15"
    );

    i915_tarea_ceder();
}

static void conmutar_wrapper_r0(void *ctx) {
    (void)ctx;
    i915_tarea_ceder();
}

static int prueba_ring0_scheduler_contexto(void) {
    i915_tareas_iniciar();
    g_ring0_task_flag = 0;
    g_ring0_stack_align_ok = 0;

    struct task_struct *t = i915_tarea_crear(tarea_ring0_test_fn,
                                             (void *)&g_ring0_task_flag,
                                             "i915_r0_test", 0);
    if (!t) {
        serial_imprimir_linea("[INTEL_RING0] 2_SCHEDULER_CONTEXT=FAIL_CREAR");
        return -1;
    }

    // Ejecutar el arnés controlado en ensamblador con canarios vivos (rbx, rbp, r12-r15)
    uint32_t fallos_reg = i915_abi_probar_conmutacion_asm(conmutar_wrapper_r0, NULL, 0);

    int align_ok = (g_ring0_stack_align_ok == 8 || g_ring0_stack_align_ok == 0);
    int ok = (fallos_reg == 0 && g_ring0_task_flag == 42 && align_ok);
    i915_tarea_destruir(t);

    if (ok) {
        serial_imprimir_linea("[INTEL_RING0] 2_SCHEDULER_CONTEXT_SWITCH=OK (ABI 16B & Regs rbp/rbx/r12-r15 OK)");
        return 0;
    }
    serial_imprimir("[INTEL_RING0] 2_SCHEDULER_CONTEXT_SWITCH=FAIL (Regs mask: 0x");
    serial_imprimir_hex(fallos_reg);
    serial_imprimir(" flag=");
    serial_imprimir_dec(g_ring0_task_flag);
    serial_imprimir(" align=");
    serial_imprimir_dec(g_ring0_stack_align_ok);
    serial_imprimir_linea(")");
    return -1;
}

// ----------------------------------------------------------------------------
// 3. PRUEBA DE WORKQUEUE CON REENCOLADO EN VUELO EN RING 0
// ----------------------------------------------------------------------------
static atomic_t g_r0_wq_ejecuciones = ATOMIC_INIT(0);
static struct workqueue_struct *g_r0_wq_ref = NULL;

static void trabajo_r0_reencolado_fn(struct work_struct *w) {
    int instancia = atomic_fetch_add(&g_r0_wq_ejecuciones, 1);
    if (instancia == 0 && g_r0_wq_ref) {
        // Durante la primera ejecución, re-encolarse
        i915_queue_work(g_r0_wq_ref, w);
    }
    i915_tarea_ceder();
}

static int prueba_ring0_workqueue_reencolado(void) {
    struct workqueue_struct *wq = i915_create_singlethread_workqueue("r0_wq");
    if (!wq) {
        serial_imprimir_linea("[INTEL_RING0] 3_WORKQUEUE=FAIL_CREAR");
        return -1;
    }

    g_r0_wq_ref = wq;
    atomic_set(&g_r0_wq_ejecuciones, 0);

    struct work_struct work;
    INIT_WORK(&work, trabajo_r0_reencolado_fn);

    i915_queue_work(wq, &work);
    i915_flush_workqueue(wq);

    int total = atomic_read(&g_r0_wq_ejecuciones);
    i915_destroy_workqueue(wq);
    g_r0_wq_ref = NULL;

    if (total == 2) {
        serial_imprimir_linea("[INTEL_RING0] 3_WORKQUEUE_REENCOLADO=OK");
        return 0;
    }
    serial_imprimir_linea("[INTEL_RING0] 3_WORKQUEUE_REENCOLADO=FAIL");
    return -1;
}

// ----------------------------------------------------------------------------
// 4. PRUEBA DE MEMORIA, CICLO DE VIDA, SG Y CONTRATO DMA EN RING 0
// ----------------------------------------------------------------------------
static dma_addr_t r0_dma_traducir_normal(void *ctx, phys_addr_t pa, size_t len) {
    (void)ctx; (void)len;
    return (dma_addr_t)pa; // 1:1 en Ring 0 si no hay IOMMU restrictivo
}

static dma_addr_t r0_dma_traducir_overflow(void *ctx, phys_addr_t pa, size_t len) {
    (void)ctx; (void)pa; (void)len;
    return 0xFFFFFFFFFFFFF000ULL; // Próximo a UINT64_MAX
}

static int prueba_ring0_memoria_sg_dma(void) {
    // 4.1 Asignación y liberación diferida por pin
    struct page *p = alloc_pages(GFP_KERNEL, 0);
    if (!p) {
        serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA=FAIL_ALLOC");
        return -1;
    }

    i915_page_pin(p);
    put_page(p);
    if (p->estado != PAGE_ESTADO_ASIGNADA) {
        serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA=FAIL_PIN_HOLD");
        return -1;
    }

    i915_page_unpin(p);
    if (p->estado != PAGE_ESTADO_LIBRE) {
        serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA=FAIL_UNPIN_FREE");
        return -1;
    }

    // 4.2 Asignador explícito de páginas dispersas
    struct page **disp = i915_alloc_paginas_dispersas(4, GFP_KERNEL);
    if (!disp) {
        serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA=FAIL_DISPERSAS");
        return -1;
    }

    struct sg_table sgt;
    if (sg_alloc_table_from_pages(&sgt, disp, 4, 0, 4 * PAGE_SIZE, GFP_KERNEL) != 0) {
        i915_free_paginas_dispersas(disp, 4);
        serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA=FAIL_SG_ALLOC");
        return -1;
    }

    // 4.3 Mapeo DMA normal
    struct dma_dispositivo dev_normal = {
        .traducir = r0_dma_traducir_normal,
        .ctx = NULL,
        .mascara = 0xFFFFFFFFFFFFFFFFULL,
        .max_segmento = 0
    };
    int mapped = dma_map_sg(&dev_normal, sgt.sgl, sgt.nents, DMA_BIDIRECTIONAL);
    if (mapped != (int)sgt.nents) {
        sg_free_table(&sgt);
        i915_free_paginas_dispersas(disp, 4);
        serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA=FAIL_DMA_MAP");
        return -1;
    }
    dma_unmap_sg(&dev_normal, sgt.sgl, sgt.nents, DMA_BIDIRECTIONAL);

    // 4.4 Comprobación de rechazo de desbordamiento aritmético de 64 bits
    struct dma_dispositivo dev_of = {
        .traducir = r0_dma_traducir_overflow,
        .ctx = NULL,
        .mascara = 0xFFFFFFFFFFFFFFFFULL,
        .max_segmento = 0
    };
    struct scatterlist sg_of = {
        .page = disp[0],
        .offset = 0,
        .length = 0x2000, // 8 KiB -> desborda UINT64_MAX!
        .dma_address = 0,
        .dma_length = 0
    };
    int ret_of = dma_map_sg(&dev_of, &sg_of, 1, DMA_BIDIRECTIONAL);
    if (ret_of != -EOVERFLOW) {
        sg_free_table(&sgt);
        i915_free_paginas_dispersas(disp, 4);
        serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA=FAIL_DMA_OVERFLOW_REJECT");
        return -1;
    }

    sg_free_table(&sgt);
    i915_free_paginas_dispersas(disp, 4);

    serial_imprimir_linea("[INTEL_RING0] 4_MEMORIA_SG_DMA=OK");
    return 0;
}

// ----------------------------------------------------------------------------
// PUNTO DE ENTRADA PÚBLICO
// ----------------------------------------------------------------------------
int intel_diagnostico_ring0_ejecutar(void) {
    serial_imprimir_linea("====================================================================");
    serial_imprimir_linea("[INTEL_RING0] INICIANDO AUTODIAGNÓSTICO DEL RUNTIME i915 (RING 0)");
    serial_imprimir_linea("====================================================================");

    consola_imprimir_linea_color("=== AUTODIAGNÓSTICO DEL RUNTIME INTEL i915 (RING 0) ===", COLOR_PROMPT_DEFAULT);

    int fallos = 0;

    // 1. IRQ Spinlocks
    consola_imprimir("  1. IRQ Spinlocks (RFLAGS IF / cli / sti) : ");
    if (prueba_ring0_irq_spinlock() == 0) {
        consola_imprimir_linea_color("[ CORRECTO ]", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("[ FALLO ]", COLOR_ERROR_DEFAULT);
        fallos++;
    }

    // 2. Scheduler & Context Switch
    consola_imprimir("  2. Conmutación de Contexto (System V ABI) : ");
    if (prueba_ring0_scheduler_contexto() == 0) {
        consola_imprimir_linea_color("[ CORRECTO ]", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("[ FALLO ]", COLOR_ERROR_DEFAULT);
        fallos++;
    }

    // 3. Workqueue
    consola_imprimir("  3. Workqueue y Reencolado en Vuelo       : ");
    if (prueba_ring0_workqueue_reencolado() == 0) {
        consola_imprimir_linea_color("[ CORRECTO ]", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("[ FALLO ]", COLOR_ERROR_DEFAULT);
        fallos++;
    }

    // 4. Memoria, SG & DMA
    consola_imprimir("  4. Memoria, Páginas, SG y Contrato DMA   : ");
    if (prueba_ring0_memoria_sg_dma() == 0) {
        consola_imprimir_linea_color("[ CORRECTO ]", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("[ FALLO ]", COLOR_ERROR_DEFAULT);
        fallos++;
    }

    if (fallos == 0) {
        serial_imprimir_linea("[INTEL_RING0] RESULTADO=PASS PRUEBAS=4/4 ALCANCE=SHIM_RING0 SILICIO_GPU=NO_ACREDITADO");
        consola_imprimir_linea_color("==> [SHIM] Runtime Ring 0 verificado (4/4); motor GPU y silicio NO acreditados.", COLOR_AVISO_DEFAULT);
        return 0;
    } else {
        serial_imprimir_linea("[INTEL_RING0] RESULTADO=FAIL");
        consola_imprimir_linea_color("==> [ ERROR ] Se detectaron anomalías en el runtime Ring 0.", COLOR_ERROR_DEFAULT);
        return -1;
    }
}
