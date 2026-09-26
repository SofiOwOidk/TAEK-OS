# Estado de subsistemas

Esta tabla distingue código presente de funcionamiento demostrado. «Validado» requiere
un registro reproducible con configuración y resultado; compilar por sí solo no basta.

| Subsistema | Experimental | Validado QEMU | Validado hardware | Descontinuado | Límite actual |
|---|---|---|---|---|---|
| USB MSC 512 B y rechazo 4Kn | Sí | Arranque y montajes con 512 B; 4Kn rechazado con USB BOT de QEMU | Pendiente | No | No hay soporte 4Kn; la unidad se deshabilita antes de exponer I/O. |
| exFAT escritura | Sí | Creación de archivo por terminal, `fsck.exfat` limpio; prueba host de 10 KB con ASan/UBSan | Pendiente | No | Bitmap y raíz de un cluster; archivos contiguos. Falta recuperación ante pérdida de alimentación. |
| ext4 lectura | Sí | Archivo de 10 000 B en 3 bloques leído íntegro por USB MSC; imagen limpia monta y copia con `RO_COMPAT_BIGALLOC` se rechaza | Pendiente | No | Árbol de extents hasta profundidad 5; montaje exige estado limpio y rechaza checksums/features no implementadas. |
| ext4 escritura | No | No | No | No | Deshabilitada: faltan journal, checksums y actualización completa de metadatos. |
| PMM y paginación | Sí | Arranque QEMU con asignación para DMA/xHCI/FS | Pendiente | No | Faltan pruebas de presión concurrente y de doble liberación inducida. |
| xHCI Event Ring | Sí | Teclado, MSC y ratón HID simultáneos; ratón conectado, desconectado y reconectado en QEMU | Pendiente | No | Dispatcher central con buzones; falta prueba de ring saturado y repetición prolongada del hotplug. |
| PCI BAR | Sí | Arranque QEMU con BAR xHCI | Pendiente | No | Falta probar BAR de 64 bits en hardware. |
| HDA/AC97 A/V | Sí | Streaming A/V validado en QEMU (19.1 s continuo, 32 BCIS, 0 vaciados, 0 micro-silencios); `stress` 64 rondas OK | Pendiente en los dos equipos | No | Requiere verificación en silicio físico con códec HDA Sunrise Point-LP `8086:9d71`. |
| DMA y Linux shim | Sí | Arena DMA usada por xHCI/MSC; `stress` completó 64 rondas con 8 bloques de 8 KiB y validación de dirección física/virtual | Pendiente | No | Liberación exige puntero, dirección física y tamaño exactos; falta prueba de agotamiento bajo carga. |
| VT-d/IOMMU | Sí | Descubrimiento solamente | Pendiente | No | Sin root/context tables ni domains; no hay aislamiento DMA. |
| VMX de contingencia | Sí | VMXON solamente; sin prueba de rescate | Pendiente | No | Faltan VMCS, VMLAUNCH, VM-exit handler y EPT. Los comandos Triple Fault se bloquean hasta disponer de interceptación real. |
| IDT/TSS/IST | Sí | Arranque con TSS/IST cargadas | Pendiente | No | Stacks separados para #DF, NMI y #MC; faltan excepciones inducidas recuperables. |
| Estrés combinado e invariantes | Sí | `stress`: tres ejecuciones de 64 rondas PMM/heap/DMA + HDA + framebuffer + 64 lecturas MSC; ratón conectado, desconectado y un reporte de movimiento durante la carga | Pendiente | No | Incluye framebuffer, no decodificación H.264 simultánea ni hotplug prolongado. |
| NVIDIA H18–H20 | No | No | No | Sí | No usar como evidencia de soporte GSP. |

Las pruebas QEMU emplearon TCG, 512 MiB o 1 GiB, q35, OVMF, xHCI, USB HID y
USB BOT/MSC. La última imagen de 53 MiB arrancó y completó la prueba exFAT con
1 GiB; una ejecución con 512 MiB quedó detenida antes del mensaje del núcleo y
requiere diagnóstico. No equivalen a validación en hardware. Revisar `BITACORA.md` para
los detalles y los límites de cada medición.

Los dos equipos físicos aportados por el usuario están descritos en
`PERFILES_HARDWARE.md`.
