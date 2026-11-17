/* Frontera de Iris (Mesa, M27) con el shim i915. Usa los mismos servicios que Media Driver (media/taek_media.h): memoria en la
 * ventana de usuario, registro, reloj, espera, DRM, ioctl y mmap. Solo cruza un nombre propio: taek_iris_entrada. */
#ifndef TAEK_IRIS_H
#define TAEK_IRIS_H
#include "../media/taek_media.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { TAEK_IRIS_RT = 1, TAEK_IRIS_INIT, TAEK_IRIS_CLEAR, TAEK_IRIS_TRIANGULO, TAEK_IRIS_TEXTURA, TAEK_IRIS_PANTALLA, TAEK_IRIS_VIDEO,
       TAEK_IRIS_OP /* H195: texto es una taek_video_peticion del reproductor (PRESENTAR_IRIS o IRIS_FIN) */ };
/* H193: el plano de pantalla escanea un objeto GEM de Iris (early_principal.c). mostrar guarda y restaurar repone y verifica. */
int taek_media_plano_info(unsigned *ancho, unsigned *alto);
int taek_media_plano_mostrar(unsigned handle, unsigned paso, unsigned *ggtt);
int taek_media_plano_restaurar(void);
/* H201: mostrar sin esperar el vblank, y esperar (solo si hace falta) a que el hardware tome lo armado */
int taek_media_plano_mostrar_async(unsigned handle, unsigned paso, unsigned *ggtt);
int taek_media_plano_esperar(void);
/* H192: copia un bloque de pixeles de 32 bits a la pantalla real (media_frontera.c). x/y negativos: desde el borde. */
int taek_media_pantalla(const void *p, unsigned w, unsigned h, unsigned paso, int x, int y);
int taek_iris_entrada(int orden, char *texto, size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
