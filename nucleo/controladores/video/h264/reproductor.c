#include "reproductor.h"
#include "h264.h"
#include "../../../../boot/limine/limine.h"
#include "../../../base/memoria.h"
#include "../../../base/tiempo.h"
#include "../../../arquitectura/x86_64/serial.h"
#include "../../consola.h"
#include "../../pantalla.h"
#include "../../teclado.h"
#include "../../xhci.h"
#include "../../audio_ac97.h"

__attribute__((used,section(".requests")))
static volatile struct limine_module_request peticion_video={.id=LIMINE_MODULE_REQUEST,.revision=0};

typedef struct {
    unsigned cuadros,ancho,alto,escala;
    int prueba,cancelado,error;
    int64_t primer_pts;
    uint64_t inicio,huella;
    size_t memoria_usada,memoria_maxima;
    uint32_t *rgb;
} reproductor;

typedef struct { size_t bytes; uint64_t reservado; } cabecera_reserva;

static int iguales(const char *a,const char *b) {
    if (!a || !b) return 0;
    while (*a && *a==*b) { a++;b++; }
    return *a==*b;
}
static void *reservar(void *usuario,size_t bytes) {
    reproductor *p=usuario;
    if (bytes>256u*1024*1024-p->memoria_usada) return NULL;
    cabecera_reserva *c=asignar_memoria(sizeof(*c)+bytes);
    if (!c) return NULL;
    c->bytes=bytes;c->reservado=0;p->memoria_usada+=bytes;
    if (p->memoria_usada>p->memoria_maxima) p->memoria_maxima=p->memoria_usada;
    return c+1;
}
static void soltar(void *usuario,void *memoria) {
    if (!memoria) return;
    reproductor *p=usuario;cabecera_reserva *c=(cabecera_reserva *)memoria-1;
    p->memoria_usada-=c->bytes;liberar_memoria(c);
}
static int atender(reproductor *p) {
    xhci_sondeo();audio_ac97_actualizar();
    while (teclado_hay_datos()) if (teclado_leer_caracter()==27) p->cancelado=1;
    return p->cancelado;
}

static int presentar(void *usuario,const h264_imagen *im) {
    reproductor *p=usuario;
    if (atender(p)) return 1;
    if (!p->cuadros) { p->primer_pts=im->marca_tiempo;p->inicio=tiempo_obtener_milisegundos(); }
    if (!p->prueba) {
        int64_t delta=im->marca_tiempo-p->primer_pts;
        /* Evita una espera ilimitada ante marcas de tiempo absurdas. */
        if (delta<0 || (uint64_t)delta/p->escala>86400) { p->error=1;return 1; }
        uint64_t destino=p->inicio+(uint64_t)delta*1000/p->escala;
        while (tiempo_obtener_milisegundos()<destino) {
            if (atender(p)) return 1;
            esperar_milisegundos(1);
        }
    }
    if (p->prueba) p->huella=h264_huella(p->huella,im);
    if (!p->prueba || p->cuadros%100==0) {
        unsigned w=im->ancho,h=im->alto;
        uint64_t sw=pantalla_obtener_ancho(),sh=pantalla_obtener_alto();
        if (!sw || !sh) { p->error=1;return 1; }
        if (w>sw) { h=(unsigned)((uint64_t)h*sw/w);w=(unsigned)sw; }
        if (h>sh) { w=(unsigned)((uint64_t)w*sh/h);h=(unsigned)sh; }
        if (!w || !h) { p->error=1;return 1; }
        if (p->ancho!=w || p->alto!=h) {
            if (p->rgb) soltar(p,p->rgb);
            p->rgb=reservar(p,(size_t)w*h*4);p->ancho=w;p->alto=h;
            if (!p->rgb) { p->error=1;return 1; }
            pantalla_limpiar(0);
        }
        h264_convertir_rgb(im,p->rgb,w,h);
        pantalla_dibujar_imagen_centrada((int)w,(int)h,p->rgb);
    }
    p->cuadros++;
    if (p->cuadros==1 || p->cuadros%100==0) {
        serial_imprimir("[H264] Fotogramas: ");serial_imprimir_dec(p->cuadros);
        serial_imprimir(" PTS: ");serial_imprimir_dec((uint64_t)im->marca_tiempo);serial_imprimir_linea("");
    }
    return 0;
}

static void reproducir(const char *nombre,int prueba) {
    struct limine_file *archivo=NULL;
    struct limine_module_response *r=peticion_video.response;
    if (r) for (uint64_t i=0;i<r->module_count;i++) {
        if (iguales(r->modules[i]->cmdline,nombre)) { archivo=r->modules[i];break; }
    }
    if (!archivo) {
        consola_imprimir_linea("No está cargado ese módulo MP4. Usa la ISO generada con make h264-iso.");
        serial_imprimir_linea("[H264] ERROR: módulo MP4 ausente");return;
    }
    h264_mp4 mp4;
    h264_resultado resultado=h264_mp4_abrir(&mp4,archivo->address,(size_t)archivo->size);
    if (resultado) { consola_imprimir_linea("MP4 inválido o contenedor no soportado.");serial_imprimir_linea("[H264] ERROR MP4");return; }
    reproductor p={.prueba=prueba,.escala=mp4.escala_tiempo,.huella=UINT64_C(14695981039346656037)};
    h264_servicios servicios={&p,reservar,soltar,presentar};
    memoria_estadisticas_t antes,despues;
    memoria_obtener_estadisticas(&antes);
    h264_decodificador *dec=h264_crear(&servicios);
    if (!dec) { consola_imprimir_linea("Memoria insuficiente para H.264.");return; }
    consola_imprimir_linea("H.264 experimental por CPU. ESC vuelve a la terminal. Video sin audio.");
    serial_imprimir("[H264] INICIO ");serial_imprimir_linea(nombre);
    resultado=h264_mp4_configurar(&mp4,dec);
    while (!resultado) {
        const uint8_t *datos;size_t bytes;int64_t pts;uint32_t duracion;
        if (atender(&p)) { resultado=H264_CANCELADO;break; }
        int siguiente=h264_mp4_siguiente(&mp4,&datos,&bytes,&pts,&duracion);
        if (siguiente<=0) { resultado=(h264_resultado)siguiente;break; }
        resultado=h264_mp4_muestra(&mp4,dec,datos,bytes,pts);
    }
    if (!resultado) resultado=h264_finalizar(dec);
    consola_limpiar();
    consola_imprimir(p.cancelado?"Reproducción cancelada. ":resultado||p.error?"Error de reproducción: ":"Reproducción terminada. ");
    if (resultado && !p.cancelado) consola_imprimir(h264_error(dec));
    consola_imprimir(" Fotogramas: ");consola_imprimir_dec(p.cuadros);consola_imprimir_linea("");
    serial_imprimir(resultado||p.error?"[H264] ERROR/CANCELADO ":"[H264] FIN OK ");
    serial_imprimir_dec(p.cuadros);serial_imprimir(" huella=");serial_imprimir_hex(p.huella);
    serial_imprimir(" tiempo_ms=");serial_imprimir_dec(tiempo_obtener_milisegundos()-p.inicio);serial_imprimir_linea("");
    if (resultado) serial_imprimir_linea(h264_error(dec));
    h264_destruir(dec);
    if (p.rgb) soltar(&p,p.rgb);
    memoria_obtener_estadisticas(&despues);
    serial_imprimir("[H264] HEAP antes=");serial_imprimir_dec(antes.heap_bytes_en_uso);
    serial_imprimir(" después=");serial_imprimir_dec(despues.heap_bytes_en_uso);
    serial_imprimir(" canarios=");serial_imprimir_dec(memoria_verificar_integridad());serial_imprimir_linea("");
    serial_imprimir("[H264] MEMORIA máximo=");serial_imprimir_dec(p.memoria_maxima);
    serial_imprimir(" pendiente=");serial_imprimir_dec(p.memoria_usada);serial_imprimir_linea("");
}

void video_h264_comando(const char *arg) {
    if (iguales(arg,"360p")) reproducir("h264:360p",0);
    else if (iguales(arg,"1080p")) reproducir("h264:1080p",0);
    else if (iguales(arg,"prueba 360p")) reproducir("h264:360p",1);
    else if (iguales(arg,"prueba 1080p")) reproducir("h264:1080p",1);
    else {
        consola_imprimir_linea("h264 360p | h264 1080p : reproducir MP4 nativo (ESC para salir).");
        consola_imprimir_linea("h264 prueba 360p | h264 prueba 1080p : decodificar y calcular huella sin espera.");
        consola_imprimir_linea("Experimental: CABAC, 8 bits, YUV420 progresivo. Requiere módulos de make h264-iso.");
    }
}

void video_h264_arranque(const char *cmdline) {
    if (!cmdline) return;
    /* Token exacto, sin activar la prueba por coincidencias parciales. */
    while (*cmdline) {
        while (*cmdline==' ') cmdline++;
        char token[32];unsigned n=0;
        while (*cmdline && *cmdline!=' ') {
            if (n+1<sizeof(token)) token[n++]=*cmdline;
            cmdline++;
        }
        token[n]=0;
        if (iguales(token,"h264=prueba360")) reproducir("h264:360p",1);
        if (iguales(token,"h264=prueba1080")) reproducir("h264:1080p",1);
    }
}
