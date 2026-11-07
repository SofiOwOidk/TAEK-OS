#include "driver_elf.h"
#include "driver_abi.h"
#include "../base/memoria.h"
struct elf_cabecera {uint8_t ident[16];uint16_t tipo,maquina;uint32_t version;uint64_t entrada,phoff,shoff;uint32_t flags;uint16_t ehsize,phsize,phnum,shsize,shnum,shstr;};
struct elf_simbolo {uint32_t nombre;uint8_t info,otro;uint16_t seccion;uint64_t valor,tamano;};
struct elf_rela {uint64_t offset,info;int64_t addend;};
_Static_assert(sizeof(struct elf_cabecera)==64,"ELF64 header");
_Static_assert(sizeof(struct driver_elf_seccion)==64,"ELF64 section");
_Static_assert(sizeof(struct elf_simbolo)==24,"ELF64 symbol");
_Static_assert(sizeof(struct elf_rela)==24,"ELF64 rela");
#define S_WRITE 1u
#define S_ALLOC 2u
#define S_EXEC 4u
static int rango(uint64_t off,uint64_t len,size_t cap){return off<=cap && len<=cap-off;}
static int nombre(const uint8_t *f,size_t n,const struct driver_elf_seccion *strings,uint32_t off,const char *want){
    if(strings->tipo!=3 || !rango(strings->offset,strings->tamano,n) || off>=strings->tamano)return -1;
    const char *s=(const char *)f+strings->offset+off;uint64_t left=strings->tamano-off;if(left>1024)left=1024;int equal=1;
    for(uint64_t i=0;i<left;i++){if(!s[i])return equal && !*want?1:0;if(equal){if(*want && *want==s[i])want++;else equal=0;}}
    return -1;
}
static int simbolo(const uint8_t *f,size_t n,const struct driver_elf_plan *p,unsigned tab,uint64_t idx,struct elf_simbolo *out){
    if(tab>=p->total)return -1;const struct driver_elf_seccion *s=&p->secciones[tab];
    if(s->tipo!=2 || s->entrada!=24 || idx>=s->tamano/24 || !rango(s->offset,s->tamano,n))return -1;
    memcpy(out,f+s->offset+idx*24,24);return 0;
}
int driver_elf_planificar(const void *input,size_t n,struct driver_elf_plan *p){
    if(!input || !p || n<64 || n>32u*1024u*1024u)return -1;
    const uint8_t *f=input;struct elf_cabecera h;memcpy(&h,f,64);memset(p,0,sizeof(*p));
    const uint8_t magic[]={127,'E','L','F',2,1,1};
    if(memcmp(h.ident,magic,7) || (h.ident[7]!=0 && h.ident[7]!=3) || h.tipo!=1 || h.maquina!=62 || h.version!=1 || h.flags || h.entrada || h.phnum || h.ehsize!=64 || h.shsize!=64 || h.shnum<2 || h.shnum>DRIVER_ELF_SECCIONES || h.shstr>=h.shnum || !rango(h.shoff,(uint64_t)h.shnum*64,n))return -1;
    p->total=h.shnum;unsigned tables=0,found=0;uint64_t bytes=0,relocaciones=0;
    for(unsigned i=0;i<p->total;i++) {
        struct driver_elf_seccion *s=&p->secciones[i];memcpy(s,f+h.shoff+(uint64_t)i*64,64);
        if(i==0){if(s->tipo || s->flags || s->tamano)return -1;continue;}
        if(s->tipo!=8 && !rango(s->offset,s->tamano,n))return -1;
        if(s->alineacion && ((s->alineacion&(s->alineacion-1)) || s->alineacion>4096))return -1;
        if(s->flags&S_ALLOC){
            /* SHF_X86_64_LARGE is emitted by the required large code model. */
            if((s->tipo!=1 && s->tipo!=8) || (s->flags&~UINT64_C(0x10000037)) || ((s->flags&(S_WRITE|S_EXEC))==(S_WRITE|S_EXEC)) || (s->tipo==8 && (s->flags&S_EXEC)))return -1;
            if(s->tamano>DRIVER_ELF_MAX)return -1;
            p->posiciones[i]=bytes;bytes+=(s->tamano+4095)&~UINT64_C(4095);if(bytes>DRIVER_ELF_MAX)return -1;
        }
        if(s->tipo==2){if(++tables>1 || s->entrada!=24 || s->tamano%24 || s->tamano/24>131072u || s->enlace>=p->total || s->info>s->tamano/24)return -1;}
        if(s->tipo==4){relocaciones+=s->tamano/24;if(relocaciones>524288u || s->entrada!=24 || s->tamano%24 || s->enlace>=p->total || s->info>=p->total)return -1;}
        /* REL, dynamic linking, compressed sections and TLS are unsupported. */
        if(s->tipo==9 || s->tipo==6 || (s->flags&UINT64_C(0xc00)))return -1;
    }
    if(!bytes || !tables)return -1;p->bytes=(uint32_t)bytes;
    for(unsigned i=1;i<p->total;i++) {
        const struct driver_elf_seccion *s=&p->secciones[i];
        if(nombre(f,n,&p->secciones[h.shstr],s->nombre,"")<0)return -1;
        if(s->tipo==2) {
            const struct driver_elf_seccion *str=&p->secciones[s->enlace];
            for(uint64_t j=0;j<s->tamano/24;j++) {
                struct elf_simbolo sym;if(simbolo(f,n,p,i,j,&sym))return -1;
                int match=nombre(f,n,str,sym.nombre,"taek_driver");if(match<0 || (sym.otro&~3u))return -1;
                if(!j){if(sym.nombre || sym.info || sym.seccion || sym.valor || sym.tamano)return -1;continue;}
                if(sym.seccion==0xfff1 && (sym.info&15)==4 && !sym.valor && !sym.tamano)continue; /* FILE metadata */
                if(sym.seccion>=p->total || !sym.seccion) { /* no imports, ABS values, COMMON or extended indexes */
                    return -1;
                }
                const struct driver_elf_seccion *own=&p->secciones[sym.seccion];
                if(!rango(sym.valor,sym.tamano,own->tamano))return -1;
                if(match) {
                    if(found++ || !(own->flags&S_ALLOC) || (own->flags&S_EXEC) || (sym.info&15)!=1 || sym.tamano!=sizeof(struct taek_driver))return -1;
                    p->descriptor_seccion=sym.seccion;p->descriptor_offset=sym.valor;
                }
            }
        }
        if(s->tipo==4) {
            if(p->secciones[s->enlace].tipo!=2)return -1;
            const struct driver_elf_seccion *target=&p->secciones[s->info];
            for(uint64_t j=0;j<s->tamano/24;j++) {
                struct elf_rela r;memcpy(&r,f+s->offset+j*24,24);uint32_t type=(uint32_t)r.info;
                unsigned width=type==0?0:type==1?8:(type==2 || type==4 || type==10 || type==11)?4:99;
                if(width==99 || !rango(r.offset,width,target->tamano))return -1;
                struct elf_simbolo sym;if(simbolo(f,n,p,s->enlace,r.info>>32,&sym))return -1;
                if(width && (target->flags&S_ALLOC) && (!sym.seccion || sym.seccion>=p->total || !(p->secciones[sym.seccion].flags&S_ALLOC)))return -1;
            }
        }
    }
    return found==1?0:-1;
}
int driver_elf_materializar(const void *input,size_t n,const struct driver_elf_plan *p,void *output,size_t cap,uint64_t base){
    if(!input || !p || !output || cap<p->bytes || !p->bytes || base>UINT64_MAX-p->bytes)return -1;
    const uint8_t *f=input;uint8_t *out=output;memset(out,0,p->bytes);
    for(unsigned i=1;i<p->total;i++) {
        const struct driver_elf_seccion *s=&p->secciones[i];
        if(!(s->flags&S_ALLOC) || s->tipo==8)continue;
        if(!rango(s->offset,s->tamano,n) || !rango(p->posiciones[i],s->tamano,cap))return -1;
        memcpy(out+p->posiciones[i],f+s->offset,s->tamano);
    }
    for(unsigned i=1;i<p->total;i++) {
        const struct driver_elf_seccion *s=&p->secciones[i];
        if(s->tipo!=4 || !(p->secciones[s->info].flags&S_ALLOC))continue;
        for(uint64_t j=0;j<s->tamano/24;j++) {
            struct elf_rela r;memcpy(&r,f+s->offset+j*24,24);uint32_t type=(uint32_t)r.info;if(!type)continue;
            struct elf_simbolo sym;if(simbolo(f,n,p,s->enlace,r.info>>32,&sym))return -1;
            uint64_t symbol=base+p->posiciones[sym.seccion]+sym.valor;
            uint64_t place=base+p->posiciones[s->info]+r.offset;
            /* Mathematical signed arithmetic avoids wraparound accepting overflow. */
            __int128 value=(__int128)symbol+r.addend;if(type==2 || type==4)value-=(__int128)place;
            unsigned width=type==1?8:4;
            if((type==1 && (value<0 || value>UINT64_MAX)) || ((type==2 || type==4 || type==11) && (value<INT32_MIN || value>INT32_MAX)) || (type==10 && (value<0 || value>UINT32_MAX)))return -1;
            uint64_t v=(uint64_t)value;memcpy(out+p->posiciones[s->info]+r.offset,&v,width);
        }
    }return 0;
}
int driver_elf_rango(const struct driver_elf_plan *p,uint64_t base,uint64_t ptr,size_t len,int exec){
    if(!p || ptr<base)return 0;uint64_t off=ptr-base;
    for(unsigned i=1;i<p->total;i++){const struct driver_elf_seccion *s=&p->secciones[i];
        if(!(s->flags&S_ALLOC) || (exec && !(s->flags&S_EXEC)) || off<p->posiciones[i])continue;
        if(rango(off-p->posiciones[i],len,s->tamano))return 1;
    }return 0;
}
