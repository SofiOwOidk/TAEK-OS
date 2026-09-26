#include "decodificador.h"

static h264_resultado fallo(h264_decodificador *d,h264_resultado codigo,const char *motivo) {
    d->fallo=codigo;d->error=motivo;return codigo;
}

h264_decodificador *h264_crear(const h264_servicios *s) {
    if (!s || !s->asignar || !s->liberar || !s->presentar) return NULL;
    h264_decodificador *d=s->asignar(s->usuario,sizeof(*d));
    if (!d) return NULL;
    h264_cero(d,sizeof(*d));d->servicios=*s;d->error="Sin error";d->max_larga=-1;
    return d;
}

void h264_destruir(h264_decodificador *d) {
    if (!d) return;
    h264_servicios s=d->servicios;
    for (int i=0;i<18;i++) {
        if (d->fotos[i].pixeles) s.liberar(s.usuario,d->fotos[i].pixeles);
        if (d->fotos[i].mb) s.liberar(s.usuario,d->fotos[i].mb);
    }
    if (d->rbsp) s.liberar(s.usuario,d->rbsp);
    s.liberar(s.usuario,d);
}

const char *h264_error(const h264_decodificador *d) { return d ? d->error : "Contexto nulo"; }

static int emitir(h264_decodificador *d,h264_foto *f) {
    h264_imagen im;
    unsigned w=f->w,h=f->h;
    im.y=f->pixeles+f->crop_y*2*w+f->crop_x*2;
    im.u=f->pixeles+(size_t)w*h+f->crop_y*(w/2)+f->crop_x;
    im.v=im.u+(size_t)w*h/4;
    im.ancho=f->ancho_visible;im.alto=f->alto_visible;im.paso_y=w;im.paso_c=w/2;
    im.orden=f->poc;im.marca_tiempo=f->tiempo;
    im.rango_completo=f->rango_completo;im.matriz_color=f->matriz_color;
    f->salida=0;
    return d->servicios.presentar(d->servicios.usuario,&im)==0;
}

static int vaciar(h264_decodificador *d,unsigned conservar) {
    for (;;) {
        h264_foto *menor=NULL;
        unsigned n=0;
        for (int i=0;i<18;i++) if (d->fotos[i].salida) {
            n++;
            if (!menor || d->fotos[i].poc<menor->poc) menor=&d->fotos[i];
        }
        if (n<=conservar) return 1;
        if (!emitir(d,menor)) return 0;
    }
}

static int terminar_foto(h264_decodificador *d) {
    h264_foto *f=d->actual;
    if (!f) return 1;
    if (d->mb_completos != d->s->ancho_mb*d->s->alto_mb) { d->error="Fotograma incompleto";return 0; }
    h264_desbloquear(d);
    f->salida=1;f->ocupado=0;
    f->ref=d->sl.nal_ref!=0;
    f->larga=d->sl.idr && d->sl.larga_idr ? 0 : -1;
    if (f->ref && !d->sl.adaptativo) {
        unsigned n=0;
        for (int i=0;i<18;i++) n+=d->fotos[i].ref;
        while (n>d->s->refs) {
            h264_foto *vieja=NULL;
            int orden_viejo=0;
            for (int i=0;i<18;i++) {
                h264_foto *a=&d->fotos[i];
                if (!a->ref || a==f || a->larga>=0) continue;
                int orden=a->frame_num > f->frame_num ? a->frame_num-(1<<d->s->log_frame) : a->frame_num;
                if (!vieja || orden<orden_viejo) { vieja=a;orden_viejo=orden; }
            }
            if (!vieja) { d->error="DPB sin referencia liberable";return 0; }
            vieja->ref=0;n--;
        }
    }
    if (d->sl.adaptativo) for (unsigned j=0;j<d->sl.marcas_n;j++) {
        unsigned op=d->sl.marcas_op[j],a=d->sl.marcas_a[j],b=d->sl.marcas_b[j];
        h264_foto *objetivo=NULL;
        if (op==1 || op==3) {
            unsigned max=1u<<d->s->log_frame;
            if (a>=max) return 0;
            int num=(f->frame_num+(int)max-1-(int)a)%(int)max;
            for (int i=0;i<18;i++) if (&d->fotos[i]!=f && d->fotos[i].ref &&
                    d->fotos[i].larga<0 && d->fotos[i].frame_num==num) objetivo=&d->fotos[i];
            if (!objetivo) { d->error="MMCO señala una referencia corta ausente";return 0; }
        } else if (op==2) {
            for (int i=0;i<18;i++) if (&d->fotos[i]!=f && d->fotos[i].ref &&
                    d->fotos[i].larga>=0 && (unsigned)d->fotos[i].larga==a) objetivo=&d->fotos[i];
            if (!objetivo) { d->error="MMCO señala una referencia larga ausente";return 0; }
        }
        if (op==1 || op==2) objetivo->ref=0;
        else if (op==3 || op==6) {
            unsigned indice=op==3?b:a;
            if (d->max_larga<0 || indice>(unsigned)d->max_larga) return 0;
            for (int i=0;i<18;i++) if (&d->fotos[i]!=f && d->fotos[i].ref &&
                    d->fotos[i].larga==(int)indice) d->fotos[i].ref=0;
            if (op==6) objetivo=f;
            objetivo->larga=(int)indice;
        } else if (op==4) {
            if (a>d->s->refs) return 0;
            d->max_larga=(int)a-1;
            for (int i=0;i<18;i++) if (&d->fotos[i]!=f && d->fotos[i].larga>d->max_larga) d->fotos[i].ref=0;
        } else if (op==5) {
            for (int i=0;i<18;i++) if (&d->fotos[i]!=f) d->fotos[i].ref=0;
            d->max_larga=-1;f->frame_num=0;f->poc=0;
            d->poc_msb_anterior=0;
            d->poc_lsb_anterior=d->sl.delta_poc[1]<0?-d->sl.delta_poc[1]:0;
            d->frame_num_anterior=d->frame_offset=0;
        }
    }
    unsigned referencias=0;
    for (int i=0;i<18;i++) referencias+=d->fotos[i].ref!=0;
    if (referencias>d->s->refs) { d->error="MMCO excede la capacidad del DPB";return 0; }
    d->actual=NULL;
    if (!vaciar(d,d->s->reordenar)) { d->fallo=H264_CANCELADO;return 0; }
    return 1;
}

h264_resultado h264_finalizar(h264_decodificador *d) {
    if (!d) return H264_DATOS_INVALIDOS;
    if (d->fallo) return (h264_resultado)d->fallo;
    if (!terminar_foto(d)) return fallo(d,d->fallo ? (h264_resultado)d->fallo : H264_DATOS_INVALIDOS,d->error);
    if (!vaciar(d,0)) return fallo(d,H264_CANCELADO,"Reproducción cancelada");
    return H264_OK;
}

static int cabecera_slice(h264_decodificador *d,unsigned nal,int64_t tiempo) {
    h264_bits *b=&d->bits;
    h264_slice *sl=&d->sl;
    h264_cero(sl,sizeof(*sl));
    sl->primero=h264_ue(b);
    unsigned tipo=h264_ue(b);
    if (tipo>9) return 0;
    sl->tipo=tipo%5;
    if (sl->tipo>2) { d->error="Slice SP/SI no soportada";return 0; }
    sl->pps_id=h264_ue(b);
    if (sl->pps_id>=H264_MAX_PPS || !d->pps[sl->pps_id].valido) return 0;
    d->p=&d->pps[sl->pps_id];
    d->s=&d->sps[d->p->sps];
    if (!d->s->valido || sl->primero>=d->s->ancho_mb*d->s->alto_mb) return 0;
    sl->frame_num=h264_bits_leer(b,d->s->log_frame);
    sl->idr=(nal&31)==5;sl->nal_ref=(nal>>5)&3;
    if (sl->idr) h264_ue(b);
    if (d->s->poc_tipo==0) {
        sl->poc_lsb=h264_bits_leer(b,d->s->log_poc);
        if (d->p->poc_abajo) sl->delta_poc[1]=h264_se(b);
    } else if (d->s->poc_tipo==1 && !d->s->delta_poc_cero) {
        sl->delta_poc[0]=h264_se(b);
        if (d->p->poc_abajo) sl->delta_poc[1]=h264_se(b);
    }
    if (d->p->redundante && h264_ue(b)) return 0;
    if (sl->tipo==1) sl->directo_espacial=h264_bits_leer(b,1);
    sl->refs[0]=d->p->ref_def[0];sl->refs[1]=d->p->ref_def[1];
    if (sl->tipo!=2 && h264_bits_leer(b,1)) {
        unsigned a=h264_ue(b),c=sl->tipo==1 ? h264_ue(b) : 0;
        if (a>31 || c>31) return 0;
        sl->refs[0]=a+1;if (sl->tipo==1) sl->refs[1]=c+1;
    }
    if (sl->tipo!=2) for (unsigned l=0;l<(sl->tipo==1?2u:1u);l++) {
        if (h264_bits_leer(b,1)) for (unsigned i=0;i<64;i++) {
            unsigned op=h264_ue(b);
            if (op==3) break;
            if (op>3 || i==63) return 0;
            sl->reord_tipo[l][i]=op;sl->reord_val[l][i]=h264_ue(b);sl->reord_n[l]++;
        }
    }
    if ((d->p->ponderado && sl->tipo==0) || (d->p->bipred==1 && sl->tipo==1)) {
        unsigned dy=h264_ue(b),dc=h264_ue(b);
        if (dy>7 || dc>7) return 0;
        sl->denom[0]=(int)dy;sl->denom[1]=sl->denom[2]=(int)dc;
        for (unsigned l=0;l<(sl->tipo==1?2u:1u);l++) for (unsigned r=0;r<sl->refs[l];r++) {
            for (int p=0;p<3;p++) sl->peso[l][r][p]=1<<sl->denom[p];
            if (h264_bits_leer(b,1)) { sl->peso[l][r][0]=h264_se(b);sl->offset[l][r][0]=h264_se(b); }
            if (h264_bits_leer(b,1)) for (int p=1;p<3;p++) {
                sl->peso[l][r][p]=h264_se(b);sl->offset[l][r][p]=h264_se(b);
            }
            for (int p=0;p<3;p++) if (sl->peso[l][r][p]<-128 || sl->peso[l][r][p]>128 ||
                    sl->offset[l][r][p]<-128 || sl->offset[l][r][p]>127) return 0;
        }
    }
    if (sl->nal_ref) {
        if (sl->idr) { sl->omitir_salida=h264_bits_leer(b,1);sl->larga_idr=h264_bits_leer(b,1); }
        else if ((sl->adaptativo=h264_bits_leer(b,1))) for (unsigned i=0;i<64;i++) {
            unsigned op=h264_ue(b);
            if (!op) break;
            if (op>6 || i==63) return 0;
            sl->marcas_op[i]=op;
            if (op==1 || op==3 || op==2 || op==4 || op==6) sl->marcas_a[i]=h264_ue(b);
            if (op==3) sl->marcas_b[i]=h264_ue(b);
            sl->marcas_n++;
        }
    }
    if (d->p->cabac && sl->tipo!=2) sl->cabac_id=h264_ue(b);
    if (sl->cabac_id>2) return 0;
    int delta=h264_se(b);
    if (delta < -51 || delta > 51) return 0;
    sl->qp=d->p->qp_inicial+delta;
    if (sl->qp<0 || sl->qp>51) return 0;
    if (d->p->deblock_presente) {
        unsigned filtro=h264_ue(b);
        if (filtro>2) return 0;
        sl->filtro=(int)filtro;
        if (filtro!=1) {
            sl->alfa=h264_se(b);sl->beta=h264_se(b);
            if (sl->alfa < -6 || sl->alfa > 6 || sl->beta < -6 || sl->beta > 6) return 0;
            sl->alfa*=2;sl->beta*=2;
        }
    }
    (void)tiempo;
    return !b->error;
}

static int iniciar_foto(h264_decodificador *d,int64_t tiempo) {
    h264_slice *sl=&d->sl;
    if (sl->idr) {
        if (sl->omitir_salida) for (int i=0;i<18;i++) d->fotos[i].salida=0;
        else if (!vaciar(d,0)) { d->fallo=H264_CANCELADO;return 0; }
        for (int i=0;i<18;i++) d->fotos[i].ref=0;
        d->max_larga=sl->larga_idr?0:-1;
        d->poc_lsb_anterior=d->poc_msb_anterior=d->frame_num_anterior=d->frame_offset=0;
    }
    h264_foto *f=NULL;
    for (int i=0;i<18;i++) if (!d->fotos[i].ref && !d->fotos[i].salida) { f=&d->fotos[i];break; }
    if (!f) { d->error="DPB lleno";return 0; }
    unsigned w=d->s->ancho_mb*16,h=d->s->alto_mb*16;
    if (f->w!=w || f->h!=h || !f->pixeles || !f->mb) {
        if (f->pixeles) d->servicios.liberar(d->servicios.usuario,f->pixeles);
        if (f->mb) d->servicios.liberar(d->servicios.usuario,f->mb);
        f->pixeles=d->servicios.asignar(d->servicios.usuario,(size_t)w*h*3/2);
        f->mb=d->servicios.asignar(d->servicios.usuario,(size_t)d->s->ancho_mb*d->s->alto_mb*sizeof(h264_mb));
        if (!f->pixeles || !f->mb) { d->fallo=H264_SIN_MEMORIA;d->error="Memoria insuficiente para fotograma/DPB";return 0; }
    }
    f->w=w;f->h=h;f->ancho_mb=d->s->ancho_mb;f->alto_mb=d->s->alto_mb;
    f->crop_x=d->s->crop_izq;f->crop_y=d->s->crop_arriba;
    f->ancho_visible=d->s->ancho;f->alto_visible=d->s->alto;
    f->rango_completo=d->s->rango_completo;f->matriz_color=d->s->matriz_color;
    if (d->siguiente_identificador==UINT64_MAX) { d->fallo=H264_LIMITE_EXCEDIDO;return 0; }
    f->identificador=++d->siguiente_identificador;
    h264_cero(f->referencias_id,sizeof(f->referencias_id));
    h264_cero(f->pixeles,(size_t)w*h*3/2);
    size_t n=(size_t)d->s->ancho_mb*d->s->alto_mb;
    h264_cero(f->mb,n*sizeof(h264_mb));
    for (size_t i=0;i<n;i++) {
        f->mb[i].slice=-1;
        for (int l=0;l<2;l++) for (int b=0;b<16;b++) {
            f->mb[i].ref[l][b]=-1;f->mb[i].ref_foto[l][b]=-1;
        }
    }
    f->frame_num=(int)sl->frame_num;f->ref=0;f->larga=-1;f->ocupado=1;f->tiempo=tiempo;
    int msb=d->poc_msb_anterior;
    if (d->s->poc_tipo==0) {
        int max=1<<d->s->log_poc;
        if ((int)sl->poc_lsb<d->poc_lsb_anterior && d->poc_lsb_anterior-(int)sl->poc_lsb>=max/2) msb+=max;
        else if ((int)sl->poc_lsb>d->poc_lsb_anterior && (int)sl->poc_lsb-d->poc_lsb_anterior>max/2) msb-=max;
        int64_t orden=(int64_t)msb+sl->poc_lsb+(sl->delta_poc[1]<0 ? sl->delta_poc[1] : 0);
        if (orden<-(1<<28) || orden>(1<<28)) { d->fallo=H264_LIMITE_EXCEDIDO;return 0; }
        f->poc=(int)orden;
        if (sl->nal_ref) { d->poc_msb_anterior=msb;d->poc_lsb_anterior=(int)sl->poc_lsb; }
    } else { d->fallo=H264_NO_SOPORTADO;d->error="POC tipo 1/2 no soportado";return 0; }
    d->actual=f;d->mb_completos=0;d->slice_id=0;
    return 1;
}

static int decodificar_slice(h264_decodificador *d,unsigned nal,int64_t tiempo) {
    h264_bits copia=d->bits;
    unsigned primero=h264_ue(&copia);
    if (copia.error) return 0;
    if (!primero && !terminar_foto(d)) return 0;
    unsigned pps_previo=d->sl.pps_id,frame_previo=d->sl.frame_num,poc_previo=d->sl.poc_lsb;
    unsigned ref_previo=d->sl.nal_ref,idr_previo=d->sl.idr;
    if (!cabecera_slice(d,nal,tiempo)) return 0;
    if (!d->p->cabac) { d->fallo=H264_NO_SOPORTADO;d->error="CAVLC no soportado";return 0; }
    if (!primero) {
        if (!iniciar_foto(d,tiempo)) return 0;
    } else {
        if (!d->actual || primero != d->mb_completos || d->slice_id>=32766) return 0;
        if (d->sl.pps_id!=pps_previo || d->sl.frame_num!=frame_previo || d->sl.poc_lsb!=poc_previo ||
            d->sl.nal_ref!=ref_previo || d->sl.idr!=idr_previo ||
            d->actual->w!=d->s->ancho_mb*16 || d->actual->h!=d->s->alto_mb*16) return 0;
        d->slice_id++;
    }
    if (d->sl.tipo!=2 && !h264_preparar_referencias(d)) return 0;
    while (d->bits.posicion&7) if (!h264_bits_leer(&d->bits,1)) return 0;
    if (!h264_cabac_iniciar(&d->cabac,&d->bits,d->sl.qp,d->sl.tipo==2?0:d->sl.cabac_id+1)) return 0;
    unsigned n=d->s->ancho_mb*d->s->alto_mb;
    for (d->mb_actual=primero;d->mb_actual<n;d->mb_actual++) {
        h264_mb *m=&d->actual->mb[d->mb_actual];
        m->slice=(int16_t)d->slice_id;
        m->filtro=(int8_t)d->sl.filtro;m->alfa=(int8_t)d->sl.alfa;m->beta=(int8_t)d->sl.beta;
        int salto=0;
        if (d->sl.tipo!=2) {
            int x=-1,y=0;
            h264_mb *a=h264_vecino(d,&x,&y,0);
            x=0;y=-1;
            h264_mb *b=h264_vecino(d,&x,&y,0);
            salto=(int)h264_cabac_bin(&d->cabac,(d->sl.tipo==1?24:11)+(a&&!a->salto)+(b&&!b->salto));
        }
        int tipo=salto?0:h264_leer_mb_tipo(d);
        int intra=d->sl.tipo==2 ? tipo : d->sl.tipo==0 ? tipo-5 : tipo-23;
        if (salto || intra<0) {
            if (!h264_inter(d,(unsigned)tipo,salto)) return 0;
        } else if (!h264_intra(d,(unsigned)intra)) return 0;
        d->mb_completos++;
        unsigned fin=h264_cabac_terminar(&d->cabac);
        if (d->bits.error) return 0;
        if (fin) return 1;
    }
    return 0;
}

h264_resultado h264_nal(h264_decodificador *d,const uint8_t *p,size_t n,int64_t tiempo) {
    if (!d || !p || n<1) return H264_DATOS_INVALIDOS;
    if (d->fallo) return (h264_resultado)d->fallo;
    if (n>H264_MAX_NAL) return fallo(d,H264_LIMITE_EXCEDIDO,"NAL excede el límite de memoria");
    if (p[0]&128) return fallo(d,H264_DATOS_INVALIDOS,"forbidden_zero_bit");
    unsigned nal=p[0],tipo=nal&31;
    if (tipo==6 || tipo==9 || tipo==12) return H264_OK;
    if (tipo==10 || tipo==11) return h264_finalizar(d);
    if (tipo!=1 && tipo!=5 && tipo!=7 && tipo!=8) return fallo(d,H264_NO_SOPORTADO,"Tipo de NAL no soportado");
    if ((tipo==7 || tipo==8) && !terminar_foto(d))
        return fallo(d,d->fallo ? (h264_resultado)d->fallo : H264_DATOS_INVALIDOS,d->error);
    if (n>d->capacidad_rbsp) {
        uint8_t *q=d->servicios.asignar(d->servicios.usuario,n);
        if (!q) return fallo(d,H264_SIN_MEMORIA,"Memoria para RBSP");
        if (d->rbsp) d->servicios.liberar(d->servicios.usuario,d->rbsp);
        d->rbsp=q;d->capacidad_rbsp=n;
    }
    size_t k=0;unsigned ceros=0;
    for (size_t i=1;i<n;i++) {
        if (ceros==2 && p[i]==3) {
            if (i+1>=n || p[i+1]>3) return fallo(d,H264_DATOS_INVALIDOS,"Emulation prevention inválida");
            ceros=0;continue;
        }
        if (ceros==2 && p[i]<3) return fallo(d,H264_DATOS_INVALIDOS,"Secuencia reservada dentro de NAL");
        d->rbsp[k++]=p[i];ceros=p[i]==0?ceros+1:0;
    }
    d->bits=(h264_bits){d->rbsp,k,0,0};d->error="Sintaxis o reconstrucción de slice inválida";
    if (tipo==7) {
        h264_sps s;
        h264_resultado r=h264_leer_sps(&d->bits,&s);
        if (r!=H264_OK) return fallo(d,r,"SPS inválido o función no soportada");
        d->sps[s.id]=s;
    } else if (tipo==8) {
        h264_pps pp;
        h264_resultado r=h264_leer_pps(&d->bits,&pp);
        if (r!=H264_OK) return fallo(d,r,"PPS inválido o función no soportada");
        d->pps[pp.id]=pp;
    } else if (!decodificar_slice(d,nal,tiempo)) {
        return fallo(d,d->fallo ? (h264_resultado)d->fallo : H264_DATOS_INVALIDOS,d->error);
    }
    d->error="Sin error";
    return H264_OK;
}
