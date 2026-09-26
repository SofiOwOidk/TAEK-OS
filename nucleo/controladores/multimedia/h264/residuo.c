#include "decodificador.h"

static unsigned bin(h264_decodificador *d, unsigned ctx) { return h264_cabac_bin(&d->cabac,ctx); }

static int vecino_cbf(h264_decodificador *d, int cat, int bloque, int arriba) {
    h264_mb *actual = &d->actual->mb[d->mb_actual], *v;
    int x, y, plano = bloque / 4;
    if (cat == 0 || cat == 3) {
        x = arriba ? 0 : -1; y = arriba ? -1 : 0;
        v = h264_vecino(d,&x,&y,0);
        if (!v) return actual->tipo != 0;
        if (v->tipo == 3) return 1;
        if (cat == 0) return v->tipo == 2 && (v->dc&1);
        return (v->dc >> (plano+1))&1;
    }
    x = cat == 4 ? bloque%2 : bloque%4;
    y = cat == 4 ? (bloque%4)/2 : bloque/4;
    if (arriba) y--; else x--;
    v = h264_vecino(d,&x,&y,cat == 4);
    if (!v) return actual->tipo != 0;
    if (v->tipo == 3) return 1;
    return v->nz[cat == 4 ? 16+plano*4+y*2+x : y*4+x] != 0;
}

int h264_residuo(h264_decodificador *d, int32_t c[64], int cat, int bloque) {
    static const unsigned sig[6] = {105,120,134,149,152,402};
    static const unsigned last[6] = {166,181,195,210,213,417};
    static const unsigned absbase[6] = {227,237,247,257,266,426};
    /* Tabla 9-43, significant_coeff_flag para bloques frame 8x8. */
    static const uint8_t sig8[63] = {
        0,1,2,3,4,5,5,4,4,3,3,4,4,4,5,5,
        4,4,4,4,3,3,6,7,7,7,8,9,10,9,8,7,
        7,6,11,12,13,11,6,7,8,9,14,10,9,8,6,11,
        12,13,11,6,9,14,10,9,11,12,13,11,14,10,12
    };
    static const uint8_t last8[63] = {
        0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
        2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
        3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,
        5,5,5,5,6,6,6,6,7,7,7,7,8,8,8
    };
    h264_mb *m = &d->actual->mb[d->mb_actual];
    int max = cat == 3 ? 4 : cat == 5 ? 64 : cat == 1 || cat == 4 ? 15 : 16;
    h264_cero(c,64*sizeof(*c));
    if (cat != 5) {
        unsigned ctx = 85+cat*4 + vecino_cbf(d,cat,bloque,0)+2*vecino_cbf(d,cat,bloque,1);
        if (!bin(d,ctx)) return d->bits.error ? -1 : 0;
    }
    int posiciones[64], n = 0, final = 0;
    for (int i = 0; i < max-1 && !d->bits.error; i++) {
        if (bin(d,sig[cat]+(cat == 5 ? sig8[i] : (unsigned)i))) {
            posiciones[n++] = i;
            if (bin(d,last[cat]+(cat == 5 ? last8[i] : (unsigned)i))) { final = 1; break; }
        }
    }
    if (!final) posiciones[n++] = max-1;
    unsigned unos = 0, mayores = 0;
    for (int k = n-1; k >= 0 && !d->bits.error; k--) {
        unsigned ctx = mayores ? 0 : unos < 3 ? 1+unos : 4;
        unsigned magnitud = 1;
        if (bin(d,absbase[cat]+ctx)) {
            unsigned prefijo = 1;
            ctx = 5+(mayores < (cat == 3 ? 3u : 4u) ? mayores : (cat == 3 ? 3u : 4u));
            while (prefijo < 14 && bin(d,absbase[cat]+ctx)) prefijo++;
            magnitud = prefijo+1;
            if (prefijo == 14) {
                unsigned ceros = 0, sufijo = 0;
                while (!d->bits.error && h264_cabac_bypass(&d->cabac)) {
                    if (++ceros > 14) { d->bits.error = 1; break; }
                }
                for (unsigned b = 0; b < ceros; b++) sufijo = sufijo*2+h264_cabac_bypass(&d->cabac);
                magnitud += (1u<<ceros)-1+sufijo;
            }
            mayores++;
        } else unos++;
        if (magnitud > 32767) { d->bits.error = 1; break; }
        int nivel = h264_cabac_bypass(&d->cabac) ? -(int)magnitud : (int)magnitud;
        int i = posiciones[k];
        unsigned indice = cat == 3 ? (unsigned)i : cat == 5 ? h264_scan8[i] : h264_scan4[i+(cat == 1 || cat == 4)];
        c[indice] = nivel;
    }
    if (d->bits.error) return -1;
    if (cat == 0) m->dc |= 1;
    else if (cat == 3) m->dc |= (uint8_t)(1u<<(bloque/4+1));
    return n;
}

int h264_leer_mb_tipo(h264_decodificador *d) {
    int x=-1,y=0;
    h264_mb *a=h264_vecino(d,&x,&y,0);
    x=0;y=-1;
    h264_mb *b=h264_vecino(d,&x,&y,0);
    unsigned base = 3;
    if (d->sl.tipo == 0) {
        if (!bin(d,14)) {
            unsigned b1=bin(d,15);
            unsigned b2=bin(d,b1 ? 17 : 16);
            return b1 ? (b2 ? 1 : 2) : (b2 ? 3 : 0);
        }
        base=17;
    } else if (d->sl.tipo == 1) {
        unsigned inc=(a && !a->directo)+(b && !b->directo);
        if (!bin(d,27+inc)) return 0;
        if (!bin(d,30)) return 1+(int)bin(d,32);
        unsigned v=bin(d,31);
        for (int i=0;i<3;i++) v=v*2+bin(d,32);
        if (v < 8) return 3+(int)v;
        if (v == 14) return 11;
        if (v == 15) return 22;
        if (v != 13) return 12+2*(int)(v-8)+(int)bin(d,32);
        base=32;
    }
    unsigned inc=base == 3 ? (a && a->tipo != 1)+(b && b->tipo != 1) : 0;
    int prefijo=d->sl.tipo == 0 ? 5 : d->sl.tipo == 1 ? 23 : 0;
    if (!bin(d,base+inc)) return prefijo;
    if (h264_cabac_terminar(&d->cabac)) return prefijo+25;
    unsigned ctx=base+(base == 3 ? 3 : 1);
    unsigned luma=bin(d,ctx);
    unsigned croma=0;
    if (bin(d,ctx+1)) croma=1+bin(d,ctx+(base == 3 ? 2 : 1));
    unsigned pm=bin(d,ctx+(base == 3 ? 3 : 2))*2;
    pm+=bin(d,ctx+(base == 3 ? 4 : 2));
    return prefijo+1+(int)pm+4*(int)croma+12*(int)luma;
}

int h264_leer_cbp(h264_decodificador *d) {
    h264_mb *m=&d->actual->mb[d->mb_actual];
    int cbp=0;
    for (int i=0;i<4;i++) {
        int x=(i%2)*2-1,y=(i/2)*2;
        h264_mb *a=h264_vecino(d,&x,&y,0);
        int ac=a ? (a == m ? cbp : a->cbp) : 15;
        unsigned ca=a && a->tipo != 3 && !(ac&(1<<(y/2*2+x/2)));
        x=(i%2)*2;y=(i/2)*2-1;
        h264_mb *b=h264_vecino(d,&x,&y,0);
        int bc=b ? (b == m ? cbp : b->cbp) : 15;
        unsigned cb=b && b->tipo != 3 && !(bc&(1<<(y/2*2+x/2)));
        cbp|=(int)bin(d,73+ca+2*cb)<<i;
    }
    int x=-1,y=0;
    h264_mb *a=h264_vecino(d,&x,&y,0);
    x=0;y=-1;
    h264_mb *b=h264_vecino(d,&x,&y,0);
    int ca=a ? (a->tipo == 3 ? 2 : a->cbp>>4) : 0;
    int cb=b ? (b->tipo == 3 ? 2 : b->cbp>>4) : 0;
    if (bin(d,77+(ca>0)+2*(cb>0))) cbp|=(1+(int)bin(d,81+(ca==2)+2*(cb==2)))<<4;
    return cbp;
}

int h264_leer_delta_qp(h264_decodificador *d) {
    unsigned valor=0;
    if (bin(d,60+(d->sl.previo_delta != 0))) {
        valor=1;
        while (!d->bits.error && bin(d,valor == 1 ? 62 : 63)) {
            if (++valor > 102) { d->bits.error=1; break; }
        }
    }
    int delta=valor&1 ? (int)(valor+1)/2 : -(int)valor/2;
    d->sl.previo_delta=delta;
    d->sl.qp=(d->sl.qp+delta+52)%52;
    return !d->bits.error;
}

static const uint8_t escala4[6][3]={{10,13,16},{11,14,18},{13,16,20},{14,18,23},{16,20,25},{18,23,29}};
static int desquant4(int32_t *c,int qp,int inicio) {
    for (int i=inicio;i<16;i++) {
        int x=i%4,y=i/4, tipo=(x&1)+(y&1);
        int64_t v=(int64_t)c[i]*escala4[qp%6][tipo]*(1<<(qp/6));
        if (v < -(1<<20) || v > (1<<20)) return 0;
        c[i]=(int32_t)v;
    }
    return 1;
}

static void hadamard4(int32_t *c) {
    int32_t t[16];
    for (int y=0;y<4;y++) {
        int a=c[y*4]+c[y*4+2],b=c[y*4]-c[y*4+2],e=c[y*4+1]-c[y*4+3],f=c[y*4+1]+c[y*4+3];
        t[y*4]=a+f;t[y*4+1]=b+e;t[y*4+2]=b-e;t[y*4+3]=a-f;
    }
    for (int x=0;x<4;x++) {
        int a=t[x]+t[x+8],b=t[x]-t[x+8],e=t[x+4]-t[x+12],f=t[x+4]+t[x+12];
        c[x]=a+f;c[x+4]=b+e;c[x+8]=b-e;c[x+12]=a-f;
    }
}

int h264_reconstruir_residuo(h264_decodificador *d,int modo16) {
    h264_mb *m=&d->actual->mb[d->mb_actual];
    int32_t coef[24][16], dc[64], tmp[64], dc_c[2][4];
    h264_cero(coef,sizeof(coef));h264_cero(dc,sizeof(dc));h264_cero(dc_c,sizeof(dc_c));
    int qp=m->qp;
    if (m->tipo == 2) {
        if (h264_residuo(d,dc,0,0)<0) return 0;
        hadamard4(dc);
        for (int i=0;i<16;i++) {
            int64_t v=((int64_t)dc[i]*escala4[qp%6][0]*(1<<(qp/6))+2)>>2;
            if (v < -(1<<20) || v > (1<<20)) return 0;
            coef[i][0]=(int32_t)v;
        }
    }
    for (int q=0;q<4;q++) if (m->cbp&(1<<q)) {
        if (m->transformada8) {
            int cantidad=h264_residuo(d,tmp,5,0);
            if (cantidad<0) return 0;
            /* Matriz de normalización 8x8, cláusula 8.5.3.2. */
            static const uint8_t esc[6][6]={{20,18,32,19,25,24},{22,19,35,21,28,26},{26,23,42,24,33,31},{28,25,45,26,35,33},{32,28,51,30,40,38},{36,32,58,34,46,43}};
            for (int y=0;y<8;y++) for (int x=0;x<8;x++) {
                int tipo;
                if ((x&1)&&(y&1)) tipo=1;
                else if (!(x&3)&&!(y&3)) tipo=0;
                else if ((x&3)==2&&(y&3)==2) tipo=2;
                else if ((x&1)||(y&1)) tipo=((x&3)==2||(y&3)==2)?5:3;
                else tipo=4;
                int64_t v=(int64_t)tmp[y*8+x]*esc[qp%6][tipo]*(1<<(qp/6));
                v=(v+2)>>2;
                if (v < -(1<<20) || v > (1<<20)) return 0;
                ((int32_t *)coef)[q*64+y*8+x]=(int32_t)v;
            }
            for (int j=0;j<4;j++) m->nz[h264_orden4[q*4+j]]=(uint8_t)cantidad;
        } else for (int j=0;j<4;j++) {
            int bloque=h264_orden4[q*4+j];
            int cantidad=h264_residuo(d,tmp,m->tipo == 2 ? 1 : 2,bloque);
            if (cantidad<0 || !desquant4(tmp,qp,m->tipo == 2 ? 1 : 0)) return 0;
            m->nz[bloque]=(uint8_t)cantidad;
            for (int k=m->tipo == 2 ? 1 : 0;k<16;k++) coef[bloque][k]=tmp[k];
        }
    }
    if (m->cbp>>4) for (int p=0;p<2;p++) {
        if (h264_residuo(d,tmp,3,p*4)<0) return 0;
        int a=tmp[0]+tmp[1],b=tmp[0]-tmp[1],c=tmp[2]+tmp[3],e=tmp[2]-tmp[3];
        int v[4]={a+c,b+e,a-c,b-e};
        int qc=h264_qp_croma(qp,d->p->qp_c[p]);
        for (int k=0;k<4;k++) dc_c[p][k]=(v[k]*escala4[qc%6][0]*(1<<(qc/6)))>>1;
    }
    if ((m->cbp>>4)==2) for (int p=0;p<2;p++) for (int j=0;j<4;j++) {
        int cantidad=h264_residuo(d,tmp,4,p*4+j);
        int qc=h264_qp_croma(qp,d->p->qp_c[p]);
        if (cantidad<0 || !desquant4(tmp,qc,1)) return 0;
        m->nz[16+p*4+j]=(uint8_t)cantidad;
        for (int k=1;k<16;k++) coef[16+p*4+j][k]=tmp[k];
    }
    if (m->tipo==2 && !h264_predecir(d,0,0,0,16,modo16)) return 0;
    unsigned paso=d->actual->w;
    uint8_t *dst=d->actual->pixeles+(d->mb_actual/d->s->ancho_mb)*16*paso+(d->mb_actual%d->s->ancho_mb)*16;
    if (m->transformada8) for (int q=0;q<4;q++) {
        int x=(q%2)*8,y=(q/2)*8;
        if (m->tipo && !h264_predecir(d,0,x,y,8,m->modo[y+x/4])) return 0;
        h264_transformar8(((int32_t *)coef)+q*64,dst+y*paso+x,paso);
        for (int j=0;j<4;j++) m->reconstruidos|=(uint16_t)(1u<<h264_orden4[q*4+j]);
    } else for (int j=0;j<16;j++) {
        int b=h264_orden4[j], x=(b%4)*4,y=(b/4)*4;
        if (m->tipo==1 && !h264_predecir(d,0,x,y,4,m->modo[b])) return 0;
        h264_transformar4(coef[b],dst+y*paso+x,paso);
        m->reconstruidos|=(uint16_t)(1u<<b);
    }
    for (int p=0;p<2;p++) {
        if (m->tipo && !h264_predecir(d,p+1,0,0,8,m->modo_c)) return 0;
        unsigned pc=paso/2;
        uint8_t *cd=d->actual->pixeles+(size_t)paso*d->actual->h*(4+p)/4+
             (d->mb_actual/d->s->ancho_mb)*8*pc+(d->mb_actual%d->s->ancho_mb)*8;
        for (int j=0;j<4;j++) {
            coef[16+p*4+j][0]=dc_c[p][j];
            h264_transformar4(coef[16+p*4+j],cd+(j/2)*4*pc+(j%2)*4,pc);
        }
    }
    return !d->bits.error;
}

int h264_intra(h264_decodificador *d,unsigned tipo) {
    h264_mb *m=&d->actual->mb[d->mb_actual];
    m->tipo=tipo == 0 ? 1 : tipo == 25 ? 3 : 2;
    int x=-1,y=0;
    h264_mb *a=h264_vecino(d,&x,&y,0);
    x=0;y=-1;
    h264_mb *b=h264_vecino(d,&x,&y,0);
    if (tipo == 25) { d->fallo=H264_NO_SOPORTADO;d->error="I_PCM no soportado"; return 0; }
    if (!tipo) {
        if (d->p->transformada8) m->transformada8=(uint8_t)bin(d,399+(a&&a->transformada8)+(b&&b->transformada8));
        for (int k=0;k<16;k+=m->transformada8?4:1) {
            int idx=h264_orden4[k], xx=idx%4, yy=idx/4;
            x=xx-1;y=yy;
            h264_mb *iz=h264_vecino(d,&x,&y,0);
            int mi=iz && iz->tipo==1 ? iz->modo[y*4+x] : 2;
            x=xx;y=yy-1;
            h264_mb *ar=h264_vecino(d,&x,&y,0);
            int ma=ar && ar->tipo==1 ? ar->modo[y*4+x] : 2;
            int modo=(!iz||!ar) ? 2 : mi<ma ? mi:ma;
            if (!bin(d,68)) {
                int r=(int)bin(d,69);
                r+=(int)bin(d,69)*2;
                r+=(int)bin(d,69)*4;
                modo=r < modo ? r:r+1;
            }
            if (m->transformada8) for (int j=0;j<4;j++) m->modo[h264_orden4[k+j]]=(uint8_t)modo;
            else m->modo[idx]=(uint8_t)modo;
        }
    } else m->cbp=(uint8_t)(((tipo-1)/12)*15+(((tipo-1)/4)%3)*16);
    unsigned c=(a&&a->tipo&&a->modo_c)+(b&&b->tipo&&b->modo_c);
    if (bin(d,64+c)) {
        m->modo_c=1;
        while (m->modo_c<3 && bin(d,67)) m->modo_c++;
    }
    if (!tipo) m->cbp=(uint8_t)h264_leer_cbp(d);
    if (m->cbp || m->tipo==2) {
        if (!h264_leer_delta_qp(d)) return 0;
    } else d->sl.previo_delta=0;
    m->qp=(uint8_t)d->sl.qp;
    return h264_reconstruir_residuo(d,tipo ? (int)(tipo-1)%4 : 0);
}
