#ifndef RED_STREAM_H
#define RED_STREAM_H
#include <stdint.h>
void red_stream_iniciar(const uint8_t ip[4], uint16_t puerto);
void red_stream_detener(void);
int red_stream_activo(void);
int red_stream_conectado(void);
void red_stream_atender(void);
char red_stream_tecla(void);
void red_stream_pausar(int pausa);
void red_stream_arranque(const char *cmdline);
#endif
