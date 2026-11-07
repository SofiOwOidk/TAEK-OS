#include "driver_abi.h"
/* USB HID 1.11 B.2: only the standard three boot mouse bytes are decoded.
 * Extra bytes are ignored; wheel and vendor report protocols need descriptors. */
static const struct taek_kernel_api *k;
static struct taek_hid_mouse_reporte report;
static struct {uint64_t reportes;int64_t dx,dy;uint32_t botones,errores;} estado;
static int ocupado;
static int adquirido,registrado;
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void signo(int64_t n){if(n<0){k->imprimir("-");k->imprimir_dec((uint64_t)(-(n+1))+1);}else k->imprimir_dec((uint64_t)n);}
static int sondear(uint32_t command,uint64_t value){
    (void)value;if(command!=TAEK_CONTROL_SONDEAR)return -1;
    for(unsigned i=0;i<64;i++){
        int r=k->hid_mouse_leer(&report);
        if(!r)return 0;
        if(r== -2){estado.botones=0;return 0;}
        if(r<0){estado.errores++;return r;}
        if(report.longitud<3 || report.longitud>8 || !report.dispositivo){estado.errores++;continue;}
        int32_t x=(int8_t)report.datos[1],y=(int8_t)report.datos[2];uint32_t b=report.datos[0]&7u;
        /* Saturating diagnostics avoid signed overflow over indefinite uptime. */
        if((x>0 && estado.dx>INT64_MAX-x) || (x<0 && estado.dx<INT64_MIN-x) ||
           (y>0 && estado.dy>INT64_MAX-y) || (y<0 && estado.dy<INT64_MIN-y)){estado.errores++;continue;}
        if(k->puntero_publicar(x,y,b,report.dispositivo)){estado.errores++;continue;}
        estado.reportes++;estado.dx+=x;estado.dy+=y;estado.botones=b;
    }
    return 0;
}
static int leer(uint64_t off,void *out,uint32_t n){
    if(!out || off>sizeof(estado) || n>sizeof(estado)-off)return -1;
    const uint8_t *s=(const uint8_t *)&estado+off;uint8_t *d=out;for(uint32_t i=0;i<n;i++)d[i]=s[i];return 0;
}
static const struct taek_driver_ops ops={sizeof(ops),TAEK_DRIVER_ABI,sizeof(estado),leer,0,sondear};
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_HID_BYTES)return -1;k=api;
    int devices=k->hid_mouse_tomar();if(devices<0)return -2;
    adquirido=1;
#ifdef TAEK_MOUSE_TEST_INIT_FAIL
    return -99; /* Loader must release acquired HID ownership on failed init. */
#endif
    if(k->registrar("mouse0",TAEK_DRIVER_PERIFERICO,&ops)){k->hid_mouse_liberar();adquirido=0;return -3;}
    registrado=1;
    k->imprimir("[MOUSE.CHAD] Transporte Boot Mouse USB adquirido. Presente: ");k->imprimir_dec((unsigned)devices);k->imprimir_linea("");return 0;
}
static int comando(const char *p){
    if(igual(p,"busy")){ocupado=1;return 0;}
    if(igual(p,"idle")){ocupado=0;return 0;}
    if(!igual(p,"estado"))return -1;
    k->imprimir("[MOUSE.CHAD] reportes=");k->imprimir_dec(estado.reportes);k->imprimir(" dx=");signo(estado.dx);k->imprimir(" dy=");signo(estado.dy);
    k->imprimir(" botones=");k->imprimir_dec(estado.botones);k->imprimir(" errores=");k->imprimir_dec(estado.errores);k->imprimir_linea("");return 0;
}
static int detener(void){
    if(ocupado)return -1;
    if(!k)return 0;
    if(adquirido){if(k->hid_mouse_liberar())return -2;adquirido=0;}
    if(registrado){if(k->retirar("mouse0"))return -3;registrado=0;}
    k->imprimir_linea("[MOUSE.CHAD] Transporte y mouse0 retirados.");return 0;
}
const struct taek_driver taek_driver={TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"mouse-usb-chad",iniciar,comando,detener};
