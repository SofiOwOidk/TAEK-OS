#include "../nucleo/controladores/driver_abi.h"
#include "../nucleo/compatibilidad/linux_i915/upstream_params.h"
static const struct taek_kernel_api *kernel;
static unsigned activo,rondas;
void *memset(void *dest,int valor,size_t n){volatile unsigned char *p=dest;for(size_t i=0;i<n;i++)p[i]=(unsigned char)valor;return dest;}
void *memcpy(void *dest,const void *src,size_t n){volatile unsigned char *p=dest;const volatile unsigned char *s=src;for(size_t i=0;i<n;i++)p[i]=s[i];return dest;}
static void *reservar(unsigned long n){return kernel->asignar(n);}
static int liberar(void *p){return kernel->liberar(p);}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static void campo(const char *nombre,unsigned long value){kernel->imprimir(nombre);kernel->imprimir_dec(value);}
static int iniciar(const struct taek_kernel_api *api){
    if(!api || api->version!=TAEK_DRIVER_ABI || api->bytes<TAEK_KERNEL_API_BASE_BYTES || !api->asignar || !api->liberar)return -1;
    kernel=api;int r=taek_upstream_heap_configurar(reservar,liberar);if(r)return r;
    activo=1;rondas=0;kernel->imprimir_linea("[PARAMS RAM] i915 params/kstrdup originales conectados al heap nativo BSP.");return 0;
}
static int comando(const char *arg){
    if(!activo)return -1;
    if(igual(arg,"contrato")){
        struct taek_upstream_heap_contrato r={0};int code=taek_upstream_heap_probar(&r);
        if(r.checks!=18 || r.fallos || !r.limpio || r.errores_esperados!=4 || r.fallos_esperados!=5)code=-1;
        kernel->imprimir("[HEAP RAM] RESULTADO=");kernel->imprimir(code?"FAIL":"OK");
        campo(" checks=",r.checks);campo(" mask=",r.fallos);campo(" limpio=",r.limpio);
        campo(" errores_esperados=",r.errores_esperados);campo(" fallos_esperados=",r.fallos_esperados);kernel->imprimir_linea("");
        if(!code){code=taek_upstream_heap_configurar(reservar,liberar);rondas=0;}return code;
    }
    if(igual(arg,"probar")){
        struct taek_upstream_params_resultado r={0};int code=taek_upstream_params_probar(&r);
        struct taek_upstream_heap_estado st;taek_upstream_heap_estado(&st);rondas++;
        if(r.checks!=25 || r.fallos || !r.limpio || r.privados!=6 || r.oom_probados!=6 ||
            st.activos || st.bytes || st.errores || st.fallos!=6*rondas || st.pico!=6)code=-1;
        kernel->imprimir("[PARAMS RAM] RESULTADO=");kernel->imprimir(code?"FAIL":"OK");
        campo(" checks=",r.checks);campo(" mask=",r.fallos);campo(" limpio=",r.limpio);
        campo(" privados=",r.privados);campo(" oom=",r.oom_probados);campo(" ronda=",rondas);kernel->imprimir_linea("");
        kernel->imprimir("[PARAMS HEAP]");campo(" activos=",st.activos);campo(" bytes=",st.bytes);campo(" errores=",st.errores);
        campo(" fallos=",st.fallos);campo(" pico=",st.pico);kernel->imprimir_linea("");
        kernel->imprimir_linea("[PARAMS RAM] Copia/liberacion reales; probe, MMIO, GPU, 3D y video pendientes.");return code;
    }
    kernel->imprimir_linea("[PARAMS RAM] Comandos: contrato | probar");return 0;
}
static int detener(void){
    struct taek_upstream_heap_estado st;taek_upstream_heap_estado(&st);
    if(st.activos || st.bytes){if(kernel)kernel->imprimir_linea("[PARAMS RAM] Heap ocupado; descarga rechazada.");return -16;}
    activo=0;if(kernel)kernel->imprimir_linea("[PARAMS RAM] Detenido, sin asignaciones ni callbacks externos.");kernel=0;return 0;
}
const struct taek_driver taek_driver={TAEK_DRIVER_MAGIA,sizeof(struct taek_driver),TAEK_DRIVER_ABI,
    "upstream-params",iniciar,comando,detener};
