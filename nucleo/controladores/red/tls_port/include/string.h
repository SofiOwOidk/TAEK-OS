#ifndef TAEK_TLS_STRING_H
#define TAEK_TLS_STRING_H
#include <stddef.h>
#include "../../../../base/memoria.h"
static inline size_t strlen(const char *p){size_t n=0;while(p[n])n++;return n;}
static inline int strcmp(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
static inline int strncmp(const char *a,const char *b,size_t n){while(n && *a && *a==*b){a++;b++;n--;}return n?(unsigned char)*a-(unsigned char)*b:0;}
static inline char *strchr(const char *p,int c){do{if((unsigned char)*p==(unsigned char)c)return (char *)p;}while(*p++);return 0;}
static inline char *strstr(const char *p,const char *s){size_t n=strlen(s);for(;*p;p++)if(!strncmp(p,s,n))return (char *)p;return !n?(char *)p:0;}
static inline void *memchr(const void *p,int c,size_t n){const unsigned char *b=p;for(size_t i=0;i<n;i++)if(b[i]==(unsigned char)c)return (void *)(b+i);return 0;}
#endif
