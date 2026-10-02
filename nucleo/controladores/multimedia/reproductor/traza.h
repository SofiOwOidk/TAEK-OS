#ifndef TAEK_TRAZA_VIDEO_H
#define TAEK_TRAZA_VIDEO_H
#include <stdint.h>
#define TRAZA_CPU 4
#define TRAZA_EVENTOS 512
enum {TR_DEMUX=1,TR_LECTURA,TR_DECODIFICAR,TR_RECONSTRUIR,TR_DEPENDENCIA,
      TR_DESBLOQUEO,TR_CONVERSION,TR_LISTO,TR_FRAMEBUFFER,TR_SERVICIO,TR_AAC,TR_PTS,TR_DESCARTADO};
typedef struct {
    uint64_t inicio,fin,detalle;
    int64_t pts;
    unsigned cuadro,etapa,audio,video;
} traza_evento;
typedef struct {
    /* Cada CPU escribe únicamente su anillo. Se lee después de unir los AP. */
    uint64_t escritos[TRAZA_CPU];
    traza_evento eventos[TRAZA_CPU][TRAZA_EVENTOS];
    traza_evento maximos[TRAZA_CPU][14];
} traza_video;
static inline void traza_registrar(traza_video *t,unsigned cpu,unsigned etapa,
    unsigned cuadro,int64_t pts,uint64_t inicio,uint64_t fin,uint64_t detalle,unsigned audio,unsigned video) {
    if(!t || cpu>=TRAZA_CPU)return;
    uint64_t n=t->escritos[cpu]++;
    t->eventos[cpu][n%TRAZA_EVENTOS]=(traza_evento){inicio,fin,detalle,pts,cuadro,etapa,audio,video};
    if(etapa<14 && fin-inicio>t->maximos[cpu][etapa].fin-t->maximos[cpu][etapa].inicio)
        t->maximos[cpu][etapa]=t->eventos[cpu][n%TRAZA_EVENTOS];
}
#endif
