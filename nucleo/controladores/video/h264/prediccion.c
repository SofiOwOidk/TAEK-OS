#include "decodificador.h"

/* Vecino disponible para sintaxis/predicción dentro de la misma slice. */
h264_mb *h264_vecino(h264_decodificador *d, int *x4, int *y4, int croma) {
    int escala = croma ? 2 : 4;
    int mx = (int)(d->mb_actual % d->s->ancho_mb);
    int my = (int)(d->mb_actual / d->s->ancho_mb);
    int x = *x4, y = *y4;
    while (x < 0) { x += escala; mx--; }
    while (y < 0) { y += escala; my--; }
    while (x >= escala) { x -= escala; mx++; }
    while (y >= escala) { y -= escala; my++; }
    if (mx < 0 || my < 0 || mx >= (int)d->s->ancho_mb || my >= (int)d->s->alto_mb) return NULL;
    unsigned indice = (unsigned)my*d->s->ancho_mb+(unsigned)mx;
    if (indice > d->mb_actual) return NULL;
    h264_mb *mb = &d->actual->mb[indice];
    if (mb->slice != d->slice_id) return NULL;
    *x4 = x; *y4 = y;
    return mb;
}

static int muestra_intra(h264_decodificador *d, int plano, int x, int y, int *valor) {
    int n = plano ? 8 : 16;
    int bx = x >= 0 ? x/4 : -1, by = y >= 0 ? y/4 : -1;
    h264_mb *mb = h264_vecino(d, &bx, &by, plano != 0);
    if (!mb || (d->p->intra_restringida && !mb->tipo)) return 0;
    if (!plano && mb == &d->actual->mb[d->mb_actual] &&
        !(mb->reconstruidos & (1u << (by*4+bx)))) return 0;
    int px = (int)(d->mb_actual%d->s->ancho_mb)*n+x;
    int py = (int)(d->mb_actual/d->s->ancho_mb)*n+y;
    unsigned paso = plano ? d->actual->w/2 : d->actual->w;
    size_t base = plano ? (size_t)d->actual->w*d->actual->h +
                          (plano-1)*(size_t)d->actual->w*d->actual->h/4 : 0;
    *valor = d->actual->pixeles[base+(size_t)py*paso+(unsigned)px];
    return 1;
}

static int ref_diagonal(const int *t, const int *l, int tl, int i) {
    return i > 0 ? t[i-1] : i < 0 ? l[-i-1] : tl;
}

int h264_predecir(h264_decodificador *d, int plano, int bx, int by, int n, int modo) {
    int t[33], l[17], tl = 128;
    int arriba = 1, izq = 1;
    for (int i = 0; i < n; i++) {
        t[i] = l[i] = 128;
        arriba &= muestra_intra(d,plano,bx+i,by-1,&t[i]);
        izq &= muestra_intra(d,plano,bx-1,by+i,&l[i]);
    }
    int esquina = muestra_intra(d,plano,bx-1,by-1,&tl);
    for (int i = n; i < 2*n; i++) {
        t[i] = t[n-1];
        muestra_intra(d,plano,bx+i,by-1,&t[i]);
    }
    t[2*n] = t[2*n-1]; l[n] = l[n-1];
    int pred16 = !plano && n == 16;
    /* Croma usa 0=DC,1=horizontal,2=vertical,3=plano. */
    if (plano) {
        if (modo == 0) modo = 2;
        else if (modo == 2) modo = 0;
    }
    if ((modo == 0 && !arriba) || (modo == 1 && !izq) ||
        ((pred16 || plano) && modo == 3 && (!arriba || !izq || !esquina)) ||
        (!pred16 && !plano && (modo == 3 || modo == 7) && !arriba) ||
        (!pred16 && !plano && (modo >= 4 && modo <= 6) && (!arriba || !izq || !esquina)) ||
        (!pred16 && !plano && modo == 8 && !izq)) return 0;
    if (!plano && n == 8) {
        int ft[17], fl[9];
        for (int i = 0; i < 16; i++)
            ft[i] = ((i ? t[i-1] : esquina ? tl : t[0])+2*t[i]+t[i+1]+2)>>2;
        for (int i = 0; i < 8; i++)
            fl[i] = ((i ? l[i-1] : esquina ? tl : l[0])+2*l[i]+l[i+1]+2)>>2;
        tl = ((izq ? l[0] : tl) + 2*tl + (arriba ? t[0] : tl)+2)>>2;
        for (int i = 0; i < 16; i++) t[i] = ft[i];
        for (int i = 0; i < 8; i++) l[i] = fl[i];
        t[16] = t[15]; l[8] = l[7];
    }
    unsigned paso = plano ? d->actual->w/2 : d->actual->w;
    size_t base = plano ? (size_t)d->actual->w*d->actual->h +
                          (plano-1)*(size_t)d->actual->w*d->actual->h/4 : 0;
    unsigned tam_mb = plano ? 8 : 16;
    uint8_t *dst = d->actual->pixeles + base +
        ((d->mb_actual/d->s->ancho_mb)*tam_mb+(unsigned)by)*paso +
        (d->mb_actual%d->s->ancho_mb)*tam_mb+(unsigned)bx;
    int suma_t = 0, suma_l = 0;
    for (int i = 0; i < n; i++) { suma_t += t[i]; suma_l += l[i]; }
    int dc = arriba && izq ? (suma_t+suma_l+n)/(2*n) : arriba ? (suma_t+n/2)/n : izq ? (suma_l+n/2)/n : 128;
    int pa = 0, pb = 0, pc = 0;
    if ((pred16 || plano) && modo == 3) {
        int h = 0, v = 0, medio = n/2-1;
        for (int i = 1; i <= n/2; i++) {
            h += i*(t[medio+i]-(medio-i < 0 ? tl : t[medio-i]));
            v += i*(l[medio+i]-(medio-i < 0 ? tl : l[medio-i]));
        }
        pa = 16*(t[n-1]+l[n-1]);
        pb = n == 16 ? (5*h+32)>>6 : (17*h+16)>>5;
        pc = n == 16 ? (5*v+32)>>6 : (17*v+16)>>5;
    }
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        int v = dc;
        if (modo == 0) v = t[x];
        else if (modo == 1) v = l[y];
        else if (modo == 2 && plano) {
            int tx = x&~3, ly = y&~3, a = 0, b = 0;
            for (int i = 0; i < 4; i++) { a += t[tx+i]; b += l[ly+i]; }
            if (arriba && izq) v = !ly && tx ? (a+2)>>2 : ly && !tx ? (b+2)>>2 : (a+b+4)>>3;
            else if (arriba) v = (a+2)>>2;
            else if (izq) v = (b+2)>>2;
            else v = 128;
        } else if (modo == 3 && (pred16 || plano)) {
            v = h264_recortar((pa+pb*(x-(n/2-1))+pc*(y-(n/2-1))+16)>>5,0,255);
        } else if (modo == 3) {
            int i = x+y;
            v = (t[i]+2*t[i+1]+t[i+2]+2)>>2;
        } else if (modo == 4) {
            int i = x-y;
            v = (ref_diagonal(t,l,tl,i-1)+2*ref_diagonal(t,l,tl,i)+ref_diagonal(t,l,tl,i+1)+2)>>2;
        } else if (modo == 5 || modo == 6) {
            const int *a = modo == 5 ? t : l, *b = modo == 5 ? l : t;
            int z = modo == 5 ? 2*x-y : 2*y-x;
            if (z < 0) v = (ref_diagonal(a,b,tl,z)+2*ref_diagonal(a,b,tl,z+1)+ref_diagonal(a,b,tl,z+2)+2)>>2;
            else if (!(z&1)) v = (ref_diagonal(a,b,tl,z/2)+ref_diagonal(a,b,tl,z/2+1)+1)>>1;
            else v = (ref_diagonal(a,b,tl,z/2)+2*ref_diagonal(a,b,tl,z/2+1)+ref_diagonal(a,b,tl,z/2+2)+2)>>2;
        } else if (modo == 7 || modo == 8) {
            int z = modo == 7 ? 2*x+y : 2*y+x;
            const int *a = modo == 7 ? t : l;
            int lim = modo == 7 ? 2*n-1 : n-1;
            int i = z/2;
            int v0 = a[h264_recortar(i,0,lim)], v1 = a[h264_recortar(i+1,0,lim)], v2 = a[h264_recortar(i+2,0,lim)];
            v = z&1 ? (v0+2*v1+v2+2)>>2 : (v0+v1+1)>>1;
        }
        dst[(size_t)y*paso+x] = (uint8_t)v;
    }
    return 1;
}

void h264_transformar4(int32_t *c, uint8_t *dst, unsigned paso) {
    int32_t t[16];
    for (int y = 0; y < 4; y++) {
        int a = c[4*y]+c[4*y+2], b = c[4*y]-c[4*y+2];
        int e = (c[4*y+1]>>1)-c[4*y+3], f = c[4*y+1]+(c[4*y+3]>>1);
        t[4*y]=a+f; t[4*y+1]=b+e; t[4*y+2]=b-e; t[4*y+3]=a-f;
    }
    for (int x = 0; x < 4; x++) {
        int a = t[x]+t[x+8], b = t[x]-t[x+8];
        int e = (t[x+4]>>1)-t[x+12], f = t[x+4]+(t[x+12]>>1);
        int v[4] = {a+f,b+e,b-e,a-f};
        for (int y = 0; y < 4; y++)
            dst[y*paso+x] = (uint8_t)h264_recortar(dst[y*paso+x]+((v[y]+32)>>6),0,255);
    }
}

static void transformada8_una(const int32_t *c, int32_t *r, int s) {
    int a0=c[0]+c[4*s], a2=c[0]-c[4*s], a4=(c[2*s]>>1)-c[6*s], a6=c[2*s]+(c[6*s]>>1);
    int a1=-c[3*s]+c[5*s]-c[7*s]-(c[7*s]>>1);
    int a3=c[s]+c[7*s]-c[3*s]-(c[3*s]>>1);
    int a5=-c[s]+c[7*s]+c[5*s]+(c[5*s]>>1);
    int a7=c[3*s]+c[5*s]+c[s]+(c[s]>>1);
    int b0=a0+a6, b2=a2+a4, b4=a2-a4, b6=a0-a6;
    int b1=a1+(a7>>2), b3=a3+(a5>>2), b5=(a3>>2)-a5, b7=a7-(a1>>2);
    r[0]=b0+b7; r[1]=b2+b5; r[2]=b4+b3; r[3]=b6+b1;
    r[4]=b6-b1; r[5]=b4-b3; r[6]=b2-b5; r[7]=b0-b7;
}

void h264_transformar8(int32_t *c, uint8_t *dst, unsigned paso) {
    int32_t t[64], r[8];
    for (int y = 0; y < 8; y++) transformada8_una(c+y*8,t+y*8,1);
    for (int x = 0; x < 8; x++) {
        transformada8_una(t+x,r,8);
        for (int y = 0; y < 8; y++)
            dst[y*paso+x]=(uint8_t)h264_recortar(dst[y*paso+x]+((r[y]+32)>>6),0,255);
    }
}

int h264_qp_croma(int qp, int offset) {
    static const uint8_t altas[22]={29,30,31,32,32,33,34,34,35,35,36,36,37,37,37,38,38,38,39,39,39,39};
    qp = h264_recortar(qp+offset,0,51);
    return qp < 30 ? qp : altas[qp-30];
}
