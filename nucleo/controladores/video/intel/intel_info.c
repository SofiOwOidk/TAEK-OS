#include "intel_info.h"
#include "arquitectura/x86_64/serial.h"
#include "controladores/consola.h"
#include "controladores/pantalla.h"
#include "controladores/iommu.h"
#include "base/paginacion.h"

int intel_inventario_detectar(struct intel_gpu_info *salida, int max_dispositivos) {
    if (!salida || max_dispositivos <= 0) return 0;
    if (!pci_esta_iniciado()) return 0; // Consulta pasiva: no disparar enumeración PCI
    int encontrados = 0;
    int total_pci = pci_obtener_conteo();

    void *fb_virt = pantalla_obtener_base();
    uint64_t gop_phys = fb_virt ? paginacion_obtener_fisica((uint64_t)fb_virt) : 0;
    uint64_t gop_tam = pantalla_obtener_tamano_bytes();

    const iommu_estado_t *iommu = iommu_obtener_estado_pasivo();

    for (int i = 0; i < total_pci && encontrados < max_dispositivos; i++) {
        const struct dispositivo_pci *dev = pci_obtener_dispositivo(i);
        if (!dev || dev->id_proveedor != 0x8086 || dev->clase != 0x03) continue;

        struct intel_gpu_info *info = &salida[encontrados];
        for (size_t b = 0; b < sizeof(struct intel_gpu_info); b++) {
            ((uint8_t *)info)[b] = 0;
        }

        info->detectado       = 1;
        info->bus             = dev->bus;
        info->ranura          = dev->ranura;
        info->funcion         = dev->funcion;
        info->id_proveedor    = dev->id_proveedor;
        info->id_dispositivo  = dev->id_dispositivo;
        info->revision        = dev->revision;
        info->clase           = dev->clase;
        info->subclase        = dev->subclase;
        info->prog_if         = dev->prog_if;
        info->sub_proveedor   = dev->sub_proveedor;
        info->sub_dispositivo = dev->sub_dispositivo;
        info->linea_irq       = dev->linea_irq;
        info->pin_irq         = dev->pin_irq;
        info->pci_comando = pci_leer_config_16(dev->bus, dev->ranura, dev->funcion, 0x04);

        for (int b = 0; b < 6; b++) {
            info->barras[b] = dev->barras[b];
        }

        // Recorrido de capacidades PCI estándar (Capabilities List)
        uint16_t status = pci_leer_config_16(dev->bus, dev->ranura, dev->funcion, 0x06);
        if (status & (1 << 4)) {
            uint8_t cap_ptr = pci_leer_config_8(dev->bus, dev->ranura, dev->funcion, 0x34) & ~0x03;
            int saltos = 0;
            while (cap_ptr >= 0x40 && cap_ptr <= 0xFC && saltos++ < 48) {
                uint8_t cap_id = pci_leer_config_8(dev->bus, dev->ranura, dev->funcion, cap_ptr);
                uint8_t next_ptr = pci_leer_config_8(dev->bus, dev->ranura, dev->funcion, cap_ptr + 1) & ~0x03;

                if (cap_id == 0x01) {
                    info->cap_pm = 1;
                } else if (cap_id == 0x05) {
                    info->cap_msi = 1;
                    uint16_t msi_ctrl = pci_leer_config_16(dev->bus, dev->ranura, dev->funcion, cap_ptr + 2);
                    info->msi_habilitado = (msi_ctrl & 0x01) ? 1 : 0;
                } else if (cap_id == 0x11) {
                    info->cap_msix = 1;
                } else if (cap_id == 0x10) {
                    info->cap_pcie = 1;
                }

                if (next_ptr == cap_ptr || next_ptr == 0) break;
                cap_ptr = next_ptr;
            }
        }

        // Diagnóstico de Framebuffer GOP
        info->gop_fb_fisico = gop_phys;
        info->gop_fb_tamano = gop_tam;
        info->gop_ancho = pantalla_obtener_ancho();
        info->gop_alto = pantalla_obtener_alto();
        info->gop_stride = pantalla_obtener_stride_bytes();
        info->gop_rango_valido = gop_phys != 0 && gop_tam != 0 &&
            gop_tam - 1 <= UINT64_MAX - gop_phys;
        info->gop_en_apertura_intel = 0;
        if (info->gop_rango_valido && dev->barras[2].valida && dev->barras[2].tamano > 0) {
            uint64_t bar2_base = dev->barras[2].dir_base;
            uint64_t bar2_tam = dev->barras[2].tamano;
            if (bar2_tam - 1 <= UINT64_MAX - bar2_base &&
                gop_tam <= bar2_tam && gop_phys >= bar2_base &&
                gop_phys - bar2_base <= bar2_tam - gop_tam) {
                info->gop_en_apertura_intel = 1;
            }
        }

        // Diagnóstico IOMMU / VT-d
        if (iommu) {
            info->iommu_inventario_disponible =
                iommu->modo_operacion != IOMMU_MODO_DESCONOCIDO;
            info->iommu_dmar_presente     = iommu->tabla_dmar_detectada;
            info->iommu_modo              = (uint32_t)iommu->modo_operacion;
            info->iommu_traduccion_activa = (iommu->modo_operacion == IOMMU_MODO_VT_D_TRADUCCION_ACTIVA);
            info->conteo_rmrr             = iommu->conteo_rmrr;
        }

        encontrados++;
    }

    return encontrados;
}

static const char *nombre_generacion_intel(uint16_t dev_id) {
    if (dev_id == 0x5917 || dev_id == 0x5916 || dev_id == 0x5926 || dev_id == 0x5927) {
        return "Intel Gen 9.5 (Kaby Lake Refresh / UHD 620)";
    }
    if ((dev_id >= 0x3E90 && dev_id <= 0x3EA9) || dev_id == 0x9B41 || dev_id == 0x9BA4) {
        return "Intel Gen 9.5 (Coffee Lake / Comet Lake / UHD 630)";
    }
    if (dev_id >= 0x8A50 && dev_id <= 0x8A5D) {
        return "Intel Gen 11 (Ice Lake / Iris Plus)";
    }
    if (dev_id == 0x9A49 || dev_id == 0x9A40 || dev_id == 0x9A60 || dev_id == 0x9A70) {
        return "Intel Gen 12 (Tiger Lake / Xe-LP / UHD 730/750)";
    }
    if (dev_id == 0x4680 || dev_id == 0x4682 || dev_id == 0x4690 || dev_id == 0x4692) {
        return "Intel Gen 12 (Alder Lake / UHD 770)";
    }
    if (dev_id == 0xA780 || dev_id == 0xA781 || dev_id == 0xA788 || dev_id == 0xA789 || dev_id == 0xA7A0) {
        return "Intel Gen 12 (Raptor Lake Refresh / Core i9-14900HX UHD 770)";
    }
    return "Intel HD/UHD Graphics";
}

void intel_inventario_imprimir_h0(void) {
    if (!pci_esta_iniciado()) {
        serial_imprimir_linea("[INTEL_H0] ENCONTRADOS=DESCONOCIDO INVENTARIO_PCI=0 BACKEND_READY=0");
        consola_imprimir_linea_color("Inventario PCI no disponible; consulta sin inicializacion.", COLOR_AVISO_DEFAULT);
        return;
    }
    struct intel_gpu_info dispositivos[INTEL_MAX_DISPOSITIVOS_IGPU];
    int encontrados = intel_inventario_detectar(dispositivos, INTEL_MAX_DISPOSITIVOS_IGPU);

    if (encontrados == 0) {
        serial_imprimir_linea("[INTEL_H0] ENCONTRADOS=0 BACKEND_READY=0");
        consola_imprimir_linea_color("No se detectó GPU Intel integrada en el bus PCI.", COLOR_AVISO_DEFAULT);
        return;
    }

    consola_imprimir_linea_color("=== INVENTARIO FISICO INTEL GPU (HITO H0 - PLAN_I915) ===", COLOR_PROMPT_DEFAULT);

    for (int i = 0; i < encontrados; i++) {
        const struct intel_gpu_info *g = &dispositivos[i];

        // Telemetría serial estructurada H0.1
        serial_imprimir("[INTEL_H0] PCI=");
        serial_imprimir_dec(g->bus); serial_imprimir(":");
        serial_imprimir_dec(g->ranura); serial_imprimir(".");
        serial_imprimir_dec(g->funcion);
        serial_imprimir(" VENDOR=0x8086 DEVICE=");
        serial_imprimir_hex(g->id_dispositivo);
        serial_imprimir(" REVISION=");
        serial_imprimir_hex(g->revision);
        serial_imprimir(" SUBSYSTEM=");
        serial_imprimir_hex((uint64_t)g->sub_proveedor << 16 | g->sub_dispositivo);
        serial_imprimir(" CLASE=");
        serial_imprimir_hex((uint64_t)g->clase << 16 | (uint64_t)g->subclase << 8 | g->prog_if);
        serial_imprimir_linea("");

        for (int b = 0; b < 6; b++) {
            if (!g->barras[b].valida) continue;
            serial_imprimir("[INTEL_H0] BAR");
            serial_imprimir_dec(b);
            serial_imprimir(" BASE=");
            serial_imprimir_hex(g->barras[b].dir_base);
            serial_imprimir(" BYTES=");
            serial_imprimir_dec(g->barras[b].tamano);
            serial_imprimir(g->barras[b].es_io ? " TIPO=IO" : " TIPO=MEM");
            if (g->barras[b].es_64bits) serial_imprimir(" 64BIT");
            if (g->barras[b].predecible) serial_imprimir(" PREFETCH");
            serial_imprimir_linea("");
        }

        serial_imprimir("[INTEL_H0] IRQ_PIN=");
        serial_imprimir_dec(g->pin_irq);
        serial_imprimir(" IRQ_LINE=");
        serial_imprimir_dec(g->linea_irq);
        serial_imprimir(" PCI_COMMAND="); serial_imprimir_hex(g->pci_comando);
        serial_imprimir(" CAP_PM="); serial_imprimir_dec(g->cap_pm);
        serial_imprimir(" CAP_MSI="); serial_imprimir_dec(g->cap_msi);
        serial_imprimir(" MSI_ACTIVO="); serial_imprimir_dec(g->msi_habilitado);
        serial_imprimir(" CAP_MSIX="); serial_imprimir_dec(g->cap_msix);
        serial_imprimir(" CAP_PCIE="); serial_imprimir_dec(g->cap_pcie);
        serial_imprimir_linea("");

        serial_imprimir("[INTEL_H0] GOP_FB_PHYS=");
        serial_imprimir_hex(g->gop_fb_fisico);
        serial_imprimir(" GOP_FB_BYTES=");
        serial_imprimir_dec(g->gop_fb_tamano);
        serial_imprimir(" WIDTH="); serial_imprimir_dec(g->gop_ancho);
        serial_imprimir(" HEIGHT="); serial_imprimir_dec(g->gop_alto);
        serial_imprimir(" STRIDE="); serial_imprimir_dec(g->gop_stride);
        serial_imprimir(" RANGO_VALIDO="); serial_imprimir_dec(g->gop_rango_valido);
        serial_imprimir(g->gop_en_apertura_intel ? " GOP_ORIGEN=INTEL_APERTURE_BAR2" : " GOP_ORIGEN=NO_ACREDITADO");
        serial_imprimir_linea("");

        serial_imprimir("[INTEL_H0] IOMMU_DMAR=");
        serial_imprimir_dec(g->iommu_dmar_presente);
        serial_imprimir(" MODO=");
        serial_imprimir_dec(g->iommu_modo);
        serial_imprimir(" INVENTARIO_DISPONIBLE=");
        serial_imprimir_dec(g->iommu_inventario_disponible);
        serial_imprimir(" DMA_ACREDITADO=0");
        serial_imprimir(" TRADUCCION_ACTIVA=");
        serial_imprimir_dec(g->iommu_traduccion_activa);
        serial_imprimir(" RMRR_REGIONES=");
        serial_imprimir_dec(g->conteo_rmrr);
        serial_imprimir_linea("");

        serial_imprimir_linea("[INTEL_H0] BACKEND_READY=0 STAGE=H0");

        // Salida formateada en pantalla/consola
        consola_imprimir("  Dispositivo      : ");
        consola_imprimir_color(nombre_generacion_intel(g->id_dispositivo), COLOR_AVISO_DEFAULT);
        consola_imprimir(" (");
        consola_imprimir_hex(g->id_dispositivo);
        consola_imprimir_linea(")");

        consola_imprimir("  Ubicación PCI    : ");
        consola_imprimir_dec(g->bus); consola_imprimir(":");
        consola_imprimir_dec(g->ranura); consola_imprimir(".");
        consola_imprimir_dec(g->funcion);
        consola_imprimir(" | Rev: "); consola_imprimir_hex(g->revision);
        consola_imprimir(" | Subsys: "); consola_imprimir_hex((uint64_t)g->sub_proveedor << 16 | g->sub_dispositivo);
        consola_imprimir_linea("");

        if (g->barras[0].valida) {
            consola_imprimir("  BAR0 (MMIO Regs) : Base ");
            consola_imprimir_hex(g->barras[0].dir_base);
            consola_imprimir(" (");
            consola_imprimir_dec(g->barras[0].tamano / (1024 * 1024));
            consola_imprimir_linea(" MB)");
        }
        if (g->barras[2].valida) {
            consola_imprimir("  BAR2 (Apertura)  : Base ");
            consola_imprimir_hex(g->barras[2].dir_base);
            consola_imprimir(" (");
            consola_imprimir_dec(g->barras[2].tamano / (1024 * 1024));
            consola_imprimir_linea(" MB prefetchable)");
        }

        consola_imprimir("  Interrupciones   : Pin ");
        consola_imprimir_dec(g->pin_irq);
        consola_imprimir(" (Línea "); consola_imprimir_dec(g->linea_irq); consola_imprimir(")");
        consola_imprimir(" | MSI: "); consola_imprimir(g->cap_msi ? (g->msi_habilitado ? "Activo" : "Disponible") : "No");
        consola_imprimir(" | MSI-X: "); consola_imprimir_linea(g->cap_msix ? "Sí" : "No");

        consola_imprimir("  PCI command      : "); consola_imprimir_hex(g->pci_comando);
        consola_imprimir(" | IO: "); consola_imprimir_dec((g->pci_comando & 1) != 0);
        consola_imprimir(" | MEM: "); consola_imprimir_dec((g->pci_comando & 2) != 0);
        consola_imprimir(" | Bus Master: "); consola_imprimir_dec((g->pci_comando & 4) != 0);
        consola_imprimir_linea(" (lectura; no acredita DMA)");

        consola_imprimir("  Framebuffer GOP  : Base física ");
        consola_imprimir_hex(g->gop_fb_fisico);
        consola_imprimir(" -> ");
        consola_imprimir_linea_color(g->gop_en_apertura_intel ? "[APERTURA INTEL BAR2]" : "[RAM DEL SISTEMA / EXTERNO]", COLOR_USUARIO_DEFAULT);

        consola_imprimir("  GOP geometria    : "); consola_imprimir_dec(g->gop_ancho);
        consola_imprimir(" x "); consola_imprimir_dec(g->gop_alto);
        consola_imprimir(" | Stride bytes: "); consola_imprimir_dec(g->gop_stride);
        consola_imprimir_linea("");
        consola_imprimir("  GOP intervalo    : Bytes "); consola_imprimir_dec(g->gop_fb_tamano);
        consola_imprimir(" | Rango valido: "); consola_imprimir_dec(g->gop_rango_valido);
        consola_imprimir(" | Completo en BAR2: "); consola_imprimir_dec(g->gop_en_apertura_intel);
        consola_imprimir_linea("");

        consola_imprimir("  Intel VT-d       : ");
        if (g->iommu_dmar_presente) {
            consola_imprimir_color(g->iommu_traduccion_activa ? "Activo (Traducción DMA)" : "Sin traducción activa (1:1 no acreditado)", COLOR_AVISO_DEFAULT);
            consola_imprimir(" | Regiones RMRR: "); consola_imprimir_dec(g->conteo_rmrr);
            consola_imprimir_linea("");
        } else {
            consola_imprimir_linea_color("Sin tabla DMAR detectada (1:1 no acreditado)", COLOR_AVISO_DEFAULT);
        }

        consola_imprimir("  Estado de Port   : ");
        consola_imprimir_linea_color("H0 NO CERRADO (inventario registrado; backend GPU y acreditacion pendientes)", COLOR_AVISO_DEFAULT);
    }
}
