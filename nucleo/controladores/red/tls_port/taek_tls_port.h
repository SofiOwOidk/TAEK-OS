#ifndef TAEK_TLS_PORT_H
#define TAEK_TLS_PORT_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
void *taek_tls_calloc(size_t,size_t);
void taek_tls_free(void *);
int taek_tls_snprintf(char *,size_t,const char *,...);
int64_t taek_tls_time(int64_t *);
int taek_tls_random(void *,unsigned char *,size_t);
#endif
