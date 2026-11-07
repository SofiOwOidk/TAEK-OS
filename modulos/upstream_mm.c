#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_mm.h"
static const struct taek_kernel_api *kernel;
static unsigned avisos,activo,sucio,rondas;
void *memset(void *dest,int valor,size_t bytes){
    volatile unsigned char *p=dest;
    for(size_t i=0;i<bytes;i++)p[i]=(unsigned char)valor;
    return dest;
}
void __warn_printk(const char *formato,...){
    kernel->imprimir("[DRM_MM RAM WARN] ");kernel->imprimir_linea(formato);
}
void taek_linux_warn_site(const char *archivo,unsigned linea,unsigned flags){
    avisos++;kernel->imprimir("[DRM_MM RAM WARN SITE] ");kernel->imprimir(archivo);
    kernel->imprimir(" linea=");kernel->imprimir_dec(linea);
    kernel->imprimir(" flags=");kernel->imprimir_dec(flags);kernel->imprimir_linea("");
}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_BASE_BYTES)return -1;
    kernel=api;activo=1;sucio=0;rondas=avisos=0;
    kernel->imprimir_linea("[DRM_MM RAM] drm_mm/rbtree originales listos; rangos sinteticos, GPU pendiente.");
    return 0;
}
static int comando(const char *arg){
    if(!activo || sucio)return -1;
    if(!igual(arg,"probar")){
        kernel->imprimir_linea("[DRM_MM RAM] Comando: probar");return 0;
    }
    struct taek_upstream_mm_resultado r={0};unsigned antes=avisos;
    sucio=1;int code=taek_upstream_drm_mm_probar(&r);
    r.avisos=avisos-antes;
    if(r.limpio)sucio=0;
    if(r.checks!=21 || r.fallos || r.limpio!=1 || r.avisos!=1 || r.bajo || r.alto!=0xf8000)code=-1;
    rondas++;
    kernel->imprimir("[DRM_MM RAM] RESULTADO=");kernel->imprimir(code?"FAIL":"OK");
    kernel->imprimir(" checks=");kernel->imprimir_dec(r.checks);
    kernel->imprimir(" mask=");kernel->imprimir_dec(r.fallos);
    kernel->imprimir(" limpio=");kernel->imprimir_dec(r.limpio);
    kernel->imprimir(" avisos=");kernel->imprimir_dec(r.avisos);
    kernel->imprimir(" ronda=");kernel->imprimir_dec(rondas);kernel->imprimir_linea("");
    return code;
}
static int detener(void){
    if(sucio)return -1;
    if(kernel)kernel->imprimir_linea("[DRM_MM RAM] Detenido: gestores limpios, sin callbacks externos.");
    activo=0;kernel=0;return 0;
}
const struct taek_driver taek_driver={TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,
    "upstream-mm",iniciar,comando,detener};
