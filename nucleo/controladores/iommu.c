#include "iommu.h"
#include "../base/memoria.h"
#include "../base/paginacion.h"
#include "../base/huevo.h"
#include "../arquitectura/x86_64/serial.h"

#define LIMINE_API_REVISION 2
#include "../../boot/limine/limine.h"

// Petición de puntero RSDP (Root System Description Pointer) entregado por Limine
__attribute__((used, section(".requests")))
static volatile struct limine_rsdp_request g_peticion_rsdp = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0
};

// Estructura de cabecera estándar de tabla ACPI
struct acpi_encabezado_tabla {
    char     firma[4];            // "DMAR", "XSDT", "RSDT", etc.
    uint32_t longitud;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

// Estructura de puntero raíz ACPI (RSDP)
struct acpi_rsdp {
    char     firma[8];            // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;            // 0 = ACPI 1.0, 2 = ACPI 2.0+
    uint32_t rsdt_dir;
    // Campos extendidos ACPI 2.0+
    uint32_t longitud;
    uint64_t xsdt_dir;
    uint8_t  checksum_extendido;
    uint8_t  reservado[3];
} __attribute__((packed));

// Estructura de cabecera de tabla DMAR de Intel VT-d
struct acpi_dmar_encabezado {
    struct acpi_encabezado_tabla sdt;
    uint8_t  haw;                 // Ancho de dirección del host (Haw)
    uint8_t  banderas;            // bit 0: INTR_REMAP, bit 1: X2APIC_OPT_OUT
    uint8_t  reservado[10];
} __attribute__((packed));

// Subestructura genérica DMAR
struct dmar_subestructura {
    uint16_t tipo;
    uint16_t longitud;
} __attribute__((packed));

// Subestructura DRHD (DMA Remapping Hardware Unit Definition)
struct dmar_drhd_crudo {
    uint16_t tipo;                // 0 = DRHD
    uint16_t longitud;
    uint8_t  banderas;            // bit 0: INCLUDE_PCI_ALL
    uint8_t  reservado;
    uint16_t segmento_pci;
    uint64_t dir_base_mmio;
} __attribute__((packed));

// Subestructura RMRR (Reserved Memory Region Reporting)
struct dmar_rmrr_crudo {
    uint16_t tipo;                // 1 = RMRR
    uint16_t longitud;
    uint16_t reservado;
    uint16_t segmento_pci;
    uint64_t dir_base_fisica;
    uint64_t dir_limite_fisica;
} __attribute__((packed));

#define IOMMU_MMIO_BASE_VIRT 0xFFFFFE0002000000ULL
#define IOMMU_MMIO_PASO      0x0000000000100000ULL // 1 MiB de ventana por unidad DRHD

static iommu_estado_t g_estado_iommu;
static int            g_iommu_iniciado = 0;

#define ACPI_VENTANA_VIRT_BASE 0xFFFFFE0003000000ULL

static uint64_t g_acpi_cursor_virt = ACPI_VENTANA_VIRT_BASE;

static void *acpi_mapear_memoria_fisica(uint64_t phys_addr, uint64_t bytes) {
    if (phys_addr == 0 || bytes == 0) return NULL;

    // Si ya es dirección virtual canonical alta (>= 0xFFFF800000000000)
    if (phys_addr >= 0xFFFF800000000000ULL) {
        return (void *)phys_addr;
    }

    uint64_t hhdm = memoria_obtener_hhdm_offset();
    uint64_t virt_hhdm = phys_addr + hhdm;

    // Si ya está mapeada en el HHDM de Limine y es válida
    if (paginacion_obtener_fisica(virt_hhdm) == phys_addr) {
        return (void *)virt_hhdm;
    }

    // No mapeada en HHDM (memoria ACPI Reclaimable / NVS de UEFI fuera de RAM usable)
    uint64_t phys_alineada = phys_addr & ~0xFFFULL;
    uint64_t offset_pagina = phys_addr & 0xFFFULL;
    uint64_t pags = (offset_pagina + bytes + 4095ULL) / 4096ULL;

    uint64_t virt_base = g_acpi_cursor_virt;
    g_acpi_cursor_virt += (pags * 4096ULL);

    for (uint64_t p = 0; p < pags; p++) {
        paginacion_mapear(virt_base + (p * 4096ULL), phys_alineada + (p * 4096ULL), PAGINA_ATRIBUTOS_KERNEL);
    }

    return (void *)(virt_base + offset_pagina);
}

static int verificar_checksum_acpi(const void *datos, uint32_t tamano) {
    const uint8_t *p = (const uint8_t *)datos;
    uint8_t suma = 0;
    for (uint32_t i = 0; i < tamano; i++) {
        suma += p[i];
    }
    return (suma == 0);
}

int iommu_iniciar(void) {
    if (g_iommu_iniciado) return 0;

    for (int i = 0; i < (int)sizeof(g_estado_iommu); i++) {
        ((uint8_t *)&g_estado_iommu)[i] = 0;
    }

    g_estado_iommu.modo_operacion = IOMMU_MODO_PASSTHROUGH_DIRECTO;

    if (g_peticion_rsdp.response == NULL || g_peticion_rsdp.response->address == 0) {
        serial_imprimir_linea("[IOMMU] Limine no reportó RSDP. Asumiendo modo DMA Directo Físico 1:1.");
        g_iommu_iniciado = 1;
        return 0;
    }

    uint64_t rsdp_addr = g_peticion_rsdp.response->address;
    struct acpi_rsdp *rsdp = (struct acpi_rsdp *)acpi_mapear_memoria_fisica(rsdp_addr, sizeof(struct acpi_rsdp));
    if (!rsdp) {
        serial_imprimir_linea("[IOMMU] No fue posible mapear estructura RSDP. Operando en modo DMA Directo.");
        g_iommu_iniciado = 1;
        return 0;
    }

    // Validar firma "RSD PTR "
    if (rsdp->firma[0] != 'R' || rsdp->firma[1] != 'S' || rsdp->firma[2] != 'D' ||
        rsdp->firma[3] != ' ' || rsdp->firma[4] != 'P' || rsdp->firma[5] != 'T' ||
        rsdp->firma[6] != 'R' || rsdp->firma[7] != ' ') {
        serial_imprimir_linea("[IOMMU] Firma RSDP inválida. Operando en modo DMA Directo.");
        g_iommu_iniciado = 1;
        return 0;
    }

    if (!verificar_checksum_acpi(rsdp, 20)) {
        serial_imprimir_linea("[IOMMU] Checksum RSDP inválido. Operando en modo DMA Directo.");
        g_iommu_iniciado = 1;
        return 0;
    }

    struct acpi_dmar_encabezado *tabla_dmar = NULL;

    // Buscar tabla DMAR mediante XSDT (ACPI 2.0+) o RSDT (ACPI 1.0)
    if (rsdp->revision >= 2 && rsdp->xsdt_dir != 0) {
        struct acpi_encabezado_tabla *xsdt_hdr = (struct acpi_encabezado_tabla *)acpi_mapear_memoria_fisica(rsdp->xsdt_dir, sizeof(struct acpi_encabezado_tabla));
        if (xsdt_hdr && xsdt_hdr->firma[0] == 'X' && xsdt_hdr->firma[1] == 'S' && xsdt_hdr->firma[2] == 'D' && xsdt_hdr->firma[3] == 'T') {
            struct acpi_encabezado_tabla *xsdt = (struct acpi_encabezado_tabla *)acpi_mapear_memoria_fisica(rsdp->xsdt_dir, xsdt_hdr->longitud);
            uint32_t num_entradas = (xsdt->longitud - sizeof(struct acpi_encabezado_tabla)) / sizeof(uint64_t);
            uint64_t *punteros = (uint64_t *)((uint8_t *)xsdt + sizeof(struct acpi_encabezado_tabla));

            for (uint32_t i = 0; i < num_entradas; i++) {
                struct acpi_encabezado_tabla *thdr = (struct acpi_encabezado_tabla *)acpi_mapear_memoria_fisica(punteros[i], sizeof(struct acpi_encabezado_tabla));
                if (thdr && thdr->firma[0] == 'D' && thdr->firma[1] == 'M' &&
                    thdr->firma[2] == 'A' && thdr->firma[3] == 'R') {
                    tabla_dmar = (struct acpi_dmar_encabezado *)acpi_mapear_memoria_fisica(punteros[i], thdr->longitud);
                    break;
                }
            }
        }
    }

    if (!tabla_dmar && rsdp->rsdt_dir != 0) {
        struct acpi_encabezado_tabla *rsdt_hdr = (struct acpi_encabezado_tabla *)acpi_mapear_memoria_fisica((uint64_t)rsdp->rsdt_dir, sizeof(struct acpi_encabezado_tabla));
        if (rsdt_hdr && rsdt_hdr->firma[0] == 'R' && rsdt_hdr->firma[1] == 'S' && rsdt_hdr->firma[2] == 'D' && rsdt_hdr->firma[3] == 'T') {
            struct acpi_encabezado_tabla *rsdt = (struct acpi_encabezado_tabla *)acpi_mapear_memoria_fisica((uint64_t)rsdp->rsdt_dir, rsdt_hdr->longitud);
            uint32_t num_entradas = (rsdt->longitud - sizeof(struct acpi_encabezado_tabla)) / sizeof(uint32_t);
            uint32_t *punteros = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_encabezado_tabla));

            for (uint32_t i = 0; i < num_entradas; i++) {
                struct acpi_encabezado_tabla *thdr = (struct acpi_encabezado_tabla *)acpi_mapear_memoria_fisica((uint64_t)punteros[i], sizeof(struct acpi_encabezado_tabla));
                if (thdr && thdr->firma[0] == 'D' && thdr->firma[1] == 'M' &&
                    thdr->firma[2] == 'A' && thdr->firma[3] == 'R') {
                    tabla_dmar = (struct acpi_dmar_encabezado *)acpi_mapear_memoria_fisica((uint64_t)punteros[i], thdr->longitud);
                    break;
                }
            }
        }
    }

    if (!tabla_dmar) {
        serial_imprimir_linea("[IOMMU] Tabla ACPI DMAR no presente (Plataforma opera en DMA Directo Físico 1:1)");
        g_estado_iommu.tabla_dmar_detectada = 0;
        g_estado_iommu.modo_operacion = IOMMU_MODO_PASSTHROUGH_DIRECTO;
        g_iommu_iniciado = 1;
        return 0;
    }

    // Tabla DMAR detectada
    g_estado_iommu.tabla_dmar_detectada = 1;
    g_estado_iommu.ancho_direccion_host = tabla_dmar->haw + 1;
    g_estado_iommu.banderas_dmar = tabla_dmar->banderas;

    serial_imprimir("[IOMMU] ¡Tabla ACPI DMAR detectada! Ancho Dirección Host: ");
    serial_imprimir_dec(g_estado_iommu.ancho_direccion_host);
    serial_imprimir_linea(" bits");

    // Recorrer subestructuras DMAR
    uint8_t *cursor = (uint8_t *)tabla_dmar + sizeof(struct acpi_dmar_encabezado);
    uint8_t *fin = (uint8_t *)tabla_dmar + tabla_dmar->sdt.longitud;

    while (cursor + sizeof(struct dmar_subestructura) <= fin) {
        struct dmar_subestructura *sub = (struct dmar_subestructura *)cursor;
        if (sub->longitud < sizeof(struct dmar_subestructura) || cursor + sub->longitud > fin) {
            break;
        }

        if (sub->tipo == 0 && g_estado_iommu.conteo_drhd < MAX_UNIDADES_DRHD) {
            // DRHD (DMA Remapping Hardware Unit Definition)
            struct dmar_drhd_crudo *drhd_crudo = (struct dmar_drhd_crudo *)cursor;
            uint32_t idx = g_estado_iommu.conteo_drhd;
            iommu_unidad_drhd_t *u = &g_estado_iommu.drhd[idx];

            u->segmento_pci = drhd_crudo->segmento_pci;
            u->abarca_todos_pci = (drhd_crudo->banderas & 1) != 0;
            u->mmio_fisica = drhd_crudo->dir_base_mmio;
            u->mmio_virtual = IOMMU_MMIO_BASE_VIRT + ((uint64_t)idx * IOMMU_MMIO_PASO);

            // Mapear los registros de hardware de la unidad VT-d
            paginacion_mapear(u->mmio_virtual, u->mmio_fisica, PAGINA_ATRIBUTOS_MMIO);

            // Lectura de registros MMIO de silicio Intel VT-d
            volatile uint32_t *reg_ver  = (volatile uint32_t *)(u->mmio_virtual + 0x00);
            volatile uint64_t *reg_cap  = (volatile uint64_t *)(u->mmio_virtual + 0x08);
            volatile uint64_t *reg_ecap = (volatile uint64_t *)(u->mmio_virtual + 0x10);
            volatile uint32_t *reg_gsts = (volatile uint32_t *)(u->mmio_virtual + 0x1C);

            u->version = *reg_ver;
            u->capacidades = *reg_cap;
            u->capacidades_extendidas = *reg_ecap;
            u->estado_global = *reg_gsts;

            u->traduccion_activa   = (u->estado_global & (1U << 31)) != 0; // TES
            u->soporta_passthrough = (u->capacidades_extendidas & (1ULL << 6)) != 0; // PT
            u->coherente           = (u->capacidades_extendidas & (1ULL << 0)) != 0; // C
            u->soporta_paginas_2mb = (u->capacidades & (1ULL << 34)) != 0; // SLLPS
            u->activa = 1;

            g_estado_iommu.conteo_drhd++;
        } else if (sub->tipo == 1 && g_estado_iommu.conteo_rmrr < MAX_REGIONES_RMRR) {
            // RMRR (Reserved Memory Region Reporting)
            struct dmar_rmrr_crudo *rmrr_crudo = (struct dmar_rmrr_crudo *)cursor;
            uint32_t idx = g_estado_iommu.conteo_rmrr;
            iommu_region_rmrr_t *r = &g_estado_iommu.rmrr[idx];

            r->segmento_pci = rmrr_crudo->segmento_pci;
            r->dir_base_fisica = rmrr_crudo->dir_base_fisica;
            r->dir_limite_fisica = rmrr_crudo->dir_limite_fisica;

            g_estado_iommu.conteo_rmrr++;
        }

        cursor += sub->longitud;
    }

    // Determinar modo de operación global
    int alguna_traduccion = 0;
    int algun_passthrough = 0;
    for (uint32_t i = 0; i < g_estado_iommu.conteo_drhd; i++) {
        if (g_estado_iommu.drhd[i].traduccion_activa) alguna_traduccion = 1;
        if (g_estado_iommu.drhd[i].soporta_passthrough) algun_passthrough = 1;
    }

    if (alguna_traduccion) {
        g_estado_iommu.modo_operacion = IOMMU_MODO_VT_D_TRADUCCION_ACTIVA;
    } else if (algun_passthrough) {
        g_estado_iommu.modo_operacion = IOMMU_MODO_VT_D_PASSTHROUGH;
    } else {
        g_estado_iommu.modo_operacion = IOMMU_MODO_PASSTHROUGH_DIRECTO;
    }

    g_iommu_iniciado = 1;
    return 0;
}

int iommu_vt_d_detectado(void) {
    if (!g_iommu_iniciado) iommu_iniciar();
    return g_estado_iommu.tabla_dmar_detectada;
}

const iommu_estado_t *iommu_obtener_estado(void) {
    if (!g_iommu_iniciado) iommu_iniciar();
    return &g_estado_iommu;
}

int iommu_ejecutar_autodiagnostico(void) {
    serial_imprimir_linea("--- INICIO DE AUTODIAGNÓSTICO IOMMU / VT-d ---");
    if (!g_iommu_iniciado) iommu_iniciar();

    if (!g_estado_iommu.tabla_dmar_detectada) {
        serial_imprimir_linea("[INFO] Plataforma sin tabla ACPI DMAR. Operando en modo DMA Directo Físico 1:1.");
        serial_imprimir_linea("[OK] Bus PCIe autorizado para transacciones directas Bus Master sin aislamiento IOMMU.");
        serial_imprimir_linea("--- AUTODIAGNÓSTICO IOMMU COMPLETADO ---");
        return 0;
    }

    serial_imprimir("[OK] Unidades de Remapeo DRHD descubiertas: ");
    serial_imprimir_dec(g_estado_iommu.conteo_drhd);
    serial_imprimir_linea("");

    for (uint32_t i = 0; i < g_estado_iommu.conteo_drhd; i++) {
        iommu_unidad_drhd_t *u = &g_estado_iommu.drhd[i];
        serial_imprimir("  DRHD #");
        serial_imprimir_dec(i);
        serial_imprimir(": MMIO Fís ");
        serial_imprimir_hex(u->mmio_fisica);
        serial_imprimir(" | Versión: ");
        serial_imprimir_hex(u->version);
        serial_imprimir(" | Trad. Activa: ");
        serial_imprimir_dec(u->traduccion_activa);
        serial_imprimir(" | PassThrough: ");
        serial_imprimir_dec(u->soporta_passthrough);
        serial_imprimir_linea("");
    }

    serial_imprimir("[OK] Regiones Reservadas RMRR descubiertas: ");
    serial_imprimir_dec(g_estado_iommu.conteo_rmrr);
    serial_imprimir_linea("");

    for (uint32_t i = 0; i < g_estado_iommu.conteo_rmrr; i++) {
        iommu_region_rmrr_t *r = &g_estado_iommu.rmrr[i];
        serial_imprimir("  RMRR #");
        serial_imprimir_dec(i);
        serial_imprimir(": Base ");
        serial_imprimir_hex(r->dir_base_fisica);
        serial_imprimir(" - Límite ");
        serial_imprimir_hex(r->dir_limite_fisica);
        serial_imprimir_linea("");
    }

    serial_imprimir_linea("--- AUTODIAGNÓSTICO IOMMU EXITOSO ---");
    return 0;
}
