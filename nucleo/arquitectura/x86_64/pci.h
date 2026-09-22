#ifndef ARQUITECTURA_X86_64_PCI_H
#define ARQUITECTURA_X86_64_PCI_H

#include <stdint.h>

struct dispositivo_pci {
    uint8_t  bus;
    uint8_t  ranura;
    uint8_t  funcion;
    uint16_t id_proveedor;
    uint16_t id_dispositivo;
    uint32_t bar0;
    uint32_t bar1;
    uint8_t  linea_irq;
};

uint32_t pci_leer_config_32(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento);
uint16_t pci_leer_config_16(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento);
void     pci_escribir_config_16(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento, uint16_t valor);

int  pci_buscar_dispositivo(uint16_t id_proveedor, uint16_t id_dispositivo, struct dispositivo_pci *salida);
void pci_activar_bus_master(const struct dispositivo_pci *dev);

#endif // ARQUITECTURA_X86_64_PCI_H
