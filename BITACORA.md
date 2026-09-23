# Bitácora de Desarrollo e Historial de Cambios: TAEK OS
> **Proyecto:** TAEK OS (TelAvivEpsteinKirkOS)  
> **Arquitectura:** x86_64 UEFI Freestanding (Ring 0)  
> **Procesador Objetivo:** Intel Core i9-14900HX Compatible  
> **Regla del Proyecto:** Toda modificación técnica debe quedar registrada con fecha, hora, archivos modificados, decisiones de diseño, errores encontrados y su solución para trazabilidad total.

---

## 📅 Registro Cronológico de Cambios

### [2026-09-22 15:52 - 16:05] — Hito 0: Infraestructura Base, Limine UEFI y «El Huevo de la Estabilidad»
* **Objetivo:** Construir el chasis mínimo del sistema operativo en 64 bits con telemetría serial, captura de excepciones y el subsistema de vigilancia.
* **Archivos Creados:**
  * `boot/limine/`: Binarios y protocolo Limine v8 (`BOOTX64.EFI`, `limine.h`).
  * `boot/limine.conf`: Configuración del menú de arranque UEFI.
  * `linker.ld`: Script de enlace ELF64 higher-half (`0xFFFFFFFF80000000`).
  * `nucleo/arquitectura/x86_64/puertos.h`: Primitivas inline de puertos I/O (`inb`, `outb`, `inw`, `outw`).
  * `nucleo/arquitectura/x86_64/serial.c / .h`: Controlador UART 16550 COM1 (`0x3F8`) a 115200 baudios, 8N1.
  * `nucleo/arquitectura/x86_64/gdt.c / .h`: Recarga de la GDT de 64 bits con selectores de código y datos Ring 0.
  * `nucleo/arquitectura/x86_64/idt.c / .h` y `trampas.s`: 32 descriptores de interrupción en ensamblador con alineación de pila.
  * `nucleo/base/huevo.c / .h`: Subsistema de vigilancia activa con canario de memoria `0xDEADBEEFCAFECAFE` y arte ASCII.
  * `nucleo/base/energia.c / .h`: Rutinas de apagado de emergencia (ACPI / `outw(0x604, 0x2000)`).
  * `nucleo/principal.c`: Punto de entrada `void principal(void)`.
  * `Makefile` y `run.ps1`: Automatización de compilación con Clang/LLD en WSL y ejecución en QEMU Windows.
* **Pruebas y Verificación:**
  * Happy Path: Arranque exitoso en QEMU UEFI con telemetría serial completa y detección de Framebuffer GOP a 1280x800x32bpp.
  * Stress Test: Inyección forzada de división por cero (`42 / 0` -> `#DE`). El Huevo capturó la excepción, imprimió el volcado del registro `RIP` y apagó la máquina virtual al instante.

---

### [2026-09-22 16:11 - 16:16] — Hito 1: Castellanización Total de la Arquitectura
* **Objetivo:** Cumplir con la premisa de diseño de crear un kernel 100% en español por rebeldía tecnológica frente al estándar en inglés de Silicon Valley.
* **Cambios Realizados:**
  * Renombrado de directorios: `kernel/` -> `nucleo/`, `core/` -> `base/`, `drivers/` -> `controladores/`.
  * Punto de entrada: `kmain` -> `principal` (actualizado en `linker.ld` con `ENTRY(principal)`).
  * Funciones en español: `puertos.h` (`escribir_puerto_b`, `leer_puerto_b`), `serial_iniciar()`, `serial_imprimir_linea()`, `gdt_iniciar()`, `idt_iniciar()`, `manejador_excepciones()`, `huevo_iniciar()`, `huevo_etapa()`, `huevo_agrietar()`, `huevo_quebrar()`, `apagar_equipo()`.
  * Nombres de las 32 excepciones de la CPU traducidas al español en el volcado forense.
* **Resultado:** El binario compiló y enlazó de forma transparente bajo el nuevo estándar.

---

### [2026-09-22 16:17 - 16:22] — Hito 2: Soporte Nativo para la Letra Ñ y Caracteres Especiales
* **Objetivo:** Resolver el problema de mojibake donde la letra `ñ` se imprimía como `├▒`.
* **Causa Raíz:** En codificación UTF-8 la `ñ` son 2 bytes (`0xC3 0xB1`). En la consola tradicional con página de códigos CP437, esos bytes representan los caracteres gráficos de caja `├` y `▒`.
* **Archivos Creados / Modificados:**
  * `nucleo/base/utf8.h / .c`: Módulo decodificador de secuencias Unicode multibyte y mapeo al mapa extendido CP437 (`ñ` -> `0xA4`, `Ñ` -> `0xA5`, `á` -> `0xA0`, etc.).
  * `run.ps1`: Configuración forzada de la consola de Windows a UTF-8 mediante `chcp 65001` y `[Console]::OutputEncoding = UTF-8`.
  * `nucleo/principal.c`: Incorporada etapa de validación en el arranque con `[Ñ, ñ, á, é, í, ó, ú, ¡, ¿]`.
* **Resultado:** Caracteres en español se muestran de forma nítida y nativa sin artefactos en la terminal.

---

### [2026-09-22 16:23 - 16:27] — Hito 3: Reubicación en Espacio de Trabajo Oficial
* **Objetivo:** Mover el repositorio al espacio de trabajo del usuario en `C:\Users\Pat\AndroidStudioProjects\taek-os`.
* **Cambios Realizados:**
  * Copia integra del proyecto y estructura modular.
  * `run.ps1` actualizado para resolver dinámicamente la ruta de WSL mediante `wsl wslpath -u "$PSScriptRoot"`, haciéndolo 100% portable a cualquier ruta o equipo.
  * Recompilación limpia y verificación en el nuevo directorio.

---

### [2026-09-22 16:30 - 16:38] — Hito 4: Pantalla de Bienvenida Gráfica y Audio de Encendido AC97
* **Objetivo:** Mostrar la imagen `FiveNightsinTelAvivIronMouse.png` en pantalla y reproducir la sintonía `Qué bonito es Israel Damonte.mp3` durante 9 segundos al arrancar.
* **Archivos Creados / Modificados:**
  * `nucleo/controladores/pantalla.h / .c`: Controlador del Framebuffer UEFI GOP. Limpieza en negro (`0x00000000`) y función `pantalla_dibujar_imagen_centrada()`.
  * `nucleo/arquitectura/x86_64/pci.h / .c`: Escáner de bus PCI en puertos `0xCF8`/`0xCFC` para localizar dispositivos y activar Bus Mastering.
  * `nucleo/controladores/audio_ac97.h / .c`: Controlador de audio Intel AC97 (Vendor `0x8086`, Device `0x2415`). Traducción de memoria virtual a física, configuración de la lista de descriptores BDL y reproducción por DMA directo a 44.1 kHz.
  * `nucleo/base/tiempo.h / .c`: Calibración del contador TSC con el chip PIT 8254 e implementación de `esperar_milisegundos()`.
  * `Makefile`: Reglas automáticas para convertir con `ffmpeg` el PNG a BGRA32 crudo y el MP3 a PCM estéreo de 16 bits, incrustándolos en `nucleo.elf` con `objcopy`.
  * `run.ps1`: Añadidos parámetros de QEMU `-audiodev dsound,id=snd0 -device AC97,audiodev=snd0`.
  * `nucleo/principal.c`: Orquestación del arranque con dibujo de la imagen, lanzamiento del audio DMA y conteo regresivo de 9 segundos en la consola serial.
### [2026-09-22 16:45 - 16:55] — Hito 5: Hipervisor en Ring -1 (Intel VMX) y Protocolo de Autodestrucción Don Cangrejo
* **Objetivo:** Implementar la infraestructura de virtualización por hardware (Intel VMX Root Operation / Ring -1) para interceptar colapsos totales (Triple Faults) y reproducir la animación de Don Cangrejo explotando con audio antes de apagar el equipo.
* **Inspección y Caracterización del Video (`Don cangrejo explota meme.mp4`):**
  * **Resolución nativa:** 288 x 360 píxeles.
  * **Tasa de fotogramas:** 30.00 FPS.
  * **Duración y conteo:** 49 fotogramas (~1.63 segundos).
  * **Pista de Audio:** AAC 44.1 kHz estéreo.
  * **Estrategia de Decodificación:** Para evitar la sobrecarga y complejidad inmanejable de un decodificador H.264/MP4 en Ring -1 / Anillo 0, se aplicó pre-conversión en el `Makefile` usando `ffmpeg` a fotogramas planos BGRA32 (49 * 288 * 360 * 4 ≈ 20 MB) y audio PCM crudo de 16 bits firmado a 44.1 kHz (~304 KB).
* **Archivos Creados / Modificados:**
  * `nucleo/arquitectura/x86_64/vmx.h / .c`:
    * Detección de soporte VMX en el procesador mediante CPUID (Hoja 1, bit 5 de ECX).
    * Configuración y desbloqueo del MSR `IA32_FEATURE_CONTROL` (`0x3A`, bits 0 y 2).
    * Habilitación del bit VMXE (bit 13) en el registro de control `CR4`.
    * Lectura del VMCS Revision ID desde el MSR `IA32_VMX_BASIC` (`0x480`).
    * Asignación alineada a 4096 bytes de la región VMXON y cálculo de su dirección física mediante el protocolo de direcciones de Limine.
    * Ejecución de la instrucción ensamblador `vmxon` para ingresar en VMX Root Operation (Ring -1).
  * `nucleo/controladores/animacion_cangrejo.h / .c`:
    * Motor de renderizado directo al Framebuffer físico UEFI GOP de los 49 fotogramas de 288x360 a 30 FPS (33 ms por fotograma mediante calibración TSC/PIT).
    * Disparo simultáneo del audio de explosión vía DMA en el chip PCI Intel AC97.
  * `nucleo/base/huevo.c`:
    * Conexión directa: cuando el huevo sufre una fractura fatal (`huevo_quebrar`), antes de apagar el hardware invoca a `animacion_don_cangrejo_explotar(2)` para realizar la secuencia de explosión en bucle.
  * `Makefile`:
    * Nuevas reglas de extracción de video/audio de `cangrejo.mp4` hacia objetos ELF64 (`cangrejo_video.o`, `cangrejo_audio.o`) con `objcopy`.
    * Expansión de la imagen UEFI FAT32 a 128 MB para alojar los 23 MB del kernel con assets pre-renderizados.
* **Pruebas y Verificación:**
  * Compilación y enlace limpio con Clang 22 y LLD.
  * Ejecución exitosa en QEMU UEFI: el hipervisor entra en VMX Root Operation, el huevo valida todas las etapas del arranque sin fallos, el audio suena durante 9 segundos y la máquina queda lista.

---

### [2026-09-22 17:15 - 17:21] — Hito 6: Terminal Gráfica Interactiva, Usuario «sudo» y Meme «sudo rm -rf /»
* **Objetivo:** Implementar una terminal interactiva en Anillo 0 que arranque tras la secuencia musical de bienvenida, con el usuario `sudo` como usuario supremo (root por defecto), renderizado de texto gráfico sobre el Framebuffer GOP y el meme catastrófico de `sudo rm -rf /` conectado a Don Cangrejo.
* **Archivos Creados / Modificados:**
  * `nucleo/controladores/fuente8x16.h`: Matriz de fuente bitmap 8x16 CP437 completa (256 glifos, 4096 bytes) con soporte para caracteres españoles (`ñ`, `Ñ`, `á`, `é`, etc.) y caracteres de bloque/caja (`█`, `_`).
  * `nucleo/controladores/teclado.h / .c`: Controlador de teclado PS/2 (puerto `0x60`/`0x64`) con Scancode Set 1, teclas modificadoras (Shift, Caps Lock) y decodificación no bloqueante.
  * `nucleo/arquitectura/x86_64/serial.h / .c`: Implementadas funciones de lectura `serial_hay_datos()` y `serial_leer_caracter()` para permitir escribir también desde la consola serial.
  * `nucleo/controladores/pantalla.h / .c`: Implementadas primitivas `pantalla_dibujar_caracter()` (dibujo directo de 8x16 píxeles a memoria de video) y `pantalla_desplazar_arriba()` (hardware scrolling copiando líneas en 64 bits con limpieza inferior).
  * `nucleo/controladores/consola.h / .c`: Consola gráfica con cursor de bloque parpadeante, salto de línea, retroceso (`\b`), decodificación UTF-8 en vivo y entrada dual (PS/2 + Serial COM1).
  * `nucleo/controladores/terminal.h / .c`: Terminal interactiva con el prompt `sudo@taek-os:~# `. Comandos soportados:
    * `ayuda`: Menú de asistencia en español.
    * `huevo`: Diagnóstico en tiempo real y arte ASCII de El Huevo.
    * `info`: Datos técnicos del CPU x86_64, VMX Root y Framebuffer.
    * `musica`: Reproducción de la sintonía por AC97 DMA.
    * `cangrejo`: Ejecución manual de Don Cangrejo explotando.
    * `calc <expr>`: Calculadora aritmética básica (estilo HolyC / C).
    * `quiensoy`: Identidad del usuario (`sudo`).
    * `eco <texto>`: Eco de texto.
    * `limpiar` / `cls`: Limpieza de pantalla.
    * `apagar`: Apagado limpio vía ACPI.
    * `sudo rm -rf /`: Meme supremo: advierte del colapso del sistema, quiebra El Huevo y detona el protocolo Don Cangrejo en Ring -1 antes de apagar el equipo.
  * `nucleo/principal.c`: Transición automática desde el splash screen y la música hacia `terminal_ejecutar()`.
  * `Makefile`: Regla de generación de `taek-os.img` convertida a incremental (`mcopy -o`) para permitir compilar y actualizar el kernel sin necesidad de cerrar QEMU ni sufrir bloqueos de archivos en Windows.
* **Pruebas y Verificación:**
  * Happy Path: Arranque completo, prompt `sudo@taek-os:~# `, ejecución de `ayuda`, `quiensoy` y `calc 40 + 2` -> `42 (0x2A)`.
  * Meme Path: Ejecución de `sudo rm -rf /` desencadenó la advertencia, fracturó El Huevo, lanzó los 2 bucles de Don Cangrejo con audio y apagó la máquina virtual con código 0.

---

### [2026-09-22 17:28 - 17:33] — Hito 7: Mecánica de «Ruleta Rusa» y Desbloqueo de «El comando»
* **Objetivo:** Incorporar el comando `ruleta` con probabilidad 1 de 6 de borrar el sistema y 1 de 6 de que el kernel pierda, desbloqueando el comando secreto `"El comando"` (que no hace nada).
* **Mecánica del Juego:**
  1. El cilindro de 6 recámaras gira (`obtener_aleatorio() % 6`).
  2. **Turno del usuario:** 1 de 6 de detonación. Si toca la bala, El Huevo se quiebra fatalmente (`huevo_quebrar`), reproduciendo la explosión de Don Cangrejo en bucle y apagando el sistema.
  3. **Turno del sistema:** Si el jugador sobrevive, el sistema operativo aprieta el gatillo contra sí mismo (1 de 6). Si toca la bala, el kernel pierde y desbloquea el comando `"El comando"`.
  4. **"El comando":** Si no está desbloqueado, la terminal indica que está bloqueado. Una vez desbloqueado, el comando se ejecuta y no hace absolutamente nada.
* **Archivos Modificados:**
  * `nucleo/controladores/terminal.c`:
    * Implementación de `obtener_aleatorio()` con entropía combinada del contador de ciclos `rdtsc` y el algoritmo de permutación Xorshift32.
    * Lógica de turnos en `ruleta` con pausas y efectos de texto.
    * Manejo y desbloqueo de `El comando` (sin distinción de mayúsculas o comillas).
* **Pruebas y Verificación:**
  * Al ejecutar `ruleta`, el jugador sobrevivió (*¡CLIC!*), el kernel disparó la recámara cargada (*¡PUM!*), se desbloqueó `"El comando"`, y al invocar `El comando` no hizo absolutamente nada con total éxito.

---

### [2026-09-22 17:33 - 17:36] — Hito 8: Banda Sonora de Duelo en Ruleta Rusa («El Bueno, El Feo y El Malo»)
* **Objetivo:** Reproducir en bucle la sintonía del Spaghetti Western (`El Bueno, El Feo Y El Malo - II Buono, II Brutto, Il Cattivo.mp3`) de fondo durante el enfrentamiento 1 vs 1 de la Ruleta Rusa contra el sistema.
* **Pipeline de Audio:**
  * Extracción de la icónica frase melódica inicial (silbido + arpegio de guitarra, 11 segundos) a PCM estéreo de 16 bits sin compresión a 44.1 kHz (~1.9 MB).
  * Enlace mediante `objcopy` como objeto ELF64 (`duelo_audio.o`) en `Makefile`.
* **Archivos Modificados:**
  * `recursos/duelo.mp3`: Enlace simbólico al MP3 con espacios en el nombre para compatibilidad con GNU Make.
  * `Makefile`: Regla para generar `duelo_audio.bin` y enlazar `duelo_audio.o` en `nucleo.elf`.
  * `nucleo/controladores/terminal.c`:
    * Función `esperar_con_audio_bucle(ms, audio, tam)`: monitoriza el estado del DMA AC97 y reinicia la reproducción de inmediato si el audio termina mientras el duelo continúa.
    * Activación de la pista musical al iniciar el duelo y detención limpia si el jugador gana o hay empate.
* **Pruebas y Verificación:**
  * En QEMU, al ejecutar `ruleta`, el silbido legendario suena de fondo durante el suspenso y los turnos, deteniéndose limpiamente al resolverse el enfrentamiento.

---

### [2026-09-22 18:05 - 18:15] — Hito 9: Gestor de Memoria Dinámica (PMM + Kernel Heap kmalloc/kfree) y Comando 'memoria'
* **Objetivo:** Cumplir el Paso 1 de la hoja de ruta solicitada por el usuario: implementar un gestor de memoria física (PMM) y un asignador dinámico en el núcleo (Kernel Heap) vigilado por "El Huevo de la Estabilidad", proveyendo shims de compatibilidad con Linux (`kmalloc`, `kfree`, `kzalloc`) y un comando interactivo `memoria` con autodiagnóstico en vivo.
* **Diseño Arquitectónico:**
  1. **PMM (Page Frame Allocator de 4 KiB):**
     - Consume el mapa de memoria UEFI entregado por Limine (`LIMINE_MEMMAP_REQUEST`) y el Higher Half Direct Map (`LIMINE_HHDM_REQUEST`).
     - Utiliza una **pila intrusiva de marcos libres** en el espacio virtual mapeado por el HHDM: cada página libre de 4096 bytes aloja en sus primeros 8 bytes la dirección física de la página anterior en la pila.
     - Complejidad $O(1)$ pura tanto para `pmm_asignar_pagina_fisica()` como para `pmm_liberar_pagina_fisica()`, con **0 bytes** de memoria desperdiciada en tablas de mapa de bits.
  2. **Kernel Heap Allocator (Memoria Dinámica):**
     - Arena inicial continua de 8 MiB (`TAMANO_ARENA_INICIAL`) reservada al inicio de la memoria física utilizable.
     - Lista doblemente enlazada de bloques con división (*splitting*) en asignación y fusión (*coalescing*) bidireccional con bloques adyacentes continuos al liberar.
     - **Canarios de Seguridad de El Huevo:** Cada bloque cuenta con cabecera y pie vigilados: `canario_inicio = 0x7AEECC05` ("TAEK OS") y `canario_fin = 0xCAFEBABEDEAD1000`. Si un puntero se desborda (*buffer overflow*) o se intenta liberar dos veces (*double free*), El Huevo se quiebra de inmediato invocando el protocolo Don Cangrejo.
     - **Shims de Compatibilidad Linux:** Implementados en `memoria.h`: `kmalloc(size, flags)`, `kzalloc(size, flags)`, `kfree(ptr)`, `krealloc(ptr, size, flags)`, `vmalloc(size)`, `vfree(ptr)` con banderas `GFP_KERNEL`, `GFP_ATOMIC`, `GFP_DMA`.
  3. **Comando de Terminal `memoria`:**
     - Reporta: RAM física total, RAM usable, páginas de 4 KiB (totales, libres y en uso), capacidad del Heap del Kernel, memoria asignada y libre, bloques activos y estado de los canarios.
     - Subcomando `memoria probar`: Batería de 7 pruebas en vivo (asignación `kmalloc`, `asignar_memoria`, comprobación de ceros en `kzalloc`, redimensionamiento con `krealloc`, marco físico de 4 KiB en PMM, auditoría de canarios con El Huevo, y liberación/coalescing limpio con `kfree`).
* **Archivos Creados y Modificados:**
  * `nucleo/base/memoria.h`: Definición de la interfaz en español, estadísticas de memoria y macros compatibles con Linux.
  * `nucleo/base/memoria.c`: Implementación completa del PMM, Heap con canarios y funciones de biblioteca freestanding (`memset`, `memcpy`, `memmove`, `memcmp`).
  * `nucleo/principal.c`: Inicialización de `memoria_iniciar()` tras la calibración del temporizador.
  * `nucleo/base/huevo.c`: `huevo_verificar()` ahora inspecciona periódicamente la integridad estructural de todo el Heap.
  * `nucleo/controladores/terminal.c`: Comando `memoria` (con soporte para `memoria probar`), atajos `free` y `ram`, y actualización del menú `ayuda`.
  * `Makefile`: Inclusión de `nucleo/base/memoria.c` en `C_SRCS`.
* **Pruebas y Verificación:**
  * En QEMU UEFI (512 MB de RAM configurados):
    - Detección de 455 MiB de RAM utilizable (114,364 páginas de 4 KiB disponibles en PMM).
    - Creación de arena de Heap de 8192 KiB (8 MiB).
    - Ejecución de `memoria probar`: Los 7 pasos completados con `[OK]` y canarios al `100% INTACTO`.
    - Apagado ordenado mediante `apagar` por ACPI con código 0.

---

### [2026-09-22 18:25 - 18:31] — Hito 10: Tablas de Paginación x86_64 de 4 Niveles (PML4 / VMM) y Control Soberano de CR3
* **Objetivo:** Cumplir el Paso 2 de la hoja de ruta solicitada por el usuario: tomar el control soberano del espacio de direcciones virtual en Anillo 0 mediante la construcción de un árbol PML4 propio de TAEK OS, permitiendo el mapeo dinámico de memoria virtual a física, la invalidación de TLB (`invlpg`), soporte para registros MMIO de hardware y el comando interactivo `paginacion`.
* **Diseño Arquitectónico:**
  1. **PML4 Soberano y Transición de CR3:**
     - Lectura del registro `CR3` inicial configurado por el bootloader Limine.
     - Asignación de una nueva página física de 4 KiB para el PML4 raíz del Kernel de TAEK OS con el PMM (`g_pml4_kernel_fisico`).
     - Clonación limpia de las entradas superiores (256 a 511), preservando el Higher Half Direct Map (`0xFFFF800000000000`), el código del Kernel (`0xFFFFFFFF80000000`) y los búferes de hardware.
     - Carga del nuevo PML4 en el registro de hardware `CR3` de la CPU (`mov %rax, %cr3`).
     - Transición sin fallos ni interrupción del pipeline de ejecución, verificada con éxito por El Huevo.
  2. **Árbol Jerárquico de 4 Niveles (PML4 -> PDPT -> PD -> PT):**
     - Primitiva `paginacion_mapear(virt, phys, flags)` con asignación bajo demanda de tablas intermedias (PDPT, PD, PT) mediante el PMM de TAEK OS.
     - Banderas en español: `PAGINA_PRESENTE`, `PAGINA_ESCRITURA`, `PAGINA_USUARIO`, `PAGINA_SIN_CACHE` (PCD/MMIO), `PAGINA_GLOBAL`, `PAGINA_NO_EJECUTABLE` (NX).
     - Primitiva `paginacion_desmapear(virt)` con limpieza de entradas y purga de TLB.
     - Primitiva `paginacion_obtener_fisica(virt)` con soporte para páginas de 4 KiB, páginas grandes de 2 MiB y páginas gigantes de 1 GiB.
     - Invalidación de hardware en el Translation Lookaside Buffer mediante `invlpg` en ensamblador inline.
  3. **Comando de Terminal `paginacion`:**
     - Reporta: Modelo de 4 niveles x86_64, valor actual de `CR3`, PML4 físico de TAEK OS, mitigación NX, PCD e invalidación TLB.
     - Subcomando `paginacion probar`: Autodiagnóstico en vivo que verifica el árbol de 4 niveles, mapea `0x00007FFF00000000` a un marco físico, escribe la firma mágica `TAEKOSVM` (`0x5441454B4F53564D`), comprueba la consistencia de datos a través del HHDM, valida la resolución inversa con `paginacion_obtener_fisica()`, desmapea la página e invalida el TLB con `invlpg`.
* **Archivos Creados y Modificados:**
  * `nucleo/base/paginacion.h`: Definición de la interfaz en español, banderas de bits y estructuras de tablas de 4 niveles.
  * `nucleo/base/paginacion.c`: Implementación completa de la creación del PML4, carga de CR3, mapeo dinámico, desmapeo, resolución inversa y autodiagnóstico.
  * `nucleo/principal.c`: Inicialización de `paginacion_iniciar()` tras `memoria_iniciar()`.
  * `nucleo/controladores/terminal.c`: Comando `paginacion` (con soporte para `paginacion probar`), alias `paginas`, `vmm`, `paging`, y actualización del menú `ayuda`.
  * `Makefile`: Inclusión de `nucleo/base/paginacion.c` en `C_SRCS`.
* **Pruebas y Verificación:**
  * En QEMU UEFI:
    - Arranque con recarga de CR3 exitosa: `CR3 Limine: 0x000000001DD36000 -> CR3 TAEK OS Soberano: 0x000000001FEF3000`.
    - Etapa validada con `[ OK ]` por El Huevo.
    - Ejecución de `paginacion probar`: Pasos 1, 2 y 3 completados con `[OK - 100% CORRECTO]` y `[PRESERVADO OK]`.
    - Comando `paginacion` reportó el árbol jerárquico y estado de protecciones.
    - Apagado limpio por ACPI.

---

### [2026-09-22 18:35 - 18:45] — Hito 11: Ruleta Rusa Cinematográfica (Modo Tensión Extrema de 20s y Munición Progresiva Acumulativa)
* **Objetivo:** Rediseñar por completo la mecánica del duelo de Ruleta Rusa en la terminal de TAEK OS para convertirlo en una experiencia cinematográfica de alta tensión psicológica (inspirada en *Buckshot Roulette* y el cine de Sergio Leone):
  1. Cada turno dura aproximadamente **20 segundos** con pausas milimétricas y narración introspectiva antes de la caída del percutor.
  2. Mecánica de munición progresiva acumulativa: En cada ronda ganada/sobrevivida, la letalidad aumenta (+1 proyectil en el tambor de 6 recámaras, desde la Ronda 1 con 1/6 hasta la Ronda 6 con 6/6 donde la muerte es certera).
  3. Bucle interactivo por turnos: Jugador $\rightarrow$ Máquina $\rightarrow$ Siguiente ronda con mayor calibre y tensión.
  4. Banda sonora continua: Reproducción en bucle del tema *El Bueno, El Feo y El Malo* vía DMA en el chip de audio Intel AC97 durante todo el conteo y la agonía de ambos bandos.
  5. Consecuencias implacables:
     - Si el jugador pierde: La bala atraviesa el sistema, **El Huevo de la Estabilidad** se quiebra fatalmente (`huevo_quebrar`), se dispara la animación de Don Cangrejo explotando a 30 FPS con su chillido y el sistema se apaga de golpe.
     - Si la máquina pierde: El silicio del kernel colapsa, la recámara vacía salva al jugador y se desbloquea de por vida el comando legendario: `"El comando"`.
* **Desglose de los 20 Segundos de Tensión por Turno:**
  - **Segundo 0 - 1:** Sacas el revólver de la cartuchera (espera 1s).
  - **Segundo 1 - 2.5:** Haces girar el tambor de acero: `*whiiir... clac-clac-clac...*` (espera 1.5s).
  - **Segundo 2.5 - 4.5:** Levantas el cañón frío y lo apoyas temblando contra tu sien (espera 2s).
  - **Segundo 4.5 - 7.5:** El sudor frío recorre tu nuca. El pulso se acelera en el silencio absoluto (espera 3s).
  - **Segundo 7.5 - 11:** *Introspección:* Recuerdas todo lo que te estás jugando: tu sistema operativo, tus datos, tu familia, tus hijos... (espera 3.5s).
  - **Segundo 11 - 14:** *Duda existencial:* *"¿De verdad vale la pena esto?"*, te preguntas en lo más profundo de tu alma... (espera 3s).
  - **Segundo 14 - 17:** Tu dedo índice acaricia el gatillo metálico... 3... 2... 1... (espera 3s).
  - **Segundo 17 - 19:** El gatillo cede el último milímetro de recorrido... ¡¡¡DISPARO INMINENTE!!! (espera 2s).
  - **Segundo 19 - 20:** *¡CLIC!* salvador o *¡PUM!* mortal.
* **Archivos Modificados:**
  * `nucleo/controladores/terminal.c`: Reescritura del comando `ruleta_rusa` con bucle de rondas 1 a 6, probabilidades dinámicas `tiro < (uint32_t)ronda`, pausas de 20s orquestadas con `esperar_con_audio_bucle()`, y mensajes inmersivos.
* **Pruebas y Verificación:**
  * Compilación limpia con Clang/LLD en WSL (`taek-os.img` generada y actualizada).
  * Soporte de audio estéreo AC97 para los turnos de 20 segundos sin interrupciones ni bloqueos de búfer.

---

### [2026-09-22 18:46 - 18:52] — Hito 12: Motor de Streaming de Audio AC97 de Larga Duración y Canción Completa de Duelo (2m 42s)
* **Objetivo:** Resolver el problema reportado por el usuario donde el tema *El Bueno, El Feo y El Malo* se escuchaba solo como un fragmento breve de ~11 segundos en bucle, logrando que suene la pista completa de **2 minutos y 42 segundos** y que únicamente al finalizar toda la canción comience de nuevo el bucle si el duelo continúa.
* **Causa Raíz Diagnosticada:**
  1. En el `Makefile`, la regla `ffmpeg` tenía un parámetro forzado `-t 11`, truncando el archivo MP3 a solo 11 segundos de audio PCM.
  2. En el controlador de hardware AC97 (`audio_ac97.c`), la tabla Buffer Descriptor List (BDL) de la arquitectura Intel ICH está estrictamente limitada por hardware a **32 entradas** de 64 KB cada una ($32 \times 65,536\text{ bytes} = 2,097,152\text{ bytes} \approx 11.88\text{ segundos}$). El driver original configuraba el BDL una sola vez; al agotarse los 11.88s, el DMA se detenía y la función `esperar_con_audio_bucle` volvía a disparar la pista desde el byte 0.
* **Solución Arquitectónica:**
  1. **Conversión Íntegra:** Se eliminó `-t 11` del `Makefile`, generando un archivo PCM lineal estéreo de 16 bits a 44.1 kHz de 27,972 KiB (~28 MB) que abarca los 162.38 segundos exactos de la canción.
  2. **Motor de Streaming Circular en AC97:**
     - Se rediseñó `nucleo/controladores/audio_ac97.c` implementando un buffer en anillo con puntero de reproducción continuo (`g_audio_cursor`), cola de descriptores activos (`g_entradas_en_cola`) y función de refresco no bloqueante `audio_ac97_actualizar()`.
     - Mientras el hardware reproduce una entrada en el índice `CIV` (Current Index Value), el software recicla los descriptores liberados y los rellena con los siguientes 64 KB de la pista, actualizando en tiempo real el registro `LVI` (Last Valid Index).
     - El cursor solo regresa al inicio (`g_audio_cursor = 0`) cuando se han transmitido los 28.6 MB completos (2m 42s).
  3. **Sincronización Transparente:** La rutina `esperar_con_audio_bucle()` en la terminal invoca periódicamente a `audio_ac97_actualizar()` cada 50 ms, asegurando que la cola de hardware mantenga siempre hasta ~11.8 segundos de anticipación y nunca sufra microcortes o underruns.
* **Archivos Modificados:**
  * `Makefile`: Conversión completa sin `-t 11`.
  * `nucleo/controladores/audio_ac97.h / .c`: Nuevas funciones `audio_ac97_reproducir_pcm_bucle()` y `audio_ac97_actualizar()`.
  * `nucleo/controladores/terminal.c`: Invocación del streaming en el comando `ruleta_rusa`.
* **Pruebas y Verificación:**
  * El kernel enlazado (`build/nucleo.elf`) creció a 51 MB (alojando video, pantallas y los 28 MB de audio crudo en el archivo ELF64).
  * La imagen UEFI FAT32 de 128 MB (`build/taek-os.img`) lo alojó con 80 MB de espacio libre restante.
  * Verificación en QEMU: Arranque limpio de 512 MB, ejecución de la ruleta rusa con audio streaming continuo y activación fiel de las consecuencias (disparo mortal, quiebre de El Huevo y animación de Don Cangrejo).

---

### [2026-09-22 19:05 - 19:12] — Hito 13: Subsistema de Enumeración PCI/PCIe, Cálculo Dinámico de BARs, Detección de GPU y Comandos `lspci` / `pci gpu`
* **Objetivo:** Cumplir el Paso 1 de la ruta hacia el driver de NVIDIA y aceleración gráfica: construir un escáner de hardware PCI / PCI Express de bajo nivel que explore todos los buses (0..255), ranuras y funciones, calcule por sondeo los tamaños y tipos de los Base Address Registers (BARs), identifique la GPU primaria (tanto en QEMU como en metal real) y proporcione herramientas de diagnóstico interactivas en la terminal.
* **Diseño Arquitectónico:**
  1. **Exploración de la Topología PCIe:**
     - Sondeo de los 256 buses, 32 ranuras y hasta 8 funciones (respetando el bit de multi-función en el registro `Header Type` `0x0E`).
     - Almacenamiento en tabla de dispositivos de tamaño dinámico con metadatos completos: Vendor ID, Device ID, Clase, Subclase, Prog IF, Revisión, Header Type, Subsystem IDs, Línea y Pin de Interrupción.
  2. **Algoritmo de Sondeo Físico de BARs (0 a 5):**
     - Lectura del valor original configurado por el firmware UEFI.
     - Escritura temporal de `0xFFFFFFFF` a los puertos `0xCF8/0xCFC` para revelar los bits alambrados en el silicio.
     - Restauración inmediata del valor original para no romper el direccionamiento del hardware.
     - Cálculo matemático del tamaño: `~(mask & ~0xF) + 1` (para memoria) o `~(mask & ~0x3) + 1` (para I/O).
     - Detección de BARs de 64 bits (tipo `0x02` en bits 2:1): Consumo conjunto del par (BAR $N$ y BAR $N+1$) y ensamblado de direcciones físicas de 64 bits (`dir_base` y `tamano` de 64 bits).
     - Clasificación semántica de memoria: MMIO sin caché vs. VRAM Aperture Prefetchable (ideal para Write-Combining con nuestro VMM).
  3. **Diccionario de Fabricantes y Clases:**
     - Identificación inmediata de NVIDIA (`0x10DE`), Intel (`0x8086`), AMD/ATI (`0x1002`), Red Hat VirtIO (`0x1AF4`), Bochs/QEMU (`0x1234`), Realtek (`0x10EC`), etc.
     - Identificación de clases de pantalla (`0x0300` VGA, `0x0302` 3D/Acelerador dedicado, etc.).
  4. **Comandos de Terminal:**
     - `lspci` / `pci`: Vista tabular compacta con resaltado en verde para GPUs y en cyan para audio.
     - `lspci -v` / `pci detalle`: Desglose exhaustivo de cada dispositivo con sus BARs físicos, tamaño en KiB/MiB/GiB, modo 64b/32b y flags.
     - `pci gpu` / `gpu`: Diagnóstico especializado de la controladora gráfica, mostrando la apertura de VRAM y registros MMIO listos para el futuro mapeo con `paginacion_mapear()`.
* **Archivos Modificados:**
  * `nucleo/arquitectura/x86_64/pci.h / .c`: Estructuras `struct barra_pci`, `struct dispositivo_pci`, sondeo de BARs, diccionarios y funciones de consulta.
  * `nucleo/principal.c`: Nueva etapa de arranque supervisada por El Huevo: *"Enumeración del Bus PCI / PCIe y Dispositivos de Video"*.
  * `nucleo/controladores/terminal.c`: Comandos `lspci`, `pci`, `pci gpu`, `gpu` y actualización del menú `ayuda`.
* **Pruebas y Verificación:**
  * En QEMU UEFI:
    - Etapa validada con éxito por El Huevo: 7 dispositivos PCI detectados (`[Dispositivos PCI: 7] [GPU: Bochs / QEMU Standard VGA]`).
    - `lspci`: Listó el Puente Host Q35 (`8086:29c0`), VGA (`1234:1111`), Ethernet (`8086:10d3`), Audio AC97 (`8086:2415`), Puente ISA (`8086:2918`), SATA AHCI (`8086:2922`) y SMBus (`8086:2930`).
    - `pci gpu`: Detectó la GPU en BDF `00:01.0`, reportó BAR0 de VRAM de 16 MiB en `0x80000000` con `[VRAM APERTURE - WRITE-COMBINING]` y BAR2 MMIO de 4 KiB en `0x81085000`.
    - `lspci -v`: Desglosó todos los BARs con precisión matemática.
### [2026-09-22 19:15 - 19:25] — Hito 14: Subsistema de GPU, Mapeo MMIO sin Caché (PCD/PWT), Lectura de Silicio y Comandos `gpu` / `gpu probar`
* **Objetivo:** Cumplir el Paso 2 de la ruta hacia el soporte de GPUs dedicadas (NVIDIA RTX 5070 Ti Laptop / Blackwell y emuladas): implementar el subsistema de GPU de TAEK OS, mapear las regiones de registros MMIO en memoria virtual sin caché a través del VMM de 4 niveles (`PAGINA_ATRIBUTOS_MMIO`), activar el Bus Mastering en el bus PCIe, leer los registros de silicio del hardware y proveer herramientas interactivas de diagnóstico con medición de latencia.
* **Diseño Arquitectónico:**
  1. **Asignación Canónica de Memoria Virtual para GPU:**
     - Dirección base virtual definida en `GPU_MMIO_VIRTUAL_BASE` (`0xFFFFFE0000000000ULL`), situada en la mitad superior del espacio de 64 bits pero aislada del kernel base y del heap.
  2. **Mapeo sin Caché (PCD y PWT) con el VMM:**
     - Uso de `paginacion_mapear()` con los flags `PAGINA_PRESENTE | PAGINA_ESCRITURA | PAGINA_SIN_CACHE | PAGINA_ESCRITURA_DIR` (Page Cache Disable y Page Write-Through).
     - Esta configuración es obligatoria en x86_64 para evitar que las lecturas y escrituras de registros de la GPU queden atrapadas en la memoria caché L1/L2/L3 de la CPU y garantiza que los ciclos de bus viajen eléctricamente por PCIe.
  3. **Activación de Bus Master y Lectura del Silicio:**
     - Invocación de `pci_activar_bus_master()` para permitir que la GPU opere como maestra del bus y acceda a DMA si lo requiere.
     - Primitivas inline y funciones seguras de acceso: `gpu_leer_mmio_32()` y `gpu_escribir_mmio_32()`.
     - Lectura del registro maestro de arranque (en NVIDIA, offset `+0x00000000` = `NV_PMC_BOOT_0`). Decodificación de arquitecturas NVIDIA: Blackwell (RTX 5000 / GB20x), Ada Lovelace (RTX 4000 / AD10x), Ampere (RTX 3000 / GA10x), Turing, Volta, Pascal, Maxwell, Kepler.
  4. **Comandos de Terminal:**
     - `gpu`: Resumen integral con BDF PCIe, fabricante, arquitectura detectada, firma de silicio, regiones físicas y virtuales de MMIO, capacidad de VRAM y estado de la capa de driver.
     - `gpu probar`: Autodiagnóstico riguroso de 5 pasos (detección PCIe, BAR MMIO, mapeo VMM sin caché, lectura de silicio y medición de latencia por TSC mediante `rdtsc`).
* **Archivos Creados / Modificados:**
  * `nucleo/controladores/gpu.h / .c`: Estructura `struct estado_gpu`, decodificación de arquitecturas, mapeo VMM, lectura/escritura MMIO y autodiagnóstico.
  * `nucleo/principal.c`: Etapa de arranque supervisada por El Huevo: *"Mapeo MMIO sin Caché y Comunicación con Silicio GPU"*.
  * `nucleo/controladores/terminal.c`: Inclusión de `gpu.h`, implementación de `ejecutar_comando_gpu()`, comandos `gpu` y `gpu probar`, y actualización del menú `ayuda`.
  * `Makefile`: Inclusión de `nucleo/controladores/gpu.c` en `C_SRCS`.
* **Pruebas y Verificación:**
  * Compilación y enlace limpios con Clang 22 / LLD.
  * En QEMU UEFI:
    - El Huevo validó la etapa de GPU sin agrietarse: `[Silicio: Bochs / QEMU Extended VGA | MMIO Virt: 0x0xFFFFFE0000000000] [ OK ]`.
    - Comando `gpu`: Mostró el dispositivo `00:01.0 [1234:1111]`, MMIO Físico `0x81085000` (4 KiB), MMIO Virtual `0xfffffe0000000000 [ACTIVO - SIN CACHÉ / PCD]` y VRAM `0x80000000` (16 MiB).
### [2026-09-22 19:25 - 19:30] — Hito 15: Capa de Compatibilidad Linux Kernel Shim (Ring 0), Adaptador PCI, Memoria DMA Coherente y Comandos `linux` / `linux probar`
* **Objetivo:** Construir el puente de compatibilidad ABI con el núcleo de Linux (`nucleo/compatibilidad/linux.h` y `linux.c`) para permitir la integración y ejecución directa de módulos de controladores de video (NVIDIA Open GPU Kernel Modules y VirtIO-GPU), contemplando la arquitectura de placa MoDT de escritorio (Intel Core i9-14900HX con GPU dedicada en bus PCIe directo a la CPU sin intermediarios Optimus).
* **Diseño Arquitectónico:**
  1. **Tipos de Datos y Códigos de Error POSIX/Linux:**
     - Definición de enteros canónicos (`u8`, `u16`, `u32`, `u64`), tipos de direcciones físicas y DMA (`dma_addr_t`, `phys_addr_t`, `resource_size_t`) y códigos de error estándar (`EINVAL`, `ENOMEM`, `EIO`, `EBUSY`, `ENODEV`).
  2. **Primitivas de Concurrencia y Sincronización:**
     - Spinlocks de Anillo 0 (`spinlock_t`, `spin_lock`, `spin_unlock`, `spin_lock_irqsave`) con ensamblador atómico y mitigación de contienda mediante instrucción `pause`.
     - Variables atómicas (`atomic_t`, `atomic_read`, `atomic_set`, `atomic_inc`, `atomic_dec`) con ordenamiento de memoria secuencial.
     - Barreras de hardware x86_64: `mb()` (`mfence`), `rmb()` (`lfence`), `wmb()` (`sfence`).
  3. **Memoria DMA Coherente para Silicio (`dma_alloc_coherent`):**
     - Asignación de páginas físicas contiguas mediante el PMM de TAEK OS para alojar las colas de comandos (*ring buffers*) y el firmware GSP de NVIDIA.
     - Devolución simultánea del puntero virtual HHDM y la dirección física (`dma_handle`).
  4. **Mapeo MMIO sin Caché (`ioremap` / `iounmap`):**
     - Rango virtual canónico exclusivo asignado: `LINUX_SHIM_IOREMAP_BASE` (`0xFFFFFD0000000000ULL`), aislado del espacio del kernel y de la terminal.
     - Páginas de protección (guard pages) de 4 KiB entre mapeos para aislar fallos de desbordamiento.
  5. **Adaptador de Dispositivos PCI (`struct pci_dev`):**
     - Adaptación en tiempo de arranque de los dispositivos físicos descubiertos en el escáner del Hito 13 hacia las estructuras `struct pci_dev` de Linux.
     - Funciones de sondeo: `pci_get_device()`, `pci_get_class()`, `pci_enable_device()`, `pci_set_master()`.
     - Primitivas de lectura y escritura en el espacio de configuración PCI: `pci_read_config_*` y `pci_write_config_*` (8, 16 y 32 bits).
  6. **Subsistema de Telemetría y Comandos:**
     - Implementación de `printk()` con soporte de niveles de registro (`KERN_INFO`, etc.) y macros `pr_info()`, `pr_warn()`, `pr_err()`.
     - Comando `linux` / `shim`: Reporta estadísticas de memoria DMA, rango virtual y dispositivos registrados.
     - Comando `linux probar`: Autodiagnóstico de 5 pasos que certifica el correcto funcionamiento del puente.
* **Archivos Creados / Modificados:**
  * `nucleo/compatibilidad/linux.h / .c`: Implementación completa de la capa shim.
  * `nucleo/arquitectura/x86_64/pci.h / .c`: Incorporación de la primitiva de escritura en 8 bits `pci_escribir_config_8()`.
  * `nucleo/principal.c`: Etapa de arranque supervisada por El Huevo: *"Capa de Compatibilidad Linux Kernel Shim (Ring 0)"*.
  * `nucleo/controladores/terminal.c`: Comandos `linux` y `linux probar`, e inclusión en el menú `ayuda`.
  * `Makefile`: Inclusión de `nucleo/compatibilidad/linux.c` en `C_SRCS`.
* **Pruebas y Verificación:**
  * Compilación y enlace limpios con Clang 22 / LLD.
  * En QEMU UEFI:
    - Etapa supervisada por El Huevo: `[Linux ABI 6.12 | Dispositivos PCI: 7] [ OK ]`.
    - Comando `linux`: Reportó ABI 6.12 LTS, rango virtual `0xfffffd0000000000`, 0 KiB DMA en uso y 7 dispositivos adaptados.
    - Comando `linux probar`: Superó los 5 pasos con éxito total (`==> [ AUTODIAGNÓSTICO EXITOSO ] Capa Linux Shim 100% lista para controladores externos.`).
### [2026-09-22 19:35 - 19:42] — Hito 15.1: Aislamiento Arquitectónico de NVIDIA (Resource Manager Core en `controladores/video/nvidia/`, `nv_os_interface` y Comandos `nvidia` / `nvidia probar`)
* **Objetivo:** Cumplir con la directriz de diseño del usuario de aislar formalmente todo el código específico de NVIDIA dentro de `nucleo/controladores/video/nvidia/`, desacoplándolo del núcleo de TAEK OS mediante la interfaz abstracta `nv_os_interface`. Este aislamiento garantiza que si ocurre un fallo o regresión en el pipeline gráfico (cómputo o renderizado), sea trivial determinar si el error provino del silicio/driver de NVIDIA o del kernel base de la CPU.
* **Diseño Arquitectónico de 3 Capas:**
  1. **Capa 1: NVIDIA Core Aislado (`nucleo/controladores/video/nvidia/`):**
     - `inc/nvtypes.h`: Tipos de datos oficiales de NVIDIA (`NvU8`..`NvU64`, `NvBool`, `NvHandle`).
     - `inc/nvstatus.h`: Códigos de retorno oficiales (`NV_OK`, `NV_ERR_NO_MEMORY`, etc.).
     - `inc/nv_gsp.h`: Protocolo de mensajería RPC y estructuras para el firmware GSP (GPU System Processor) para Blackwell (RTX 5070 Ti) y Ada/Ampere.
     - `core/nvidia_core.h / .c`: Resource Manager Core, sondeo de silicio e inicialización de canales.
  2. **Capa 2: Capa Puente y Enlace (`nucleo/compatibilidad/`):**
     - `nv_os_interface.h / .c`: Implementa la interfaz formal que el Core de NVIDIA requiere (`nv_os_alloc_pages`, `nv_os_free_pages`, `nv_os_map_mmio`, `nv_os_unmap_mmio`, `nv_os_read_pci_32`, `nv_os_spinlock_*`, `nv_os_delay_*`), enrutándolas hacia el Linux Shim y el VMM de TAEK OS.
  3. **Capa 3: Núcleo Soberano de TAEK OS:**
     - Provee memoria física (PMM), paginación sin caché (VMM), sondeo PCIe y vigilancia de **El Huevo de la Estabilidad**.
  4. **Comandos de Terminal:**
     - `nvidia`: Reporta estado del pipeline, silicio Blackwell/Ada, identificadores PCI y canal DMA RPC de GSP.
     - `nvidia probar`: Autodiagnóstico riguroso de 4 pasos (spinlocks de silicio, memoria DMA coherente, formato RPC de GSP y verificación de aislamiento sin fugas de fallos al kernel).
* **Archivos Creados / Modificados:**
  * `nucleo/controladores/video/nvidia/inc/nvtypes.h` [NUEVO]
  * `nucleo/controladores/video/nvidia/inc/nvstatus.h` [NUEVO]
  * `nucleo/controladores/video/nvidia/inc/nv_gsp.h` [NUEVO]
  * `nucleo/controladores/video/nvidia/core/nvidia_core.h / .c` [NUEVOS]
  * `nucleo/compatibilidad/nv_os_interface.h / .c` [NUEVOS]
  * `nucleo/principal.c`: Etapa de arranque supervisada: *"Subsistema Aislado NVIDIA Resource Manager (Core / GSP)"*.
  * `nucleo/controladores/terminal.c`: Inclusión de `nvidia_core.h`, comandos `nvidia` y `nvidia probar`, y menú `ayuda`.
  * `Makefile`: Inclusión de `nv_os_interface.c` y `nvidia_core.c` en `C_SRCS`.
* **Pruebas y Verificación:**
  * Compilación y enlace limpios con Clang 22 / LLD.
  * En QEMU UEFI:
    - Etapa supervisada por El Huevo: `[Pipeline: NVIDIA Blackwell GB20x (Verificación Pipeline) (Canal GSP Listo)] [ OK ]`.
    - Comando `nvidia`: Mostró aislamiento al 100%, canal DMA RPC de 8 KiB en dirección física `0x1feef000`.
    - Comando `nvidia probar`: 4 pasos aprobados al 100% (`==> [ AUTODIAGNÓSTICO EXITOSO ] Pipeline NVIDIA aislado y listo para H16 (APIC+IRQ).`).

---

## 🔍 Registro de Errores y Lecciones Aprendidas (Post-Mortem)

| Error / Problema | Causa Raíz | Solución Aplicada |
| :--- | :--- | :--- |
| **Audio de duelo sonaba en bucle corto de 11s en vez de la canción completa** | 1) `Makefile` tenía `-t 11` forzando el corte en ffmpeg. 2) La lista de descriptores BDL de AC97 sólo tiene 32 entradas fijas (~11.8s de audio), y el driver original no tenía refresco circular dinámico, reiniciando desde el byte 0. | Se quitó el flag `-t 11` convirtiendo los 2m 42s completos (28 MB), y se rediseñó el controlador AC97 con un motor de streaming circular continuo (`audio_ac97_actualizar`) que rellena dinámicamente los descriptores reproducidos y solo reinicia el cursor tras agotar los 2m 42s. |
| **`instruction expected, found ' ['` en NASM** | `Set-Content -Encoding utf8` en PowerShell escribe una marca de orden de bytes (BOM `\xef\xbb\xbf`) al inicio del archivo. | Se creó una rutina con `sed -i '1s/^\xef\xbb\xbf//'` para eliminar el BOM de todos los archivos fuente. |
| **`qemu: could not load PC BIOS`** | En QEMU moderno para x86_64, el firmware UEFI OVMF es una imagen pflash, no una BIOS legacy. | Se cambió el parámetro a `-drive if=pflash,format=raw,readonly=on,file=edk2-x86_64-code.fd`. |
| **`rm: cannot remove taek-os.img: Permission denied`** | QEMU seguía en ejecución en segundo plano o el usuario tenía la ventana abierta, bloqueando el archivo en Windows. | Se modificó la regla del Makefile: ahora solo se crea la imagen si no existe, y las actualizaciones de `nucleo.elf` se hacen in-situ con `mcopy -o`, eliminando el bloqueo. |
| **Mojibake `├▒` en consola** | La terminal de Windows usaba la página de códigos CP437 (DOS) en vez de UTF-8. | Se configuró `chcp 65001` y se añadió el módulo `utf8.c` en el núcleo. |
| **`No rule to make target Recursos` en GNU Make** | El nombre de la carpeta contenía un espacio (`Recursos Asets`), rompiendo la sintaxis de prerequisitos en Make. | Se crearon enlaces simbólicos sin espacios (`recursos/fivenights.png` y `recursos/damonte.mp3`). |
| **`limine.h API revision unsupported`** | `#define LIMINE_API_REVISION` se fijó en 3, pero la cabecera soporta hasta la revisión 2. | Se ajustó `#define LIMINE_API_REVISION 2` antes de incluir `limine.h`. |
| **`No se puede llamar a un método en una expresión con valor NULL ($wslDir)`** | WSL escapa las contrabarras de Windows (`\U`, `\P`), haciendo fallar a `wslpath`, o `$PSScriptRoot` es nulo al invocar comandos interactivamente. | Se implementó resolución con respaldo a `(Get-Location).Path`, reemplazo de barras a POSIX (`/`) y conversión directa a `/mnt/<unidad>/`. |
| **`Instruccion / Opcode Invalido (#UD)` al tirar del gatillo** | La CPU virtual por defecto de QEMU no tiene la instrucción de silicio `rdrand` activada, provocando que la CPU lance la excepción `#UD`. | Se reemplazó por un generador pseudoaleatorio Xorshift32 alimentado directamente por el Time Stamp Counter (`rdtsc`), 100% universal y sin riesgo de `#UD`. |
| **Bloqueo / Congelamiento en 4174 bytes al redirigir salida de QEMU en PowerShell** | Deadlock clásico del búfer de pipe anónimo en Windows (4096 bytes). Si el hijo escribe más de 4 KB y el padre duerme sin drenar el pipe, `WriteFile` bloquea el UART en el kernel. | Se implementó lectura con streaming continuo asíncrono en Python (`subprocess.Popen` con hilo lector en tiempo real) y drenaje constante. |
