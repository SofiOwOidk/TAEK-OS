#include "energia.h"
#include "../arquitectura/x86_64/puertos.h"
#include "../arquitectura/x86_64/triple_fault.h"
#include "../controladores/iommu.h"
#include "tiempo.h"

void reiniciar_equipo(void){
    uint8_t espacio,valor;uint64_t direccion;
    if(!acpi_obtener_reset(&espacio,&direccion,&valor)){
        if(espacio==1)escribir_puerto_b((uint16_t)direccion,valor);
        else *(volatile uint8_t *)(uintptr_t)direccion=valor;
        esperar_milisegundos(500);
    }
    /* Fallback reset control de chipsets PC/Intel y QEMU q35. */
    escribir_puerto_b(0xcf9,0x02);escribir_puerto_b(0xcf9,0x06);esperar_milisegundos(500);
    for(unsigned i=0;i<10000;i++){if(!(leer_puerto_b(0x64)&2)){escribir_puerto_b(0x64,0xfe);break;}esperar_io();}
    esperar_milisegundos(500);triple_fault_tier1_limpio();
}

void apagar_equipo(void) {
    // Apagado ACPI moderno en QEMU
    escribir_puerto_w(0x604, 0x2000);

    // Apagado ACPI clasico en QEMU
    escribir_puerto_w(0xB004, 0x2000);

    // Apagado en VirtualBox
    escribir_puerto_w(0x4004, 0x3400);

    // Dispositivo de salida de depuracion de QEMU (puerto 0xF4)
    escribir_puerto_b(0xF4, 0x00);

    // Si el hardware fisico o hipervisor no responde al apagado ACPI por puertos, congelar CPU
    detener_cpu();
}

void detener_cpu(void) {
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
