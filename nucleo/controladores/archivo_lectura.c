#include "archivo_lectura.h"
#include "fat_lector.h"
#include "consola.h"
#include "../base/memoria.h"
#include "../base/sha256.h"
struct lectura_hash {
    struct fat_lector_volumen vol;
    struct fat_lector_nodo nodo;
    struct fat_lector_cursor cursor;
    struct sha256 hash;
    uint8_t bloque[32768];
};
int archivo_calcular_sha256(const struct particion *part,const char *nombre,uint8_t digest[32],uint64_t *bytes){
    if(!part || !nombre || !digest || !bytes)return VOLUMEN_NO_SOPORTADO;
    *bytes=0;memset(digest,0,32);
    struct lectura_hash *l=asignar_memoria(sizeof(*l));if(!l)return VOLUMEN_NO_SOPORTADO;
    memset(l,0,sizeof(*l));int r=fat_lector_montar(&l->vol,part);
    if(!r)r=fat_lector_resolver(&l->vol,nombre,&l->nodo,0);
    if(!r && (l->nodo.directorio || l->nodo.longitud>UINT64_C(1024)*1024*1024 || l->nodo.valida!=l->nodo.longitud))r=VOLUMEN_NO_SOPORTADO;
    if(!r){
        fat_lector_cursor_iniciar(&l->cursor,&l->nodo);sha256_iniciar(&l->hash);
        for(uint64_t off=0;off<l->nodo.longitud;){
            if(consola_sondear_cancelacion()){r=VOLUMEN_CANCELADO;break;}
            unsigned n=l->nodo.longitud-off>sizeof(l->bloque)?sizeof(l->bloque):(unsigned)(l->nodo.longitud-off);
            int64_t leidos=fat_lector_leer(&l->vol,&l->cursor,off,l->bloque,n,0);
            if(leidos!=(int64_t)n){r=leidos<0?(int)leidos:VOLUMEN_CORRUPTO;break;}
            sha256_actualizar(&l->hash,l->bloque,n);off+=n;
        }
        if(!r){sha256_final(&l->hash,digest);*bytes=l->nodo.longitud;}
    }
    fat_lector_desmontar(&l->vol);liberar_memoria(l);return r;
}
