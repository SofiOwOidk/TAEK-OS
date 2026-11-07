#ifndef RED_REMOTO_H
#define RED_REMOTO_H
#include <stdint.h>
void red_remoto_ejecutor(void (*ejecutor)(const char *));
void red_remoto_iniciar(const uint8_t ip[4], uint16_t puerto);
void red_remoto_detener(void);
int red_remoto_activo(void);
void red_remoto_capturar(char c);
void red_remoto_atender(void);
#endif
