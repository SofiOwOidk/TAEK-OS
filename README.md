# TAEK OS (TelAvivEpsteinKirkOS)

> **Sistema Operativo x86_64 UEFI de Alto Rendimiento desarrollado desde cero en Anillo 0 (Ring 0).**

[![Licencia: MIT](https://img.shields.io/badge/Licencia-MIT-yellow.svg)](LICENSE)
[![Arquitectura](https://img.shields.io/badge/Arquitectura-x86__64%20UEFI-blue.svg)]()
[![Lenguaje](https://img.shields.io/badge/Lenguaje-C%20%7C%20NASM-brightgreen.svg)]()
[![Hardware](https://img.shields.io/badge/Silicio%20Real-Intel%208%C2%AA%20a%2014%C2%AA%20Gen-orange.svg)]()

---

## 🤖 Desarrollo y Co-Ingeniería

> **Usando apoyo de Gemini (Google DeepMind)** como copiloto de ingeniería, arquitectura de sistemas bare-metal, depuración en bajo nivel y diseño de controladores de hardware.  
> **Documentación creada por Gemini:** Bitácoras técnicas, guías de arranque en hardware real, especificaciones de controladores y manuales de referencia.

---

## 📖 Descripción General

**TAEK OS** es un sistema operativo experimental moderno diseñado específicamente para la arquitectura **x86_64** sobre firmware **UEFI nativo** (sin depender del BIOS heredado de 16 bits). El núcleo opera completamente en modo privilegiado Anillo 0 (*Ring 0*), maximizando el control directo sobre el silicio y los buses de comunicación del procesador.

El proyecto combina un desarrollo técnico de ingeniería inversa de bajo nivel (USB 3.x xHCI, controladores de audio por DMA, telemetría de hardware) con una identidad satírica de cultura de internet y humor negro, manteniendo los estándares de seguridad de memoria mediante su guardián residente: **El Huevo de la Estabilidad**.

> [!NOTE]
> **Compatibilidad en Hardware Real:** Testeado y verificado en bare metal en procesadores Intel desde la **8ª Generación (Core i7-8650U)** hasta la **14ª Generación (Core i9-14900HX)**. En generaciones o arquitecturas más allá de la 14ª Generación se desconoce si funcionará.

---

## ⚡ Características Principales

### ⌨️ Controlador USB xHCI Bare-Metal (Multi-Teclado Simultáneo)
* Implementación completa del estándar **eXtensible Host Controller Interface (xHCI)** para puertos USB 2.0 y USB 3.x.
* Inicialización de estructuras de datos físicas: *Device Context Base Address Array (DCBAA)*, *Command Ring*, *Event Ring* con *Interrupter* configurado.
* Asignación dinámica de ranuras (*Slots*), configuración de puntos de enlace (*Endpoints*) de interrupción para dispositivos HID.
* **Soporte Concurrente Multi-Teclado:** Capacidad de conectar y escribir simultáneamente con varios teclados en puertos distintos en tiempo real sobre hardware físico (validado en laptop con Intel Core i7-8650U y probado también en plataformas de escritorio MoDT con procesador Intel Core i9-14900HX).

### 🔊 Subsistema de Audio Dual (Intel HDA & AC97)
* Driver nativo para **Intel High Definition Audio (HDA)** con descubrimiento de codecs y enlace DMA cíclico (*Buffer Descriptor List*).
* Driver de audio heredado **AC97** para compatibilidad con emuladores e hipervisores.
* Motor de mezcla y streaming de muestras PCM de audio en búferes de memoria física.

### 🥚 El Huevo de la Estabilidad (*The Stability Egg*)
* Subsistema de protección de memoria y monitorización en tiempo real.
* **Canario de Memoria:** `0xDEADBEEFCAFECAFE` con verificación de integridad de cáscara en cada etapa del arranque.
* Si ocurre una violación de memoria, desbordamiento o excepción crítica de CPU (`#DE`, `#GP`, `#PF`, `#UD`), el huevo se quiebra (*SHATTERED*).
* Despliega en pantalla y puerto serie la autopsia del procesador (`RIP`, `RSP`, registros de control y código de error) junto al arte ASCII del huevo quebrado, ejecutando de inmediato un **apagado de emergencia ACPI** para proteger el hardware.

### 🦀 Multimedia y Animación en Framebuffer
* Renderizado directo en el búfer de cuadros gráfico UEFI (*Linear Framebuffer RGB/BGR*).
* Reproductor de video y animaciones integrado para secuencias gráficas de Don Cangrejo con sincronización de audio.

### 🐧 Capa de Compatibilidad Linux Shim
* Infraestructura de compatibilidad a nivel de kernel diseñada para facilitar la adaptación de módulos y controladores complejos (gestión de `mutex`, `waitqueue`, `workqueue`, temporizadores e interfaces RPC para el microcontrolador GSP de tarjetas gráficas modernas).

### 🐚 Terminal de Control Ring 0 (`sudo@taek-os`)
* Consola de comandos interactiva en vivo con soporte para depuración y exploración del hardware:
  * `ayuda`: Catálogo de comandos disponibles.
  * `pci`: Escaneo e inspección exhaustiva de dispositivos en el bus PCI/PCIe.
  * `audio`: Reproducción y pruebas de los subsistemas de audio HDA/AC97.
  * `video`: Lanzador de animaciones multimedia.
  * `huevo`: Diagnóstico del estado del canario de integridad.
  * `apagar` / `reiniciar`: Gestión de energía mediante controladores ACPI y teclado PS/2 / 8042.

---

## 🚀 Cómo Compilar y Ejecutar

### Requisitos Previos
* **Compilador C:** `clang` (con soporte para destino `x86_64-elf` o `x86_64-unknown-linux-gnu`).
* **Ensamblador:** `nasm`.
* **Herramientas de construcción:** `make`, `mtools`, `xorriso`.
* **Bootloader:** Limine (incluido en el repositorio).
* **Emulador (Opcional):** QEMU x86_64 con firmware `OVMF.fd`.

### 1. Ejecución en Emulador (QEMU)
Desde PowerShell en Windows:
```powershell
.\run.ps1
```
Este script automatiza:
1. La compilación de los módulos en C y ensamblador utilizando WSL (Arch Linux / LLVM Clang).
2. La creación del medio booteable FAT32 UEFI (`build/taek-os.img` o `.iso`).
3. El lanzamiento de **QEMU x86_64** con soporte UEFI (`OVMF`), emulación de audio HDA/AC97, puertos USB 3.0 xHCI y telemetría por consola serie en la terminal.

### 2. Ejecución en Hardware Real (Bare Metal)
1. Generar la imagen booteable:
   ```bash
   make iso
   ```
2. Flashear el archivo ISO generado (`build/taek-os.iso`) en una memoria USB utilizando [Rufus](https://rufus.ie/) (esquema de partición **GPT**, sistema de destino **UEFI non-CSM**) o mediante `dd` en Linux:
   ```bash
   sudo dd if=build/taek-os.iso of=/dev/sdX bs=4M status=progress conv=fsync
   ```
3. Conectar la memoria USB a tu equipo, desactivar *Secure Boot* en la BIOS/UEFI, y arrancar desde el dispositivo USB.

---

## 📂 Estructura del Repositorio

```text
taek-os/
├── boot/                      # Configuración y binarios de Limine Bootloader
├── nucleo/
│   ├── arquitectura/x86_64/   # IDT, GDT, APIC, interrupciones, serial y VMX
│   ├── base/                  # Memoria, DMA, canarios del Huevo y tiempo
│   ├── controladores/         # Drivers: xHCI (USB 3.x), Audio HDA/AC97, Teclado, Terminal
│   ├── principal.c            # Punto de entrada del kernel (kmain)
├── Recursos Asets/            # Medios, texturas, fuentes y pistas de audio
├── BITACORA.md                # Registro histórico de hitos y sesiones de ingeniería
├── Makefile                   # Reglas de compilación y enlace
└── run.ps1                    # Script automatizado de compilación y prueba en QEMU
```

---

## 📜 Licencia

Este proyecto está bajo la Licencia **MIT**. Consulta el archivo [LICENSE](LICENSE) para más detalles.
