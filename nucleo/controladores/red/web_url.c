#include "web_url.h"
#include "../../base/memoria.h"
static int prefijo(const char *p,const char *s){while(*s){char c=*p++;if(c>='A' && c<='Z')c+=32;if(c!=*s++)return 0;}return 1;}
static int poner(char *p,size_t cap,size_t *n,char c){if(*n+1>=cap)return -1;p[(*n)++]=c;return 0;}
static int texto(char *p,size_t cap,size_t *n,const char *s){while(*s)if(poner(p,cap,n,*s++))return -1;return 0;}
int web_url_parsear(const char *url,int *https,char *host,size_t nh,uint16_t *port,char *ruta,size_t nr){
    if(!url || !https || !host || nh<2 || !port || !ruta || nr<2)return -1;
    while(*url==' ' || *url=='\t')url++;*https=1;
    if(prefijo(url,"https://"))url+=8;else if(prefijo(url,"http://")){url+=7;*https=0;}
    size_t n=0,label=0;char previo=0;
    while(*url && *url!='/' && *url!='?' && *url!='#' && *url!=':'){
        char c=*url++;if(n>=253 || n+1>=nh)return -1;
        if(c=='.'){if(!label || previo=='-')return -1;label=0;}
        else {if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-') || (!label && c=='-') || ++label>63)return -1;}
        host[n++]=c;previo=c;
    }if(!n || !label || previo=='-')return -1;host[n]=0;*port=*https?443:80;
    if(*url==':'){url++;uint32_t value=0;unsigned digs=0;while(*url>='0' && *url<='9'){unsigned d=(unsigned)(*url++-'0');if(value>(65535-d)/10)return -1;value=value*10+d;digs++;}
        if(!digs || !value || (*url && *url!='/' && *url!='?' && *url!='#'))return -1;*port=(uint16_t)value;}
    n=0;if(*url!='/')ruta[n++]='/';
    while(*url && *url!='#'){unsigned char c=(unsigned char)*url++;if(c<=32 || c==127 || n+1>=nr)return -1;ruta[n++]=(char)c;}
    while(*url){if((unsigned char)*url<32 || (unsigned char)*url==127)return -1;url++;}
    ruta[n]=0;return 0;
}
static int normalizar(const char *path,char *out,size_t cap){
    size_t n=1,stack[128];unsigned depth=0;out[0]='/';int trailing=0;
    while(*path && *path!='?'){
        if(*path=='/'){trailing=1;path++;continue;}
        const char *end=path;while(*end && *end!='/' && *end!='?')end++;size_t len=(size_t)(end-path);trailing=0;
        if(len==1 && path[0]=='.')trailing=1;
        else if(len==2 && path[0]=='.' && path[1]=='.'){if(depth)n=stack[--depth];trailing=1;}
        else {if(depth==128 || n+len+2>cap)return -1;stack[depth++]=n;if(n>1)out[n++]='/';memcpy(out+n,path,len);n+=len;}
        path=end;
    }
    if(trailing && n>1 && poner(out,cap,&n,'/'))return -1;
    if(texto(out,cap,&n,path))return -1;out[n]=0;return 0;
}
int web_url_resolver(const char *base,const char *rel,char *out,size_t cap){
    if(!base || !rel || !out || cap<2)return -1;
    char input[WEB_URL_MAX],host[254],path[WEB_URL_MAX],merged[WEB_URL_MAX],clean[WEB_URL_MAX];int https;uint16_t port;size_t n=0;
    int absolute=prefijo(rel,"https://") || prefijo(rel,"http://");
    if(absolute){if(texto(input,sizeof(input),&n,rel))return -1;input[n]=0;}
    else if(rel[0]=='/' && rel[1]=='/'){
        if(web_url_parsear(base,&https,host,sizeof(host),&port,path,sizeof(path)))return -1;
        if(texto(input,sizeof(input),&n,https?"https:":"http:") || texto(input,sizeof(input),&n,rel))return -1;input[n]=0;
    }else {
        if(web_url_parsear(base,&https,host,sizeof(host),&port,path,sizeof(path)))return -1;
        /* Esquemas ajenos y caracteres de control no son enlaces navegables. */
        for(const char *p=rel;*p && *p!='/' && *p!='?' && *p!='#';p++)if(*p==':')return -1;
        size_t used=0,base_len=0;while(path[base_len] && path[base_len]!='?')base_len++;
        if(!*rel || *rel=='#'){if(texto(merged,sizeof(merged),&used,path))return -1;}
        else {
            if(*rel!='/'){
                size_t prefix=base_len;
                if(*rel!='?'){while(prefix && path[prefix-1]!='/')prefix--;}
                for(size_t i=0;i<prefix;i++)if(poner(merged,sizeof(merged),&used,path[i]))return -1;
            }
            while(*rel && *rel!='#')if(poner(merged,sizeof(merged),&used,*rel++))return -1;
        }merged[used]=0;
        if(normalizar(merged,clean,sizeof(clean)))return -1;
        if(texto(input,sizeof(input),&n,https?"https://":"http://") || texto(input,sizeof(input),&n,host))return -1;
        if(port!=(https?443:80)){char digits[6];unsigned count=0,value=port;do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
            if(poner(input,sizeof(input),&n,':'))return -1;while(count)if(poner(input,sizeof(input),&n,digits[--count]))return -1;}
        if(texto(input,sizeof(input),&n,clean))return -1;input[n]=0;
    }
    if(web_url_parsear(input,&https,host,sizeof(host),&port,path,sizeof(path)))return -1;
    n=0;if(texto(out,cap,&n,https?"https://":"http://") || texto(out,cap,&n,host))return -1;
    if(port!=(https?443:80)){char digits[6];unsigned count=0,value=port;do{digits[count++]=(char)('0'+value%10);value/=10;}while(value);
        if(poner(out,cap,&n,':'))return -1;while(count)if(poner(out,cap,&n,digits[--count]))return -1;}
    if(texto(out,cap,&n,path))return -1;out[n]=0;return 0;
}
