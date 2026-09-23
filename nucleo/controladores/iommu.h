#ifndef CONTROLADORES_IOMMU_H
#define CONTROLADORES_IOMMU_H

#include <stdint.h>
#include <stddef.h>

// Modos de operación de IOMMU en TAEK OS
typedef enum {
    IOMMU_MODO_DESCONOCIDO = 0,
    IOMMU_MODO_PASSTHROUGH_DIRECTO,   // DMA físico 1:1 transparente (Sin tabla DMAR o VT-d desactivado por BIOS)
    IOMMU_MODO_VT_D_PASSTHROUGH,      // Intel VT-d activo con soporte de hardware Pass-Through (PT)
    IOMMU_MODO_VT_D_TRADUCCION_ACTIVA // Intel VT-d activo con tablas de traducción habilitadas por firmware
} iommu_modo_t;

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
} iommu_unidad_drhd_t;

// Región de Memoria Reservada de Hardware (RMRR)
typedef struct {
    uint16_t segmento_pci;
    uint64_t dir_base_fisica;
    uint64_t dir_limite_fisica;
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
} iommu_estado_t;

// --- API NATIVA EN ESPAÑOL (ANILLO 0) ---

// Inicializa el subsistema IOMMU: descubre RSDP/XSDT, tabla DMAR e inicializa DRHD/RMRR
int  iommu_iniciar(void);

// Devuelve 1 si se detectó Intel VT-d (tabla ACPI DMAR)
int  iommu_vt_d_detectado(void);

// Obtiene el estado actual del controlador IOMMU
const iommu_estado_t *iommu_obtener_estado(void);

// Autodiagnóstico de IOMMU, validación de permisos DMA para GPU y ausencia de fallos DMAR
int  iommu_ejecutar_autodiagnostico(void);

#endif // CONTROLADORES_IOMMU_H
