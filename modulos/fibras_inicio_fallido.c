#include "../nucleo/controladores/driver_abi.h"
/* Init parcial adversario: detener dice cero con una pila suspendida. */
static const struct taek_kernel_api *k;
static uint64_t handle;
static unsigned stops,terminado;
static void worker(void *ctx){(void)ctx;k->fibra_bloquear();terminado=1;}
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_FIBRAS_BYTES)return -1;
    k=api;
    if(k->fibra_crear(worker,0,&handle) || k->fibra_ejecutar(1)!=1)return -2;
    return -99;
}
static int comando(const char *arg){(void)arg;k->imprimir_linea("[FIBRAS_FAIL] ERROR: comando en cuarentena.");return -1;}
static int detener(void){
    if(!k || !handle)return 0;
    if(++stops==1)return 0;
    if(k->fibra_despertar(handle) || k->fibra_ejecutar(1)!=1 || !terminado || k->fibra_liberar(handle))return -1;
    handle=0;k->imprimir_linea("[FIBRAS_FAIL] Parada real tras init fallido.");return 0;
}
__attribute__((used,section(".taek_driver"))) const struct taek_driver taek_driver={
    TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,"fibras-inicio-fallido",iniciar,comando,detener};
