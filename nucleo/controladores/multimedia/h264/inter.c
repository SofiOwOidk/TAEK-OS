#include "decodificador.h"
#include "inter_kernels.h"

#ifndef H264_PERFIL_INTER
#define H264_PERFIL_INTER 0
#endif
#if H264_PERFIL_INTER
static uint64_t reloj_inter(void) {
    uint32_t lo,hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi<<32)|lo;
}
static void medir_inter(h264_telemetria *t,int etapa,uint64_t inicio,int pixeles) {
    if(t) {
        t->ciclos_inter_etapa[etapa]+=reloj_inter()-inicio;
        t->bloques_inter_etapa[etapa]++;
        t->pixeles_inter_etapa[etapa]+=(unsigned)pixeles;
    }
}
#endif

static unsigned bin(h264_decodificador *d,unsigned ctx) { return h264_cabac_bin(&d->cabac,ctx); }
static int wrap(h264_decodificador *d,h264_foto *f) {
    return f->frame_num>(int)d->sl.frame_num ? f->frame_num-(1<<d->s->log_frame) : f->frame_num;
}
static int antes(h264_decodificador *d,h264_foto *a,h264_foto *b,unsigned lista) {
    if (a->larga>=0 || b->larga>=0)
        return a->larga<0 || (b->larga>=0 && a->larga<b->larga);
    if (d->sl.tipo==0) return wrap(d,a)>wrap(d,b);
    int ap=a->poc<d->actual->poc,bp=b->poc<d->actual->poc;
    if (ap!=bp) return lista ? !ap : ap;
    return ap ? a->poc>b->poc : a->poc<b->poc;
}

int h264_preparar_referencias(h264_decodificador *d) {
    h264_slice *s=&d->sl;
    unsigned cuenta[2]={0,0},listas=s->tipo==1?2:1;
    for (unsigned l=0;l<listas;l++) {
        for (int i=0;i<18;i++) if (d->fotos[i].ref) {
            if (d->fotos[i].w!=d->actual->w || d->fotos[i].h!=d->actual->h) {
                d->error="Cambio de resolución sin IDR no soportado";return 0;
            }
            d->actual->referencias_id[i]=d->fotos[i].identificador;
            unsigned pos=cuenta[l]++;
            while (pos && antes(d,&d->fotos[i],s->lista[l][pos-1],l)) {
                s->lista[l][pos]=s->lista[l][pos-1];pos--;
            }
            s->lista[l][pos]=&d->fotos[i];
        }
    }
    if (listas==2 && cuenta[0]>1) {
        int iguales=1;
        for (unsigned i=0;i<cuenta[0];i++) if (s->lista[0][i]!=s->lista[1][i]) iguales=0;
        if (iguales) { h264_foto *f=s->lista[1][0];s->lista[1][0]=s->lista[1][1];s->lista[1][1]=f; }
    }
    for (unsigned l=0;l<listas;l++) {
        int pred=(int)s->frame_num,max=1<<d->s->log_frame;
        for (unsigned j=0;j<s->reord_n[l];j++) {
            if (j>=s->refs[l]) return 0;
            h264_foto *f=NULL;
            unsigned op=s->reord_tipo[l][j],v=s->reord_val[l][j];
            if (op<2) {
                if (v>=(unsigned)max) return 0;
                pred=(pred+(op?1:-1)*(int)(v+1)+max)%max;
                for (int i=0;i<18;i++) if (d->fotos[i].ref && d->fotos[i].larga<0 && d->fotos[i].frame_num==pred) f=&d->fotos[i];
            } else for (int i=0;i<18;i++) if (d->fotos[i].ref && d->fotos[i].larga==(int)v) f=&d->fotos[i];
            if (!f) { d->error="Referencia solicitada ausente del DPB";return 0; }
            h264_foto *temporal[32];
            for (unsigned k=0;k<j;k++) temporal[k]=s->lista[l][k];
            temporal[j]=f;
            unsigned n=j+1;
            for (unsigned k=j;k<cuenta[l] && n<32;k++) if (s->lista[l][k]!=f) temporal[n++]=s->lista[l][k];
            for (unsigned k=0;k<n;k++) s->lista[l][k]=temporal[k];
            cuenta[l]=n;
        }
        if (cuenta[l]<s->refs[l]) { d->error="Lista de referencias incompleta";return 0; }
    }
    return 1;
}

typedef struct { int ref,x,y; } movimiento;
static movimiento vecino_mv(h264_decodificador *d,int x,int y,int l) {
    h264_mb *m=h264_vecino(d,&x,&y,0);
    movimiento r={-2,0,0};
    if (!m) return r;
    int i=y*4+x;
    if (m==h264_mb_actual(d) && !(m->movimiento_listo[l]&(1u<<i))) return r;
    r.ref=m->ref[l][i];r.x=m->mv[l][i][0];r.y=m->mv[l][i][1];
    return r;
}
static void vecinos_abc(h264_decodificador *d,int x,int y,int w,int l,movimiento *a,movimiento *b,movimiento *c) {
    *a=vecino_mv(d,x-1,y,l);*b=vecino_mv(d,x,y-1,l);*c=vecino_mv(d,x+w,y-1,l);
    if (c->ref==-2) *c=vecino_mv(d,x-1,y-1,l);
}
static int mediana(int a,int b,int c) { return a>b ? (b>c ? b : a>c ? c:a) : (a>c ? a : b>c ? c:b); }
static movimiento predecir_mv(h264_decodificador *d,int x,int y,int w,int h,int l,int ref) {
    movimiento a,b,c;
    vecinos_abc(d,x,y,w,l,&a,&b,&c);
    if (w==4 && h==2) {
        if (!y && b.ref==ref) return b;
        if (y && a.ref==ref) return a;
    }
    if (w==2 && h==4) {
        if (!x && a.ref==ref) return a;
        if (x && c.ref==ref) return c;
    }
    if (b.ref==-2 && c.ref==-2 && a.ref!=-2) b=c=a;
    if ((a.ref==ref)+(b.ref==ref)+(c.ref==ref)==1) return a.ref==ref?a:b.ref==ref?b:c;
    movimiento r={ref,mediana(a.x,b.x,c.x),mediana(a.y,b.y,c.y)};
    return r;
}

static int leer_ref(h264_decodificador *d,int x,int y,int l) {
    int ax=x-1,ay=y,bx=x,by=y-1;
    h264_mb *a=h264_vecino(d,&ax,&ay,0),*b=h264_vecino(d,&bx,&by,0);
    unsigned ca=a && !(a->bloques_directos&(1u<<(ay*4+ax))) && a->ref[l][ay*4+ax]>0;
    unsigned cb=b && !(b->bloques_directos&(1u<<(by*4+bx))) && b->ref[l][by*4+bx]>0;
    unsigned r=0,ctx=54+ca+2*cb;
    while (bin(d,ctx)) {
        if (++r>=d->sl.refs[l]) { d->bits.error=1;return 0; }
        ctx=r==1?58:59;
    }
    return (int)r;
}

static int leer_mvd(h264_decodificador *d,int x,int y,int l,int eje) {
    int ax=x-1,ay=y,bx=x,by=y-1;
    h264_mb *a=h264_vecino(d,&ax,&ay,0),*b=h264_vecino(d,&bx,&by,0);
    unsigned suma=(a?a->mvd[l][ay*4+ax][eje]:0)+(b?b->mvd[l][by*4+bx][eje]:0);
    unsigned base=eje?47:40;
    if (!bin(d,base+(suma<3?0:suma>32?2:1))) return 0;
    unsigned v=1;
    while (v<9 && bin(d,base+(v<4?v+2:6))) v++;
    if (v==9) {
        unsigned k=3;
        while (!d->bits.error && h264_cabac_bypass(&d->cabac)) {
            v+=1u<<k;
            if (++k>14) { d->bits.error=1;return 0; }
        }
        unsigned sufijo=0;
        for (unsigned i=0;i<k;i++) sufijo=sufijo*2+h264_cabac_bypass(&d->cabac);
        v+=sufijo;
    }
    if (v>32767) { d->bits.error=1;return 0; }
    return h264_cabac_bypass(&d->cabac)?-(int)v:(int)v;
}

static void guardar_mv(h264_decodificador *d,int x,int y,int w,int h,int l,int ref,int vx,int vy,int dx,int dy) {
    h264_mb *m=h264_mb_actual(d);
    int foto=ref<0?-1:(int)(d->sl.lista[l][ref]-d->fotos);
    for (int j=y;j<y+h;j++) for (int i=x;i<x+w;i++) {
        int k=j*4+i;
        m->ref[l][k]=(int8_t)ref;m->ref_foto[l][k]=(int8_t)foto;
        m->mv[l][k][0]=(int16_t)vx;m->mv[l][k][1]=(int16_t)vy;
        m->mvd[l][k][0]=(uint8_t)h264_recortar(h264_abs(dx),0,70);
        m->mvd[l][k][1]=(uint8_t)h264_recortar(h264_abs(dy),0,70);
        m->movimiento_listo[l]|=(uint16_t)(1u<<k);
    }
}

static int pixel(const h264_foto *f,int p,int x,int y) {
    int w=(int)f->w/(p?2:1),h=(int)f->h/(p?2:1);
    x=h264_recortar(x,0,w-1);y=h264_recortar(y,0,h-1);
    size_t base=p?(size_t)f->w*f->h*(3+p)/4:0;
    return f->pixeles[base+(size_t)y*w+x];
}
static int medio_h_crudo(const h264_foto *f,int x,int y) {
    return pixel(f,0,x-2,y)-5*pixel(f,0,x-1,y)+20*pixel(f,0,x,y)+20*pixel(f,0,x+1,y)-5*pixel(f,0,x+2,y)+pixel(f,0,x+3,y);
}
static int medio_h(const h264_foto *f,int x,int y) { return h264_recortar((medio_h_crudo(f,x,y)+16)>>5,0,255); }
static int medio_v(const h264_foto *f,int x,int y) {
    int v=pixel(f,0,x,y-2)-5*pixel(f,0,x,y-1)+20*pixel(f,0,x,y)+20*pixel(f,0,x,y+1)-5*pixel(f,0,x,y+2)+pixel(f,0,x,y+3);
    return h264_recortar((v+16)>>5,0,255);
}
static int medio_d(const h264_foto *f,int x,int y) {
    int v=medio_h_crudo(f,x,y-2)-5*medio_h_crudo(f,x,y-1)+20*medio_h_crudo(f,x,y)+20*medio_h_crudo(f,x,y+1)-5*medio_h_crudo(f,x,y+2)+medio_h_crudo(f,x,y+3);
    return h264_recortar((v+512)>>10,0,255);
}
static int interpolar(const h264_foto *f,int plano,int xx,int yy) {
    int den=plano?8:4,x=xx>>(plano?3:2),y=yy>>(plano?3:2);
    int dx=xx&(den-1),dy=yy&(den-1);
    if (plano) return ((8-dx)*(8-dy)*pixel(f,plano,x,y)+dx*(8-dy)*pixel(f,plano,x+1,y)+
                      (8-dx)*dy*pixel(f,plano,x,y+1)+dx*dy*pixel(f,plano,x+1,y+1)+32)>>6;
    if (!dx&&!dy) return pixel(f,0,x,y);
    if (!dy) {
        int b=medio_h(f,x,y);
        return dx==2?b:(b+pixel(f,0,x+(dx==3),y)+1)>>1;
    }
    if (!dx) {
        int h=medio_v(f,x,y);
        return dy==2?h:(h+pixel(f,0,x,y+(dy==3))+1)>>1;
    }
    if (dx==2&&dy==2) return medio_d(f,x,y);
    if (dx==2) return (medio_d(f,x,y)+medio_h(f,x,y+(dy==3))+1)>>1;
    if (dy==2) return (medio_d(f,x,y)+medio_v(f,x+(dx==3),y)+1)>>1;
    return (medio_h(f,x,y+(dy==3))+medio_v(f,x+(dx==3),y)+1)>>1;
}

/* Descriptor inmutable durante una compensación: los valores dependientes del
 * bloque se calculan una vez, fuera de los recorridos de píxeles. */
typedef struct {
    const h264_foto *foto;
    const uint8_t *plano;
    int ancho,alto,paso,base_x,base_y,mvx,mvy,den,dx,dy;
    unsigned componente;
    h264_telemetria *medicion;
    int sse2;
} pred_bloque;

static void preparar_pred(const h264_foto *f,int plano,int x,int y,int mvx,int mvy,pred_bloque *p) {
    p->foto=f;p->medicion=NULL;p->sse2=0;p->componente=(unsigned)plano;p->ancho=(int)f->w/(plano?2:1);
    p->alto=(int)f->h/(plano?2:1);p->paso=p->ancho;
    size_t base=plano?(size_t)f->w*f->h*(3+plano)/4:0;
    p->plano=f->pixeles+base;p->den=plano?8:4;p->mvx=mvx;p->mvy=mvy;
    int xx=x*p->den+mvx,yy=y*p->den+mvy;
    p->base_x=xx>> (plano?3:2);p->base_y=yy>>(plano?3:2);
    p->dx=xx&(p->den-1);p->dy=yy&(p->den-1);
}

static int filtro_h(const uint8_t *q) {
    return (int)q[-2]-5*(int)q[-1]+20*(int)q[0]+20*(int)q[1]-5*(int)q[2]+q[3];
}
static int filtro_v(const uint8_t *q,int paso) {
    return (int)q[-2*paso]-5*(int)q[-paso]+20*(int)q[0]+20*(int)q[paso]-5*(int)q[2*paso]+q[3*paso];
}

/* Kernel escalar: origen apunta al píxel entero del bloque; luma <=16x16,
 * croma <=8x8, sin alineación exigida. Márgenes: luma -2/+3 si hay filtro
 * en ese eje; croma +1 sólo en ejes fraccionarios. salida es del llamador.
 * Intermedios con signo de 32 bits: no recortar antes del filtro diagonal. */
static void pred_kernel(const uint8_t *src,int paso,int w,int h,int dx,int dy,
                        int croma,uint8_t *salida) {
    if (!dx&&!dy) {
        for (int j=0;j<h;j++) h264_copiar(salida+j*w,src+j*paso,(size_t)w);
        return;
    }
    if (croma) {
            for (int j=0;j<h;j++) for (int i=0;i<w;i++) {
                const uint8_t *q=src+j*paso+i;
                int a=q[0],b=dx?q[1]:a,c=dy?q[paso]:a;
                int d=dx&&dy?q[paso+1]:a;
                salida[j*w+i]=(uint8_t)(((8-dx)*(8-dy)*a+dx*(8-dy)*b+(8-dx)*dy*c+dx*dy*d+32)>>6);
            }
        return;
    }
    if (!dy) {
        for (int j=0;j<h;j++) for (int i=0;i<w;i++) {
            const uint8_t *q=src+j*paso+i;
            int medio=h264_recortar((filtro_h(q)+16)>>5,0,255);
            salida[j*w+i]=(uint8_t)(dx==2?medio:(medio+q[dx==3]+1)>>1);
        }
        return;
    }
    if (!dx) {
        for (int j=0;j<h;j++) for (int i=0;i<w;i++) {
            const uint8_t *q=src+j*paso+i;
            int medio=h264_recortar((filtro_v(q,paso)+16)>>5,0,255);
            salida[j*w+i]=(uint8_t)(dy==2?medio:(medio+q[dy==3?paso:0]+1)>>1);
        }
        return;
    }
    /* Interpolación diagonal: una fila horizontal intermedia se calcula una
     * sola vez para cada coordenada usada por todo el bloque. */
    int32_t horizontal[21][16];
    if (dx==2 || dy==2) {
        int filas=h+5;
        for (int j=0;j<filas;j++) for (int i=0;i<w;i++) {
            const uint8_t *q=src+(j-2)*paso+i;
            horizontal[j][i]=filtro_h(q);
        }
    }
    for (int j=0;j<h;j++) for (int i=0;i<w;i++) {
        const uint8_t *q=src+j*paso+i;
        int valor;
            if(dx==2||dy==2) {
                int diag=horizontal[j][i]-5*horizontal[j+1][i]+20*horizontal[j+2][i]+
                         20*horizontal[j+3][i]-5*horizontal[j+4][i]+horizontal[j+5][i];
                diag=h264_recortar((diag+512)>>10,0,255);
                if (dx==2&&dy==2) valor=diag;
                else if (dx==2) {
                    int medio=h264_recortar((horizontal[j+2+(dy==3)][i]+16)>>5,0,255);
                    valor=(diag+medio+1)>>1;
                } else {
                    const uint8_t *c=q+(dx==3);
                    int medio=h264_recortar((filtro_v(c,paso)+16)>>5,0,255);
                    valor=(diag+medio+1)>>1;
                }
            } else {
                const uint8_t *r=q+(dy==3?paso:0),*c=q+(dx==3);
                int vh=filtro_v(c,paso),hh=filtro_h(r);
                valor=(h264_recortar((hh+16)>>5,0,255)+h264_recortar((vh+16)>>5,0,255)+1)>>1;
            }
        salida[j*w+i]=(uint8_t)valor;
    }
}

static int pred_necesita_borde(const pred_bloque *p,int w,int h) {
    int izq=p->componente||!p->dx?0:2,arr=p->componente||!p->dy?0:2;
    int der=!p->dx?0:p->componente?1:3,abajo=!p->dy?0:p->componente?1:3;
    return p->base_x-izq<0||p->base_y-arr<0||
           p->base_x+w-1+der>=p->ancho||p->base_y+h-1+abajo>=p->alto;
}

static int pred_inter_bloque(const pred_bloque *p,int w,int h,uint8_t *salida) {
    if (w<1||h<1||w>(p->componente?8:16)||h>(p->componente?8:16)) return 0;
#if H264_PERFIL_INTER
    int etapa=p->componente?H264_INTER_CROMA:!p->dx&&!p->dy?H264_INTER_ENTERO:
              !p->dy?H264_INTER_HORIZONTAL:!p->dx?H264_INTER_VERTICAL:H264_INTER_DIAGONAL;
    uint64_t inicio;
#endif
    if (!pred_necesita_borde(p,w,h)) {
#if H264_PERFIL_INTER
        inicio=reloj_inter();
#endif
        const uint8_t *src=p->plano+(size_t)p->base_y*p->paso+p->base_x;
        if(!p->sse2||!h264_pred_sse2(src,p->paso,w,h,p->dx,p->dy,p->componente!=0,salida))
            pred_kernel(src,p->paso,w,h,p->dx,p->dy,p->componente!=0,salida);
    } else {
        /* Extensión acotada en stack: como máximo 21x21 bytes. Cada clamp se
         * resuelve una vez por muestra fuente, fuera del filtro interior. */
        uint8_t extendido[21*21];
#if H264_PERFIL_INTER
        uint64_t borde_inicio=reloj_inter();
#endif
        int izq=p->componente||!p->dx?0:2,arr=p->componente||!p->dy?0:2;
        int der=!p->dx?0:p->componente?1:3,abajo=!p->dy?0:p->componente?1:3;
        int ew=w+izq+der,eh=h+arr+abajo;
        for (int j=0;j<eh;j++) {
            int y=h264_recortar(p->base_y+j-arr,0,p->alto-1);
            for (int i=0;i<ew;i++) {
                int x=h264_recortar(p->base_x+i-izq,0,p->ancho-1);
                extendido[j*ew+i]=p->plano[(size_t)y*p->paso+x];
            }
        }
#if H264_PERFIL_INTER
        medir_inter(p->medicion,H264_INTER_BORDES,borde_inicio,ew*eh);
        inicio=reloj_inter();
#endif
        const uint8_t *src=extendido+arr*ew+izq;
        if(!p->sse2||!h264_pred_sse2(src,ew,w,h,p->dx,p->dy,p->componente!=0,salida))
            pred_kernel(src,ew,w,h,p->dx,p->dy,p->componente!=0,salida);
    }
#if H264_PERFIL_INTER
    medir_inter(p->medicion,etapa,inicio,w*h);
#endif
    return 1;
}

/* El modo de combinación se despacha una vez por bloque. El destino avanza
 * por filas; no se divide ni calcula módulo por cada píxel. */
static void combinar_bloque(uint8_t *dst,int paso,int w,int h,const uint8_t *a,
                            const uint8_t *b,int modo,int peso0,int peso1,int den,int offset) {
#define COMBINAR(expr) do { \
    for (int y=0;y<h;y++) { \
        for (int x=0;x<w;x++) dst[x]=(uint8_t)h264_recortar((expr),0,255); \
        dst+=paso;a+=w;b+=w; \
    } \
} while (0)
    switch (modo) {
        case 0: for(int y=0;y<h;y++) {h264_copiar(dst,a,(size_t)w);dst+=paso;a+=w;} break;
        case 1: COMBINAR((a[x]+b[x]+1)>>1);break;
        case 2: {int redondeo=den?1<<(den-1):0;COMBINAR(((peso0*a[x]+redondeo)>>den)+offset);break;}
        case 3: {int redondeo=1<<den;COMBINAR(((peso0*a[x]+peso1*b[x]+redondeo)>>(den+1))+offset);break;}
        case 4: COMBINAR(((64-peso1)*a[x]+peso1*b[x]+32)>>6);break;
    }
#undef COMBINAR
}

static int __attribute__((unused)) compensar_bloques(h264_decodificador *d) {
    h264_mb *m=h264_mb_actual(d);h264_foto *f=d->actual;
    uint16_t cubiertos=0;unsigned mx=d->mb_actual%d->s->ancho_mb,my=d->mb_actual/d->s->ancho_mb;
    for (int semilla=0;semilla<16;semilla++) {
        if (cubiertos&(1u<<semilla)) continue;
        int r0=m->ref[0][semilla],r1=m->ref[1][semilla];
        if (r0<0&&r1<0) return 0;
        int tx=semilla%4,ty=semilla/4,tw=1,th=1;
        while (tx+tw<4) { int k=ty*4+tx+tw;if(cubiertos&(1u<<k))break;
            if (m->ref[0][k]!=r0||m->ref[1][k]!=r1||m->mv[0][k][0]!=m->mv[0][semilla][0]||m->mv[0][k][1]!=m->mv[0][semilla][1]||m->mv[1][k][0]!=m->mv[1][semilla][0]||m->mv[1][k][1]!=m->mv[1][semilla][1]) break;tw++; }
        while (ty+th<4) { int bien=1;for(int xx=0;xx<tw;xx++){int k=(ty+th)*4+tx+xx;
            if((cubiertos&(1u<<k))||m->ref[0][k]!=r0||m->ref[1][k]!=r1||m->mv[0][k][0]!=m->mv[0][semilla][0]||m->mv[0][k][1]!=m->mv[0][semilla][1]||m->mv[1][k][0]!=m->mv[1][semilla][0]||m->mv[1][k][1]!=m->mv[1][semilla][1])bien=0;}if(!bien)break;th++; }
        for(int yy=0;yy<th;yy++)for(int xx=0;xx<tw;xx++)cubiertos|=(uint16_t)(1u<<((ty+yy)*4+tx+xx));
        for (int p=0;p<3;p++) {
            int factor=p?2:4,w=(int)f->w/(p?2:1),x=(int)mx*16+tx*4,y=(int)my*16+ty*4;
            int ox=p?x/2:x,oy=p?y/2:y,bw=tw*factor,bh=th*factor;
            size_t base=p?(size_t)f->w*f->h*(3+p)/4:0;uint8_t pred[2][256];
            int refs[2]={r0,r1};
            for(int l=0;l<2;l++)if(refs[l]>=0){
                h264_foto *ref=d->sl.lista[l][(unsigned)refs[l]];pred_bloque pb;
                preparar_pred(ref,p,ox,oy,m->mv[l][semilla][0],m->mv[l][semilla][1],&pb);
                pb.medicion=&d->telemetria;
                pb.sse2=d->inter_sse2;
                if(!pred_inter_bloque(&pb,bw,bh,pred[l]))
                    for(int yy=0;yy<bh;yy++)for(int xx=0;xx<bw;xx++)pred[l][yy*bw+xx]=(uint8_t)interpolar(ref,p,(ox+xx)*(p?8:4)+m->mv[l][semilla][0],(oy+yy)*(p?8:4)+m->mv[l][semilla][1]);
            }
            int den=d->sl.denom[p],ponderar=(d->p->ponderado&&d->sl.tipo==0)||(d->p->bipred==1&&d->sl.tipo==1);
            int peso0=0,peso1=0,offset=0,impl=0;
            if(ponderar){
                if(r0>=0&&r1>=0){peso0=d->sl.peso[0][r0][p];peso1=d->sl.peso[1][r1][p];offset=(d->sl.offset[0][r0][p]+d->sl.offset[1][r1][p]+1)>>1;}
                else {int l=r0<0,r=l?r1:r0;peso0=d->sl.peso[l][r][p];offset=d->sl.offset[l][r][p];}
            } else if(r0>=0&&r1>=0&&d->sl.tipo==1&&d->p->bipred==2){
                h264_foto *a=d->sl.lista[0][r0],*b=d->sl.lista[1][r1];int td=h264_recortar(b->poc-a->poc,-128,127),tb=h264_recortar(f->poc-a->poc,-128,127),w1=32;
                if(td&&a->larga<0&&b->larga<0){int t=(16384+h264_abs(td/2))/td,ds=h264_recortar((tb*t+32)>>6,-1024,1023);if((ds>>2)>=-64&&(ds>>2)<=128)w1=ds>>2;}impl=w1;
            }
            int doble=r0>=0&&r1>=0,modo=doble?1:0;
            if(ponderar)modo=doble?3:2;
            else if(doble&&d->sl.tipo==1&&d->p->bipred==2){modo=4;peso1=impl;}
#if H264_PERFIL_INTER
            uint64_t mezcla_inicio=reloj_inter();
#endif
            uint8_t *dst=f->pixeles+base+(size_t)oy*w+ox;
            if(!d->inter_sse2||!h264_combinar_sse2(dst,w,bw,bh,pred[r0<0?1:0],pred[1],modo,peso0,peso1,den,offset))
                combinar_bloque(dst,w,bw,bh,pred[r0<0?1:0],pred[1],modo,peso0,peso1,den,offset);
#if H264_PERFIL_INTER
            medir_inter(&d->telemetria,modo>=2?H264_INTER_PONDERADA:
                        doble?H264_INTER_DOBLE:H264_INTER_SIMPLE,mezcla_inicio,bw*bh);
#endif
        }
    }
    return 1;
}

/* Oráculo escalar compilable con -DH264_INTER_ESCALAR para contrastar la ruta
 * por bloques con el comportamiento previo, sin cambiar el flujo CABAC. */
static int __attribute__((unused)) compensar_escalar(h264_decodificador *d) {
    h264_mb *m=h264_mb_actual(d);h264_foto *f=d->actual;
    for (int k=0;k<16;k++) {
        int r0=m->ref[0][k],r1=m->ref[1][k];
        if (r0<0&&r1<0) return 0;
        for (int p=0;p<3;p++) {
            int n=p?2:4,w=(int)f->w/(p?2:1);
            int x=(int)(d->mb_actual%d->s->ancho_mb)*4*n+(k%4)*n;
            int y=(int)(d->mb_actual/d->s->ancho_mb)*4*n+(k/4)*n;
            size_t base=p?(size_t)f->w*f->h*(3+p)/4:0;
            for (int by=0;by<n;by++) for (int bx=0;bx<n;bx++) {
                int v[2]={0,0};
                for (int l=0;l<2;l++) if (m->ref[l][k]>=0) {
                    h264_foto *ref=d->sl.lista[l][(unsigned)m->ref[l][k]];
                    v[l]=interpolar(ref,p,(x+bx)*(p?8:4)+m->mv[l][k][0],(y+by)*(p?8:4)+m->mv[l][k][1]);
                }
                int valor=r0<0?v[1]:r1<0?v[0]:(v[0]+v[1]+1)>>1;
                if ((d->p->ponderado&&d->sl.tipo==0)||(d->p->bipred==1&&d->sl.tipo==1)) {
                    int den=d->sl.denom[p];
                    if (r0>=0&&r1>=0) valor=((d->sl.peso[0][r0][p]*v[0]+d->sl.peso[1][r1][p]*v[1]+(1<<den))>>(den+1))+
                         ((d->sl.offset[0][r0][p]+d->sl.offset[1][r1][p]+1)>>1);
                    else { int l=r0<0,r=l?r1:r0;valor=((d->sl.peso[l][r][p]*v[l]+(den?(1<<(den-1)):0))>>den)+d->sl.offset[l][r][p]; }
                } else if (r0>=0&&r1>=0&&d->sl.tipo==1&&d->p->bipred==2) {
                    h264_foto *a=d->sl.lista[0][r0],*b=d->sl.lista[1][r1];
                    int td=h264_recortar(b->poc-a->poc,-128,127),tb=h264_recortar(f->poc-a->poc,-128,127),w1=32;
                    if (td&&a->larga<0&&b->larga<0) { int tx=(16384+h264_abs(td/2))/td,ds=h264_recortar((tb*tx+32)>>6,-1024,1023);if((ds>>2)>=-64&&(ds>>2)<=128)w1=ds>>2; }
                    valor=((64-w1)*v[0]+w1*v[1]+32)>>6;
                }
                f->pixeles[base+(size_t)(y+by)*w+x+bx]=(uint8_t)h264_recortar(valor,0,255);
            }
        }
    }
    return 1;
}

int h264_compensar(h264_decodificador *d) {
#ifdef H264_INTER_ESCALAR
    return compensar_escalar(d);
#else
    return compensar_bloques(d);
#endif
}

static int directo(h264_decodificador *d,int gx,int gy,int gw,int gh) {
    if (!d->sl.lista[1][0] || !d->sl.lista[0][0]) return 0;
    h264_foto *col=d->sl.lista[1][0];
    h264_mb *cm=&col->mb[d->mb_actual],*m=h264_mb_actual(d);
    int refs[2]={-1,-1},cero=0;movimiento pred[2];
    if (d->sl.directo_espacial) {
        for (int l=0;l<2;l++) {
            movimiento a,b,c;vecinos_abc(d,0,0,4,l,&a,&b,&c);
            int r[3]={a.ref,b.ref,c.ref};
            for (int i=0;i<3;i++) if (r[i]>=0 && (refs[l]<0||r[i]<refs[l])) refs[l]=r[i];
        }
        if (refs[0]<0&&refs[1]<0) { refs[0]=refs[1]=0;cero=1; }
        for (int l=0;l<2;l++) pred[l]=predecir_mv(d,0,0,4,4,l,refs[l]);
    }
    for (int y=gy;y<gy+gh;y++) for (int x=gx;x<gx+gw;x++) {
        int ci=d->s->directo8 ? (y<2?0:12)+(x<2?0:3) : y*4+x;
        int cl=cm->ref[0][ci]>=0?0:1;
        int cr=cm->ref[cl][ci],cx=cm->mv[cl][ci][0],cy=cm->mv[cl][ci][1];
        if (cm->tipo) { cr=-1;cx=cy=0; }
        int mv[2][2]={{0,0},{0,0}};
        if (d->sl.directo_espacial) {
            int cz=col->larga<0 && cr==0 && h264_abs(cx)<=1 && h264_abs(cy)<=1;
            for (int l=0;l<2;l++) if (!cero&&refs[l]>=0&&!(refs[l]==0&&cz)) { mv[l][0]=pred[l].x;mv[l][1]=pred[l].y; }
        } else {
            refs[0]=0;refs[1]=0;
            if (cr>=0) {
                int slot=cm->ref_foto[cl][ci],encontrado=0;
                if (slot<0 || slot>=18) return 0;
                /* Un slot del DPB puede haberse reutilizado desde que se
                 * decodificó col; la identidad de la foto debe conservarse. */
                for (unsigned i=0;i<d->sl.refs[0];i++)
                    if (d->sl.lista[0][i]->identificador==col->referencias_id[slot]) {
                        refs[0]=(int)i;encontrado=1;break;
                    }
                if (!encontrado) return 0;
            }
            h264_foto *a=d->sl.lista[0][refs[0]];
            int td=h264_recortar(col->poc-a->poc,-128,127),tb=h264_recortar(d->actual->poc-a->poc,-128,127);
            if (!td || a->larga>=0) { mv[0][0]=cx;mv[0][1]=cy; }
            else {
                int tx=(16384+h264_abs(td/2))/td,ds=h264_recortar((tb*tx+32)>>6,-1024,1023);
                mv[0][0]=(ds*cx+128)>>8;mv[0][1]=(ds*cy+128)>>8;
                mv[1][0]=mv[0][0]-cx;mv[1][1]=mv[0][1]-cy;
            }
        }
        for (int l=0;l<2;l++) guardar_mv(d,x,y,1,1,l,refs[l],mv[l][0],mv[l][1],0,0);
        m->bloques_directos|=(uint16_t)(1u<<(y*4+x));
    }
    return 1;
}

typedef struct { int x,y,w,h,sw,sh,mascara,directo,ref[2]; } particion;
static int sub_tipo(h264_decodificador *d) {
    if (d->sl.tipo==0) {
        if (bin(d,21)) return 0;
        if (!bin(d,22)) return 1;
        return bin(d,23)?2:3;
    }
    if (!bin(d,36)) return 0;
    if (!bin(d,37)) return 1+(int)bin(d,39);
    if (!bin(d,38)) { int n=(int)bin(d,39)*2;n+=(int)bin(d,39);return 3+n; }
    if (bin(d,39)) return 11+(int)bin(d,39);
    int n=(int)bin(d,39)*2;n+=(int)bin(d,39);return 7+n;
}

int h264_inter(h264_decodificador *d,unsigned tipo,int salto) {
    h264_mb *m=h264_mb_actual(d);
    m->salto=(uint8_t)salto;m->directo=d->sl.tipo==1 && (salto||tipo==0);
    int permite8=1;
    if (salto && d->sl.tipo==0) {
        movimiento a=vecino_mv(d,-1,0,0),b=vecino_mv(d,0,-1,0),p={0,0,0};
        if (a.ref!=-2&&b.ref!=-2&&!(a.ref==0&&!a.x&&!a.y)&&!(b.ref==0&&!b.x&&!b.y)) p=predecir_mv(d,0,0,4,4,0,0);
        guardar_mv(d,0,0,4,4,0,0,p.x,p.y,0,0);
    } else if (m->directo) {
        if (!directo(d,0,0,4,4)) return 0;
        permite8=d->s->directo8;
    } else {
        particion g[4];h264_cero(g,sizeof(g));
        int n=1,sub=(d->sl.tipo==0 && tipo==3)||(d->sl.tipo==1 && tipo==22);
        if (sub) {
            n=4;
            for (int i=0;i<4;i++) {
                int t=sub_tipo(d);
                g[i].x=(i%2)*2;g[i].y=(i/2)*2;g[i].w=g[i].h=2;
                if (d->sl.tipo==0) {
                    g[i].mascara=1;g[i].sw=t==2||t==3?1:2;g[i].sh=t==1||t==3?1:2;
                } else {
                    g[i].directo=t==0;
                    g[i].mascara=t==1||t==4||t==5||t==10?1:t==2||t==6||t==7||t==11?2:3;
                    g[i].sw=t==5||t==7||t==9||t>=10?1:2;
                    g[i].sh=t==4||t==6||t==8||t>=10?1:2;
                }
                if (g[i].directo ? !d->s->directo8 : g[i].sw<2||g[i].sh<2) permite8=0;
            }
        } else {
            int horizontal=0;
            if (d->sl.tipo==0) { n=tipo?2:1;horizontal=tipo==1;g[0].mascara=g[1].mascara=1; }
            else if (tipo<=3) g[0].mascara=(int)tipo;
            else {
                static const uint8_t masks[9][2]={{1,1},{2,2},{1,2},{2,1},{1,3},{2,3},{3,1},{3,2},{3,3}};
                if (tipo>21) return 0;
                n=2;horizontal=!(tipo&1);unsigned par=(tipo-4)/2;
                g[0].mascara=masks[par][0];g[1].mascara=masks[par][1];
            }
            for (int i=0;i<n;i++) {
                g[i].w=n==1||horizontal?4:2;g[i].h=n==1||!horizontal?4:2;
                g[i].x=horizontal?0:i*g[i].w;g[i].y=horizontal?i*g[i].h:0;
                g[i].sw=g[i].w;g[i].sh=g[i].h;
            }
        }
        /* RefIdx se transmite antes que los vectores de todas las particiones. */
        for (int l=0;l<(d->sl.tipo==1?2:1);l++) for (int i=0;i<n;i++) {
            particion *p=&g[i];p->ref[l]=-1;
            if (p->directo || !(p->mascara&(1<<l))) continue;
            int ref=d->sl.refs[l]>1?leer_ref(d,p->x,p->y,l):0;
            p->ref[l]=ref;
            for (int y=p->y;y<p->y+p->h;y++) for (int x=p->x;x<p->x+p->w;x++) m->ref[l][y*4+x]=(int8_t)ref;
        }
        /* Direct usa sólo vecinos externos y puede rellenarse antes del resto. */
        for (int i=0;i<n;i++) if (g[i].directo && !directo(d,g[i].x,g[i].y,g[i].w,g[i].h)) return 0;
        for (int l=0;l<(d->sl.tipo==1?2:1);l++) for (int i=0;i<n;i++) {
            particion *p=&g[i];
            if (p->directo) continue;
            if (!(p->mascara&(1<<l))) {
                /* Una partición ya visitada sin esta lista sí está disponible
                 * como vecina: su RefIdx es -1, no "vecino inexistente". */
                guardar_mv(d,p->x,p->y,p->w,p->h,l,-1,0,0,0,0);
                continue;
            }
            for (int y=p->y;y<p->y+p->h;y+=p->sh) for (int x=p->x;x<p->x+p->w;x+=p->sw) {
                int dx=leer_mvd(d,x,y,l,0),dy=leer_mvd(d,x,y,l,1);
                movimiento pred=predecir_mv(d,x,y,p->sw,p->sh,l,p->ref[l]);
                /* La aritmética de vectores se define con envoltura a 16 bits. */
                int vx=(int16_t)(uint16_t)(pred.x+dx),vy=(int16_t)(uint16_t)(pred.y+dy);
                guardar_mv(d,x,y,p->sw,p->sh,l,p->ref[l],vx,vy,dx,dy);
            }
        }
    }
    if (d->bits.error || (!d->separar && !h264_compensar(d))) return 0;
    if (!salto) {
        m->cbp=(uint8_t)h264_leer_cbp(d);
        if ((m->cbp&15)&&d->p->transformada8&&permite8) {
            int x=-1,y=0;h264_mb *a=h264_vecino(d,&x,&y,0);
            x=0;y=-1;h264_mb *b=h264_vecino(d,&x,&y,0);
            m->transformada8=(uint8_t)bin(d,399+(a&&a->transformada8)+(b&&b->transformada8));
        }
    }
    if (m->cbp) { if (!h264_leer_delta_qp(d)) return 0; }
    else d->sl.previo_delta=0;
    m->qp=(uint8_t)d->sl.qp;
    return h264_reconstruir_residuo(d,0);
}
