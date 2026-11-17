#include "puntero.h"
#include "pantalla.h"
#include "consola.h"
#include "xhci.h"
#include "hid_i2c.h"
#include "../base/tiempo.h"
#include "entrada.h"   /* H210: cerrojo de la entrada (Fase B2) */
static struct puntero_estado g;
static int iniciado;
int puntero_publicar(int32_t dx,int32_t dy,uint32_t botones,uint32_t dispositivo){ ENTRADA_EXCLUSIVA();
    if(dx< -32768 || dx>32767 || dy< -32768 || dy>32767 || botones>7 || !dispositivo)return -1;
    uint64_t ancho=pantalla_obtener_ancho(),alto=pantalla_obtener_alto();
    if(!ancho || !alto || ancho>INT32_MAX || alto>INT32_MAX)return -1;
    if(!iniciado){g.x=(int32_t)(ancho/2);g.y=(int32_t)(alto/2);iniciado=1;}
    int64_t x=(int64_t)g.x+dx,y=(int64_t)g.y+dy;
    g.x=x<0?0:x>=(int64_t)ancho?(int32_t)ancho-1:(int32_t)x;
    g.y=y<0?0:y>=(int64_t)alto?(int32_t)alto-1:(int32_t)y;
    if((botones&1) && !(g.botones&1)){g.clicks++;g.click_x=g.x;g.click_y=g.y;}
    g.botones=botones;g.pulsados|=botones;g.dispositivo=dispositivo;g.secuencia++;return 0;
}
void puntero_retirar(void){ ENTRADA_EXCLUSIVA();g.botones=0;g.dispositivo=0;g.secuencia++;}
uint32_t puntero_tomar_pulsados(void){ if(!__atomic_load_n(&g.pulsados,__ATOMIC_RELAXED))return 0;   /* H211: SYS_EVENTO lo llama en cada vuelta */
    ENTRADA_EXCLUSIVA();uint32_t b=g.pulsados;g.pulsados=0;return b;}
/* Ganancia en 1/256: 1,125x con 1 cuenta por reporte, 1,5x con 4 y 2,5x desde 12 (v = la mayor de |dx| y |dy|). La
 * fraccion se guarda en *resto para que un movimiento lento no se pierda por redondeo. */
int32_t puntero_acelerar(int32_t d,int32_t v,int32_t *resto){
    if(d>12000)d=12000;
    if(d< -12000)d=-12000;
    int32_t t=d*(256+(v>12?12:v)*32)+*resto;
    int32_t r=t/256;*resto=t-r*256;return r;
}
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
        /* Si ningun modulo tiene el mouse USB, se toma aqui mismo (antes hacia falta mouse-usb.chad y sin el no se movia
         * nada). El trackpad por I2C se lee siempre que este iniciado. */
        int propio=xhci_hid_mouse_tomar()>=0;
        consola_imprimir_linea(propio?"[PUNTERO] Mouse USB tomado por esta prueba.":"[PUNTERO] Mouse USB en manos de un modulo: se lee lo que el publique.");
        consola_imprimir_linea(tactil_listo()?"[PUNTERO] Trackpad activo.":"[PUNTERO] Trackpad sin iniciar ('tactil iniciar').");
        consola_imprimir_linea("[PUNTERO] Mueve el mouse y pulsa botones. q / Esc / Ctrl+C: salir.");
        uint64_t anterior=g.secuencia;
        struct taek_hid_mouse_reporte rep;
        for(;;){char c=consola_leer_caracter();if(c=='q' || c==27 || c==3)break;
            if(propio)for(unsigned i=0;i<64;i++){
                int r=xhci_hid_mouse_leer(&rep);
                if(r<=0){if(r==-2)puntero_retirar();break;}
                if(rep.longitud<3 || rep.longitud>8 || !rep.dispositivo)continue;
                puntero_publicar((int8_t)rep.datos[1],(int8_t)rep.datos[2],rep.datos[0]&7u,rep.dispositivo);
            }
            tactil_sondear();
            if(g.secuencia!=anterior){anterior=g.secuencia;imprimir();}esperar_milisegundos(1);}
        if(propio)xhci_hid_mouse_liberar();
        imprimir();return;
    }
    if(arg[0]=='c' && arg[1]=='r' && arg[2]=='u' && arg[3]=='d' && arg[4]=='o'){
        /* Sin modulo: cada transferencia de cada endpoint HID, tal como llega, durante N segundos (20 por omision) */
        const char *p=arg+5;while(*p==' ')p++;
        uint64_t s=0;while(*p>='0' && *p<='9')s=s*10+(uint64_t)(*p++-'0');
        if(!s || s>120)s=20;
        xhci_hid_listar();
        consola_imprimir("[CRUDO] Mueve el mouse y pulsa sus botones durante ");consola_imprimir_dec(s);consola_imprimir_linea(" s. q / Esc / Ctrl+C: salir.");
        xhci_hid_crudo_activar(1);
        uint64_t t0=tiempo_obtener_milisegundos(),vistos=0;
        struct xhci_hid_crudo c;
        /* Resumen por dispositivo aunque se deje de imprimir: transferencias y si hubo botones */
        static uint32_t por_slot[9][32];static uint8_t botones_vistos[9][32];
        for(unsigned a=0;a<9;a++)for(unsigned b2=0;b2<32;b2++){por_slot[a][b2]=0;botones_vistos[a][b2]=0;}
        while(tiempo_obtener_milisegundos()-t0<s*1000){
            char k=consola_leer_caracter();   /* Tambien sondea xHCI */
            if(k=='q' || k==27 || k==3)break;
            while(xhci_hid_crudo_leer(&c)){
                if(c.slot<9 && c.dci<32){por_slot[c.slot][c.dci]++;if(c.len && c.es_raton)botones_vistos[c.slot][c.dci]|=c.datos[0];}
                if(++vistos>400)continue;      /* Que la consola no se ahogue: se cuentan igual */
                consola_imprimir("[CRUDO] t=");consola_imprimir_dec(c.ms-t0);
                consola_imprimir(" slot=");consola_imprimir_dec(c.slot);consola_imprimir(" dci=");consola_imprimir_dec(c.dci);
                consola_imprimir(" if=");consola_imprimir_dec(c.iface);consola_imprimir(c.es_raton?(c.confirmado?" raton":" raton?"):" teclado");
                consola_imprimir(" cc=");consola_imprimir_dec(c.cc);consola_imprimir(" len=");consola_imprimir_dec(c.len);consola_imprimir(" :");
                static const char h[]="0123456789ABCDEF";
                for(unsigned i=0;i<c.len && i<8;i++){char b[4]={' ',h[c.datos[i]>>4],h[c.datos[i]&15],0};consola_imprimir(b);}
                consola_imprimir_linea("");
            }
            esperar_milisegundos(1);
        }
        xhci_hid_crudo_activar(0);
        consola_imprimir("[CRUDO] FIN transferencias=");consola_imprimir_dec(vistos);
        consola_imprimir(" perdidas=");consola_imprimir_dec(xhci_hid_crudo_perdidos());consola_imprimir_linea("");
        for(unsigned a=0;a<9;a++)for(unsigned b2=0;b2<32;b2++)if(por_slot[a][b2]){
            consola_imprimir("[CRUDO] resumen slot=");consola_imprimir_dec(a);consola_imprimir(" dci=");consola_imprimir_dec(b2);
            consola_imprimir(" transferencias=");consola_imprimir_dec(por_slot[a][b2]);
            consola_imprimir(" bits del primer byte vistos=");consola_imprimir_hex(botones_vistos[a][b2]);consola_imprimir_linea("");
        }
        imprimir();return;
    }
    consola_imprimir_linea("raton [estado|probar|crudo [segundos]]: probar requiere el modulo mouse-usb.chad; crudo no.");
}
