/* Archivos nuevos 8.3 en raiz FAT32 existente, FATs espejadas y extent
 * contiguo. No habilita el escritor VFS antiguo. I/O ambiguo conserva Dirty
 * y reserva; no promete atomicidad ni recuperacion tras un corte de energia. */
#include "fat32_transfer.h"
#include "fat_lector.h"
#include "archivo_lectura.h"
#include "usb_msc.h"
#include "consola.h"
#include "../base/memoria.h"
#include "../base/sha256.h"
static struct fat_lector_volumen vol;
static uint8_t sector[512], espejo[512], original[512], directorio[512], buffer[32768];
static uint64_t libres,total,escritos,dir_lba;
static uint32_t primero,cantidad;
static unsigned fats,ranura,fsinfo,backup,reservados;
static int activo,fallo;
static char final[13], temporal[13];
static uint8_t corto[11];
static uint16_t u16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p){return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void b32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++){p[i]=n;n>>=8;}}
static int leer(uint64_t lba,unsigned n,void *p){int r=particion_leer(&vol.particion,lba,n,p,0);if(r)fallo=1;return r;}
static int escribir(uint64_t lba,unsigned n,const void *p){
    int r=particion_leer(&vol.particion,0,0,espejo,0);if(r)return fallo=1,r;
    if(lba>vol.particion.sectores || n>vol.particion.sectores-lba || vol.particion.inicio+lba>UINT32_MAX)return fallo=1,VOLUMEN_CORRUPTO;
    if(usb_msc_escribir_sectores(vol.particion.unidad,(uint32_t)(vol.particion.inicio+lba),(uint16_t)n,p))return fallo=1,VOLUMEN_TRANSPORTE;
    return 0;
}
static int sync(void){if(particion_leer(&vol.particion,0,0,espejo,0)||usb_msc_sincronizar(vol.particion.unidad))return fallo=1,VOLUMEN_TRANSPORTE;return 0;}
static int dirty(int estado){
    for(unsigned f=0;f<fats;f++){
        uint64_t lba=vol.fat+(uint64_t)f*vol.sectores_fat;int r=leer(lba,1,sector);if(r)return r;
        uint32_t v=u32(sector+4);b32(sector+4,estado?v&~0x08000000u:v|0x08000000u);
        r=escribir(lba,1,sector);if(r)return r;
    }return sync();
}
static int nombre83(const char *s,uint8_t out[11]){
    memset(out,' ',11);unsigned a=0,b=0;int punto=0;
    for(unsigned i=0;s[i];i++){
        unsigned char c=(unsigned char)s[i];if(c=='.'){if(punto||!a)return 0;punto=1;continue;}
        if(c>='a'&&c<='z')c-=32;
        if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'))return 0;
        if(!punto){if(a==8)return 0;out[a++]=c;}else{if(b==3)return 0;out[8+b++]=c;}
    }return a && (!punto || b);
}
static int ranura_buscar(void){
    struct fat_lector_nodo raiz;struct fat_lector_cursor c;int r=fat_lector_resolver(&vol,"/",&raiz,0);if(r)return r;
    fat_lector_cursor_iniciar(&c,&raiz);unsigned anterior=0;
    for(uint64_t off=0;off<raiz.longitud;off+=512){
        int64_t n=fat_lector_leer(&vol,&c,off,original,512,0);if(n!=512)return n<0?(int)n:VOLUMEN_CORRUPTO;
        for(unsigned i=0;i<16;i++){
          if(!original[i*32] || original[i*32]==0xe5){
            /* Evitar convertir un LFN huérfano precedente en el nombre del nuevo archivo. */
            if(anterior==0x0f){if(!original[i*32])return VOLUMEN_NO_SOPORTADO;anterior=0;continue;}
            ranura=i;dir_lba=vol.datos+(uint64_t)(c.cluster-2)*vol.sectores_cluster+off%vol.bytes_cluster/512;
            memcpy(directorio,original,512);if(!original[i*32])memset(directorio+i*32,0,512-i*32);return 0;
          }
          anterior=original[i*32+11];
        }
    }return VOLUMEN_NO_SOPORTADO;
}
static void entrada(const char *name){
    uint8_t *p=directorio+ranura*32;uint8_t raw[11];nombre83(name,raw);memset(p,0,32);memcpy(p,raw,11);p[11]=0x20;
    p[20]=(uint8_t)(primero>>16);p[21]=(uint8_t)(primero>>24);p[26]=(uint8_t)primero;p[27]=(uint8_t)(primero>>8);b32(p+28,(uint32_t)total);
    /* Fecha DOS minima valida (1980-01-01). */p[16]=p[18]=p[24]=0x21;
}
static int fsinfo_actualizar(uint32_t disponibles){
    if(!fsinfo || fsinfo==0xffff)return 0;
    for(unsigned i=0;i<(backup && backup!=0xffff?2u:1u);i++){
        unsigned lba=fsinfo+(i?backup:0);if(lba>=reservados)return VOLUMEN_CORRUPTO;
        int r=leer(lba,1,sector);if(r)return r;
        if(u32(sector)!=0x41615252 || u32(sector+484)!=0x61417272 || u32(sector+508)!=0xaa550000)return VOLUMEN_CORRUPTO;
        /* Conteo obtenido de todas las entradas FAT; pista de siguiente libre desconocida. */
        b32(sector+488,disponibles);b32(sector+492,UINT32_MAX);r=escribir(lba,1,sector);if(r)return r;
    }return sync();
}
static int reserva(int poner){
    uint32_t fin=primero+cantidad;
    for(uint32_t s=primero/128;s<=(fin-1)/128;s++){
        int r=leer(vol.fat+s,1,sector);if(r)return r;
        unsigned desde=primero>s*128?primero-s*128:0,hasta=fin-s*128;if(hasta>128)hasta=128;
        for(unsigned i=desde;i<hasta;i++){
            uint32_t cl=s*128+i,v=u32(sector+i*4),n=poner?(cl+1==fin?0x0fffffffu:cl+1):0;
            if(poner && (v&0x0fffffffu))return fallo=1,VOLUMEN_CORRUPTO;
            b32(sector+i*4,(v&0xf0000000u)|n);
        }
        for(unsigned f=0;f<fats;f++){r=escribir(vol.fat+(uint64_t)f*vol.sectores_fat+s,1,sector);if(r)return r;}
    }memset(vol.fat_valido,0,sizeof(vol.fat_valido));return sync();
}
int fat32_transfer_destino(const struct particion *p){
    if(activo)return VOLUMEN_ELEGIR;fat_lector_desmontar(&vol);libres=0;fallo=0;
    if(!p||p->formato!=VOLUMEN_FAT32)return VOLUMEN_NO_SOPORTADO;
    int r=fat_lector_montar(&vol,p);if(r)return r;
    r=leer(0,1,sector);if(r)goto mal;
    fats=sector[16];reservados=u16(sector+14);fsinfo=u16(sector+48);backup=u16(sector+50);
    if((fats!=1&&fats!=2)||u16(sector+40)||u16(sector+42)||vol.sectores_fat*UINT64_C(128)<(uint64_t)vol.clusters+2){r=VOLUMEN_NO_SOPORTADO;goto mal;}
    if(fsinfo && fsinfo!=0xffff){
        for(unsigned i=0;i<(backup && backup!=0xffff?2u:1u);i++){
            uint64_t lba=fsinfo+(i?backup:0);if(lba>=reservados){r=VOLUMEN_CORRUPTO;goto mal;}
            r=leer(lba,1,sector);if(r)goto mal;
            if(u32(sector)!=0x41615252||u32(sector+484)!=0x61417272||u32(sector+508)!=0xaa550000){r=VOLUMEN_NO_SOPORTADO;goto mal;}
        }
    }
    for(uint32_t s=0;s<vol.sectores_fat;s++){
        r=leer(vol.fat+s,1,sector);if(r)goto mal;
        if(fats==2){r=leer(vol.fat+vol.sectores_fat+s,1,espejo);if(r)goto mal;if(memcmp(sector,espejo,512)){r=VOLUMEN_CORRUPTO;goto mal;}}
        if(!s && (u32(sector+4)&0x0c000000u)!=0x0c000000u){r=VOLUMEN_CORRUPTO;goto mal;}
        for(unsigned i=0;i<128;i++){uint32_t c=s*128+i;if(c>=2&&c<vol.clusters+2&&!(u32(sector+i*4)&0x0fffffffu))libres+=vol.bytes_cluster;}
    }return 0;
mal:fat_lector_desmontar(&vol);return r;
}
uint64_t fat32_transfer_libres(void){return libres;}
int fat32_transfer_activo(void){return activo;}
const char *fat32_transfer_temporal(void){return temporal;}
const struct particion *fat32_transfer_particion(void){return vol.montado?&vol.particion:0;}
int fat32_transfer_iniciar(const char *name,uint64_t bytes){
    if(!vol.montado||activo||fallo||!name||!bytes||bytes>UINT32_MAX||bytes>libres||!nombre83(name,corto))return VOLUMEN_NO_SOPORTADO;
    memset(final,0,sizeof(final));unsigned len=0;while(name[len])len++;if(len>12)return VOLUMEN_NO_SOPORTADO;memcpy(final,name,len);
    struct fat_lector_nodo nodo;int r=fat_lector_resolver(&vol,final,&nodo,0);if(r!=VOLUMEN_NO_ENCONTRADO)return r?r:VOLUMEN_ELEGIR;
    /* Temporal corto distinto del final, comprobado antes de cualquier escritura. */
    memcpy(temporal,"TAEK0000.PRT",13);int encontrado=0;const char hex[]="0123456789ABCDEF";
    for(unsigned i=0;i<65536;i++){
        for(unsigned j=0;j<4;j++)temporal[4+j]=hex[(i>>(12-j*4))&15];
        uint8_t tmp[11];nombre83(temporal,tmp);if(!memcmp(tmp,corto,11))continue;
        r=fat_lector_resolver(&vol,temporal,&nodo,0);if(r==VOLUMEN_NO_ENCONTRADO){encontrado=1;break;}if(r)return r;
    }if(!encontrado)return VOLUMEN_NO_SOPORTADO;
    total=bytes;escritos=0;cantidad=(uint32_t)((bytes-1)/vol.bytes_cluster+1);r=ranura_buscar();if(r)return r;
    uint32_t seguidas=0;primero=0;
    for(uint32_t s=0;s<vol.sectores_fat && !primero;s++){
        r=leer(vol.fat+s,1,sector);if(r)return r;
        for(unsigned i=0;i<128;i++){
            uint32_t c=s*128+i;if(c<2||c>=vol.clusters+2)continue;
            if(u32(sector+i*4)&0x0fffffffu)seguidas=0;else if(++seguidas==cantidad){primero=c+1-cantidad;break;}
        }
    }if(!primero)return VOLUMEN_NO_SOPORTADO;
    r=sync();if(r)return r;activo=1;r=dirty(1);if(r)return r;
    r=fsinfo_actualizar((uint32_t)(libres/vol.bytes_cluster)-cantidad);if(r){fallo=1;return r;}r=reserva(1);if(r)return r;
    entrada(temporal);r=escribir(dir_lba,1,directorio);return r?r:sync();
}
int fat32_transfer_escribir(uint64_t off,const void *p,unsigned n){
    if(!activo||fallo||off!=escritos||!n||n>32768||n>total-off||off%512)return VOLUMEN_NO_SOPORTADO;
    unsigned sectores=(n+511)/512;memset(buffer,0,sectores*512);memcpy(buffer,p,n);
    int r=escribir(vol.datos+(uint64_t)(primero-2)*vol.sectores_cluster+off/512,sectores,buffer);if(!r)escritos+=n;return r;
}
int fat32_transfer_publicar(const uint8_t hash[32]){
    if(!activo||fallo||escritos!=total)return VOLUMEN_NO_SOPORTADO;int r=sync();if(r)return r;
    struct sha256 h;uint8_t obtenido[32];sha256_iniciar(&h);
    for(uint64_t off=0;off<total;){
        if(consola_sondear_cancelacion())return VOLUMEN_CANCELADO;
        unsigned n=total-off>32768?32768:(unsigned)(total-off);
        r=leer(vol.datos+(uint64_t)(primero-2)*vol.sectores_cluster+off/512,(n+511)/512,buffer);if(r)return r;
        sha256_actualizar(&h,buffer,n);off+=n;
    }sha256_final(&h,obtenido);if(memcmp(hash,obtenido,32))return VOLUMEN_CORRUPTO;
    entrada(final);r=escribir(dir_lba,1,directorio);if(r)return r;r=sync();if(r)return r;
    uint64_t bytes=0;r=archivo_calcular_sha256(&vol.particion,final,obtenido,&bytes);
    if(r||bytes!=total||memcmp(hash,obtenido,32)){fallo=1;return r?r:VOLUMEN_CORRUPTO;}
    r=dirty(0);if(r)return r;activo=0;libres-=cantidad*(uint64_t)vol.bytes_cluster;return 0;
}
int fat32_transfer_cancelar(void){
    if(!activo)return 0;if(fallo)return VOLUMEN_TRANSPORTE;
    int r=escribir(dir_lba,1,original);if(r)return r;r=sync();if(r)return r;r=reserva(0);if(r)return r;
    r=fsinfo_actualizar((uint32_t)(libres/vol.bytes_cluster));if(r){fallo=1;return r;}
    r=dirty(0);if(r)return r;activo=0;return 0;
}
