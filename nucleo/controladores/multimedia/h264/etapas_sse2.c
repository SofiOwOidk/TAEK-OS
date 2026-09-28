#include "etapas_sse2.h"
#include <emmintrin.h>
typedef __m128i V;
#define A _mm_add_epi32
#define S _mm_sub_epi32
#define R(x,n) _mm_srai_epi32(x,n)
static void t4(V *v,int had) {
    V a=A(v[0],v[2]),b=S(v[0],v[2]);
    V e=had?S(v[1],v[3]):S(R(v[1],1),v[3]);
    V f=had?A(v[1],v[3]):A(v[1],R(v[3],1));
    v[0]=A(a,f);v[1]=A(b,e);v[2]=S(b,e);v[3]=S(a,f);
}
static void t8(V *v) {
    V a0=A(v[0],v[4]),a2=S(v[0],v[4]),a4=S(R(v[2],1),v[6]),a6=A(v[2],R(v[6],1));
    V a1=S(S(S(v[5],v[3]),v[7]),R(v[7],1));
    V a3=S(S(A(v[1],v[7]),v[3]),R(v[3],1));
    V a5=A(A(S(v[7],v[1]),v[5]),R(v[5],1));
    V a7=A(A(A(v[3],v[5]),v[1]),R(v[1],1));
    V b0=A(a0,a6),b2=A(a2,a4),b4=S(a2,a4),b6=S(a0,a6);
    V b1=A(a1,R(a7,2)),b3=A(a3,R(a5,2)),b5=S(R(a3,2),a5),b7=S(a7,R(a1,2));
    v[0]=A(b0,b7);v[1]=A(b2,b5);v[2]=A(b4,b3);v[3]=A(b6,b1);
    v[4]=S(b6,b1);v[5]=S(b4,b3);v[6]=S(b2,b5);v[7]=S(b0,b7);
}
static void transformar(const int32_t *c,int32_t *r,unsigned n,int had) {
    int32_t t[64];V v[8];
    for(unsigned y=0;y<n;y+=4) {
        for(unsigned x=0;x<n;x+=4) {
            V a=_mm_loadu_si128((const V *)(c+y*n+x)),b=_mm_loadu_si128((const V *)(c+(y+1)*n+x));
            V e=_mm_loadu_si128((const V *)(c+(y+2)*n+x)),f=_mm_loadu_si128((const V *)(c+(y+3)*n+x));
            V ab0=_mm_unpacklo_epi32(a,b),ab1=_mm_unpackhi_epi32(a,b),ef0=_mm_unpacklo_epi32(e,f),ef1=_mm_unpackhi_epi32(e,f);
            v[x]=_mm_unpacklo_epi64(ab0,ef0);v[x+1]=_mm_unpackhi_epi64(ab0,ef0);
            v[x+2]=_mm_unpacklo_epi64(ab1,ef1);v[x+3]=_mm_unpackhi_epi64(ab1,ef1);
        }
        if(n==4)t4(v,had);else t8(v);
        for(unsigned x=0;x<n;x+=4) {
            V ab0=_mm_unpacklo_epi32(v[x],v[x+1]),ab1=_mm_unpackhi_epi32(v[x],v[x+1]);
            V ef0=_mm_unpacklo_epi32(v[x+2],v[x+3]),ef1=_mm_unpackhi_epi32(v[x+2],v[x+3]);
            _mm_storeu_si128((V *)(t+y*n+x),_mm_unpacklo_epi64(ab0,ef0));
            _mm_storeu_si128((V *)(t+(y+1)*n+x),_mm_unpackhi_epi64(ab0,ef0));
            _mm_storeu_si128((V *)(t+(y+2)*n+x),_mm_unpacklo_epi64(ab1,ef1));
            _mm_storeu_si128((V *)(t+(y+3)*n+x),_mm_unpackhi_epi64(ab1,ef1));
        }
    }
    for(unsigned x=0;x<n;x+=4) {
        for(unsigned y=0;y<n;y++)v[y]=_mm_loadu_si128((const V *)(t+y*n+x));
        if(n==4)t4(v,had);else t8(v);
        for(unsigned y=0;y<n;y++)_mm_storeu_si128((V *)(r+y*n+x),v[y]);
    }
}
void h264_transformada_sse2(const int32_t *c,int32_t *r,unsigned n){transformar(c,r,n,0);}
void h264_hadamard_sse2(int32_t *c){int32_t r[16];transformar(c,r,4,1);for(unsigned i=0;i<16;i++)c[i]=r[i];}
void h264_sumar_sse2(const int32_t *r,uint8_t *dst,unsigned paso,unsigned n) {
    V cero=_mm_setzero_si128(),red=_mm_set1_epi32(32);
    for(unsigned y=0;y<n;y++)for(unsigned x=0;x<n;x+=4) {
        uint32_t p;__builtin_memcpy(&p,dst+y*paso+x,4);
        V pix=_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)p),cero),cero);
        V v=A(pix,R(A(_mm_loadu_si128((const V *)(r+y*n+x)),red),6));
        v=_mm_packus_epi16(_mm_packs_epi32(v,cero),cero);p=(uint32_t)_mm_cvtsi128_si32(v);
        __builtin_memcpy(dst+y*paso+x,&p,4);
    }
}
#undef A
#undef S
#undef R
#define A _mm_add_epi16
#define S _mm_sub_epi16
#define R(x,n) _mm_srai_epi16(x,n)
#define L(x,n) _mm_slli_epi16(x,n)
static V N(int x){return _mm_set1_epi16((short)x);}
static V elegir(V m,V a,V b){return _mm_or_si128(_mm_and_si128(m,a),_mm_andnot_si128(m,b));}
static V absoluto(V x){V m=R(x,15);return S(_mm_xor_si128(x,m),m);}
static V menor(V x,int b){return _mm_cmpgt_epi16(N(b),absoluto(x));}
static V limitar(V x,V lim){return _mm_min_epi16(lim,_mm_max_epi16(S(N(0),lim),x));}
void h264_filtro_sse2(uint8_t *q,int paso,int avance,int n,int fuerza,int a,int b,int t0) {
    if(!a || !b)return;
    V p[8];short valores[8];
    for(int k=0;k<8;k++) {
        if(avance==1) {
            unsigned pix=0;__builtin_memcpy(&pix,q+(k-4)*paso,(size_t)n);
            p[k]=_mm_unpacklo_epi8(_mm_cvtsi32_si128((int)pix),N(0));
        } else {
            p[k]=_mm_setr_epi16(q[(k-4)*paso],q[(k-4)*paso+avance],n==4?q[(k-4)*paso+2*avance]:0,n==4?q[(k-4)*paso+3*avance]:0,0,0,0,0);
        }
    }
    V p3=p[0],p2=p[1],p1=p[2],p0=p[3],q0=p[4],q1=p[5],q2=p[6],q3=p[7];
    V valido=_mm_and_si128(menor(S(p0,q0),a),_mm_and_si128(menor(S(p1,p0),b),menor(S(q1,q0),b)));
    if(!((unsigned)_mm_movemask_epi8(valido)&((1u<<(2*n))-1)))return;
    int croma=n==2;
    V ap=menor(S(p2,p0),b),aq=menor(S(q2,q0),b);
    if(fuerza==4) {
        V fuerte=croma?N(0):menor(S(p0,q0),a/4+2);
        V mp=_mm_and_si128(fuerte,ap),mq=_mm_and_si128(fuerte,aq);
        p[3]=elegir(mp,R(A(A(A(A(p2,L(p1,1)),L(p0,1)),L(q0,1)),A(q1,N(4))),3),R(A(A(L(p1,1),p0),A(q1,N(2))),2));
        p[2]=elegir(mp,R(A(A(p2,p1),A(A(p0,q0),N(2))),2),p1);
        p[1]=elegir(mp,R(A(A(L(p3,1),A(L(p2,1),p2)),A(A(p1,p0),A(q0,N(4)))),3),p2);
        p[4]=elegir(mq,R(A(A(A(A(p1,L(p0,1)),L(q0,1)),L(q1,1)),A(q2,N(4))),3),R(A(A(L(q1,1),q0),A(p1,N(2))),2));
        p[5]=elegir(mq,R(A(A(p0,q0),A(A(q1,q2),N(2))),2),q1);
        p[6]=elegir(mq,R(A(A(L(q3,1),A(L(q2,1),q2)),A(A(q1,q0),A(p0,N(4)))),3),q2);
    } else {
        V lim=A(N(t0),croma?N(1):A(_mm_and_si128(ap,N(1)),_mm_and_si128(aq,N(1))));
        V delta=limitar(R(A(A(L(S(q0,p0),2),S(p1,q1)),N(4)),3),lim);
        p[3]=_mm_min_epi16(N(255),_mm_max_epi16(N(0),A(p0,delta)));
        p[4]=_mm_min_epi16(N(255),_mm_max_epi16(N(0),S(q0,delta)));
        if(!croma) {
            V media=R(A(A(p0,q0),N(1)),1);
            p[2]=elegir(ap,A(p1,limitar(R(S(A(p2,media),L(p1,1)),1),N(t0))),p1);
            p[5]=elegir(aq,A(q1,limitar(R(S(A(q2,media),L(q1,1)),1),N(t0))),q1);
        }
    }
    V original[6]={p2,p1,p0,q0,q1,q2};
    for(int k=1;k<7;k++) {
        if((k==1 || k==6) && (fuerza!=4 || croma))continue;
        if((k==2 || k==5) && croma)continue;
        V resultado=elegir(valido,p[k],original[k-1]);
        if(avance==1) {
            unsigned pix=(unsigned)_mm_cvtsi128_si32(_mm_packus_epi16(resultado,N(0)));
            __builtin_memcpy(q+(k-4)*paso,&pix,(size_t)n);
        } else {
            _mm_storeu_si128((V *)valores,resultado);
            for(int i=0;i<n;i++)q[(k-4)*paso+i*avance]=(uint8_t)valores[i];
        }
    }
}
/* PMADDWD evita multiplicación truncada de 16 bits (541*127 > 32767). */
static V producto(V a,int k){return _mm_madd_epi16(_mm_unpacklo_epi16(a,N(0)),_mm_set1_epi32(k&65535));}
static V canal(V a){V z=N(0);return _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_packus_epi16(_mm_packs_epi32(a,z),z),z),z);}
void h264_rgb_sse2(const h264_imagen *im,uint32_t *rgb,unsigned w,unsigned h,unsigned y0,unsigned y1) {
    int completo=im->rango_completo,yr=completo?256:298,off=completo?0:16;
    int rv=completo?359:409,gu=completo?88:100,gv=completo?183:208,bu=completo?454:516;
    if(im->matriz_color==1){rv=completo?403:459;gu=completo?48:55;gv=completo?120:136;bu=completo?475:541;}
    if(im->matriz_color==9){rv=completo?377:430;gu=completo?42:48;gv=completo?146:167;bu=completo?482:548;}
    for(unsigned y=y0;y<y1;y++) {
        unsigned sy=w==im->ancho && h==im->alto?y:(unsigned)((uint64_t)y*im->alto/h);
        const uint8_t *py=im->y+(size_t)sy*im->paso_y,*pu=im->u+(size_t)(sy/2)*im->paso_c,*pv=im->v+(size_t)(sy/2)*im->paso_c;
        for(unsigned x=0;x<w;x+=4) {
            short ys[8]={0},us[8]={0},vs[8]={0};unsigned n=w-x<4?w-x:4;
            for(unsigned i=0;i<n;i++) {
                unsigned sx=w==im->ancho?x+i:(unsigned)((uint64_t)(x+i)*im->ancho/w);
                ys[i]=py[sx]-off;us[i]=pu[sx/2]-128;vs[i]=pv[sx/2]-128;
            }
            V yy=producto(_mm_loadu_si128((const V *)ys),yr),u=_mm_loadu_si128((const V *)us),v=_mm_loadu_si128((const V *)vs),red=_mm_set1_epi32(128);
            V r=canal(_mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(yy,producto(v,rv)),red),8));
            V g=canal(_mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_sub_epi32(yy,producto(u,gu)),producto(v,gv)),red),8));
            V b=canal(_mm_srai_epi32(_mm_add_epi32(_mm_add_epi32(yy,producto(u,bu)),red),8));
            uint32_t salida[4];_mm_storeu_si128((V *)salida,_mm_or_si128(_mm_or_si128(_mm_slli_epi32(r,16),_mm_slli_epi32(g,8)),b));
            for(unsigned i=0;i<n;i++)rgb[(size_t)y*w+x+i]=salida[i];
        }
    }
}
