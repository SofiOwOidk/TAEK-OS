# TAEK OS — Estructura e Integración del Proyecto

> **Documento mantenido por hitos.** Se revisa y actualiza cada vez que el hito
> actual termina en `0`: Hito 70, 80, 90, 100, 110, 120… Ver
> [Regla de mantenimiento](#3-regla-de-mantenimiento).

<!-- META:INICIO -->
**Versión:** `v0.1.0` · **Hito:** `Hito 67` · **Actualizado:** `2026-09-30`
<!-- META:FIN -->

**Leyenda:** 🧪 = experimental / en integración / no testeado en silicio.

---

## 1. Árbol de integración (curado)

> Esta sección se mantiene a mano: cada nodo lleva su descripción funcional.
> El listado crudo de archivos vive en el
> [inventario autogenerado](#5-inventario-autogenerado).

```text
taek-os/
│
├── README.md                     Presentación, características, build (QEMU/hardware real).
├── BITACORA.md                   Registro cronológico de hitos 0→67: decisiones, errores y soluciones.
├── PLAN_I915.md                  Plan maestro del port de i915 a Ring 0. 🧪
├── MULTIMEDIA_PIPELINE.md        Diseño y comandos de validación del pipeline multimedia.
├── LICENSE                       Licencia MIT.
├── Makefile                      Build: compila C/NASM, genera FAT32 (.img) e ISO híbrida (xorriso).
├── run.ps1                       Automatiza compilación en WSL y arranque en QEMU.
├── linker.ld                     Script de enlace ELF64 higher-half (0xFFFFFFFF80000000).
│
├── boot/                         Arranque
│   ├── limine/                   Bootloader Limine v8.x (BOOTX64.EFI, limine.c/h, binarios BIOS/UEFI).
│   └── limine.conf               Configuración del menú de arranque UEFI.
│
├── nucleo/                       Núcleo (kernel)
│   ├── principal.c               Punto de entrada `principal()`: orquesta todo el arranque.
│   │
│   ├── arquitectura/x86_64/      Capa de CPU/bus
│   │   ├── serial.c/.h           UART 16550 COM1 0x3F8 @115200 8N1 (telemetría; fallback a RAM/dmesg).
│   │   ├── gdt.c/.h              GDT 64-bit Ring 0 (selectores de código/datos).
│   │   ├── idt.c/.h + trampas.s  32 descriptores de interrupción en ASM con alineación de pila.
│   │   ├── fpu.c/.h              Habilitación/estado de FPU.
│   │   ├── smp.c + smp_entrada.s Arranque de CPUs secundarias (APs) y entrada en ASM.
│   │   ├── apic.c/.h             Local APIC, x2APIC e interrupciones MSI.
│   │   ├── pci.c/.h              Enumeración PCI/PCIe (0xCF8/0xCFC), BARs, Bus Mastering.
│   │   ├── vmx.c/.h              Hipervisor Ring -1 (Intel VMX) para capturar Triple Fault.
│   │   ├── triple_fault.c/.h     Rutas de colapso deterministas (IDTR nulo, UD2, CR3 sacrificial).
│   │   └── puertos.h             Primitivas inline de E/S (inb/outb/inw/outw).
│   │
│   ├── base/                     Servicios del núcleo
│   │   ├── memoria.c/.h          PMM y heap kmalloc.
│   │   ├── paginacion.c/.h       Memoria virtual, árbol PML4, CR3, mapeo MMIO, invlpg.
│   │   ├── dma.c/.h              Gestor de DMA contigua y coherencia de caché.
│   │   ├── huevo.c/.h            "El Huevo de la Estabilidad": canario 0xDEADBEEFCAFECAFE + autopsia ACPI.
│   │   ├── energia.c/.h          Apagado/reinicio limpio (ACPI; outw 0x604,0x2000).
│   │   ├── tiempo.c/.h           TSC calibrado con PIT 8254; esperas en ms.
│   │   ├── trabajos.c/.h         Workqueue/tareas diferidas del núcleo.
│   │   ├── utf8.c/.h             Decodificación Unicode → CP437 (tildes/ñ sin mojibake).
│   │   └── version.c/.h          Versión, hito y fecha/hora de compilación.
│   │
│   ├── compatibilidad/           Puentes para drivers externos
│   │   ├── linux.c/.h            Linux Kernel Shim: mutex, waitqueue, workqueue, temporizadores, io*, spinlocks.
│   │   ├── nv_os_interface.c/.h  Interfaz OS para la pila NVIDIA (nv_os_interface).
│   │   └── linux_i915/ 🧪        Runtime i915 en Ring 0: tareas.c (scheduler/context switch), memoria_i915.c,
│   │                             abi_test_asm.s + linux_types.h. En integración, no testeado en silicio.
│   │
│   └── controladores/            Drivers
│       ├── pantalla.c/.h         Framebuffer lineal UEFI GOP (RGB/BGR), dibujo centrado, ASCII.
│       ├── consola.c/.h          Consola de texto en framebuffer (fuente 8x16) + colores.
│       ├── terminal.c/.h         Shell `sudo@taek-os`: ayuda, pci, gpu, red, usb, audio, h264, aac, fs,
│       │                         comando `intel`, stress, apagado, easter eggs (ruleta/pray/…).
│       ├── teclado.c/.h          Teclado PS/2 (8042) + HID USB.
│       ├── audio_ac97.c/.h       Audio Intel AC97 (DMA, BDL) — emuladores/hipervisores.
│       ├── audio_hda.c/.h        Intel High Definition Audio: descubrimiento de codecs + BDL cíclico.
│       ├── animacion_cangrejo.c  Animación "Don Cangrejo explotando" (BGRA preconvertido) + audio.
│       ├── gpu.c/.h              Diagnóstico GPU genérico: MMIO BAR, VRAM, firma de silicio.
│       ├── iommu.c/.h            Intel VT-d: DRHD, RMRR, modos pass-through/traducción.
│       ├── xhci.c/.h             USB 3.x xHCI bare-metal: DCBAA, Command/Event Ring, slots/endpoints HID multi-teclado.
│       ├── usb_msc.c/.h          Almacenamiento masivo USB (BOT/SCSI).
│       ├── particiones.c/.h      Parser de tablas de particiones (MBR/GPT).
│       ├── fat_lector.c/.h       Lectura FAT de bajo nivel (oem850).
│       ├── fat32.c/.h            Sistema de archivos FAT32 (lectura/escritura).
│       ├── exfat.c/.h            Sistema de archivos exFAT.
│       ├── ntfs.c/.h             Sistema de archivos NTFS.
│       ├── ext4.c/.h             Sistema de archivos ext4.
│       ├── vfs.c/.h              VFS: selector dinámico FAT32/exFAT/NTFS/ext4 + catálogo indexado (Hito 67/68).
│       │
│       ├── red/ 🧪               Subsistema Ethernet (en integración / sin validar en silicio)
│       │   ├── red.h             API: info, enlace, TX/RX, DHCP, DNS, ARP, ICMP, HTTP.
│       │   ├── e1000.c           Driver Intel e1000/e1000e (8254x, 8257x y PCH I217/I218/I219).
│       │   └── pila.c            Pila IPv4: Ethernet+ARP+IPv4+ICMP+UDP+DHCP+DNS+TCP+HTTP (sondeo).
│       │
│       ├── video/                GPUs dedicadas/integradas
│       │   ├── intel/ 🧪         Port Intel i915 (en integración)
│       │   │   ├── intel_info.c/.h            Inventario PCI pasivo H0, BARs, IOMMU, coexistencia GOP.
│       │   │   ├── intel_diagnostico_ring0.c  Autodiagnóstico Ring 0 (scheduler/ABI, memoria, workqueue, DMA).
│       │   │   └── i915_drv.c/.h              Driver i915: detección, mapeo BAR0 MMIO, VT-d; submission -ENOSYS.
│       │   └── nvidia/           Pila NVIDIA (GSP H18–H20 descontinuada; queda inventario PCI).
│       │       ├── core/nvidia_core.c/.h      Resource Manager: máquina de estados de la GPU, BAR0/BAR1, capacidades.
│       │       ├── firmware/gsp_firmware.c/.h Carga/validación de firmware GSP.
│       │       ├── gsp/gsp_rpc.c/.h           Transporte RPC hacia el microcontrolador GSP.
│       │       └── inc/                       Cabeceras nvtypes/nvstatus/nv_gsp.
│       │
│       └── multimedia/           Decodificación y reproducción
│           ├── mp4/mp4.c/.h                   Demuxer ISO BMFF (moov/trak/mdia/stbl, tablas de muestras/chunks).
│           ├── h264/                          Decodificador H.264/AVC 100% C11 en Ring 0 (README lo llama experimental):
│           │                                  decodificador, cabac, filtro (deblocking), inter, prediccion,
│           │                                  reconstruccion, residuo, imagen, parametros, bits,
│           │                                  inter_sse2/etapas_sse2 (SIMD), tablas_cabac.
│           ├── aac/                           Decodificador AAC (Helix): aacdec, sbr, huffman, imdct, dequant, tns…
│           ├── mp3/minimp3 + reproductor_mp3  Decodificador MP3 (minimp3, con SSE2) + reproducción por ventanas VFS.
│           ├── imagen/                        Visor BMP/JPEG/PNG vía stb_image.
│           └── reproductor/                   Motor AV: reproductor.c, cache_fuente, cola_muestras, traza.
│
├── docs/intel/                   Documentación técnica del port i915 (diseño, no ejecutado): 🧪
│   ├── COMPILACION_UPSTREAM_TAEK.md
│   ├── ESTRATEGIA_GOP_FIRMWARE_RECUPERACION.md
│   ├── GRAFO_DEPENDENCIAS_I915.md
│   └── H0_VERIFICACION_FUENTES.md
│
├── herramientas/                 Utilidades de soporte al desarrollo
│   ├── identificar_fuentes.py    Genera hash de revisión de fuentes (manifest).
│   ├── informe_traza.py          Analiza trazas del reproductor.
│   ├── crear_fragmento_ram.sh / preparar_comparacion.sh / preparar_usb_1080p.sh
│   ├── aac/                      Arneses de prueba de decodificación/reproductor AAC.
│   ├── h264/                     Validación, fuzz, generación de tablas, scripts QEMU/QMP.
│   └── intel/                    Descarga de fuentes Intel y ensayo de compilación. 🧪
│
├── tests/                        Suite de pruebas (host + QEMU)
│   ├── FS: probar fat32/exfat/ntfs/ext4/vfs, comparativas mtools/linux, imágenes de prueba.
│   ├── H264: etapas, inter, rendimiento, pipeline, invariante SMP (host).
│   ├── Multimedia: pipeline, mp4 error de fuente, YUV→RGB, referencias PCM.
│   ├── Núcleo: trabajos, entrada, lectura host.
│   └── intel/ 🧪               ABI/conmutación, H1a/H1b, runtime real, casos reparados, plan H1.
│                               (la suite `pruebas_carencias_runtime.c` es simulación; H1 sigue pendiente).
│
├── terceros/                     Terceros / contratos
│   ├── intel/manifest.json       Contrato de port i915: commits fijados (Linux v6.6.78, media-driver, gmmlib,
│   │                             libva, linux-firmware) + SHA-256 de firmware. Estado: H0_NO_CERRADO. 🧪
│   └── intel/include/cxx_shim/   Shim C++ mínimo para cabeceras de terceros.
│
├── Recursos Asets/               Medios usados por el Makefile (vía enlace `recursos/`)
│   ├── fivenights.png            Imagen de bienvenida.
│   ├── damonte.mp3               Sintonía de arranque.
│   ├── cangrejo.mp4              Video/audio "Don Cangrejo".
│   ├── duelo.mp3                 Sintonía del easter egg "ruleta".
│   ├── Video 360p.mp4            Fuente de prueba H.264 (USB).
│   ├── Video 1080p.mp4           Fuente de prueba H.264 (se recorta a ≤32 MiB para RAM/USB).
│   ├── FiveNightsinTelAvivIronMouse.png
│   └── El Bueno, El Feo Y El Malo… / Qué bonito es Israel Damonte…   (audio).
│       `recursos/` es un enlace simbólico al directorio de assets.
│
├── ext4_tmp.c, ntfs_tmp.c        Apuntes/borradores de los drivers de FS (no compilados).
├── scratch_usb_tree.py           Script suelto de apoyo USB.
├── build/, "build antigua/", "build antigua+/"   Artefactos de compilación y compilaciones archivadas.
└── qemu_*.log                    Registros de arranques/pruebas en QEMU (audio, bench, h31…h47, intel_r0, test).
```

---

## 2. Estado experimental 🧪

| Subsistema | Ubicación | Estado |
|---|---|---|
| **Ethernet (red)** | `nucleo/controladores/red/` — `e1000.c`, `pila.c`, `red.h` | 🧪 En integración. Driver e1000/e1000e + pila IPv4 completa, **sin validación en silicio** (solo prevista para QEMU con `make qemu-red`). |
| **Intel i915** | `nucleo/controladores/video/intel/` + `nucleo/compatibilidad/linux_i915/` + `docs/intel/` + `terceros/intel/` | 🧪 En integración. Inventario PCI y autodiagnóstico Ring 0 pasan; **submission GPU deshabilitado** (`-ENOSYS`), GGTT/firmware/VT-d sin cerrar. Manifiesto `H0_NO_CERRADO` y plan H1 pendiente. |

> **Notas de fidelidad al repositorio** (no son las marcas 🧪 definidas por el proyecto):
> el **decodificador H.264** se describe en `README.md` como «Experimental», y
> **NVIDIA GSP (H18–H20)** está **descontinuado** (solo queda el inventario PCI).

---

## 3. Regla de mantenimiento

Este documento se **revisa y actualiza cada vez que el hito actual termina en `0`**:
Hito **70, 80, 90, 100, 110, 120…** El hito vigente está en
`nucleo/base/version.h` (`TAEK_HITO_ACTUAL`).

Procedimiento al cerrar un hito «redondo»:

1. Regenerar metadatos e inventario de archivos:
   ```bash
   python3 herramientas/generar_estructura.py     # WSL / Linux
   python  herramientas/generar_estructura.py     # Windows
   ```
   (o `make estructura`).
2. Revisar la [sección 1](#1-árbol-de-integración-curado) y añadir/actualizar la
   descripción de módulos nuevos o retirados.
3. Revisar la [sección 2](#2-estado-experimental-) y **promover o retirar** la marca 🧪
   según lo que ya esté validado en silicio.
4. Añadir una fila en la [sección 4](#4-historial-de-revisiones-del-documento).
5. Confirmar que el proyecto compila (`make`) y registrar el cierre del hito en
   `BITACORA.md`.

El script **no inventa descripciones**: solo reconstruye la lista real de archivos
entre los marcadores `INVENTARIO`, de modo que cualquier nodo nuevo o eliminado
quede visible para revisión manual.

---

## 4. Historial de revisiones del documento

| Fecha | Hito | Cambio |
|---|---|---|
| 2026-09-29 | Hito 67 | Creación del documento y primera instantánea de la estructura. |

---

## 5. Inventario autogenerado

<!-- INVENTARIO:INICIO (autogenerado por herramientas/generar_estructura.py) -->
```text
taek-os/
├── experimental/
│   ├── planes/
│   │   └── PLAN_MAESTRO_I915.md
│   ├── ext4_tmp.c
│   ├── ntfs_tmp.c
│   └── scratch_usb_tree.py
├── github/
│   ├── boot/
│   │   ├── limine/
│   │   │   ├── .gitignore
│   │   │   ├── install-sh
│   │   │   ├── LICENSE
│   │   │   ├── limine
│   │   │   ├── limine-bios-hdd.h
│   │   │   ├── limine.c
│   │   │   ├── limine.h
│   │   │   └── Makefile
│   │   └── limine.conf
│   ├── docs/
│   │   ├── intel/
│   │   │   ├── M00_LINEA_BASE_EVIDENCIA.md
│   │   │   ├── M02_INVENTARIO_PLATAFORMAS.md
│   │   │   ├── M03_FUENTES_REPRODUCIBLES.md
│   │   │   ├── M04_ENSAYO_COMPILACION_Y_GRAFO.md
│   │   │   ├── M05_CONTRATOS_TECNICOS_PORT.md
│   │   │   └── M25_MATRIZ_PLATAFORMAS_Y_CIERRE.md
│   │   └── ESTRUCTURA_PROYECTO.md
│   ├── nucleo/
│   │   ├── arquitectura/
│   │   │   └── x86_64/
│   │   │       ├── apic.c
│   │   │       ├── apic.h
│   │   │       ├── fpu.c
│   │   │       ├── fpu.h
│   │   │       ├── gdt.c
│   │   │       ├── gdt.h
│   │   │       ├── idt.c
│   │   │       ├── idt.h
│   │   │       ├── pci.c
│   │   │       ├── pci.h
│   │   │       ├── puertos.h
│   │   │       ├── serial.c
│   │   │       ├── serial.h
│   │   │       ├── smp.c
│   │   │       ├── smp_entrada.s
│   │   │       ├── trampas.s
│   │   │       ├── triple_fault.c
│   │   │       ├── triple_fault.h
│   │   │       ├── vmx.c
│   │   │       └── vmx.h
│   │   ├── base/
│   │   │   ├── dma.c
│   │   │   ├── dma.h
│   │   │   ├── energia.c
│   │   │   ├── energia.h
│   │   │   ├── huevo.c
│   │   │   ├── huevo.h
│   │   │   ├── memoria.c
│   │   │   ├── memoria.h
│   │   │   ├── paginacion.c
│   │   │   ├── paginacion.h
│   │   │   ├── tiempo.c
│   │   │   ├── tiempo.h
│   │   │   ├── trabajos.c
│   │   │   ├── trabajos.h
│   │   │   ├── utf8.c
│   │   │   ├── utf8.h
│   │   │   ├── version.c
│   │   │   └── version.h
│   │   ├── compatibilidad/
│   │   │   ├── linux_i915/
│   │   │   │   ├── abi_test_asm.s
│   │   │   │   ├── linux_types.h
│   │   │   │   ├── memoria_i915.c
│   │   │   │   ├── memoria_i915.h
│   │   │   │   ├── tareas.c
│   │   │   │   └── tareas.h
│   │   │   ├── linux.c
│   │   │   ├── linux.h
│   │   │   ├── nv_os_interface.c
│   │   │   └── nv_os_interface.h
│   │   ├── controladores/
│   │   │   ├── multimedia/
│   │   │   │   ├── aac/
│   │   │   │   │   ├── aac.c
│   │   │   │   │   ├── aac.h
│   │   │   │   │   ├── aac_memoria.c
│   │   │   │   │   ├── aac_memoria.h
│   │   │   │   │   ├── aaccommon.h
│   │   │   │   │   ├── aacdec.c
│   │   │   │   │   ├── aacdec.h
│   │   │   │   │   ├── aactabs.c
│   │   │   │   │   ├── assembly.h
│   │   │   │   │   ├── bitstream.c
│   │   │   │   │   ├── bitstream.h
│   │   │   │   │   ├── buffers.c
│   │   │   │   │   ├── coder.h
│   │   │   │   │   ├── ConfigHelix.h
│   │   │   │   │   ├── dct4.c
│   │   │   │   │   ├── decelmnt.c
│   │   │   │   │   ├── dequant.c
│   │   │   │   │   ├── fft.c
│   │   │   │   │   ├── filefmt.c
│   │   │   │   │   ├── huffman.c
│   │   │   │   │   ├── hufftabs.c
│   │   │   │   │   ├── imdct.c
│   │   │   │   │   ├── noiseless.c
│   │   │   │   │   ├── pns.c
│   │   │   │   │   ├── sbr.c
│   │   │   │   │   ├── sbr.h
│   │   │   │   │   ├── sbrfft.c
│   │   │   │   │   ├── sbrfreq.c
│   │   │   │   │   ├── sbrhfadj.c
│   │   │   │   │   ├── sbrhfgen.c
│   │   │   │   │   ├── sbrhuff.c
│   │   │   │   │   ├── sbrimdct.c
│   │   │   │   │   ├── sbrmath.c
│   │   │   │   │   ├── sbrqmf.c
│   │   │   │   │   ├── sbrside.c
│   │   │   │   │   ├── sbrtabs.c
│   │   │   │   │   ├── statname.h
│   │   │   │   │   ├── stproc.c
│   │   │   │   │   ├── tns.c
│   │   │   │   │   └── trigtabs.c
│   │   │   │   ├── h264/
│   │   │   │   │   ├── bits.c
│   │   │   │   │   ├── cabac.c
│   │   │   │   │   ├── cabac.h
│   │   │   │   │   ├── decodificador.c
│   │   │   │   │   ├── decodificador.h
│   │   │   │   │   ├── etapas_sse2.c
│   │   │   │   │   ├── etapas_sse2.h
│   │   │   │   │   ├── filtro.c
│   │   │   │   │   ├── h264.h
│   │   │   │   │   ├── imagen.c
│   │   │   │   │   ├── inter.c
│   │   │   │   │   ├── inter_kernels.h
│   │   │   │   │   ├── inter_sse2.c
│   │   │   │   │   ├── interno.h
│   │   │   │   │   ├── invariante_smp.h
│   │   │   │   │   ├── parametros.c
│   │   │   │   │   ├── prediccion.c
│   │   │   │   │   ├── reconstruccion.c
│   │   │   │   │   ├── residuo.c
│   │   │   │   │   └── tablas_cabac.h
│   │   │   │   ├── imagen/
│   │   │   │   │   ├── imagen.c
│   │   │   │   │   ├── imagen.h
│   │   │   │   │   └── stb_image.h
│   │   │   │   ├── mp3/
│   │   │   │   │   ├── LICENSE
│   │   │   │   │   ├── minimp3.h
│   │   │   │   │   ├── minimp3_impl.c
│   │   │   │   │   └── reproductor_mp3.c
│   │   │   │   ├── mp4/
│   │   │   │   │   ├── mp4.c
│   │   │   │   │   └── mp4.h
│   │   │   │   └── reproductor/
│   │   │   │       ├── anillo_almacenamiento.c
│   │   │   │       ├── anillo_almacenamiento.h
│   │   │   │       ├── cache_fuente.c
│   │   │   │       ├── cache_fuente.h
│   │   │   │       ├── cola_muestras.c
│   │   │   │       ├── cola_muestras.h
│   │   │   │       ├── reproductor.c
│   │   │   │       ├── reproductor.h
│   │   │   │       └── traza.h
│   │   │   ├── red/
│   │   │   │   ├── e1000.c
│   │   │   │   ├── pila.c
│   │   │   │   └── red.h
│   │   │   ├── video/
│   │   │   │   ├── intel/
│   │   │   │   │   ├── i915_drv.c
│   │   │   │   │   ├── i915_drv.h
│   │   │   │   │   ├── i915_gem.c
│   │   │   │   │   ├── i915_gem.h
│   │   │   │   │   ├── i915_va.c
│   │   │   │   │   ├── i915_va.h
│   │   │   │   │   ├── i915_vcs.c
│   │   │   │   │   ├── i915_vcs.h
│   │   │   │   │   ├── intel_diagnostico_ring0.c
│   │   │   │   │   ├── intel_diagnostico_ring0.h
│   │   │   │   │   ├── intel_info.c
│   │   │   │   │   └── intel_info.h
│   │   │   │   └── nvidia/
│   │   │   │       ├── core/
│   │   │   │       │   ├── nvidia_core.c
│   │   │   │       │   └── nvidia_core.h
│   │   │   │       ├── firmware/
│   │   │   │       │   ├── gsp_firmware.c
│   │   │   │       │   └── gsp_firmware.h
│   │   │   │       ├── gsp/
│   │   │   │       │   ├── gsp_rpc.c
│   │   │   │       │   └── gsp_rpc.h
│   │   │   │       └── inc/
│   │   │   │           ├── nv_gsp.h
│   │   │   │           ├── nvstatus.h
│   │   │   │           └── nvtypes.h
│   │   │   ├── animacion_cangrejo.c
│   │   │   ├── animacion_cangrejo.h
│   │   │   ├── audio_ac97.c
│   │   │   ├── audio_ac97.h
│   │   │   ├── audio_hda.c
│   │   │   ├── audio_hda.h
│   │   │   ├── consola.c
│   │   │   ├── consola.h
│   │   │   ├── exfat.c
│   │   │   ├── exfat.h
│   │   │   ├── ext4.c
│   │   │   ├── ext4.h
│   │   │   ├── fat32.c
│   │   │   ├── fat32.h
│   │   │   ├── fat_lector.c
│   │   │   ├── fat_lector.h
│   │   │   ├── fat_oem850.h
│   │   │   ├── fuente8x16.h
│   │   │   ├── gpu.c
│   │   │   ├── gpu.h
│   │   │   ├── iommu.c
│   │   │   ├── iommu.h
│   │   │   ├── ntfs.c
│   │   │   ├── ntfs.h
│   │   │   ├── pantalla.c
│   │   │   ├── pantalla.h
│   │   │   ├── particiones.c
│   │   │   ├── particiones.h
│   │   │   ├── teclado.c
│   │   │   ├── teclado.h
│   │   │   ├── terminal.c
│   │   │   ├── terminal.h
│   │   │   ├── usb_msc.c
│   │   │   ├── usb_msc.h
│   │   │   ├── usb_msc_telemetria.c
│   │   │   ├── usb_msc_telemetria.h
│   │   │   ├── vfs.c
│   │   │   ├── vfs.h
│   │   │   ├── xhci.c
│   │   │   └── xhci.h
│   │   └── principal.c
│   ├── BITACORA.md
│   ├── LICENSE
│   ├── linker.ld
│   ├── Makefile
│   ├── MULTIMEDIA_PIPELINE.md
│   ├── README.md
│   └── run.ps1
├── herramientas/
│   ├── aac/
│   │   ├── probar_audio.py
│   │   ├── probar_reproductor_av
│   │   ├── probar_reproductor_av.c
│   │   └── test_decodificar_aac.c
│   ├── h264/
│   │   ├── crear_iso.sh
│   │   ├── diagnostico.c
│   │   ├── fuzz.c
│   │   ├── generar_tablas.py
│   │   ├── qemu_p5.ps1
│   │   ├── qmp_p5.ps1
│   │   └── validar.py
│   ├── intel/
│   │   ├── descargar_fuentes_intel.sh
│   │   ├── ensayo_compilacion.sh
│   │   └── ensayo_upstream_real.sh
│   ├── crear_fragmento_ram.sh
│   ├── generar_estructura.py
│   ├── identificar_fuentes.py
│   ├── informe_traza.py
│   ├── preparar_comparacion.sh
│   └── preparar_usb_1080p.sh
├── tests/
│   ├── intel/
│   │   ├── apoyo_intel_host.c
│   │   ├── PLAN_PRUEBAS_ADAPTADORES_REALES.md
│   │   ├── probar_abi_conmutacion.sh
│   │   ├── probar_casos_reparados_h1.sh
│   │   ├── probar_correcciones_2_host.sh
│   │   ├── probar_h1_host.sh
│   │   ├── probar_m01_contencion.sh
│   │   ├── probar_m11_m16_vcs_gem.sh
│   │   ├── probar_m17_m22_va_h264.sh
│   │   ├── probar_m23_m24_robustez_rendimiento.sh
│   │   ├── probar_runtime_real.sh
│   │   ├── probar_spike_compatibilidad.sh
│   │   ├── pruebas_abi_conmutacion.c
│   │   ├── pruebas_carencias_runtime.c
│   │   ├── pruebas_casos_reparados_h1.c
│   │   ├── pruebas_correcciones_2_host.c
│   │   ├── pruebas_h1a_sincronizacion_host.c
│   │   ├── pruebas_h1b_memoria_host.c
│   │   ├── pruebas_m01_contencion.c
│   │   ├── pruebas_m11_m16_vcs_gem.c
│   │   ├── pruebas_m17_m22_va_h264.c
│   │   ├── pruebas_m23_m24_robustez_rendimiento.c
│   │   └── pruebas_runtime_reales.c
│   ├── apoyo_fs_host.c
│   ├── cerrar_especificacion_lectura.sh
│   ├── cerrar_lectura_host.sh
│   ├── cerrar_validacion_p5.sh
│   ├── comparar_fat_mtools.py
│   ├── comparar_linux_lectura.py
│   ├── compilar_lectura_host.sh
│   ├── crear_imagenes_lectura.py
│   ├── crear_matriz_lectura.py
│   ├── crear_referencia_pcm.sh
│   ├── editar_lectores.py
│   ├── editar_p3_p5.py
│   ├── editar_reproductor_p5.py
│   ├── editar_terminal_lectura.py
│   ├── editar_vfs_lectura.py
│   ├── fs_host_fuentes.sh
│   ├── generar_oem_fat.py
│   ├── h264_pool_host.h
│   ├── medir_h264_etapas.sh
│   ├── preparar_pipeline_qemu.sh
│   ├── preparar_usb_multimedia_lectura.sh
│   ├── probar_anillo_host.sh
│   ├── probar_entrada_lectura.sh
│   ├── probar_exfat_especificacion.py
│   ├── probar_exfat_host.sh
│   ├── probar_fat32_host.sh
│   ├── probar_h264_etapas_host.sh
│   ├── probar_h264_inter_host.sh
│   ├── probar_h264_p5.sh
│   ├── probar_h264_rendimiento_host.sh
│   ├── probar_imagenes_lectura.py
│   ├── probar_invariante_smp_host.sh
│   ├── probar_lectura_completa.sh
│   ├── probar_matriz_lectura.py
│   ├── probar_mp4_error_fuente.sh
│   ├── probar_pipeline_host.sh
│   ├── probar_pipeline_qemu.ps1
│   ├── probar_readahead_host.sh
│   ├── probar_stream_video_lectura.sh
│   ├── probar_trabajos_host.sh
│   ├── probar_usb_msc_telemetria_host.sh
│   ├── pruebas_anillo_host.c
│   ├── pruebas_entrada_lectura.c
│   ├── pruebas_exfat_host.c
│   ├── pruebas_fat32_host.c
│   ├── pruebas_h264_etapas_host.c
│   ├── pruebas_h264_inter_host.c
│   ├── pruebas_invariante_smp_host.c
│   ├── pruebas_lectura_host.c
│   ├── pruebas_mp4_error_fuente.c
│   ├── pruebas_pipeline_host.c
│   ├── pruebas_readahead_host.c
│   ├── pruebas_trabajos_host.c
│   ├── pruebas_usb_msc_telemetria_host.c
│   ├── pruebas_yuv_rgb_host.c
│   ├── qemu_lectura.ps1
│   └── qmp_lectura.py
└── .gitignore
```
<!-- INVENTARIO:FIN -->
