#include "energia.h"
#include "../arquitectura/x86_64/puertos.h"

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
