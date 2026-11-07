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

#define IOMMU_MMIO_BASE_VIRT 0xFFFFFE0002000000ULL
#define IOMMU_MMIO_PASO      0x0000000000100000ULL // 1 MiB de ventana por unidad DRHD

static iommu_estado_t g_estado_iommu;
static int            g_iommu_iniciado = 0;

#define ACPI_VENTANA_VIRT_BASE 0xFFFFFE0003000000ULL

static uint64_t g_acpi_cursor_virt = ACPI_VENTANA_VIRT_BASE;

static void *acpi_mapear_memoria_fisica(uint64_t phys_addr, uint64_t bytes) {
    if (!phys_addr || !bytes || bytes > 1024 * 1024 || bytes - 1 > UINT64_MAX - phys_addr)
        return NULL;

    // Si ya es dirección virtual canonical alta (>= 0xFFFF800000000000)
    if (phys_addr >= 0xFFFF800000000000ULL) {
        for (uint64_t pos = 0; pos < bytes;) {
            if (!paginacion_obtener_fisica(phys_addr + pos)) return NULL;
            uint64_t paso = 4096 - ((phys_addr + pos) & 4095);
            pos += paso;
        }
        return (void *)phys_addr;
    }
    if (phys_addr >= UINT64_C(0x10000000000000)) return NULL;

    uint64_t hhdm = memoria_obtener_hhdm_offset();
    uint64_t virt_hhdm = phys_addr + hhdm;

    // Si ya está mapeada en el HHDM de Limine y es válida
    int hhdm_completo = hhdm && phys_addr <= UINT64_MAX - hhdm;
    for (uint64_t pos = 0; hhdm_completo && pos < bytes;) {
        if (paginacion_obtener_fisica(virt_hhdm + pos) != phys_addr + pos) hhdm_completo = 0;
        pos += 4096 - ((phys_addr + pos) & 4095);
    }
    if (hhdm_completo) {
        return (void *)virt_hhdm;
    }

    // No mapeada en HHDM (memoria ACPI Reclaimable / NVS de UEFI fuera de RAM usable)
    uint64_t phys_alineada = phys_addr & ~0xFFFULL;
    uint64_t offset_pagina = phys_addr & 0xFFFULL;
    uint64_t pags = (offset_pagina + bytes + 4095ULL) / 4096ULL;

    uint64_t virt_base = g_acpi_cursor_virt;
    if (pags * 4096 > UINT64_C(0xFFFFFE0003F00000) - virt_base) return NULL;
    g_acpi_cursor_virt += (pags * 4096ULL);

    for (uint64_t p = 0; p < pags; p++) {
        if (paginacion_mapear(virt_base + p * 4096, phys_alineada + p * 4096, PAGINA_PRESENTE)) {
            for (uint64_t j = 0; j < p; ++j) paginacion_desmapear(virt_base + j * 4096);
            return NULL;
        }
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

int acpi_obtener_reset(uint8_t *espacio,uint64_t *direccion,uint8_t *valor){
    if(!espacio||!direccion||!valor||!g_peticion_rsdp.response)return -1;
    const struct acpi_rsdp *r=acpi_mapear_memoria_fisica(g_peticion_rsdp.response->address,sizeof(*r));
    if(!r || memcmp(r->firma,"RSD PTR ",8)||!verificar_checksum_acpi(r,20))return -1;
    unsigned paso=4;uint64_t fis=r->rsdt_dir;
    if(r->revision>=2){
        if(r->longitud<36||r->longitud>4096)return -1;
        r=acpi_mapear_memoria_fisica(g_peticion_rsdp.response->address,r->longitud);
        if(!r || !verificar_checksum_acpi(r,r->longitud))return -1;
        if(r->xsdt_dir){paso=8;fis=r->xsdt_dir;}
    }
    const struct acpi_encabezado_tabla *h=acpi_mapear_memoria_fisica(fis,36);
    if(!h || h->longitud<36||h->longitud>65536 || memcmp(h->firma,paso==8?"XSDT":"RSDT",4)||(h->longitud-36)%paso)return -1;
    const uint8_t *tabla=acpi_mapear_memoria_fisica(fis,h->longitud);
    if(!tabla || !verificar_checksum_acpi(tabla,h->longitud))return -1;
    for(unsigned off=36;off<h->longitud;off+=paso){
        uint64_t addr=0;memcpy(&addr,tabla+off,paso);
        const struct acpi_encabezado_tabla *t=acpi_mapear_memoria_fisica(addr,36);
        if(!t || memcmp(t->firma,"FACP",4))continue;
        if(t->longitud<129||t->longitud>65536)return -1;
        const uint8_t *f=acpi_mapear_memoria_fisica(addr,t->longitud);
        if(!f || !verificar_checksum_acpi(f,t->longitud))return -1;
        uint32_t flags;memcpy(&flags,f+112,4);
        if(!(flags&(1u<<10))||f[117]!=8||f[118] || f[119]>1 || f[116]>1)return -1;
        memcpy(&addr,f+120,8);if(!addr)return -1;
        *espacio=f[116];*valor=f[128];
        if(*espacio==1){if(addr>65535)return -1;*direccion=addr;}
        else{
            if(addr>=UINT64_C(0x10000000000000))return -1;
            uint64_t base=UINT64_C(0xfffffe0003ff0000);
            if(paginacion_mapear(base,addr&~UINT64_C(4095),PAGINA_ATRIBUTOS_MMIO))return -1;
            *direccion=base+(addr&4095);
        }return 0;
    }return -1;
}

/* Indice de tablas del firmware: se lee una vez porque cada mapeo fuera del HHDM gasta ventana. */
#define ACPI_TABLAS_MAX 64
static struct { char firma[4]; const uint8_t *datos; uint32_t bytes; } g_acpi_tablas[ACPI_TABLAS_MAX];
static unsigned g_acpi_n_tablas;
static int g_acpi_indexado;

static void acpi_indexar_una(uint64_t fisica) {
    if (g_acpi_n_tablas >= ACPI_TABLAS_MAX || !fisica) return;
    const struct acpi_encabezado_tabla *h = acpi_mapear_memoria_fisica(fisica, 36);
    if (!h || h->longitud < 36 || h->longitud > 1024 * 1024) return;
    const uint8_t *t = acpi_mapear_memoria_fisica(fisica, h->longitud);
    if (!t || !verificar_checksum_acpi(t, h->longitud)) return;
    memcpy(g_acpi_tablas[g_acpi_n_tablas].firma, t, 4);
    g_acpi_tablas[g_acpi_n_tablas].datos = t;
    g_acpi_tablas[g_acpi_n_tablas].bytes = h->longitud;
    g_acpi_n_tablas++;
}
static void acpi_indexar(void) {
    if (g_acpi_indexado) return;
    g_acpi_indexado = 1;
    if (!g_peticion_rsdp.response) return;
    const struct acpi_rsdp *r = acpi_mapear_memoria_fisica(g_peticion_rsdp.response->address, sizeof(*r));
    if (!r || memcmp(r->firma, "RSD PTR ", 8) || !verificar_checksum_acpi(r, 20)) return;
    unsigned paso = 4; uint64_t fis = r->rsdt_dir;
    if (r->revision >= 2 && r->longitud >= 36 && r->longitud <= 4096 && r->xsdt_dir) { paso = 8; fis = r->xsdt_dir; }
    const struct acpi_encabezado_tabla *h = acpi_mapear_memoria_fisica(fis, 36);
    if (!h || h->longitud < 36 || h->longitud > 65536 || (h->longitud - 36) % paso) return;
    const uint8_t *raiz = acpi_mapear_memoria_fisica(fis, h->longitud);
    if (!raiz || !verificar_checksum_acpi(raiz, h->longitud)) return;
    for (unsigned off = 36; off < h->longitud; off += paso) {
        uint64_t addr = 0; memcpy(&addr, raiz + off, paso);
        unsigned antes = g_acpi_n_tablas;
        acpi_indexar_una(addr);
        /* La DSDT no esta en la raiz: la señala la FADT. */
        if (g_acpi_n_tablas > antes && !memcmp(g_acpi_tablas[antes].firma, "FACP", 4)) {
            const uint8_t *f = g_acpi_tablas[antes].datos; uint32_t n = g_acpi_tablas[antes].bytes;
            uint64_t d = 0;
            if (n >= 148) memcpy(&d, f + 140, 8);
            if (!d && n >= 44) memcpy(&d, f + 40, 4);
            acpi_indexar_una(d);
        }
    }
}
int acpi_tabla_buscar(const char *firma, unsigned indice, const uint8_t **datos, uint32_t *bytes) {
    acpi_indexar();
    for (unsigned i = 0; i < g_acpi_n_tablas; i++)
        if (!memcmp(g_acpi_tablas[i].firma, firma, 4) && indice-- == 0) { *datos = g_acpi_tablas[i].datos; *bytes = g_acpi_tablas[i].bytes; return 0; }
    return -1;
}
int acpi_tabla_listar(unsigned indice, char firma[5], const uint8_t **datos, uint32_t *bytes) {
    acpi_indexar();
    if (indice >= g_acpi_n_tablas) return -1;
    memcpy(firma, g_acpi_tablas[indice].firma, 4); firma[4] = 0;
    *datos = g_acpi_tablas[indice].datos; *bytes = g_acpi_tablas[indice].bytes;
    return 0;
}
/* Una pagina fisica para leerla sin cache: para el interprete AML, que puede apuntar a registros del chipset. */
#define ACPI_LECTURA_VENTANA UINT64_C(0xFFFFFE0003F00000)
#define ACPI_LECTURA_PAGINAS 224
static uint64_t g_acpi_lectura_fis[ACPI_LECTURA_PAGINAS];
static unsigned g_acpi_lectura_n;
const void *acpi_mapear_lectura(uint64_t fisica) {
    uint64_t pag = fisica & ~UINT64_C(4095);
    if (pag >= UINT64_C(0x10000000000000)) return NULL;
    uint64_t hhdm = memoria_obtener_hhdm_offset();
    if (hhdm && paginacion_obtener_fisica(pag + hhdm) == pag) return (const void *)(fisica + hhdm);
    for (unsigned i = 0; i < g_acpi_lectura_n; i++)
        if (g_acpi_lectura_fis[i] == pag) return (const void *)(ACPI_LECTURA_VENTANA + i * UINT64_C(4096) + (fisica & 4095));
    if (g_acpi_lectura_n >= ACPI_LECTURA_PAGINAS) return NULL;
    uint64_t v = ACPI_LECTURA_VENTANA + g_acpi_lectura_n * UINT64_C(4096);
    if (paginacion_mapear(v, pag, PAGINA_PRESENTE | PAGINA_SIN_CACHE | PAGINA_ESCRITURA_DIR | PAGINA_NO_EJECUTABLE)) return NULL;
    g_acpi_lectura_fis[g_acpi_lectura_n++] = pag;
    return (const void *)(v + (fisica & 4095));
}

/* Discovery validates the complete table before mapping any DRHD registers. */
static const uint8_t *acpi_tabla(uint64_t fisica, const char *firma) {
    const struct acpi_encabezado_tabla *h = acpi_mapear_memoria_fisica(fisica, 36);
    if (!h || memcmp(h->firma, firma, 4) || h->longitud < 36 || h->longitud > 1024 * 1024)
        return NULL;
    const uint8_t *p = acpi_mapear_memoria_fisica(fisica, h->longitud);
    return p && verificar_checksum_acpi(p, h->longitud) ? p : NULL;
}
int iommu_iniciar(void) {
    if (g_iommu_iniciado) return g_estado_iommu.error_descubrimiento ? -1 : 0;
    g_iommu_iniciado = 1;
    memset(&g_estado_iommu, 0, sizeof(g_estado_iommu));
    if (!g_peticion_rsdp.response || !g_peticion_rsdp.response->address) goto fallo;
    uint64_t addr = g_peticion_rsdp.response->address;
    const struct acpi_rsdp *r = acpi_mapear_memoria_fisica(addr, sizeof(*r));
    if (!r || memcmp(r->firma, "RSD PTR ", 8) || !verificar_checksum_acpi(r, 20)) goto fallo;
    unsigned paso = 4;
    uint64_t raiz = r->rsdt_dir;
    if (r->revision >= 2) {
        if (r->longitud < 36 || r->longitud > 4096) goto fallo;
        r = acpi_mapear_memoria_fisica(addr, r->longitud);
        if (!r || !verificar_checksum_acpi(r, r->longitud)) goto fallo;
        if (r->xsdt_dir) { paso = 8; raiz = r->xsdt_dir; }
    }
    const uint8_t *tabla = acpi_tabla(raiz, paso == 8 ? "XSDT" : "RSDT");
    if (!tabla) goto fallo;
    uint32_t bytes;
    memcpy(&bytes, tabla + 4, 4);
    if ((bytes - 36) % paso) goto fallo;
    const uint8_t *dmar = NULL;
    uint32_t dmar_bytes = 0;
    for (unsigned pos = 36; pos < bytes; pos += paso) {
        uint64_t fisica = 0;
        memcpy(&fisica, tabla + pos, paso);
        if (!fisica) goto fallo;
        const struct acpi_encabezado_tabla *h = acpi_mapear_memoria_fisica(fisica, 36);
        if (!h) goto fallo;
        if (memcmp(h->firma, "DMAR", 4)) continue;
        if (dmar) goto fallo; /* Duplicate DMAR is ambiguous. */
        dmar = acpi_tabla(fisica, "DMAR");
        if (!dmar) goto fallo;
        memcpy(&dmar_bytes, dmar + 4, 4);
    }
    if (!dmar) {
        serial_imprimir_linea("[IOMMU] DMAR ausente; ruta DMA desconocida, no se presume 1:1.");
        return 0;
    }
    if (iommu_dmar_analizar(dmar, dmar_bytes, &g_estado_iommu)) goto fallo;
    for (unsigned i = 0; i < g_estado_iommu.conteo_drhd; ++i) {
        iommu_unidad_drhd_t *u = &g_estado_iommu.drhd[i];
        u->mmio_virtual = IOMMU_MMIO_BASE_VIRT + (uint64_t)i * IOMMU_MMIO_PASO;
        if (paginacion_mapear(u->mmio_virtual, u->mmio_fisica, PAGINA_ATRIBUTOS_MMIO))
            goto fallo_mapas;
        u->version = *(volatile uint32_t *)(u->mmio_virtual + 0x00);
        u->capacidades = *(volatile uint64_t *)(u->mmio_virtual + 0x08);
        u->capacidades_extendidas = *(volatile uint64_t *)(u->mmio_virtual + 0x10);
        u->estado_global = *(volatile uint32_t *)(u->mmio_virtual + 0x1c);
        if (u->version == UINT32_MAX || !u->version ||
            u->capacidades == UINT64_MAX || u->capacidades_extendidas == UINT64_MAX)
            goto fallo_mapas;
        u->traduccion_activa = !!(u->estado_global & (1u << 31));
        u->soporta_passthrough = !!(u->capacidades_extendidas & (UINT64_C(1) << 6));
        u->coherente = !!(u->capacidades_extendidas & 1);
        u->soporta_paginas_2mb = !!(u->capacidades & (UINT64_C(1) << 34));
        u->activa = 1;
    }
    g_estado_iommu.modo_operacion = IOMMU_MODO_PASSTHROUGH_DIRECTO;
    for (unsigned i = 0; i < g_estado_iommu.conteo_drhd; ++i)
        if (g_estado_iommu.drhd[i].traduccion_activa)
            g_estado_iommu.modo_operacion = IOMMU_MODO_VT_D_TRADUCCION_ACTIVA;
    serial_imprimir_linea("[IOMMU] DMAR y scopes validados; TES observado, dominios DMA no configurados.");
    return 0;
fallo_mapas:
    for (unsigned i = 0; i < g_estado_iommu.conteo_drhd; ++i)
        if (g_estado_iommu.drhd[i].mmio_virtual)
            paginacion_desmapear(g_estado_iommu.drhd[i].mmio_virtual);
fallo:
    g_estado_iommu.modo_operacion = IOMMU_MODO_DESCONOCIDO;
    g_estado_iommu.inventario_completo = 0;
    g_estado_iommu.error_descubrimiento = 1;
    serial_imprimir_linea("[IOMMU] Descubrimiento invalido/incompleto; ruta DMA desconocida.");
    return -1;
}

int iommu_vt_d_detectado(void) {
    if (!g_iommu_iniciado) iommu_iniciar();
    return g_estado_iommu.tabla_dmar_detectada;
}

int iommu_esta_iniciado(void) {
    return g_iommu_iniciado;
}

const iommu_estado_t *iommu_obtener_estado_pasivo(void) {
    return g_iommu_iniciado ? &g_estado_iommu : NULL;
}

const iommu_estado_t *iommu_obtener_estado(void) {
    if (!g_iommu_iniciado) iommu_iniciar();
    return &g_estado_iommu;
}

int iommu_ejecutar_autodiagnostico(void) {
    serial_imprimir_linea("--- INICIO DE AUTODIAGNÓSTICO IOMMU / VT-d ---");
    if (!g_iommu_iniciado) iommu_iniciar();
    if (g_estado_iommu.error_descubrimiento) {
        serial_imprimir_linea("[ERROR] Descubrimiento incompleto; sin acreditacion DMA.");
        return -1;
    }

    if (!g_estado_iommu.tabla_dmar_detectada) {
        serial_imprimir_linea("[INFO] DMAR ausente; traduccion y ruta DMA no acreditadas.");
        serial_imprimir_linea("--- DESCUBRIMIENTO IOMMU COMPLETADO; DMA DESCONOCIDO ---");
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

    serial_imprimir_linea("--- DESCUBRIMIENTO VT-d COMPLETADO; DOMINIOS DMA NO CONFIGURADOS ---");
    return 0;
}
