// Solo diagnostico (TAEK_MEDIA_TRAZA=1): se incluye a la fuerza y saca por el registro los mensajes de error que la version de
// produccion de Media Driver compila fuera. No cambia la logica del driver. Sin argumentos: el formato podria pedir %f.
#ifndef TAEK_TRAZA_H
#define TAEK_TRAZA_H
#include "mos_util_debug.h"
extern "C" void taek_traza(const char *archivo, int linea, const char *mensaje);
#undef MOS_ASSERTMESSAGE
#define MOS_ASSERTMESSAGE(_c, _s, _m, ...) taek_traza(__FILE__, __LINE__, _m)
#endif
