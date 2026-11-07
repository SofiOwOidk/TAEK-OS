#include "../nucleo/controladores/driver_abi.h"
static const struct taek_kernel_api *kernel;
static unsigned char *disco;
static int impedir_descarga;
static int registrado;
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static int leer(uint64_t off,void *buf,uint32_t n){if(!disco || off>4096 || n>4096-off)return -1;for(unsigned i=0;i<n;i++)((unsigned char *)buf)[i]=disco[off+i];return 0;}
static int escribir(uint64_t off,const void *buf,uint32_t n){if(!disco || off>4096 || n>4096-off)return -1;for(unsigned i=0;i<n;i++)disco[off+i]=((const unsigned char *)buf)[i];return 0;}
static const struct taek_driver_ops ops={sizeof(ops),TAEK_DRIVER_ABI,4096,leer,escribir,0};
static int iniciar(const struct taek_kernel_api *api){
    if(api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_BASE_BYTES)return -1;kernel=api;
    disco=kernel->asignar(4096);if(!disco)return -2;
    if(kernel->registrar("ram0",TAEK_DRIVER_BLOQUE,&ops)){kernel->liberar(disco);disco=0;return -3;}
    registrado=1;
    kernel->imprimir_linea("[RAM DRIVER] Dispositivo ram0 registrado, 4096 bytes.");
#if defined(TAEK_TEST_INIT_FAIL) || defined(TAEK_TEST_INIT_BUSY)
    return -99; /* No trabajo externo: el cargador debe retirar registro/heap. */
#else
    return 0;
#endif
}
static int comando(const char *arg){
    if(igual(arg,"busy")){impedir_descarga=1;return 0;}
    if(igual(arg,"idle")){impedir_descarga=0;return 0;}
    if(igual(arg,"probar")){
        unsigned char src[4]={0x54,0x41,0x45,0x4b},dst[4]={0};
        if(escribir(100,src,4) || leer(100,dst,4))return -1;
        for(unsigned i=0;i<4;i++)if(src[i]!=dst[i])return -1;
        kernel->imprimir_linea("[RAM DRIVER] PASS lectura/escritura mediante VTable.");return 0;
    }
    kernel->imprimir_linea("[RAM DRIVER] Comandos: probar | busy | idle");return 0;
}
static int detener(void){
#ifdef TAEK_TEST_INIT_BUSY
    static unsigned intentos;
    if(intentos++<2)return -77;
#endif
    if(impedir_descarga)return -1;
    if(!kernel)return 0;
    if(registrado){if(kernel->retirar("ram0"))return -2;registrado=0;}
    if(disco){if(kernel->liberar(disco))return -3;disco=0;}
    kernel->imprimir_linea("[RAM DRIVER] Detenido.");return 0;
}
#ifdef TAEK_TEST_BAD_ABI
#define VERSION_MODULO 999u
#else
#define VERSION_MODULO TAEK_DRIVER_ABI
#endif
const struct taek_driver taek_driver={TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),VERSION_MODULO,"ram-bloques",iniciar,comando,detener};
