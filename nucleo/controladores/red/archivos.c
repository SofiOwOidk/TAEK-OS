#include "archivos.h"
#include "red.h"
#include "stream.h"
#include "remoto.h"
#include "../consola.h"
#include "../exfat_transfer.h"
#include "../fat32_transfer.h"
#include "../usb_msc.h"
#include "../archivo_lectura.h"
#include "../../base/memoria.h"
#include "../../base/sha256.h"
#include "../../base/tiempo.h"
#define BLOQUE 32768
#define LIMITE (UINT64_C(1024)*1024*1024)
static int backend_fat32,destino_elegido;
static int usb_archivo_activo(void){return exfat_transfer_activo() || fat32_transfer_activo();}
static const struct particion *usb_archivo_particion(void){return !destino_elegido?0:backend_fat32?fat32_transfer_particion():exfat_transfer_particion();}
static uint64_t usb_archivo_libres(void){return backend_fat32?fat32_transfer_libres():exfat_transfer_libres();}
static const char *usb_archivo_temporal(void){return backend_fat32?fat32_transfer_temporal():exfat_transfer_temporal();}
static int usb_archivo_destino(const struct particion *p){
    if(usb_archivo_activo())return VOLUMEN_ELEGIR;
    int fat=p && p->formato==VOLUMEN_FAT32;
    int r=fat?fat32_transfer_destino(p):exfat_transfer_destino(p);destino_elegido=!r;if(!r)backend_fat32=fat;return r;
}
static int usb_archivo_iniciar(const char *n,uint64_t t){
    const struct particion *p=usb_archivo_particion();if(!p)return VOLUMEN_NO_ENCONTRADO;
    struct particion copia=*p;int r=usb_archivo_destino(&copia);if(r)return r;
    return backend_fat32?fat32_transfer_iniciar(n,t):exfat_transfer_iniciar(n,t);
}
static int usb_archivo_escribir(uint64_t o,const void *p,unsigned n){return backend_fat32?fat32_transfer_escribir(o,p,n):exfat_transfer_escribir(o,p,n);}
static int usb_archivo_publicar(const uint8_t h[32]){return backend_fat32?fat32_transfer_publicar(h):exfat_transfer_publicar(h);}
static int usb_archivo_cancelar(void){return backend_fat32?fat32_transfer_cancelar():exfat_transfer_cancelar();}
static uint8_t ip[4],cab[128],respuesta[128],datos[BLOQUE],esperado[32],*ram;
static uint16_t puerto=9154;
static uint64_t tamano,offset;
static char nombre[64];
static int configurado,pendiente,en_usb,completo;
int red_archivos_ram_obtener(const void **p,uint64_t *n,uint8_t digest[32]){
    if(!p || !n || !digest || !completo || pendiente || en_usb || !ram || offset!=tamano)return -1;
    *p=ram;*n=tamano;memcpy(digest,esperado,32);return 0;
}
static struct sha256 hash;
struct recibo {
    char nombre[64];uint8_t hash[32];uint64_t bytes,milisegundos;
    int usb;struct particion destino;
};
static struct recibo recibos[8];
static unsigned recibos_total,recibos_proximo;
static void imprimir_hash(const uint8_t *digest){
    const char hex[]="0123456789abcdef";
    for(unsigned i=0;i<32;i++){consola_escribir_caracter(hex[digest[i]>>4]);consola_escribir_caracter(hex[digest[i]&15]);}
}
static void imprimir_destino(const struct particion *p){
    consola_imprimir("USB ");consola_imprimir_dec(p->unidad);
    consola_imprimir(" particion ");consola_imprimir_dec(p->numero);
    consola_imprimir(" /; generacion ");consola_imprimir_dec(p->generacion_msc);
    consola_imprimir("; LBA ");consola_imprimir_dec(p->inicio);consola_imprimir_linea("");
}
static void registrar_recibo(uint64_t ms){
    struct recibo *r=&recibos[recibos_proximo];memset(r,0,sizeof(*r));
    memcpy(r->nombre,nombre,64);memcpy(r->hash,esperado,32);r->bytes=tamano;r->milisegundos=ms;r->usb=en_usb;
    if(en_usb)r->destino=*usb_archivo_particion();
    recibos_proximo=(recibos_proximo+1)%8;if(recibos_total<8)recibos_total++;
}
static uint32_t u32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint64_t u64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n=(n<<8)|p[i];return n;}
static void b32(uint8_t *p,uint32_t n){for(int i=3;i>=0;i--){p[i]=n;n>>=8;}}
static void b64(uint8_t *p,uint64_t n){for(int i=7;i>=0;i--){p[i]=n;n>>=8;}}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static const char *palabra(const char *p,char *out,unsigned max){unsigned n=0;while(*p==' ')p++;while(*p && *p!=' '){if(n+1>=max)return 0;out[n++]=*p++;}out[n]=0;while(*p==' ')p++;return p;}
static int numero(const char *p,unsigned *out){if(!*p)return 0;unsigned n=0;while(*p){if(*p<'0'||*p>'9'||n>6553)return 0;n=n*10+(*p++-'0');}if(n>65535)return 0;*out=n;return 1;}
static int valido(const char *p){unsigned n=0;for(;p[n] && n<61;n++)if(!((p[n]>='a'&&p[n]<='z')||(p[n]>='A'&&p[n]<='Z')||(p[n]>='0'&&p[n]<='9')||p[n]=='-'||p[n]=='_'||p[n]=='.'))return 0;return n && n<=60 && p[0]!='.' && p[n-1]!='.';}
static void paquete(unsigned op,unsigned n){memset(cab,0,128);memcpy(cab,"TAEKFIL1",8);b32(cab+8,op);b32(cab+12,n);if(op!=1){b64(cab+16,tamano);b64(cab+24,offset);memcpy(cab+32,esperado,32);}memcpy(cab+64,nombre,64);}
static int recibir(void *out,unsigned n){uint64_t fin=tiempo_obtener_milisegundos()+5000;unsigned leidos=0;while(leidos<n){if(consola_sondear_cancelacion())return VOLUMEN_CANCELADO;int r=red_tcp_flujo_recibir((uint8_t *)out+leidos,n-leidos);if(r<0)return -1;leidos+=(unsigned)r;if(tiempo_obtener_milisegundos()>fin)return -2;__asm__ volatile("pause");}return 0;}
static int intercambio(unsigned op,unsigned n){
    paquete(op,n);uint64_t fin=tiempo_obtener_milisegundos()+5000;int r;
    while((r=red_tcp_flujo_enviar(cab,128))==1){if(consola_sondear_cancelacion())return VOLUMEN_CANCELADO;red_pila_sondeo();if(tiempo_obtener_milisegundos()>fin)return -2;}
    if(r<0)return r;r=recibir(respuesta,128);if(r)return r;
    if(memcmp(respuesta,"TAEKFIL1",8)||u32(respuesta+8)!=(op==1?2:op==3?4:6)||u32(respuesta+12)!=n||memcmp(respuesta+64,nombre,64))return VOLUMEN_CORRUPTO;
    if(op==1){if(u64(respuesta+24)||!u64(respuesta+16)||u64(respuesta+16)>LIMITE)return VOLUMEN_CORRUPTO;}
    else if(u64(respuesta+16)!=tamano||u64(respuesta+24)!=offset||memcmp(respuesta+32,esperado,32))return VOLUMEN_CORRUPTO;
    return n?recibir(datos,n):0;
}
static void progreso(void){consola_imprimir("[ARCHIVO] ");consola_imprimir_dec(offset);consola_imprimir(" / ");consola_imprimir_dec(tamano);consola_imprimir_linea(" B");consola_lote_finalizar();consola_lote_iniciar();}
static int transferir(int nueva){
    red_stream_pausar(1);red_remoto_detener();int r=red_tcp_flujo_abrir(ip,puerto);if(r)goto fin;
    r=intercambio(1,0);if(r)goto fin;
    if(nueva){
        tamano=u64(respuesta+16);offset=0;memcpy(esperado,respuesta+32,32);sha256_iniciar(&hash);
        if(en_usb){r=usb_archivo_iniciar(nombre,tamano);if(r)goto fin;}
        else {if(tamano>32*1024*1024){r=VOLUMEN_NO_SOPORTADO;goto fin;}ram=asignar_memoria(tamano);if(!ram){r=VOLUMEN_NO_SOPORTADO;goto fin;}}
        pendiente=1;completo=0;
    }else if(tamano!=u64(respuesta+16)||memcmp(esperado,respuesta+32,32)){r=VOLUMEN_CORRUPTO;goto fin;}
    uint64_t inicio=tiempo_obtener_milisegundos(),ultimo=0;
    while(offset<tamano){
        unsigned n=tamano-offset>BLOQUE?BLOQUE:(unsigned)(tamano-offset);
        r=intercambio(3,n);if(r)goto fin;
        if(en_usb){r=usb_archivo_escribir(offset,datos,n);if(r)goto fin;}else memcpy(ram+offset,datos,n);
        sha256_actualizar(&hash,datos,n);offset+=n;
        r=intercambio(5,0);if(r)goto fin;
        uint64_t ahora=tiempo_obtener_milisegundos();if(ahora-ultimo>=500){progreso();ultimo=ahora;}
    }
    struct sha256 copia=hash;uint8_t obtenido[32];sha256_final(&copia,obtenido);if(memcmp(obtenido,esperado,32)){r=VOLUMEN_CORRUPTO;goto fin;}
    if(en_usb){consola_imprimir_linea("Releyendo USB y verificando SHA-256...");progreso();r=usb_archivo_publicar(esperado);if(r)goto fin;}
    pendiente=0;completo=1;uint64_t ms=tiempo_obtener_milisegundos()-inicio;registrar_recibo(ms);
    progreso();consola_imprimir("Archivo: ");consola_imprimir_linea(nombre);
    if(en_usb)imprimir_destino(usb_archivo_particion());
    consola_imprimir("SHA-256: ");imprimir_hash(esperado);consola_imprimir_linea("");
    consola_imprimir("Tiempo: ");consola_imprimir_dec(ms);consola_imprimir_linea(" ms");
    consola_imprimir_linea(en_usb?(backend_fat32?"[OK] Archivo publicado en FAT32, sincronizado y releido.":"[OK] Archivo publicado en exFAT, sincronizado y releido. Seleccion Ventoy manual."):"[OK] Archivo verificado en RAM. No instalado en USB.");
fin:
    red_tcp_flujo_cerrar();red_stream_pausar(0);return r;
}
static int elegir(unsigned unidad,unsigned num){
    struct particiones *ps=asignar_memoria(sizeof(*ps));if(!ps)return VOLUMEN_NO_SOPORTADO;
    int r=particiones_descubrir((uint8_t)unidad,ps);const struct particion *p=0;
    if(!r)for(unsigned i=0;i<ps->total;i++)if(ps->entradas[i].numero==num)p=&ps->entradas[i];
    if(!r)r=p?usb_archivo_destino(p):VOLUMEN_NO_ENCONTRADO;liberar_memoria(ps);return r;
}
static void resultado(int r){
    if(r==VOLUMEN_CANCELADO){
        int c=en_usb?usb_archivo_cancelar():0;
        if(!c){if(ram)liberar_memoria(ram);ram=0;pendiente=0;completo=0;}
        consola_imprimir_linea(c?"Cancelada con I/O ambiguo; conservar temporal y revisar medio.":"Cancelada; recursos liberados.");
    }else if(r){
        if(en_usb && usb_archivo_activo())pendiente=1;
        consola_imprimir("Transferencia detenida, codigo ");consola_imprimir_dec((uint64_t)-r);
        consola_imprimir_linea(pendiente?". archivo reintentar | archivo cancelar":". Sin recepcion completada.");
    }
}
void red_archivos_comando(const char *arg){
    char op[32],a[64],b[65];const char *p=palabra(arg,op,sizeof(op));if(!p)goto uso;
    if(igual(op,"listar")){
        consola_imprimir_linea("Recibidos verificados en este arranque (ultimos 8):");
        for(unsigned i=0;i<recibos_total;i++){
            const struct recibo *r=&recibos[(recibos_proximo+8-recibos_total+i)%8];
            consola_imprimir_linea(r->usb?"PUBLICADO USB":"RAM verificada (registro; no implica contenido retenido)");
            consola_imprimir_linea(r->nombre);consola_imprimir_dec(r->bytes);consola_imprimir_linea(" B");
            if(r->usb)imprimir_destino(&r->destino);
            consola_imprimir("SHA-256: ");imprimir_hash(r->hash);consola_imprimir_linea("");
        }return;
    }
    if(igual(op,"estado")){
        progreso();consola_imprimir_linea(completo?(en_usb?"PUBLICADO USB":"VERIFICADO RAM"):pendiente?"Pendiente: archivo reintentar | archivo cancelar":"Sin transferencia pendiente");
        if(completo || pendiente){consola_imprimir_linea(nombre);consola_imprimir("SHA-256: ");imprimir_hash(esperado);consola_imprimir_linea("");if(en_usb)imprimir_destino(usb_archivo_particion());}
        if(en_usb&&pendiente)consola_imprimir_linea(usb_archivo_temporal());return;
    }
    if(igual(op,"cancelar")){
        int r=en_usb?usb_archivo_cancelar():0;if(r){consola_imprimir_linea("I/O ambiguo: se conserva .part y Dirty. Revisar medio externamente.");return;}
        if(ram)liberar_memoria(ram);ram=0;pendiente=0;completo=0;consola_imprimir_linea("Transferencia liberada.");return;
    }
    if(igual(op,"reintentar")){if(!pendiente)goto uso;resultado(transferir(0));return;}
    if(pendiente || usb_archivo_activo()){consola_imprimir_linea("Hay una transferencia pendiente; reintentar o cancelar primero.");return;}
    if(igual(op,"verificar")){
        p=palabra(p,a,sizeof(a));if(!p)goto uso;p=palabra(p,b,sizeof(b));if(!p || *p || !valido(a))goto uso;
        uint8_t referencia[32],calculado[32];int comparar=*b!=0;
        if(comparar){
            unsigned n=0;while(b[n])n++;if(n!=64)goto uso;
            for(unsigned i=0;i<32;i++){
                unsigned valor=0;for(unsigned j=0;j<2;j++){
                    char c=b[i*2+j];if(c>='A'&&c<='F')c=(char)(c+'a'-'A');
                    unsigned dig;if(c>='0'&&c<='9')dig=(unsigned)(c-'0');else if(c>='a'&&c<='f')dig=(unsigned)(c-'a'+10);else goto uso;
                    valor=valor*16+dig;
                }referencia[i]=(uint8_t)valor;
            }
        }
        const struct particion *dest=usb_archivo_particion();if(!dest){consola_imprimir_linea("Selecciona destino con archivo destino <USB> <particion>.");return;}
        consola_imprimir_linea("Calculando SHA-256 del archivo USB; Ctrl+C cancela...");
        consola_lote_finalizar();consola_lote_iniciar();uint64_t bytes=0;
        int r=archivo_calcular_sha256(dest,a,calculado,&bytes);
        if(r){consola_imprimir_linea(volumen_error(r));return;}
        consola_imprimir("Archivo: ");consola_imprimir_linea(a);imprimir_destino(dest);
        consola_imprimir_dec(bytes);consola_imprimir_linea(" B");consola_imprimir("SHA-256: ");imprimir_hash(calculado);consola_imprimir_linea("");
        if(comparar && memcmp(referencia,calculado,32))consola_imprimir_linea("[ERROR] SHA-256 no coincide con la referencia.");
        else consola_imprimir_linea(comparar?"[OK] SHA-256 coincide; lectura del archivo USB completa.":"[OK] SHA-256 calculado desde el archivo USB.");
        return;
    }
    if(igual(op,"servidor")){
        p=palabra(p,a,sizeof(a));if(!p)goto uso;p=palabra(p,b,sizeof(b));unsigned port=9154;if(!p || *p || red_texto_a_ip(a,ip) || (*b && (!numero(b,&port)||!port)))goto uso;puerto=(uint16_t)port;configurado=1;consola_imprimir_linea("Servidor de archivos configurado.");return;
    }
    if(igual(op,"destino")){
        if(igual(p,"ninguno")){destino_elegido=0;completo=0;consola_imprimir_linea("Destino USB liberado; archivos publicados conservados.");return;}
        p=palabra(p,a,sizeof(a));if(!p)goto uso;p=palabra(p,b,sizeof(b));unsigned unidad,num;if(!p || *p || !numero(a,&unidad)||unidad>3||!numero(b,&num))goto uso;
        int r=elegir(unidad,num);if(r){consola_imprimir_linea(volumen_error(r));return;}
        const struct particion *dest=usb_archivo_particion();completo=0;consola_imprimir(backend_fat32?"Destino FAT32 / (nombre 8.3); generacion USB ":"Destino exFAT /; generacion USB ");consola_imprimir_dec(dest->generacion_msc);consola_imprimir("; inicio LBA ");consola_imprimir_dec(dest->inicio);consola_imprimir("; libres ");consola_imprimir_dec(usb_archivo_libres());consola_imprimir_linea(" B. Aun no se ha escrito.");return;
    }
    if(igual(op,"recibir") || igual(op,"ram")){
        p=palabra(p,a,sizeof(a));if(!p || *p || !valido(a) || !configurado)goto uso;
        int usb=igual(op,"recibir");if(usb && !usb_archivo_particion()){consola_imprimir_linea("Selecciona destino USB primero. Usa seleccionar <unidad> [particion] para inventario.");return;}
        en_usb=usb;completo=0;tamano=offset=0;memset(esperado,0,32);
        if(ram)liberar_memoria(ram);ram=0;memset(nombre,0,64);unsigned len=0;while(a[len])len++;memcpy(nombre,a,len);
        consola_imprimir_linea("Transferencia binaria: stream pausado; Ctrl+C LOCAL cancela. Se reconecta al terminar.");
        consola_lote_finalizar();consola_lote_iniciar();resultado(transferir(1));
        return;
    }
uso:
    consola_imprimir_linea("archivo servidor <IP> [9154] | destino <USB> <particion> | destino ninguno | recibir <oferta>");
    consola_imprimir_linea("archivo ram <oferta> (max 32 MiB) | estado | listar | reintentar | cancelar");
    consola_imprimir_linea("archivo verificar <archivo> [SHA-256 esperado] (solo lectura USB)");
}

int red_archivos_preparar_reinicio(void){
    if(pendiente || usb_archivo_activo())return VOLUMEN_ELEGIR;
    const struct particion *p=usb_archivo_particion();if(!p)return 0;
    uint8_t sector[512];int r=particion_leer(p,0,0,sector,0);if(r)return r;
    return usb_msc_sincronizar(p->unidad)?VOLUMEN_TRANSPORTE:0;
}
