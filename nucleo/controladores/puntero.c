#include "puntero.h"
#include "pantalla.h"
#include "consola.h"
#include "xhci.h"
#include "../base/tiempo.h"
static struct puntero_estado g;
static int iniciado;
int puntero_publicar(int32_t dx,int32_t dy,uint32_t botones,uint32_t dispositivo){
    if(dx< -32768 || dx>32767 || dy< -32768 || dy>32767 || botones>7 || !dispositivo)return -1;
    uint64_t ancho=pantalla_obtener_ancho(),alto=pantalla_obtener_alto();
    if(!ancho || !alto || ancho>INT32_MAX || alto>INT32_MAX)return -1;
    if(!iniciado){g.x=(int32_t)(ancho/2);g.y=(int32_t)(alto/2);iniciado=1;}
    int64_t x=(int64_t)g.x+dx,y=(int64_t)g.y+dy;
    g.x=x<0?0:x>=(int64_t)ancho?(int32_t)ancho-1:(int32_t)x;
    g.y=y<0?0:y>=(int64_t)alto?(int32_t)alto-1:(int32_t)y;
    if((botones&1) && !(g.botones&1)){g.clicks++;g.click_x=g.x;g.click_y=g.y;}
    g.botones=botones;g.dispositivo=dispositivo;g.secuencia++;return 0;
}
void puntero_retirar(void){g.botones=0;g.dispositivo=0;g.secuencia++;}
const struct puntero_estado *puntero_obtener_estado(void){return &g;}
static void imprimir(void){
    consola_imprimir("[PUNTERO] seq=");consola_imprimir_dec(g.secuencia);
    consola_imprimir(" x=");consola_imprimir_dec((uint32_t)g.x);consola_imprimir(" y=");consola_imprimir_dec((uint32_t)g.y);
    consola_imprimir(" botones=");consola_imprimir_dec(g.botones);consola_imprimir(" dispositivo=");consola_imprimir_dec(g.dispositivo);
    consola_imprimir(" reportes perdidos=");consola_imprimir_dec(xhci_hid_mouse_perdidos());consola_imprimir_linea("");
}
void puntero_comando(const char *arg){
    if(!arg || !*arg || (arg[0]=='e' && arg[1]=='s' && arg[2]=='t' && arg[3]=='a' && arg[4]=='d' && arg[5]=='o' && !arg[6])){imprimir();return;}
    if(arg[0]=='p' && arg[1]=='r' && arg[2]=='o' && arg[3]=='b' && arg[4]=='a' && arg[5]=='r' && !arg[6]){
        consola_imprimir_linea("[PUNTERO] Mueve el mouse y pulsa botones. q / Esc / Ctrl+C: salir.");
        uint64_t anterior=g.secuencia;
        for(;;){char c=consola_leer_caracter();if(c=='q' || c==27 || c==3)break;
            if(g.secuencia!=anterior){anterior=g.secuencia;imprimir();}esperar_milisegundos(1);}
        imprimir();return;
    }
    consola_imprimir_linea("raton [estado|probar]: requiere el modulo mouse-usb.chad cargado.");
}
