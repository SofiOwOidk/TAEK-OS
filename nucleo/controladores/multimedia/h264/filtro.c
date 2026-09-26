#include "decodificador.h"

/* Tablas 8-16/17 de ITU-T H.264, profundidad de ocho bits. */
static const uint8_t alfa[52]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,6,7,8,9,10,12,13,15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,182,203,226,255,255};
static const uint8_t beta[52]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3,3,3,3,4,4,4,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18};
static const uint8_t tc[3][52]={
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,2,2,2,2,3,3,3,4,4,4,5,6,6,7,8,9,10,11,13},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,2,2,2,2,3,3,3,4,4,5,5,6,7,8,8,10,11,12,13,15,17},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,2,2,2,2,3,3,3,4,4,4,5,6,6,7,8,9,10,11,13,14,16,18,20,23,25}
};

static void filtrar(uint8_t *q,int paso,int fuerza,int ia,int ib,int croma) {
    int a=alfa[ia],b=beta[ib];
    int p0=q[-paso],p1=q[-2*paso],p2=q[-3*paso];
    int q0=q[0],q1=q[paso],q2=q[2*paso];
    if (h264_abs(p0-q0)>=a || h264_abs(p1-p0)>=b || h264_abs(q1-q0)>=b) return;
    int ap=h264_abs(p2-p0)<b,aq=h264_abs(q2-q0)<b;
    if (fuerza==4) {
        int fuerte=!croma && h264_abs(p0-q0)<(a/4+2);
        if (fuerte && ap) {
            int p3=q[-4*paso];
            q[-paso]=(uint8_t)((p2+2*p1+2*p0+2*q0+q1+4)>>3);
            q[-2*paso]=(uint8_t)((p2+p1+p0+q0+2)>>2);
            q[-3*paso]=(uint8_t)((2*p3+3*p2+p1+p0+q0+4)>>3);
        } else q[-paso]=(uint8_t)((2*p1+p0+q1+2)>>2);
        if (fuerte && aq) {
            int q3=q[3*paso];
            q[0]=(uint8_t)((p1+2*p0+2*q0+2*q1+q2+4)>>3);
            q[paso]=(uint8_t)((p0+q0+q1+q2+2)>>2);
            q[2*paso]=(uint8_t)((2*q3+3*q2+q1+q0+p0+4)>>3);
        } else q[0]=(uint8_t)((2*q1+q0+p1+2)>>2);
    } else {
        int t0=tc[fuerza-1][ia],lim=t0+(croma?1:ap+aq);
        int delta=h264_recortar((4*(q0-p0)+p1-q1+4)>>3,-lim,lim);
        q[-paso]=(uint8_t)h264_recortar(p0+delta,0,255);
        q[0]=(uint8_t)h264_recortar(q0-delta,0,255);
        if (!croma && ap) q[-2*paso]=(uint8_t)(p1+h264_recortar((p2+((p0+q0+1)>>1)-2*p1)>>1,-t0,t0));
        if (!croma && aq) q[paso]=(uint8_t)(q1+h264_recortar((q2+((p0+q0+1)>>1)-2*q1)>>1,-t0,t0));
    }
}

static int movimiento_distinto(const h264_mb *a,int ai,const h264_mb *b,int bi) {
    /* Los identificadores de referencia se normalizan al índice físico DPB. */
    int mismo=a->ref_foto[0][ai]==b->ref_foto[0][bi] && a->ref_foto[1][ai]==b->ref_foto[1][bi];
    int cruzado=a->ref_foto[0][ai]==b->ref_foto[1][bi] && a->ref_foto[1][ai]==b->ref_foto[0][bi];
    if (!mismo&&!cruzado) return 1;
    int grande[2]={0,0};
    for (int cruz=0;cruz<2;cruz++) for (int l=0;l<2;l++) {
        int r=l^cruz;
        if (a->ref[l][ai]>=0 && (h264_abs(a->mv[l][ai][0]-b->mv[r][bi][0])>=4 ||
                                  h264_abs(a->mv[l][ai][1]-b->mv[r][bi][1])>=4)) grande[cruz]=1;
    }
    return (mismo ? grande[0] : 1) && (cruzado ? grande[1] : 1);
}

void h264_desbloquear(h264_decodificador *d) {
    h264_foto *f=d->actual;
    for (unsigned idx=0;idx<f->ancho_mb*f->alto_mb;idx++) {
        h264_mb *q=&f->mb[idx];
        if (q->filtro==1) continue;
        int mx=(int)(idx%f->ancho_mb),my=(int)(idx/f->ancho_mb);
        for (int horizontal=0;horizontal<2;horizontal++) for (int borde=0;borde<4;borde++) {
            if (!borde && (horizontal ? !my : !mx)) continue;
            if ((borde&1)&&q->transformada8) continue;
            h264_mb *p=borde ? q : &f->mb[idx-(horizontal?f->ancho_mb:1)];
            if (!borde && q->filtro==2 && p->slice!=q->slice) continue;
            int qp=(p->qp+q->qp+1)/2;
            for (int segmento=0;segmento<4;segmento++) {
                int qi=horizontal?borde*4+segmento:segmento*4+borde;
                int pi=horizontal?(borde?borde-1:3)*4+segmento:segmento*4+(borde?borde-1:3);
                int fuerza=p->tipo || q->tipo ? (borde?3:4) : p->nz[pi] || q->nz[qi] ? 2 : movimiento_distinto(p,pi,q,qi);
                if (!fuerza) continue;
                for (int plano=0;plano<3;plano++) {
                    if (plano && (borde&1)) continue;
                    int esc=plano?2:1,paso=(int)f->w/esc,n=4/esc;
                    int x=mx*16/esc+(horizontal?segmento:borde)*4/esc;
                    int y=my*16/esc+(horizontal?borde:segmento)*4/esc;
                    size_t base=plano?(size_t)f->w*f->h*(3+plano)/4:0;
                    int qpc=qp;
                    if (plano) qpc=(h264_qp_croma(p->qp,d->p->qp_c[plano-1])+h264_qp_croma(q->qp,d->p->qp_c[plano-1])+1)/2;
                    int ia=h264_recortar(qpc+q->alfa,0,51),ib=h264_recortar(qpc+q->beta,0,51);
                    uint8_t *dst=f->pixeles+base+(size_t)y*paso+x;
                    for (int t=0;t<n;t++) filtrar(dst+t*(horizontal?1:paso),horizontal?paso:1,fuerza,ia,ib,plano!=0);
                }
            }
        }
    }
}
