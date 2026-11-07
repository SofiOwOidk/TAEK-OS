#include "remoto.h"
#include "red.h"
#include "../consola.h"
#include "../../base/memoria.h"
#include "../../base/tiempo.h"
#include "../../arquitectura/x86_64/serial.h"

static uint8_t destino[4];
static uint16_t puerto;
static int activo, capturando, pendiente;
static uint64_t proximo, ultimo_id;
static uint32_t longitud, perdidos;
static char salida[8192];
static void (*ejecutar)(const char *);

static uint32_t u32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void b32(uint8_t *p,uint32_t n) { for(int i=3;i>=0;--i){p[i]=n;n>>=8;} }
static void b64(uint8_t *p,uint64_t n) { for(int i=7;i>=0;--i){p[i]=n;n>>=8;} }
static int igual(const char *a,const char *b) { while(*a&&*a==*b){++a;++b;}return *a==*b; }
static int permitido(const char *cmd) {
    static const char *lista[]={"version","red","intel estado","h264 intel h0","huevo","iommu"};
    for(unsigned i=0;i<sizeof(lista)/sizeof(lista[0]);++i)if(igual(cmd,lista[i]))return 1;
    return 0;
}
void red_remoto_ejecutor(void (*fn)(const char *)) { ejecutar=fn; }
void red_remoto_iniciar(const uint8_t ip[4],uint16_t port) {
    memcpy(destino,ip,4);puerto=port;ultimo_id=0;pendiente=0;proximo=0;activo=1;
}
void red_remoto_detener(void) { activo=0;capturando=0;pendiente=0; }
int red_remoto_activo(void) { return activo; }
void red_remoto_capturar(char c) {
    if(!capturando)return;
    if(longitud<sizeof(salida))salida[longitud++]=c;else ++perdidos;
}
static int intercambio(uint32_t op,const void *data,uint32_t n,uint64_t id,uint8_t rsp[264]) {
    uint8_t header[24]={0},payload[240]={0};
    memcpy(header,"TAEKMSG1",8);b32(header+8,op);b32(header+12,n);b64(header+16,id);
    if(n)memcpy(payload,data,n);
    int r=red_tcp_intercambiar(destino,puerto,header,24,payload,240,rsp,264,750);
    if(!r&&(memcmp(rsp,"TAEKRSP1",8)||u32(rsp+12)>240))r=-4;
    return r;
}
static int entregar(void) {
    uint8_t rsp[264],datos[240],id[8];b64(id,ultimo_id);
    uint32_t partes=(longitud+235)/236;
    for(uint32_t i=0;i<partes;++i){
        uint32_t n=longitud-i*236;if(n>236)n=236;
        b32(datos,i);memcpy(datos+4,salida+i*236,n);
        if(intercambio(4,datos,n+4,ultimo_id,rsp)||u32(rsp+8)!=1||u32(rsp+12)||memcmp(rsp+16,id,8))return -1;
    }
    b32(datos,partes);b32(datos+4,perdidos);
    if(intercambio(5,datos,8,ultimo_id,rsp)||u32(rsp+8)!=1||u32(rsp+12)||memcmp(rsp+16,id,8))return -1;
    pendiente=0;
    serial_imprimir("[REMOTO_LAN] RESULTADO=1 ID=");serial_imprimir_dec(ultimo_id);serial_imprimir_linea("");
    return 0;
}
void red_remoto_atender(void) {
    if(!activo||!ejecutar)return;
    uint64_t ahora=tiempo_obtener_milisegundos();
    if(ahora<proximo)return;
    proximo=ahora+500;
    if(pendiente){if(entregar())proximo=tiempo_obtener_milisegundos()+3000;return;}
    uint8_t rsp[264],ack[264];
    if(intercambio(2,0,0,0,rsp)){proximo=tiempo_obtener_milisegundos()+3000;return;}
    uint32_t n=u32(rsp+12);
    if(u32(rsp+8)==2&&!n)return;
    if(u32(rsp+8)!=3||n<22||memcmp(rsp+24,"CMD ",4)||rsp[44]!=' ')return;
    uint64_t id=0;
    for(unsigned i=28;i<44;++i){
        uint8_t c=rsp[i];unsigned v;
        if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;else return;
        id=(id<<4)|v;
    }
    char cmd[220];uint32_t nc=n-21;
    memcpy(cmd,rsp+45,nc);cmd[nc]=0;
    for(uint32_t i=0;i<nc;++i)if((uint8_t)cmd[i]<32||(uint8_t)cmd[i]>126)return;
    uint64_t reply_id=0;for(unsigned i=16;i<24;++i)reply_id=(reply_id<<8)|rsp[i];
    if(intercambio(3,0,0,reply_id,ack)||u32(ack+8)!=4||memcmp(ack+16,rsp+16,8))return;
    // ID monotono: una retransmision no vuelve a ejecutar el ultimo comando.
    if(id<=ultimo_id)return;
    ultimo_id=id;longitud=0;perdidos=0;
    consola_imprimir("\n[REMOTO] ");consola_imprimir_linea(cmd);
    capturando=1;
    if(permitido(cmd))ejecutar(cmd);
    else consola_imprimir_linea("Comando remoto rechazado. Permitidos: version, red, intel estado, h264 intel h0, huevo, iommu.");
    capturando=0;pendiente=1;
    serial_imprimir("[REMOTO_LAN] EJECUTADO=1 ID=");serial_imprimir_dec(id);serial_imprimir_linea("");
    if(entregar())proximo=tiempo_obtener_milisegundos()+3000;
    consola_imprimir("sudo@taek-os:~# ");
}
