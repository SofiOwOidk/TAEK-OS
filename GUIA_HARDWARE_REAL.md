# Guía de Pruebas en Hardware Real (Placa MoDT + NVIDIA GeForce RTX 5070 Ti) 🖥️⚡

Esta guía documenta los pasos exactos, configuraciones de BIOS UEFI, conexiones de telemetría serial y comandos de verificación para arrancar **TAEK OS** en la placa Desktop MoDT (*Mobile-on-Desktop*) equipada con un procesador **Intel Core i9-14900HX** y una tarjeta gráfica física dedicada **NVIDIA GeForce RTX 5070 Ti** conectada al slot PCIe x16.

---

## 1. Preparación del Medio de Arranque (Pendrive USB)

La imagen booteable híbrida (UEFI 64-bit y BIOS) se encuentra en:
📁 **`build/taek-os.iso`** (~54 MB)

### Método A: Ventoy (Recomendado)
1. Conecta tu pendrive con Ventoy.
2. Copia directamente `build/taek-os.iso` a la raíz de la unidad USB.

### Método B: Rufus
1. Abre **Rufus** en Windows.
2. Selecciona tu unidad USB.
3. En *Elección de arranque*, haz clic en **SELECCIONAR** y elige `build/taek-os.iso`.
4. Configuración obligatoria:
   - **Esquema de partición:** `GPT`
   - **Sistema de destino:** `UEFI (no CSM)`
   - **Sistema de archivos:** `FAT32`
5. Haz clic en **EMPEZAR** (escribe en modo ISO o DD).

---

## 2. Ajustes Obligatorios en la BIOS UEFI de la Placa MoDT

Reinicia la PC y presiona `Supr` o `F2` para ingresar al menú de configuración UEFI (AMI Aptio V). Ajusta las siguientes opciones:

| Parámetro | Configuración | Justificación Técnica |
| :--- | :--- | :--- |
| **Primary Display / Initial Display** | **IGFX / iGPU / CPU Graphics** | Fuerza que el video UEFI GOP salga por el HDMI/DP de la motherboard (Intel UHD 770). |
| **Internal Graphics / Multi-Monitor** | **Enabled** | Evita que la placa desactive los gráficos del i9-14900HX al detectar la RTX 5070 Ti en el PCIe. |
| **Secure Boot** | **Disabled** (o *Other OS*) | TAEK OS corre su propio kernel ELF64 sin clave criptográfica de Microsoft KEK. |
| **Boot Mode** | **Pure UEFI** (*CSM Disabled*) | Requerido por el bootloader Limine 8.7.0 y el Framebuffer UEFI GOP a 32bpp. |
| **Above 4G Decoding** | **Enabled** | Permite asignar los 16 GiB de VRAM de la RTX 5070 Ti por encima de los 4 GB de RAM física. |
| **Resizable BAR (ReBAR)** | **Enabled / Auto** | Permite a la CPU acceder a toda la VRAM en un solo rango de BAR1 sin particiones de 256 MB. |
| **Intel Virtualization (VT-x)** | **Enabled** | Requerido para el Hipervisor VMX en Ring -1 de TAEK OS. |
| **Intel VT-d (IOMMU)** | **Enabled** | Permite que el analizador ACPI DMAR descubra las unidades DRHD y opere en Pass-Through (PT). |
| **Serial Port (COM1)** | **Enabled** (*0x3F8 / IRQ 4*) | Activa el chip Super I/O para transmitir toda la telemetría en caso de usar cable serie. |

> [!TIP]
> **Conexión de Cable de Video (HDMI / DisplayPort):**  
> **Conéctalo SIEMPRE a la salida de video de la motherboard (Panel trasero de la placa)**, NO a la tarjeta RTX 5070 Ti.  
> *¿Por qué?* El procesador Intel Core i9-14900HX cuenta con gráficos integrados Intel UHD 770. Si conectaras el monitor a la RTX 5070 Ti, cualquier fallo en los registros de la GPU durante el desarrollo de nuestro driver apagaría la pantalla por completo (*blackout*). Conectando a la motherboard, la terminal y El Huevo mantienen la señal de video 100% visible sin importar lo que ocurra con la tarjeta NVIDIA.

---

## 3. Captura Externa de Telemetría Serial COM1 (Sin Pantalla)

Si el kernel sufriera un fallo crítico (*Kernel Panic*) antes de que la pantalla se active, la telemetría se transmite en tiempo real por el puerto serie COM1 (I/O `0x3F8` a **115,200 baudios, 8N1**).

### Conexión por Hardware (Cable / Adaptador):
1. **Localiza el cabezal de pines `JCOM1` o `COM1`** en la placa madre MoDT (suele estar cerca del panel frontal o los slots PCIe).
2. Conecta un adaptador USB-Serial (FTDI, CH340, CP2102) o cable Null-Modem a una segunda computadora o laptop:
   - **Pin TX de la placa** $\rightarrow$ **RX del adaptador**
   - **Pin RX de la placa** $\rightarrow$ **TX del adaptador**
   - **Pin GND** $\rightarrow$ **GND del adaptador**
3. Abre un emulador de terminal en la segunda máquina (PuTTY, TeraTerm o `minicom`):
   - **Velocidad:** `115200` baudios
   - **Bits de datos:** `8`
   - **Paridad:** `Ninguna (None)`
   - **Bits de parada:** `1`
   - **Control de flujo:** `Ninguno`

> [!NOTE]
> Toda la telemetría se graba simultáneamente en el **búfer circular de RAM (`dmesg`) de 64 KiB**. Si no cuentas con cable serie, puedes ver todo el log en pantalla ejecutando el comando `dmesg`.

---

## 4. Secuencia de Verificación Paso a Paso en TAEK OS

Una vez que el sistema arranca y muestra el prompt `sudo@taek-os:~#`, sigue esta secuencia ordenada:

### Paso 1: Ver el Menú y la Guía Integrada
```text
sudo@taek-os:~# ayuda
sudo@taek-os:~# guia gpu
```

### Paso 2: Auditar el Registro de Arranque
```text
sudo@taek-os:~# dmesg
```
Verifica que aparezca:
* `[BOOT] Puerto Serial COM1 (0x3F8): Activo a 115200 8N1 [OK]`
* `[BOOT] Protocolo Limine Base Revision verificado [OK]`
* Estado de El Huevo: `INTACTO (100% Integridad)`

### Paso 3: Escaneo del Bus PCIe (Detección de la RTX 5070 Ti)
```text
sudo@taek-os:~# lspci
sudo@taek-os:~# pci gpu
```
* **Qué buscar:**
  - Vendor ID: `0x10DE` (NVIDIA)
  - Device ID: `0x2F04` (GeForce RTX 5070 Ti)
  - Clase: `0x0300` (VGA Controller)
  - Si en `Modo de Operación` dice `Hardware Real MoDT`, la placa madre y las líneas PCIe están operando de forma nativa.

### Paso 4: Inspección de Direcciones Físicas BAR0 y BAR1
```text
sudo@taek-os:~# gpu
sudo@taek-os:~# gpu probar
```
* **BAR0:** Espacio MMIO de registros de silicio asignado por UEFI (típicamente `0x80000000` a `0xF6000000`).
* **BAR1:** Apertura física de VRAM (16 GiB en direccionamiento de 64 bits).

### Paso 5: Gestor de Memoria DMA e IOMMU
```text
sudo@taek-os:~# dma
sudo@taek-os:~# dma probar
sudo@taek-os:~# iommu
sudo@taek-os:~# iommu probar
```
* Comprueba que la arena física DMA de 32 MiB esté en `0x0000000001200000` y que la tabla ACPI DMAR detecte las unidades DRHD de Intel VT-d.

### Paso 6: Secuencia de Arranque del Coprocesador GSP
```text
sudo@taek-os:~# nvidia gsp
sudo@taek-os:~# nvidia inicializar
sudo@taek-os:~# nvidia
sudo@taek-os:~# nvidia probar
```

---

## 5. Protocolo en Caso de Pantalla Negra o Cuelgue

Si el monitor no da video tras seleccionar TAEK OS en el menú de Limine:
1. Revisa el log en la segunda PC conectada al puerto COM1: te dirá en qué instrucción exacta se detuvo.
2. Si no hay salida en COM1, verifica si el monitor está conectado a la **salida de la GPU dedicada RTX 5070 Ti** o a la **salida de la placa madre (iGPU Intel UHD del 14900HX)**:
   - Limine usa UEFI GOP. La BIOS suele inicializar como primaria la iGPU o la dGPU según la opción *"Primary Display"* en la BIOS (*PEG/PCIe* vs *IGD*). Conecta el cable a la otra salida de video para verificar.
3. Si el equipo se reinicia abruptamente, revisa que el CPU no lance una excepción `#GP` o `#PF` no capturada (la IDT de TAEK OS vuelca los registros `CR2`, `RIP` y `RSP` antes de quebrar El Huevo).
