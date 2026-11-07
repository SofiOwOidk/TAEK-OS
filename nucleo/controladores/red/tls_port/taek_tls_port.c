#include "taek_tls_port.h"
#include "../../../base/memoria.h"
#include "../../../base/tiempo.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"
#include "psa/crypto_extra.h"
#include <time.h>

void *taek_tls_calloc(size_t n,size_t s){if(s && n>SIZE_MAX/s)return 0;return asignar_memoria_cero(n*s);}
void taek_tls_free(void *p){liberar_memoria(p);}
int64_t mbedtls_ms_time(void){return (int64_t)tiempo_obtener_milisegundos();}
static void caracter(char *out,size_t cap,size_t *total,char c){if(*total+1<cap)out[*total]=c;(*total)++;}
int taek_tls_snprintf(char *out,size_t cap,const char *fmt,...){
    if(!fmt || (!out && cap))return -1;va_list args;va_start(args,fmt);size_t total=0;int error=0;
    while(*fmt){
        if(*fmt!='%'){caracter(out,cap,&total,*fmt++);continue;}fmt++;
        if(*fmt=='%'){caracter(out,cap,&total,*fmt++);continue;}
        int izquierda=0,cero=0,ancho=0,precision=-1,largo=0;
        while(*fmt=='-' || *fmt=='0'){if(*fmt=='-')izquierda=1;else cero=1;fmt++;}
        if(*fmt=='*'){ancho=va_arg(args,int);fmt++;if(ancho<0){if(ancho==INT32_MIN){error=1;break;}ancho=-ancho;izquierda=1;}}
        else while(*fmt>='0' && *fmt<='9'){if(ancho>1024){error=1;break;}ancho=ancho*10+*fmt++-'0';}
        if(error || ancho>16384){error=1;break;}
        if(*fmt=='.'){fmt++;precision=0;if(*fmt=='*'){precision=va_arg(args,int);fmt++;}else while(*fmt>='0' && *fmt<='9'){if(precision>1024){error=1;break;}precision=precision*10+*fmt++-'0';}}
        if(error || precision>16384){error=1;break;}
        if(*fmt=='l'){largo=1;fmt++;if(*fmt=='l'){largo=2;fmt++;}}else if(*fmt=='z'){largo=3;fmt++;}
        char tipo=*fmt;if(!tipo){error=1;break;}fmt++;
        char buf[80];const char *texto=buf;size_t len=0;int negativo=0;
        if(tipo=='s'){texto=va_arg(args,const char *);if(!texto)texto="(null)";while(texto[len] && (precision<0 || len<(size_t)precision))len++;}
        else if(tipo=='c'){buf[len++]=(char)va_arg(args,int);}
        else if(tipo=='d' || tipo=='i' || tipo=='u' || tipo=='x' || tipo=='X'){
            uint64_t valor;
            if(tipo=='d' || tipo=='i'){int64_t v=largo==3?(int64_t)va_arg(args,ptrdiff_t):largo==2?va_arg(args,long long):largo==1?va_arg(args,long):va_arg(args,int);
                negativo=v<0;valor=negativo?0-(uint64_t)v:(uint64_t)v;}
            else valor=largo==3?(uint64_t)va_arg(args,size_t):largo==2?va_arg(args,unsigned long long):largo==1?va_arg(args,unsigned long):va_arg(args,unsigned);
            unsigned base=(tipo=='x' || tipo=='X')?16:10;const char *digits=tipo=='X'?"0123456789ABCDEF":"0123456789abcdef";
            do{buf[len++]=digits[valor%base];valor/=base;}while(valor);
            for(size_t i=0;i<len/2;i++){char c=buf[i];buf[i]=buf[len-1-i];buf[len-1-i]=c;}
        }else {error=1;break;}
        size_t pad=ancho>(int)(len+negativo)?(size_t)ancho-len-negativo:0;
        if(!izquierda && !cero)while(pad){caracter(out,cap,&total,' ');pad--;}
        if(negativo)caracter(out,cap,&total,'-');
        if(!izquierda && cero)while(pad){caracter(out,cap,&total,'0');pad--;}
        for(size_t i=0;i<len;i++)caracter(out,cap,&total,texto[i]);
        if(izquierda)while(pad){caracter(out,cap,&total,' ');pad--;}
    }
    va_end(args);if(cap)out[total<cap?total:cap-1]=0;return error || total>INT32_MAX?-1:(int)total;
}

int taek_tls_random(void *context,unsigned char *out,size_t n){
    (void)context;if(!out && n)return -1;uint32_t a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    if(!(c&(1u<<30)))return -1;
    static uint64_t previo;static int hay_previo;
    for(size_t i=0;i<n;){uint64_t valor=0;unsigned char ok=0;
        for(unsigned intento=0;intento<128;intento++){__asm__ volatile("rdrand %0; setc %1":"=r"(valor),"=qm"(ok));if(ok)break;__asm__ volatile("pause");}
        if(!ok || !valor || valor==UINT64_MAX || (hay_previo && valor==previo)){mbedtls_platform_zeroize(out,n);return -1;}
        previo=valor;hay_previo=1;size_t cuantos=n-i<8?n-i:8;memcpy(out+i,&valor,cuantos);i+=cuantos;
    }return 0;
}
psa_status_t mbedtls_psa_external_get_random(mbedtls_psa_external_random_context_t *ctx,uint8_t *out,size_t n,size_t *hechos){
    (void)ctx;*hechos=0;if(taek_tls_random(0,out,n))return PSA_ERROR_INSUFFICIENT_ENTROPY;*hechos=n;return PSA_SUCCESS;
}
static int bisiesto(int y){return y%4==0 && (y%100!=0 || y%400==0);}
static int dias_mes(int y,int m){static const int dias[]={31,28,31,30,31,30,31,31,30,31,30,31};return dias[m-1]+(m==2 && bisiesto(y));}
static uint8_t rtc(uint8_t reg){uint8_t v;__asm__ volatile("outb %0,$0x70"::"a"((uint8_t)(reg|0x80)));__asm__ volatile("inb $0x71,%0":"=a"(v));return v;}
static int decimal(uint8_t n,int bin){return bin?n:(n&15)+10*(n>>4);}
int64_t taek_tls_time(int64_t *salida){
    uint64_t flags;__asm__ volatile("pushfq; popq %0; cli":"=r"(flags)::"memory");
    uint8_t anterior;__asm__ volatile("inb $0x70,%0":"=a"(anterior));
    uint8_t uno[7],dos[7];int igual=0;
    for(unsigned intento=0;intento<64;intento++){
        if(rtc(0x0a)&0x80)continue;
        uno[0]=rtc(0);uno[1]=rtc(2);uno[2]=rtc(4);uno[3]=rtc(7);uno[4]=rtc(8);uno[5]=rtc(9);uno[6]=rtc(0x0b);
        if(rtc(0x0a)&0x80)continue;
        dos[0]=rtc(0);dos[1]=rtc(2);dos[2]=rtc(4);dos[3]=rtc(7);dos[4]=rtc(8);dos[5]=rtc(9);dos[6]=rtc(0x0b);
        if(!memcmp(uno,dos,sizeof(uno)) && !(rtc(0x0a)&0x80)){igual=1;break;}
    }
    __asm__ volatile("outb %0,$0x70"::"a"(anterior));__asm__ volatile("pushq %0; popfq"::"r"(flags):"memory","cc");
    int64_t value=-1;
    if(igual){int bin=uno[6]&4,s=decimal(uno[0],bin),min=decimal(uno[1],bin),h=decimal(uno[2]&0x7f,bin),dia=decimal(uno[3],bin),mes=decimal(uno[4],bin),y=2000+decimal(uno[5],bin);
        if(!(uno[6]&2)){if(h<1 || h>12)h=99;else h=h%12+((uno[2]&128)?12:0);}
        if(y>=2026 && y<=2099 && mes>=1 && mes<=12 && dia>=1 && dia<=dias_mes(y,mes) && h<24 && min<60 && s<60){
            int64_t dias=0;for(int i=1970;i<y;i++)dias+=365+bisiesto(i);for(int i=1;i<mes;i++)dias+=dias_mes(y,i);dias+=dia-1;value=dias*86400+h*3600+min*60+s;
        }
    }if(salida)*salida=value;return value;
}
struct tm *mbedtls_platform_gmtime_r(const int64_t *t,struct tm *out){
    if(!t || !out || *t<0 || *t>INT64_C(4102444799))return 0;
    memset(out,0,sizeof(*out));int64_t dias=*t/86400;int resto=(int)(*t%86400);out->tm_sec=resto%60;out->tm_min=(resto/60)%60;out->tm_hour=resto/3600;out->tm_wday=(int)((dias+4)%7);
    int y=1970;while(dias>=365+bisiesto(y)){dias-=365+bisiesto(y);y++;}out->tm_yday=(int)dias;out->tm_year=y-1900;
    int m=1;while(dias>=dias_mes(y,m)){dias-=dias_mes(y,m);m++;}out->tm_mon=m-1;out->tm_mday=(int)dias+1;return out;
}
