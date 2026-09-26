# H.264 nativo: resultados y límites

Estado: **hito confirmado por el usuario tras verificar el funcionamiento en
hardware físico**. Sigue siendo un decodificador experimental con cobertura
parcial del estándar H.264.

## Implementación y procedencia

El decodificador de video se escribió en C para TAEK OS, sin incorporar código
de bibliotecas de códecs. Utiliza aritmética entera, callbacks de memoria y
presentación, y se compila con las opciones freestanding del núcleo. El host
usa un arnés con libc para alimentar exactamente los mismos fuentes.

La referencia normativa fue [ITU-T H.264, edición 08/2024](https://www.itu.int/rec/T-REC-H.264-202408-S/en).
Las tablas numéricas CABAC proceden de esa especificación; el generador local
`herramientas/h264/generar_tablas.py` conserva su procedencia. FFmpeg se usó como
oráculo de pruebas y para preparar fragmentos. La reproducción nativa consume
los MP4 comprimidos: no utiliza los fotogramas que produce FFmpeg.

El usuario confirmó después que también lo probó y funciona en su dispositivo
real. No se registró aquí el modelo del equipo ni las condiciones exactas de esa
prueba. La integración AAC y la reorganización posterior a `multimedia/` son
cambios distintos; el usuario debe verificarlos por separado.

## Cobertura real

- AVC progresivo, YUV 4:2:0 de ocho bits, CABAC y matrices de escala uniformes.
- Slices I/P/B; predicción intra 4×4, 8×8 y 16×16; compensación de movimiento,
  interpolación fraccional, predicción ponderada y modos directos espacial/temporal.
- Transformadas inversas, reconstrucción, filtro de desbloqueo, referencias DPB,
  reordenación de salida y operaciones MMCO. POC de tipo 0.
- MP4 no fragmentado: configuración avcC, tamaños/ubicaciones de muestras y tiempos
  de decodificación/composición, incluyendo offsets de composición negativos.
- Presentación YUV→RGB por CPU sobre el framebuffer GOP. Reducción al tamaño de
  pantalla conservando la relación entre ancho y alto.

**No soportado:** CAVLC, I_PCM, POC 1/2, video entrelazado/MBAFF, FMO, SP/SI,
profundidades superiores a ocho bits, otros formatos de croma y matrices de escala
personalizadas. Tampoco se implementaron listas de edición MP4, rotación ni
corrección de píxeles no cuadrados. No se afirma conformidad completa con H.264.
El lector MP4 original ignora las pistas de audio; este registro no certifica AAC.

Los límites del núcleo del códec son 4096 píxeles por dimensión, 16 referencias y
16 MiB por NAL. La disponibilidad de memoria puede imponer límites menores.
El adaptador de reproducción limita sus reservas a 256 MiB. El rendimiento de
1080p en tiempo real no está garantizado.

## Archivos originales probados

| Archivo | Perfil y formato | Fotogramas | SHA-256 del MP4 |
|---|---|---:|---|
| `Recursos Asets/Video 360p.mp4` | Main, 640×360, YUV420p, 30 fps | 4350 | `a8f64507ea32f8e4622473116ca90e1d4ad076421fe58f169d759b6e3f8d6ae8` |
| `Recursos Asets/Video 1080p.mp4` | High, 1920×1080, YUV420p, 24000/1001 fps | 4637 | `d487dc5b1027285f2084e55a8bc71ea345e36ac9231fa186d7f6f27d70846fbf` |

Se compararon **todos los bytes de los tres planos YUV visibles de todos los
fotogramas**, en orden de presentación. No se utilizó una tolerancia visual ni
una comparación de unos pocos fotogramas para declarar el resultado completo.

## Resultados de la implementación original

| Prueba | Resultado |
|---|---|
| 360p completo contra FFmpeg, ASan y UBSan | 4350 fotogramas idénticos; procesos con salida 0 |
| 1080p completo contra FFmpeg, ASan y UBSan | 4637 fotogramas idénticos; procesos con salida 0 |
| Segunda comparación de 1080p, compilación `-O2` | 4637 fotogramas idénticos |
| Predicción temporal y cuatro slices, fragmento recodificado de prueba | 32 fotogramas idénticos |
| Mismo fragmento con CTTS negativo | 32 fotogramas idénticos |
| Entrada Baseline con CAVLC | Rechazo explícito `H264_NO_SOPORTADO`; cero fotogramas |
| libFuzzer, corpus MP4/NAL y sanitizadores | 311 ejecuciones en 92 s sin fallo detectado |
| libFuzzer, corpus corto y sanitizadores | 175181 ejecuciones en 121 s sin fallo detectado |

El fuzzing tiene tiempo, tamaño de entrada y presupuesto de memoria acotados.
Estos resultados no constituyen una prueba de ausencia de vulnerabilidades.

### Ejecución real del núcleo en QEMU

QEMU ejecutó el kernel freestanding completo con 1 GiB de RAM y aceleración TCG.
La huella FNV-1a se calculó sobre las muestras visibles Y, U y V. Sirve para
contrastar las ejecuciones nativas; no es criptográfica. La igualdad exacta del
host se verificó adicionalmente byte por byte como se describe arriba.

```text
360p:
[H264] FIN OK 4350 huella=0x984A4460415D1B1C tiempo_ms=118832
[H264] HEAP antes=0 después=0 canarios=1
Host: 4350 fotogramas; huella=984a4460415d1b1c

1080p:
[H264] FIN OK 4637 huella=0xEE33F1F16B0A0C7F tiempo_ms=1081162
[H264] HEAP antes=0 después=0 canarios=1
Host: 4637 fotogramas; huella=ee33f1f16b0a0c7f
```

El tiempo es el medido por el invitado durante el diagnóstico. No equivale a un
benchmark de hardware físico. El heap puede conservar arenas ampliadas libres;
volver a cero bytes en uso no significa devolver todas esas arenas al PMM.

Artefactos locales que produjeron estos resultados:

| Prueba | ISO | SHA-256 |
|---|---|---|
| 360p | `build/h264/taek-h264-20260926-060008-H7tUr16r.iso` | `6be871a8cbd53b6037c6fd61db9c397669741eaee120ab6f08d615e346cf0458` |
| 1080p | `build/h264/taek-h264-20260926-061033-zWyUgrW9.iso` | `477c8430a57c03fe91c4b4132f859602c93245164b1f8c4db2c9187569acfa58` |

Registros completos locales: `build/h264-pruebas/qemu360-serial.log`,
`qemu1080-arenas-serial.log`, `validacion-final1080.log`, `huella360.log`,
`huella1080.log`, `fuzz.log` y `fuzz-corto.log` en el mismo directorio.
Captura: `build/h264-pruebas/h264-1080-en-kernel.png`.

## Errores encontrados durante el desarrollo

1. Orden incorrecto de filas/columnas en la transformada DC Hadamard: los primeros
   cuadros negros pasaban, pero el contenido posterior difería. Se corrigió contra
   la ecuación normativa y se repitieron las comparaciones completas.
2. Escala y signo de un término de la transformada 8×8: provocaban diferencias de
   luminancia en High Profile. Se corrigieron antes de validar 1080p completo.
3. Una partición B que no utilizaba una lista se trataba como vecino inexistente.
   Su referencia debe ser -1 y el vecino debe considerarse disponible. La corrección
   resolvió las primeras diferencias de predicción entre cuadros.
4. Faltaban operaciones de marcado adaptativo de referencias. La primera prueba
   corta de 360p se detenía en la muestra 48; se implementaron las operaciones MMCO.
5. La expansión previa del heap sólo reconocía páginas físicas ascendentes y se
   detenía en regiones UEFI pequeñas. 1080p fallaba antes del primer fotograma.
   Se admitieron ambas direcciones y se incorporaron los fragmentos como arenas
   libres, con búsqueda acotada. Luego QEMU completó los dos videos.
6. `video` ya era un alias del diagnóstico de GPU. Se eligió `h264` como comando
   del reproductor para conservar esa función existente.

## Uso y confirmación en hardware físico

En una ISO con módulos MP4, los comandos son:

```text
h264
h264 360p
h264 1080p
h264 prueba 360p
h264 prueba 1080p
```

ESC cancela la reproducción. Los módulos Limine se identifican mediante
`module_cmdline: h264:360p` y `module_cmdline: h264:1080p`.

Las ISO de diagnóstico indicadas arriba arrancan automáticamente la prueba
correspondiente. La generación se hizo en `/tmp` nativo de WSL, con `sync` antes
de copiar a un nombre nuevo en `build/`; no se formatearon discos en drvfs ni se
reescribieron imágenes abiertas por QEMU.

La validación del usuario satisface el requisito del proyecto para confirmar el
hito físico. El estatus experimental describe la cobertura parcial del estándar,
no pone en duda esa confirmación en el equipo probado.
