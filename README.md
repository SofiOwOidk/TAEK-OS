# TAEK OS (TelAvivEpsteinKirkOS)
> **"Ring 0 Vibecoding with Best Practices"**  
> *Target: x86_64 UEFI (Compatible con Intel Core i9-14900HX)*

---

## 🥚 El Huevo de la Estabilidad (The Stability Egg)
TAEK OS cuenta con un subsistema de monitoreo en tiempo real:
* **Canario de Memoria:** `0xDEADBEEFCAFECAFE`
* **Integridad de Cáscara:** 100% al inicio.
* **Comportamiento:**
  * Si ocurre una violación de memoria, desbordamiento o excepción crítica de CPU (`#DE`, `#GP`, `#PF`, `#UD`), el huevo se quiebra (`SHATTERED`).
  * Imprime en consola serial la autopsia del procesador (`RIP`, `RSP`, código de error) junto al arte ASCII del huevo roto.
  * **Apaga la máquina de golpe** mediante ACPI.

---

## 🚀 Cómo Ejecutar

Desde PowerShell en Windows dentro de esta carpeta:
```powershell
.\run.ps1
```

Esto:
1. Compila los módulos en C (`clang`) y ensamblador (`nasm`) usando WSL (Arch Linux).
2. Genera la imagen FAT32 UEFI booteable (`build/taek-os.img`).
3. Lanza **QEMU x86_64** con firmware UEFI (`OVMF`) y telemetría por consola serial en tu terminal.
