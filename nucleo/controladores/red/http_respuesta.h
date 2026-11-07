#ifndef TAEK_HTTP_RESPUESTA_H
#define TAEK_HTTP_RESPUESTA_H
#include <stddef.h>
#include <stdint.h>
struct http_respuesta {size_t cuerpo,fin;int estado,chunked;};
/* 1 completa, 0 incompleta, -1 mal formada. EOF autentico permite
 * delimitar cuerpos sin Content-Length; nunca completa chunks truncados. */
int http_respuesta_validar(const char *,size_t,int,struct http_respuesta *);
int http_respuesta_desempaquetar(char *,size_t *,struct http_respuesta *);
#endif
