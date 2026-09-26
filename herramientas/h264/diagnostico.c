/* Arnés de escritorio: únicamente este archivo usa libc. Se enlaza exactamente
 * el mismo decodificador freestanding que usará el núcleo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "controladores/video/h264/decodificador.h"

typedef struct { FILE *salida; unsigned cuadros; uint64_t huella; } prueba;
static void *asignar(void *u,size_t n) { (void)u;return malloc(n); }
static void liberar(void *u,void *p) { (void)u;free(p); }
static int presentar(void *u,const h264_imagen *im) {
    prueba *p=u;
    p->huella=h264_huella(p->huella,im);
    for (int plano=0;plano<3;plano++) {
        unsigned w=plano?im->ancho/2:im->ancho,h=plano?im->alto/2:im->alto;
        unsigned paso=plano?im->paso_c:im->paso_y;
        const uint8_t *src=plano==0?im->y:plano==1?im->u:im->v;
        for (unsigned y=0;y<h;y++) if (fwrite(src+(size_t)y*paso,1,w,p->salida)!=w) return 1;
    }
    p->cuadros++;
    if (p->cuadros<=3 || p->cuadros%100==0)
        fprintf(stderr,"Fotograma %u: %ux%u POC=%d PTS=%lld\n",p->cuadros,im->ancho,im->alto,im->orden,(long long)im->marca_tiempo);
    return 0;
}
int main(int argc,char **argv) {
    if (argc!=3) { fprintf(stderr,"Uso: %s entrada.mp4 salida.yuv\n",argv[0]);return 2; }
    FILE *f=fopen(argv[1],"rb");
    if (!f || fseek(f,0,SEEK_END)) return 2;
    long largo=ftell(f);
    if (largo<=0 || fseek(f,0,SEEK_SET)) return 2;
    uint8_t *buf=malloc((size_t)largo);
    if (!buf || fread(buf,1,(size_t)largo,f)!=(size_t)largo) return 2;
    fclose(f);
    prueba p={fopen(argv[2],"wb"),0,UINT64_C(14695981039346656037)};
    if (!p.salida) return 2;
    h264_servicios servicios={&p,asignar,liberar,presentar};
    h264_decodificador *d=h264_crear(&servicios);
    if (!d) return 2;
    h264_mp4 mp4;
    int r=h264_mp4_abrir(&mp4,buf,(size_t)largo);
    if (!r) r=h264_mp4_configurar(&mp4,d);
    unsigned muestra=0;
    clock_t inicio=clock();
    while (!r) {
        const uint8_t *datos;size_t bytes;int64_t tiempo;uint32_t duracion;
        int siguiente=h264_mp4_siguiente(&mp4,&datos,&bytes,&tiempo,&duracion);
        if (siguiente<=0) { r=siguiente;break; }
        r=h264_mp4_muestra(&mp4,d,datos,bytes,tiempo);muestra++;
    }
    if (!r) r=h264_finalizar(d);
    if (r) fprintf(stderr,"ERROR %d: %s, muestra=%u MB=%u bit=%zu/%zu tipo=%u QP=%d CABAC=%u/%u\n",r,h264_error(d),muestra,d->mb_actual,d->bits.posicion,d->bits.bytes*8,d->sl.tipo,d->sl.qp,d->cabac.valor,d->cabac.rango);
    fprintf(stderr,"Total: %u fotogramas; CPU %.3f s; huella=%016llx\n",p.cuadros,(double)(clock()-inicio)/CLOCKS_PER_SEC,(unsigned long long)p.huella);
    h264_destruir(d);free(buf);fclose(p.salida);
    return r?1:0;
}
