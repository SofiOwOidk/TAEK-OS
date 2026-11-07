#ifndef CONTROLADORES_IOMMU_H
#define CONTROLADORES_IOMMU_H

#include <stdint.h>
#include <stddef.h>

// Modos de operación de IOMMU en TAEK OS
typedef enum {
    IOMMU_MODO_DESCONOCIDO = 0,
    IOMMU_MODO_PASSTHROUGH_DIRECTO,   // TES=0 observado; no acredita por si solo una ruta DMA
    IOMMU_MODO_VT_D_PASSTHROUGH,      // Reservado: requiere contexto de dispositivo con tipo PT comprobado
    IOMMU_MODO_VT_D_TRADUCCION_ACTIVA // Intel VT-d activo con tablas de traducción habilitadas por firmware
} iommu_modo_t;

#define IOMMU_MAX_SCOPES 16
#define IOMMU_MAX_PASOS 8
typedef struct { uint8_t dispositivo, funcion; } iommu_paso_pci_t;
typedef struct {
    uint8_t tipo, id_enumeracion, bus_inicio, conteo_pasos;
    iommu_paso_pci_t pasos[IOMMU_MAX_PASOS];
} iommu_scope_t;
/* Ruta completa desde un bus raiz conocido; no inferirla de un BDF tras puente. */
typedef struct {
    uint16_t segmento;
    uint8_t bus_raiz, conteo_pasos;
    iommu_paso_pci_t pasos[IOMMU_MAX_PASOS];
} iommu_ruta_pci_t;

// Unidad de Remapeo de Hardware DMA (DRHD) de Intel VT-d
typedef struct {
    uint16_t segmento_pci;
    uint8_t  abarca_todos_pci;        // 1 = INCLUDE_PCI_ALL (cubre todos los endpoints)
    uint8_t  activa;
    uint64_t mmio_fisica;             // Dirección física de registros VT-d
    uint64_t mmio_virtual;            // Dirección virtual mapeada en VMM (0xFFFFFE0002000000 + i*0x100000)
    uint32_t version;                 // Registro VERSION
    uint64_t capacidades;             // Registro CAP_REG
    uint64_t capacidades_extendidas;  // Registro ECAP_REG
    uint32_t estado_global;           // Registro GSTS_REG
    int      traduccion_activa;       // Bit TES (Translation Enable Status: 1=activa, 0=passthrough directo)
    int      soporta_passthrough;     // Bit PT (Pass-Through en ECAP_REG)
    int      coherente;               // Bit C (Page-walk Coherency)
    int      soporta_paginas_2mb;     // Bit SLLPS (2MB / 1GB Large Page Support)
    uint8_t  conteo_scopes;
    iommu_scope_t scopes[IOMMU_MAX_SCOPES];
} iommu_unidad_drhd_t;

// Región de Memoria Reservada de Hardware (RMRR)
typedef struct {
    uint16_t segmento_pci;
    uint64_t dir_base_fisica;
    uint64_t dir_limite_fisica;
    uint8_t conteo_scopes;
    iommu_scope_t scopes[IOMMU_MAX_SCOPES];
} iommu_region_rmrr_t;

#define MAX_UNIDADES_DRHD 8
#define MAX_REGIONES_RMRR 8

// Estado global de IOMMU en TAEK OS
typedef struct {
    int                 tabla_dmar_detectada;
    uint8_t             ancho_direccion_host;  // Ancho efectivo en bits (haw + 1)
    uint8_t             banderas_dmar;         // INTR_REMAP, X2APIC_OPT_OUT
    iommu_modo_t        modo_operacion;
    uint32_t            conteo_drhd;
    iommu_unidad_drhd_t drhd[MAX_UNIDADES_DRHD];
    uint32_t            conteo_rmrr;
    iommu_region_rmrr_t rmrr[MAX_REGIONES_RMRR];
    int inventario_completo;         // Estructuras, scopes, limites y checksum validos
    int error_descubrimiento;        // No sustituir errores por una ruta DMA directa
} iommu_estado_t;

/* Parser real sin MMIO. Error deja salida vacia/desconocida; no trunca inventario. */
int iommu_dmar_analizar(const void *tabla, size_t bytes, iommu_estado_t *salida);
/* Consulta sin efectos. 0: unidad unica, -1: desconocido/ausente, -2: ambiguo. */
int iommu_drhd_por_ruta(const iommu_estado_t *estado, const iommu_ruta_pci_t *ruta,
                        uint32_t *indice);
/* 1: TES=0 observado en la unidad correspondiente, 0: traduccion, -1: desconocido.
 * No demuestra accesibilidad de buffers, coherencia ni ownership GGTT. */
int iommu_dma_directo_observado(const iommu_estado_t *estado,
                              const iommu_ruta_pci_t *ruta, uint32_t *indice);

// --- API NATIVA EN ESPAÑOL (ANILLO 0) ---

// Inicializa el subsistema IOMMU: descubre RSDP/XSDT, tabla DMAR e inicializa DRHD/RMRR
int  iommu_iniciar(void);

// Devuelve 1 si se detectó Intel VT-d (tabla ACPI DMAR)
int  iommu_vt_d_detectado(void);

// Obtiene el estado actual del controlador IOMMU (inicializa si no lo estaba)
const iommu_estado_t *iommu_obtener_estado(void);

// Devuelve 1 si el subsistema IOMMU ya fue inicializado, 0 si no.
int  iommu_esta_iniciado(void);

// Consulta pasiva: devuelve el estado si ya está inicializado, NULL si no. Sin efectos colaterales.
const iommu_estado_t *iommu_obtener_estado_pasivo(void);

// Telemetría de DMAR/DRHD/RMRR. No configura root/context tables ni aísla DMA.
int  iommu_ejecutar_autodiagnostico(void);
/* Registro de reset FADT validado; memoria ya mapeada o puerto System I/O. */
int acpi_obtener_reset(uint8_t *espacio,uint64_t *direccion,uint8_t *valor);
/* Tablas del firmware. indice cuenta las que comparten firma (hay varias SSDT). 0 si existe. */
int acpi_tabla_buscar(const char *firma,unsigned indice,const uint8_t **datos,uint32_t *bytes);
int acpi_tabla_listar(unsigned indice,char firma[5],const uint8_t **datos,uint32_t *bytes);
/* Direccion de lectura para una direccion fisica; valida hasta el final de su pagina. */
const void *acpi_mapear_lectura(uint64_t fisica);

#endif // CONTROLADORES_IOMMU_H
