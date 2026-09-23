#ifndef ARQUITECTURA_X86_64_PCI_H
#define ARQUITECTURA_X86_64_PCI_H

#include <stdint.h>

#define PCI_MAX_DISPOSITIVOS 64

struct barra_pci {
    uint64_t dir_base;    // Dirección física base
    uint64_t tamano;      // Tamaño en bytes (calculado por sondeo)
    uint8_t  es_io;       // 1 = Puerto I/O, 0 = Memoria MMIO
    uint8_t  es_64bits;   // 1 = BAR de 64 bits, 0 = 32 bits
    uint8_t  predecible;  // 1 = Prefetchable (VRAM/Write-Combining), 0 = No
    uint8_t  valida;      // 1 = Barra configurada y activa
};

struct dispositivo_pci {
    uint8_t  bus;
    uint8_t  ranura;
    uint8_t  funcion;
    uint16_t id_proveedor;
    uint16_t id_dispositivo;
    uint8_t  clase;
    uint8_t  subclase;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  tipo_cabecera;
    uint16_t sub_proveedor;
    uint16_t sub_dispositivo;
    uint8_t  linea_irq;
    uint8_t  pin_irq;

    // Compatibilidad retroactiva
    uint32_t bar0;
    uint32_t bar1;

    // Detalle de los 6 Base Address Registers
    struct barra_pci barras[6];
};

uint32_t pci_leer_config_32(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento);
uint16_t pci_leer_config_16(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento);
uint8_t  pci_leer_config_8 (uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento);
void     pci_escribir_config_32(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento, uint32_t valor);
void     pci_escribir_config_16(uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento, uint16_t valor);
void     pci_escribir_config_8 (uint8_t bus, uint8_t ranura, uint8_t funcion, uint8_t desplazamiento, uint8_t valor);

void pci_iniciar(void);
int  pci_obtener_conteo(void);
const struct dispositivo_pci *pci_obtener_dispositivo(int indice);
const struct dispositivo_pci *pci_obtener_gpu_primaria(void);
const char *pci_nombre_proveedor(uint16_t id_proveedor);
const char *pci_nombre_clase(uint8_t clase, uint8_t subclase);

int  pci_buscar_dispositivo(uint16_t id_proveedor, uint16_t id_dispositivo, struct dispositivo_pci *salida);
void pci_activar_bus_master(const struct dispositivo_pci *dev);

#endif // ARQUITECTURA_X86_64_PCI_H
