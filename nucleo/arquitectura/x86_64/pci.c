#include "pci.h"
#include "puertos.h"

#define PUERTO_CONFIG_DIR   0xCF8
#define PUERTO_CONFIG_DATOS 0xCFC

static struct dispositivo_pci g_dispositivos[PCI_MAX_DISPOSITIVOS];
static int g_conteo_dispositivos = 0;
static const struct dispositivo_pci *g_gpu_primaria = 0;
static int g_pci_inicializado = 0;

static inline uint32_t pci_direccion_bus(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento) {
    return (uint32_t)((1U << 31) |
                      ((uint32_t)bus << 16) |
                      ((uint32_t)(ranura & 0x1F) << 11) |
                      ((uint32_t)(funcion & 0x07) << 8) |
                      ((uint32_t)(desplazamiento & 0xFC)));
}

uint32_t pci_leer_config_32(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento) {
    escribir_puerto_l(PUERTO_CONFIG_DIR, pci_direccion_bus(bus, ranura, funcion, desplazamiento));
    return leer_puerto_l(PUERTO_CONFIG_DATOS);
}

uint16_t pci_leer_config_16(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento) {
    uint32_t dir = pci_direccion_bus(bus, ranura, funcion, desplazamiento);
    escribir_puerto_l(PUERTO_CONFIG_DIR, dir);
    return (uint16_t)((leer_puerto_l(PUERTO_CONFIG_DATOS) >> ((desplazamiento & 2) * 8)) & 0xFFFF);
}

uint8_t pci_leer_config_8(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento) {
    uint32_t dir = pci_direccion_bus(bus, ranura, funcion, desplazamiento);
    escribir_puerto_l(PUERTO_CONFIG_DIR, dir);
    return (uint8_t)((leer_puerto_l(PUERTO_CONFIG_DATOS) >> ((desplazamiento & 3) * 8)) & 0xFF);
}

void pci_escribir_config_32(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento, uint32_t valor) {
    escribir_puerto_l(PUERTO_CONFIG_DIR, pci_direccion_bus(bus, ranura, funcion, desplazamiento));
    escribir_puerto_l(PUERTO_CONFIG_DATOS, valor);
}

void pci_escribir_config_16(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento, uint16_t valor) {
    uint32_t dir = pci_direccion_bus(bus, ranura, funcion, desplazamiento);
    escribir_puerto_l(PUERTO_CONFIG_DIR, dir);
    escribir_puerto_w(PUERTO_CONFIG_DATOS + (desplazamiento & 2), valor);
}

const char *pci_nombre_proveedor(uint16_t id_proveedor) {
    switch (id_proveedor) {
        case 0x10DE: return "NVIDIA Corporation";
        case 0x8086: return "Intel Corporation";
        case 0x1002: return "Advanced Micro Devices (AMD/ATI)";
        case 0x1AF4: return "Red Hat, Inc. (VirtIO)";
        case 0x1234: return "Bochs / QEMU Standard VGA";
        case 0x10EC: return "Realtek Semiconductor Corp.";
        case 0x1B36: return "Red Hat QEMU PCIe Root Port";
        case 0x80EE: return "Oracle VirtualBox";
        case 0x15AD: return "VMware Inc.";
        case 0x1414: return "Microsoft Corporation";
        case 0x1022: return "AMD Inc.";
        case 0x1106: return "VIA Technologies";
        case 0x104B: return "BusLogic";
        default:     return "Fabricante Desconocido";
    }
}

const char *pci_nombre_clase(uint8_t clase, uint8_t subclase) {
    switch (clase) {
        case 0x00:
            if (subclase == 0x01) return "VGA Compatible (No clasificado)";
            return "Dispositivo No Clasificado";
        case 0x01: // Almacenamiento
            switch (subclase) {
                case 0x01: return "Controlador IDE";
                case 0x06: return "Controlador SATA (AHCI)";
                case 0x08: return "Controlador NVMe (Almacenamiento No Volátil)";
                default:   return "Controlador de Almacenamiento";
            }
        case 0x02: // Red
            switch (subclase) {
                case 0x00: return "Controlador Ethernet (LAN)";
                case 0x80: return "Controlador de Red Inalámbrica / Otro";
                default:   return "Controlador de Red";
            }
        case 0x03: // Pantalla / Video
            switch (subclase) {
                case 0x00: return "Controlador VGA Compatible (Tarjeta Gráfica)";
                case 0x02: return "Controlador 3D (GPU Dedicada / Acelerador)";
                default:   return "Controlador de Pantalla / Video";
            }
        case 0x04: // Multimedia
            switch (subclase) {
                case 0x01: return "Controlador de Audio AC97";
                case 0x03: return "Controlador Intel High Definition Audio (HDA)";
                default:   return "Controlador Multimedia / Audio";
            }
        case 0x06: // Puentes
            switch (subclase) {
                case 0x00: return "Puente Host (Host Bridge / Root Complex)";
                case 0x01: return "Puente ISA (LPC)";
                case 0x04: return "Puente PCI a PCI (PCIe Root Port)";
                default:   return "Puente del Sistema (Bridge)";
            }
        case 0x0C: // Buses serie
            switch (subclase) {
                case 0x03: return "Controlador USB (UHCI/EHCI/xHCI)";
                case 0x05: return "Controlador SMBus (I2C / Gestión de Sistema)";
                default:   return "Controlador de Bus Serie";
            }
        default:
            return "Dispositivo Periférico PCI";
    }
}

static void pci_sondear_barra(uint8_t bus, uint8_t ranura, uint8_t func, int barra_idx, struct barra_pci *barra, int *es_64) {
    *es_64 = 0;
    barra->valida     = 0;
    barra->dir_base   = 0;
    barra->tamano     = 0;
    barra->es_io      = 0;
    barra->es_64bits  = 0;
    barra->predecible = 0;

    uint8_t offset = (uint8_t)(0x10 + barra_idx * 4);
    uint32_t orig_lo = pci_leer_config_32(bus, ranura, func, offset);

    // Escribir 0xFFFFFFFF para descubrir los bits cableados en el silicio
    pci_escribir_config_32(bus, ranura, func, offset, 0xFFFFFFFF);
    uint32_t mask_lo = pci_leer_config_32(bus, ranura, func, offset);
    pci_escribir_config_32(bus, ranura, func, offset, orig_lo); // Restaurar inmediatamente

    if (mask_lo == 0 || mask_lo == 0xFFFFFFFF) {
        return; // Barra no implementada
    }

    if (orig_lo & 0x01) {
        // Espacio de Puertos I/O
        barra->es_io    = 1;
        barra->dir_base = orig_lo & ~0x3;
        uint32_t tam    = ~(mask_lo & ~0x3) + 1;
        barra->tamano   = tam;
        barra->valida   = 1;
    } else {
        // Espacio de Memoria Física (MMIO)
        barra->es_io      = 0;
        barra->predecible = (orig_lo & 0x08) ? 1 : 0;
        uint8_t tipo      = (orig_lo >> 1) & 0x03;

        if (tipo == 0x02 && barra_idx < 5) {
            // BAR de 64 bits (consume barra_idx y barra_idx + 1)
            *es_64           = 1;
            barra->es_64bits = 1;
            uint8_t offset_hi = (uint8_t)(0x10 + (barra_idx + 1) * 4);
            uint32_t orig_hi  = pci_leer_config_32(bus, ranura, func, offset_hi);

            pci_escribir_config_32(bus, ranura, func, offset_hi, 0xFFFFFFFF);
            uint32_t mask_hi = pci_leer_config_32(bus, ranura, func, offset_hi);
            pci_escribir_config_32(bus, ranura, func, offset_hi, orig_hi);

            uint64_t base64 = ((uint64_t)orig_hi << 32) | (orig_lo & ~0xF);
            uint64_t mask64 = ((uint64_t)mask_hi << 32) | (mask_lo & ~0xF);
            uint64_t tam64  = ~mask64 + 1;

            barra->dir_base = base64;
            barra->tamano   = tam64;
            barra->valida   = 1;
        } else {
            // BAR de 32 bits
            barra->es_64bits = 0;
            barra->dir_base  = orig_lo & ~0xF;
            uint32_t tam     = ~(mask_lo & ~0xF) + 1;
            barra->tamano    = tam;
            barra->valida    = 1;
        }
    }
}

void pci_iniciar(void) {
    if (g_pci_inicializado) return;

    g_conteo_dispositivos = 0;
    g_gpu_primaria = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t ranura = 0; ranura < 32; ranura++) {
            // Leer función 0 primero para determinar si la ranura está poblada
            uint16_t ven0 = pci_leer_config_16((uint8_t)bus, ranura, 0, 0x00);
            if (ven0 == 0xFFFF) {
                continue;
            }

            uint8_t tipo_cab0 = pci_leer_config_8((uint8_t)bus, ranura, 0, 0x0E);
            uint8_t max_funciones = (tipo_cab0 & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_funciones; func++) {
                uint16_t ven = pci_leer_config_16((uint8_t)bus, ranura, func, 0x00);
                if (ven == 0xFFFF) {
                    continue;
                }

                if (g_conteo_dispositivos >= PCI_MAX_DISPOSITIVOS) {
                    break;
                }

                struct dispositivo_pci *dev = &g_dispositivos[g_conteo_dispositivos];
                dev->bus            = (uint8_t)bus;
                dev->ranura         = ranura;
                dev->funcion        = func;
                dev->id_proveedor   = ven;
                dev->id_dispositivo = pci_leer_config_16((uint8_t)bus, ranura, func, 0x02);
                dev->revision       = pci_leer_config_8 ((uint8_t)bus, ranura, func, 0x08);
                dev->prog_if        = pci_leer_config_8 ((uint8_t)bus, ranura, func, 0x09);
                dev->subclase       = pci_leer_config_8 ((uint8_t)bus, ranura, func, 0x0A);
                dev->clase          = pci_leer_config_8 ((uint8_t)bus, ranura, func, 0x0B);
                dev->tipo_cabecera  = pci_leer_config_8 ((uint8_t)bus, ranura, func, 0x0E);
                dev->sub_proveedor  = pci_leer_config_16((uint8_t)bus, ranura, func, 0x2C);
                dev->sub_dispositivo= pci_leer_config_16((uint8_t)bus, ranura, func, 0x2E);
                dev->linea_irq      = pci_leer_config_8 ((uint8_t)bus, ranura, func, 0x3C);
                dev->pin_irq        = pci_leer_config_8 ((uint8_t)bus, ranura, func, 0x3D);

                // Sondeo de BARs (solo cabeceras estándar tipo 00h)
                if ((dev->tipo_cabecera & 0x7F) == 0x00) {
                    for (int b = 0; b < 6; b++) {
                        int es_64 = 0;
                        pci_sondear_barra((uint8_t)bus, ranura, func, b, &dev->barras[b], &es_64);
                        if (es_64 && b < 5) {
                            // La barra b+1 es la mitad superior de la barra de 64 bits
                            dev->barras[b + 1].valida = 0;
                            dev->barras[b + 1].tamano = 0;
                            b++; // Saltar la siguiente entrada en el bucle
                        }
                    }
                }

                dev->bar0 = (uint32_t)dev->barras[0].dir_base;
                dev->bar1 = (uint32_t)dev->barras[1].dir_base;

                // Identificar GPU / Dispositivo de Pantalla
                if (dev->clase == 0x03 || (dev->clase == 0x00 && dev->subclase == 0x01)) {
                    if (!g_gpu_primaria || dev->id_proveedor == 0x10DE) {
                        g_gpu_primaria = dev; // Priorizar NVIDIA si existe
                    }
                }

                g_conteo_dispositivos++;
            }
        }
    }

    g_pci_inicializado = 1;
}

int pci_obtener_conteo(void) {
    if (!g_pci_inicializado) pci_iniciar();
    return g_conteo_dispositivos;
}

const struct dispositivo_pci *pci_obtener_dispositivo(int indice) {
    if (!g_pci_inicializado) pci_iniciar();
    if (indice < 0 || indice >= g_conteo_dispositivos) return 0;
    return &g_dispositivos[indice];
}

const struct dispositivo_pci *pci_obtener_gpu_primaria(void) {
    if (!g_pci_inicializado) pci_iniciar();
    return g_gpu_primaria;
}

int pci_buscar_dispositivo(uint16_t id_proveedor, uint16_t id_dispositivo, struct dispositivo_pci *salida) {
    if (!g_pci_inicializado) pci_iniciar();

    for (int i = 0; i < g_conteo_dispositivos; i++) {
        if (g_dispositivos[i].id_proveedor == id_proveedor &&
            g_dispositivos[i].id_dispositivo == id_dispositivo) {
            if (salida) {
                *salida = g_dispositivos[i];
            }
            return 0; // Encontrado
        }
    }
    return 1; // No encontrado
}

void pci_activar_bus_master(const struct dispositivo_pci *dev) {
    if (!dev) return;
    uint16_t comando = pci_leer_config_16(dev->bus, dev->ranura, dev->funcion, 0x04);
    // Bit 0: Espacio I/O, Bit 1: Memoria, Bit 2: Bus Master
    comando |= 0x0007;
    pci_escribir_config_16(dev->bus, dev->ranura, dev->funcion, 0x04, comando);
}
