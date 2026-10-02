#include "fat_lector.h"
#include "vfs.h"
#include "consola.h"
#include "../base/memoria.h"
#include "fat_oem850.h"
struct fat_lector_volumen fat_lector_fat32, fat_lector_exfat;
_Static_assert(sizeof(struct fat_lector_cursor)<=VFS_CURSOR_TAMANO_MAX,"Cursor FAT acotado");
static uint16_t u16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p){return u16(p)|((uint32_t)u16(p+2)<<16);}
static uint64_t u64(const uint8_t *p){return u32(p)|((uint64_t)u32(p+4)<<32);}
static int valido(const struct fat_lector_volumen *v,uint32_t c){return c>=2 && (uint64_t)c<v->clusters+UINT64_C(2);}
static uint32_t rotar(uint32_t n,uint8_t b){return ((n>>1)|(n<<31))+b;}
static uint16_t checksum16(uint16_t n,uint8_t b){return (uint16_t)(((n>>1)|(n<<15))+b);}
void fat_lector_desmontar(struct fat_lector_volumen *v){if(v->upcase)liberar_memoria(v->upcase);memset(v,0,sizeof(*v));}
static int siguiente_cluster(struct fat_lector_volumen *v,uint32_t c,uint32_t *salida,const volatile uint8_t *cancel){
    if(!valido(v,c))return VOLUMEN_CORRUPTO;
    uint64_t lba=v->fat+(uint64_t)c*4/512;unsigned slot=(unsigned)(lba%4);
    int r=particion_leer(&v->particion,0,0,v->fat_cache[slot],cancel);if(r)return r;
    if(!v->fat_valido[slot] || v->fat_sector[slot]!=lba) {
        r=particion_leer(&v->particion,lba,1,v->fat_cache[slot],cancel);if(r)return r;
        v->fat_sector[slot]=lba;v->fat_valido[slot]=1;
    }
    uint8_t *s=v->fat_cache[slot];
    uint32_t n=u32(s+(c*4u%512));if(!v->exfat)n&=0x0fffffff;
    if((v->exfat && n==0xffffffff) || (!v->exfat && n>=0x0ffffff8))return 1;
    /* Libre=0, reservado=1, defectuoso=fffffff7 y fuera de heap nunca son EOF. */
    if(!valido(v,n))return VOLUMEN_CORRUPTO;*salida=n;return 0;
}
void fat_lector_cursor_iniciar(struct fat_lector_cursor *c,const struct fat_lector_nodo *n){memset(c,0,sizeof(*c));c->nodo=*n;c->cluster=c->tortuga=n->cluster;c->potencia=1;}
static int localizar(struct fat_lector_volumen *v,struct fat_lector_cursor *c,uint32_t indice,const volatile uint8_t *cancel){
    if(indice>=v->clusters)return VOLUMEN_CORRUPTO;
    if(c->nodo.contiguo){uint64_t fin=(uint64_t)c->nodo.cluster+indice;if(fin>UINT32_MAX || !valido(v,(uint32_t)fin))return VOLUMEN_CORRUPTO;c->cluster=(uint32_t)fin;c->indice=indice;return 0;}
    if(indice<c->indice){
        c->indice=0;c->cluster=c->tortuga=c->nodo.cluster;c->potencia=1;c->pasos=c->total_pasos=0;
        for(unsigned i=0;i<c->puntos;i++){struct fat_lector_punto *p=&c->punto[i];if(p->indice<=indice && p->indice>c->indice){c->indice=p->indice;c->cluster=p->cluster;c->tortuga=p->tortuga;c->potencia=p->potencia;c->pasos=p->pasos;c->total_pasos=p->indice;}}
    }
    if(!valido(v,c->cluster))return VOLUMEN_CORRUPTO;
    while(c->indice<indice){
        if(cancel && *cancel)return VOLUMEN_CANCELADO;
        uint32_t n;int r=siguiente_cluster(v,c->cluster,&n,cancel);if(r)return r;
        if(++c->total_pasos>=v->clusters || n==c->tortuga)return VOLUMEN_CORRUPTO;
        c->pasos++;c->cluster=n;c->indice++;
        if(c->pasos==c->potencia){c->tortuga=n;c->pasos=0;if(c->potencia<=UINT32_MAX/2)c->potencia*=2;}
        if((c->indice&255)==0){unsigned j=c->proximo++%FAT_LECTOR_PUNTOS;c->punto[j]=(struct fat_lector_punto){c->indice,c->cluster,c->tortuga,c->potencia,c->pasos};if(c->puntos<FAT_LECTOR_PUNTOS)c->puntos++;}
    }
    return 0;
}
static int asignado(struct fat_lector_volumen *v,uint32_t cluster,const volatile uint8_t *cancel){
    if(!v->exfat || !v->cluster_bitmap)return 0;
    uint64_t byte=(cluster-2)/8,pagina=byte/512;
    if(pagina!=v->bitmap_pagina){
        memset(v->bitmap_cache,0,512);size_t n=v->longitud_bitmap-pagina*512<512?(size_t)(v->longitud_bitmap-pagina*512):512;
        /* La lectura de la propia bitmap no consulta asignado(): evita recursión. */
        int64_t r=fat_lector_leer(v,&v->bitmap,pagina*512,v->bitmap_cache,n,cancel);if(r!=(int64_t)n)return r<0?(int)r:VOLUMEN_CORRUPTO;
        v->bitmap_pagina=pagina;
    }
    return (v->bitmap_cache[byte%512]&(1u<<((cluster-2)%8)))?0:VOLUMEN_CORRUPTO;
}
int64_t fat_lector_leer(struct fat_lector_volumen *v,struct fat_lector_cursor *c,uint64_t off,void *buf,size_t cantidad,const volatile uint8_t *cancel){
    if(!v || !buf || !c || !v->bytes_cluster)return VOLUMEN_CORRUPTO;
    if(off>=c->nodo.longitud || !cantidad)return 0;
    if(cantidad>INT64_MAX)return VOLUMEN_CORRUPTO;
    if(cantidad>c->nodo.longitud-off)cantidad=(size_t)(c->nodo.longitud-off);
    uint8_t *dst=buf;size_t hechos=0;
    while(hechos<cantidad){
        int identidad=particion_leer(&v->particion,0,0,dst,cancel);if(identidad)return identidad;
        uint64_t indice=(off+hechos)/v->bytes_cluster;if(indice>=v->clusters)return VOLUMEN_CORRUPTO;
        int r=localizar(v,c,(uint32_t)indice,cancel);if(r)return r>0?VOLUMEN_CORRUPTO:r;
        if(c!=&v->bitmap){r=asignado(v,c->cluster,cancel);if(r)return r;}
        unsigned dentro=(unsigned)((off+hechos)%v->bytes_cluster);size_t n=cantidad-hechos;
        if(n>v->bytes_cluster-dentro)n=v->bytes_cluster-dentro;
        if(off+hechos>=c->nodo.valida){memset(dst+hechos,0,n);hechos+=n;continue;}
        if(n>c->nodo.valida-(off+hechos))n=(size_t)(c->nodo.valida-(off+hechos));
        /* Agrupar sólo después de recorrer/validar FAT y bitmap. Nunca
         * inferir contigüidad por offsets MP4 o por número de cluster. */
        if(!(dentro%512) && n>=512) {
            size_t objetivo=cantidad-hechos;
            if(objetivo>262144)objetivo=262144; /* tope READ(10): 256 KiB */
            if(objetivo>c->nodo.valida-(off+hechos))objetivo=(size_t)(c->nodo.valida-(off+hechos));
            struct fat_lector_cursor prueba=*c;
            while(n<objetivo) {
                uint32_t anterior=prueba.cluster;
                r=localizar(v,&prueba,prueba.indice+1,cancel);
                if(r)return r>0?VOLUMEN_CORRUPTO:r;
                if(prueba.cluster!=anterior+1)break;
                if(c!=&v->bitmap){r=asignado(v,prueba.cluster,cancel);if(r)return r;}
                size_t k=objetivo-n;if(k>v->bytes_cluster)k=v->bytes_cluster;n+=k;
            }
        }
        uint64_t lba=v->datos+(uint64_t)(c->cluster-2)*v->sectores_cluster+dentro/512;
        if((dentro%512) || n<512){size_t k=512-dentro%512;if(k>n)k=n;
            if(!c->parcial_valido || c->sector_parcial!=lba){r=particion_leer(&v->particion,lba,1,c->parcial,cancel);if(r)return r;c->sector_parcial=lba;c->parcial_valido=1;}
            memcpy(dst+hechos,c->parcial+dentro%512,k);n=k;}
        else {unsigned bloques=(unsigned)(n/512);if(bloques>512)bloques=512;/* 256 KiB */r=particion_leer(&v->particion,lba,bloques,dst+hechos,cancel);if(r)return r;n=(size_t)bloques*512;}
        hechos+=n;
    }
    return (int64_t)hechos;
}
static int boot_exfat(struct fat_lector_volumen *v,unsigned inicio,uint8_t *cabecera){
    uint8_t s[512];uint32_t sum=0;int r;
    for(unsigned i=0;i<12;i++){
        r=particion_leer(&v->particion,inicio+i,1,s,0);if(r)return r;
        if(!i){memcpy(cabecera,s,512);if(memcmp(s+3,"EXFAT   ",8))return VOLUMEN_CORRUPTO;if(s[108]!=9)return VOLUMEN_SECTOR_NO_SOPORTADO;}
        if(i>=1 && i<=8 && u32(s+508)!=0xaa550000)return VOLUMEN_CORRUPTO;
        if(i==11){for(unsigned j=0;j<512;j+=4)if(u32(s+j)!=sum)return VOLUMEN_CORRUPTO;}
        else for(unsigned j=0;j<512;j++)if(i || (j!=106 && j!=107 && j!=112))sum=rotar(sum,s[j]);
    }
    const uint8_t *s0=cabecera;
    if(s0[0]!=0xeb || s0[1]!=0x76 || s0[2]!=0x90 || u16(s0+510)!=0xaa55)return VOLUMEN_CORRUPTO;
    for(unsigned j=11;j<64;j++)if(s0[j])return VOLUMEN_CORRUPTO;
    if(s0[110]==2)return VOLUMEN_TEXFAT_NO_SOPORTADO;
    if(u16(s0+104)!=0x100 || s0[110]!=1)return VOLUMEN_NO_SOPORTADO;
    if(s0[109]>16 || (s0[112]>100 && s0[112]!=255))return VOLUMEN_CORRUPTO;
    uint64_t total=u64(s0+72),fat=u32(s0+80),nf=u32(s0+84),datos=u32(s0+88),nc=u32(s0+92),spc=UINT64_C(1)<<s0[109];
    uint64_t desplazamiento=u64(s0+64);
    if(total>v->particion.sectores || total<2048 || fat<24 || !nf || nc<1 || nc>0xfffffff5 || nf*512<(nc+2)*4 || datos<fat+nf || datos>=total || nc*spc>total-datos || (desplazamiento && desplazamiento!=v->particion.inicio) || (u16(s0+106)&1))return VOLUMEN_CORRUPTO;
    uint64_t calculados=(total-datos)/spc;if(calculados>0xfffffff5)calculados=0xfffffff5;
    if(nc!=calculados)return VOLUMEN_CORRUPTO;
    if(u32(s0+96)<2 || (uint64_t)u32(s0+96)>=nc+2)return VOLUMEN_CORRUPTO;
    v->sectores=total;v->fat=fat;v->datos=datos;v->clusters=(uint32_t)nc;v->sectores_cluster=(uint32_t)spc;v->bytes_cluster=(uint32_t)spc*512;v->raiz=u32(s0+96);v->sectores_fat=(uint32_t)nf;
    return 0;
}
static int boot_fat32(struct fat_lector_volumen *v,const uint8_t *s){
    if(u16(s+11)!=512)return VOLUMEN_SECTOR_NO_SOPORTADO;
    unsigned spc=s[13],nf=s[16],reservados=u16(s+14);uint64_t total=u32(s+32),fat=u32(s+36),meta=reservados+nf*fat;
    if(u16(s+510)!=0xaa55 || !spc || spc>128 || (spc&(spc-1)) || !reservados || !nf || nf>2 || !fat || u16(s+17) || u16(s+19) || u16(s+22) || total<=meta || total>v->particion.sectores)return VOLUMEN_CORRUPTO;
    if(u16(s+42))return VOLUMEN_NO_SOPORTADO;
    uint64_t nc=(total-meta)/spc;
    if(nc<65525 || nc>0x0ffffff5 || fat*512<(nc+2)*4 || u32(s+44)<2 || u32(s+44)>=nc+2)return VOLUMEN_CORRUPTO;
    unsigned activo=(u16(s+40)&0x80)?u16(s+40)&15:0;if(activo>=nf)return VOLUMEN_CORRUPTO;
    v->sectores=total;v->fat=reservados+activo*fat;v->datos=meta;v->clusters=(uint32_t)nc;v->raiz=u32(s+44);v->sectores_cluster=spc;v->bytes_cluster=spc*512;v->sectores_fat=(uint32_t)fat;return 0;
}
static struct fat_lector_nodo raiz(struct fat_lector_volumen *v){struct fat_lector_nodo n={.cluster=v->raiz,.directorio=1,.longitud=(uint64_t)v->clusters*v->bytes_cluster,.valida=(uint64_t)v->clusters*v->bytes_cluster};return n;}
int fat_lector_iterar(struct fat_lector_volumen *v,const struct fat_lector_nodo *n,struct fat_lector_iterador *it){
    if(!v || !n || !n->directorio)return VOLUMEN_NO_ENCONTRADO;memset(it,0,sizeof(*it));fat_lector_cursor_iniciar(&it->cursor,n);it->sector_cache=UINT64_MAX;return 0;
}
static int entrada_raw(struct fat_lector_volumen *v,struct fat_lector_iterador *it,uint8_t *e,const volatile uint8_t *cancel){
    if(it->posicion>=it->cursor.nodo.longitud)return 0;
    uint64_t indice=it->posicion/v->bytes_cluster;int r=localizar(v,&it->cursor,(uint32_t)indice,cancel);if(r)return r==1?0:r;
    r=asignado(v,it->cursor.cluster,cancel);if(r)return r;
    uint64_t sec=it->posicion/512;
    if(it->sector_cache!=sec){uint64_t lba=v->datos+(uint64_t)(it->cursor.cluster-2)*v->sectores_cluster+(it->posicion%v->bytes_cluster)/512;r=particion_leer(&v->particion,lba,1,it->cache,cancel);if(r)return r;it->sector_cache=sec;}
    memcpy(e,it->cache+it->posicion%512,32);it->posicion+=32;return 1;
}
static int utf8(const uint16_t *src,unsigned n,char *out){
    unsigned j=0;for(unsigned i=0;i<n;i++){
        uint32_t cp=src[i];if(cp<32 || cp==0xffff || cp==127 || cp=='/' || cp=='\\')return VOLUMEN_CORRUPTO;
        if(cp>=0xd800 && cp<=0xdbff){if(i+1>=n || src[i+1]<0xdc00 || src[i+1]>0xdfff)return VOLUMEN_CORRUPTO;cp=0x10000+((cp-0xd800)<<10)+(src[++i]-0xdc00);}
        else if(cp>=0xdc00 && cp<=0xdfff)return VOLUMEN_CORRUPTO;
        if(cp<0x80)out[j++]=(char)cp;
        else if(cp<0x800){out[j++]=(char)(0xc0|(cp>>6));out[j++]=(char)(0x80|(cp&63));}
        else if(cp<0x10000){out[j++]=(char)(0xe0|(cp>>12));out[j++]=(char)(0x80|((cp>>6)&63));out[j++]=(char)(0x80|(cp&63));}
        else{out[j++]=(char)(0xf0|(cp>>18));out[j++]=(char)(0x80|((cp>>12)&63));out[j++]=(char)(0x80|((cp>>6)&63));out[j++]=(char)(0x80|(cp&63));}
    }out[j]=0;return 0;
}
static int nodo_valido(struct fat_lector_volumen *v,struct fat_lector_nodo *n){
    if(n->valida>n->longitud || n->longitud>(uint64_t)v->clusters*v->bytes_cluster)return VOLUMEN_CORRUPTO;
    if(!n->longitud && !n->directorio)return (n->cluster || n->contiguo)?VOLUMEN_CORRUPTO:0;
    if(!valido(v,n->cluster))return VOLUMEN_CORRUPTO;
    uint64_t cant=(n->longitud+v->bytes_cluster-1)/v->bytes_cluster;
    if(n->contiguo && cant>v->clusters+UINT64_C(2)-n->cluster)return VOLUMEN_CORRUPTO;
    return 0;
}
static int siguiente_exfat(struct fat_lector_volumen *v,struct fat_lector_iterador *it,struct fat_lector_nodo *n,const volatile uint8_t *cancel){
    uint8_t e[32],stream[32];int r;
    while((r=entrada_raw(v,it,e,cancel))>0){
        if(!e[0]){it->terminado=1;return 0;}if(!(e[0]&0x80))continue;
        if(e[0]!=0x85){if(e[0]==0x81 || e[0]==0x82 || e[0]==0x83 || (e[0]&0x20))continue;return VOLUMEN_NO_SOPORTADO;}
        unsigned secundarias=e[1],nombres=0;uint16_t sum=0,esperado=u16(e+2);if(secundarias<2)return VOLUMEN_CORRUPTO;
        for(unsigned j=0;j<32;j++)if(j!=2 && j!=3)sum=checksum16(sum,e[j]);
        memset(n,0,sizeof(*n));n->directorio=!!(u16(e+4)&16);
        for(unsigned i=0;i<secundarias;i++){
            r=entrada_raw(v,it,e,cancel);if(r!=1)return r<0?r:VOLUMEN_CORRUPTO;
            for(unsigned j=0;j<32;j++)sum=checksum16(sum,e[j]);
            if(!i){if(e[0]!=0xc0)return VOLUMEN_CORRUPTO;memcpy(stream,e,32);n->unidades=e[3];n->contiguo=!!(e[1]&2);n->cluster=u32(e+20);n->valida=u64(e+8);n->longitud=u64(e+24);if(!(e[1]&1) || (e[1]&~3) || !n->unidades)return VOLUMEN_CORRUPTO;}
            else if(e[0]==0xc1){if(e[1])return VOLUMEN_CORRUPTO;for(unsigned k=0;k<15;k++){if(nombres<n->unidades)n->nombre16[nombres]=u16(e+2+k*2);nombres++;}}
            else if(!(e[0]&0x20) || !(e[0]&0x80) || !(e[0]&0x40))return VOLUMEN_NO_SOPORTADO;
        }
        if(sum!=esperado || nombres<n->unidades || nombres>=n->unidades+15)return VOLUMEN_CORRUPTO;
        if(v->upcase){uint16_t h=0;for(unsigned k=0;k<n->unidades;k++){uint16_t c=v->upcase[n->nombre16[k]];h=checksum16(h,(uint8_t)c);h=checksum16(h,(uint8_t)(c>>8));}if(h!=u16(stream+4))return VOLUMEN_CORRUPTO;}
        if(n->directorio && (!n->longitud || n->longitud%v->bytes_cluster || n->valida!=n->longitud))return VOLUMEN_CORRUPTO;
        r=nodo_valido(v,n);if(r)return r;return utf8(n->nombre16,n->unidades,n->nombre)?VOLUMEN_CORRUPTO:1;
    }return r;
}
static uint8_t checksum83(const uint8_t *e){uint8_t sum=0;for(unsigned i=0;i<11;i++)sum=(uint8_t)(((sum>>1)|(sum<<7))+e[i]);return sum;}
static uint16_t nombre_corto(uint8_t c,int minuscula){
    uint16_t u=c<128?c:fat_oem850[c-128];
    if(minuscula && ((u>='A' && u<='Z') || (u>=0xc0 && u<=0xd6) || (u>=0xd8 && u<=0xde)))u+=32;
    return u;
}
static int siguiente_fat32(struct fat_lector_volumen *v,struct fat_lector_iterador *it,struct fat_lector_nodo *n,const volatile uint8_t *cancel){
    uint8_t e[32];int r;while((r=entrada_raw(v,it,e,cancel))>0){
        if(!e[0]){it->terminado=1;return it->lfn_total?VOLUMEN_CORRUPTO:0;}if(e[0]==0xe5){it->lfn_esperado=it->lfn_total=0;continue;}
        if(e[11]==15){unsigned orden=e[0]&31;if(e[0]&0x40){it->lfn_total=orden;it->lfn_esperado=orden;it->lfn_checksum=e[13];for(unsigned j=0;j<260;j++)it->lfn[j]=0xffff;}
            if((e[0]&0xa0) || !orden || orden>20 || orden!=it->lfn_esperado || e[12] || u16(e+26) || e[13]!=it->lfn_checksum)return VOLUMEN_CORRUPTO;
            static const unsigned pos[]={1,3,5,7,9,14,16,18,20,22,24,28,30};for(unsigned j=0;j<13;j++)it->lfn[(orden-1)*13+j]=u16(e+pos[j]);it->lfn_esperado--;continue;
        }
        if(e[11]&8){it->lfn_total=0;continue;}
        memset(n,0,sizeof(*n));n->cluster=((uint32_t)u16(e+20)<<16)|u16(e+26);n->directorio=!!(e[11]&16);n->longitud=n->valida=n->directorio?(uint64_t)v->clusters*v->bytes_cluster:u32(e+28);
        if(it->lfn_total){if(it->lfn_esperado || checksum83(e)!=it->lfn_checksum)return VOLUMEN_CORRUPTO;unsigned lim=it->lfn_total*13;while(n->unidades<lim && it->lfn[n->unidades] && it->lfn[n->unidades]!=0xffff){if(n->unidades==255)return VOLUMEN_CORRUPTO;n->nombre16[n->unidades]=it->lfn[n->unidades];n->unidades++;}if(n->unidades<lim && it->lfn[n->unidades])return VOLUMEN_CORRUPTO;for(unsigned j=n->unidades+1;j<lim;j++)if(it->lfn[j]!=0xffff)return VOLUMEN_CORRUPTO;}
        else {for(unsigned j=0;j<8 && e[j]!=' ';j++){uint8_t c=e[j];if(!j && c==5)c=0xe5;n->nombre16[n->unidades++]=nombre_corto(c,e[12]&8);}if(e[8]!=' '){n->nombre16[n->unidades++]='.';for(unsigned j=8;j<11 && e[j]!=' ';j++){uint8_t c=e[j];n->nombre16[n->unidades++]=nombre_corto(c,e[12]&16);}}}
        it->lfn_total=it->lfn_esperado=0;
        if((n->unidades==1 && n->nombre16[0]=='.') || (n->unidades==2 && n->nombre16[0]=='.' && n->nombre16[1]=='.'))continue;
        r=nodo_valido(v,n);if(r)return r;return utf8(n->nombre16,n->unidades,n->nombre)?VOLUMEN_CORRUPTO:1;
    }return !r && it->lfn_total?VOLUMEN_CORRUPTO:r;
}
int fat_lector_siguiente(struct fat_lector_volumen *v,struct fat_lector_iterador *it,struct fat_lector_nodo *n,const volatile uint8_t *cancel){if(it->terminado)return 0;return v->exfat?siguiente_exfat(v,it,n,cancel):siguiente_fat32(v,it,n,cancel);}

static int validar_cadena(struct fat_lector_volumen *v,const struct fat_lector_nodo *n){
    struct fat_lector_cursor c;fat_lector_cursor_iniciar(&c,n);uint64_t cantidad=(n->longitud+v->bytes_cluster-1)/v->bytes_cluster;
    if(!cantidad)return VOLUMEN_CORRUPTO;
    int r=localizar(v,&c,(uint32_t)(cantidad-1),0);if(r)return r>0?VOLUMEN_CORRUPTO:r;
    uint32_t sig;r=siguiente_cluster(v,c.cluster,&sig,0);return r==1?0:r<0?r:VOLUMEN_CORRUPTO;
}
static int metadatos_exfat(struct fat_lector_volumen *v){
    struct fat_lector_nodo n=raiz(v),up={0},bm={0};struct fat_lector_iterador it;fat_lector_iterar(v,&n,&it);
    uint32_t sum_up=0;uint8_t e[32];int r,fin=0;
    while((r=entrada_raw(v,&it,e,0))>0){
        if(!e[0]){fin=1;break;}if(!(e[0]&0x80))continue;
        if(e[0]==0x81){if(bm.cluster || e[1])return VOLUMEN_NO_SOPORTADO;bm.cluster=u32(e+20);bm.longitud=bm.valida=u64(e+24);}
        else if(e[0]==0x82){if(up.cluster)return VOLUMEN_CORRUPTO;up.cluster=u32(e+20);up.longitud=up.valida=u64(e+24);sum_up=u32(e+4);}
        else if(e[0]==0xa1 || e[0]==0xa2)return VOLUMEN_NO_SOPORTADO;
    }
    (void)fin;if(r<0)return r;
    if(!valido(v,bm.cluster) || bm.longitud<((uint64_t)v->clusters+7)/8 || bm.longitud>(uint64_t)v->clusters*v->bytes_cluster || !valido(v,up.cluster) || !up.longitud || up.longitud>131072 || (up.longitud&1))return VOLUMEN_CORRUPTO;
    r=validar_cadena(v,&bm);if(r)return r;r=validar_cadena(v,&up);if(r)return r;
    fat_lector_cursor_iniciar(&v->bitmap,&bm);v->longitud_bitmap=bm.longitud;v->bitmap_pagina=UINT64_MAX;v->cluster_bitmap=bm.cluster;
    v->upcase=asignar_memoria(65536u*sizeof(uint16_t));if(!v->upcase)return VOLUMEN_NO_SOPORTADO;
    struct fat_lector_cursor c;fat_lector_cursor_iniciar(&c,&up);uint8_t pagina[512];uint32_t sum=0,pos=0;int salto=0;
    for(uint64_t off=0;off<up.longitud;off+=512){unsigned cantidad=(unsigned)(up.longitud-off<512?up.longitud-off:512);
        int64_t leidos=fat_lector_leer(v,&c,off,pagina,cantidad,0);if(leidos!=cantidad)return leidos<0?(int)leidos:VOLUMEN_CORRUPTO;
        for(unsigned j=0;j<cantidad;j++)sum=rotar(sum,pagina[j]);
        for(unsigned j=0;j<cantidad;j+=2){uint16_t valor=u16(pagina+j);
            if(salto){if(!valor || valor>65536-pos)return VOLUMEN_CORRUPTO;for(unsigned k=0;k<valor;k++){v->upcase[pos]=(uint16_t)pos;pos++;}salto=0;}
            else if(valor==0xffff && pos<65535)salto=1;
            else {if(pos>=65536)return VOLUMEN_CORRUPTO;v->upcase[pos++]=valor;}
        }
    }
    if(sum!=sum_up || pos!=65536 || salto)return VOLUMEN_CORRUPTO;
    r=asignado(v,v->raiz,0);if(r)return r;
    /* Todos los clusters de las cadenas de metadatos deben figurar asignados. */
    struct fat_lector_nodo *metas[]={&bm,&up};
    for(unsigned k=0;k<2;k++){fat_lector_cursor_iniciar(&c,metas[k]);unsigned total=(unsigned)((metas[k]->longitud+v->bytes_cluster-1)/v->bytes_cluster);for(unsigned j=0;j<total;j++){r=localizar(v,&c,j,0);if(r)return r;r=asignado(v,c.cluster,0);if(r)return r;}}
    return 0;
}
int fat_lector_montar(struct fat_lector_volumen *v,const struct particion *p){
    fat_lector_desmontar(v);v->particion=*p;v->exfat=p->formato==VOLUMEN_EXFAT;
    uint8_t s[512],b[512];int r;
    if(v->exfat){
        int principal=boot_exfat(v,0,s);
        if(principal==VOLUMEN_NO_SOPORTADO || principal==VOLUMEN_TEXFAT_NO_SOPORTADO || principal==VOLUMEN_SECTOR_NO_SOPORTADO || principal==VOLUMEN_DESCONECTADO){r=principal;goto error;}
        int respaldo=boot_exfat(v,12,b);
        if(principal){if(respaldo){r=principal;goto error;}v->respaldo=1;}
        else {
            if(!respaldo && memcmp(s+64,b+64,42)){r=VOLUMEN_CORRUPTO;goto error;}
            if(respaldo){v->respaldo=2;r=boot_exfat(v,0,s);if(r)goto error;}
        }
        r=metadatos_exfat(v);if(r)goto error;
    }else if(p->formato==VOLUMEN_FAT32){r=particion_leer(p,0,1,s,0);if(r)goto error;r=boot_fat32(v,s);if(r)goto error;}
    else {r=VOLUMEN_DESCONOCIDO_ERROR;goto error;}
    v->montado=1;return 0;
error:
    fat_lector_desmontar(v);v->error=r;return r;
}
static int decodificar_nombre(const char *s,size_t bytes,uint16_t *out,unsigned *n){
    unsigned j=0;for(size_t i=0;i<bytes;){uint8_t a=(uint8_t)s[i++];uint32_t cp;unsigned resto;
        if(a<0x80){cp=a;resto=0;}else if(a>=0xc2 && a<=0xdf){cp=a&31;resto=1;}else if(a>=0xe0 && a<=0xef){cp=a&15;resto=2;}else if(a>=0xf0 && a<=0xf4){cp=a&7;resto=3;}else return VOLUMEN_CORRUPTO;
        if(resto>bytes-i)return VOLUMEN_CORRUPTO;
        for(unsigned k=0;k<resto;k++){uint8_t b=(uint8_t)s[i++];if((b&0xc0)!=0x80)return VOLUMEN_CORRUPTO;cp=(cp<<6)|(b&63);}
        if((resto==1 && cp<128) || (resto==2 && cp<2048) || (resto==3 && cp<65536) || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) || !cp)return VOLUMEN_CORRUPTO;
        if(cp>=65536){if(j>253)return VOLUMEN_CORRUPTO;cp-=65536;out[j++]=(uint16_t)(0xd800|(cp>>10));out[j++]=(uint16_t)(0xdc00|(cp&1023));}
        else {if(j==255)return VOLUMEN_CORRUPTO;out[j++]=(uint16_t)cp;}
    }*n=j;return 0;
}
static uint16_t mayuscula(struct fat_lector_volumen *v,uint16_t c){
    if(v->upcase)return v->upcase[c];
    /* FAT no contiene Up-case Table; plegado ASCII/Latin-1 para nombres españoles. */
    if((c>='a' && c<='z') || (c>=0xe0 && c<=0xf6) || (c>=0xf8 && c<=0xfe))return c-32;
    if(c==0xff)return 0x178;
    return c;
}
int fat_lector_resolver(struct fat_lector_volumen *v,const char *ruta,struct fat_lector_nodo *out,const volatile uint8_t *cancel){
    if(!v->montado)return VOLUMEN_NO_ENCONTRADO;
    struct fat_lector_nodo actual=raiz(v);if(!ruta){*out=actual;return 0;}
    struct {uint64_t longitud,valida;uint32_t cluster;uint8_t contiguo;} padres[64];
    unsigned profundidad=0;
    while(*ruta){while(*ruta=='/' || *ruta=='\\')ruta++;if(!*ruta)break;
        const char *fin=ruta;while(*fin && *fin!='/' && *fin!='\\')fin++;
        if(fin-ruta==1 && ruta[0]=='.'){ruta=fin;continue;}
        if(!actual.directorio)return VOLUMEN_NO_ENCONTRADO;
        if(fin-ruta==2 && ruta[0]=='.' && ruta[1]=='.'){
            if(profundidad){profundidad--;memset(&actual,0,sizeof(actual));actual.directorio=1;
                actual.longitud=padres[profundidad].longitud;actual.valida=padres[profundidad].valida;
                actual.cluster=padres[profundidad].cluster;actual.contiguo=padres[profundidad].contiguo;}
            ruta=fin;continue;
        }
        if(profundidad==64)return VOLUMEN_NO_SOPORTADO;
        padres[profundidad].longitud=actual.longitud;padres[profundidad].valida=actual.valida;
        padres[profundidad].cluster=actual.cluster;padres[profundidad].contiguo=actual.contiguo;profundidad++;
        uint16_t buscado[255];unsigned n;int r=decodificar_nombre(ruta,(size_t)(fin-ruta),buscado,&n);if(r)return r;
        struct fat_lector_iterador it;fat_lector_iterar(v,&actual,&it);int hallado=0;
        while((r=fat_lector_siguiente(v,&it,&actual,cancel))>0){if(actual.unidades!=n)continue;unsigned i=0;while(i<n && mayuscula(v,buscado[i])==mayuscula(v,actual.nombre16[i]))i++;if(i==n){hallado=1;break;}}
        if(r<0)return r;if(!hallado)return VOLUMEN_NO_ENCONTRADO;ruta=fin;
    }*out=actual;return 0;
}
int fat_lector_abrir_stream(struct fat_lector_volumen *v,const char *ruta,void *fd_generico){
    struct vfs_descriptor_archivo *fd=fd_generico;struct fat_lector_nodo n;int r=fat_lector_resolver(v,ruta,&n,&fd->cancelado);if(r)return r;
    if(n.directorio)return VOLUMEN_NO_ENCONTRADO;
    fat_lector_cursor_iniciar((struct fat_lector_cursor *)fd->cursor,&n);fd->tamano=n.longitud;return 0;
}
int64_t fat_lector_leer_stream(struct fat_lector_volumen *v,void *fd_generico,void *buf,size_t n){struct vfs_descriptor_archivo *fd=fd_generico;int64_t r=fat_lector_leer(v,(struct fat_lector_cursor *)fd->cursor,fd->posicion,buf,n,&fd->cancelado);if(r>0)fd->posicion+=(uint64_t)r;return r;}
int fat_lector_listar(struct fat_lector_volumen *v,const char *ruta,unsigned pagina){
    struct fat_lector_nodo dir,n;int r=fat_lector_resolver(v,ruta,&dir,0);if(r)return r;
    struct fat_lector_iterador it;r=fat_lector_iterar(v,&dir,&it);if(r)return r;vfs_limpiar_catalogo();
    uint64_t saltar=(uint64_t)pagina*VFS_MAX_CATALOGO,indice=0;unsigned entregadas=0;
    while((r=fat_lector_siguiente(v,&it,&n,0))>0){if(indice++<saltar)continue;if(entregadas==VFS_MAX_CATALOGO){consola_imprimir_linea("Mas entradas: ls pagina <numero> (paginas desde 0)");break;}
        vfs_agregar_entrada_catalogo(n.nombre,n.longitud,n.directorio?VFS_NODO_DIRECTORIO:VFS_NODO_ARCHIVO);entregadas++;
        consola_imprimir("[");consola_imprimir_dec(entregadas);consola_imprimir("] ");consola_imprimir(n.nombre);consola_imprimir(n.directorio?"/":"");consola_imprimir("  ");consola_imprimir_dec(n.directorio?0:n.longitud);consola_imprimir_linea(" bytes");
    }
    if(r<0)vfs_limpiar_catalogo();return r<0?r:0;
}
static int tree(struct fat_lector_volumen *v,const struct fat_lector_nodo *dir,unsigned profundidad,uint32_t *ancestros){
    if(profundidad>=32)return VOLUMEN_NO_SOPORTADO;
    for(unsigned i=0;i<profundidad;i++)if(ancestros[i]==dir->cluster)return VOLUMEN_CORRUPTO;ancestros[profundidad]=dir->cluster;
    /* Iteradores separados: la recursión no pisa el estado del padre. */
    struct fat_lector_iterador *it=asignar_memoria(sizeof(*it));struct fat_lector_nodo *n=asignar_memoria(sizeof(*n));if(!it || !n){if(it)liberar_memoria(it);if(n)liberar_memoria(n);return VOLUMEN_NO_SOPORTADO;}
    int r=fat_lector_iterar(v,dir,it);if(!r)while((r=fat_lector_siguiente(v,it,n,0))>0){for(unsigned i=0;i<profundidad;i++)consola_imprimir("  ");consola_imprimir_linea(n->nombre);if(n->directorio){int sub=tree(v,n,profundidad+1,ancestros);if(sub){r=sub;break;}}}
    liberar_memoria(n);liberar_memoria(it);return r<0?r:0;
}
int fat_lector_tree(struct fat_lector_volumen *v,const char *ruta){struct fat_lector_nodo n;int r=fat_lector_resolver(v,ruta,&n,0);uint32_t ancestros[32];return r?r:tree(v,&n,0,ancestros);}
