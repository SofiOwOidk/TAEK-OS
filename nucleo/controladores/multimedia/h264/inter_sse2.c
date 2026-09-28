#include "inter_kernels.h"
#include <emmintrin.h>

/* Cargas/stores pequeños sin sobrelectura, también válidos para padding
 * mínimo, buffers no alineados y filas de croma de dos píxeles. */
static __m128i cargar(const uint8_t *p,int n) {
    if(n==8)return _mm_loadl_epi64((const __m128i *)p);
    uint32_t v=0;
    if(n==4)__builtin_memcpy(&v,p,4);
    else {uint16_t s;__builtin_memcpy(&s,p,2);v=s;}
    return _mm_cvtsi32_si128((int)v);
}
static void guardar(uint8_t *p,__m128i v,int n) {
    if(n==8)_mm_storel_epi64((__m128i *)p,v);
    else {uint32_t s=(uint32_t)_mm_cvtsi128_si32(v);__builtin_memcpy(p,&s,(unsigned)n);}
}
static __m128i palabras(const uint8_t *p,int n) {
    return _mm_unpacklo_epi8(cargar(p,n),_mm_setzero_si128());
}
static __m128i filtro6(const uint8_t *q,int salto,int n) {
    __m128i extremos=_mm_add_epi16(palabras(q-2*salto,n),palabras(q+3*salto,n));
    __m128i vecinos=_mm_add_epi16(palabras(q-salto,n),palabras(q+2*salto,n));
    __m128i centros=_mm_add_epi16(palabras(q,n),palabras(q+salto,n));
    return _mm_sub_epi16(_mm_add_epi16(extremos,_mm_mullo_epi16(centros,_mm_set1_epi16(20))),
                          _mm_mullo_epi16(vecinos,_mm_set1_epi16(5)));
}
static __m128i medio(__m128i v) {
    v=_mm_srai_epi16(_mm_add_epi16(v,_mm_set1_epi16(16)),5);
    return _mm_packus_epi16(v,_mm_setzero_si128());
}
static __m128i ampliar(__m128i v,int alto) {
    __m128i signo=_mm_srai_epi16(v,15);
    return alto?_mm_unpackhi_epi16(v,signo):_mm_unpacklo_epi16(v,signo);
}
static __m128i por20(__m128i v) {
    return _mm_add_epi32(_mm_slli_epi32(v,4),_mm_slli_epi32(v,2));
}
static __m128i por5(__m128i v) {
    return _mm_add_epi32(_mm_slli_epi32(v,2),v);
}
static __m128i diagonal(const int16_t filas[21][16],int y,int x) {
    __m128i f[6],res[2];
    for(int k=0;k<6;k++)f[k]=_mm_loadu_si128((const __m128i *)&filas[y+k][x]);
    for(int alto=0;alto<2;alto++) {
        __m128i extremos=_mm_add_epi32(ampliar(f[0],alto),ampliar(f[5],alto));
        __m128i vecinos=_mm_add_epi32(ampliar(f[1],alto),ampliar(f[4],alto));
        __m128i centros=_mm_add_epi32(ampliar(f[2],alto),ampliar(f[3],alto));
        res[alto]=_mm_srai_epi32(_mm_add_epi32(_mm_sub_epi32(_mm_add_epi32(extremos,por20(centros)),por5(vecinos)),_mm_set1_epi32(512)),10);
    }
    return _mm_packus_epi16(_mm_packs_epi32(res[0],res[1]),_mm_setzero_si128());
}

static inline __attribute__((always_inline)) int pred_vector(const uint8_t *src,int paso,int w,int h,
    int dx,int dy,int croma,uint8_t *salida,int n) {
    if(!dx&&!dy) {
        for(int y=0;y<h;y++)for(int x=0;x<w;x+=n)guardar(salida+y*w+x,cargar(src+y*paso+x,n),n);
        return 1;
    }
    if(croma) {
        __m128i wa=_mm_set1_epi16((short)((8-dx)*(8-dy))),wb=_mm_set1_epi16((short)(dx*(8-dy)));
        __m128i wc=_mm_set1_epi16((short)((8-dx)*dy)),wd=_mm_set1_epi16((short)(dx*dy));
        for(int y=0;y<h;y++)for(int x=0;x<w;x+=n) {
            const uint8_t *q=src+y*paso+x;
            __m128i a=palabras(q,n),b=dx?palabras(q+1,n):a,c=dy?palabras(q+paso,n):a;
            __m128i d=dx&&dy?palabras(q+paso+1,n):a;
            __m128i v=_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(a,wa),_mm_mullo_epi16(b,wb)),
                                    _mm_add_epi16(_mm_mullo_epi16(c,wc),_mm_mullo_epi16(d,wd)));
            v=_mm_srli_epi16(_mm_add_epi16(v,_mm_set1_epi16(32)),6);
            guardar(salida+y*w+x,_mm_packus_epi16(v,_mm_setzero_si128()),n);
        }
        return 1;
    }
    if(!dy||!dx) {
        int frac=dx?dx:dy,salto=dx?1:paso;
        for(int y=0;y<h;y++)for(int x=0;x<w;x+=n) {
            const uint8_t *q=src+y*paso+x;
            __m128i v=medio(filtro6(q,salto,n));
            if(frac!=2)v=_mm_avg_epu8(v,cargar(q+(frac==3?salto:0),n));
            guardar(salida+y*w+x,v,n);
        }
        return 1;
    }
    int16_t filas[21][16];
    if(dx==2||dy==2)for(int y=0;y<h+5;y++)for(int x=0;x<w;x+=n)
        _mm_storeu_si128((__m128i *)&filas[y][x],filtro6(src+(y-2)*paso+x,1,n));
    for(int y=0;y<h;y++)for(int x=0;x<w;x+=n) {
        const uint8_t *q=src+y*paso+x;
        __m128i v;
        if(dx==2||dy==2) {
            v=diagonal(filas,y,x);
            if(dx==2&&dy!=2)v=_mm_avg_epu8(v,medio(_mm_loadu_si128((const __m128i *)&filas[y+2+(dy==3)][x])));
            else if(dy==2&&dx!=2)v=_mm_avg_epu8(v,medio(filtro6(q+(dx==3),paso,n)));
        } else v=_mm_avg_epu8(medio(filtro6(q+(dy==3?paso:0),1,n)),medio(filtro6(q+(dx==3),paso,n)));
        guardar(salida+y*w+x,v,n);
    }
    return 1;
}

/* Especializar el ancho de carga al despachar: evita seis comprobaciones de
 * tamaño por cada filtro de seis taps. No cambia sus márgenes de lectura. */
int h264_pred_sse2(const uint8_t *src,int paso,int w,int h,int dx,int dy,int croma,uint8_t *salida) {
    if(h<1||h>16)return 0;
    if(w==2)return pred_vector(src,paso,w,h,dx,dy,croma,salida,2);
    if(w==4)return pred_vector(src,paso,w,h,dx,dy,croma,salida,4);
    if(w==8||w==16)return pred_vector(src,paso,w,h,dx,dy,croma,salida,8);
    return 0;
}

static inline __attribute__((always_inline)) int combinar_vector(uint8_t *dst,int paso,int w,int h,
    const uint8_t *a,const uint8_t *b,int modo,int peso0,int peso1,int den,int offset,int n) {
    if(modo<2) {
        for(int y=0;y<h;y++)for(int x=0;x<w;x+=n) {
            __m128i v=cargar(a+y*w+x,n);
            if(modo)v=_mm_avg_epu8(v,cargar(b+y*w+x,n));
            guardar(dst+y*paso+x,v,n);
        }
        return 1;
    }
    /* PMADDWD acumula pesos con signo en int32. Ninguna saturación previa
     * al shift o al offset, incluso con bipred explícita -128/+127. */
    int doble=modo!=2;
    int wa=modo==4?64-peso1:peso0,wb=modo==4?peso1:doble?peso1:0;
    int shift=modo==4?6:den+(doble?1:0);
    int redondeo=modo==4?32:doble?1<<den:den?1<<(den-1):0;
    if(modo==4)offset=0;
    __m128i pesos=_mm_set1_epi32((int)((uint32_t)(uint16_t)wa|((uint32_t)(uint16_t)wb<<16)));
    __m128i cuenta=_mm_cvtsi32_si128(shift);
    for(int y=0;y<h;y++)for(int x=0;x<w;x+=n) {
        __m128i aa=palabras(a+y*w+x,n),bb=doble?palabras(b+y*w+x,n):_mm_setzero_si128();
        __m128i lo=_mm_madd_epi16(_mm_unpacklo_epi16(aa,bb),pesos);
        __m128i hi=_mm_madd_epi16(_mm_unpackhi_epi16(aa,bb),pesos);
        lo=_mm_add_epi32(_mm_sra_epi32(_mm_add_epi32(lo,_mm_set1_epi32(redondeo)),cuenta),_mm_set1_epi32(offset));
        hi=_mm_add_epi32(_mm_sra_epi32(_mm_add_epi32(hi,_mm_set1_epi32(redondeo)),cuenta),_mm_set1_epi32(offset));
        guardar(dst+y*paso+x,_mm_packus_epi16(_mm_packs_epi32(lo,hi),_mm_setzero_si128()),n);
    }
    return 1;
}

int h264_combinar_sse2(uint8_t *dst,int paso,int w,int h,const uint8_t *a,const uint8_t *b,
                       int modo,int peso0,int peso1,int den,int offset) {
    if(h<1||h>16)return 0;
    if(w==2)return combinar_vector(dst,paso,w,h,a,b,modo,peso0,peso1,den,offset,2);
    if(w==4)return combinar_vector(dst,paso,w,h,a,b,modo,peso0,peso1,den,offset,4);
    if(w==8||w==16)return combinar_vector(dst,paso,w,h,a,b,modo,peso0,peso1,den,offset,8);
    return 0;
}
