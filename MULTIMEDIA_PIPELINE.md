# Pipeline multimedia y diagnóstico de pausas

## Implementación

La reproducción reserva hasta 8 MiB de video comprimido y 2 MiB de AAC, con
256 descriptores como máximo por cola. La precarga comienza con cuatro muestras
de video y 64 de audio y repone al bajar de dos/32. Cada cola tiene su cursor
MP4 privado: los offsets, tamaños, PTS y duraciones se calculan sin I/O; la
extracción conserva la secuencia completa de decodificación, incluidos P/B.
Las muestras mayores que su presupuesto generan un error explícito.

El BSP es el único propietario de VFS/MSC BOT. La precarga es **síncrona entre
tareas**, con servicio y cancelación entre muestras y bloques. El transporte
actual no ofrece solicitudes asíncronas; no se simula concurrencia sobre BOT.
La cola consume RAM y la lectura no entra en callbacks de reconstrucción.
Queda pendiente transformar xHCI/MSC en una máquina de estados asíncrona con
terminaciones verificables antes de introducir I/O realmente solapado.

Dos ventanas compartidas de hasta 64 KiB reutilizan rangos entre audio/video,
incluso al cruzar una ventana. La identidad del medio se valida antes de entregar
datos cacheados y después de cada lectura. FAT incorpora cuatro sectores de
tabla y un sector parcial por cursor. Los tramos físicos se agrupan únicamente
después de verificar los clusters y, en exFAT, su bitmap. Nunca se deduce
contigüidad de los offsets MP4. El límite MSC/DMA sigue en 64 KiB.

Los callbacks del decoder atienden exclusivamente sondeo/cancelación. El sondeo
breve de xHCI difiere enumeración y resets hasta el sondeo normal, también cuando
se invoca indirectamente desde la consola. El audio se produce desde su cola
comprimida antes de decodificar video y durante las esperas PTS. Las filas
conservan sus dependencias. El selector SMP permite BSP calculando o coordinando
y uno, dos o cuatro participantes; una CPU siempre calcula.

Dos superficies RGB tienen ownership por slot hasta terminar la copia síncrona.
En interactivo se descartan presentaciones atrasadas más de dos períodos cuando
hay un cuadro más reciente o ya se presentó en ese período. Se mantiene una
presentación reciente para evitar una pantalla vacía cuando el decoder es lento.
Benchmark y forense conservan el procesamiento completo. El reloj A/V usa el
avance de audio con continuidad monótona. GOP confirma copia, **no scanout ni
sincronización de refresco**.

## Contadores y perfiles de instrumentación

Cada contador del informe declara su alcance; los intervalos padre no se suman
con sus hijos:

| Contador | Alcance |
|---|---|
| `READ_CYCLES/BYTES/CALLS` | Llamadas a la fuente tras la caché; no son comandos USB |
| `USB_READ_COMMANDS/BYTES` | Comandos reales READ(10) y bytes físicos de la sesión |
| `RECON_WALL_CYCLES` | Pared en el BSP de todos los lotes de reconstrucción |
| `WORKER_CPU_CYCLES` | Suma del cómputo de los trabajadores (pared de fila menos espera) |
| `DEPENDENCY_CPU_CYCLES` | Espera por avance de la fila anterior, dentro de la pared de fila |
| `RECON_REGIONES(_OK)` | Filas despachadas y ejecutadas exactamente una vez (por identificador) |
| `RECON_REGIONES_DUP/FALTA` | Regiones repetidas o nunca ejecutadas; deben ser 0 |
| `RECON_RATIO_MILES` | Peor relación cómputo/pared (×1000); > participantes indica relojes no comparables |
| `DEBLOCK_SEG_EVAL/FILT/DESC` | Segmentos de borde evaluados, filtrados y descartados (EVAL = FILT + DESC) |
| `DEBLOCK_H/V`, `DEBLOCK_LUMA/CROMA` | Segmentos filtrados por orientación y por plano |
| `DEBLOCK_FUERZA_TIPO/NZ/MOV` | Origen de la fuerza: intra/inter, coeficientes no nulos, movimiento |
| `DEBLOCK_SKIP_*` | MB/franja/8x8/slice sin filtrar, para interpretar los tiempos |
| `DEBLOCK_KERNEL_SSE2/ESCALAR` | Llamadas al kernel SSE2 y al escalar |
| `DEBLOCK_CICLOS_FUERZA/KERNEL` | Ciclos del cálculo de fuerza y del kernel (sólo perfil detallado) |
| `SILENCE_*_BYTES` | Relleno de silencio durante pista y tras EOF |
| `HEAP_DELTA` | Heap tras liberar menos heap inicial |

`H264_PERFIL_INTER=1` sólo activa las subetapas de compensación inter. Si el
TSC de un AP difiere del BSP, `SMP` emite `TSC_DESVIACION` y la reconstrucción
marca `RECON_INCOHERENTES` o `RECON_RATIO_MILES` por encima del número de
participantes; **el TSC es la hipótesis, no una causa confirmada**, y no se
presentan los ciclos por CPU como comparables sin normalizar.

`H264_TELEMETRIA_DETALLADA=1` (por defecto) mide pared por macrobloque y ciclos
por borde de deblocking; sirve para localizar costes. `H264_TELEMETRIA_DETALLADA=0`
es el perfil de rendimiento con instrumentación mínima (pared por lote y
recuentos de bordes, sin relojes por macrobloque ni por borde) para comparar
velocidad. La capa de reconstrucción conserva siempre pared por lote.

## Comparación reproducible

Con el volumen USB montado y el mismo MP4 (ruta absoluta):

```text
h264 cpu 4
h264 bsp calcula
h264 io 4
reproducir usb bench /video.mp4
reproducir ram bench /video.mp4
reproducir usb /video.mp4
reproducir ram /video.mp4
```

`h264 io 4` queda fijado como **referencia** antes de comparar `io 16/32/64`
(`[PIPELINE_CONFIG] IO_KIB=4 REFERENCIA=1`). Las dos compilaciones se preparan
con `herramientas/preparar_comparacion.sh`: genera el USB con el mismo fragmento
y las ISO de **perfil detallado** (`H264_TELEMETRIA_DETALLADA=1`, localiza costes)
y **perfil de rendimiento** (`=0`, instrumentación mínima para velocidad), además
de `comandos.txt`. El perfil detallado se usa para elegir el siguiente objetivo;
el de rendimiento para medir si mejoró. Ambos ejecutan la **misma fuente** desde
RAM y USB.

Para el 1080p, `herramientas/preparar_usb_1080p.sh` crea un disco FAT32 con el
mismo fragmento que la ISO (`/video_1080p_ram.mp4`, SHA-256 idéntico) y el 360p;
úsalo como medio USB para que RAM y USB compartan bytes exactos. Comparar
primero cuatro CPUs con BSP calculando y luego variar una condición por vez.

### Deblocking por componente (C1)

`h264_desbloquear` separa fuerza (vecinos, QP y umbrales, `movimiento_distinto`)
de la aplicación del kernel, con recuentos de segmentos siempre activos y ciclos
por borde sólo en el perfil detallado. En el fragmento 360p de 61 cuadros se
midió `DEBLOCK_SEG_EVAL=1.780.468 = FILT 367.451 + DESC 1.413.017`, kernel
escalar 754.193 y SSE2 0: el despacho por defecto (`etapas_sse2=6`) mantiene el
filtro SSE2 desactivado (bit 0), tal como documentó la medición previa. La
equivalencia escalar/SSE2 (`tests/probar_h264_etapas_host.sh`) cubre fuerzas
1–4, luma/croma, horizontal/vertical, extremos 0/255, umbrales, strides 1/32/37/64
y un frame completo idéntico entre ambas rutas.

`ram` es diagnóstico explícito: máximo 32 MiB de archivo, además del presupuesto
de 256 MiB del reproductor. La copia inicial no forma parte del tiempo de
decodificación. Un archivo mayor requiere un MP4 independiente recortado desde
IDR; usar **ese mismo fragmento** para RAM y USB. El `make` genera ese fragmento
automáticamente desde `Recursos Asets/Video 1080p.mp4` con
`herramientas/crear_fragmento_ram.sh` y lo graba como `/boot/video_1080p.mp4` en
la ISO y en la imagen FAT, de modo que el módulo Limine y `reproducir ram`
comparten la misma fuente <= 32 MiB. Si el video fuente cambia, borra
`build/video_1080p_ram.mp4` para forzar su regeneración. Ejemplo manual desde el
comienzo:

```bash
ffmpeg -i entrada.mp4 -t 10 -c copy fragmento.mp4
```

Repetir con `h264 cpu 1`, `h264 cpu 2`, `h264 cpu 4`, manteniendo SSE2 y la
fuente. Con cuatro CPUs repetir `h264 bsp coordinador`. Comparar luego
`h264 io 16`, `h264 io 32`, `h264 io 64`. El valor previo de 4 KiB se conserva
por defecto hasta medir latencia de servicio y cancelación en hardware.
Una geometría fragmentada o clusters pequeños pueden limitar el tamaño efectivo.

El informe serial incluye comandos READ(10), bytes físicos, fallos de
disponibilidad, cuadros descartados y distribución. Los contadores USB incluyen
las tablas FAT durante la sesión, pero excluyen apertura MP4 y precarga diagnóstica
del archivo completo. `READ_CALLS` mide llamadas a la fuente tras la caché;
`USB_READ_COMMANDS` mide comandos reales. No son intercambiables.

## Trazas

Cuatro anillos de 512 eventos, escritos por su CPU y volcados tras unir los AP;
sin impresión por cuadro. Se preserva el máximo de cada etapa/CPU y una copia
acotada de la traza alrededor de la mayor operación de recarga/decodificación
de al menos 150 ms. Las marcas TSC son absolutas. Para comparar CPUs se necesita
un TSC sincronizado; los ciclos no se convierten sin calibración disponible.

Campos: CPU, etapa, muestra, PTS, inicio, fin, detalle, ocupación PCM en bytes,
cuadros RGB pendientes. En los AP las ocupaciones son cero: no consultan hardware.
Las muestras de audio/video tienen espacios de índice diferentes; etapa demux
usa `detalle=0` para video y `detalle=1` para audio. PTS de lectura identifica la
muestra que solicitó la ventana, que puede contener bytes de varias muestras.
Los eventos del decoder usan el PTS de su foto, necesario para el reordenamiento.
El índice de muestra en una salida identifica la llamada que la emitió; el PTS
identifica el cuadro. La etapa reconstrucción usa detalle=fila; dependencia,
detalle=(fila << 32) | avance requerido de la fila precedente.

| Etapa | Medición |
|---|---|
| 1 | CPU de las tablas MP4, sin lectura |
| 2 | Lectura a la fuente: ciclos y bytes de la ventana |
| 3 | Pared del decoder; incluye hijos |
| 4 | Pared de una fila por trabajador; incluye dependencias |
| 5 | Espera por avance de la fila anterior |
| 6 | Desbloqueo |
| 7 | Conversión YUV/RGB |
| 8 | Cuadro convertido listo para presentar |
| 9 | Copia al framebuffer |
| 10 | Servicio audio/xHCI/entrada |
| 11 | AAC |
| 12 | Espera PTS; detalle=ciclos de trabajo a descontar |
| 13 | Presentación descartada por retraso |

```bash
python3 herramientas/informe_traza.py captura.serial.log --umbral-ms 150
```

Las duraciones padre no se suman a sus hijos. El cómputo de fila es su pared menos
dependencias y servicio BSP superpuesto. Los anillos finales pueden sobrescribir
contexto antiguo: el analizador lo indica; los máximos y la captura de la mayor
pausa sobreviven. No garantiza guardar todas las pausas de una reproducción larga.

Cada compilación registra HEAD completo y SHA-256 del árbol real. El manifiesto
`build/fuentes.json` enumera archivos y hashes, incluidos fuentes/pruebas locales
ignorados por Git. Conservarlo junto con kernel, flags, MP4 y log de cada prueba.

## Validación automatizada

```bash
bash tests/probar_pipeline_host.sh
bash tests/probar_mp4_error_fuente.sh
bash tests/probar_h264_etapas_host.sh
bash tests/probar_fat32_host.sh
bash tests/probar_exfat_host.sh
bash tests/preparar_pipeline_qemu.sh
```

Desde PowerShell: `./tests/probar_pipeline_qemu.ps1`.
La prueba QEMU usa el mismo fragmento en módulo RAM y USB FAT virtual, compara
CPU 1/2/4, BSP coordinador/calculando, benchmark e interactivo y heap tras sesiones.
Los tests host usan ASan/UBSan y comparan todos los bytes y PTS de ambas pistas,
caché compartida, cruce/fin de ventanas, lectura corta, desconexión y cancelación.

## Frente Intel

`h264 intel h0` registra todos los dispositivos gráficos Intel encontrados:
BDF, ID PCI, revisión, subsystem y BAR0/tamaño. Es inventario para H0; no declara
viabilidad completada ni habilita un backend sin ejecución verificable.

**H0 no está cerrado** (ver `docs/intel/H0_VERIFICACION_FUENTES.md`): la etiqueta
`intel-gmmlib-24.3.0` no existe, los commits y SHA-256 de firmware previos se
corrigieron contra los repositorios oficiales, el alcance listaba rutas ausentes
y el ensayo de compilación con fuentes reales falla bajo el objetivo TAEK
(`cstdio` en Media Driver; `drm/drm_managed.h` en i915). Las pruebas de contratos
de `tests/intel/` son simulaciones, no verificación de adaptadores reales.

Fuentes oficiales revisadas: [Intel Media Driver](https://github.com/intel/media-driver)
y [documentación i915](https://docs.kernel.org/gpu/i915.html). Media Driver enumera
KBL/CFL/WHL y soporte AVC; i915 documenta GGTT/PPGTT, objetos de memoria, contextos
y envío de trabajo. La familia de CPU no sustituye al ID/revisión de la GPU.

Para una GPU Gen9/9.5 confirmada, contrastar el PRM de **esa generación** con las
implementaciones MFX de Intel: modo de pipeline, estado de superficies, direcciones
de buffers e indirectos, estado AVC de imagen/referencias/slices y objetos BSD.
Antes de un batch AVC, H1 necesita memoria fijada y mapeada GPU, coherencia/TLB,
contexto del motor VCS, envío, fence/terminación, timeout y reset del motor. No
reutilizar offsets/estructuras de Tiger Lake en Kaby Lake. El inventario físico
exacto todavía falta; por esa dependencia los hitos H1–H5 quedan pendientes.

H2 comparará YUV de una IDR independiente; H3 requiere DPB y cuadros P/B;
H4 incorpora selección/cancelación y recuperación desde IDR, después de detener
trabajo pendiente; H5 valida el archivo completo en la laptop. NVIDIA queda fuera.
La optimización CPU posterior y AVX2 también requieren el perfil físico desde RAM.
Estas pruebas de host/QEMU no demuestran 1080p a 23,976 FPS ni ausencia de cortes
en audio real; esos criterios se verifican en el hardware con los comandos anteriores.

Resultados obtenidos en esta implementación: 4.350 muestras de video y 6.245 de
AAC idénticas al demux original; lectura completa FAT del MP4 de 10.106.942 bytes
con SHA-256 idéntico; 61 cuadros del fragmento con hashes iguales al decoder
escalar en las rutas SSE2 de uno, dos y cuatro trabajadores. Las nueve sesiones
QEMU del fragmento completaron con `HEAP_DELTA=0`; las dos sesiones interactivas
registraron `AUDIO_UNDERRUN_TRACK=0`. Los tiempos TCG/host no se extrapolan a la
laptop. La matriz extensa de FS no se ejecutó por faltar su manifiesto de fixtures;
las pruebas específicas FAT32/exFAT y el MP4 completo sí se verificaron.
