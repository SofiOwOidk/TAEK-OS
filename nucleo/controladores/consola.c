#include "consola.h"
#include "audio_ac97.h"
#include "pantalla.h"
#include "teclado.h"
#include "xhci.h"
#include "driver_ram.h"
#include "puntero.h"
#include "../base/utf8.h"
#include "../base/tiempo.h"
#include "../arquitectura/x86_64/serial.h"
#include "red/remoto.h"
#include "red/stream.h"
#include "../base/memoria.h"
#define RETENIDAS 1024
#define COLUMNAS_MAX 240
struct celda { uint32_t fg,bg; uint8_t glifo; };
static struct celda g_texto[RETENIDAS][COLUMNAS_MAX];
static uint64_t g_linea;
static unsigned g_vista,g_lote;
static int g_redibujar;
static int g_puntero_dibujado,g_puntero_x,g_puntero_y,g_puntero_sucio=1;
static uint64_t g_puntero_secuencia;
static char g_historial[32][256],g_borrador[256];
static unsigned g_hist_total,g_hist_proximo;

static int g_columnas = 160;
static int g_filas    = 50;

static int g_cursor_x = 0;
static int g_cursor_y = 0;

static uint32_t g_color_fg = COLOR_TEXTO_DEFAULT;
static uint32_t g_color_bg = COLOR_FONDO_DEFAULT;
static void nueva_linea(void){
    struct celda *p=g_texto[g_linea%RETENIDAS];
    for(int x=0;x<g_columnas;x++)p[x]=(struct celda){g_color_fg,g_color_bg,' '};
}
static void repintar(void){
    g_puntero_dibujado=0;g_puntero_sucio=1;
    uint64_t base=g_linea>=(uint64_t)g_filas?g_linea-g_filas+1:0;
    if(g_vista>base)g_vista=(unsigned)base;
    base-=g_vista;
    pantalla_limpiar(g_color_bg);
    for(int y=0;y<g_filas;y++){
        uint64_t l=base+y;if(l>g_linea)break;
        for(int x=0;x<g_columnas;x++){struct celda *p=&g_texto[l%RETENIDAS][x];pantalla_dibujar_caracter(x,y,p->glifo,p->fg,p->bg);}
    }
}
static void puntero_restaurar(int col,int fila){
    if(col<0 || col>=g_columnas || fila<0 || fila>=g_filas)return;
    uint64_t base=g_linea>=(uint64_t)g_filas?g_linea-g_filas+1:0;base-=g_vista;
    uint64_t linea=base+(unsigned)fila;
    struct celda vacia={g_color_fg,g_color_bg,' '};
    struct celda *p=linea>g_linea?&vacia:&g_texto[linea%RETENIDAS][col];
    pantalla_dibujar_caracter(col,fila,p->glifo,p->fg,p->bg);
}
void consola_obtener_posicion(int *col,uint64_t *linea){if(col)*col=g_cursor_x;if(linea)*linea=g_linea;}
uint64_t consola_linea_visible(unsigned fila){
    uint64_t base=g_linea>=(uint64_t)g_filas?g_linea-g_filas+1:0;
    return fila>=(unsigned)g_filas?UINT64_MAX:base-g_vista+fila;
}
void consola_puntero_presentar(void){
    const struct puntero_estado *p=puntero_obtener_estado();
    if(!g_puntero_sucio && p->secuencia==g_puntero_secuencia)return;
    if(g_puntero_dibujado)for(int y=g_puntero_y/16;y<=(g_puntero_y+13)/16;y++)
        for(int x=g_puntero_x/8;x<=(g_puntero_x+7)/8;x++)puntero_restaurar(x,y);
    g_puntero_dibujado=0;g_puntero_sucio=0;g_puntero_secuencia=p->secuencia;
    if(!p->dispositivo)return;
    for(int y=0;y<14;y++)for(int x=0;x<8;x++){
        int dentro=y<9?x<=y/2:(x>=2 && x<=4);
        if(dentro)pantalla_dibujar_pixel(p->x+x,p->y+y,(x==0 || (y<9 && x==y/2))?0x00000000:0x00ffffff);
    }
    g_puntero_x=p->x;g_puntero_y=p->y;g_puntero_dibujado=1;
}
void consola_lote_iniciar(void){g_lote++;}
static void presentar(void){if(g_redibujar){repintar();g_redibujar=0;}}
void consola_lote_finalizar(void){if(g_lote && !--g_lote)presentar();}
void consola_desplazar(int n){
    uint64_t base=g_linea>=(uint64_t)g_filas?g_linea-g_filas+1:0;
    uint64_t primero=g_linea>=RETENIDAS?g_linea-RETENIDAS+1:0;
    int max=(int)(base-primero);int vista=(int)g_vista+n;
    g_vista=vista<0?0:vista>max?(unsigned)max:(unsigned)vista;repintar();
}

// Estado del decodificador UTF-8 para cadenas continuas
static uint32_t g_utf8_codepunto = 0;
static int      g_utf8_restantes = 0;
/* Teclas escritas mientras el VFS trabaja: conservarlas para la terminal. */
static char g_entrada_pendiente[128];
static unsigned g_entrada_inicio,g_entrada_total;

void consola_iniciar(void) {
    uint64_t ancho = pantalla_obtener_ancho();
    uint64_t alto  = pantalla_obtener_alto();

    if (ancho > 0 && alto > 0) {
        g_columnas = (int)(ancho / 8);
        g_filas    = (int)(alto / 16);
    }

    g_cursor_x = 0;
    g_cursor_y = 0;
    g_color_fg = COLOR_TEXTO_DEFAULT;
    g_color_bg = COLOR_FONDO_DEFAULT;
    if(g_columnas>COLUMNAS_MAX)g_columnas=COLUMNAS_MAX;
    if(g_filas>RETENIDAS)g_filas=RETENIDAS;
    if(g_columnas<1)g_columnas=1;if(g_filas<1)g_filas=1;
    g_linea=0;g_vista=0;g_lote=0;nueva_linea();
}

void consola_limpiar(void) {
    g_puntero_dibujado=0;g_puntero_sucio=1;
    pantalla_limpiar(g_color_bg);
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_linea=0;g_vista=0;nueva_linea();
}

void consola_establecer_color_texto(uint32_t fg) {
    g_color_fg = fg;
}

void consola_establecer_color_fondo(uint32_t bg) {
    g_color_bg = bg;
}

uint32_t consola_obtener_color_texto(void) {
    return g_color_fg;
}

static void consola_avanzar_linea(void) {
    g_linea++;nueva_linea();
    g_cursor_x = 0;
    g_cursor_y++;
    if (g_cursor_y >= g_filas) {
        if(!g_lote && !g_vista)pantalla_desplazar_arriba(1, g_color_bg);
        g_cursor_y = g_filas - 1;
    }
}

void consola_escribir_caracter(char c) {
    g_puntero_sucio=1;
    if(g_lote)g_redibujar=1;
    red_remoto_capturar(c);
    // Espejo al puerto serial
    serial_escribir_caracter(c);

    // Caracteres de control
    if (c == '\r') {
        g_cursor_x = 0;
        return;
    }
    if (c == '\n') {
        consola_avanzar_linea();
        return;
    }
    if (c == '\b') {
        if (g_cursor_x > 0 || g_linea) {
            if(!g_cursor_x){g_linea--;g_cursor_x=g_columnas;if(g_cursor_y)g_cursor_y--;}
            g_cursor_x--;
            g_texto[g_linea%RETENIDAS][g_cursor_x]=(struct celda){g_color_fg,g_color_bg,' '};
            if(!g_lote && !g_vista)pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, ' ', g_color_fg, g_color_bg);
        }
        return;
    }
    if (c == '\t') {
        int espacios = 4 - (g_cursor_x % 4);
        for (int i = 0; i < espacios; i++) {
            consola_escribir_caracter(' ');
        }
        return;
    }

    // Decodificación de secuencias UTF-8 multibyte
    uint8_t byte = (uint8_t)c;
    uint8_t glifo = 0;

    if (g_utf8_restantes == 0) {
        if ((byte & 0x80) == 0) {
            glifo = byte;
        } else if ((byte & 0xE0) == 0xC0) {
            g_utf8_codepunto = (byte & 0x1F);
            g_utf8_restantes = 1;
            return;
        } else if ((byte & 0xF0) == 0xE0) {
            g_utf8_codepunto = (byte & 0x0F);
            g_utf8_restantes = 2;
            return;
        } else if ((byte & 0xF8) == 0xF0) {
            g_utf8_codepunto = (byte & 0x07);
            g_utf8_restantes = 3;
            return;
        } else {
            glifo = '?';
        }
    } else {
        g_utf8_codepunto = (g_utf8_codepunto << 6) | (byte & 0x3F);
        g_utf8_restantes--;
        if (g_utf8_restantes > 0) {
            return;
        }
        glifo = unicode_a_cp437(g_utf8_codepunto);
    }

    // Dibujar el caracter en el Framebuffer
    g_texto[g_linea%RETENIDAS][g_cursor_x]=(struct celda){g_color_fg,g_color_bg,glifo};
    if(!g_lote && !g_vista)pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, glifo, g_color_fg, g_color_bg);
    g_cursor_x++;

    if (g_cursor_x >= g_columnas) {
        consola_avanzar_linea();
    }
}

void consola_imprimir(const char *texto) {
    if (!texto) return;
    for (int i = 0; texto[i] != '\0'; i++) {
        consola_escribir_caracter(texto[i]);
    }
}

void consola_imprimir_color(const char *texto, uint32_t fg) {
    uint32_t anterior = g_color_fg;
    g_color_fg = fg;
    consola_imprimir(texto);
    g_color_fg = anterior;
}

void consola_imprimir_linea(const char *texto) {
    consola_imprimir(texto);
    consola_escribir_caracter('\n');
}

void consola_imprimir_linea_color(const char *texto, uint32_t fg) {
    consola_imprimir_color(texto, fg);
    consola_escribir_caracter('\n');
}

void consola_imprimir_dec(uint64_t valor) {
    if (valor == 0) {
        consola_escribir_caracter('0');
        return;
    }
    char buf[32];
    int idx = 0;
    while (valor > 0) {
        buf[idx++] = '0' + (valor % 10);
        valor /= 10;
    }
    for (int i = idx - 1; i >= 0; i--) {
        consola_escribir_caracter(buf[i]);
    }
}

void consola_imprimir_hex(uint64_t valor) {
    const char hex[] = "0123456789ABCDEF";
    consola_imprimir("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (valor >> i) & 0xF;
        consola_escribir_caracter(hex[nibble]);
    }
}

static char consola_leer_caracter_directo(void) {
    // 1. En Modo Nativo (modo=ps2), leer exclusivamente del teclado PS/2 (i8042 / EC)
    if (teclado_es_modo_nativo()) {
        char c = teclado_leer_caracter();
        if (c != 0) return c;
    } else {
        // 2. En Modo xHCI (modo=xhci), leer exclusivamente del controlador USB xHCI
        char cu = xhci_leer_caracter();
        if (cu != 0) return cu;
    }

    // 3. Revisar puerto serial COM1 (siempre disponible para telemetría y depuración)
    if (serial_hay_datos()) {
        char cs = serial_leer_caracter();
        if (cs == '\r') cs = '\n'; // Normalizar enter de terminal
        return cs;
    }

    return red_stream_tecla();
}

char consola_leer_caracter(void) {
    driver_ram_sondear();consola_puntero_presentar();
    if(g_lote)presentar();
    serial_atender();
    red_stream_atender();
    if(g_entrada_total){char c=g_entrada_pendiente[g_entrada_inicio];g_entrada_inicio=(g_entrada_inicio+1)%128;g_entrada_total--;return c;}
    return consola_leer_caracter_directo();
}
int consola_sondear_cancelacion(void) {
    driver_ram_sondear();consola_puntero_presentar();
    if(g_lote)presentar();
    red_stream_atender();
    int cancelar=0;
    while(g_entrada_total<128){
        char c=consola_leer_caracter_directo();if(!c)break;
        if(c==3 || c==27){cancelar=1;continue;}
        g_entrada_pendiente[(g_entrada_inicio+g_entrada_total)%128]=c;g_entrada_total++;
    }
    return cancelar;
}

int consola_leer_linea(char *buffer, int max_len) {
    if (!buffer || max_len <= 1) return 0;

    int idx = 0;
    buffer[0] = '\0';

    int ciclo_cursor = 0;
    int estado_cursor = 1;
    unsigned historial=0;char escape[8];unsigned esc=0;

    for (;;) {
        // Mantener el búfer ping-pong de HDA abastecido mientras la terminal
        // espera una tecla. AC97 ignora esta llamada cuando no hay audio.
        audio_ac97_actualizar();

        // Dibujar cursor parpadeante de bloque
        ciclo_cursor++;
        if (ciclo_cursor >= 50000 && !g_vista) {
            ciclo_cursor = 0;
            estado_cursor = !estado_cursor;
            uint8_t c_cursor = estado_cursor ? 0xDB : ' '; // 0xDB es el bloque sólido en CP437
            pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, c_cursor, COLOR_PROMPT_DEFAULT, g_color_bg);
        }

        char c = consola_leer_caracter();
        if (c == 0) {
            if (idx == 0) red_remoto_atender();
            continue;
        }
        if(c==27){esc=1;continue;}
        if(esc){
            if(esc<sizeof(escape))escape[esc++-1]=c;else{esc=0;continue;}
            if(c=='A'||c=='B'){
                int ctrl=esc>=5 && escape[esc-3]=='5';
                c=c=='A'?(ctrl?TECLA_CTRL_ARRIBA:TECLA_ARRIBA):(ctrl?TECLA_CTRL_ABAJO:TECLA_ABAJO);esc=0;
            }else if(c!='[' && c!=';' && !(c>='0'&&c<='9')){esc=0;continue;}else continue;
        }
        if(c==TECLA_CTRL_ARRIBA || c==TECLA_CTRL_ABAJO){consola_desplazar(c==TECLA_CTRL_ARRIBA?5:-5);continue;}
        if(g_vista){g_vista=0;repintar();}
        if(c==TECLA_ARRIBA || c==TECLA_ABAJO){
            if(!historial){unsigned n=(unsigned)idx;if(n>255)n=255;memcpy(g_borrador,buffer,n);g_borrador[n]=0;}
            if(c==TECLA_ARRIBA && historial<g_hist_total)historial++;
            if(c==TECLA_ABAJO && historial)historial--;
            const char *texto=historial?g_historial[(g_hist_proximo+32-historial)%32]:g_borrador;
            consola_lote_iniciar();while(idx-- >0){consola_escribir_caracter('\b');serial_escribir_caracter(' ');serial_escribir_caracter('\b');}
            idx=0;while(texto[idx] && idx<max_len-1){buffer[idx]=texto[idx];consola_escribir_caracter(texto[idx++]);}buffer[idx]=0;consola_lote_finalizar();continue;
        }

        // Limpiar el cursor en la posición actual antes de procesar
        pantalla_dibujar_caracter(g_cursor_x, g_cursor_y, ' ', g_color_fg, g_color_bg);

        if (c == 3) { // Ctrl+C
            buffer[0] = '\0';
            consola_imprimir_linea_color("^C", COLOR_AVISO_DEFAULT);
            return 0;
        }

        if (c == '\n' || c == '\r') {
            buffer[idx] = '\0';
            if(idx>0 && idx<256 && (!g_hist_total || memcmp(g_historial[(g_hist_proximo+31)%32],buffer,(size_t)idx+1))){
                memcpy(g_historial[g_hist_proximo],buffer,(size_t)idx+1);g_hist_proximo=(g_hist_proximo+1)%32;if(g_hist_total<32)g_hist_total++;
            }
            consola_escribir_caracter('\n');
            return idx;
        }

        if (c == '\b' || (uint8_t)c == 127) { // Backspace o Delete
            if (idx > 0) {
                idx--;
                buffer[idx] = '\0';
                {
                    consola_escribir_caracter('\b');
                    serial_escribir_caracter(' ');
                    serial_escribir_caracter('\b');
                }
            }
            continue;
        }

        if (c >= 32 && idx < max_len - 1) {
            buffer[idx++] = c;
            buffer[idx] = '\0';
            consola_escribir_caracter(c);
        }
    }
}
