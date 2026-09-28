#include "particiones.h"
#include "usb_msc.h"
#include "../base/memoria.h"
static uint16_t u16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p){return u16(p)|((uint32_t)u16(p+2)<<16);}
static uint64_t u64(const uint8_t *p){return u32(p)|((uint64_t)u32(p+4)<<32);}
static int (*servicio)(void);
void particiones_configurar_servicio(int (*f)(void)){servicio=f;}
static uint32_t crc(uint32_t c,const uint8_t *p,unsigned n){while(n--){c^=*p++;for(unsigned b=0;b<8;b++)c=(c>>1)^(0xedb88320u&-(c&1));}return c;}
int particion_leer(const struct particion *p,uint64_t sector,unsigned n,void *buf,const volatile uint8_t *cancel) {
    if(!p || !buf || sector>p->sectores || n>p->sectores-sector)return VOLUMEN_CORRUPTO;
    if(p->inicio>UINT64_MAX-sector)return VOLUMEN_CORRUPTO;
    if(!n){
        if(servicio && servicio())return VOLUMEN_CANCELADO;
        if(cancel && *cancel)return VOLUMEN_CANCELADO;
        const struct usb_msc_dispositivo *dev=usb_msc_obtener_dispositivo(p->unidad);
        return !dev || !dev->activo || !dev->listo || dev->generacion!=p->generacion_msc?VOLUMEN_DESCONECTADO:0;
    }
    uint8_t *d=buf;
    while(n){
        if(servicio && servicio())return VOLUMEN_CANCELADO;
        if(cancel && *cancel)return VOLUMEN_CANCELADO;
        const struct usb_msc_dispositivo *dev=usb_msc_obtener_dispositivo(p->unidad);
        if(!dev || !dev->activo || !dev->listo || dev->generacion!=p->generacion_msc)return VOLUMEN_DESCONECTADO;
        if(dev->tamano_sector!=512)return VOLUMEN_SECTOR_NO_SOPORTADO;
        unsigned k=n>8?8:n;uint64_t lba=p->inicio+sector;
        if(lba>UINT32_MAX || k>dev->sectores_totales || lba>dev->sectores_totales-k)return VOLUMEN_CORRUPTO;
        if(usb_msc_leer_sectores(p->unidad,(uint32_t)lba,(uint16_t)k,d))return VOLUMEN_TRANSPORTE;
        dev=usb_msc_obtener_dispositivo(p->unidad);
        if(!dev || !dev->activo || !dev->listo || dev->generacion!=p->generacion_msc)return VOLUMEN_DESCONECTADO;
        d+=(size_t)k*512;sector+=k;n-=k;
    }
    return 0;
}
const char *volumen_nombre(enum volumen_formato f){static const char *n[]={"Desconocido","FAT32","exFAT","NTFS","ext4"};return f<=VOLUMEN_EXT4?n[f]:n[0];}
const char *volumen_error(int r){switch(r){
    case 0:return "Identificado";case VOLUMEN_NO_SOPORTADO:return "Formato o variante no soportados";
    case VOLUMEN_CORRUPTO:return "Metadatos o geometria inconsistentes";case VOLUMEN_TRANSPORTE:return "Fallo de lectura USB";
    case VOLUMEN_DESCONECTADO:return "Unidad desconectada o reemplazada";case VOLUMEN_CANCELADO:return "Operacion cancelada";
    case VOLUMEN_ELEGIR:return "Elige una particion";case VOLUMEN_SECTOR_NO_SOPORTADO:return "Sector logico no soportado (se requiere 512 B)";
    case VOLUMEN_SOLO_LECTURA:return "Volumen abierto en solo lectura";case VOLUMEN_NO_ENCONTRADO:return "Archivo o directorio no encontrado";
    case VOLUMEN_TEXFAT_NO_SOPORTADO:return "TexFAT o dos FAT no soportados";
    default:return "Formato desconocido";
}}
int particion_identificar(struct particion *p) {
    uint8_t s[512];int r=particion_leer(p,0,1,s,0);p->formato=VOLUMEN_DESCONOCIDO;
    if(r)return p->resultado=r;
    if(!memcmp(s+3,"EXFAT   ",8))p->formato=VOLUMEN_EXFAT;
    else if(!memcmp(s+3,"NTFS    ",8))p->formato=VOLUMEN_NTFS;
    else {
        unsigned b=u16(s+11),spc=s[13],res=u16(s+14),nf=s[16];
        uint64_t total=u16(s+19)?u16(s+19):u32(s+32),fat=u16(s+22)?u16(s+22):u32(s+36);
        uint64_t root=((uint64_t)u16(s+17)*32+b-1)/(b?b:1),meta=res+nf*fat+root;
        if(s[510]==0x55 && s[511]==0xaa && b>=512 && b<=4096 && !(b&(b-1)) && spc && !(spc&(spc-1)) && res && nf && nf<=2 && total>meta && fat) {
            uint64_t nc=(total-meta)/spc;
            if(nc>=65525 && !u16(s+17) && !u16(s+22))p->formato=VOLUMEN_FAT32;
            else return p->resultado=VOLUMEN_NO_SOPORTADO; /* FAT12/16, jamás asumir FAT32 por etiqueta. */
        } else if(!memcmp(s+82,"FAT32   ",8)){p->formato=VOLUMEN_FAT32;return p->resultado=VOLUMEN_CORRUPTO;}
        if(p->formato==VOLUMEN_DESCONOCIDO && p->sectores>12){r=particion_leer(p,12,1,s,0);if(r)return p->resultado=r;if(!memcmp(s+3,"EXFAT   ",8)){p->formato=VOLUMEN_EXFAT;p->recuperado=1;}}
        if(p->formato==VOLUMEN_DESCONOCIDO && p->sectores>2){r=particion_leer(p,2,1,s,0);if(r)return p->resultado=r;if(u16(s+56)==0xef53)p->formato=VOLUMEN_EXT4;}
    }
    return p->resultado=p->formato==VOLUMEN_DESCONOCIDO?VOLUMEN_DESCONOCIDO_ERROR:0;
}
static int agregar(struct particiones *out,const struct particion *disco,uint64_t inicio,uint64_t n,unsigned num,unsigned esquema){
    if(!n || !inicio || inicio>=disco->sectores || n>disco->sectores-inicio)return VOLUMEN_CORRUPTO;
    if(out->total==PARTICIONES_MAX)return VOLUMEN_NO_SOPORTADO;
    for(unsigned i=0;i<out->total;i++){const struct particion *p=&out->entradas[i];if(inicio<p->inicio+p->sectores && p->inicio<inicio+n)return VOLUMEN_CORRUPTO;}
    struct particion *p=&out->entradas[out->total++];*p=*disco;p->inicio=inicio;p->sectores=n;p->numero=num;p->esquema=esquema;
    return 0;
}
static int gpt(struct particiones *out,const struct particion *disco,uint64_t donde){
    uint8_t h[512],s[512];int r=particion_leer(disco,donde,1,h,0);if(r)return r;
    unsigned tam=u32(h+12),n=u32(h+80),entrada=u32(h+84);uint64_t tabla=u64(h+72),primero=u64(h+40),ultimo=u64(h+48);
    if(memcmp(h,"EFI PART",8) || u32(h+8)!=0x10000 || tam<92 || tam>512 || u32(h+20) || u64(h+24)!=donde || u64(h+32)>=disco->sectores || primero>ultimo || ultimo>=disco->sectores)return VOLUMEN_CORRUPTO;
    uint64_t alterno=donde==1?disco->sectores-1:1;
    if(u64(h+32)!=alterno || primero<2 || ultimo>=disco->sectores-1 ||
       (donde>=primero && donde<=ultimo) || (alterno>=primero && alterno<=ultimo))return VOLUMEN_CORRUPTO;
    uint32_t esperado=u32(h+16);memset(h+16,0,4);if((crc(~0u,h,tam)^~0u)!=esperado)return VOLUMEN_CORRUPTO;
    if(!n || n>4096 || entrada<128 || entrada>4096 || (entrada&(entrada-1)))return VOLUMEN_NO_SOPORTADO;
    uint64_t bytes=(uint64_t)n*entrada,sectors=(bytes+511)/512;
    if(!tabla || tabla>=disco->sectores || sectors>disco->sectores-tabla || (tabla<=ultimo && tabla+sectors>primero))return VOLUMEN_CORRUPTO;
    if((donde>=tabla && donde<tabla+sectors) || (alterno>=tabla && alterno<tabla+sectors))return VOLUMEN_CORRUPTO;
    uint32_t c=~0u;for(uint64_t off=0;off<bytes;off+=512){r=particion_leer(disco,tabla+off/512,1,s,0);if(r)return r;c=crc(c,s,(unsigned)(bytes-off<512?bytes-off:512));}
    if((c^~0u)!=u32(h+88))return VOLUMEN_CORRUPTO;
    for(unsigned i=0;i<n;i++){
        r=particion_leer(disco,tabla+(uint64_t)i*entrada/512,1,s,0);if(r)return r;
        const uint8_t *e=s+((uint64_t)i*entrada%512);int usado=0;for(unsigned k=0;k<16;k++)usado|=e[k];if(!usado)continue;
        uint64_t a=u64(e+32),b=u64(e+40);if(a<primero || b>ultimo || a>b)return VOLUMEN_CORRUPTO;
        r=agregar(out,disco,a,b-a+1,i+1,2);if(r)return r;
    }
    return 0;
}
int particiones_descubrir(uint8_t unidad,struct particiones *out){
    if(!out)return VOLUMEN_CORRUPTO;memset(out,0,sizeof(*out));
    const struct usb_msc_dispositivo *d=usb_msc_obtener_dispositivo(unidad);
    if(!d || !d->activo || !d->listo)return VOLUMEN_DESCONECTADO;
    if(d->tamano_sector!=512)return VOLUMEN_SECTOR_NO_SOPORTADO;
    struct particion disco={.sectores=d->sectores_totales,.generacion_msc=d->generacion,.unidad=unidad};
    uint8_t s[512];int r=particion_leer(&disco,0,1,s,0);if(r)return r;
    struct particion directo=disco;r=particion_identificar(&directo);
    if(r==VOLUMEN_TRANSPORTE || r==VOLUMEN_DESCONECTADO || r==VOLUMEN_CANCELADO)return r;
    if(directo.formato!=VOLUMEN_DESCONOCIDO){out->entradas[0]=directo;out->entradas[0].numero=1;out->total=1;return 0;}
    if(s[510]!=0x55 || s[511]!=0xaa)return VOLUMEN_CORRUPTO;
    int protector=0;for(unsigned i=0;i<4;i++)protector|=s[446+i*16+4]==0xee;
    if(protector){
        r=gpt(out,&disco,1);
        if(r==VOLUMEN_CORRUPTO){out->total=0;r=gpt(out,&disco,disco.sectores-1);if(!r)out->recuperado=1;}
        if(r){out->total=0;return r;}
    } else {
        for(unsigned i=0;i<4;i++){
            const uint8_t *e=s+446+i*16;if(!e[4])continue;
            if(e[0]!=0 && e[0]!=0x80)return VOLUMEN_CORRUPTO;
            if(e[4]==5 || e[4]==15 || e[4]==0x85)return VOLUMEN_NO_SOPORTADO; /* EBR no implementado. */
            r=agregar(out,&disco,u32(e+8),u32(e+12),i+1,1);if(r){out->total=0;return r;}
        }
    }
    for(unsigned i=0;i<out->total;i++){r=particion_identificar(&out->entradas[i]);if(r==VOLUMEN_TRANSPORTE || r==VOLUMEN_DESCONECTADO)return r;}
    if(!out->total)return VOLUMEN_DESCONOCIDO_ERROR;return 0;
}
