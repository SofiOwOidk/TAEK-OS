#include "http_respuesta.h"
#include "../../base/memoria.h"
static int igual(const char *p,size_t n,const char *s){size_t i=0;for(;i<n && s[i];i++){char c=p[i];if(c>='A' && c<='Z')c+=32;if(c!=s[i])return 0;}return i==n && !s[i];}
static size_t linea(const char *p,size_t inicio,size_t n){for(size_t i=inicio;i+1<n;i++)if(p[i]=='\r' && p[i+1]=='\n')return i;return SIZE_MAX;}
static int numero(const char *p,size_t n,unsigned base,uint64_t *out){
    if(!n)return -1;uint64_t v=0;for(size_t i=0;i<n;i++){unsigned c=(unsigned char)p[i];unsigned d=c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:c>='A' && c<='F'?c-'A'+10:99;
        if(d>=base || v>(UINT64_MAX-d)/base)return -1;v=v*base+d;}*out=v;return 0;
}
static int chunks(const char *p,size_t n,size_t comienzo,size_t *fin,char *dest,size_t *largo){
    size_t off=comienzo,escritos=0;
    for(;;){size_t end=linea(p,off,n);if(end==SIZE_MAX)return 0;size_t hex=off;while(hex<end && p[hex]!=';')hex++;
        uint64_t tam;if(numero(p+off,hex-off,16,&tam))return -1;
        for(size_t i=hex;i<end;i++)if((unsigned char)p[i]<32 || (unsigned char)p[i]==127)return -1;
        off=end+2;
        if(!tam){
            for(;;){end=linea(p,off,n);if(end==SIZE_MAX)return 0;if(end==off){*fin=end+2;if(largo)*largo=escritos;return 1;}
                int colon=0;for(size_t i=off;i<end;i++){if(p[i]==':')colon=1;if((unsigned char)p[i]<32 || (unsigned char)p[i]==127)return -1;}
                if(!colon)return -1;off=end+2;}
        }
        if(tam>SIZE_MAX-off || (size_t)tam>n-off)return 0;
        size_t siguiente=off+(size_t)tam;if(n-siguiente<2)return 0;if(p[siguiente]!='\r' || p[siguiente+1]!='\n')return -1;
        if(dest)memmove(dest+escritos,p+off,(size_t)tam);escritos+=(size_t)tam;off=siguiente+2;
    }
}
int http_respuesta_validar(const char *p,size_t n,int eof,struct http_respuesta *r){
    if(!p || !r)return -1;memset(r,0,sizeof(*r));size_t end=linea(p,0,n);if(end==SIZE_MAX)return 0;
    if(end<12 || memcmp(p,"HTTP/1.",7) || (p[7]!='0' && p[7]!='1') || p[8]!=' ' || p[9]<'1' || p[9]>'5' || p[10]<'0' || p[10]>'9' || p[11]<'0' || p[11]>'9')return -1;
    r->estado=(p[9]-'0')*100+(p[10]-'0')*10+p[11]-'0';if(r->estado<200)return -1;
    size_t off=end+2;uint64_t length=0;int tiene_len=0,tiene_te=0;
    for(;;){end=linea(p,off,n);if(end==SIZE_MAX)return 0;if(end==off){off=end+2;break;}
        size_t colon=off;while(colon<end && p[colon]!=':')colon++;if(colon==off || colon==end)return -1;
        for(size_t i=off;i<colon;i++)if((unsigned char)p[i]<=32 || (unsigned char)p[i]>=127)return -1;
        size_t val=colon+1;while(val<end && (p[val]==' ' || p[val]=='\t'))val++;size_t ve=end;while(ve>val && (p[ve-1]==' ' || p[ve-1]=='\t'))ve--;
        for(size_t i=val;i<ve;i++)if(((unsigned char)p[i]<32 && p[i]!='\t') || (unsigned char)p[i]==127)return -1;
        if(igual(p+off,colon-off,"content-length")){uint64_t v;if(numero(p+val,ve-val,10,&v) || (tiene_len && v!=length))return -1;length=v;tiene_len=1;}
        if(igual(p+off,colon-off,"transfer-encoding")){if(tiene_te || !igual(p+val,ve-val,"chunked"))return -1;tiene_te=1;}
        off=end+2;if(off>32768)return -1;
    }r->cuerpo=off;r->chunked=tiene_te;if(tiene_te && tiene_len)return -1;
    if(r->estado==204 || r->estado==304){r->fin=off;return n==off?1:-1;}
    if(tiene_te){int code=chunks(p,n,off,&r->fin,0,0);if(code==1 && r->fin!=n)return -1;return eof && !code?-1:code;}
    if(tiene_len){if(length>SIZE_MAX-off)return -1;r->fin=off+(size_t)length;if(n>r->fin)return -1;return n==r->fin?1:eof?-1:0;}
    if(eof){r->fin=n;return 1;}return 0;
}
int http_respuesta_desempaquetar(char *p,size_t *n,struct http_respuesta *r){
    int code=http_respuesta_validar(p,*n,1,r);if(code!=1)return -1;
    if(r->chunked){size_t fin,decoded=0;code=chunks(p,*n,r->cuerpo,&fin,p+r->cuerpo,&decoded);if(code!=1)return -1;*n=r->cuerpo+decoded;r->fin=*n;}
    p[*n]=0;return 0;
}
