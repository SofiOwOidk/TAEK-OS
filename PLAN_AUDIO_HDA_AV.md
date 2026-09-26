# Plan de diagnóstico y corrección del audio HDA durante H.264 interactivo

Estado: diagnóstico del código y de las cifras físicas comunicadas por el
usuario. No se modifica el controlador aquí; implementación a cargo de otro
modelo. Objetivo: 4350 cuadros a 30 FPS con audio AAC continuo en la laptop
Intel HDA `8086:9d71`.

## Lo que demuestran las cifras

La prueba física interactiva informó 4350/4350 cuadros, cero omitidos, 144994
ms de duración y p95 de 30,00 ms. La reproducción de video cumple 30 FPS.
El usuario oye audio entrecortado. Los 6245 «vaciados» que muestra
`reproductor.c` son llamadas a `audio_ac97_encolar_pcm()` con retorno negativo,
no eventos de FIFO o silencio DMA medidos. El supuesto «avance DMA» de
25579520 bytes es la suma de los tamaños PCM producidos por AAC; el contador
se incrementa incluso cuando el encolado falla. La igualdad con 6245 paquetes
de 4096 bytes indica fallo de la ruta de encolado en esa corrida, pero no
identifica por sí sola el punto de fallo ni valida consumo por hardware.

La bitácora H64 atribuye los fallos al timeout `SRST=-2` y ya registra un
cambio posterior: un pulso fijo de 20 µs que no espera confirmación. Esa
atribución necesita el código de retorno y registros MMIO del equipo real.
La especificación Intel HDA 1.0a exige que, con `RUN=0`, software confirme
`SRST=1` y después `SRST=0` antes de acceder a los registros del stream.
Un pulso por tiempo fijo no confirma la transición.

Hay una segunda causa independiente visible en el código actual. El primer
paquete AAC típico aporta 4096 bytes, equivalentes a 23,2 ms a 44,1 kHz,
16 bits y estéreo. `audio_hda_encolar_pcm()` inicia HDA inmediatamente y
`hda_llenar_bloque_dma()` precarga dos bloques de 65536 bytes, rellenando el
resto con cero. El anillo inicial abarca 743 ms, de los que unos 720 ms pueden
ser silencio. Las recargas posteriores también rellenan con silencio cualquier
bloque para el que la cola aún no tenga 65536 bytes. El productor AAC alimenta
la cola según el PTS actual del video, por lo que este diseño puede producir
pausas periódicas aunque `SRST` funcione.

## Orden de trabajo del modelo implementador

1. **Separar contadores y registrar el error real.** Medir paquetes AAC
   decodificados, bytes aceptados íntegramente por la cola, bytes rechazados,
   retornos exactos del encolado, bytes copiados al anillo, avance `LPIB/DPIB`,
   `BCIS`, `FIFOE`, `DESE`, ceros insertados y mínimo/máximo de ocupación de
   cola. No llamar «avance DMA» a bytes producidos por AAC ni «underrun» a
   cualquier retorno negativo.
2. **Auditar el arranque del stream en hardware.** Registrar BDF, BAR, `GCAP`,
   número de streams de entrada/salida, índice y dirección del descriptor,
   `SDCTL` antes y después de bajar `RUN`, afirmar `SRST`, negarlo y subir
   `RUN`. Confirmar cada transición por lectura y plazo acotado. Si falla,
   informar etapa y valor MMIO y dejar el stream detenido. No usar el pulso
   fijo como sustituto de la confirmación que exige la especificación.
3. **Arrancar con audio suficiente.** Elegir y medir una política de
   prealimentación: precargar los bloques DMA antes de `RUN` y adelantar la
   decodificación AAC respecto del PTS de video, o reducir el tamaño de los
   periodos BDL y mantener una reserva que cubra el jitter observado. Iniciar
   el reloj A/V al mismo tiempo que el stream. El productor debe mantener
   una ventaja estable sin exceder la cola de 256 KiB.
4. **Aplicar backpressure completo.** La API actual puede copiar sólo parte de
   un paquete y devolver `1`; el reproductor ignora ese valor. Acordar un
   contrato que indique cuántos bytes se aceptaron o que acepte el paquete
   completo tras esperar espacio. Reintentar lo pendiente sin duplicar ni
   omitir muestras y atender HDA/USB durante la espera.
5. **Recargar con propiedad DMA comprobada.** Conservar el seguimiento de
   `LPIB/DPIB/BCIS`; rellenar sólo el bloque ya consumido. Distinguir un
   verdadero vaciado de cola del final de archivo. En un vaciado, registrar
   cuántas muestras silenciosas se insertaron; no contar esos bytes como PCM
   de la pista. Si la posición no avanza de forma fiable, detener y reportar.
6. **Validar antes de declarar resuelto.** Comparar una captura WAV completa
   contra la secuencia PCM esperada con marcas en cada periodo, probar audio
   corto, introducción y H.264 interactivo completo en QEMU y en la laptop.
   Exigir 4350 cuadros, cero paquetes descartados, cero retornos negativos,
   cero silencios insertados durante la pista, ausencia de `FIFOE/DESE` y
   audio audible continuo. Medir desfase A/V con el reloj de bytes realmente
   reproducidos; no deducirlo sólo del PTS de video.

## Prioridad frente a H.264

La corrida física ubica YUV→RGB en 7399 ms (8,23 %) tras la optimización y
la compensación inter en 58861 ms (65,49 %). Las mejoras de `inter.c` pueden
continuar como trabajo separado. El fallo audible y la telemetría engañosa de
HDA son la puerta de aceptación para declarar operativo el reproductor A/V.
