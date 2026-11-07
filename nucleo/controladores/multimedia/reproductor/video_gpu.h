#ifndef TAEK_VIDEO_GPU_H
#define TAEK_VIDEO_GPU_H
#include <stddef.h>
#include "compatibilidad/linux_i915/media/taek_video_abi.h"

/* Decide si este equipo puede decodificar por GPU y, la primera vez, carga el modulo y levanta el driver.
 * 0 si esta lista. Si no, *motivo dice por que sigue el video por software. */
int video_gpu_preparar(const void *modulo, size_t bytes, const char **motivo);
/* Una orden al cliente del modulo. Negativo si fallo; p->motivo trae el texto. */
int video_gpu_orden(struct taek_video_peticion *p, uint32_t orden);
/* Fallos de prueba pendientes para el siguiente video. */
void video_gpu_inyeccion(struct taek_video_peticion *p);
/* Atiende 'h264 motor ...' y 'h264 gpu ...'. 1 si el argumento era suyo. */
int video_gpu_comando(const char *argumento);
#endif
