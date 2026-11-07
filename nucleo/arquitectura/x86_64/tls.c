#include "tls.h"
#include "base/memoria.h"
#include "base/trabajos.h"

#define TLS_IMAGEN_MAX 4096u
#define TLS_ALINEACION_MAX 4096u
#define TLS_MAGIA UINT64_C(0x5441454b544c5331)
extern const uint8_t __taek_tls_inicio[],__taek_tls_datos_fin[],__taek_tls_fin[];
extern const uint8_t __taek_tls_alineacion[];
struct tls_tcb {void *self,*asignacion;uint64_t magia;};
static uint8_t cpu_espacio[TRABAJOS_MAX_CPU][TLS_IMAGEN_MAX+TLS_ALINEACION_MAX+sizeof(struct tls_tcb)]
    __attribute__((aligned(64)));
static void *cpu_tp[TRABAJOS_MAX_CPU];

static int layout(size_t *bytes,size_t *datos,size_t *alineacion){
    uintptr_t start=(uintptr_t)__taek_tls_inicio,end=(uintptr_t)__taek_tls_fin;
    uintptr_t data=(uintptr_t)__taek_tls_datos_fin;
    size_t align=(uintptr_t)__taek_tls_alineacion;
    if(end<start || data<start || data>end || !align || (align&(align-1)) ||
       align>TLS_ALINEACION_MAX || end-start>TLS_IMAGEN_MAX)return -1;
    size_t total=(end-start+align-1)&~(align-1);
    if(total>TLS_IMAGEN_MAX)return -1;
    *bytes=total;*datos=data-start;*alineacion=align;return 0;
}
size_t taek_tls_imagen_bytes(void){size_t b,d,a;return layout(&b,&d,&a)?0:b;}
uintptr_t taek_tls_base_actual(void){
    uint32_t lo,hi;__asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0xc0000100));
    return ((uint64_t)hi<<32)|lo;
}
static void *preparar(void *raw,size_t bytes,size_t datos,size_t align,void *asignacion){
    uintptr_t base=((uintptr_t)raw+align-1)&~(uintptr_t)(align-1);
    uint8_t *image=(void *)base;
    for(size_t i=0;i<bytes;i++)image[i]=0;
    for(size_t i=0;i<datos;i++)image[i]=__taek_tls_inicio[i];
    struct tls_tcb *tp=(void *)(base+bytes);
    *tp=(struct tls_tcb){.self=tp,.asignacion=asignacion,.magia=TLS_MAGIA};
    return tp;
}
int taek_tls_cpu_iniciar(unsigned cpu){
    size_t bytes,datos,align;
    if(cpu>=TRABAJOS_MAX_CPU || layout(&bytes,&datos,&align))return -1;
    if(!cpu_tp[cpu])cpu_tp[cpu]=preparar(cpu_espacio[cpu],bytes,datos,align,NULL);
    uint64_t base=(uintptr_t)cpu_tp[cpu];
    __asm__ volatile("wrmsr"::"c"(0xc0000100),"a"((uint32_t)base),"d"((uint32_t)(base>>32)):"memory");
    return 0;
}
void *taek_tls_crear_con(void *(*asignar)(size_t)){
    if(!asignar)return NULL;
    size_t bytes,datos,align;if(layout(&bytes,&datos,&align))return NULL;
    void *raw=asignar(bytes+align+sizeof(struct tls_tcb));
    return raw?preparar(raw,bytes,datos,align,raw):NULL;
}
void *taek_tls_crear(void){return taek_tls_crear_con(asignar_memoria);}
int taek_tls_destruir_con(void *base,int (*liberar)(void *)){
    if(!base)return 0;
    if(!liberar)return -1;
    if((uintptr_t)base==taek_tls_base_actual())return -1;
    struct tls_tcb *tp=base;
    if(tp->self!=tp || tp->magia!=TLS_MAGIA || !tp->asignacion)return -1;
    void *raw=tp->asignacion;tp->magia=0;
    int r=liberar(raw);if(r)tp->magia=TLS_MAGIA;return r;
}
static int liberar_predeterminado(void *raw){liberar_memoria(raw);return 0;}
int taek_tls_destruir(void *base){return taek_tls_destruir_con(base,liberar_predeterminado);}
