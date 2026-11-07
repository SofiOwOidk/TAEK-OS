#include "driver_ram.h"
#include "driver_elf.h"
#include "driver_abi.h"
#include "driver_fibras.h"
#include "driver_contexto_prueba.h"
#include "../arquitectura/x86_64/tls.h"
#include "../arquitectura/x86_64/idt.h"
#include "consola.h"
#include "xhci.h"
#include "puntero.h"
#include "red/archivos.h"
#include "../base/memoria.h"
#include "../base/dma.h"
#include "../base/paginacion.h"
#include "../base/tiempo.h"
#include "../base/sha256.h"
#include "../base/trabajos.h"
#include "../base/huevo.h"
#include "../arquitectura/x86_64/pci.h"
#include "iommu.h"
#include "pantalla.h"
#define DRIVER_VA UINT64_C(0x0000400000000000)
#define OWN_MAX 4096
#define DEV_MAX 8
#define PAGINAS_MAX 4096
#define MMIO_MAX 32
#define MMIO_VA UINT64_C(0x0000500000000000)
struct paginas_propias {uint64_t fisica;uint32_t n;};
struct mapeo_mmio {uint64_t virt,bytes;};
struct propietario {void *p;size_t n;unsigned fibra;};
struct interfaz {char nombre[32];uint32_t clase;struct taek_driver_ops ops;};
struct observador_modulo {void (*fn)(uint64_t,void *);void *ctx;};
static struct {
    struct driver_elf_plan *plan;uint64_t fisica;unsigned paginas,mapeadas;
    struct taek_driver descriptor;struct propietario owned[OWN_MAX];size_t owned_bytes;struct paginas_propias paginas_propias[PAGINAS_MAX];struct mapeo_mmio mmio[MMIO_MAX];uint64_t mmio_siguiente;
    struct interfaz interfaces[DEV_MAX];int vivo,inicio_fallido,en_callback,hid_owned,fibras;uint8_t lock;
    struct observador_modulo observadores[2];unsigned observador_actual;int en_observador;
} g;
static size_t longitud(const char *s){size_t n=0;while(s[n])n++;return n;}
static int igual(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return *a==*b;}
static int ptr_texto(uint64_t p){return p && driver_elf_rango(g.plan,DRIVER_VA,p,1,1);}
static int ptr_datos(const void *p,size_t n){
    uint64_t v=(uint64_t)p;if(driver_elf_rango(g.plan,DRIVER_VA,v,n,0))return 1;
    for(unsigned i=0;i<OWN_MAX;i++)if(g.owned[i].p && v>=(uint64_t)g.owned[i].p && v-(uint64_t)g.owned[i].p<=g.owned[i].n && n<=g.owned[i].n-(v-(uint64_t)g.owned[i].p))return 1;
    return 0;
}
static int ptr_escritura(const void *p,size_t n){
    uint64_t v=(uint64_t)p;
    if(g.plan && v>=DRIVER_VA){
        uint64_t off=v-DRIVER_VA;
        for(unsigned i=1;i<g.plan->total;i++){
            const struct driver_elf_seccion *s=&g.plan->secciones[i];
            if((s->flags&3)!=3 || off<g.plan->posiciones[i])continue;
            uint64_t delta=off-g.plan->posiciones[i];
            if(delta<=s->tamano && n<=s->tamano-delta)return 1;
        }
    }
    for(unsigned i=0;i<OWN_MAX;i++)if(g.owned[i].p && v>=(uint64_t)g.owned[i].p && v-(uint64_t)g.owned[i].p<=g.owned[i].n && n<=g.owned[i].n-(v-(uint64_t)g.owned[i].p))return 1;
    return 0;
}
static int copiar_nombre(const char *src,char out[32]){
    if(!src)return -1;
    for(unsigned i=0;i<32;i++){
        if(!ptr_datos(src+i,1))return -1;char c=src[i];out[i]=c;
        if(!c)return i?0:-1;
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_'))return -1;
    }return -1;
}
static void *asignar_propietario(size_t n,unsigned fibra){
    if(!g.en_callback || !n || n>64u*1024u*1024u-g.owned_bytes)return 0;
    for(unsigned i=0;i<OWN_MAX;i++)if(!g.owned[i].p){
        void *p=asignar_memoria_cero(n);if(!p)return 0;g.owned[i]=(struct propietario){p,n,fibra};g.owned_bytes+=n;return p;
    }
    return 0;
}
static void *api_asignar(size_t n){return asignar_propietario(n,0);}
static void *fibra_asignar(size_t n){return asignar_propietario(n,1);}
static int liberar_propietario(void *p,unsigned fibra){
    if(!g.en_callback)return -1;
    for(unsigned i=0;i<OWN_MAX;i++)if(g.owned[i].p==p && p && g.owned[i].fibra==fibra){
        struct observador_modulo *o=&g.observadores[g.observador_actual];
        uint64_t ctx=(uint64_t)o->ctx;
        if(o->fn && ctx>=(uint64_t)p && ctx-(uint64_t)p<g.owned[i].n)return -1;
        g.owned_bytes-=g.owned[i].n;liberar_memoria(p);g.owned[i]=(struct propietario){0};return 0;
    }
    return -1;
}
static int api_liberar(void *p){return liberar_propietario(p,0);}
static int fibra_free(void *p){return liberar_propietario(p,1);}
static void *fibra_tls_crear(void){return taek_tls_crear_con(fibra_asignar);}
static int fibra_tls_destruir(void *p){return taek_tls_destruir_con(p,fibra_free);}
static int fibra_permitido(void){return g.en_callback && trabajos_es_coordinador() && !trabajos_en_curso() && !idt_en_interrupcion();}
static int api_fibra_contexto(void){
    if(!g.fibras || !trabajos_es_coordinador() || trabajos_en_curso() || idt_en_interrupcion())return 0;
    if(g.en_observador)return 2;
    return g.en_callback?1:0;
}
static int api_fibra_crear(void (*fn)(void *),void *ctx,uint64_t *out){
    if(!fibra_permitido() || !g.fibras || !ptr_texto((uint64_t)fn) ||
       (ctx && !ptr_datos(ctx,1)) || !ptr_escritura(out,sizeof(*out)))return FIBRA_E_ARGUMENTOS;
    return fibra_crear(fn,ctx,out);
}
static const struct fibra_callbacks fibra_backend={sizeof(fibra_backend),FIBRA_CALLBACKS_VERSION,
    fibra_asignar,fibra_free,fibra_tls_crear,fibra_tls_destruir,fibra_permitido};
static void observar_destino(uint64_t h,void *arg){
    struct observador_modulo *o=arg;
    /* El codigo del modulo ejecuta esta notificacion en un contexto de
     * transicion, no desde el callback general que puede crear/liberar. */
    int antes=g.en_callback,observando=g.en_observador;
    g.en_callback=0;g.en_observador=1;o->fn(h,o->ctx);
    g.en_observador=observando;g.en_callback=antes;
}
static int api_fibra_observar(void (*fn)(uint64_t,void *),void *ctx){
    if(!fibra_permitido() || !g.fibras || (fn && !ptr_texto((uint64_t)fn)) ||
       (ctx && !ptr_datos(ctx,1)) || (!fn && ctx))return FIBRA_E_ARGUMENTOS;
    unsigned anterior=g.observador_actual,nuevo=anterior^1;
    g.observadores[nuevo]=(struct observador_modulo){fn,ctx};
    int r=fibra_motor_observar(fn?observar_destino:NULL,fn?&g.observadores[nuevo]:NULL);
    if(!r){g.observador_actual=nuevo;g.observadores[anterior]=(struct observador_modulo){0};}
    else g.observadores[nuevo]=(struct observador_modulo){0};
    return r;
}
/* Invocar solo con el callback del propietario aun admitido. La ausencia de
 * referencias es independiente de lo que haya devuelto detener del modulo. */
static int detener_fibras(void){
    if(!g.fibras)return 0;
    int r=fibra_motor_detener();if(!r){g.fibras=0;g.observadores[0]=g.observadores[1]=(struct observador_modulo){0};g.observador_actual=0;}return r;
}
static int api_registrar(const char *nombre,uint32_t clase,const struct taek_driver_ops *ops){
    char id[32]={0};if(!g.en_callback || clase<1 || clase>TAEK_DRIVER_VIDEO || copiar_nombre(nombre,id) || !ptr_datos(ops,sizeof(*ops)))return -1;
    struct taek_driver_ops copia;memcpy(&copia,ops,sizeof(copia));
    if(copia.bytes!=sizeof(copia) || copia.version!=TAEK_DRIVER_ABI || (!copia.leer && !copia.escribir && !copia.control) ||
       (copia.leer && !ptr_texto((uint64_t)copia.leer)) || (copia.escribir && !ptr_texto((uint64_t)copia.escribir)) || (copia.control && !ptr_texto((uint64_t)copia.control)))return -1;
    for(unsigned i=0;i<DEV_MAX;i++)if(g.interfaces[i].clase && igual(id,g.interfaces[i].nombre))return -2;
    for(unsigned i=0;i<DEV_MAX;i++)if(!g.interfaces[i].clase){memcpy(g.interfaces[i].nombre,id,32);g.interfaces[i].clase=clase;g.interfaces[i].ops=copia;return 0;}return -3;
}
static int api_retirar(const char *nombre){
    char id[32]={0};if(!g.en_callback || copiar_nombre(nombre,id))return -1;
    for(unsigned i=0;i<DEV_MAX;i++)if(g.interfaces[i].clase && igual(id,g.interfaces[i].nombre)){memset(&g.interfaces[i],0,sizeof(g.interfaces[i]));return 0;}return -1;
}
static int api_hid_tomar(void){
    if(!g.en_callback || g.hid_owned)return -1;
    int r=xhci_hid_mouse_tomar();if(r>=0)g.hid_owned=1;return r;
}
static int api_hid_leer(struct taek_hid_mouse_reporte *p){
    if(!g.en_callback || !g.hid_owned || !ptr_datos(p,sizeof(*p)))return -1;
    int r=xhci_hid_mouse_leer(p);if(r== -2)puntero_retirar();return r;
}
static int api_hid_liberar(void){
    if(!g.en_callback || !g.hid_owned)return -1;
    xhci_hid_mouse_liberar();puntero_retirar();g.hid_owned=0;return 0;
}
static int api_puntero_publicar(int32_t x,int32_t y,uint32_t botones,uint32_t dispositivo){
    if(!g.en_callback || !g.hid_owned)return -1;return puntero_publicar(x,y,botones,dispositivo);
}
static uint64_t api_hhdm(void){return memoria_obtener_hhdm_offset();}
static int api_alias(uint64_t v){return g.en_callback?paginacion_alias_pml4(v,memoria_obtener_hhdm_offset()):-1;}
static uint64_t api_paginas(uint32_t n,uint64_t al){
    if(!g.en_callback || !n)return 0;
    for(unsigned i=0;i<PAGINAS_MAX;i++)if(!g.paginas_propias[i].fisica){
        uint64_t f=pmm_asignar_bloque_contiguo(n,al?al:4096);
        if(f)g.paginas_propias[i]=(struct paginas_propias){f,n};
        return f;
    }
    return 0;
}
static uint64_t api_paginas_gpu(uint32_t n){
    if(!g.en_callback || !n)return 0;
    for(unsigned i=0;i<PAGINAS_MAX;i++)if(!g.paginas_propias[i].fisica){
        uint64_t f=dma_gpu_asignar(n);
        if(f)g.paginas_propias[i]=(struct paginas_propias){f,n};
        return f;
    }
    return 0;
}
static int api_paginas_liberar(uint64_t f,uint32_t n){
    if(!g.en_callback || !f)return -1;
    for(unsigned i=0;i<PAGINAS_MAX;i++)if(g.paginas_propias[i].fisica==f && g.paginas_propias[i].n==n){
        pmm_liberar_bloque_contiguo(f,n);g.paginas_propias[i]=(struct paginas_propias){0};return 0;
    }
    return -1;
}
static uint32_t api_pci(uint8_t b,uint8_t r,uint8_t f,uint8_t o,uint8_t n){
    return n==1?pci_leer_config_8(b,r,f,o):n==2?pci_leer_config_16(b,r,f,o):n==4?pci_leer_config_32(b,r,f,o):UINT32_MAX;
}
static void api_colapsar(const char *m){huevo_quebrar(m,(uint64_t)__builtin_return_address(0),0,0);}
static const struct dispositivo_pci *pci_buscar(uint8_t b,uint8_t r,uint8_t f){
    for(int i=0;i<pci_obtener_conteo();i++){const struct dispositivo_pci *d=pci_obtener_dispositivo(i);if(d && d->bus==b && d->ranura==r && d->funcion==f)return d;}
    return 0;
}
static int api_pci_barra(uint8_t b,uint8_t r,uint8_t f,uint32_t n,uint64_t *base,uint64_t *tam){
    const struct dispositivo_pci *d=pci_buscar(b,r,f);
    if(!g.en_callback || !d || n>=6 || !ptr_escritura(base,sizeof(*base)) || !ptr_escritura(tam,sizeof(*tam)) || !d->barras[n].valida)return -1;
    *base=d->barras[n].dir_base;*tam=d->barras[n].tamano;return d->barras[n].es_io?1:0;
}
static uint64_t api_mmio_mapear(uint64_t fisica,uint64_t bytes,int escritura){
    if(!g.en_callback || !bytes || (fisica&4095) || (bytes&4095) || bytes>512u*1024u*1024u)return 0;
    const struct dispositivo_pci *d=pci_buscar(0,2,0);if(!d)return 0;
    int dentro=0;
    for(unsigned i=0;i<6;i++){const struct barra_pci *b=&d->barras[i];
        if(b->valida && !b->es_io && fisica>=b->dir_base && fisica-b->dir_base<b->tamano && bytes<=b->tamano-(fisica-b->dir_base))dentro=1;}
    if(!dentro)return 0;
    unsigned slot=MMIO_MAX;for(unsigned i=0;i<MMIO_MAX;i++)if(!g.mmio[i].virt){slot=i;break;}
    if(slot==MMIO_MAX)return 0;
    uint64_t v=MMIO_VA+g.mmio_siguiente;
    uint64_t fl=PAGINA_PRESENTE|PAGINA_SIN_CACHE|PAGINA_ESCRITURA_DIR|PAGINA_NO_EJECUTABLE|(escritura?PAGINA_ESCRITURA:0);
    for(uint64_t o=0;o<bytes;o+=4096)if(paginacion_mapear(v+o,fisica+o,fl)){for(uint64_t q=0;q<o;q+=4096)paginacion_desmapear(v+q);return 0;}
    g.mmio_siguiente+=bytes+4096;   /* Pagina de guarda sin mapear entre mapeos. */
    g.mmio[slot].virt=v;g.mmio[slot].bytes=bytes;return v;
}
static int api_dma_directo_gpu(void){
    if(!g.en_callback)return -1;
    const iommu_estado_t *e=iommu_obtener_estado();
    iommu_ruta_pci_t ruta;memset(&ruta,0,sizeof(ruta));
    ruta.conteo_pasos=1;ruta.pasos[0].dispositivo=2;ruta.pasos[0].funcion=0;
    uint32_t i;int r=iommu_dma_directo_observado(e,&ruta,&i);
    if(r!=1)return r;
    /* Lo guardado es del arranque: se relee GSTS para no fiarse de un dato antiguo. */
    uint32_t gsts=*(volatile uint32_t *)(e->drhd[i].mmio_virtual+0x1c);
    if(gsts==UINT32_MAX)return -1;
    return gsts&(1u<<31)?0:1;
}
static uint64_t api_fb_fisica(void){
    void *fb=pantalla_obtener_base();
    return fb?(uint64_t)fb-memoria_obtener_hhdm_offset():0;
}
static void mmio_retirar(unsigned i){for(uint64_t o=0;o<g.mmio[i].bytes;o+=4096)paginacion_desmapear(g.mmio[i].virt+o);g.mmio[i].virt=0;g.mmio[i].bytes=0;}
static int api_mmio_desmapear(uint64_t v,uint64_t bytes){
    if(!g.en_callback)return -1;
    for(unsigned i=0;i<MMIO_MAX;i++)if(g.mmio[i].virt==v && v && g.mmio[i].bytes==bytes){mmio_retirar(i);return 0;}
    return -1;
}
static const struct taek_kernel_api api={sizeof(api),TAEK_DRIVER_ABI,consola_imprimir,consola_imprimir_linea,consola_imprimir_dec,tiempo_obtener_milisegundos,api_asignar,api_liberar,api_registrar,api_retirar,api_hid_tomar,api_hid_leer,api_hid_liberar,api_puntero_publicar,
    api_fibra_crear,fibra_ejecutar,fibra_yield,fibra_bloquear,fibra_despertar,fibra_liberar,fibra_estado,fibra_actual,api_fibra_observar,api_fibra_contexto,api_hhdm,api_alias,api_paginas,api_paginas_liberar,api_pci,api_colapsar,api_pci_barra,api_mmio_mapear,api_mmio_desmapear,tiempo_ciclos_por_ms,api_dma_directo_gpu,api_fb_fisica,api_paginas_gpu};
static void liberar_cargador(void){
    /* Ningun error de modulo permite abandonar pilas suspendidas. */
    if(g.fibras){consola_imprimir_linea("[DRIVER] Fibras pendientes; se conserva el cargador.");return;}
    if(g.hid_owned){xhci_hid_mouse_liberar();puntero_retirar();g.hid_owned=0;}
    memset(g.interfaces,0,sizeof(g.interfaces));
    for(unsigned i=0;i<OWN_MAX;i++)if(g.owned[i].p){liberar_memoria(g.owned[i].p);g.owned[i]=(struct propietario){0};}
    g.owned_bytes=0;
    for(unsigned i=0;i<PAGINAS_MAX;i++)if(g.paginas_propias[i].fisica){pmm_liberar_bloque_contiguo(g.paginas_propias[i].fisica,g.paginas_propias[i].n);g.paginas_propias[i]=(struct paginas_propias){0};}
    for(unsigned i=0;i<MMIO_MAX;i++)if(g.mmio[i].virt)mmio_retirar(i);
    for(unsigned i=0;i<g.mapeadas;i++)paginacion_desmapear(DRIVER_VA+(uint64_t)i*4096);
    g.mapeadas=0;if(g.fisica)pmm_liberar_bloque_contiguo(g.fisica,g.paginas);
    g.fisica=0;g.paginas=0;if(g.plan)liberar_memoria(g.plan);g.plan=0;g.vivo=0;g.inicio_fallido=0;memset(&g.descriptor,0,sizeof(g.descriptor));
}
void driver_ram_sondear(void){
    if(!g.vivo || g.inicio_fallido || !trabajos_es_coordinador() || trabajos_en_curso() || idt_en_interrupcion() || __atomic_test_and_set(&g.lock,__ATOMIC_ACQUIRE))return;
    /* No module function pointer reaches an interrupt or USB event ring. */
    if(g.hid_owned)xhci_sondeo();g.en_callback=1;
    for(unsigned i=0;i<DEV_MAX;i++)if(g.hid_owned && g.interfaces[i].clase==TAEK_DRIVER_PERIFERICO && g.interfaces[i].ops.control)
        g.interfaces[i].ops.control(TAEK_CONTROL_SONDEAR,0);
    if(g.fibras)fibra_ejecutar(FIBRA_SLOTS);
    g.en_callback=0;__atomic_clear(&g.lock,__ATOMIC_RELEASE);
}
static int hex_hash(const char *s,uint8_t digest[32]){
    if(longitud(s)!=64)return -1;
    for(unsigned i=0;i<32;i++){unsigned n=0;for(unsigned j=0;j<2;j++){char c=s[i*2+j];if(c>='A' && c<='F')c+=32;unsigned d=c>='0' && c<='9'?(unsigned)(c-'0'):c>='a' && c<='f'?(unsigned)(c-'a'+10):99;if(d>15)return -1;n=n*16+d;}digest[i]=(uint8_t)n;}return 0;
}
static int cargar_archivo(const void *entrada,uint64_t n);
static int cargar(const char *hash){
    if(g.vivo)return -2;uint8_t referencia[32],recibido[32],calculado[32];const void *entrada;uint64_t n;
    if(hex_hash(hash,referencia) || red_archivos_ram_obtener(&entrada,&n,recibido) || n>32u*1024u*1024u || memcmp(referencia,recibido,32))return -3;
    struct sha256 sha;sha256_iniciar(&sha);sha256_actualizar(&sha,entrada,n);sha256_final(&sha,calculado);
    if(memcmp(referencia,calculado,32))return -3;
    return cargar_archivo(entrada,n);
}
static int cargar_archivo(const void *entrada,uint64_t n){
    /* Copy before validating: transfer storage can be released after load. */
    void *file=asignar_memoria(n);if(!file)return -4;memcpy(file,entrada,n);
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000000u),"c"(0));
    if(a<0x80000001u){liberar_memoria(file);return -7;}
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001u),"c"(0));
    if(!(d&(1u<<20))){liberar_memoria(file);return -7;}
    __asm__ volatile("rdmsr":"=a"(a),"=d"(d):"c"(0xc0000080u));
    if(!(a&(1u<<11))){a|=1u<<11;__asm__ volatile("wrmsr"::"a"(a),"d"(d),"c"(0xc0000080u):"memory");}
    int r=-5;g.plan=asignar_memoria(sizeof(*g.plan));if(!g.plan){r=-4;goto fin;}
    if(driver_elf_planificar(file,n,g.plan))goto fin;
    g.paginas=g.plan->bytes/4096;
    for(unsigned i=0;i<g.paginas;i++)if(paginacion_obtener_fisica(DRIVER_VA+(uint64_t)i*4096))goto fin;
    g.fisica=pmm_asignar_bloque_contiguo(g.paginas,4096);if(!g.fisica){r=-4;goto fin;}
    void *physical=(void *)(g.fisica+memoria_obtener_hhdm_offset());
    if(driver_elf_materializar(file,n,g.plan,physical,g.plan->bytes,DRIVER_VA))goto fin;
    for(unsigned i=0;i<g.paginas;i++){
        if(paginacion_mapear(DRIVER_VA+(uint64_t)i*4096,g.fisica+(uint64_t)i*4096,PAGINA_PRESENTE|PAGINA_NO_EJECUTABLE))goto fin;
        g.mapeadas++;
    }
    for(unsigned s=1;s<g.plan->total;s++){
        const struct driver_elf_seccion *sec=&g.plan->secciones[s];if(!(sec->flags&2))continue;
        uint64_t flags=PAGINA_PRESENTE;if(sec->flags&1)flags|=PAGINA_ESCRITURA;if(!(sec->flags&4))flags|=PAGINA_NO_EJECUTABLE;
        for(uint64_t pos=0;pos<sec->tamano;pos+=4096){uint64_t off=g.plan->posiciones[s]+pos;if(paginacion_mapear(DRIVER_VA+off,g.fisica+off,flags))goto fin;}
    }
    memcpy(&g.descriptor,(uint8_t *)physical+g.plan->posiciones[g.plan->descriptor_seccion]+g.plan->descriptor_offset,sizeof(g.descriptor));
    if(g.descriptor.magia!=TAEK_DRIVER_MAGIA || g.descriptor.bytes!=sizeof(g.descriptor) || g.descriptor.version!=TAEK_DRIVER_ABI || !ptr_texto((uint64_t)g.descriptor.iniciar) || !ptr_texto((uint64_t)g.descriptor.comando) || !ptr_texto((uint64_t)g.descriptor.detener))goto fin;
    unsigned length=0;while(length<32 && g.descriptor.nombre[length]){char c=g.descriptor.nombre[length++];if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_'))goto fin;}
    if(!length || length==32)goto fin;
    g.en_callback=1;r=fibra_motor_iniciar(&fibra_backend);
    if(!r){g.fibras=1;r=g.descriptor.iniciar(&api);}
    g.en_callback=0;
    if(r){
        /* An init error does not prove hardware, DMA or IRQ have stopped.
         * Keep the executable mapping and ownership if stop cannot confirm it.
         * Only another stop is admitted while quarantined. */
        g.en_callback=1;int stop=g.descriptor.detener();if(!stop)stop=detener_fibras();g.en_callback=0;
        if(stop){g.vivo=1;g.inicio_fallido=1;consola_imprimir_linea("[DRIVER] Inicio fallo y detener no confirmo parada; modulo y recursos retenidos.");}
        r=-6;goto fin;
    }
    g.vivo=1;consola_imprimir("[DRIVER] Cargado en RAM sin reboot: ");consola_imprimir_linea(g.descriptor.nombre);r=0;
fin:
    liberar_memoria(file);if(r && !g.vivo)liberar_cargador();return r;
}
int driver_ram_cargar_bytes(const void *datos,uint64_t bytes){
    if(!datos || !bytes || bytes>32u*1024u*1024u)return -3;
    if(!trabajos_es_coordinador() || trabajos_en_curso() || idt_en_interrupcion() || __atomic_test_and_set(&g.lock,__ATOMIC_ACQUIRE))return -1;
    int r=g.vivo?-2:cargar_archivo(datos,bytes);
    __atomic_clear(&g.lock,__ATOMIC_RELEASE);return r;
}
static struct interfaz *interfaz_video(void){
    if(!g.vivo || g.inicio_fallido)return 0;
    for(unsigned i=0;i<DEV_MAX;i++)if(g.interfaces[i].clase==TAEK_DRIVER_VIDEO && g.interfaces[i].ops.control)return &g.interfaces[i];
    return 0;
}
int driver_ram_tiene_video(void){return interfaz_video()!=0;}
int driver_ram_video(uint32_t orden,void *peticion){
    if(!trabajos_es_coordinador() || trabajos_en_curso() || idt_en_interrupcion() || __atomic_test_and_set(&g.lock,__ATOMIC_ACQUIRE))return -1000;
    struct interfaz *dev=interfaz_video();int r=-1001;
    if(dev){g.en_callback=1;r=dev->ops.control(orden,(uint64_t)peticion);g.en_callback=0;}
    __atomic_clear(&g.lock,__ATOMIC_RELEASE);return r;
}
static const char *saltar(const char *p){while(*p==' ')p++;return p;}
static int comienza(const char *p,const char *s){while(*s)if(*p++!=*s++)return 0;return 1;}
static struct interfaz *buscar(const char *name){for(unsigned i=0;i<DEV_MAX;i++)if(g.interfaces[i].clase && igual(name,g.interfaces[i].nombre))return &g.interfaces[i];return 0;}
static int numero(const char **p,uint64_t *n){*n=0;const char *s=saltar(*p);if(*s<'0' || *s>'9')return -1;while(*s>='0' && *s<='9'){unsigned d=(unsigned)(*s++-'0');if(*n>(UINT64_MAX-d)/10)return -1;*n=*n*10+d;}if(*s && *s!=' ')return -1;*p=saltar(s);return 0;}
static int probar_io(const char *p,int escribir){
    char id[32];unsigned len=0;p=saltar(p);while(*p && *p!=' '){if(len+1>=sizeof(id))return -1;id[len++]=*p++;}id[len]=0;
    uint64_t offset,bytes,valor=0;if(numero(&p,&offset) || numero(&p,&bytes) || !bytes || bytes>256 || (escribir && (numero(&p,&valor) || valor>255)) || *p)return -1;
    struct interfaz *dev=buscar(id);if(g.inicio_fallido || !dev || (dev->clase==TAEK_DRIVER_BLOQUE && (offset>dev->ops.capacidad || bytes>dev->ops.capacidad-offset)))return -1;
    uint8_t buf[256];memset(buf,(int)valor,bytes);g.en_callback=1;int r;
    if(escribir)r=dev->ops.escribir?dev->ops.escribir(offset,buf,(uint32_t)bytes):-1;
    else r=dev->ops.leer?dev->ops.leer(offset,buf,(uint32_t)bytes):-1;
    g.en_callback=0;
    if(!r && !escribir){consola_imprimir("[DRIVER] Lectura: ");for(unsigned i=0;i<bytes;i++){consola_imprimir_hex(buf[i]);consola_imprimir(" ");}consola_imprimir_linea("");}
    return r;
}
void driver_ram_comando(const char *arg){
    if(!trabajos_es_coordinador() || trabajos_en_curso() || idt_en_interrupcion() || __atomic_test_and_set(&g.lock,__ATOMIC_ACQUIRE)){consola_imprimir_linea("[DRIVER] Ocupado; no se cambia el modulo.");return;}
    const char *p=saltar(arg?arg:"");int r=0;
    if(comienza(p,"cargar "))r=cargar(saltar(p+7));
    else if(igual(p,"descargar")){
        if(!g.vivo)r=-2;else{g.en_callback=1;r=g.descriptor.detener();if(!r)r=detener_fibras();g.en_callback=0;if(!r){liberar_cargador();consola_imprimir_linea("[DRIVER] Descargado; interfaces y recursos retirados.");}else consola_imprimir_linea("[DRIVER] Detener rechazo la descarga; modulo y recursos conservados.");}
    }else if(comienza(p,"comando ")){if(!g.vivo || g.inicio_fallido)r=-2;else{g.en_callback=1;r=g.descriptor.comando(saltar(p+8));g.en_callback=0;}}
    else if(igual(p,"contexto")){
        if(!g.vivo || g.inicio_fallido || !g.fibras)r=-2;
        else {
            struct driver_contexto_resultado st;int antes=g.en_callback;g.en_callback=1;
            r=driver_contexto_probar(api_fibra_contexto,&st);g.en_callback=antes;
            consola_imprimir("[DRIVER_CONTEXTO] RESULTADO=");consola_imprimir(r?"FAIL":"OK");
            consola_imprimir(" CPUS=");consola_imprimir_dec(st.cpus);
            consola_imprimir(" ANTES=");consola_imprimir_dec((uint64_t)(int64_t)st.antes);
            consola_imprimir(" DESPUES=");consola_imprimir_dec((uint64_t)(int64_t)st.despues);
            consola_imprimir(" ENTRADAS=");consola_imprimir_dec(st.entradas);
            consola_imprimir(" IRQ=");consola_imprimir_dec((uint64_t)(int64_t)st.irq);
            consola_imprimir(" IPI=");consola_imprimir_dec(st.irq_recibidas);
            consola_imprimir(" HEAP=");consola_imprimir_dec(st.heap_limpio);
            consola_imprimir(" IF_ANTES=");consola_imprimir_dec(st.if_antes);
            consola_imprimir(" IF_DESPUES=");consola_imprimir_dec(st.if_despues);
            consola_imprimir(" VECTOR_RETENIDO=");consola_imprimir_dec(st.vector_retenido);
            consola_imprimir(" ADMISION=");consola_imprimir_dec(st.admision);
            consola_imprimir(" FALLOS=");consola_imprimir_dec(st.fallos);consola_imprimir_linea("");
            for(unsigned i=0;i<st.cpus && i<4;i++){
                consola_imprimir("[DRIVER_CONTEXTO] CPU=");consola_imprimir_dec(i);
                consola_imprimir(" LOTE=");consola_imprimir_dec((uint64_t)(int64_t)st.lote[i]);consola_imprimir_linea("");
            }
        }
    }
    else if(comienza(p,"leer "))r=probar_io(p+5,0);
    else if(comienza(p,"escribir "))r=probar_io(p+9,1);
    else if(igual(p,"estado") || igual(p,"listar")){
        consola_imprimir("[DRIVER] Estado: ");consola_imprimir_linea(g.vivo?g.descriptor.nombre:"sin modulo");
        if(g.inicio_fallido)consola_imprimir_linea("[DRIVER] Retenido por inicio fallido; solo se admite reintentar detener.");
        consola_imprimir("[DRIVER] Recursos propios: ");consola_imprimir_dec(g.owned_bytes);consola_imprimir_linea(" bytes");
        struct fibra_estadisticas st;fibra_motor_estadisticas(&st);
        consola_imprimir("[DRIVER] Fibras retenidas: ");consola_imprimir_dec(st.slots_ocupados);consola_imprimir_linea("");
        for(unsigned i=0;i<DEV_MAX;i++)if(g.interfaces[i].clase){consola_imprimir("[DRIVER] Interfaz: ");consola_imprimir(g.interfaces[i].nombre);consola_imprimir(" clase ");consola_imprimir_dec(g.interfaces[i].clase);consola_imprimir_linea("");}
    }else consola_imprimir_linea("driver cargar <SHA256 de archivo ram> | estado | listar | contexto | comando <args> | descargar | leer <id> <offset> <bytes> | escribir <id> <offset> <bytes> <valor>");
    if(r){consola_imprimir("[DRIVER] Rechazado/error: ");consola_imprimir_dec((uint64_t)(r<0?-(int64_t)r:r));consola_imprimir_linea("");}
    __atomic_clear(&g.lock,__ATOMIC_RELEASE);
}
