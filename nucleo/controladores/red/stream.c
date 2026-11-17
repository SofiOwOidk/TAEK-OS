#include "stream.h"
#include "red.h"
#include "../../arquitectura/x86_64/serial.h"
#include "../../base/memoria.h"
#include "../../base/tiempo.h"
#include "archivos.h"
#include "../consola.h"
#include "wifi_rtw88.h"
#include "red_cerrojo.h"   /* H211: cerrojo de la red (Fase C) */

#define TRAMA 1024
#define CABECERA 48
static uint8_t destino[4], tx[TRAMA], rx[TRAMA], pendiente[TRAMA], teclas[512];
static uint16_t puerto;
static uint32_t recibidos, inicio_teclas, total_teclas, n_pendiente;
static uint64_t sesion, cursor, fin_pendiente, ack_teclas, proximo, plazo;
static int activo, conectado, saludo, esperando, ocupado, hay_pendiente;
static int pausado;
static int dhcp_automatico;
static uint64_t dhcp_proximo;
static uint64_t wifi_revision;   /* H166 */
static int wifi_revisiones, wifi_preferido;
static int conectando, fallos_conexion;   /* H189: conexion sin bloquear */
static uint64_t plazo_conexion;
static uint32_t espera_reintento(void){
    uint32_t ms=1000u<<(fallos_conexion<5?fallos_conexion:5);
    if(fallos_conexion<5)fallos_conexion++;
    return ms>30000?30000:ms;
}
static void cortar_intento(void){if(conectando){red_tcp_flujo_abandonar();conectando=0;}}
void red_stream_arranque(const char *cmdline){ RED_EXCLUSIVA();
    if(!cmdline)return;
    const char *p=cmdline;
    while(*p){
        while(*p==' ')p++;if(!*p)break;
        if(p[0]=='l' && p[1]=='a' && p[2]=='n' && p[3]=='='){
            char host[32],orden[64];unsigned n=0;uint32_t port=9153;
            p+=4;while(*p && *p!=' ' && *p!=':'){if(n+1>=sizeof(host))return;host[n++]=*p++;}host[n]=0;
            if(*p==':'){
                p++;port=0;unsigned dig=0;
                while(*p && *p!=' '){if(*p<'0'||*p>'9'||port>6553)return;port=port*10+(unsigned)(*p++-'0');dig++;}
                if(!dig || !port || port>65535)return;
            }
            uint8_t ip[4];if(red_texto_a_ip(host,ip))return;
            consola_imprimir_linea("[LAN AUTO] DHCP y stream de arranque...");
            /* H166: sin enlace por cable no se gastan 9 s en DHCP; el WiFi entra desde red_stream_atender. */
            red_enlace_actualizar();
            int r=red_obtener_info()->enlace_activo?red_dhcp(9000):-1;dhcp_proximo=tiempo_obtener_milisegundos()+10000;
            red_stream_iniciar(ip,(uint16_t)port);
            dhcp_automatico=1;
            wifi_revision=tiempo_obtener_milisegundos()+15000;   /* H166: que el USB termine de montarse */
            memcpy(orden,"servidor ",9);memcpy(orden+9,host,n+1);red_archivos_comando(orden);
            consola_imprimir_linea(r?"[LAN AUTO] DHCP pendiente; se reintentara con el cable conectado.":"[LAN AUTO] IP obtenida; stream conectara automaticamente.");return;
        }
        while(*p && *p!=' ')p++;
    }
}
void red_stream_pausar(int pausa){ RED_EXCLUSIVA();
    pausado=pausa;
    if(pausa){cortar_intento();red_tcp_flujo_cerrar();conectado=0;esperando=0;recibidos=0;}
    else {proximo=0;fallos_conexion=0;}
}
static uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t u64(const uint8_t *p) { uint64_t n=0;for(int i=0;i<8;++i)n=(n<<8)|p[i];return n; }
static void b32(uint8_t *p,uint32_t n) { for(int i=3;i>=0;--i){p[i]=n;n>>=8;} }
static void b64(uint8_t *p,uint64_t n) { for(int i=7;i>=0;--i){p[i]=n;n>>=8;} }
static void cabecera(uint8_t *p,uint32_t op,uint32_t n,uint64_t offset,uint64_t perdidos) {
    memset(p,0,TRAMA);memcpy(p,"TAEKSTR1",8);b32(p+8,op);b32(p+12,n);
    b64(p+16,sesion);b64(p+24,offset);b64(p+32,ack_teclas);b64(p+40,perdidos);
}
void red_stream_detener(void) { RED_EXCLUSIVA();
    cortar_intento();
    activo=0;conectado=0;total_teclas=0;hay_pendiente=0;dhcp_automatico=0;red_tcp_flujo_cerrar();
}
void red_stream_iniciar(const uint8_t ip[4],uint16_t port) { RED_EXCLUSIVA();
    red_stream_detener();memcpy(destino,ip,4);puerto=port;
    sesion=rdtsc();if(!sesion)sesion=1;cursor=serial_log_posicion();ack_teclas=0;
    inicio_teclas=0;proximo=0;fallos_conexion=0;activo=1;
}
int red_stream_activo(void) { return activo; }
int red_stream_conectado(void) { return conectado&&saludo; }
/* H150: para el panel de Configuracion. Solo lectura. */
void red_stream_estado(uint8_t ip[4],uint16_t *port,uint64_t *enviado){ RED_EXCLUSIVA();
    if(ip)memcpy(ip,destino,4);
    if(port)*port=puerto;
    if(enviado)*enviado=cursor;
}
static void reconectar(void) {
    red_tcp_flujo_cerrar();conectado=0;esperando=0;recibidos=0;
    proximo=tiempo_obtener_milisegundos()+1000;
}
static int respuesta(void) {
    if(memcmp(rx,"TAEKSTR1",8)||u64(rx+16)!=sesion||u32(rx+12)>256||u64(rx+40))return -1;
    if(!saludo) {
        if(u32(rx+8)!=4||u32(rx+12)||u64(rx+24)!=cursor||u64(rx+32)!=ack_teclas)return -1;
        saludo=1;return 0;
    }
    if(u32(rx+8)!=3||u64(rx+24)!=fin_pendiente)return -1;
    uint32_t n=u32(rx+12);uint64_t primero=u64(rx+32);
    if(primero>ack_teclas||primero+n<primero)return -1;
    for(uint32_t i=0;i<n;++i) {
        uint8_t c=rx[CABECERA+i];
        if(!(c>=32&&c<=126)&&!(c>=17&&c<=20)&&c!=3&&c!=8&&c!=9&&c!=10&&c!=13&&c!=27&&c!=127)return -1;
    }
    cursor=fin_pendiente;hay_pendiente=0;
    // ACK significa aceptado en la FIFO, no comando ejecutado. Duplicados no reinyectan teclas.
    if(primero+n>ack_teclas) {
        uint32_t omitir=(uint32_t)(ack_teclas-primero), nuevos=n-omitir;
        if(nuevos<=sizeof(teclas)-total_teclas) {
            for(uint32_t i=omitir;i<n;++i)teclas[(inicio_teclas+total_teclas++)%sizeof(teclas)]=rx[CABECERA+i];
            ack_teclas+=nuevos;
        }
    }
    return 0;
}
void red_stream_atender(void) { if (red_cede_el_sondeo()) return; RED_EXCLUSIVA();
    if(!activo||ocupado||pausado)return;
    ocupado=1;
    uint64_t ahora=tiempo_obtener_milisegundos();
    if(dhcp_automatico){
        const struct red_config *cfg=red_obtener_config();
        if(!cfg || !(cfg->ip[0]|cfg->ip[1]|cfg->ip[2]|cfg->ip[3])){
            if(ahora>=dhcp_proximo){
                /* H166: sin cable, la red de /wifi.conf (el USB ya esta montado cuando corre esto). */
                red_enlace_actualizar();
                if(!red_externa_activa()&&!red_obtener_info()->enlace_activo)wifi_red_automatica();
                else red_dhcp(2000);
                dhcp_proximo=tiempo_obtener_milisegundos()+10000;
            }
            goto fin;
        }
    }
    /* H166: /wifi.auto pide WiFi aunque haya cable (se revisa 3 veces, cada 10 s, mientras se monta el USB). */
    if(dhcp_automatico&&wifi_revisiones<3&&!red_externa_activa()&&ahora>=wifi_revision){
        wifi_revisiones++;wifi_revision=ahora+10000;
        if(wifi_preferida()){wifi_revisiones=3;wifi_preferido=1;}
    }
    /* H172: con /wifi.auto se insiste (un intento por minuto) hasta quedar en WiFi. */
    if(wifi_preferido&&!red_externa_activa())wifi_red_automatica();   /* si falla, el cable sigue sirviendo */
    /* H166: la pila esta en WiFi y el AP cerro la asociacion: reconectar (un intento por minuto). */
    if(red_externa_activa()&&!red_enlace_actualizar()){
        if(red_enlace_cable()){wifi_red_cable();proximo=0;}          /* con cable: volver a el */
        else if(wifi_red_automatica()==0)proximo=0;
        goto fin;
    }
    if(!conectado) {
        /* H189: sin bloquear. Se envia el SYN y se vuelve; las siguientes llamadas miran si llego el SYN-ACK.
         * Con el receptor caido los reintentos se espacian (1, 2, 4... hasta 30 s). */
        if(!conectando){
            red_pila_sondeo();                                   /* sin conexion la pila sigue atendiendo ARP/ICMP */
            if(ahora<proximo)goto fin;
            int r0=red_tcp_flujo_iniciar(destino,puerto);
            if(r0==1){proximo=ahora+200;goto fin;}               /* ARP pedido: mirar de nuevo en 200 ms */
            if(r0<0){proximo=ahora+espera_reintento();goto fin;}
            conectando=1;plazo_conexion=ahora+2000;goto fin;
        }
        int e=red_tcp_flujo_sondear();
        if(e==0){if(ahora>=plazo_conexion){red_tcp_flujo_abandonar();conectando=0;proximo=ahora+espera_reintento();}goto fin;}
        conectando=0;
        if(e<0){proximo=ahora+espera_reintento();goto fin;}
        fallos_conexion=0;
        conectado=1;saludo=0;esperando=0;recibidos=0;proximo=0;
    }
    int r=red_tcp_flujo_recibir(rx+recibidos,TRAMA-recibidos);
    if(r<0){reconectar();goto fin;}
    recibidos+=(uint32_t)r;
    if(recibidos==TRAMA) {
        if(!esperando||respuesta()){reconectar();goto fin;}
        recibidos=0;esperando=0;proximo=saludo&&!n_pendiente?ahora+20:0;
    }
    if(esperando) {if(ahora>=plazo)reconectar();goto fin;}
    if(ahora<proximo)goto fin;
    if(!saludo) cabecera(tx,1,0,cursor,0);
    else {
        if(!hay_pendiente) {
            uint64_t posicion=cursor,perdidos=0;
            uint8_t datos[TRAMA-CABECERA];
            n_pendiente=serial_leer_desde(&posicion,datos,sizeof(datos),&perdidos);
            fin_pendiente=posicion;
            cabecera(pendiente,2,n_pendiente,posicion-n_pendiente,perdidos);
            memcpy(pendiente+CABECERA,datos,n_pendiente);hay_pendiente=1;
        }
        memcpy(tx,pendiente,TRAMA);
        b64(tx+32,ack_teclas);
    }
    r=red_tcp_flujo_enviar(tx,TRAMA);
    if(r<0){reconectar();goto fin;}
    if(!r){esperando=1;plazo=tiempo_obtener_milisegundos()+2000;proximo=ahora+20;}
fin:
    ocupado=0;
}
/* Para quien escribe mas rapido de lo que el registro aguanta: espera a que lo escrito salga, con un plazo. */
void red_stream_drenar(void) { RED_EXCLUSIVA();
    if(!activo||pausado||!conectado)return;
    uint64_t plazo_drenar=tiempo_obtener_milisegundos()+3000;
    while(cursor!=serial_log_posicion()&&tiempo_obtener_milisegundos()<plazo_drenar){
        red_pila_sondeo();red_stream_atender();
    }
}
char red_stream_tecla(void) { if (!total_teclas) return 0; RED_EXCLUSIVA();
    if(!total_teclas)return 0;
    char c=(char)teclas[inicio_teclas];inicio_teclas=(inicio_teclas+1)%sizeof(teclas);--total_teclas;
    if(c=='\r')c='\n';
    if(c==127)c=8;
    return c;
}
