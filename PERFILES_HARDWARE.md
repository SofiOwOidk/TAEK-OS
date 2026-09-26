# Perfiles físicos para validar TAEK OS

El usuario aportó fotografías de la salida PCI de TAEK OS el 2026-09-26.
Estas fotografías demuestran enumeración PCI y salida de framebuffer; no son
prueba de reproducción HDA, escrituras de FS, aislamiento DMA o recuperación VMX.

| Equipo | Componentes visibles | Pruebas realizadas y estado |
|---|---|---|
| Laptop de pruebas (Dell) | CPU: Intel Core i7-8650U @ 1.90GHz (16 hilos APIC, TSC 2112 MHz)<br>GPU: Intel UHD 620 (`8086:5917`), xHCI `8086:9d2f`, Intel HDA `8086:9d71` (`00:1f.3`) | **H.264 360p Benchmark verificado (2026-09-26):**<br>- 4.350/4.350 cuadros decodificados y presentados (100%).<br>- Rendimiento: **47.83 FPS** (objetivo $\ge$ 30 FPS superado).<br>- Latencia: p50=21.83 ms, **p95=26.33 ms** (cumple p95 < 33.33 ms), p99=28.02 ms.<br>- Memoria: 3 MB pico, canarios de heap intactos.<br>- xHCI: Contingencia y volcado forense verificados ante timeout en comando Enable Slot en puerto 2 (Full-Speed). |
| Equipo principal | GPU NVIDIA, xHCI Intel, Intel HDA (`00:1f.3`) y HDA NVIDIA (`01:00.1`) | Distinguir cuál HDA se selecciona, probar reproducción/contador en ambos, USB MSC y hotplug. |

El selector HDA actual intenta primero los dispositivos Intel de clase de audio;
en el equipo principal debería seleccionar `00:1f.3`. Esto es una inferencia del
código y de la enumeración PCI fotografiada. La salida serial `[HDA Intento ...]`
de un arranque físico debe confirmar el BDF, el códec detectado, `LPIB` y `BCIS`.
El audio NVIDIA `01:00.1` corresponde a una ruta separada (HDMI/DisplayPort)
que aún no tiene una prueba física de reproducción.

Prueba física inicial, sin escribir al disco: arrancar la imagen actual, ejecutar
`stress` y conservar el registro serial COM1. Buscar el BDF seleccionado por
`[HDA Intento]`, `Avance HDA` mayor que cero, `BCIS`, 64 rondas y cero fallos
I/O. Repetir con teclado/ratón USB conectados y una desconexión/reconexión.
La validación de escritura exFAT requiere una memoria USB desechable y una
comprobación posterior con `fsck.exfat` en el sistema anfitrión.

Para una prueba física de escritura, utilizar sólo una memoria USB que se pueda
formatear y verificar después desde otro sistema. Guardar el serial completo y
el resultado de `fsck.exfat`/`e2fsck` antes de marcar la columna de hardware
como validada en `ESTADO_SUBSISTEMAS.md`.
