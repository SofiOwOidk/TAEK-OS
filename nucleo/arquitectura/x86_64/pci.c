#include "pci.h"
#include "puertos.h"

#define PUERTO_CONFIG_DIR   0xCF8
#define PUERTO_CONFIG_DATOS 0xCFC

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

void pci_escribir_config_16(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento, uint16_t valor) {
    uint32_t dir = pci_direccion_bus(bus, ranura, funcion, desplazamiento);
    escribir_puerto_l(PUERTO_CONFIG_DIR, dir);
    escribir_puerto_w(PUERTO_CONFIG_DATOS + (desplazamiento & 2), valor);
}

int pci_buscar_dispositivo(uint16_t id_proveedor, uint16_t id_dispositivo, struct dispositivo_pci *salida) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t ranura = 0; ranura < 32; ranura++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t ven = pci_leer_config_16((uint8_t)bus, ranura, func, 0x00);
                if (ven == 0xFFFF) {
                    if (func == 0) break; // Si func 0 no existe, salta a sig ranura
                    continue;
                }

                uint16_t dev = pci_leer_config_16((uint8_t)bus, ranura, func, 0x02);
                if (ven == id_proveedor && dev == id_dispositivo) {
                    if (salida) {
                        salida->bus            = (uint8_t)bus;
                        salida->ranura         = ranura;
                        salida->funcion        = func;
                        salida->id_proveedor   = ven;
                        salida->id_dispositivo = dev;
                        salida->bar0           = pci_leer_config_32((uint8_t)bus, ranura, func, 0x10);
                        salida->bar1           = pci_leer_config_32((uint8_t)bus, ranura, func, 0x14);
                        salida->linea_irq      = (uint8_t)(pci_leer_config_32((uint8_t)bus, ranura, func, 0x3C) & 0xFF);
                    }
                    return 0; // Encontrado
                }
            }
        }
    }
    return 1; // No encontrado
}

void pci_activar_bus_master(const struct dispositivo_pci *dev) {
    if (!dev) return;
    uint16_t comando = pci_leer_config_16(dev->bus, dev->ranura, dev->funcion, 0x04);
    // Bit 0: Espacio I/O, Bit 2: Bus Master
    comando |= 0x0005;
    pci_escribir_config_16(dev->bus, dev->ranura, dev->funcion, 0x04, comando);
}
