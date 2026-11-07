/* Escritor acotado de archivos nuevos: exFAT, raiz existente, extent contiguo.
 * Metadatos de un archivo caben en un sector. No promete atomicidad frente a
 * cortes de energia. Dirty y reservas se conservan ante I/O ambiguo. */
#include "exfat_transfer.h"
#include "fat_lector.h"
#include "usb_msc.h"
#include "consola.h"
#include "../base/memoria.h"
#include "../base/sha256.h"
static struct fat_lector_volumen vol;
static uint8_t sector[512],dir_original[512],dir_actual[512],buffer[32768];
static uint64_t libres,total,escritos,dir_sector;
static uint32_t primero,cantidad;
static unsigned ranura,entradas;
static int activo,fallo;
static char final[64],temporal[70];
static uint16_t u16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static void b16(uint8_t *p,uint16_t n){p[0]=n;p[1]=n>>8;}
static void b32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++){p[i]=n;n>>=8;}}
static void b64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++){p[i]=n;n>>=8;}}
static uint64_t lba_cluster(uint32_t c){return vol.datos+(uint64_t)(c-2)*vol.sectores_cluster;}
static int escribir(uint64_t lba,unsigned n,const void *datos){
    int r=particion_leer(&vol.particion,0,0,sector,0);if(r)return fallo=1,r;
    if(lba>vol.particion.sectores || n>vol.particion.sectores-lba || vol.particion.inicio+lba>UINT32_MAX)return fallo=1,VOLUMEN_CORRUPTO;
    r=usb_msc_escribir_sectores(vol.particion.unidad,(uint32_t)(vol.particion.inicio+lba),(uint16_t)n,datos);
    if(r){fallo=1;return VOLUMEN_TRANSPORTE;}return 0;
}
static int sync(void){if(particion_leer(&vol.particion,0,0,sector,0) || usb_msc_sincronizar(vol.particion.unidad)){fallo=1;return VOLUMEN_TRANSPORTE;}return 0;}
static int dirty(int estado){
    for(unsigned i=0;i<2;i++){
        unsigned lba=estado?i*12:(1-i)*12;
        int r=particion_leer(&vol.particion,lba,1,sector,0);if(r)return fallo=1,r;
        uint16_t flags=u16(sector+106);b16(sector+106,estado?flags|2:flags&~2);
        r=escribir(lba,1,sector);if(r)return r;
    }return sync();
}
/* cursor.cluster tras leer ubica la pagina incluso con bitmap en varios clusters. */
static int bitmap(uint64_t pagina,uint8_t *out,uint64_t *lba){
    uint64_t offset=pagina*512;size_t n=vol.longitud_bitmap-offset;if(n>512)n=512;
    memset(out,0,512);vol.bitmap.parcial_valido=0;
    int64_t r=fat_lector_leer(&vol,&vol.bitmap,offset,out,n,0);if(r!=(int64_t)n)return r<0?(int)r:VOLUMEN_CORRUPTO;
    *lba=lba_cluster(vol.bitmap.cluster)+offset%vol.bytes_cluster/512;return 0;
}
static int reserva(int asignar){
    uint64_t fin=(uint64_t)primero-2+cantidad;
    for(uint64_t pagina=((uint64_t)primero-2)/4096;pagina<=(fin-1)/4096;pagina++){
        uint64_t lba;int r=bitmap(pagina,buffer,&lba);if(r)return fallo=1,r;
        uint64_t desde=pagina*4096,hasta=desde+4096;if(desde<primero-2)desde=primero-2;if(hasta>fin)hasta=fin;
        for(uint64_t bit=desde;bit<hasta;bit++){unsigned b=(unsigned)(bit%4096);if(asignar)buffer[b/8]|=1u<<(b%8);else buffer[b/8]&=~(1u<<(b%8));}
        r=escribir(lba,1,buffer);if(r)return r;
    }vol.bitmap_pagina=UINT64_MAX;return sync();
}
static unsigned largo(const char *s){unsigned n=0;while(s[n])n++;return n;}
static void entrada(const char *nombre,uint64_t valida){
    uint8_t *p=dir_actual+ranura*32;unsigned len=largo(nombre),n=2+(len+14)/15;
    for(unsigned i=0;i<entradas;i++){memset(p+i*32,0,32);p[i*32]=1;}
    memset(p,0,n*32);p[0]=0x85;p[1]=(uint8_t)(n-1);b16(p+4,0x20);
    p[32]=0xc0;p[33]=3;p[35]=(uint8_t)len;uint16_t hash=0;
    for(unsigned i=0;i<len;i++){uint16_t u=vol.upcase[(uint8_t)nombre[i]];hash=(uint16_t)(((hash<<15)|(hash>>1))+(u&255));hash=(uint16_t)(((hash<<15)|(hash>>1))+(u>>8));unsigned off=64+(i/15)*32;p[off]=0xc1;b16(p+off+2+(i%15)*2,(uint8_t)nombre[i]);}
    b16(p+36,hash);b64(p+40,valida);b32(p+52,primero);b64(p+56,total);
    uint16_t sum=0;for(unsigned i=0;i<n*32;i++)if(i!=2 && i!=3)sum=(uint16_t)(((sum<<15)|(sum>>1))+p[i]);b16(p+2,sum);
}
static int buscar_ranura(void){
    struct fat_lector_nodo raiz;struct fat_lector_cursor c;
    int r=fat_lector_resolver(&vol,"/",&raiz,0);if(r)return r;fat_lector_cursor_iniciar(&c,&raiz);
    int terminado=0;
    for(uint64_t offset=0;offset<raiz.longitud;offset+=512){
        int64_t n=fat_lector_leer(&vol,&c,offset,dir_original,512,0);if(n!=512)return n<0?(int)n:VOLUMEN_NO_SOPORTADO;
        unsigned seguidas=0;
        for(unsigned i=0;i<16;i++){
            if(!dir_original[i*32])terminado=1;
            if(terminado || !(dir_original[i*32]&128))seguidas++;else seguidas=0;
            if(seguidas==entradas){ranura=i+1-entradas;dir_sector=lba_cluster(c.cluster)+offset%vol.bytes_cluster/512;
                memcpy(dir_actual,dir_original,512);
                // Si se consume el marcador de fin, limpiar el resto de este sector.
                if(terminado)memset(dir_actual+ranura*32,0,512-ranura*32);
                return 0;}
        }
        if(terminado)return VOLUMEN_NO_SOPORTADO;
    }return VOLUMEN_NO_SOPORTADO;
}
int exfat_transfer_destino(const struct particion *p){
    if(activo)return VOLUMEN_ELEGIR;fat_lector_desmontar(&vol);libres=0;fallo=0;
    if(!p || p->formato!=VOLUMEN_EXFAT)return VOLUMEN_NO_SOPORTADO;
    int r=fat_lector_montar(&vol,p);if(r)return r;
    uint8_t a[512],b[512];r=particion_leer(p,0,1,a,0);if(!r)r=particion_leer(p,12,1,b,0);
    if(r || vol.respaldo || (u16(a+106)&7) || (u16(b+106)&7) || memcmp(a+64,b+64,42) || a[110]!=1){fat_lector_desmontar(&vol);return r?r:VOLUMEN_NO_SOPORTADO;}
    for(uint64_t pagina=0;pagina<(vol.clusters+UINT64_C(4095))/4096;pagina++){
        uint64_t lba;r=bitmap(pagina,buffer,&lba);if(r){fat_lector_desmontar(&vol);return r;}
        for(unsigned i=0;i<4096 && pagina*4096+i<vol.clusters;i++)if(!(buffer[i/8]&(1u<<(i%8))))libres+=vol.bytes_cluster;
    }return 0;
}
uint64_t exfat_transfer_libres(void){return libres;}
int exfat_transfer_activo(void){return activo;}
const char *exfat_transfer_temporal(void){return temporal;}
const struct particion *exfat_transfer_particion(void){return vol.montado?&vol.particion:0;}
int exfat_transfer_iniciar(const char *nombre,uint64_t bytes){
    if(!vol.montado || activo || fallo || !nombre || !bytes || bytes>libres)return VOLUMEN_NO_SOPORTADO;
    unsigned len=0;for(;nombre[len] && len<63;len++)if(!((nombre[len]>='a'&&nombre[len]<='z')||(nombre[len]>='A'&&nombre[len]<='Z')||(nombre[len]>='0'&&nombre[len]<='9')||nombre[len]=='-'||nombre[len]=='_'||nombre[len]=='.'))return VOLUMEN_NO_SOPORTADO;
    if(len<1 || len>60 || nombre[len] || nombre[0]=='.' || nombre[len-1]=='.')return VOLUMEN_NO_SOPORTADO;
    memcpy(final,nombre,len+1);memcpy(temporal,nombre,len);memcpy(temporal+len,".part",6);
    struct fat_lector_nodo nodo;int r=fat_lector_resolver(&vol,final,&nodo,0);if(r!=VOLUMEN_NO_ENCONTRADO)return r?r:VOLUMEN_ELEGIR;
    r=fat_lector_resolver(&vol,temporal,&nodo,0);if(r!=VOLUMEN_NO_ENCONTRADO)return r?r:VOLUMEN_ELEGIR;
    total=bytes;escritos=0;cantidad=(uint32_t)((bytes-1)/vol.bytes_cluster+1);entradas=2+(len+5+14)/15;
    if(cantidad>vol.clusters)return VOLUMEN_NO_SOPORTADO;
    r=buscar_ranura();if(r)return r;
    uint32_t seguidas=0;primero=0;
    for(uint64_t pagina=0;pagina<(vol.clusters+UINT64_C(4095))/4096 && !primero;pagina++){
        uint64_t lba;r=bitmap(pagina,buffer,&lba);if(r)return r;
        for(unsigned i=0;i<4096 && pagina*4096+i<vol.clusters;i++){
            if(buffer[i/8]&(1u<<(i%8)))seguidas=0;else if(++seguidas==cantidad){primero=(uint32_t)(pagina*4096+i+3-cantidad);break;}
        }
    }if(!primero)return VOLUMEN_NO_SOPORTADO;
    // Probar flush antes de modificar el medio.
    r=sync();if(r)return r;r=dirty(1);if(r)return r;activo=1;
    r=reserva(1);if(r)return r;entrada(temporal,0);r=escribir(dir_sector,1,dir_actual);if(!r)r=sync();return r;
}
int exfat_transfer_escribir(uint64_t offset,const void *datos,unsigned n){
    if(!activo || fallo || offset!=escritos || offset%512 || !n || n>sizeof(buffer) || n>total-offset || (n%512 && offset+n!=total))return VOLUMEN_CORRUPTO;
    unsigned sectores=(n+511)/512;memcpy(buffer,datos,n);memset(buffer+n,0,sectores*512-n);
    int r=escribir(lba_cluster(primero)+offset/512,sectores,buffer);if(!r)escritos+=n;return r;
}
int exfat_transfer_publicar(const uint8_t esperado[32]){
    if(!activo || fallo || escritos!=total)return VOLUMEN_CORRUPTO;
    int r=sync();if(r)return r;struct sha256 hash;sha256_iniciar(&hash);
    for(uint64_t off=0;off<total;){
        if(consola_sondear_cancelacion())return VOLUMEN_CANCELADO;
        unsigned n=total-off>sizeof(buffer)?sizeof(buffer):(unsigned)(total-off);
        r=particion_leer(&vol.particion,lba_cluster(primero)+off/512,(n+511)/512,buffer,0);if(r)return fallo=1,r;
        sha256_actualizar(&hash,buffer,n);off+=n;
    }uint8_t recibido[32];sha256_final(&hash,recibido);if(memcmp(recibido,esperado,32))return VOLUMEN_CORRUPTO;
    // Publicar tras verificacion: todo el set se reemplaza en el mismo sector.
    r=particion_leer(&vol.particion,dir_sector,1,buffer,0);if(r || memcmp(buffer,dir_actual,512))return fallo=1,r?r:VOLUMEN_CORRUPTO;
    entrada(final,total);r=escribir(dir_sector,1,dir_actual);if(!r)r=sync();if(r)return r;
    // Releer nombre/set final desde medio, con caches frescas, antes de confirmar.
    vol.bitmap_pagina=UINT64_MAX;struct fat_lector_nodo nodo;r=fat_lector_resolver(&vol,final,&nodo,0);
    if(r || nodo.longitud!=total || nodo.valida!=total || nodo.cluster!=primero){fallo=1;return r?r:VOLUMEN_CORRUPTO;}
    r=dirty(0);if(r)return r;
    activo=0;libres-=(uint64_t)cantidad*vol.bytes_cluster;return 0;
}
int exfat_transfer_cancelar(void){
    if(!activo)return 0;if(fallo)return VOLUMEN_TRANSPORTE;
    int r=escribir(dir_sector,1,dir_original);if(!r)r=sync();if(!r)r=reserva(0);if(!r)r=dirty(0);
    if(!r)activo=0;return r;
}
