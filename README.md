# TAEK OS (TelAvivEpsteinKirkOS)

> **Sistema Operativo x86_64 UEFI de Alto Rendimiento desarrollado desde cero en Anillo 0 (Ring 0).**

[![Licencia: MIT](https://img.shields.io/badge/Licencia-MIT-yellow.svg)](LICENSE)
[![Arquitectura](https://img.shields.io/badge/Arquitectura-x86__64%20UEFI-blue.svg)]()
[![Lenguaje](https://img.shields.io/badge/Lenguaje-C%20%7C%20NASM-brightgreen.svg)]()
[![Hardware](https://img.shields.io/badge/Silicio%20Real-Intel%208%C2%AA%20a%2014%C2%AA%20Gen-orange.svg)]()

---

## 🌐 Versión validada Hito 96 — Mensajes LAN (2026-10-02)

Comunicación bidireccional verificada físicamente entre TAEK en el **i7-8650U**
(Intel I219-LM) y una terminal Windows, conectados al router por Ethernet.
También se probaron DHCP, DNS y ping externo. La campaña actual se limita al i7;
el i9 queda aplazado para las nuevas integraciones.

El kernel y la [ISO H96](build/taek-os-h96-2026-10-02.iso) son exactamente los
binarios probados. SHA-256 de la ISO:
`fecf820f7893264cf60f8c784bb7ccc2f94e59b5660a8b9ec767792fd96a3fee`.
Identidad y hashes de fuentes: `build/release-h96.json`.

En el PC receptor en la red local (Windows / Linux), escuchar por TCP en el puerto `9151`:

En TAEK:

```text
red dhcp
transmitir destino 192.168.18.146 9151
transmitir hola windows
```

Responder desde el PC con `transmitir hola i7` (o enviando el mensaje por el socket) y ejecutar `recibir` en TAEK.
Si DHCP cambia las IP, actualizar el destino con la IP del receptor. [Guía y límites](docs/red/MENSAJES_LAN.md).
Los problemas y sus correcciones están registrados en [Hito 96](BITACORA.md).

El canal actual intercambia texto; consola remota, telemetría continua y carga
RAM de drivers se desarrollarán en la copia experimental. El inventario i915 es
pasivo y el envío de trabajos GPU permanece bloqueado.

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

### 🎬 Decodificador H.264 por Software en Ring 0 (Listo — 90%)
* Implementación **100% nativa** en C11 para Anillo 0 (*Ring 0*), sin dependencias externas, librerías de usuario ni códecs de terceros.
* **Motor AVC/H.264 maduro:** Soporte para unidades NAL, SPS/PPS, entropía CABAC completa, predicción intra e inter-cuadro, compensación de movimiento fraccionaria y filtro de desbloqueo (*in-loop deblocking filter*).
* **Aceleración Vectorial y Paralelismo:** Optimización con instrucciones SIMD SSE2 en etapas críticas y paralelismo multinúcleo SMP (escalado a 1, 2 y 4 núcleos en hardware real).
* **Demuxer de Contenedores MP4:** Parseo directo de la jerarquía ISO Base Media File Format (`moov`, `trak`, `mdia`, `stbl`, tablas de muestras y chunks) con doble buffering de audio y video.
* Conversión de espacio de color YUV420p a RGB/BGR en memoria física con volcado en tiempo real directamente sobre el *framebuffer* lineal UEFI GOP, sincronizado con pistas de audio AAC/MP3.
* *Nota de alcance (90% listo):* El 10% restante corresponde a características avanzadas o poco frecuentes no requeridas para el objetivo del sistema (tales como CAVLC heredado, video entrelazado, campos o modos exóticos de POC). Consulta [pipeline multimedia y comandos de validación](MULTIMEDIA_PIPELINE.md).

### 🎮 Decodificación por Hardware / Driver GPU Intel i915 (Experimental — En Desarrollo)
* Driver nativo para GPUs integradas Intel (arquitecturas Gen8/Gen9/Gen9.5, probado en hardware real sobre Intel Core i7-8650U con GPU UHD Graphics 620).
* Infraestructura en Ring 0 con gestión de objetos de memoria GEM, mapeo de tablas de traducción global GGTT y control de registros MMIO.
* Inicialización del motor **VCS0 (Video Command Streamer)** y parseador de estructuras VA-API bare-metal (extracción RBSP, parámetros SPS/PPS, Slice Header y buffers de comandos MFX/BSD).
* > [!WARNING]
  > **Estado Experimental:** El backend de decodificación por hardware se encuentra **en desarrollo activo**. Aún falta completar la ejecución y validación completa de lotes de fotogramas en silicio real; la submission de comandos y la sincronización por interrupciones permanecen en desarrollo en la rama experimental antes de declararse estables.

### 🌐 Transmisión por Red y Comunicación LAN (Intel Ethernet / TCP-IP)
* Controlador bare-metal para adaptadores Gigabit Ethernet Intel (validado físicamente en **Intel I219-LM** sobre el i7-8650U y probado en emulación con **e1000/e1000e**).
* Pila TCP/IPv4 autónoma en Anillo 0: resolución de direcciones ARP, cliente **DHCP** automático, cliente de resolución de nombres **DNS**, ping **ICMP** y cliente **HTTP**.
* **Mensajería Bidireccional LAN:** Protocolo de intercambio de mensajes con Windows mediante sockets TCP dedicados (puerto 9151), con cola en memoria RAM, verificación de tramas y acuses de recibo (ACK).
* **Streaming Interactivo y Telemetría:** Soporte para espejo interactivo de consola por TCP (puerto 9153) y consola remota de diagnóstico (puerto 9152), permitiendo control interactivo bidireccional desde un PC conectado a la red.

### 🐧 Capa de Compatibilidad Linux Shim
* Infraestructura de compatibilidad a nivel de kernel diseñada para facilitar la adaptación de módulos y controladores complejos (gestión de `mutex`, `waitqueue`, `workqueue`, temporizadores e interfaces RPC para el microcontrolador GSP de tarjetas gráficas modernas).

### 🐚 Terminal de Control Ring 0 (`sudo@taek-os`)
* Consola de comandos interactiva en vivo con soporte para depuración y exploración del hardware:
  * `ayuda`: Catálogo de comandos disponibles.
  * `pci`: Escaneo e inspección exhaustiva de dispositivos en el bus PCI/PCIe.
  * `gpu`: Diagnóstico especializado de la GPU, VRAM y registros MMIO.
  * `audio`: Reproducción y pruebas de los subsistemas de audio HDA/AC97.
  * `video`: Lanzador de animaciones multimedia.
  * `h264`: Reproductor de video H.264/MP4 por software (`h264 360p`, `h264 1080p`, pruebas multihilo).
  * `red`: Configuración y diagnóstico de red: `red dhcp`, `red ping <ip|host>`, `red dns <host>`, `red http <host>`, `red estado`.
  * `transmitir <mensaje>` / `recibir`: Comunicación bidireccional de mensajes con la terminal Windows por LAN (puerto 9151).
  * `stream iniciar <ip> [puerto]` / `stream detener`: Espejo interactivo de consola por TCP en tiempo real.
  * `remoto iniciar <ip> [puerto]` / `remoto detener`: Consola remota de diagnóstico por TCP.
  * `huevo`: Diagnóstico del estado del canario de integridad (`0xDEADBEEFCAFECAFE`).
  * `apagar` / `reiniciar`: Gestión de energía mediante controladores ACPI y teclado PS/2 / 8042.

---

## 🚀 Cómo Ejecutar

### 1. Grabación y Ejecución en Hardware Real (Bare Metal)
1. Flashear la imagen ISO estable (`build/taek-os-h96-2026-10-02.iso`) en una memoria USB utilizando [Rufus](https://rufus.ie/) (esquema de partición **GPT**, sistema de destino **UEFI non-CSM**) o mediante `dd` en Linux:
   ```bash
   sudo dd if=build/taek-os-h96-2026-10-02.iso of=/dev/sdX bs=4M status=progress conv=fsync
   ```
2. Conectar la memoria USB al equipo, desactivar *Secure Boot* en la BIOS/UEFI, y arrancar desde el dispositivo USB.

### 2. Ejecución en Emulador (QEMU)
Se puede arrancar directamente la ISO en QEMU x86_64 con firmware UEFI (`OVMF.fd`), tarjeta de audio Intel HDA y controlador xHCI:
```text
qemu-system-x86_64 -bios OVMF.fd -cdrom build/taek-os-h96-2026-10-02.iso -m 1024M -M q35 -device intel-hda -device hda-output -device qemu-xhci -device usb-kbd -serial stdio
```

---

## 📂 Estructura del Repositorio

```text
taek-os/
├── boot/                      # Configuración y binarios de Limine Bootloader
├── nucleo/
│   ├── arquitectura/x86_64/   # IDT, GDT, APIC, interrupciones, serial y VMX
│   ├── base/                  # Memoria, paginación, DMA, canarios del Huevo y tiempo
│   ├── controladores/         # Drivers del sistema
│   │   ├── multimedia/        # Decodificador H.264 por CPU (Listo 90%), AAC, MP3, MP4 demuxer
│   │   ├── video/             # Driver Intel i915 VCS/GEM (Experimental), GPU diagnóstico, pantalla GOP
│   │   ├── red/               # Ethernet Intel (I219-LM / e1000), pila TCP/IP, mensajes LAN, streaming
│   │   └── xhci, audio, vfs…  # USB 3.x, Audio HDA/AC97, sistemas de archivos (FAT32/exFAT/NTFS/ext4)
│   ├── compatibilidad/        # Capa Linux Shim y runtime i915
│   └── principal.c            # Punto de entrada del kernel (kmain)
├── docs/                      # Documentación técnica y guías de red
├── recursos/assets/           # Medios, texturas y pistas de audio/video
├── .gitignore                 # Filtros limpios para el repositorio
├── BITACORA.md                # Registro histórico de hitos y sesiones de ingeniería
├── LICENSE                    # Licencia MIT
├── linker.ld                  # Script de enlace ELF64
├── MULTIMEDIA_PIPELINE.md     # Documentación y validación del pipeline multimedia H.264
└── README.md                  # Descripción y estado del proyecto
```

> El árbol de integración y el estado experimental se mantienen en
> [docs/ESTRUCTURA_PROYECTO.md](docs/ESTRUCTURA_PROYECTO.md). Se revisa cada vez
> que el hito actual termina en `0` (Hito 70, 80, 90, 100, 110…).

---

## 📜 Licencia

Este proyecto está bajo la Licencia **MIT**. Consulta el archivo [LICENSE](LICENSE) para más detalles.
