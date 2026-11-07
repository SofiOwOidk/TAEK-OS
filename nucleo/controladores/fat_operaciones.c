#include "fat_operaciones.h"
#include "fat_lector.h"
#include "usb_msc.h"
#include "consola.h"
#include "../base/memoria.h"
#include "../base/sha256.h"

#define RUTA_MAX 4096
#define HASH_SECTORES 127
struct cambio {
    uint64_t lba;
    unsigned fase, escrito;
    uint8_t antes[512], despues[512];
    struct cambio *siguiente, *hash;
};
struct directorio {
    struct fat_lector_nodo nodo;
    uint32_t *cadena, total, capacidad;
    char ruta[RUTA_MAX];
};
struct operacion {
    struct fat_lector_volumen vol;
    struct cambio *cambios, *tabla[HASH_SECTORES];
    uint8_t datos[32768], espejo[32768];
    uint64_t cache_lba[8];
    uint8_t cache_valido[8],cache[8][512];
    unsigned fats, fsinfo, backup, reservados;
    uint32_t pista, libres;
    int dirty;
};
static uint8_t ocupado;
static uint16_t u16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p){return u16(p)|((uint32_t)u16(p+2)<<16);}
static void b16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void b32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++){p[i]=(uint8_t)n;n>>=8;}}
static void b64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++){p[i]=(uint8_t)n;n>>=8;}}
static uint16_t suma16(uint16_t n,uint8_t b){return (uint16_t)(((n>>1)|((uint32_t)n<<15))+b);}
static uint64_t sector_cluster(struct operacion *o,uint32_t c){return o->vol.datos+(uint64_t)(c-2)*o->vol.sectores_cluster;}
static int cluster_valido(struct operacion *o,uint32_t c){return c>=2 && (uint64_t)c<o->vol.clusters+UINT64_C(2);}
static int identidad(struct operacion *o){
    const struct usb_msc_dispositivo *d=usb_msc_obtener_dispositivo(o->vol.particion.unidad);
    if(!d || !d->activo || !d->listo || d->generacion!=o->vol.particion.generacion_msc)return VOLUMEN_DESCONECTADO;
    return d->tamano_sector==512?0:VOLUMEN_SECTOR_NO_SOPORTADO;
}
/* Commit y rollback no consumen cancelacion: deben poder restaurar metadatos
 * incluso si el servicio del lector ya ha enclavado Ctrl+C. */
static int io(struct operacion *o,uint64_t lba,unsigned n,void *p,int escritura){
    int r=identidad(o);if(r)return r;
    if(!n || n>64 || lba>UINT32_MAX || lba>=o->vol.sectores || n>o->vol.sectores-lba || o->vol.particion.inicio>UINT32_MAX-lba ||
       o->vol.particion.inicio+lba>UINT32_MAX-(n-1))return VOLUMEN_CORRUPTO;
    uint32_t fisico=(uint32_t)(o->vol.particion.inicio+lba);
    r=escritura?usb_msc_escribir_sectores(o->vol.particion.unidad,fisico,(uint16_t)n,p):
                 usb_msc_leer_sectores(o->vol.particion.unidad,fisico,(uint16_t)n,p);
    int id=identidad(o);return id?id:r?VOLUMEN_TRANSPORTE:0;
}
static int sync(struct operacion *o){int r=identidad(o);if(r)return r;r=usb_msc_sincronizar(o->vol.particion.unidad);int id=identidad(o);return id?id:r?VOLUMEN_TRANSPORTE:0;}
static struct cambio *buscar(struct operacion *o,uint64_t lba){for(struct cambio *c=o->tabla[lba%HASH_SECTORES];c;c=c->hash)if(c->lba==lba)return c;return 0;}
static int preparar(struct operacion *o,uint64_t lba,unsigned fase,struct cambio **out){
    struct cambio *c=buscar(o,lba);if(c){if(fase>c->fase)c->fase=fase;*out=c;return 0;}
    c=asignar_memoria(sizeof(*c));if(!c)return VOLUMEN_SIN_MEMORIA;
    int r=io(o,lba,1,c->antes,0);if(r){liberar_memoria(c);return r;}
    c->lba=lba;c->fase=fase;c->escrito=0;memcpy(c->despues,c->antes,512);
    c->siguiente=o->cambios;o->cambios=c;c->hash=o->tabla[lba%HASH_SECTORES];o->tabla[lba%HASH_SECTORES]=c;*out=c;return 0;
}
static int leer_plan(struct operacion *o,uint64_t lba,uint8_t *p){
    int r=identidad(o);if(r)return r;struct cambio *c=buscar(o,lba);if(c){memcpy(p,c->despues,512);return 0;}
    unsigned slot=(unsigned)(lba%8);if(!o->cache_valido[slot] || o->cache_lba[slot]!=lba){
        r=io(o,lba,1,o->cache[slot],0);if(r)return r;o->cache_valido[slot]=1;o->cache_lba[slot]=lba;}
    memcpy(p,o->cache[slot],512);return 0;
}
static int fat_leer(struct operacion *o,uint32_t cl,uint32_t *v){
    if(!cluster_valido(o,cl))return VOLUMEN_CORRUPTO;
    uint8_t s[512];int r=leer_plan(o,o->vol.fat+cl/128,s);if(r)return r;*v=u32(s+(cl%128)*4);if(!o->vol.exfat)*v&=0x0fffffff;return 0;
}
static int fat_escribir(struct operacion *o,uint32_t cl,uint32_t v,unsigned fase){
    if(!cluster_valido(o,cl))return VOLUMEN_CORRUPTO;
    for(unsigned f=0;f<o->fats;f++){struct cambio *c;int r=preparar(o,o->vol.fat+(uint64_t)f*o->vol.sectores_fat+cl/128,fase,&c);if(r)return r;
        uint8_t *p=c->despues+(cl%128)*4;b32(p,o->vol.exfat?v:(u32(p)&0xf0000000u)|(v&0x0fffffffu));}
    return 0;
}
static int eof(struct operacion *o,uint32_t n){return o->vol.exfat?n==UINT32_MAX:n>=0x0ffffff8u;}
static int vector_agregar(struct directorio *d,uint32_t cl){
    if(d->total==d->capacidad){uint32_t cap=d->capacidad?d->capacidad*2:16;if(cap<d->capacidad)return VOLUMEN_SIN_MEMORIA;
        uint32_t *p=asignar_memoria((uint64_t)cap*4);if(!p)return VOLUMEN_SIN_MEMORIA;if(d->total)memcpy(p,d->cadena,(size_t)d->total*4);
        if(d->cadena)liberar_memoria(d->cadena);d->cadena=p;d->capacidad=cap;}
    d->cadena[d->total++]=cl;return 0;
}
static int cadena(struct operacion *o,const struct fat_lector_nodo *n,struct directorio *d,int es_dir){
    if(!n->cluster)return !n->longitud && !es_dir?0:VOLUMEN_CORRUPTO;
    uint64_t esperados=(n->longitud+o->vol.bytes_cluster-1)/o->vol.bytes_cluster;
    uint32_t cl=n->cluster,tortuga=cl,potencia=1,pasos=0;
    for(uint32_t i=0;i<o->vol.clusters;i++){
        if(!cluster_valido(o,cl))return VOLUMEN_CORRUPTO;
        int r=vector_agregar(d,cl);if(r)return r;
        if(n->contiguo){if(d->total==esperados)return 0;cl++;continue;}
        uint32_t sig;r=fat_leer(o,cl,&sig);if(r)return r;
        if(eof(o,sig)){if(es_dir && (!o->vol.exfat || n->cluster==o->vol.raiz))return 0;return d->total==esperados?0:VOLUMEN_CORRUPTO;}
        if(!es_dir && d->total>=esperados)return VOLUMEN_CORRUPTO;
        if(!cluster_valido(o,sig) || sig==tortuga)return VOLUMEN_CORRUPTO;
        cl=sig;if(++pasos==potencia){tortuga=cl;pasos=0;if(potencia<UINT32_MAX/2)potencia*=2;}
    }return VOLUMEN_CORRUPTO;
}
static int directorio_cargar(struct operacion *o,const char *ruta,struct directorio *d){
    int r=fat_lector_resolver(&o->vol,ruta,&d->nodo,0);if(r)return r;if(!d->nodo.directorio)return VOLUMEN_NO_ENCONTRADO;
    size_t len=1,inicios[64];unsigned profundidad=0;d->ruta[0]='/';
    while(*ruta){while(*ruta=='/' || *ruta=='\\')ruta++;if(!*ruta)break;const char *fin=ruta;while(*fin && *fin!='/' && *fin!='\\')fin++;
        size_t n=(size_t)(fin-ruta);
        if(n==1 && ruta[0]=='.'){ruta=fin;continue;}
        if(n==2 && ruta[0]=='.' && ruta[1]=='.'){if(profundidad)len=inicios[--profundidad];ruta=fin;continue;}
        if(profundidad==64 || n+len+2>sizeof(d->ruta))return VOLUMEN_NOMBRE_INVALIDO;
        inicios[profundidad++]=len;if(len>1)d->ruta[len++]='/';memcpy(d->ruta+len,ruta,n);len+=n;ruta=fin;
    }d->ruta[len]=0;
    r=fat_lector_resolver(&o->vol,d->ruta,&d->nodo,0);if(r)return r;if(!d->nodo.directorio)return VOLUMEN_NO_ENCONTRADO;
    return cadena(o,&d->nodo,d,1);
}
static int ruta_separar(const char *ruta,char *padre,char *nombre){
    size_t len=0,ultimo=0;if(!ruta || !*ruta)return VOLUMEN_NOMBRE_INVALIDO;
    while(ruta[len]){if(len+1>=RUTA_MAX)return VOLUMEN_NOMBRE_INVALIDO;if(ruta[len]=='/' || ruta[len]=='\\')ultimo=len+1;len++;}
    if(ultimo==len)return VOLUMEN_NOMBRE_INVALIDO;
    size_t nl=len-ultimo;if(nl>=FAT_LECTOR_NOMBRE)return VOLUMEN_NOMBRE_INVALIDO;memcpy(nombre,ruta+ultimo,nl);nombre[nl]=0;
    if(ultimo){memmove(padre,ruta,ultimo);padre[ultimo]=0;}else {padre[0]='/';padre[1]=0;}
    return 0;
}
static int dir_sector(struct operacion *o,struct directorio *d,uint64_t pos,uint64_t *lba){
    uint64_t indice=pos/o->vol.bytes_cluster;if(indice>=d->total)return VOLUMEN_CORRUPTO;
    *lba=sector_cluster(o,d->cadena[indice])+(pos%o->vol.bytes_cluster)/512;return 0;
}
static int dir_leer(struct operacion *o,struct directorio *d,uint64_t pos,uint8_t e[32]){
    uint64_t lba;uint8_t s[512];int r=dir_sector(o,d,pos,&lba);if(r)return r;r=leer_plan(o,lba,s);if(!r)memcpy(e,s+pos%512,32);return r;
}
static int dir_escribir(struct operacion *o,struct directorio *d,uint64_t pos,const uint8_t e[32],unsigned fase){
    uint64_t lba;int r=dir_sector(o,d,pos,&lba);if(r)return r;struct cambio *c;r=preparar(o,lba,fase,&c);if(!r)memcpy(c->despues+pos%512,e,32);return r;
}
static int bitmap_pos(struct operacion *o,uint32_t cl,uint64_t *lba,unsigned *bit){
    uint64_t byte=((uint64_t)cl-2)/8,off=byte/512*512;
    int r=fat_lector_sector(&o->vol,&o->vol.bitmap,off,lba);if(r)return r;*bit=(cl-2)%4096;return 0;
}
static int asignacion(struct operacion *o,uint32_t cl,int *usado){
    if(!o->vol.exfat){uint32_t v;int r=fat_leer(o,cl,&v);if(!r)*usado=v!=0;return r;}
    uint64_t lba;unsigned bit;uint8_t s[512];int r=bitmap_pos(o,cl,&lba,&bit);if(r)return r;r=leer_plan(o,lba,s);if(!r)*usado=!!(s[bit/8]&(1u<<(bit%8)));return r;
}
static int asignar(struct operacion *o,uint32_t cl,int usado,unsigned fase){
    if(!o->vol.exfat){int r=fat_escribir(o,cl,usado?0x0fffffff:0,fase);if(!r){if(usado)o->libres--;else o->libres++;}return r;}
    uint64_t lba;unsigned bit;int r=bitmap_pos(o,cl,&lba,&bit);if(r)return r;struct cambio *c;r=preparar(o,lba,fase,&c);if(r)return r;
    if(usado)c->despues[bit/8]|=(uint8_t)(1u<<(bit%8));else c->despues[bit/8]&=(uint8_t)~(1u<<(bit%8));return 0;
}
static int cluster_reservar(struct operacion *o,uint32_t *salida){
    for(uint64_t i=o->pista;i<(uint64_t)o->vol.clusters+2;i++){
        if((i&127)==0 && consola_sondear_cancelacion())return VOLUMEN_CANCELADO;
        int usado,r=asignacion(o,(uint32_t)i,&usado);if(r)return r;if(usado)continue;
        r=asignar(o,(uint32_t)i,1,0);if(r)return r;o->pista=(uint32_t)i+1;*salida=(uint32_t)i;return 0;
    }return VOLUMEN_SIN_ESPACIO;
}
static int validar_asignados(struct operacion *o,struct directorio *d){
    for(uint32_t i=0;i<d->total;i++){int usado,r=asignacion(o,d->cadena[i],&usado);if(r)return r;if(!usado)return VOLUMEN_CORRUPTO;}return 0;
}
/* Un cluster referido por dos archivos (o por un archivo y un directorio)
 * nunca se libera ni se reutiliza. Validar tambien los nodos ajenos al comando
 * evita sobrescribirlos si su bit/FAT de asignacion esta marcado libre. */
static int reclamar(struct operacion *o,const struct fat_lector_nodo *n,uint8_t *duenos){
    struct directorio d={0};int r=cadena(o,n,&d,n->directorio);
    if(!r)r=validar_asignados(o,&d);
    if(!r)for(uint32_t i=0;i<d.total;i++){uint32_t cl=d.cadena[i]-2;uint8_t bit=(uint8_t)(1u<<(cl%8));
        if(duenos[cl/8]&bit){r=VOLUMEN_CORRUPTO;break;}duenos[cl/8]|=bit;}
    if(d.cadena)liberar_memoria(d.cadena);return r;
}
static int validar_duenos(struct operacion *o){
    uint64_t bytes=((uint64_t)o->vol.clusters+7)/8;
    if(bytes>UINT64_C(32)*1024*1024)return VOLUMEN_NO_SOPORTADO;
    uint8_t *duenos=asignar_memoria_cero(bytes);
    struct fat_lector_iterador *pila=asignar_memoria(64*sizeof(*pila));
    struct fat_lector_nodo *n=asignar_memoria(sizeof(*n));
    struct directorio *root=asignar_memoria_cero(sizeof(*root));
    int r=VOLUMEN_SIN_MEMORIA;if(!duenos || !pila || !n || !root)goto fin;
    r=directorio_cargar(o,"/",root);if(r)goto fin;
    r=reclamar(o,&root->nodo,duenos);if(r)goto fin;
    if(o->vol.exfat){
        r=reclamar(o,&o->vol.bitmap.nodo,duenos);if(r)goto fin;
        int encontrado=0;uint8_t e[32];
        for(uint64_t off=0;off<(uint64_t)root->total*o->vol.bytes_cluster;off+=32){
            r=dir_leer(o,root,off,e);if(r)goto fin;if(!e[0])break;
            if(e[0]==0x82){if(encontrado){r=VOLUMEN_CORRUPTO;goto fin;}encontrado=1;memset(n,0,sizeof(*n));
                n->cluster=u32(e+20);n->longitud=n->valida=u32(e+24)|((uint64_t)u32(e+28)<<32);
                r=reclamar(o,n,duenos);if(r)goto fin;}
            if(e[0]==0x85)off+=(uint64_t)e[1]*32;
        }if(!encontrado){r=VOLUMEN_CORRUPTO;goto fin;}
    }
    r=fat_lector_iterar(&o->vol,&root->nodo,pila);if(r)goto fin;unsigned profundidad=1;
    while(profundidad){
        if(consola_sondear_cancelacion()){r=VOLUMEN_CANCELADO;goto fin;}
        r=fat_lector_siguiente(&o->vol,&pila[profundidad-1],n,0);if(r<0)goto fin;if(!r){profundidad--;continue;}
        r=reclamar(o,n,duenos);if(r)goto fin;
        if(n->directorio){if(profundidad==64){r=VOLUMEN_NO_SOPORTADO;goto fin;}r=fat_lector_iterar(&o->vol,n,&pila[profundidad++]);if(r)goto fin;}
    }r=0;
fin:
    if(root){if(root->cadena)liberar_memoria(root->cadena);liberar_memoria(root);}if(n)liberar_memoria(n);if(pila)liberar_memoria(pila);if(duenos)liberar_memoria(duenos);return r;
}
static int actualizar_directorio_exfat(struct operacion *o,struct directorio *d){
    if(!o->vol.exfat || d->nodo.cluster==o->vol.raiz)return 0;
    char *padre=asignar_memoria(RUTA_MAX),*nombre=asignar_memoria(FAT_LECTOR_NOMBRE);struct directorio *p=asignar_memoria_cero(sizeof(*p));
    if(!padre || !nombre || !p){if(padre)liberar_memoria(padre);if(nombre)liberar_memoria(nombre);if(p)liberar_memoria(p);return VOLUMEN_SIN_MEMORIA;}
    size_t len=0;while(d->ruta[len])len++;while(len && (d->ruta[len-1]=='/' || d->ruta[len-1]=='\\'))len--;
    memcpy(padre,d->ruta,len);padre[len]=0;
    int r=ruta_separar(padre,padre,nombre);if(!r)r=directorio_cargar(o,padre,p);
    uint8_t e[32];uint16_t sum=0;uint64_t pos=d->nodo.entrada_posicion;
    if(!r)for(unsigned i=0;i<d->nodo.entrada_cantidad;i++){
        r=dir_leer(o,p,pos+i*32,e);if(r)break;
        if(i==1){e[1]=1;b64(e+8,(uint64_t)d->total*o->vol.bytes_cluster);b64(e+24,(uint64_t)d->total*o->vol.bytes_cluster);r=dir_escribir(o,p,pos+32,e,1);if(r)break;}
        for(unsigned j=0;j<32;j++)if(i || (j!=2 && j!=3))sum=suma16(sum,e[j]);
    }
    if(!r){r=dir_leer(o,p,pos,e);if(!r){b16(e+2,sum);r=dir_escribir(o,p,pos,e,1);}}
    if(p->cadena)liberar_memoria(p->cadena);liberar_memoria(p);liberar_memoria(nombre);liberar_memoria(padre);return r;
}
static int directorio_ampliar(struct operacion *o,struct directorio *d){
    uint32_t nuevo;int r=cluster_reservar(o,&nuevo);if(r)return r;
    if(o->vol.exfat && d->nodo.contiguo){for(uint32_t i=0;i<d->total;i++){r=fat_escribir(o,d->cadena[i],i+1==d->total?nuevo:d->cadena[i+1],0);if(r)return r;}d->nodo.contiguo=0;}
    else {r=fat_escribir(o,d->cadena[d->total-1],nuevo,0);if(r)return r;}
    r=fat_escribir(o,nuevo,o->vol.exfat?UINT32_MAX:0x0fffffff,0);if(r)return r;
    r=vector_agregar(d,nuevo);if(r)return r;
    for(unsigned s=0;s<o->vol.sectores_cluster;s++){struct cambio *c;r=preparar(o,sector_cluster(o,nuevo)+s,0,&c);if(r)return r;memset(c->despues,0,512);}
    return actualizar_directorio_exfat(o,d);
}
static int buscar_ranuras(struct operacion *o,struct directorio *d,unsigned n,uint64_t *pos,int *fin){
    uint64_t inicio=0,off=0;unsigned seguidas=0;int terminado=0;
    for(;;){
        if(off>=(uint64_t)d->total*o->vol.bytes_cluster){int r=directorio_ampliar(o,d);if(r)return r;}
        uint8_t e[32];int r=dir_leer(o,d,off,e);if(r)return r;if(!e[0])terminado=1;
        int libre=terminado || (o->vol.exfat?!(e[0]&128):e[0]==0xe5);
        if(libre){if(!seguidas)inicio=off;if(++seguidas==n){*pos=inicio;*fin=terminado;return 0;}}else seguidas=0;
        off+=32;if((off&4095)==0 && consola_sondear_cancelacion())return VOLUMEN_CANCELADO;
    }
}
static int nombre_validar(const char *s,uint16_t *out,unsigned *len){
    size_t n=0;while(s[n])n++;if(!n || s[n-1]=='.' || s[n-1]==' ')return VOLUMEN_NOMBRE_INVALIDO;
    int r=fat_lector_nombre_utf16(s,n,out,len);if(r)return VOLUMEN_NOMBRE_INVALIDO;
    if(!*len)return VOLUMEN_NOMBRE_INVALIDO;
    for(unsigned i=0;i<*len;i++)if(out[i]<32 || out[i]=='"' || out[i]=='*' || out[i]==':' || out[i]=='<' || out[i]=='>' || out[i]=='?' || out[i]=='|' || out[i]=='/' || out[i]=='\\')return VOLUMEN_NOMBRE_INVALIDO;
    return 0;
}
static uint8_t checksum83(const uint8_t *e){uint8_t n=0;for(unsigned i=0;i<11;i++)n=(uint8_t)(((n>>1)|(n<<7))+e[i]);return n;}
static int alias_ocupado(struct operacion *o,struct directorio *d,const uint8_t alias[11]){
    for(uint64_t off=0;off<(uint64_t)d->total*o->vol.bytes_cluster;off+=32){uint8_t e[32];int r=dir_leer(o,d,off,e);if(r)return r;if(!e[0])return 0;
        if(e[0]!=0xe5 && e[11]!=15 && !memcmp(e,alias,11))return 1;}return 0;
}
static int alias_generar(struct operacion *o,struct directorio *d,const uint16_t *nombre,unsigned n,uint8_t alias[11]){
    memset(alias,' ',11);unsigned dot=n;for(unsigned i=0;i<n;i++)if(nombre[i]=='.')dot=i;
    unsigned a=0;for(unsigned i=0;i<dot && a<2;i++){uint16_t c=nombre[i];if(c>='a'&&c<='z')c-=32;if((c>='A'&&c<='Z')||(c>='0'&&c<='9'))alias[a++]=(uint8_t)c;}
    while(a<2)alias[a++]='X';alias[2]='~';unsigned e=0;
    if(dot<n)for(unsigned i=dot+1;i<n && e<3;i++){uint16_t c=nombre[i];if(c>='a'&&c<='z')c-=32;if((c>='A'&&c<='Z')||(c>='0'&&c<='9'))alias[8+e++]=(uint8_t)c;}
    for(unsigned num=1;num<=99999;num++){unsigned x=num;for(int i=7;i>=3;i--){alias[i]=(uint8_t)('0'+x%10);x/=10;}int r=alias_ocupado(o,d,alias);if(r<0)return r;if(!r)return 0;}
    return VOLUMEN_SIN_ESPACIO;
}
static int entrada_crear(struct operacion *o,struct operacion *lector,struct directorio *d,const uint16_t *nombre,unsigned n,
                        struct directorio *srcdir,const struct fat_lector_nodo *src,uint32_t primero,int copia){
    unsigned entradas=o->vol.exfat?2+(n+14)/15:1+(n+12)/13;uint8_t alias[11];
    int r=0;if(!o->vol.exfat){r=alias_generar(o,d,nombre,n,alias);if(r)return r;}
    uint64_t pos;int fin;r=buscar_ranuras(o,d,entradas,&pos,&fin);if(r)return r;
    uint8_t prim[32],stream[32],e[32];
    if(o->vol.exfat){
        if(lector->vol.exfat){
            if(src->entrada_cantidad!=2+(src->unidades+14)/15)return VOLUMEN_NO_SOPORTADO;
            r=dir_leer(lector,srcdir,src->entrada_posicion,prim);if(r)return r;
            r=dir_leer(lector,srcdir,src->entrada_posicion+32,stream);if(r)return r;
        }else{
            /* New format: archive attribute and valid baseline DOS dates.
             * Cross-format copies preserve data/name, not source timestamps. */
            memset(prim,0,32);memset(stream,0,32);prim[0]=0x85;b16(prim+4,0x20);
            b32(prim+8,33u<<16);b32(prim+12,33u<<16);b32(prim+16,33u<<16);
            prim[22]=prim[23]=prim[24]=0x80;stream[0]=0xc0;b64(stream+24,src->longitud);
        }
        prim[1]=(uint8_t)(entradas-1);b16(prim+2,0);stream[3]=(uint8_t)n;
        uint16_t h=0;for(unsigned i=0;i<n;i++){uint16_t u=o->vol.upcase[nombre[i]];h=suma16(h,(uint8_t)u);h=suma16(h,(uint8_t)(u>>8));}b16(stream+4,h);
        b32(stream+20,primero);if(copia){stream[1]=1;b64(stream+8,src->longitud);}
        if(!src->longitud)stream[1]=1;
        uint16_t sum=0;for(unsigned i=0;i<entradas;i++){
            if(!i)memcpy(e,prim,32);else if(i==1)memcpy(e,stream,32);else {memset(e,0,32);e[0]=0xc1;for(unsigned j=0;j<15;j++){unsigned k=(i-2)*15+j;if(k<n)b16(e+2+j*2,nombre[k]);}}
            for(unsigned j=0;j<32;j++)if(i || (j!=2 && j!=3))sum=suma16(sum,e[j]);r=dir_escribir(o,d,pos+i*32,e,1);if(r)return r;
        }b16(prim+2,sum);r=dir_escribir(o,d,pos,prim,1);if(r)return r;
    }else {
        if(!lector->vol.exfat){r=dir_leer(lector,srcdir,src->entrada_posicion+(src->entrada_cantidad-1)*32,prim);if(r)return r;}
        else{memset(prim,0,32);prim[11]=0x20;b16(prim+16,33);b16(prim+18,33);b16(prim+24,33);b32(prim+28,(uint32_t)src->longitud);}
        memcpy(prim,alias,11);prim[12]=0;b16(prim+20,(uint16_t)(primero>>16));b16(prim+26,(uint16_t)primero);
        unsigned lfn=entradas-1;uint8_t sum=checksum83(alias);static const unsigned offsets[]={1,3,5,7,9,14,16,18,20,22,24,28,30};
        for(unsigned i=0;i<lfn;i++){unsigned ord=lfn-i;memset(e,0,32);e[0]=(uint8_t)(ord|(i?0:0x40));e[11]=15;e[13]=sum;
            for(unsigned j=0;j<13;j++){unsigned k=(ord-1)*13+j;b16(e+offsets[j],k<n?nombre[k]:k==n?0:0xffff);}r=dir_escribir(o,d,pos+i*32,e,1);if(r)return r;}
        r=dir_escribir(o,d,pos+lfn*32,prim,1);if(r)return r;
    }
    if(fin && pos+(uint64_t)entradas*32<(uint64_t)d->total*o->vol.bytes_cluster){memset(e,0,32);r=dir_escribir(o,d,pos+(uint64_t)entradas*32,e,1);}
    return r;
}
static int entrada_eliminar(struct operacion *o,struct directorio *d,const struct fat_lector_nodo *n){
    for(unsigned i=0;i<n->entrada_cantidad;i++){uint8_t e[32];int r=dir_leer(o,d,n->entrada_posicion+i*32,e);if(r)return r;
        e[0]=o->vol.exfat?(uint8_t)(e[0]&127):0xe5;r=dir_escribir(o,d,n->entrada_posicion+i*32,e,1);if(r)return r;}return 0;
}
static int info_libres(struct operacion *o){
    if(o->vol.exfat || !o->fsinfo || o->fsinfo==0xffff)return 0;
    for(unsigned i=0;i<(o->backup && o->backup!=0xffff?2u:1u);i++){
        uint64_t lba=o->fsinfo+(i?o->backup:0);if(lba>=o->reservados)return VOLUMEN_CORRUPTO;struct cambio *c;int r=preparar(o,lba,3,&c);if(r)return r;
        if(u32(c->despues)!=0x41615252 || u32(c->despues+484)!=0x61417272 || u32(c->despues+508)!=0xaa550000)return VOLUMEN_CORRUPTO;
        b32(c->despues+488,o->libres);b32(c->despues+492,o->pista<o->vol.clusters+UINT64_C(2)?o->pista:UINT32_MAX);
    }return 0;
}
static int marcar_dirty(struct operacion *o,int valor){
    uint8_t s[512];for(unsigned f=0;f<(o->vol.exfat?2u:o->fats);f++){
        uint64_t lba=o->vol.exfat?(uint64_t)(valor?f:1-f)*12:o->vol.fat+(uint64_t)f*o->vol.sectores_fat;
        int r=io(o,lba,1,s,0);if(r)return r;if(o->vol.exfat){uint16_t flags=u16(s+106);b16(s+106,valor?flags|2:flags&~2);s[112]=255;}
        else {uint32_t v=u32(s+4);b32(s+4,valor?v&~0x08000000u:v|0x08000000u);}
        o->dirty=1;r=io(o,lba,1,s,1);if(r)return r;
    }int r=sync(o);if(!r)o->dirty=valor;return r;
}
static int escribir_cambio(struct operacion *o,struct cambio *c,int restaurar){
    uint8_t s[512];memcpy(s,restaurar?c->antes:c->despues,512);
    if(o->dirty && !o->vol.exfat)for(unsigned f=0;f<o->fats;f++)if(c->lba==o->vol.fat+(uint64_t)f*o->vol.sectores_fat)b32(s+4,u32(s+4)&~0x08000000u);
    return io(o,c->lba,1,s,1);
}
static int commit(struct operacion *o){
    for(unsigned fase=0;fase<4;fase++){
        for(struct cambio *c=o->cambios;c;c=c->siguiente)if(c->fase==fase){c->escrito=1;int r=escribir_cambio(o,c,0);if(r)return r;}
        int r=sync(o);if(r)return r;
    }
    for(struct cambio *c=o->cambios;c;c=c->siguiente){int r=io(o,c->lba,1,o->datos,0);if(r)return r;
        memcpy(o->espejo,c->despues,512);if(!o->vol.exfat)for(unsigned f=0;f<o->fats;f++)if(c->lba==o->vol.fat+(uint64_t)f*o->vol.sectores_fat)b32(o->espejo+4,u32(o->espejo+4)&~0x08000000u);
        if(memcmp(o->datos,o->espejo,512))return VOLUMEN_CORRUPTO;
    }return 0;
}
static int rollback(struct operacion *o){
    int fallo=0;for(int fase=3;fase>=0;fase--)for(struct cambio *c=o->cambios;c;c=c->siguiente)if(c->escrito && c->fase==(unsigned)fase){int r=escribir_cambio(o,c,1);if(r)fallo=r;}
    int r=sync(o);if(r)fallo=r;if(!fallo)fallo=marcar_dirty(o,0);return fallo;
}
static int preflight(struct operacion *o,const struct particion *p){
    int r=fat_lector_montar(&o->vol,p);if(r)return r;r=io(o,0,1,o->datos,0);if(r)return r;
    if(o->vol.exfat){if(o->vol.respaldo || (u16(o->datos+106)&7))return VOLUMEN_CORRUPTO;
        r=io(o,12,1,o->espejo,0);if(r)return r;if((u16(o->espejo+106)&7) || memcmp(o->datos+64,o->espejo+64,42))return VOLUMEN_CORRUPTO;o->fats=1;
    }else {
        o->fats=o->datos[16];o->reservados=u16(o->datos+14);o->fsinfo=u16(o->datos+48);o->backup=u16(o->datos+50);
        if(u16(o->datos+40))return VOLUMEN_NO_SOPORTADO;
        for(uint32_t s=0;s<o->vol.sectores_fat;){unsigned n=o->vol.sectores_fat-s;if(n>64)n=64;r=io(o,o->vol.fat+s,n,o->datos,0);if(r)return r;
            if(!s && (u32(o->datos+4)&0x0c000000u)!=0x0c000000u)return VOLUMEN_CORRUPTO;
            if(o->fats==2){r=io(o,o->vol.fat+o->vol.sectores_fat+s,n,o->espejo,0);if(r)return r;if(memcmp(o->datos,o->espejo,(size_t)n*512))return VOLUMEN_CORRUPTO;}
            for(unsigned j=0;j<n*128;j++){uint64_t cl=(uint64_t)s*128+j;if(cl>=2 && cl<o->vol.clusters+UINT64_C(2) && !(u32(o->datos+j*4)&0x0fffffffu))o->libres++;}
            s+=n;if(consola_sondear_cancelacion())return VOLUMEN_CANCELADO;
        }
    }o->pista=2;return 0;
}
static int copiar_datos(struct operacion *o,struct operacion *lector,const struct fat_lector_nodo *src,struct directorio *nuevo){
    struct fat_lector_cursor *cursor=asignar_memoria(sizeof(*cursor));if(!cursor)return VOLUMEN_SIN_MEMORIA;fat_lector_cursor_iniciar(cursor,src);
    struct sha256 h,verificado;uint8_t digest[32],real[32];sha256_iniciar(&h);sha256_iniciar(&verificado);int r=0;
    for(uint64_t off=0;off<src->longitud;){
        if(consola_sondear_cancelacion()){r=VOLUMEN_CANCELADO;break;}
        unsigned n=(unsigned)(src->longitud-off>32768?32768:src->longitud-off);
        unsigned dentro=(unsigned)(off%o->vol.bytes_cluster);if(n>o->vol.bytes_cluster-dentro)n=o->vol.bytes_cluster-dentro;
        int id=identidad(lector);if(id){r=id;break;}
        int64_t leidos=fat_lector_leer(&lector->vol,cursor,off,o->datos,n,0);if(leidos!=n){r=leidos<0?(int)leidos:VOLUMEN_CORRUPTO;break;}
        sha256_actualizar(&h,o->datos,n);unsigned bloques=(n+511)/512;memset(o->datos+n,0,bloques*512-n);
        uint64_t lba=sector_cluster(o,nuevo->cadena[off/o->vol.bytes_cluster])+dentro/512;
        r=io(o,lba,bloques,o->datos,1);if(r)break;off+=n;
    }
    if(!r)r=sync(o);
    if(!r)for(uint64_t off=0;off<src->longitud;){
        if(consola_sondear_cancelacion()){r=VOLUMEN_CANCELADO;break;}
        unsigned n=(unsigned)(src->longitud-off>32768?32768:src->longitud-off),dentro=(unsigned)(off%o->vol.bytes_cluster);
        if(n>o->vol.bytes_cluster-dentro)n=o->vol.bytes_cluster-dentro;
        r=io(o,sector_cluster(o,nuevo->cadena[off/o->vol.bytes_cluster])+dentro/512,(n+511)/512,o->datos,0);if(r)break;
        sha256_actualizar(&verificado,o->datos,n);off+=n;
    }
    if(!r){sha256_final(&h,digest);sha256_final(&verificado,real);if(memcmp(digest,real,32))r=VOLUMEN_CORRUPTO;}
    liberar_memoria(cursor);return r;
}
static int operar_interno(const struct particion *p,const struct particion *fuente,const char *origen,const char *destino,enum fat_operacion op){
    if(!p || !origen || (op!=FAT_OPERACION_ELIMINAR && !destino) || op<FAT_OPERACION_COPIAR || op>FAT_OPERACION_ELIMINAR)return VOLUMEN_NOMBRE_INVALIDO;
    if(p->formato!=VOLUMEN_FAT32 && p->formato!=VOLUMEN_EXFAT)return VOLUMEN_SOLO_LECTURA;
    struct operacion *o=asignar_memoria_cero(sizeof(*o));struct directorio *sd=asignar_memoria_cero(sizeof(*sd)),*dd=asignar_memoria_cero(sizeof(*dd));
    struct operacion *lector=o;
    if(fuente)lector=asignar_memoria_cero(sizeof(*lector));
    struct directorio *sc=asignar_memoria_cero(sizeof(*sc)),*nc=asignar_memoria_cero(sizeof(*nc));
    char *padre=asignar_memoria(RUTA_MAX),*nombre=asignar_memoria(FAT_LECTOR_NOMBRE);struct fat_lector_nodo *src=asignar_memoria(sizeof(*src));
    int r=VOLUMEN_SIN_MEMORIA;
    if(!o || !lector || !sd || !dd || !sc || !nc || !padre || !nombre || !src)goto fin;
    r=preflight(o,p);if(r)goto fin;if(fuente){r=preflight(lector,fuente);if(r)goto fin;}
    r=fat_lector_resolver(&lector->vol,origen,src,0);if(r)goto fin;if(src->directorio){r=VOLUMEN_NO_SOPORTADO;goto fin;}
    r=ruta_separar(origen,padre,nombre);if(r)goto fin;r=directorio_cargar(lector,padre,sd);if(r)goto fin;
    r=cadena(lector,src,sc,0);if(r)goto fin;r=validar_asignados(lector,sc);if(r)goto fin;
    if(fuente){r=validar_duenos(lector);if(r)goto fin;}
    r=validar_duenos(o);if(r)goto fin;
    if(op!=FAT_OPERACION_ELIMINAR){
        struct fat_lector_nodo *existente=asignar_memoria(sizeof(*existente));if(!existente){r=VOLUMEN_SIN_MEMORIA;goto fin;}
        r=fat_lector_resolver(&o->vol,destino,existente,0);liberar_memoria(existente);if(!r){r=VOLUMEN_EXISTE;goto fin;}if(r!=VOLUMEN_NO_ENCONTRADO)goto fin;
        r=ruta_separar(destino,padre,nombre);if(r)goto fin;r=directorio_cargar(o,padre,dd);if(r)goto fin;
        uint16_t utf16[255];unsigned unidades;r=nombre_validar(nombre,utf16,&unidades);if(r)goto fin;
        uint32_t primero=src->cluster;
        if(op==FAT_OPERACION_COPIAR){
            if(!o->vol.exfat && src->longitud>UINT32_MAX){r=VOLUMEN_NO_SOPORTADO;goto fin;}
            uint64_t cantidad=src->longitud/o->vol.bytes_cluster+(src->longitud%o->vol.bytes_cluster!=0);
            if(cantidad>o->vol.clusters){r=VOLUMEN_SIN_ESPACIO;goto fin;}
            for(uint32_t i=0;i<cantidad;i++){uint32_t cl;r=cluster_reservar(o,&cl);if(r)goto fin;r=vector_agregar(nc,cl);if(r)goto fin;
                if(i){r=fat_escribir(o,nc->cadena[i-1],cl,0);if(r)goto fin;}r=fat_escribir(o,cl,o->vol.exfat?UINT32_MAX:0x0fffffff,0);if(r)goto fin;}
            primero=nc->total?nc->cadena[0]:0;
        }
        r=entrada_crear(o,lector,dd,utf16,unidades,sd,src,primero,op==FAT_OPERACION_COPIAR);if(r)goto fin;
    }
    if(op!=FAT_OPERACION_COPIAR){r=entrada_eliminar(o,sd,src);if(r)goto fin;}
    if(op==FAT_OPERACION_ELIMINAR)for(uint32_t i=0;i<sc->total;i++){r=asignar(o,sc->cadena[i],0,2);if(r)goto fin;if(o->vol.exfat && !src->contiguo){r=fat_escribir(o,sc->cadena[i],0,2);if(r)goto fin;}}
    r=info_libres(o);if(r)goto fin;if(consola_sondear_cancelacion()){r=VOLUMEN_CANCELADO;goto fin;}
    r=sync(o);if(r)goto fin;r=marcar_dirty(o,1);if(r)goto fallo;
    if(op==FAT_OPERACION_COPIAR){r=copiar_datos(o,lector,src,nc);if(r)goto fallo;}
    r=commit(o);if(r)goto fallo;r=marcar_dirty(o,0);if(r)goto fallo;goto fin;
fallo:
    if(o->dirty){int restaurado=rollback(o);if(restaurado){consola_imprimir_linea("[ARCHIVOS] Fallo de restauracion; volumen Dirty. Revisar el medio antes de nuevas escrituras.");r=restaurado;}}
fin:
    if(fuente && lector){fat_lector_desmontar(&lector->vol);liberar_memoria(lector);}
    if(o){while(o->cambios){struct cambio *c=o->cambios;o->cambios=c->siguiente;liberar_memoria(c);}fat_lector_desmontar(&o->vol);liberar_memoria(o);}
    struct directorio *dirs[]={sd,dd,sc,nc};for(unsigned i=0;i<4;i++)if(dirs[i]){if(dirs[i]->cadena)liberar_memoria(dirs[i]->cadena);liberar_memoria(dirs[i]);}
    if(padre)liberar_memoria(padre);if(nombre)liberar_memoria(nombre);if(src)liberar_memoria(src);
    return r;
}
int fat_operar_archivo(const struct particion *p,const char *origen,const char *destino,enum fat_operacion op){
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return VOLUMEN_OCUPADO;
    int r=operar_interno(p,0,origen,destino,op);__atomic_clear(&ocupado,__ATOMIC_RELEASE);return r;
}
int fat_operar_entre(const struct particion *src,const char *origen,const struct particion *dst,const char *destino,int mover){
    if(!src || !dst || !origen || !destino || (mover!=0 && mover!=1))return VOLUMEN_NOMBRE_INVALIDO;
    if((src->formato!=VOLUMEN_FAT32 && src->formato!=VOLUMEN_EXFAT) ||
       (dst->formato!=VOLUMEN_FAT32 && dst->formato!=VOLUMEN_EXFAT))return VOLUMEN_SOLO_LECTURA;
    if(src->unidad==dst->unidad && src->generacion_msc==dst->generacion_msc){
        if(src->inicio==dst->inicio){if(src->sectores!=dst->sectores || src->formato!=dst->formato)return VOLUMEN_CORRUPTO;
            return fat_operar_archivo(src,origen,destino,mover?FAT_OPERACION_MOVER:FAT_OPERACION_COPIAR);}
        if(src->inicio<dst->inicio?src->sectores>dst->inicio-src->inicio:dst->sectores>src->inicio-dst->inicio)return VOLUMEN_CORRUPTO;
    }
    if(__atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return VOLUMEN_OCUPADO;
    int r=operar_interno(dst,src,origen,destino,FAT_OPERACION_COPIAR);
    if(!r && mover){
        /* Destination is flushed, reread, hashed and clean before deleting.
         * A failed delete keeps the destination; callers report partial move. */
        r=operar_interno(src,0,origen,0,FAT_OPERACION_ELIMINAR);
        if(r)r=VOLUMEN_COPIA_CONSERVADA;
    }
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);return r;
}
