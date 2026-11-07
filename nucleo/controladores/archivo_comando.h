#ifndef TAEK_ARCHIVO_COMANDO_H
#define TAEK_ARCHIVO_COMANDO_H
#include <stddef.h>
#include <stdint.h>
/* Parser compartido por la terminal y sus pruebas. Nunca trunca una ruta. */
static inline const char *archivo_espacios(const char *p){while(*p==' ' || *p=='\t')p++;return p;}
static inline int archivo_argumento(const char **cursor,char *out,size_t cap){
    const char *p=archivo_espacios(*cursor);char cierre=0;size_t n=0;
    if(*p=='"' || *p=='<'){cierre=*p=='"'?'"':'>';p++;}
    while(*p && (cierre?*p!=cierre:*p!=' ' && *p!='\t')){
        if(n+1>=cap || (unsigned char)*p<32)return -1;out[n++]=*p++;
    }
    if(!n || (cierre && *p!=cierre))return -1;if(cierre)p++;out[n]=0;*cursor=p;return 0;
}
static inline int archivo_parsear(const char *arg,int eliminar,char *origen,size_t no,char *destino,size_t nd){
    if(!arg)return -1;const char *p=archivo_espacios(arg);
    if(eliminar){const char frase[]="que sea ";unsigned i=0;while(frase[i] && p[i] && (p[i]==frase[i] || (p[i]>='A' && p[i]<='Z' && p[i]+32==frase[i])))i++;
        if(!frase[i])p=archivo_espacios(p+i);}
    if(archivo_argumento(&p,origen,no))return -1;
    p=archivo_espacios(p);
    if(!eliminar){
        if(*p=='a' || *p=='A')p++;
        else if((p[0]=='t' || p[0]=='T') && (p[1]=='o' || p[1]=='O'))p+=2;
        else return -1;
        if(*p!=' ' && *p!='\t' && *p!='<' && *p!='"')return -1;
        if(archivo_argumento(&p,destino,nd))return -1;
    }
    return *archivo_espacios(p)?-1:0;
}
static inline int archivo_indice(const char *nombre,int *indice){
    uint32_t n=0;if(!*nombre)return 0;
    for(const char *p=nombre;*p;p++)if(*p<'0' || *p>'9')return 0;
    for(const char *p=nombre;*p;p++){unsigned dig=(unsigned)(*p-'0');if(n>((uint32_t)INT32_MAX-dig)/10)return -1;n=n*10+dig;}
    if(!n)return -1;*indice=(int)n;return 1;
}
static inline int archivo_ruta(const char *base,const char *nombre,char *out,size_t cap){
    size_t n=0;if(nombre[0]!='/' && nombre[0]!='\\'){
        if(base)while(*base){if(n+1>=cap)return -1;out[n++]=*base++;}
        if(!n || (out[n-1]!='/' && out[n-1]!='\\')){if(n+1>=cap)return -1;out[n++]='/';}
    }
    while(*nombre){if(n+1>=cap)return -1;out[n++]=*nombre++;}if(!cap)return -1;out[n]=0;return 0;
}
/* 0 local path, 1 explicit disk path, -1 malformed disk path. */
static inline int archivo_disco(const char *s,uint8_t *unidad,unsigned *particion,const char **ruta){
    if(!s || s[0]!='u' || s[1]!='s' || s[2]!='b' || s[3]!=':')return 0;
    const char *p=s+4;unsigned values[2]={0};
    for(unsigned i=0;i<2;i++){
        if(*p<'0' || *p>'9')return -1;
        while(*p>='0' && *p<='9'){unsigned d=(unsigned)(*p++-'0');if(values[i]>(255u-d)/10)return -1;values[i]=values[i]*10+d;}
        if(*p++!=':')return -1;
    }
    if(!values[1] || (*p!='/' && *p!='\\'))return -1;
    *unidad=(uint8_t)values[0];*particion=values[1];*ruta=p;return 1;
}
#endif
