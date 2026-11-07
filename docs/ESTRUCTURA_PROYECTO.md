# TAEK OS — Estructura e Integración del Proyecto

> **Documento mantenido por hitos.** Se revisa y actualiza cada vez que el hito
> actual termina en `0`: Hito 70, 80, 90, 100, 110, 120… Ver
> [Regla de mantenimiento](#3-regla-de-mantenimiento).

<!-- META:INICIO -->
**Versión:** `v0.1.0` · **Hito:** `Hito 96` · **Actualizado:** `2026-10-15`
<!-- META:FIN -->

**Leyenda:** 🧪 = experimental / en integración / no testeado en silicio.

---

## 1. Árbol de integración (curado)

> Esta sección se mantiene a mano y describe **hasta el segundo nivel**: cada
> carpeta de la raíz y sus subcarpetas inmediatas. El listado crudo de archivos
> vive en el [inventario autogenerado](#5-inventario-autogenerado).
>
> `build/`, `experimental/`, `OS_experimental/`, `recursos/` y `temporal/` son
> áreas locales (no se publican) y por eso quedan fuera del inventario: aquí sí
> se describen. La única carpeta publicable es `github/`.

### 1.1 Nivel 1 y 2: raíz del repositorio

```text
taek-os/                        Raíz del repositorio. Solo github/ se publica.
│
├── build/                      Salida local de compilación (ignorada por git).
│   └── fuentes.json            Manifiesto de hashes de los fuentes compilados.
│
├── docs/                       Documentación general del repositorio.
│   └── intel/                  Vacía; la documentación Intel vigente vive en
│                               OS_experimental/i915/docs/intel/.
│
├── experimental/               Apuntes sueltos fuera de la compilación: ext4_tmp.c,
│                               ntfs_tmp.c y scratch_usb_tree.py.
│
├── github/                     Último OS estable y publicable (Hito 96).
│   │
│   ├── boot/                   Arranque: limine/ (bootloader) y limine.conf (menú UEFI).
│   ├── build/                  Artefactos de la release: nucleo.elf, ISO H96 y manifiestos JSON.
│   ├── docs/                   Documentación publicada.
│   │   ├── red/                Guía MENSAJES_LAN.md + evidencias h96-evidencia/.
│   │   └── ESTRUCTURA_PROYECTO.md   Este documento.
│   ├── nucleo/                 Kernel en C/ASM (ver detalle en la sección 1.2).
│   │   ├── arquitectura/       Capa CPU y bus (subcarpeta x86_64/).
│   │   ├── base/               Servicios del núcleo: memoria, DMA, tiempo, canario.
│   │   ├── compatibilidad/     Puentes para drivers externos (Linux Shim, NVIDIA). 🧪
│   │   └── controladores/      Drivers: vídeo, consola, USB, audio, FS, red y multimedia.
│   └── recursos/               Medios del OS.
│       └── assets/             Medios del OS (imágenes, audio y vídeo) usados al construir la ISO.
│
├── herramientas/               Utilidades de soporte al desarrollo (no se publican).
│   ├── aac/                    Arneses de prueba del decodificador y reproductor AAC.
│   ├── h264/                   Validación, fuzz, generación de tablas y QEMU/QMP de H.264.
│   └── intel/                  Campaña Intel/i915: fuentes, módulos en RAM, registro de hitos
│                               y terminales de red (mensajes, stream, consola remota).
│
├── OS_experimental/            Área de trabajo del OS en desarrollo (no se publica).
│   ├── builds/                 Copias archivadas de artefactos y planes anteriores
│   │                           (vacía en esta instantánea).
│   ├── i915/                   Copia de trabajo del OS (ver detalle en la sección 1.3).
│   └── planes/                 Plan maestro i915 y planes por hito (USB, LAN, drivers
│                               en RAM, hipervisor VMX, correcciones).
│
├── recursos/                   Fuentes upstream de terceros (local, no se publica).
│   └── linux/                  Linux v6.6.78, media-driver, libva, gmmlib, shim C++
│                               y manifiestos de importación usados por el port i915.
│
├── temporal/                   Trastero de la organización anterior (no se compila).
│   ├── docs/                   Documentos previos a la reubicación (intel/ con M00–M25,
│   │                           mensajes LAN, stream de consola, diagnóstico USB).
│   ├── herramientas/           Copia anterior de herramientas/ (aac/, h264/, intel/).
│   └── tests/                  Copia anterior de tests/ (intel/, red/ y salidas de build/).
│
├── tests/                      Suite de pruebas host + QEMU (no se publica).
│   ├── build/                  Salidas compiladas y temporales de las suites.
│   ├── intel/                  Pruebas Intel/i915: ABI, módulos en RAM, DMAR, TLS, interfaz web.
│   └── red/                    Pruebas de red: mensajes LAN, stream, consola remota y telemetría.
│
├── .gitignore                  Solo se publica github/; el resto del árbol queda local.
├── AGENTS.md                   Reglas de organización del repositorio y plataforma de pruebas i915.
├── Makefile                    Recetas de compilación del OS (clang/NASM, mtools, xorriso) y
│                               objetivo `estructura`; copia heredada de la organización
│                               anterior, con rutas relativas al árbol del OS.
└── run.ps1                     Compila en WSL y arranca la ISO en QEMU desde Windows;
                                copia heredada, igual que Makefile.
```

### 1.2 Detalle del OS estable (`github/nucleo/`)

```text
github/
├── boot/
│   ├── limine/            Bootloader Limine v8.x: BOOTX64.EFI, binarios BIOS/UEFI, limine.c/h.
│   └── limine.conf        Configuración del menú de arranque UEFI.
├── nucleo/
│   ├── principal.c        Punto de entrada `principal()`: orquesta todo el arranque.
│   ├── arquitectura/x86_64/
│   │   ├── serial.c/.h            UART 16550 COM1 0x3F8 @115200 8N1 (telemetría; fallback a RAM/dmesg).
│   │   ├── gdt.c/.h               GDT 64-bit Ring 0 (selectores de código/datos).
│   │   ├── idt.c/.h + trampas.s   32 descriptores de interrupción en ASM con alineación de pila.
│   │   ├── fpu.c/.h               Habilitación y estado de la FPU.
│   │   ├── smp.c + smp_entrada.s  Arranque de CPUs secundarias (APs) y entrada en ASM.
│   │   ├── apic.c/.h              Local APIC, x2APIC e interrupciones MSI.
│   │   ├── pci.c/.h               Enumeración PCI/PCIe (0xCF8/0xCFC), BARs, bus mastering.
│   │   ├── vmx.c/.h               Hipervisor Ring -1 (Intel VMX) para capturar el triple fault.
│   │   ├── triple_fault.c/.h      Rutas de colapso deterministas (IDTR nulo, UD2, CR3 sacrificial).
│   │   └── puertos.h              Primitivas inline de E/S (inb/outb/inw/outw).
│   ├── base/
│   │   ├── memoria.c/.h           PMM y heap kmalloc.
│   │   ├── paginacion.c/.h        Memoria virtual, árbol PML4, CR3, mapeo MMIO, invlpg.
│   │   ├── dma.c/.h               Gestor de DMA contigua y coherencia de caché.
│   │   ├── huevo.c/.h             "El Huevo de la Estabilidad": canario 0xDEADBEEFCAFECAFE + autopsia ACPI.
│   │   ├── energia.c/.h           Apagado/reinicio limpio por ACPI.
│   │   ├── tiempo.c/.h            TSC calibrado con PIT 8254; esperas en ms.
│   │   ├── trabajos.c/.h          Workqueue y tareas diferidas del núcleo.
│   │   ├── utf8.c/.h              Decodificación Unicode → CP437 (tildes/ñ sin mojibake).
│   │   └── version.c/.h           Versión, hito y fecha/hora de compilación.
│   ├── compatibilidad/
│   │   ├── linux.c/.h             Linux Kernel Shim: mutex, waitqueue, workqueue, temporizadores, io*, spinlocks.
│   │   ├── nv_os_interface.c/.h   Interfaz OS para la pila NVIDIA.
│   │   └── linux_i915/ 🧪         Runtime i915 en Ring 0: tareas.c, memoria_i915.c,
│   │                              abi_test_asm.s y linux_types.h.
│   └── controladores/
│       ├── pantalla.c/.h          Framebuffer lineal UEFI GOP (RGB/BGR), dibujo centrado, ASCII.
│       ├── consola.c/.h           Consola de texto en framebuffer (fuente 8x16) + colores.
│       ├── terminal.c/.h          Shell `sudo@taek-os`: ayuda, pci, gpu, red, usb, audio, h264, fs, apagado…
│       ├── teclado.c/.h           Teclado PS/2 (8042) + HID USB.
│       ├── audio_ac97.c/.h        Audio Intel AC97 (DMA, BDL) para emuladores/hipervisores.
│       ├── audio_hda.c/.h         Intel High Definition Audio: codecs + BDL cíclico.
│       ├── animacion_cangrejo.c   Animación "Don Cangrejo explotando" (BGRA preconvertido) + audio.
│       ├── gpu.c/.h               Diagnóstico GPU genérico: MMIO BAR, VRAM, firma de silicio.
│       ├── iommu.c/.h             Intel VT-d: DRHD, RMRR, modos pass-through/traducción.
│       ├── xhci.c/.h              USB 3.x xHCI bare-metal: DCBAA, Command/Event Ring, slots y endpoints HID.
│       ├── usb_msc.c/.h           Almacenamiento masivo USB (BOT/SCSI).
│       ├── usb_msc_telemetria.c/.h  Telemetría del subsistema de almacenamiento USB.
│       ├── particiones.c/.h       Parser de tablas de particiones (MBR/GPT).
│       ├── fat_lector.c/.h        Lectura FAT de bajo nivel (oem850).
│       ├── fat32.c/.h             Sistema de archivos FAT32 (lectura/escritura).
│       ├── exfat.c/.h             Sistema de archivos exFAT.
│       ├── ntfs.c/.h              Sistema de archivos NTFS.
│       ├── ext4.c/.h              Sistema de archivos ext4.
│       ├── vfs.c/.h               VFS: selector dinámico FAT32/exFAT/NTFS/ext4 + catálogo indexado.
│       ├── red/ 🧪                e1000.c (Ethernet Intel), pila.c (IPv4/ARP/DHCP/DNS/ICMP/UDP/TCP/HTTP),
│       │                          mensajes.c (mensajería LAN) y telemetria.c; API en red.h.
│       ├── video/
│       │   ├── intel/ 🧪          i915_drv, i915_gem, i915_vcs, i915_va, intel_info y
│       │   │                      intel_diagnostico_ring0 (submission GPU deshabilitada).
│       │   └── nvidia/            Pila GSP (descontinuada): core/, firmware/, gsp/ e inc/.
│       └── multimedia/
│           ├── mp4/               Demuxer ISO BMFF (moov/trak/mdia/stbl, tablas de muestras/chunks).
│           ├── h264/              Decodificador H.264/AVC en C11: cabac, filtro, inter, predicción,
│           │                      reconstrucción, residuo, imagen, parámetros y variantes SSE2.
│           ├── aac/               Decodificador AAC (Helix): aacdec, sbr, huffman, imdct, dequant, tns…
│           ├── mp3/               Decodificador MP3 (minimp3 con SSE2) + reproducción por ventanas VFS.
│           ├── imagen/            Visor BMP/JPEG/PNG vía stb_image.
│           └── reproductor/       Motor AV: reproductor.c, cache_fuente, cola_muestras, anillo y traza.
└── (README.md, BITACORA.md, MULTIMEDIA_PIPELINE.md, LICENSE, linker.ld)
                                   Presentación, registro de hitos, pipeline multimedia, licencia MIT
                                   y script de enlace ELF64 higher-half.
```

### 1.3 Detalle del OS en desarrollo (`OS_experimental/i915/`)

```text
OS_experimental/i915/
├── nucleo/            Copia en desarrollo del kernel (arquitectura/, base/, compatibilidad/,
│                      controladores/ y principal.c), con los drivers de RAM y FS nuevos.
├── hipervisor/        Supervisor VMX mínimo: principal.c, supervisor.c, entrada.s, soporte.h, linker.ld.
├── modulos/           Módulos de prueba cargables en RAM: fibras, kthreads, tasks, completion,
│                      current, params, upstream_mm/preprobe y ratón USB.
├── boot/              limine/ (bootloader completo), limine.conf y stream.conf.
├── terceros/          Fuentes de terceros fijadas: mbedtls-3.6.7, intel/ (mesa-25.2.3 + parches,
│                      mesa.lock.json) y ca/cacert.pem.
├── docs/              Documentación por hito (H99–H124) y docs/intel/ (M00–M26, equipos/, referencias/).
├── recursos/          assets/ con los medios que usa esta copia.
├── build/             Ensayos, evidencias e imágenes ISO/ELF fechadas por hito: no son entregas estables.
└── (Makefile, run.ps1, BITACORA.md, README.md, UBICACION.md, MULTIMEDIA_PIPELINE.md, LICENSE, linker.ld)
                       Compilación y arranque QEMU de esta copia, registro de hitos, ubicación de
                       artefactos y script de enlace.
```

---

## 2. Estado experimental 🧪

| Subsistema | Ubicación | Estado |
|---|---|---|
| **Ethernet (red)** | `github/nucleo/controladores/red/` — `e1000.c`, `pila.c`, `mensajes.c`, `telemetria.c`, `red.h` | Mensajería LAN bidireccional validada físicamente en el i7-8650U (Hito 96). Las extensiones (telemetría continua, HTTPS, navegación y transferencia de archivos) se desarrollan en `OS_experimental/i915/` 🧪. |
| **Intel i915** | `github/nucleo/controladores/video/intel/` + `github/nucleo/compatibilidad/linux_i915/`; desarrollo en `OS_experimental/i915/` y fuentes en `recursos/linux/` | 🧪 En integración. La copia estable conserva inventario PCI, GEM/VCS/VA-API y autodiagnóstico Ring 0 con la **submission GPU deshabilitada** (`-ENOSYS`); el avance real (drm_mm, tasks/mutex, módulos en RAM, hipervisor) vive en la copia experimental. |

> **Notas de fidelidad al repositorio** (no son las marcas 🧪 definidas por el proyecto):
> el **decodificador H.264** se describe en `github/README.md` como «Experimental», y
> **NVIDIA GSP (H18–H20)** está **descontinuado** (solo queda el inventario PCI).

---

## 3. Regla de mantenimiento

Este documento se **revisa y actualiza cada vez que el hito actual termina en `0`**:
Hito **70, 80, 90, 100, 110, 120…** El hito vigente está en
`github/nucleo/base/version.h` (`TAEK_HITO_ACTUAL`).

Procedimiento al cerrar un hito «redondo»:

1. Regenerar metadatos e inventario de archivos:
   ```bash
   python3 herramientas/generar_estructura.py     # WSL / Linux
   python  herramientas/generar_estructura.py     # Windows
   ```
   (o `make estructura`).
2. Revisar la [sección 1](#1-árbol-de-integración-curado): niveles 1–2 (1.1) y los
   detalles por subsistema (1.2 y 1.3), añadiendo o ajustando la descripción de
   carpetas y módulos nuevos o retirados.
3. Revisar la [sección 2](#2-estado-experimental-) y **promover o retirar** la marca 🧪
   según lo que ya esté validado en silicio.
4. Añadir una fila en la [sección 4](#4-historial-de-revisiones-del-documento).
5. Confirmar que el proyecto compila (`make`) y registrar el cierre del hito en
   `github/BITACORA.md`.

El script **no inventa descripciones**: solo reconstruye la lista real de archivos
entre los marcadores `INVENTARIO`, de modo que cualquier nodo nuevo o eliminado
quede visible para revisión manual. Recuerda que `OS_experimental/`, `recursos/`,
`temporal/`, `experimental/` y `build/` están excluidos por diseño: su descripción
depende de la sección 1.

---

## 4. Historial de revisiones del documento

| Fecha | Hito | Cambio |
|---|---|---|
| 2026-09-29 | Hito 67 | Creación del documento y primera instantánea de la estructura. |
| 2026-10-15 | Hito 96 | Árbol del repositorio reorganizado a dos niveles con descripciones por carpeta, detalles del OS estable (1.2) y del experimental (1.3), rutas actualizadas a `github/`/`OS_experimental/` e inventario regenerado. |

---

## 5. Inventario autogenerado

<!-- INVENTARIO:INICIO (autogenerado por herramientas/generar_estructura.py) -->
```text
taek-os/
├── docs/
│   └── intel/
├── experimental/
│   ├── ext4_tmp.c
│   ├── ntfs_tmp.c
│   └── scratch_usb_tree.py
├── github/
│   ├── boot/
│   │   ├── limine/
│   │   │   ├── .gitignore
│   │   │   ├── LICENSE
│   │   │   ├── limine
│   │   │   ├── limine-bios-hdd.h
│   │   │   ├── limine.c
│   │   │   └── limine.h
│   │   └── limine.conf
│   ├── docs/
│   │   ├── red/
│   │   │   ├── h96-evidencia/
│   │   │   │   ├── artefactos.json
│   │   │   │   ├── qemu-resultado.json
│   │   │   │   ├── resultado.json
│   │   │   │   ├── terminal-windows.jsonl
│   │   │   │   └── validacion-mensajes.json
│   │   │   └── MENSAJES_LAN.md
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
│   │   │   │   ├── mensajes.c
│   │   │   │   ├── pila.c
│   │   │   │   ├── red.h
│   │   │   │   └── telemetria.c
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
│   ├── .gitignore
│   ├── BITACORA.md
│   ├── LICENSE
│   ├── linker.ld
│   ├── MULTIMEDIA_PIPELINE.md
│   └── README.md
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
│   │   ├── abrir_servidor_archivos.ps1
│   │   ├── abrir_terminal_mensajes.ps1
│   │   ├── abrir_terminal_remota.ps1
│   │   ├── abrir_terminal_stream.ps1
│   │   ├── analizar_mesa_port.py
│   │   ├── archivar_fuentes_h112.py
│   │   ├── capturar_referencia_i7.sh
│   │   ├── compactar_imagenes_ceros.py
│   │   ├── compilar_completion_modulo.py
│   │   ├── compilar_current_modulo.py
│   │   ├── compilar_kthread_modulo.py
│   │   ├── compilar_modulo.py
│   │   ├── compilar_modulo_params.py
│   │   ├── compilar_modulo_preprobe.py
│   │   ├── compilar_modulo_upstream_mm.py
│   │   ├── compilar_tasks_modulo.py
│   │   ├── comprobar_controlador.py
│   │   ├── conservar_fuentes_gpu.py
│   │   ├── conservar_modulos_h122.py
│   │   ├── conservar_revision_fibras.py
│   │   ├── consultar_stream.py
│   │   ├── descargar_fuentes_intel.sh
│   │   ├── ejecutar_firewall_archivos.ps1
│   │   ├── empaquetar_hito.py
│   │   ├── empaquetar_m02.py
│   │   ├── endurecer_azar_cripto.py
│   │   ├── ensayo_compilacion.sh
│   │   ├── ensayo_i915_completo.py
│   │   ├── ensayo_mesa_freestanding.py
│   │   ├── ensayo_mesa_real.py
│   │   ├── ensayo_upstream.py
│   │   ├── ensayo_upstream_real.sh
│   │   ├── evidencia_i915.py
│   │   ├── firewall_archivos.ps1
│   │   ├── generar_ca_tls.py
│   │   ├── generar_parche_c11_mesa.py
│   │   ├── generar_parche_clc_mesa.py
│   │   ├── importar_mesa.py
│   │   ├── importar_referencia_i7.py
│   │   ├── iniciar_receptor_telemetria.ps1
│   │   ├── integrar_navegador_h108.py
│   │   ├── medir_completions_originales.py
│   │   ├── medir_i915_early_probe.py
│   │   ├── medir_mutex_current.py
│   │   ├── preparar_bc_m04.sh
│   │   ├── preparar_completion_runtime.py
│   │   ├── preparar_current_fixture.py
│   │   ├── preparar_drm_mm_nativo.py
│   │   ├── preparar_h96.py
│   │   ├── preparar_i915_params.py
│   │   ├── preparar_i915_preprobe.py
│   │   ├── preparar_kthread_runtime.py
│   │   ├── preparar_mesa_host.py
│   │   ├── preparar_refcount_original.py
│   │   ├── preparar_task_runtime.py
│   │   ├── preparar_tls.py
│   │   ├── preparar_wake_queue.py
│   │   ├── probar_hipervisor_vmx.py
│   │   ├── recibir_telemetria.py
│   │   ├── reempaquetar_lan_h124.py
│   │   ├── registrar_campana.py
│   │   ├── registrar_completion_ram.py
│   │   ├── registrar_h109_fisico.py
│   │   ├── registrar_h118.py
│   │   ├── registrar_h119.py
│   │   ├── registrar_h120.py
│   │   ├── registrar_h120_fisico.py
│   │   ├── registrar_h121.py
│   │   ├── registrar_h122.py
│   │   ├── registrar_h123.py
│   │   ├── registrar_h124.py
│   │   ├── registrar_params.py
│   │   ├── registrar_preprobe.py
│   │   ├── registrar_upstream_mm_ram.py
│   │   ├── registrar_usb_h120.py
│   │   ├── registrar_usb_h124.py
│   │   ├── revisar_abi_desensamblado.py
│   │   ├── revisar_irq_desensamblado.py
│   │   ├── revisar_simd_desensamblado.py
│   │   ├── revisar_tls_elf.py
│   │   ├── servidor_archivos.py
│   │   ├── servidor_saludo_laptop.py
│   │   ├── sondear_mesa_host.py
│   │   ├── stream_headless.py
│   │   ├── terminal_mensajes.py
│   │   ├── terminal_remota.py
│   │   ├── terminal_stream.py
│   │   ├── verificar_current_host.py
│   │   ├── verificar_driver_fibras.py
│   │   ├── verificar_h122_elf.py
│   │   ├── verificar_h123_elf.py
│   │   ├── verificar_h124_elf.py
│   │   ├── verificar_i915_early_graph.py
│   │   ├── verificar_modulos_fibras.py
│   │   ├── verificar_mutantes_observador.py
│   │   ├── verificar_params_host.py
│   │   ├── verificar_preprobe_host.py
│   │   ├── verificar_tasks_host.py
│   │   ├── verificar_tasks_lifecycle_host.py
│   │   ├── verificar_tasks_states_host.py
│   │   └── verificar_tasks_timers_host.py
│   ├── crear_fragmento_ram.sh
│   ├── generar_estructura.py
│   ├── identificar_fuentes.py
│   ├── informe_traza.py
│   ├── preparar_comparacion.sh
│   ├── preparar_usb_1080p.sh
│   └── publicar_github.ps1
├── tests/
│   ├── intel/
│   │   ├── apoyo_intel_host.c
│   │   ├── PLAN_PRUEBAS_ADAPTADORES_REALES.md
│   │   ├── probar_abi_conmutacion.sh
│   │   ├── probar_abi_mecanismo.sh
│   │   ├── probar_casos_reparados_h1.sh
│   │   ├── probar_correcciones_2_host.sh
│   │   ├── probar_dmar.sh
│   │   ├── probar_h1_host.sh
│   │   ├── probar_m01_contencion.sh
│   │   ├── probar_m02_qemu.ps1
│   │   ├── probar_m11_m16_gem_vcs.sh
│   │   ├── probar_m17_m22_va_h264.sh
│   │   ├── probar_m23_m24_robustez_rendimiento.sh
│   │   ├── probar_runtime_real.sh
│   │   ├── probar_scheduler_smp_host.sh
│   │   ├── probar_simd_mecanismo.sh
│   │   ├── probar_spike_compatibilidad.sh
│   │   ├── pruebas_abi_conmutacion.c
│   │   ├── pruebas_abi_contexto_qemu.py
│   │   ├── pruebas_abi_mecanismo.c
│   │   ├── pruebas_archivos_qemu.py
│   │   ├── pruebas_c11_mesa_host.c
│   │   ├── pruebas_carencias_runtime.c
│   │   ├── pruebas_casos_reparados_h1.c
│   │   ├── pruebas_completion_modulo_fisico.py
│   │   ├── pruebas_completion_modulo_qemu.py
│   │   ├── pruebas_correcciones_2_host.c
│   │   ├── pruebas_current_fibras_host.c
│   │   ├── pruebas_current_modulo_qemu.py
│   │   ├── pruebas_dmar.c
│   │   ├── pruebas_dmar_qemu.py
│   │   ├── pruebas_driver_fibras.c
│   │   ├── pruebas_driver_lifecycle_qemu.py
│   │   ├── pruebas_drivers_qemu.py
│   │   ├── pruebas_entre_discos_fisico.py
│   │   ├── pruebas_entre_discos_qemu.py
│   │   ├── pruebas_fibras_modulo_qemu.py
│   │   ├── pruebas_h1a_sincronizacion_host.c
│   │   ├── pruebas_h1b_memoria_host.c
│   │   ├── pruebas_https_qemu.py
│   │   ├── pruebas_interfaz_web_qemu.py
│   │   ├── pruebas_irq_qemu.py
│   │   ├── pruebas_kthread_modulo_qemu.py
│   │   ├── pruebas_m01_contencion.c
│   │   ├── pruebas_m11_m16_gem_vcs.c
│   │   ├── pruebas_m17_m22_va_h264.c
│   │   ├── pruebas_m23_robustez_rendimiento.c
│   │   ├── pruebas_mouse_qemu.py
│   │   ├── pruebas_params_host.c
│   │   ├── pruebas_params_modulo_qemu.py
│   │   ├── pruebas_preprobe_host.c
│   │   ├── pruebas_preprobe_modulo_qemu.py
│   │   ├── pruebas_runtime_reales.c
│   │   ├── pruebas_scheduler_smp_host.c
│   │   ├── pruebas_simd_mecanismo.c
│   │   ├── pruebas_tasks_host.c
│   │   ├── pruebas_tasks_lifecycle_host.c
│   │   ├── pruebas_tasks_modulo_qemu.py
│   │   ├── pruebas_tasks_states_host.c
│   │   ├── pruebas_tasks_timers_host.c
│   │   ├── pruebas_tls_qemu.py
│   │   ├── pruebas_upstream_mm_host.c
│   │   ├── pruebas_upstream_mm_modulo_qemu.py
│   │   ├── pruebas_web_controladas.py
│   │   ├── qemu_consola.py
│   │   └── servidor_https_negativo.py
│   ├── red/
│   │   ├── probar_historial_nativo.py
│   │   ├── probar_lan_auto_arranque.py
│   │   ├── probar_mensajes_host.sh
│   │   ├── probar_mensajes_qemu.py
│   │   ├── probar_remoto_host.sh
│   │   ├── probar_remoto_qemu.py
│   │   ├── probar_stream_host.sh
│   │   ├── probar_stream_qemu.py
│   │   ├── probar_telemetria_host.sh
│   │   ├── probar_telemetria_qemu.py
│   │   ├── pruebas_archivos_servidor.py
│   │   ├── pruebas_mensajes_host.c
│   │   ├── pruebas_receptor_telemetria.py
│   │   ├── pruebas_remoto_host.c
│   │   ├── pruebas_stream_host.c
│   │   ├── pruebas_telemetria_host.c
│   │   ├── pruebas_terminal_mensajes.py
│   │   ├── pruebas_terminal_remota.py
│   │   ├── pruebas_terminal_stream.py
│   │   └── validar_stream_entrega.py
│   ├── apoyo_fs_host.c
│   ├── cerrar_especificacion_lectura.sh
│   ├── cerrar_lectura_host.sh
│   ├── cerrar_validacion_p5.sh
│   ├── comparar_fat_mtools.py
│   ├── comparar_linux_lectura.py
│   ├── compilar_lectura_host.sh
│   ├── crear_fixtures_operaciones.py
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
│   ├── inspeccionar_exfat_imagen.py
│   ├── medir_h264_etapas.sh
│   ├── preparar_pipeline_qemu.sh
│   ├── preparar_usb_multimedia_lectura.sh
│   ├── preparar_vectores_cripto.py
│   ├── probar_anillo_host.sh
│   ├── probar_cripto_real.sh
│   ├── probar_driver_compilacion.sh
│   ├── probar_driver_lifecycle.sh
│   ├── probar_entrada_lectura.sh
│   ├── probar_entre_discos.sh
│   ├── probar_exfat_especificacion.py
│   ├── probar_exfat_host.sh
│   ├── probar_exfat_transfer_host.sh
│   ├── probar_fat32_host.sh
│   ├── probar_fat32_transfer_host.sh
│   ├── probar_fat_operaciones_host.sh
│   ├── probar_h264_etapas_host.sh
│   ├── probar_h264_inter_host.sh
│   ├── probar_h264_p5.sh
│   ├── probar_h264_rendimiento_host.sh
│   ├── probar_imagenes_lectura.py
│   ├── probar_invariante_smp_host.sh
│   ├── probar_lectura_completa.sh
│   ├── probar_matriz_lectura.py
│   ├── probar_mouse_host.sh
│   ├── probar_mp4_error_fuente.sh
│   ├── probar_pipeline_host.sh
│   ├── probar_pipeline_qemu.ps1
│   ├── probar_readahead_host.sh
│   ├── probar_regresion_h110.sh
│   ├── probar_stream_video_lectura.sh
│   ├── probar_tls_port_kernel.sh
│   ├── probar_trabajos_host.sh
│   ├── probar_usb_cache_host.sh
│   ├── probar_usb_msc_telemetria_host.sh
│   ├── probar_web_historial.sh
│   ├── probar_x509_host.sh
│   ├── probar_xhci_bulk_host.sh
│   ├── pruebas_anillo_host.c
│   ├── pruebas_archivo_comando.c
│   ├── pruebas_consola_host.c
│   ├── pruebas_driver_elf.c
│   ├── pruebas_entrada_lectura.c
│   ├── pruebas_entre_discos.c
│   ├── pruebas_exfat_host.c
│   ├── pruebas_exfat_transfer_host.c
│   ├── pruebas_fat32_host.c
│   ├── pruebas_fat32_transfer_host.c
│   ├── pruebas_fat_operaciones_host.c
│   ├── pruebas_h264_etapas_host.c
│   ├── pruebas_h264_inter_host.c
│   ├── pruebas_http_respuesta.c
│   ├── pruebas_invariante_smp_host.c
│   ├── pruebas_lectura_host.c
│   ├── pruebas_mouse_modulo.c
│   ├── pruebas_mouse_transporte.c
│   ├── pruebas_mp4_error_fuente.c
│   ├── pruebas_pipeline_host.c
│   ├── pruebas_puntero.c
│   ├── pruebas_readahead_host.c
│   ├── pruebas_trabajos_host.c
│   ├── pruebas_usb_cache_host.c
│   ├── pruebas_usb_msc_telemetria_host.c
│   ├── pruebas_web_historial.c
│   ├── pruebas_web_url.c
│   ├── pruebas_x509_host.c
│   ├── pruebas_x509_negativas.py
│   ├── pruebas_xhci_bulk_driver_host.c
│   ├── pruebas_xhci_bulk_host.c
│   ├── pruebas_xhci_halt_host.c
│   ├── pruebas_yuv_rgb_host.c
│   ├── qemu_lectura.ps1
│   ├── qmp_lectura.py
│   ├── test_26bit.c
│   ├── test_cripto_all.c
│   ├── test_cripto_check
│   ├── test_cripto_check.c
│   ├── test_donna.c
│   └── test_tls13_client.c
├── .gitignore
├── AGENTS.md
├── Makefile
└── run.ps1
```
<!-- INVENTARIO:FIN -->
