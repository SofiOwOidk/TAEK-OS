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
#include "compatibilidad/linux_i915/abi_contexto.h"
#include "compatibilidad/linux_i915/abi_simd.h"
#include "compatibilidad/linux_i915/abi_tls.h"
#include "compatibilidad/linux_i915/scheduler_prueba.h"
#include "compatibilidad/linux_i915/abi_irq.h"
#include "arquitectura/x86_64/fpu.h"

int intel_diagnostico_ring0_irq(void) {
    struct i915_irq_resultado r={0};int code=i915_irq_contexto_probar(&r);
    consola_imprimir("[INTEL_IRQ] RESULTADO=");consola_imprimir(code?"FAIL":"OK");
    consola_imprimir(" cpus=");consola_imprimir_dec(r.cpus);
    consola_imprimir(" entradas=");consola_imprimir_hex(r.entradas);
    consola_imprimir(" mask=");consola_imprimir_hex(r.fallos);
    consola_imprimir(" heap=");consola_imprimir_dec(r.heap_limpio);
    consola_imprimir(" admision=");consola_imprimir_hex(r.admision);
    consola_imprimir(" IF=");consola_imprimir_dec(r.if_antes);
    consola_imprimir("/");consola_imprimir_dec(r.if_despues);consola_imprimir_linea("");
    for(unsigned i=0;i<r.cpus && i<I915_MAX_CPU;i++){
        consola_imprimir("[INTEL_IRQ] CPU=");consola_imprimir_dec(i);
        consola_imprimir(" ciclos=");consola_imprimir_dec(r.cpu[i].ciclos);
        consola_imprimir(" iniciales=");consola_imprimir_dec(r.cpu[i].iniciales);
        consola_imprimir(" ipi=");consola_imprimir_dec(r.cpu[i].recibidos);
        consola_imprimir("/");consola_imprimir_dec(r.cpu[i].emitidos);
        consola_imprimir(" en_contexto=");consola_imprimir_dec(r.cpu[i].en_contexto);
        consola_imprimir(" mask=");consola_imprimir_hex(r.cpu[i].fallos);consola_imprimir_linea("");
    }
    consola_imprimir_linea("[INTEL_IRQ] IF por contexto, IPI APIC real y TLS coherente en handler; IRQ GPU/NMI/idle pendientes.");
    return code;
}
int intel_diagnostico_ring0_scheduler(void) {
    struct i915_smp_resultado r={0};int code=i915_scheduler_smp_probar(&r);
    consola_imprimir("[INTEL_SCHED] RESULTADO=");consola_imprimir(code?"FAIL":"OK");
    consola_imprimir(" cpus=");consola_imprimir_dec(r.cpus);
    consola_imprimir(" entradas=");consola_imprimir_hex(r.entradas);
    consola_imprimir(" mask=");consola_imprimir_hex(r.fallos);
    consola_imprimir(" heap=");consola_imprimir_dec(r.heap_limpio);consola_imprimir_linea("");
    for(unsigned i=0;i<r.cpus && i<I915_MAX_CPU;i++){
        consola_imprimir("[INTEL_SCHED] CPU=");consola_imprimir_dec(i);
        consola_imprimir(" ciclos=");consola_imprimir_dec(r.cpu[i].ciclos);
        consola_imprimir(" hijos=");consola_imprimir_dec(r.cpu[i].hijos);
        consola_imprimir(" iniciales=");consola_imprimir_dec(r.cpu[i].iniciales);
        consola_imprimir(" mask=");consola_imprimir_hex(r.cpu[i].fallos);
        consola_imprimir(" FS_RAIZ=");consola_imprimir_hex(r.cpu[i].fs_raiz);consola_imprimir_linea("");
    }
    consola_imprimir_linea("[INTEL_SCHED] Tareas reales BSP/AP; asignacion/liberacion desde BSP. IRQ, refs, idle/SMP completo y GPU pendientes.");
    return code;
}
int intel_diagnostico_ring0_tls(void) {
    struct i915_tls_resultado r={0};int code=i915_tls_contexto_probar(&r);
    consola_imprimir("[INTEL_TLS] CONTEXT_SWITCH=");consola_imprimir(code?"FAIL":"OK");
    consola_imprimir(" ciclos=");consola_imprimir_dec(r.ciclos);
    consola_imprimir(" mask=");consola_imprimir_hex(r.fallos);
    consola_imprimir(" hijos=");consola_imprimir_dec(r.hijo_ciclos[0]);
    consola_imprimir("/");consola_imprimir_dec(r.hijo_ciclos[1]);
    consola_imprimir(" iniciales=");consola_imprimir_dec(r.iniciales);
    consola_imprimir(" privados=");consola_imprimir_dec(r.privados);
    consola_imprimir(" heap=");consola_imprimir_dec(r.heap_limpio);
    consola_imprimir(" imagen=");consola_imprimir_dec(r.imagen_bytes);consola_imprimir_linea("");
    consola_imprimir_linea("[INTEL_TLS] TLS estatico ELF por tarea BSP; modulos TLS, glibc, SMP y GPU pendientes.");
    return code;
}
int intel_diagnostico_ring0_simd(void) {
    struct i915_simd_resultado r = {0};
    int code = i915_simd_contexto_probar(&r);
    consola_imprimir("[INTEL_SIMD] CONTEXT_SWITCH=");
    consola_imprimir(code ? "FAIL" : "OK");
    consola_imprimir(" ciclos="); consola_imprimir_dec(r.ciclos);
    consola_imprimir(" mask="); consola_imprimir_hex(r.fallos);
    consola_imprimir(" inicial="); consola_imprimir_dec(r.inicial_limpio);
    consola_imprimir(" SSE2="); consola_imprimir_dec(r.sse2);
    consola_imprimir(" YMM="); consola_imprimir_dec(x86_64_fpu_capacidades_cpu().ymm_habilitado);
    consola_imprimir_linea("");
    consola_imprimir_linea("[INTEL_SIMD] Prueba x87, MXCSR y XMM0..15; YMM/TLS/SMP y GPU pendientes.");
    return code;
}

int intel_diagnostico_ring0_abi(void) {
    struct i915_abi_resultado r = {0};
    int code = i915_abi_contexto_probar(&r);
    consola_imprimir("[INTEL_ABI] CONTEXT_SWITCH=");
    consola_imprimir(code ? "FAIL" : "OK");
    consola_imprimir(" ciclos="); consola_imprimir_dec(r.ciclos);
    consola_imprimir(" mask="); consola_imprimir_hex(r.fallos);
    consola_imprimir_linea("");
    consola_imprimir("[INTEL_ABI] RSP_MOD16 PADRE_ENTRADA="); consola_imprimir_dec(r.padre_entrada & 15);
    consola_imprimir(" LLAMADA="); consola_imprimir_dec(r.padre_llamada & 15);
    consola_imprimir(" TRAMPOLIN="); consola_imprimir_dec(r.hijo_entrada & 15);
    consola_imprimir(" FUNCION="); consola_imprimir_dec(r.funcion_entrada & 15);
    consola_imprimir_linea("");
    consola_imprimir("[INTEL_ABI] SWITCH PADRE_ENTRADA="); consola_imprimir_dec(r.padre.entrada_rsp & 15);
    consola_imprimir(" PADRE_RETORNO="); consola_imprimir_dec(r.padre.salida_rsp & 15);
    consola_imprimir(" HIJO_ENTRADA="); consola_imprimir_dec(r.hijo.entrada_rsp & 15);
    consola_imprimir(" HIJO_RETORNO="); consola_imprimir_dec(r.hijo.salida_rsp & 15);
    consola_imprimir_linea("");
    consola_imprimir_linea("[INTEL_ABI] Registros enteros; prueba x87/XMM con intel simd. YMM/TLS/SMP pendientes.");
    return code;
}
static int prueba_ring0_scheduler_contexto(void) {
    return intel_diagnostico_ring0_abi();
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
