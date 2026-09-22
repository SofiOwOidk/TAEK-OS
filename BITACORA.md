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


## 🔍 Registro de Errores y Lecciones Aprendidas (Post-Mortem)

| Error / Problema | Causa Raíz | Solución Aplicada |
| :--- | :--- | :--- |
| **`instruction expected, found ' ['` en NASM** | `Set-Content -Encoding utf8` en PowerShell escribe una marca de orden de bytes (BOM `\xef\xbb\xbf`) al inicio del archivo. | Se creó una rutina con `sed -i '1s/^\xef\xbb\xbf//'` para eliminar el BOM de todos los archivos fuente. |
| **`qemu: could not load PC BIOS`** | En QEMU moderno para x86_64, el firmware UEFI OVMF es una imagen pflash, no una BIOS legacy. | Se cambió el parámetro a `-drive if=pflash,format=raw,readonly=on,file=edk2-x86_64-code.fd`. |
| **`rm: cannot remove taek-os.img: Permission denied`** | QEMU seguía en ejecución en segundo plano o el usuario tenía la ventana abierta, bloqueando el archivo en Windows. | Se modificó la regla del Makefile: ahora solo se crea la imagen si no existe, y las actualizaciones de `nucleo.elf` se hacen in-situ con `mcopy -o`, eliminando el bloqueo. |
| **Mojibake `├▒` en consola** | La terminal de Windows usaba la página de códigos CP437 (DOS) en vez de UTF-8. | Se configuró `chcp 65001` y se añadió el módulo `utf8.c` en el núcleo. |
| **`No rule to make target Recursos` en GNU Make** | El nombre de la carpeta contenía un espacio (`Recursos Asets`), rompiendo la sintaxis de prerequisitos en Make. | Se crearon enlaces simbólicos sin espacios (`recursos/fivenights.png` y `recursos/damonte.mp3`). |
| **`limine.h API revision unsupported`** | `#define LIMINE_API_REVISION` se fijó en 3, pero la cabecera soporta hasta la revisión 2. | Se ajustó `#define LIMINE_API_REVISION 2` antes de incluir `limine.h`. |
| **`No se puede llamar a un método en una expresión con valor NULL ($wslDir)`** | WSL escapa las contrabarras de Windows (`\U`, `\P`), haciendo fallar a `wslpath`, o `$PSScriptRoot` es nulo al invocar comandos interactivamente. | Se implementó resolución con respaldo a `(Get-Location).Path`, reemplazo de barras a POSIX (`/`) y conversión directa a `/mnt/<unidad>/`. |
| **`Instruccion / Opcode Invalido (#UD)` al tirar del gatillo** | La CPU virtual por defecto de QEMU no tiene la instrucción de silicio `rdrand` activada, provocando que la CPU lance la excepción `#UD`. | Se reemplazó por un generador pseudoaleatorio Xorshift32 alimentado directamente por el Time Stamp Counter (`rdtsc`), 100% universal y sin riesgo de `#UD`. |
