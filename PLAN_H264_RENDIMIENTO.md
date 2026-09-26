# Plan de rendimiento H.264 para la laptop de pruebas

Estado: plan de trabajo actualizado con la medición física comunicada por el
usuario. SMP no está implementado. El benchmark H.264 cumple 30 FPS en 360p;
la reproducción A/V completa requiere validación separada. Implementación a
cargo de otro modelo.

## Objetivo y hechos del código actual

El video integrado `h264:360p` tiene 640×360 píxeles, 30 fps y 4350 cuadros
(`H264_VALIDACION.md`); cada cuadro dispone de 33,33 ms. Se busca reproducción
fluida del archivo completo en la laptop i7-8650U, sin perder la exactitud del
decodificador ni la continuidad de audio/USB.

* El bucle de `reproductor.c` demultiplexa AAC, decodifica AAC y H.264, convierte
  YUV a RGB y presenta en serie. `presentar()` espera si un cuadro llega antes
  de su PTS, pero también dibuja los cuadros que ya llegan tarde.
* `h264_convertir_rgb()` calcula las coordenadas de escala y los tres canales
  por píxel; `pantalla_dibujar_imagen_centrada()` copia cada píxel al framebuffer.
* El decodificador mantiene CABAC, slice actual, fotograma actual y DPB en un
  único `h264_decodificador`. `decodificar_slice()` recorre macroblocks en orden.
  El filtro de desbloqueo consulta bordes vecinos. Repartir esos macroblocks
  entre núcleos sin rediseñar dependencias produciría cuadros incorrectos.
* No hay arranque de procesadores secundarios: APIC e IPI existen, pero no hay
  solicitud SMP de Limine ni trabajadores del kernel. `Makefile` compila con
  `-O2` y deshabilita SSE/SSE2. Activar SSE globalmente antes de gestionar su
  estado por CPU/interrupción no es una optimización segura.
* La ruta AAC llama `audio_ac97_reproducir_pcm()` por paquete. En HDA termina
  llamando a una función que detiene y reinicia el stream; en AC97 la ruta DMA
  conserva una referencia al PCM local. Se necesita una cola PCM persistente y
  un único dueño del stream antes de usar el audio como reloj A/V fiable.
* La prueba histórica de QEMU de 118832 ms para 4350 cuadros es `h264 prueba`:
  calcula huella YUV en todos los cuadros y dibuja uno de cada 100. No mide la
  reproducción interactiva completa ni la laptop física.

## Actualización tras el Encargo A

La bitácora registra una corrida forense completa de 4350 cuadros en **QEMU
TCG**, con 399 205 millones de ciclos (91,67 %) en CABAC/sintaxis/reconstrucción
y 31 408 millones (7,21 %) en desbloqueo. El p95 fue 65,64 ms y el p99
87,84 ms. Los porcentajes de TCG no son los porcentajes del i7-8650U físico.
La huella YUV
coincidió con la referencia y los canarios del heap quedaron intactos.

El 91,67 % agrupa trabajo secuencial y trabajo potencialmente vectorizable.
CABAC actualiza estados según cada símbolo y su contexto; **no se debe dar por
hecho que CABAC en sí se acelere con SSE2/AVX2**. Antes de programar SIMD,
separar contadores para CABAC puro, predicción intra, compensación inter,
transformadas 4×4/8×8, Hadamard y copias. La conversión RGB (0,11 %) y la
copia al framebuffer (<0,01 %) parecían secundarias en QEMU. La medición
física posterior cambió esa prioridad. Incluso una mejora de 4× en
desbloqueo, sin tocar otras etapas, sólo aceleraría el total de esta corrida
de QEMU cerca de 6 %.

## Actualización con la prueba física comunicada por el usuario

El usuario reporta 4350/4350 cuadros, cero omitidos, 47,83 FPS de capacidad,
camino crítico medio 20,90 ms, p50 21,83 ms, p95 26,33 ms, p99 28,02 ms,
pico de heap 3 732 422 bytes y canarios intactos. Desglose: CABAC y
reconstrucción 71,07 % (64,3 s), YUV→RGB 20,88 % (18,9 s), desbloqueo 7,35 %
(6,6 s), demux y copia al framebuffer <0,7 %. Esto cumple el presupuesto de
33,33 ms del benchmark 360p; no demuestra aún ausencia de underruns, sincronía
A/V o fluidez de la reproducción interactiva completa.

La descripción dice «Segunda Foto» e i7-8650U. En `PERFILES_HARDWARE.md`, la
primera foto corresponde a la laptop `8086:5917` y la segunda al equipo con
NVIDIA. Conservar el BDF/CPU de la traza para atribuir el resultado al equipo
correcto.

**Nueva prioridad para el implementador:** primero corregir el bucle HDA y
validar A/V; después optimizar YUV→RGB como kernel aislado, con igualdad exacta
de píxeles y tiempos físicos antes/después. Aun acelerándola 4×, la ganancia
total aproximada es 1,186× por Amdahl si las demás etapas no cambian. Separar
el 71,07 % de CABAC/reconstrucción antes de elegir optimizaciones H.264.
El desbloqueo queda tercero. SMP no es necesario para 360p según este
benchmark; continúa como trabajo de arquitectura independiente.

Para HDA, registrar LPIB, posición DMA en RAM, BCIS y cursor; recargar sólo
un bloque cuya lectura haya concluido. El tiempo transcurrido por sí solo no
demuestra que sea seguro sobrescribir un bloque.

## Contrato común de medición (primera entrega)

La primera implementación debe medir, con la misma imagen y archivo, en QEMU
y en la laptop con alimentación de CA. Registrar versión, resolución, duración
real, CPU/núcleos detectados y salida serial. Repetir una corrida fría y otra
caliente para detectar degradación térmica.

Agregar contadores de ciclos/tiempo acumulados y máximos, sin imprimir por
cuadro, para estas etapas: demux MP4; AAC; CABAC/sintaxis y reconstrucción H.264;
desbloqueo; conversión YUV→RGB; copia al framebuffer; espera por PTS; servicio
HDA/USB. Registrar además cuadros decodificados/presentados/omitidos, percentiles
p50/p95/p99 del camino crítico, retraso frente a PTS, avance DMA y vaciados de
audio. No sumar tiempos anidados como si fueran etapas independientes.
Comprobar la calibración de TSC antes de comparar milisegundos; en SMP, medir
ciclos por CPU y no restar TSC de dos núcleos sin demostrar sincronización.

Ejecutar por separado `h264 prueba 360p`, `h264 360p` y `aac 360p`. El modo
`prueba` actual incluye hash de todos los cuadros y presentación ocasional:
etiquetarlo así en el informe, o añadir modos de benchmark que separen esas
operaciones. Conservar como referencia la huella YUV de 360p
`984a4460415d1b1c`; la comparación exacta contra FFmpeg descrita en
`H264_VALIDACION.md` sigue siendo la prueba de corrección.

## Frentes que pueden implementarse en paralelo

| Encargo para el modelo implementador | Archivos bajo su responsabilidad | Entrega comprobable | Dependencia |
|---|---|---|---|
| A. Telemetría | `multimedia/reproductor/reproductor.c`, `h264/decodificador.c` | Perfiles QEMU y físico recibidos; confirmar equipo/modo, medir A/V y desglosar el 71,07 % | Punto de partida de B/G |
| B. Ruta principal H.264 | `h264/cabac.c`, `inter.c`, `prediccion.c`, `residuo.c` | Identificar funciones dominantes; optimizar primero escalar y después kernels SIMD aislados de predicción/transformada que ganen tiempo físico sin alterar bytes | Desglose A + oráculo E |
| C. Desbloqueo | `h264/filtro.c` | Reducir tiempo de filtro y p99 con bordes luma/croma correctos, incluidos límites de slice y cuadro | Puede avanzar junto con B; oráculo E |
| D. Cola de audio persistente | `audio_hda.c`, `audio_ac97.c`, sus cabeceras | Un stream continuo, PCM con vida válida hasta consumo DMA, contador reproducido y prueba de underrun | Puede avanzar junto con B/C; integración posterior en `reproductor.c` |
| E. Oráculo y regresiones host | `tests/`, `herramientas/h264/`, `H264_VALIDACION.md` | Comparación exacta YUV/RGB, sanitizadores, slices múltiples, vídeo completo y cancelación | Puede avanzar junto con A/B/C/D |
| F. Infraestructura SMP | nuevo módulo SMP, Limine MP, APIC, GDT/TSS/IST y memoria por CPU | APs con stack propio y cola de trabajos pequeña; barrera de inicio/fin y apagado limpios | Independiente de B/C/D; integrar después de E |
| G. YUV→RGB | `h264/imagen.c` | Prioridad tras HDA: reducir el 20,88 % físico, con salida RGB idéntica y métricas antes/después | Perfil físico A + oráculo E + estado SIMD seguro |

Para evitar conflictos, A es el único frente que modifica `reproductor.c` al
principio. B y C no se reparten el mismo archivo; E mantiene el oráculo sin
cambiar el códec. D añade su API de encolado y sólo después acuerda con A la
llamada desde el reproductor. F no toca el decodificador. Cada encargo se
integra por separado sobre una base que compila y supera E antes del siguiente.

## Orden de integración y decisiones

1. **Confirmar perfil y corregir audio.** Identificar el equipo de la prueba.
   Resolver el bucle HDA antes de evaluar reproducción A/V. Incorporar la
   cola PCM persistente, con límite de capacidad y backpressure; el controlador
   consume búferes propios y no reinicia DMA por muestra AAC. El reloj A/V usa
   bytes realmente reproducidos y se reinicia de forma explícita al empezar
   otra reproducción. Si HDA/AC97 no está operativo, usar el reloj monotónico.
2. **YUV→RGB.** Medir y reducir cálculos repetidos por píxel. Comparar salida
   exacta y tiempos físicos de una mejora escalar antes de elegir SIMD.
3. **Desglose del núcleo H.264.** Separar dentro del 71,07 % físico CABAC puro,
   predicción intra/inter,
   interpolación, transformadas inversas, Hadamard y escritura de píxeles.
   Comparar ciclos por cuadro I/P/B y máximos; elegir los dos subtramos más
   caros en hardware. Optimizar primero su forma escalar y cambiar una función
   por vez con comparación exacta de cada cuadro. CABAC se estudia por tablas,
   ramas y acceso a memoria; SIMD se dirige a operaciones de varios píxeles o
   coeficientes independientes, no al estado adaptativo secuencial de CABAC.
4. **SIMD acotado y seguro.** Mantener `-mno-sse2` global. Antes de ejecutar
   instrucciones XMM/YMM en Ring 0, comprobar CPUID y soporte del SO para
   guardar/restaurar estado SIMD por CPU e interrupción; para AVX2 comprobar
   también OSXSAVE/XGETBV y el estado YMM. Compilar kernels separados con
   dispatch escalar/SSE2/AVX2 y fallback probado. Si esa infraestructura no
   existe, entregar sólo la mejora escalar. Probar redondeo, saturación y bordes
   bit a bit en transformadas 4×4/8×8, Hadamard y predicción.
5. **Filtro de desbloqueo.** Optimizar `filtro.c` por separado, primero con
   perf físico y luego, si compensa, con XMM para luma/croma. Mantener el orden
   de bordes y los límites entre slices: vectorizar filas arbitrarias o filtrar
   bordes contiguos a la vez puede cambiar los píxeles vecinos. La prioridad
   principal es reducir p99 sin alterar el resultado de los cuadros críticos.
6. **Política de presentación.** Conservar siempre la decodificación de cuadros
   necesarios para referencias/DPB. Si la salida llega tarde, se puede omitir
   conversión RGB y presentación de ese cuadro, con umbral medido; no saltar
   NALs ni cuadros de referencia por intuición. Medir mejora de latencia y
   contar omisiones. Comprobar PTS de salida reordenada y sincronía con audio.
7. **SMP opcional.** Inicializar APs por Limine con stacks, TSS/IST y datos por
   CPU; añadir una cola de trabajos de capacidad fija y barreras con semántica
   de memoria clara. No acceder al heap/PMM, xHCI ni al controlador HDA desde
   trabajadores hasta que sus rutas sean seguras para concurrencia. Una prueba
   independiente debe verificar ejecución en más de un APIC ID, finalización
   ordenada, cancelación por ESC y ausencia de trabajos pendientes al destruir
   un fotograma.
8. **Primera carga paralela.** El benchmark físico ya cumple 30 FPS en un
   núcleo. Usar YUV→RGB por franjas sólo como
   prueba controlada de la cola de trabajos: escrituras RGB disjuntas, YUV/DPB
   inmutable y barrera antes de presentar. Comparar 1, 2 y 4 núcleos en la
   laptop; retirar el cambio si empeora la latencia total. No paralelizar CABAC
   de una sola slice compartiendo `h264_decodificador`.
9. **Paralelismo más profundo, condicionado por el perfil.** Separar AAC y video
   en un pipeline acotado con un solo propietario del demux y del stream HDA,
   o estudiar slices H.264 independientes si el archivo contiene suficientes.
   El video habitual puede tener una sola slice por cuadro; CABAC y predicción
   de macroblocks dependen de vecinos y referencias. Decodificación simultánea
   de cuadros B/P requiere un grafo de dependencias y contextos privados, por
   lo que se trata como un proyecto posterior con prueba bit a bit propia.

## Criterios de aceptación y límites

* Conservar los 4350 cuadros de 360p y el p95 físico reportado de 26,33 ms;
  objetivo de reproducción completa: menos de 1 % de presentaciones omitidas, cero
  underruns DMA y desfase A/V inferior a 80 ms durante el archivo completo.
  Registrar también p99, duración total y mejora frente a la base física.
* Mantener igualdad exacta de los planos YUV y de los píxeles RGB para los
  casos soportados, con orden POC/PTS correcto. La huella sola no reemplaza
  la comparación byte a byte. Probar 360p y los fragmentos de cuatro slices;
  ejecutar 1080p como prueba de corrección y memoria, sin prometer tiempo real.
* Ejecutar ASan/UBSan del arnés host, QEMU con 1 y varios vCPU, y luego la
  laptop. `ESC` debe cancelar sin jobs colgantes ni uso tras liberar. Verificar
  canarios de heap, ausencia de bloqueos, continuidad HDA y que USB/HID sigue
  respondiendo durante reproducción.
* Si una optimización no reduce el tiempo total en hardware o altera la salida,
  revertirla. El benchmark físico no exige SMP para el objetivo de 360p.

## Entregas para revisión

Cada encargo debe incluir: resumen de archivos cambiados, cifras antes/después
de la misma prueba, salida de las pruebas exactas, fallos restantes y efecto
sobre A/V. No atribuir un fallo de reproducción A/V al rendimiento del
decodificador sin medir presentación, audio y USB durante la ejecución.

---

## Estado de implementación de encargos (2026-09-26)

### 1. Encargo D: Cola Persistente de Audio HDA / AC97 (Completado)
* **Archivos:** `audio_hda.h`, `audio_hda.c`, `audio_ac97.h`, `audio_ac97.c`, `reproductor.c`.
* **Implementación:**
  - Cola circular de 256 KiB (`g_cola_pcm`) desacoplada del motor DMA de 128 KiB (2 bloques ping-pong de 64 KiB).
  - API `audio_hda_encolar_pcm()` con backpressure, evitando que cada paquete AAC (2048 muestras / 4096 bytes) reinicie el hardware con `SRST`.
  - Alimentación continua en `hda_llenar_bloque_dma()` desde la cola circular; si la cola se vacía, rellena con silencio (0) y registra underrun sin parar el hardware.
  - Reloj A/V basado en hardware DAC mediante `audio_hda_obtener_tiempo_ms()` y reinicio explícito en `audio_hda_reiniciar_reloj()`.
  - Integración en `presentar()` de `reproductor.c` para sincronizar video con el reloj de audio del DAC.

### 2. Encargo G: Optimización Escalar de YUV $\rightarrow$ RGB (Completado)
* **Archivos:** `nucleo/controladores/multimedia/h264/imagen.c`, `tests/pruebas_yuv_rgb_host.c`.
* **Implementación y Resultados:**
  - Sustitución de dos divisiones enteras de 64 bits por píxel (`(uint64_t)x * ancho / w`) por paso incremental entero de punto fijo.
  - Reutilización de coeficientes de croma U/V para pares horizontales de píxeles y líneas de croma 4:2:0.
  - Validación de coincidencia exacta bit a bit: **0 discrepancias en 230,400 píxeles por fotograma** en las 6 combinaciones (rango completo/limitado y matrices BT.601, BT.709, BT.2020) en el oráculo `tests/pruebas_yuv_rgb_host.c`.
  - En pruebas de banco de micro-benchmark host, la rutina elimina las divisiones pesadas y acelera radicalmente el cálculo escalar, atacando directamente los 18.9 s (20.88%) medidos en hardware real.

### 3. Encargo A: Desglose de CABAC / Reconstrucción (Completado)
* **Archivos:** `h264.h`, `decodificador.c`, `reproductor.c`.
* **Implementación:**
  - Instrumentación de sub-etapas en `h264_telemetria`: `ciclos_cabac_puro`, `ciclos_inter` (compensación de movimiento) y `ciclos_intra` (predicción espacial y Hadamard/DCT).
  - Medición por macrobloque en `decodificar_macrobloque()` acumulada sin anidamiento.
  - Emisión en pantalla en la tabla de benchmark y en el informe serial `[BENCHMARK]`:
    * `CABAC_PURO_CICLOS`
    * `INTER_CICLOS`
    * `INTRA_CICLOS`
  - Base lista para guiar las optimizaciones escalares/SIMD de los Encargos B y C.

