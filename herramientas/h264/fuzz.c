/* Arnés de host para libFuzzer + ASan + UBSan. No forma parte del kernel.
 * Entrada: 0 + MP4, o 1 + registros [longitud BE32, NAL]. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include "controladores/video/h264/h264.h"

typedef struct { size_t usada; unsigned cuadros; } estado;
typedef union { max_align_t alineacion; size_t bytes; } cabecera;
static void *reservar(void *u,size_t n) {
    estado *e=u;
    if (n>64u*1024*1024-e->usada) return NULL;
    cabecera *p=malloc(sizeof(*p)+n);
    if (!p) return NULL;
    p->bytes=n;e->usada+=n;
    return p+1;
}
static void soltar(void *u,void *p) {
    if (!p) return;
    estado *e=u;cabecera *c=(cabecera *)p-1;
    if (c->bytes>e->usada) abort();
    e->usada-=c->bytes;free(c);
}
static int presentar(void *u,const h264_imagen *im) {
    estado *e=u;
    if (!im->ancho || !im->alto || im->ancho>4096 || im->alto>4096 ||
        im->paso_y<im->ancho || im->paso_c<im->ancho/2) abort();
    /* Fuerza lecturas de las esquinas y de ambos planos de croma. */
    volatile uint8_t v=im->y[(size_t)(im->alto-1)*im->paso_y+im->ancho-1];
    v^=im->u[(size_t)(im->alto/2-1)*im->paso_c+im->ancho/2-1];
    v^=im->v[(size_t)(im->alto/2-1)*im->paso_c+im->ancho/2-1];
    (void)v;
    return ++e->cuadros>=16;
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
}
int LLVMFuzzerTestOneInput(const uint8_t *datos,size_t n) {
    if (!n || n>1024*1024) return 0;
    unsigned modo=*datos++;n--;
    estado e={0,0};h264_servicios s={&e,reservar,soltar,presentar};
    h264_decodificador *d=h264_crear(&s);
    if (!d) abort();
    int r=0;
    if (!(modo&1)) {
        h264_mp4 m;
        r=h264_mp4_abrir(&m,datos,n);
        if (!r) r=h264_mp4_configurar(&m,d);
        for (unsigned i=0;!r && i<32;i++) {
            const uint8_t *p;size_t bytes;int64_t pts;uint32_t dur;
            int siguiente=h264_mp4_siguiente(&m,&p,&bytes,&pts,&dur);
            if (siguiente<=0) { r=siguiente;break; }
            r=h264_mp4_muestra(&m,d,p,bytes,pts);
        }
    } else {
        unsigned i=0;
        while (!r && n>=4 && i<32) {
            uint32_t bytes=be32(datos);datos+=4;n-=4;
            if (!bytes || bytes>n) break;
            r=h264_nal(d,datos,bytes,i++);datos+=bytes;n-=bytes;
        }
    }
    if (!r) h264_finalizar(d);
    h264_destruir(d);
    if (e.usada) abort();
    return 0;
}
