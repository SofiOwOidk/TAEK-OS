#include "cola_muestras.h"
static uint64_t reloj(void){unsigned a,d;__asm__ volatile("lfence;rdtsc":"=a"(a),"=d"(d)::"memory");return ((uint64_t)d<<32)|a;}
void cola_muestras_iniciar(cola_muestras *q,const mp4_contenedor *m,int pista,uint8_t *datos,size_t capacidad) {
    *q=(cola_muestras){.cursor=*m,.datos=datos,.capacidad=capacidad,.pista=pista};
}
int cola_muestras_recargar(cola_muestras *q,unsigned objetivo,int (*servicio)(void *),void *u) {
    if(q->error)return q->error;
    if(objetivo>COLA_MUESTRAS_MAX)objetivo=COLA_MUESTRAS_MAX;
    if(q->cantidad>=objetivo || q->eof)return 1;
    size_t usados=0;
    for(unsigned i=0;i<q->cantidad;i++) {
        muestra_lista m=q->muestras[q->cabeza+i];
        for(size_t j=0;j<m.bytes;j++)q->datos[usados+j]=q->datos[m.inicio+j];
        m.inicio=usados;q->muestras[i]=m;usados+=m.bytes;
    }
    q->cabeza=0;q->usados=usados;
    while(q->cantidad<objetivo && !q->eof) {
        if(servicio && servicio(u))return q->error=MP4_ERROR_LECTURA;
        mp4_contenedor siguiente=q->cursor;
        siguiente.solo_indice=1;
        const uint8_t *ignorado;size_t bytes;int64_t pts;uint32_t duracion=0;
        uint64_t ini=reloj();
        int r=q->pista==MP4_FUENTE_VIDEO?
            mp4_siguiente_video(&siguiente,&ignorado,&bytes,&pts,&duracion):
            mp4_siguiente_audio(&siguiente,&ignorado,&bytes,&pts);
        if(r>0 && q->observar_indice)q->observar_indice(q->usuario_observador,q->pista,
            q->pista==MP4_FUENTE_VIDEO?q->cursor.v_indice:q->cursor.a_indice,pts,ini,reloj());
        if(!r){q->eof=1;break;}
        if(r<0)return q->error=r;
        if(bytes>q->capacidad)return q->error=MP4_LIMITE_EXCEDIDO;
        if(bytes>q->capacidad-q->usados)break;
        uint64_t off=(q->pista==MP4_FUENTE_VIDEO?siguiente.v_offset_muestra:siguiente.a_offset_muestra)-bytes;
        if(q->cursor.leer_fuente) {
            int64_t n=q->cursor.leer_fuente(q->cursor.fuente_contexto,q->pista,off,q->datos+q->usados,bytes);
            if(n!=(int64_t)bytes){q->cursor.ultimo_error_lectura=n<0?n:MP4_ERROR_LECTURA;return q->error=MP4_ERROR_LECTURA;}
        } else for(size_t j=0;j<bytes;j++)q->datos[q->usados+j]=q->cursor.archivo[off+j];
        siguiente.solo_indice=0;q->cursor=siguiente;
        q->muestras[q->cantidad++]=(muestra_lista){q->usados,bytes,pts,duracion};
        q->usados+=bytes;q->bytes_leidos+=bytes;q->muestras_leidas++;
    }
    return 1;
}
int cola_muestras_tomar(cola_muestras *q,const uint8_t **datos,size_t *bytes,int64_t *pts,uint32_t *duracion) {
    if(q->error)return q->error;
    if(!q->cantidad){if(q->eof)return 0;q->faltantes++;return MP4_ERROR_LECTURA;}
    muestra_lista *m=&q->muestras[q->cabeza++];q->cantidad--;
    *datos=q->datos+m->inicio;*bytes=m->bytes;*pts=m->pts;if(duracion)*duracion=m->duracion;
    return 1;
}
