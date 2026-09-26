#include "h264.h"

/* Huella de diagnóstico FNV-1a sobre las muestras visibles en orden Y, U, V.
 * No es una función criptográfica ni sustituye la comparación exacta. */
uint64_t h264_huella(uint64_t valor,const h264_imagen *im) {
    for (int p=0;p<3;p++) {
        const uint8_t *src=p==0?im->y:p==1?im->u:im->v;
        unsigned w=p?im->ancho/2:im->ancho,h=p?im->alto/2:im->alto;
        unsigned paso=p?im->paso_c:im->paso_y;
        for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++)
            valor=(valor^src[(size_t)y*paso+x])*UINT64_C(1099511628211);
    }
    return valor;
}

static uint32_t canal(int x) { return (uint32_t)(x<0?0:x>255?255:x); }

/* Conversión entera y reducción por vecino más próximo. La matriz VUI
 * distingue BT.709, BT.601 y BT.2020 no constante. Sin VUI usamos BT.601. */
void h264_convertir_rgb(const h264_imagen *im,uint32_t *rgb,unsigned w,unsigned h) {
    if (!im || !rgb || !w || !h) return;
    int yr=im->rango_completo?256:298,offset=im->rango_completo?0:16;
    int rv=im->rango_completo?359:409,gu=im->rango_completo?88:100;
    int gv=im->rango_completo?183:208,bu=im->rango_completo?454:516;
    if (im->matriz_color==1) {
        rv=im->rango_completo?403:459;gu=im->rango_completo?48:55;
        gv=im->rango_completo?120:136;bu=im->rango_completo?475:541;
    } else if (im->matriz_color==9) {
        rv=im->rango_completo?377:430;gu=im->rango_completo?42:48;
        gv=im->rango_completo?146:167;bu=im->rango_completo?482:548;
    }
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        unsigned sx=(unsigned)((uint64_t)x*im->ancho/w),sy=(unsigned)((uint64_t)y*im->alto/h);
        int yy=yr*((int)im->y[(size_t)sy*im->paso_y+sx]-offset);
        int u=(int)im->u[(size_t)(sy/2)*im->paso_c+sx/2]-128;
        int v=(int)im->v[(size_t)(sy/2)*im->paso_c+sx/2]-128;
        rgb[(size_t)y*w+x]=canal((yy+rv*v+128)>>8)<<16 |
                          canal((yy-gu*u-gv*v+128)>>8)<<8 | canal((yy+bu*u+128)>>8);
    }
}
