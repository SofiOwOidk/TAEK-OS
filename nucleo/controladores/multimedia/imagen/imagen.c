/* stb_image v2.30, revisión f58f558c120e9b32c217290b80bad1a0729fbb2c.
 * SHA256 upstream: 594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3.
 * Adaptación única del header: STBI_NO_STD_HEADERS protege libc; licencia al pie. */
#include "imagen.h"
#include "../../vfs.h"
#include "../../../base/memoria.h"
#define IMAGEN_PRESUPUESTO (128u*1024u*1024u)
typedef struct {size_t bytes;uint64_t reservado;} cabecera;
static size_t usados,pico;
static void *reservar(size_t n){
    if(n>IMAGEN_PRESUPUESTO-usados || n>SIZE_MAX-sizeof(cabecera))return NULL;
    cabecera *p=asignar_memoria(n+sizeof(*p));if(!p)return NULL;p->bytes=n;usados+=n;if(usados>pico)pico=usados;return p+1;
}
void imagen_liberar(void *ptr){if(ptr){cabecera *p=(cabecera *)ptr-1;usados-=p->bytes;liberar_memoria(p);}}
static void *cambiar(void *ptr,size_t n){if(!ptr)return reservar(n);cabecera *p=(cabecera *)ptr-1;size_t anterior=p->bytes;if(n>IMAGEN_PRESUPUESTO-(usados-anterior) || n>SIZE_MAX-sizeof(*p))return NULL;cabecera *q=reasignar_memoria(p,n+sizeof(*p));if(!q)return NULL;q->bytes=n;usados=usados-anterior+n;if(usados>pico)pico=usados;return q+1;}
size_t imagen_memoria_pico(void){return pico;}
#define STBI_MALLOC(n) reservar(n)
#define STBI_REALLOC(p,n) cambiar(p,n)
#define STBI_FREE(p) imagen_liberar(p)
#define STBI_NO_STD_HEADERS
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS 8192
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
typedef struct {int fd,error;} origen;
static int leer(void *u,char *p,int n){origen *o=u;if(n<0 || o->error)return 0;int64_t r=vfs_leer(o->fd,p,(size_t)n);if(r<0){o->error=(int)r;return 0;}return (int)r;}
static void saltar(void *u,int n){origen *o=u;if(vfs_buscar(o->fd,n,VFS_SEEK_CUR)<0)o->error=VOLUMEN_CORRUPTO;}
static int eof(void *u){origen *o=u;return o->error || vfs_posicion_fd(o->fd)>=vfs_tamano_fd(o->fd);}
static uint32_t be32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static uint32_t crc(uint32_t c,const uint8_t *p,size_t n){while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^(0xedb88320u&-(c&1));}return c;}
static int validar_png(int fd,uint64_t total){
    /* stb omite CRC PNG; validarlos en ventanas antes de confiar en los chunks. */
    uint64_t pos=8;int cab=0,datos=0;uint8_t h[8],pagina[4096],esperado[4];
    while(pos<total){
        if(total-pos<12)return VOLUMEN_CORRUPTO;
        int64_t leidos=vfs_leer_en(fd,pos,h,8);if(leidos!=8)return leidos<0?(int)leidos:VOLUMEN_CORRUPTO;
        uint32_t n=be32(h);if(n>total-pos-12)return VOLUMEN_CORRUPTO;
        if(!cab && (memcmp(h+4,"IHDR",4) || n!=13))return VOLUMEN_CORRUPTO;
        if(!memcmp(h+4,"IDAT",4))datos=1;
        uint32_t sum=crc(~0u,h+4,4);
        for(uint64_t j=0;j<n;){size_t k=n-j>sizeof(pagina)?sizeof(pagina):(size_t)(n-j);int64_t r=vfs_leer_en(fd,pos+8+j,pagina,k);if(r!=(int64_t)k)return r<0?(int)r:VOLUMEN_CORRUPTO;sum=crc(sum,pagina,k);j+=k;}
        leidos=vfs_leer_en(fd,pos+8+n,esperado,4);if(leidos!=4)return leidos<0?(int)leidos:VOLUMEN_CORRUPTO;
        if((sum^~0u)!=be32(esperado))return VOLUMEN_CORRUPTO;
        pos+=12u+(uint64_t)n;cab=1;
        if(!memcmp(h+4,"IEND",4))return !n && datos && pos==total?0:VOLUMEN_CORRUPTO;
    }return VOLUMEN_CORRUPTO;
}
int imagen_decodificar_vfs(int fd,uint32_t **rgb,unsigned *w,unsigned *h){
    if(!rgb || !w || !h || usados)return VOLUMEN_NO_SOPORTADO;*rgb=NULL;*w=*h=0;pico=0;
    uint64_t tam=vfs_tamano_fd(fd);if(tam<8 || tam>32u*1024*1024)return VOLUMEN_NO_SOPORTADO;
    uint8_t firma[8];int64_t r=vfs_leer_en(fd,0,firma,8);if(r!=8)return r<0?(int)r:VOLUMEN_CORRUPTO;
    if(!memcmp(firma,"\x89PNG\r\n\x1a\n",8)){int e=validar_png(fd,tam);if(e)return e;}
    else if(firma[0]==255 && firma[1]==216){uint8_t ultimo[2];r=vfs_leer_en(fd,tam-2,ultimo,2);if(r!=2)return r<0?(int)r:VOLUMEN_CORRUPTO;if(ultimo[0]!=255 || ultimo[1]!=217)return VOLUMEN_CORRUPTO;}
    else return VOLUMEN_NO_SOPORTADO;
    const stbi_io_callbacks cb={leer,saltar,eof};origen o={fd,0};int x,y,c;
    if(vfs_buscar(fd,0,VFS_SEEK_SET)<0)return VOLUMEN_CORRUPTO;
    if(!stbi_info_from_callbacks(&cb,&o,&x,&y,&c) || o.error)return o.error?o.error:VOLUMEN_CORRUPTO;
    if(x<=0 || y<=0 || x>8192 || y>8192 || (uint64_t)x*y>16u*1024*1024)return VOLUMEN_NO_SOPORTADO;
    if(vfs_buscar(fd,0,VFS_SEEK_SET)<0)return VOLUMEN_CORRUPTO;
    stbi_uc *p=stbi_load_from_callbacks(&cb,&o,&x,&y,&c,4);
    if(!p || o.error){if(p)imagen_liberar(p);return o.error?o.error:VOLUMEN_CORRUPTO;}
    uint32_t *out=(uint32_t *)p;
    for(size_t i=0;i<(size_t)x*y;i++){unsigned a=p[i*4+3],ro=(p[i*4]*a+127)/255,ve=(p[i*4+1]*a+127)/255,az=(p[i*4+2]*a+127)/255;out[i]=(ro<<16)|(ve<<8)|az;}
    *rgb=out;*w=(unsigned)x;*h=(unsigned)y;return 0;
}
