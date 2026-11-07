#include "tls.h"
#include "red.h"
#include "stream.h"
#include "http_respuesta.h"
#include "tls_port/taek_tls_port.h"
#include "../../base/memoria.h"
#include "../../base/tiempo.h"
#include "../consola.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

extern const unsigned char taek_tls_ca_bundle[];
extern const size_t taek_tls_ca_bundle_size;
struct cliente_tls {mbedtls_ssl_context ssl;mbedtls_ssl_config conf;mbedtls_x509_crt ca;uint64_t fin;int error;};
static uint8_t ocupado;
static int restante(struct cliente_tls *c){uint64_t ahora=tiempo_obtener_milisegundos();return ahora>=c->fin?0:(int)(c->fin-ahora);}
static int limite(struct cliente_tls *c){if(consola_sondear_cancelacion())c->error=TLS_ERR_CANCELADO;else if(!restante(c))c->error=TLS_ERR_TIMEOUT;return c->error;}
static int enviar(void *ctx,const unsigned char *p,size_t n){
    struct cliente_tls *c=ctx;if(limite(c))return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if(n>1400)n=1400;int r=red_tcp_flujo_enviar(p,(uint32_t)n);
    if(r<0){c->error=TLS_ERR_TX;return MBEDTLS_ERR_SSL_INTERNAL_ERROR;}return r?MBEDTLS_ERR_SSL_WANT_WRITE:(int)n;
}
static int recibir(void *ctx,unsigned char *p,size_t n){
    struct cliente_tls *c=ctx;if(limite(c))return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if(n>16384)n=16384;int r=red_tcp_flujo_recibir(p,(uint32_t)n);
    return r<0?MBEDTLS_ERR_SSL_CONN_EOF:r?r:MBEDTLS_ERR_SSL_WANT_READ;
}
static int esperar(struct cliente_tls *c,int code){
    if(code!=MBEDTLS_ERR_SSL_WANT_READ && code!=MBEDTLS_ERR_SSL_WANT_WRITE)return 0;
    if(limite(c))return 0;red_pila_sondeo();esperar_milisegundos(1);return 1;
}
static int parametros(const char *host,const char *ruta){
    size_t n=0;if(!host || !*host || !ruta || ruta[0]!='/')return -1;
    for(const char *p=host;*p;p++){unsigned char ch=(unsigned char)*p;if(++n>253 || !((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') || ch=='.' || ch=='-'))return -1;}
    n=0;for(const char *p=ruta;*p;p++)if(++n>2048 || (unsigned char)*p<=32 || (unsigned char)*p==127 || *p=='#')return -1;
    return 0;
}
int red_https_obtener(const char *host,const char *ruta,uint16_t puerto,char *salida,uint32_t cap,int timeout_ms){
    if(!ruta || !*ruta)ruta="/";if(!salida || cap<64 || cap>INT32_MAX || parametros(host,ruta))return TLS_ERR_PARAM;
    salida[0]=0;if(timeout_ms<=0)timeout_ms=15000;if(timeout_ms>120000)timeout_ms=120000;if(!puerto)puerto=443;
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return TLS_ERR_OCUPADO;
    int ret=TLS_ERR_RELOJ,pausado=0,tcp=0;struct cliente_tls *c=0;uint32_t usados=0;
    if(taek_tls_time(0)<0){consola_imprimir_linea("[HTTPS] Reloj RTC invalido; no se puede comprobar la vigencia del certificado.");goto fin;}
    unsigned char prueba;if(taek_tls_random(0,&prueba,1)){ret=TLS_ERR_ENTROPIA;consola_imprimir_linea("[HTTPS] Entropia hardware no disponible.");goto fin;}
    const struct red_config *cfg=red_obtener_config();if(!cfg || !cfg->configurada){ret=TLS_ERR_RED_NOCONFIG;goto fin;}
    c=asignar_memoria_cero(sizeof(*c));if(!c){ret=TLS_ERR_BUFER;goto fin;}
    c->fin=tiempo_obtener_milisegundos()+(uint32_t)timeout_ms;
    mbedtls_ssl_init(&c->ssl);mbedtls_ssl_config_init(&c->conf);mbedtls_x509_crt_init(&c->ca);
    red_stream_pausar(1);pausado=1;uint8_t ip[4];
    if(red_texto_a_ip(host,ip) && red_resolver_nombre(host,ip,restante(c))){ret=TLS_ERR_DNS;goto fin;}
    if(limite(c)){ret=c->error;goto fin;}
    if(red_tcp_flujo_abrir_timeout(ip,puerto,restante(c))){ret=TLS_ERR_TCP;goto fin;}tcp=1;
    if(psa_crypto_init()!=PSA_SUCCESS){ret=TLS_ERR_ENTROPIA;goto fin;}
    ret=mbedtls_x509_crt_parse(&c->ca,taek_tls_ca_bundle,taek_tls_ca_bundle_size);
    if(ret<0){ret=TLS_ERR_AUTH_FALLIDA;goto fin;}
    if(mbedtls_ssl_config_defaults(&c->conf,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT)){ret=TLS_ERR_PROTOCOLO;goto fin;}
    mbedtls_ssl_conf_min_tls_version(&c->conf,MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&c->conf,MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_authmode(&c->conf,MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf,&c->ca,0);
    mbedtls_ssl_conf_rng(&c->conf,taek_tls_random,0);
    static const char *alpn[]={"http/1.1",0};
    if(mbedtls_ssl_conf_alpn_protocols(&c->conf,alpn) || mbedtls_ssl_setup(&c->ssl,&c->conf) || mbedtls_ssl_set_hostname(&c->ssl,host)){ret=TLS_ERR_PROTOCOLO;goto fin;}
    mbedtls_ssl_set_bio(&c->ssl,c,enviar,recibir,0);
    for(;;){int code=mbedtls_ssl_handshake(&c->ssl);if(!code)break;if(esperar(c,code))continue;
        ret=c->error?c->error:TLS_ERR_AUTH_FALLIDA;consola_imprimir_linea("[HTTPS] Handshake rechazado; no se entrega contenido sin autenticar.");
        consola_imprimir("[HTTPS] Diagnostico TLS: ");consola_imprimir_hex((uint32_t)(-code));
        consola_imprimir("; verificacion X509: ");consola_imprimir_hex(mbedtls_ssl_get_verify_result(&c->ssl));consola_imprimir_linea("");goto fin;}
    if(mbedtls_ssl_get_verify_result(&c->ssl)){ret=TLS_ERR_AUTH_FALLIDA;goto fin;}
    consola_imprimir_linea("[HTTPS] TLS 1.3: certificado, nombre, vigencia y firma verificados.");
    char peticion[3072];int len=taek_tls_snprintf(peticion,sizeof(peticion),
        "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: TAEK-OS/1.0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",ruta,host,(unsigned)puerto);
    if(len<0 || (size_t)len>=sizeof(peticion)){ret=TLS_ERR_PARAM;goto fin;}
    for(int off=0;off<len;){int code=mbedtls_ssl_write(&c->ssl,(unsigned char *)peticion+off,(size_t)(len-off));
        if(code>0){off+=code;continue;}if(esperar(c,code))continue;ret=c->error?c->error:TLS_ERR_TX;goto fin;}
    for(;;){
        struct http_respuesta framing;int completo=http_respuesta_validar(salida,usados,0,&framing);
        if(completo<0){ret=TLS_ERR_PROTOCOLO;goto fin;}if(completo==1){ret=(int)usados;break;}
        if(usados+1>=cap){ret=TLS_ERR_BUFER;goto fin;}
        int code=mbedtls_ssl_read(&c->ssl,(unsigned char *)salida+usados,cap-usados-1);
        if(code>0){usados+=(uint32_t)code;salida[usados]=0;continue;}
        if(esperar(c,code))continue;
        if(code==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || !code || code==MBEDTLS_ERR_SSL_CONN_EOF){
            int autentico=code==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
            completo=http_respuesta_validar(salida,usados,autentico,&framing);
            ret=completo==1?(int)usados:TLS_ERR_RX;break;
        }
        ret=c->error?c->error:TLS_ERR_AUTH_FALLIDA;goto fin;
    }
fin:
    if(ret<0){memset(salida,0,usados+1);}if(c){mbedtls_ssl_free(&c->ssl);mbedtls_ssl_config_free(&c->conf);mbedtls_x509_crt_free(&c->ca);liberar_memoria(c);}
    if(tcp)red_tcp_flujo_cerrar();if(pausado)red_stream_pausar(0);
    mbedtls_psa_crypto_free();__atomic_clear(&ocupado,__ATOMIC_RELEASE);return ret;
}
