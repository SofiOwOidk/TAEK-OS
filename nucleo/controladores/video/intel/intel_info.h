#ifndef CONTROLADORES_VIDEO_INTEL_INFO_H
#define CONTROLADORES_VIDEO_INTEL_INFO_H

#include <stdint.h>
#include <stddef.h>
#include "arquitectura/x86_64/pci.h"

#define INTEL_MAX_DISPOSITIVOS_IGPU 4

// Estructura de inventario y estado H0 del dispositivo gráfico Intel
struct intel_gpu_info {
    uint8_t  detectado;
    uint8_t  bus;
    uint8_t  ranura;
    uint8_t  funcion;
    uint16_t id_proveedor;      // 0x8086
    uint16_t id_dispositivo;    // p.ej. 0x5917 (KBL-R) o 0xA788 (RPL-S/HX)
    uint8_t  revision;
    uint8_t  clase;
    uint8_t  subclase;
    uint8_t  prog_if;
    uint16_t sub_proveedor;
    uint16_t sub_dispositivo;
    uint8_t  linea_irq;
    uint8_t  pin_irq;
    uint16_t pci_comando;       // Readback pasivo; nunca habilita Bus Mastering

    // Capacidades PCI detectadas por recorrido de lista (offset 0x34)
    uint8_t  cap_pm;            // Power Management (0x01)
    uint8_t  cap_msi;           // MSI (0x05)
    uint8_t  cap_msix;          // MSI-X (0x11)
    uint8_t  cap_pcie;          // PCI Express (0x10)
    uint8_t  msi_habilitado;    // Estado de activación en registro de control MSI

    // Detalle de los 6 BARs
    struct barra_pci barras[6];

    // Diagnóstico de coexistencia GOP Framebuffer
    uint64_t gop_fb_fisico;
    uint64_t gop_fb_tamano;
    uint64_t gop_ancho, gop_alto, gop_stride;
    int      gop_rango_valido;
    int      gop_en_apertura_intel; // 1 si el framebuffer lineal reside dentro de BAR2 (GMADR)

    // Diagnóstico de IOMMU / VT-d
    int      iommu_inventario_disponible; // Estado pasivo conocido; no acredita DMA por BDF
    int      iommu_dmar_presente;
    int      iommu_traduccion_activa;
    uint32_t iommu_modo;
    uint32_t conteo_rmrr;
};

// Funciones del módulo de inventario Intel H0
int  intel_inventario_detectar(struct intel_gpu_info *salida, int max_dispositivos);
void intel_inventario_imprimir_h0(void);

#endif // CONTROLADORES_VIDEO_INTEL_INFO_H
