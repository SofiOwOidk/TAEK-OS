#include "decodificador.h"
#include "etapas_sse2.h"
#include "invariante_smp.h"
#define RECON_MAX_MB 8192
#define RECON_MAX_FILAS 256
typedef struct {int32_t coef[24][16];int modo16;} datos_mb;
struct h264_reconstruccion {
    datos_mb *datos;
    unsigned capacidad, primero, fin, fallo;
    unsigned avance[RECON_MAX_FILAS];
    uint8_t vista[RECON_MAX_FILAS];
    h264_decodificador *privados[4];
    h264_decodificador *dueno;
    uint64_t cpu[4], espera[4], regiones[4];
};
static uint64_t reloj(void){unsigned a,d;__asm__ volatile("lfence;rdtsc":"=a"(a),"=d"(d)::"memory");return ((uint64_t)d<<32)|a;}
void h264_cancelar_trabajadores(h264_decodificador *d) {
    if(d && d->reconstruccion)__atomic_store_n(&d->reconstruccion->fallo,1,__ATOMIC_RELEASE);
}
int h264_configurar_trabajadores(h264_decodificador *d,h264_lote_fn f,void *u,unsigned n,int (*servicio)(void *)) {
    if(!d || n<1 || n>4)return 0;
    d->ejecutar_lote=f;d->usuario_lote=u;d->trabajadores=n;d->servicio_coordinador=servicio;return 1;
}
void h264_reconstruccion_liberar(h264_decodificador *d) {
    struct h264_reconstruccion *r=d->reconstruccion;if(!r)return;
    for(unsigned i=0;i<4;i++)if(r->privados[i])d->servicios.liberar(d->servicios.usuario,r->privados[i]);
    if(r->datos)d->servicios.liberar(d->servicios.usuario,r->datos);
    d->servicios.liberar(d->servicios.usuario,r);d->reconstruccion=NULL;
}
int h264_reconstruccion_preparar(h264_decodificador *d,unsigned n) {
    if(n>RECON_MAX_MB || d->s->alto_mb>RECON_MAX_FILAS){d->fallo=H264_LIMITE_EXCEDIDO;d->error="Presupuesto de reconstrucción";return 0;}
    if(!d->reconstruccion) {
        d->reconstruccion=d->servicios.asignar(d->servicios.usuario,sizeof(*d->reconstruccion));
        if(!d->reconstruccion)return 0;
        h264_cero(d->reconstruccion,sizeof(*d->reconstruccion));
    }
    struct h264_reconstruccion *r=d->reconstruccion;
    if(n>r->capacidad) {
        datos_mb *p=d->servicios.asignar(d->servicios.usuario,n*sizeof(*p));if(!p)return 0;
        if(r->datos)d->servicios.liberar(d->servicios.usuario,r->datos);
        r->datos=p;r->capacidad=n;
    }
    for(unsigned i=0;i<d->trabajadores;i++)if(!r->privados[i]) {
        r->privados[i]=d->servicios.asignar(d->servicios.usuario,sizeof(*d));
        if(!r->privados[i])return 0;
        h264_cero(r->privados[i],sizeof(*d));
    }
    d->coef_publicar=r->datos[d->mb_actual<n?d->mb_actual:0].coef;
    d->telemetria.presupuesto_reconstruccion_bytes=(unsigned)(sizeof(*r)+r->capacidad*sizeof(datos_mb)+d->trabajadores*sizeof(*d));
    return 1;
}
static void fila(void *u,unsigned region,unsigned ejecutor) {
    struct h264_reconstruccion *r=u;h264_decodificador *d=r->privados[ejecutor];
    unsigned ancho=d->s->ancho_mb,y=r->primero/ancho+region;
    unsigned ini=y*ancho<r->primero?r->primero:y*ancho;
    unsigned fin=(y+1)*ancho>r->fin?r->fin:(y+1)*ancho;
    uint64_t inicio=reloj();
    r->regiones[ejecutor]++;
    if(region<RECON_MAX_FILAS)r->vista[region]++;
    for(unsigned idx=ini;idx<fin;idx++) {
        unsigned x=idx%ancho;
        if(!ejecutor && (x&7)==0 && r->dueno->servicio_coordinador &&
           r->dueno->servicio_coordinador(r->dueno->usuario_lote))
            __atomic_store_n(&r->fallo,1,__ATOMIC_RELEASE);
        if(d->actual->mb[idx].tipo && y>r->primero/ancho) {
            unsigned requerido=x+2>ancho?ancho:x+2;
            uint64_t t=reloj();
            while(__atomic_load_n(&r->avance[y-1],__ATOMIC_ACQUIRE)<requerido &&
                  !__atomic_load_n(&r->fallo,__ATOMIC_ACQUIRE))__asm__ volatile("pause");
            r->espera[ejecutor]+=reloj()-t;
#if H264_TELEMETRIA_DETALLADA
            if(r->dueno->traza)r->dueno->traza(r->dueno->usuario_traza,ejecutor,5,d->actual->tiempo,t,reloj(),((uint64_t)y<<32)|requerido);
#endif
        }
        if(__atomic_load_n(&r->fallo,__ATOMIC_ACQUIRE))break;
        h264_mb privado=d->actual->mb[idx];d->mb_actual=idx;d->mb_privado=&privado;
        datos_mb *b=&r->datos[idx];
        int ok=(privado.tipo || h264_compensar(d)) && h264_aplicar_residuo(d,b->coef,b->modo16);
        d->mb_privado=NULL;
        if(!ok)__atomic_store_n(&r->fallo,1,__ATOMIC_RELEASE);
        __atomic_store_n(&r->avance[y],x+1,__ATOMIC_RELEASE);
    }
    /* Desbloquear dependientes al cancelar; ninguno sigue reconstruyendo. */
    __atomic_store_n(&r->avance[y],ancho,__ATOMIC_RELEASE);
    r->cpu[ejecutor]+=reloj()-inicio;
#if H264_TELEMETRIA_DETALLADA
    if(r->dueno->traza)r->dueno->traza(r->dueno->usuario_traza,ejecutor,4,d->actual->tiempo,inicio,reloj(),y);
#endif
}
int h264_reconstruccion_ejecutar(h264_decodificador *d,unsigned primero,unsigned fin) {
    struct h264_reconstruccion *r=d->reconstruccion;
    r->primero=primero;r->fin=fin;r->dueno=d;r->fallo=0;
    /* El parser acaba antes de publicar: SPS/PPS/MB sintácticos y referencias
     * permanecen inmutables hasta el retorno síncrono del backend. */
    for(unsigned i=0;i<4;i++){r->cpu[i]=r->espera[i]=0;r->regiones[i]=0;}
    for(unsigned y=0;y<d->s->alto_mb;y++){r->avance[y]=0;r->vista[y]=0;}
    for(unsigned i=0;i<d->trabajadores;i++) {
        h264_decodificador *p=r->privados[i];p->s=d->s;p->p=d->p;p->sl=d->sl;
        p->actual=d->actual;p->slice_id=d->slice_id;p->inter_sse2=d->inter_sse2;
        p->etapas_sse2=d->etapas_sse2;
        h264_cero(&p->telemetria,sizeof(p->telemetria));
    }
    /* modo16 se guarda al terminar cada MB en el parser. */
    unsigned filas=(fin-1)/d->s->ancho_mb-primero/d->s->ancho_mb+1;
    uint64_t inicio=reloj();
    int ok=d->ejecutar_lote(d->usuario_lote,fila,r,filas);
    uint64_t pared=reloj()-inicio;
    d->telemetria.ciclos_reconstruccion_pared+=pared;
    d->telemetria.lotes_reconstruccion++;
    unsigned unicas=0,duplicadas=0,faltantes=0;
    unicas=h264_regiones_contar(r->vista,filas,&duplicadas,&faltantes);
    uint64_t cpu_total=0;
    int incoherente=0;
    for(unsigned i=0;i<4;i++) {
        uint64_t computo = r->espera[i]<=r->cpu[i] ? r->cpu[i]-r->espera[i] : 0;
        d->telemetria.regiones_ejecutor[i]+=r->regiones[i];
        d->telemetria.computo_ejecutor[i]+=computo;
        d->telemetria.espera_ejecutor[i]+=r->espera[i];
        d->telemetria.pared_ejecutor[i]+=r->cpu[i];
        cpu_total+=r->cpu[i];
        if(r->espera[i]>r->cpu[i])incoherente=1;
    }
    for(unsigned i=0;i<d->trabajadores;i++) {
        h264_telemetria *t=&r->privados[i]->telemetria;
        uint64_t computo = r->espera[i]<=r->cpu[i] ? r->cpu[i]-r->espera[i] : 0;
        d->telemetria.ciclos_reconstruccion_cpu+=computo;
        d->telemetria.ciclos_dependencias_cpu+=r->espera[i];
        d->telemetria.ciclos_transformadas+=t->ciclos_transformadas;
        d->telemetria.ciclos_suma_residuo+=t->ciclos_suma_residuo;
        for(unsigned k=0;k<H264_INTER_ETAPAS;k++) {
            d->telemetria.ciclos_inter_etapa[k]+=t->ciclos_inter_etapa[k];
            d->telemetria.bloques_inter_etapa[k]+=t->bloques_inter_etapa[k];
            d->telemetria.pixeles_inter_etapa[k]+=t->pixeles_inter_etapa[k];
        }
    }
    d->telemetria.regiones_reconstruccion+=filas;
    d->telemetria.regiones_ok_reconstruccion+=unicas;
    d->telemetria.regiones_duplicadas+=duplicadas;
    d->telemetria.regiones_faltantes+=faltantes;
    if(ok && !r->fallo) {
        if(unicas!=filas || duplicadas || faltantes)incoherente=1;
        /* Invariante de reloj: el cómputo sumado de los trabajadores no puede
         * exceder participantes*pared, salvo desincronía entre relojes por CPU.
         * La relación se conserva aunque el lote se descarte por otra causa. */
        uint64_t ratio_miles=0;
        if(h264_smp_invariante(cpu_total,pared,d->trabajadores,&ratio_miles))incoherente=1;
        if(ratio_miles>d->telemetria.ratio_worst_miles)d->telemetria.ratio_worst_miles=ratio_miles;
        if(incoherente)d->telemetria.lotes_incoherentes++;
    }
    if(!ok || r->fallo){d->error="Lote de reconstrucción cancelado o inválido";return 0;}
    for(unsigned i=primero;i<fin;i++)d->actual->mb[i].reconstruidos=65535;
    return 1;
}
/* Transformada y suma medibles por separado; ninguna saturación anticipada. */
void h264_aplicar_transformada(h264_decodificador *d,int32_t *c,uint8_t *dst,unsigned paso,unsigned n) {
    if(!d->inter_sse2 || !(d->etapas_sse2&2)) {
        if(n==4)h264_transformar4(c,dst,paso);else h264_transformar8(c,dst,paso);
        return;
    }
    int32_t r[64];
#if H264_PERFIL_INTER
    uint64_t t=reloj();
#endif
    h264_transformada_sse2(c,r,n);
#if H264_PERFIL_INTER
    d->telemetria.ciclos_transformadas+=reloj()-t;t=reloj();
#endif
    h264_sumar_sse2(r,dst,paso,n);
#if H264_PERFIL_INTER
    d->telemetria.ciclos_suma_residuo+=reloj()-t;
#endif
}
/* Invocado sólo por el parser: publicar también la decisión intra 16x16. */
void h264_reconstruccion_modo(h264_decodificador *d) {
    if(d->separar)d->reconstruccion->datos[d->mb_actual].modo16=d->modo16_publicar;
}
