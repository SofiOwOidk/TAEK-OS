# Guía de Pruebas en Hardware Real (Laptops y PCs x86_64) 🖥️⚡
> **Documentación creada por Gemini (Google DeepMind)**

Esta guía documenta los pasos de configuración de BIOS UEFI, conexiones de telemetría serial y comandos de verificación para arrancar **TAEK OS** en hardware real: probado y validado en silicio Intel desde la **8ª Generación (Core i7-8650U)** hasta la **14ª Generación (Core i9-14900HX)**. Más allá de la 14ª Generación se desconoce si funcionará.

---

## 1. Preparación del Medio de Arranque (Pendrive USB)

La imagen booteable híbrida (UEFI 64-bit y BIOS) se genera en:
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

## 2. Ajustes Recomendados en la BIOS UEFI

Reinicia el equipo y presiona `Supr`, `F2` o `F12` para ingresar al menú de configuración UEFI. Ajusta las siguientes opciones según estén disponibles en tu placa:

| Parámetro | Configuración | Justificación Técnica |
| :--- | :--- | :--- |
| **Primary Display / Initial Display** | **IGFX / iGPU / CPU Graphics** (en PCs con gráficos integrados) | Asegura que el video UEFI GOP salga por la motherboard durante desarrollo. |
| **Secure Boot** | **Disabled** (o *Other OS*) | TAEK OS corre su propio kernel ELF64 nativo sin clave Microsoft KEK. |
| **Boot Mode** | **Pure UEFI** (*CSM Disabled*) | Requerido por el bootloader Limine y el Framebuffer UEFI GOP a 32bpp. |
| **Above 4G Decoding** | **Enabled** (en equipos de escritorio) | Permite direccionar rangos de memoria de dispositivos PCIe por encima de 4 GB. |
| **Resizable BAR (ReBAR)** | **Enabled / Auto** | Permite acceso amplio a la memoria de tarjetas PCIe. |
| **Intel Virtualization (VT-x / AMD-V)** | **Enabled** | Requerido para el Hipervisor VMX en Ring -1 de TAEK OS. |
| **Intel VT-d / AMD IOMMU** | **Enabled** | Permite que el analizador ACPI DMAR descubra las unidades DRHD y opere en Pass-Through (PT). |
| **Serial Port (COM1)** | **Enabled** (*0x3F8 / IRQ 4*, opcional) | Activa el chip Super I/O para transmitir telemetría en tiempo real por cable serie. |

---

## 3. Captura Externa de Telemetría Serial COM1 (Opcional)

Si el kernel sufriera un fallo crítico (*Kernel Panic*) antes de inicializar la pantalla gráfica, toda la telemetría se transmite en tiempo real por el puerto serie COM1 (I/O `0x3F8` a **115,200 baudios, 8N1**).

### Conexión por Hardware (Cable / Adaptador USB-Serial):
1. **Localiza el cabezal de pines `COM1`** en la placa madre o usa un adaptador serial en caso de tener puerto UART.
2. Conecta un adaptador USB-Serial (FTDI, CH340, CP2102) a una segunda computadora o laptop:
   - **Pin TX de la placa** $\rightarrow$ **RX del adaptador**
   - **Pin RX de la placa** $\rightarrow$ **TX del adaptador**
   - **Pin GND** $\rightarrow$ **GND del adaptador**
3. Abre PuTTY, TeraTerm o `tio` en la segunda máquina a **115200 baudios, 8 bits de datos, sin paridad, 1 bit de parada**.

---

## 4. Secuencia de Verificación al Iniciar TAEK OS

Una vez cargado el kernel y presentada la terminal interactiva `sudo@taek-os:~#`:

### Paso 1: Diagnóstico de Teclados USB y Concurrencia xHCI
```text
sudo@taek-os:~# xhci
sudo@taek-os:~# teclado
```
* Ambos deben mostrar la enumeración activa de los controladores de puerto USB y el estado de los teclados conectados. Puedes conectar teclados adicionales en caliente en cualquier puerto USB y escribir de inmediato con cualquiera de ellos.

### Paso 2: Escaneo de Dispositivos PCI/PCIe
```text
sudo@taek-os:~# lspci
```

### Paso 3: Subsistema de Audio (HDA / AC97)
```text
sudo@taek-os:~# audio
sudo@taek-os:~# audio probar
```

### Paso 4: Estado del Huevo de la Estabilidad
```text
sudo@taek-os:~# huevo
```

---

## 5. Protocolo en Caso de Pantalla Negra

Si el monitor no da video tras seleccionar TAEK OS en el menú de Limine:
1. Revisa el log en la segunda PC conectada al puerto COM1: indicará en qué instrucción exacta se detuvo.
2. En computadoras con múltiples salidas de video (tarjeta dedicada e integrada), prueba conectar el cable HDMI/DisplayPort a la otra salida disponible.
3. Si el equipo se reinicia abruptamente, la IDT de TAEK OS vuelca los registros `CR2`, `RIP` y `RSP` antes del apagado de seguridad.
