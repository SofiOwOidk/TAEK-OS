#ifndef TAEK_WEB_URL_H
#define TAEK_WEB_URL_H
#include <stddef.h>
#include <stdint.h>
#define WEB_URL_MAX 1024
int web_url_parsear(const char *,int *,char *,size_t,uint16_t *,char *,size_t);
int web_url_resolver(const char *,const char *,char *,size_t);
#endif
