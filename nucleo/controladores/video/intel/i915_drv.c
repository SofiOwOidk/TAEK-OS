#include "i915_drv.h"
#include "arquitectura/x86_64/serial.h"
#include "arquitectura/x86_64/pci.h"
#include "controladores/consola.h"
#include "controladores/pantalla.h"
#include "base/paginacion.h"
#include "base/tiempo.h"
#include "compatibilidad/linux_i915/memoria_i915.h"
#include "i915_gem.h"
#include "i915_vcs.h"

static struct i915_dispositivo g_i915;
static int g_iniciado = 0;

static inline void mb(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

uint32_t i915_leer_mmio_32(uint32_t offset) {
    if (!g_iniciado || offset >= g_i915.mmio_tamano) return 0xFFFFFFFFU;
    volatile uint32_t *reg = (volatile uint32_t *)(g_i915.mmio_base_virt + offset);
    return *reg;
}

void i915_escribir_mmio_32(uint32_t offset, uint32_t valor) {
    if (!g_iniciado || offset >= g_i915.mmio_tamano) return;
    volatile uint32_t *reg = (volatile uint32_t *)(g_i915.mmio_base_virt + offset);
    *reg = valor;
    mb();
}

const struct i915_dispositivo *i915_obtener_dispositivo(void) {
    // Getter sin efectos: no inicia el controlador.
    return g_iniciado ? &g_i915 : NULL;
}

// Acreditación exigida antes de CUALQUIER mutación de hardware (mapeo MMIO,
// Bus Mastering, GGTT). Devuelve 0 sólo con plataforma, DMA y ownership
// demostrados; hoy ninguna lo está, por lo que la init activa queda bloqueada.
static int i915_acreditacion(int *motivo) {
    if (!g_i915.info.detectado) {
        if (motivo) *motivo = I915_BLOQUEO_SIN_PLATAFORMA;
        return -1;
    }
    if (!g_i915.info.iommu_inventario_disponible) {
        if (motivo) *motivo = I915_BLOQUEO_INVENTARIO_IOMMU;
        return -1;
    }
    if (g_i915.info.iommu_dmar_presente && g_i915.info.iommu_traduccion_activa) {
        if (motivo) *motivo = I915_BLOQUEO_VTD_TRADUCCION;
        return -1;
    }
    if (!g_i915.platform_acreditada) {
        if (motivo) *motivo = I915_BLOQUEO_PLATAFORMA_NO_ACREDITADA;
        return -1;
    }
    if (!g_i915.dma_acreditado) {
        if (motivo) *motivo = I915_BLOQUEO_DMA_NO_ACREDITADO;
        return -1;
    }
    if (!g_i915.ggtt_ownership) {
        if (motivo) *motivo = I915_BLOQUEO_SIN_OWNERSHIP_GGTT;
        return -1;
    }
    return 0;
}

static const char *i915_motivo_texto(int motivo) {
    switch (motivo) {
        case I915_BLOQUEO_INVENTARIO_PCI: return "inventario PCI no disponible";
        case I915_BLOQUEO_INVENTARIO_IOMMU: return "inventario IOMMU desconocido; DMA no acreditado";
        case I915_BLOQUEO_SIN_PLATAFORMA:           return "plataforma no identificada en PCI";
        case I915_BLOQUEO_VTD_TRADUCCION:           return "VT-d con traduccion activa (DMA no 1:1)";
        case I915_BLOQUEO_PLATAFORMA_NO_ACREDITADA: return "plataforma no acreditada";
        case I915_BLOQUEO_DMA_NO_ACREDITADO:        return "DMA 1:1 no acreditado";
        case I915_BLOQUEO_SIN_OWNERSHIP_GGTT:       return "ownership formal de GGTT no adquirido";
        default:                                    return "motivo desconocido";
    }
}

int i915_driver_iniciar(void) {
    if (g_iniciado) return 0;

    for (size_t i = 0; i < sizeof(g_i915); i++) {
        ((uint8_t *)&g_i915)[i] = 0;
    }
    g_i915.estado = I915_ESTADO_NO_INICIADO;

    if (!pci_esta_iniciado()) {
        g_i915.estado = I915_ESTADO_BLOQUEADO;
        g_i915.motivo_bloqueo = I915_BLOQUEO_INVENTARIO_PCI;
        serial_imprimir_linea("[I915_DRV] inventario PCI no disponible; init activa bloqueada.");
        return -2;
    }

    // 1. Detección pasiva en PCI (el bus ya se enumeró en el arranque).
    int n = intel_inventario_detectar(&g_i915.info, 1);
    if (n <= 0 || !g_i915.info.detectado) {
        serial_imprimir_linea("[I915_DRV] No se detectó dispositivo gráfico Intel en bus PCI.");
        g_i915.estado = I915_ESTADO_FALLO;
        g_i915.motivo_bloqueo = I915_BLOQUEO_SIN_PLATAFORMA;
        return -1;
    }

    // 2. Bloqueo de la inicialización activa hasta acreditar plataforma, DMA
    //    y ownership. Mientras no se acrediten, no se toca hardware.
    int motivo = I915_BLOQUEO_NINGUNO;
    if (i915_acreditacion(&motivo) != 0) {
        g_i915.estado = I915_ESTADO_BLOQUEADO;
        g_i915.motivo_bloqueo = motivo;
        serial_imprimir("[I915_DRV] Inicializacion activa BLOQUEADA: ");
        serial_imprimir_linea(i915_motivo_texto(motivo));
        serial_imprimir_linea("[I915_DRV] No se mapea MMIO, no se activa Bus Mastering, no se toca GGTT.");
        return -2;
    }

    if (!g_i915.info.barras[0].valida || g_i915.info.barras[0].tamano == 0) {
        serial_imprimir_linea("[I915_DRV] ERROR: BAR0 MMIO inválido o de tamaño 0.");
        g_i915.estado = I915_ESTADO_FALLO;
        return -1;
    }

    g_i915.mmio_tamano = g_i915.info.barras[0].tamano;
    if (g_i915.mmio_tamano > 16ULL * 1024ULL * 1024ULL) {
        g_i915.mmio_tamano = 16ULL * 1024ULL * 1024ULL; // Limitar a 16 MB normativos
    }

    // 2. Mapeo MMIO BAR0 en espacio virtual soberano
    g_i915.mmio_base_virt = I915_MMIO_VIRTUAL_BASE;
    for (uint64_t off = 0; off < g_i915.mmio_tamano; off += 4096) {
        int ret = paginacion_mapear(g_i915.mmio_base_virt + off,
                                    g_i915.info.barras[0].dir_base + off,
                                    PAGINA_ATRIBUTOS_MMIO);
        if (ret != 0) {
            serial_imprimir_linea("[I915_DRV] ERROR: Fallo al mapear MMIO BAR0 en paginación.");
            g_i915.estado = I915_ESTADO_FALLO;
            return -1;
        }
    }

    // 3. Estado efectivo de Intel VT-d (se llega aquí sólo con DMA acreditado).
    g_i915.vtd_passthrough_confirmado = 0;
    serial_imprimir_linea("[I915_DRV] Estado global VT-d no acredita direccionamiento DMA por dispositivo.");

    // 4. Activación de PCI Bus Mastering con readback verificable.
    struct dispositivo_pci dev_pci;
    if (pci_buscar_dispositivo(g_i915.info.id_proveedor, g_i915.info.id_dispositivo, &dev_pci) == 0) {
        pci_activar_bus_master(&dev_pci);
        g_i915.pci_comando_leido = pci_leer_config_16(dev_pci.bus, dev_pci.ranura, dev_pci.funcion, 0x04);
        serial_imprimir("[I915_DRV] PCI comando readback=0x");
        serial_imprimir_hex(g_i915.pci_comando_leido);
        serial_imprimir_linea((g_i915.pci_comando_leido & 0x0004) ?
            " (Bus Master=1 verificado)" : " (Bus Master=0: NO activado)");
    } else {
        serial_imprimir_linea("[I915_DRV] ERROR: no se halló el dispositivo para Bus Mastering.");
    }

    // 5. Configuración de GGTT y PRESERVACIÓN ESTRICTA DEL FRAMEBUFFER GOP
    g_i915.ggtt_ptes_virt = g_i915.mmio_base_virt + GEN8_GGTT_PTE_OFFSET;
    uint64_t apertura_tam = g_i915.info.barras[2].valida ? g_i915.info.barras[2].tamano : (256ULL * 1024 * 1024);
    g_i915.ggtt_total_ptes = (uint32_t)(apertura_tam / 4096);

    if (g_i915.info.gop_en_apertura_intel) {
        uint64_t gop_tam = g_i915.info.gop_fb_tamano ? g_i915.info.gop_fb_tamano : (1920ULL * 1080 * 4);
        g_i915.ggtt_gop_reservadas = (uint32_t)((gop_tam + 4095) / 4096);
        // Margen de seguridad: iniciar asignaciones 1024 páginas por encima del Framebuffer
        g_i915.ggtt_proxima_libre = g_i915.ggtt_gop_reservadas + 1024;
        serial_imprimir("[I915_DRV] Apertura BAR2: reserva declarada de ");
        serial_imprimir_dec(g_i915.ggtt_gop_reservadas);
        serial_imprimir_linea(" PTEs sobre el framebuffer GOP (no programadas en este paso).");
    } else {
        g_i915.ggtt_gop_reservadas = 0;
        g_i915.ggtt_proxima_libre = 1024;
    }

#ifdef TAEK_I915_HW
    // [EXPERIMENTAL / NO VALIDADO] GEM + VCS propios de TAEK. Excluido del build
    // estable hasta acreditar GGTT/DMA/energia (plan maestro M11-M15).
    i915_gem_iniciar((uint64_t)g_i915.ggtt_proxima_libre * 4096ULL, (uint64_t)g_i915.ggtt_total_ptes * 4096ULL);
    if (i915_vcs_motor_iniciar() == 0) {
        g_i915.vcs_activo = 1;
        g_i915.estado = I915_ESTADO_VCS_LISTO;
    } else {
        g_i915.estado = I915_ESTADO_MMIO_LISTO;
    }
#else
    // Build estable: MMIO mapeado, GGTT NO programada, submission bloqueada.
    g_i915.estado = I915_ESTADO_MMIO_LISTO;
#endif
    g_iniciado = 1;

    serial_imprimir("[I915_DRV] MMIO mapeado y subsistemas listos; estado=");
    serial_imprimir_dec((uint64_t)g_i915.estado);
    serial_imprimir(" ggtt_ownership=");
    serial_imprimir_dec((uint64_t)g_i915.ggtt_ownership);
    serial_imprimir(" device=0x");
    serial_imprimir_hex(g_i915.info.id_dispositivo);
    serial_imprimir_linea("");
    return 0;
}

int i915_vcs_primer_trabajo(uint32_t firma_esperada, uint32_t *resultado_out) {
    if (g_i915.estado == I915_ESTADO_BLOQUEADO || !g_i915.submission_habilitada) {
        if (resultado_out) *resultado_out = 0;
        serial_imprimir_linea("--------------------------------------------------------------------");
        serial_imprimir_linea("[I915_VCS] AVISO: Envío de trabajos al motor VCS deshabilitado temporalmente.");
        serial_imprimir_linea("[I915_VCS] Requisitos previos pendientes antes de submission:");
        serial_imprimir_linea("[I915_VCS]  1. Ownership formal de GGTT (no pisar apertura BAR2 GOP)");
        serial_imprimir_linea("[I915_VCS]  2. Verificación de direccionamiento DMA y tablas VT-d");
        serial_imprimir_linea("[I915_VCS]  3. Selección de plataforma y protocolo de recuperación VCS");
        serial_imprimir_linea("--------------------------------------------------------------------");
        return -38; /* -ENOSYS: Función no implementada/deshabilitada temporalmente */
    }

#ifdef TAEK_I915_HW
    return i915_vcs_ejecutar_primer_trabajo(firma_esperada, resultado_out);
#else
    (void)firma_esperada;
    if (resultado_out) *resultado_out = 0;
    serial_imprimir_linea("[I915_VCS] Submission no compilada (TAEK_I915_HW=0).");
    return -38; /* -ENOSYS */
#endif
}

void i915_imprimir_estado(void) {
    // CONSULTA SIN EFECTOS: no dispara la enumeración PCI ni la inicialización.
    consola_imprimir_linea_color("=== ESTADO DEL CONTROLADOR INTEL i915 (CONSULTA SIN EFECTOS) ===", COLOR_PROMPT_DEFAULT);
    if (!pci_esta_iniciado()) {
        consola_imprimir_linea_color("  [INFO] PCI no inicializado: esta consulta no dispara enumeración.", COLOR_AVISO_DEFAULT);
        return;
    }

    struct intel_gpu_info info;
    int n = intel_inventario_detectar(&info, 1);

    if (n <= 0 || !info.detectado) {
        consola_imprimir_linea_color("  Dispositivo         : No detectado en el bus PCI", COLOR_AVISO_DEFAULT);
        return;
    }

    consola_imprimir("  Dispositivo         : ");
    consola_imprimir_hex(info.id_dispositivo);
    consola_imprimir_linea(" [DETECTADO EN BUS PCI]");

    consola_imprimir("  Ubicación PCI       : ");
    consola_imprimir_dec(info.bus); consola_imprimir(":");
    consola_imprimir_dec(info.ranura); consola_imprimir(".");
    consola_imprimir_dec(info.funcion);
    consola_imprimir(" | Rev: "); consola_imprimir_hex(info.revision);
    consola_imprimir(" | Subsys: "); consola_imprimir_hex((uint64_t)info.sub_proveedor << 16 | info.sub_dispositivo);
    consola_imprimir_linea("");

    if (info.barras[0].valida) {
        consola_imprimir("  BAR0 (MMIO Regs)    : Base física ");
        consola_imprimir_hex(info.barras[0].dir_base);
        consola_imprimir(" (");
        consola_imprimir_dec(info.barras[0].tamano / (1024 * 1024));
        consola_imprimir_linea(" MB - Sin mapear en VMM)");
    }

    if (info.barras[2].valida) {
        consola_imprimir("  BAR2 (Apertura)     : Base física ");
        consola_imprimir_hex(info.barras[2].dir_base);
        consola_imprimir(" (");
        consola_imprimir_dec(info.barras[2].tamano / (1024 * 1024));
        consola_imprimir_linea(" MB prefetchable)");
    }

    consola_imprimir("  Framebuffer GOP     : Base física ");
    consola_imprimir_hex(info.gop_fb_fisico);
    consola_imprimir(" -> ");
    if (info.gop_en_apertura_intel) {
        consola_imprimir_linea_color("[COINCIDE CON BAR2 - PRESERVACIÓN REQUERIDA]", COLOR_AVISO_DEFAULT);
    } else {
        consola_imprimir_linea_color("[EXTERNO O RAM SISTEMA]", COLOR_USUARIO_DEFAULT);
    }

    consola_imprimir("  Intel VT-d          : ");
    if (!info.iommu_inventario_disponible) {
        consola_imprimir_linea_color("Inventario desconocido (DMA no acreditado)", COLOR_AVISO_DEFAULT);
    } else if (info.iommu_dmar_presente) {
        consola_imprimir_linea_color(info.iommu_traduccion_activa ? "Traducción Activa (Requiere mapeo IOMMU)" : "Sin traducción activa (1:1 no acreditado)", COLOR_AVISO_DEFAULT);
    } else {
        consola_imprimir_linea_color("Sin tabla DMAR detectada (1:1 no acreditado)", COLOR_AVISO_DEFAULT);
    }

    consola_imprimir("  Modo de Operación   : ");
    consola_imprimir_linea_color("Solo lectura PCI (init activa bloqueada hasta acreditación)", COLOR_AVISO_DEFAULT);

    consola_imprimir("  Envío de Trabajos   : ");
    consola_imprimir_linea_color("DESHABILITADO (Pendiente: GGTT ownership, DMA y recuperación)", COLOR_AVISO_DEFAULT);

    consola_imprimir("  Estado del driver   : ");
    if (g_i915.estado == I915_ESTADO_BLOQUEADO) {
        consola_imprimir_linea_color("BLOQUEADO (falta acreditacion de plataforma/DMA/ownership)", COLOR_AVISO_DEFAULT);
    } else if (g_i915.estado == I915_ESTADO_NO_INICIADO) {
        consola_imprimir_linea_color("NO INICIADO (sin efectos activos)", COLOR_AVISO_DEFAULT);
    } else {
        consola_imprimir("estado="); consola_imprimir_dec((uint64_t)g_i915.estado);
        consola_imprimir_linea("");
    }
}
