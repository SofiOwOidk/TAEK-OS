#include "../../base/trabajos.h"
#include "../../../boot/limine/limine.h"
#include "../../base/paginacion.h"
#include "../../base/tiempo.h"
#include "gdt.h"
#include "idt.h"
#include "fpu.h"
#include "apic.h"
#include "serial.h"
#include "vmx.h"
_Static_assert(offsetof(struct limine_smp_info,extra_argument)==24,"ABI del trampoline Limine");

__attribute__((used,section(".requests")))
static volatile struct limine_smp_request peticion={.id=LIMINE_SMP_REQUEST,.revision=0};
typedef struct {
    uint64_t pila;
    unsigned indice, lapic, listo, epoca_vista, detenido, xmm;
    x86_64_capacidades_simd capacidades;
    uint8_t espacio[65536];
} datos_cpu;
static datos_cpu cpus[TRABAJOS_MAX_CPU] __attribute__((aligned(64)));
static trabajos_lote lote;
static unsigned cantidad=1, detectadas=1, arrancadas=1, epoca, parar, ocupado;
static unsigned limite_cpu=4, bsp_id;
static uint64_t cr3;
extern void smp_entrada(struct limine_smp_info *);
extern void gdt_iniciar_cpu(unsigned);
extern void idt_cargar_cpu(void);
static void ipi(struct marco_interrupcion *m){(void)m;}
static void despertar(unsigned id) {
    const struct estado_apic *a=apic_obtener_estado();
    if(a->es_x2apic) {
        uint64_t v=((uint64_t)id<<32)|81;
        __asm__ volatile("wrmsr"::"c"(0x830),"a"((uint32_t)v),"d"((uint32_t)(v>>32)):"memory");
    } else {
        while(apic_leer(0x300)&(1u<<12))__asm__ volatile("pause");
        apic_escribir(0x310,id<<24);apic_escribir(0x300,81);
    }
}
void smp_trabajador(datos_cpu *c) {
    __asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
    gdt_iniciar_cpu(c->indice);x86_64_fpu_iniciar();idt_cargar_cpu();
    unsigned lo,hi;
    __asm__ volatile("rdmsr":"=a"(lo),"=d"(hi):"c"(0x1b));
    uint64_t apic_base=((uint64_t)hi<<32)|lo;
    apic_base|=1u<<11;
    if(apic_obtener_estado()->es_x2apic)apic_base|=1u<<10;
    __asm__ volatile("wrmsr"::"c"(0x1b),"a"((uint32_t)apic_base),"d"((uint32_t)(apic_base>>32)):"memory");
    /* Cada AP tiene APIC local; las IRQ externas siguen dirigidas al BSP. */
    apic_escribir(0xf0,0x100|255);apic_escribir(0x320,1u<<16);
    apic_escribir(0x350,1u<<16);apic_escribir(0x360,1u<<16);apic_escribir(0x80,0);
    uint64_t base=(uint64_t)c;
    __asm__ volatile("wrmsr"::"c"(0xc0000101),"a"((uint32_t)base),"d"((uint32_t)(base>>32)):"memory");
    c->xmm=x86_64_fpu_sse2_lista();
    c->capacidades=x86_64_fpu_capacidades_cpu();
    __atomic_store_n(&c->listo,1,__ATOMIC_RELEASE);
    for(;;) {
        __asm__ volatile("cli":::"memory");
        unsigned e=__atomic_load_n(&epoca,__ATOMIC_ACQUIRE);
        if(__atomic_load_n(&parar,__ATOMIC_ACQUIRE))break;
        if(e==c->epoca_vista){__asm__ volatile("sti; hlt":::"memory");continue;}
        /* Mapeos sólo cambian entre lotes. Recarga antes de leer sus buffers. */
        uint64_t cr4;__asm__ volatile("mov %%cr4,%0":"=r"(cr4));
        if(cr4&(1u<<7)) {
            uint64_t sin_global=cr4&~(UINT64_C(1)<<7);
            __asm__ volatile("mov %0,%%cr4;mov %1,%%cr4"::"r"(sin_global),"r"(cr4):"memory");
        }
        __asm__ volatile("mov %0,%%cr3; sti"::"r"(cr3):"memory");
        while(trabajos_tomar(&lote,c->indice)){}
        __atomic_store_n(&c->epoca_vista,e,__ATOMIC_RELEASE);
    }
    __atomic_store_n(&c->detenido,1,__ATOMIC_RELEASE);
    for(;;)__asm__ volatile("cli; hlt");
}
unsigned trabajos_cpu_activas(void){return cantidad;}
unsigned trabajos_cpu_arrancadas(void){return arrancadas;}
unsigned trabajos_cpu_detectadas(void){return detectadas;}
int trabajos_en_curso(void){return __atomic_load_n(&ocupado,__ATOMIC_ACQUIRE);}
void trabajos_cancelar_actual(void){if(trabajos_en_curso())trabajos_cancelar(&lote);}
void trabajos_limitar_cpu(unsigned n){if(n<=4)limite_cpu=n;}
int trabajos_es_coordinador(void) {
    if(arrancadas==1)return 1;
    unsigned a,b,c,d;
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(11),"c"(0));
    return d==bsp_id;
}
int trabajos_iniciar(void) {
    const struct estado_apic *a=apic_obtener_estado();
    if(!peticion.response || !a->activo || vmx_esta_activo())return 0;
    struct limine_smp_response *r=peticion.response;
    detectadas=(unsigned)r->cpu_count;bsp_id=r->bsp_lapic_id;cr3=paginacion_obtener_cr3();
    if(limite_cpu<2)return 0;
    unsigned x,b,c,d, max, desplazamiento=0;
    __asm__ volatile("cpuid":"=a"(max),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));
    if(max>=11) {
        __asm__ volatile("cpuid":"=a"(x),"=b"(b),"=c"(c),"=d"(d):"a"(11),"c"(0));
        if(b && ((c>>8)&255)==1)desplazamiento=x&31;
    } else return 0; /* Sin topología no asumimos que cada hilo es un núcleo. */
    idt_registrar_manejador(81,ipi);
    unsigned claves[TRABAJOS_MAX_CPU]={r->bsp_lapic_id>>desplazamiento};
    for(uint64_t j=0;j<r->cpu_count && cantidad<limite_cpu;j++) {
        struct limine_smp_info *i=r->cpus[j];unsigned clave=i->lapic_id>>desplazamiento;
        int existe=0;for(unsigned k=0;k<cantidad;k++)existe|=claves[k]==clave;
        if(existe)continue;
        unsigned k=cantidad;datos_cpu *p=&cpus[k];
        p->indice=k;p->lapic=i->lapic_id;p->pila=(uint64_t)(p->espacio+sizeof(p->espacio));
        i->extra_argument=(uint64_t)p;
        __atomic_store_n(&i->goto_address,smp_entrada,__ATOMIC_RELEASE);
        uint64_t inicio=tiempo_obtener_milisegundos();
        while(!__atomic_load_n(&p->listo,__ATOMIC_ACQUIRE)) {
            if(tiempo_obtener_milisegundos()-inicio>2000) {
                /* No reutilizar un índice de AP cuyo arranque sigue pendiente. */
                serial_imprimir_linea("SMP: tiempo de arranque agotado; pool deshabilitado");
                trabajos_parar();return 0;
            }
            __asm__ volatile("pause");
        }
        claves[k]=clave;cantidad++;arrancadas++;
        serial_imprimir("SMP CPU=");serial_imprimir_dec(k);serial_imprimir(" LAPIC=");serial_imprimir_dec(p->lapic);
        serial_imprimir(" SSE2=");serial_imprimir_dec(p->capacidades.sse2);
        serial_imprimir(" AVX2_CPUID=");serial_imprimir_dec(p->capacidades.avx2);
        serial_imprimir(" YMM_ENABLED=");serial_imprimir_dec(p->capacidades.ymm_habilitado);serial_imprimir_linea("");
    }
    serial_imprimir("SMP CPU_DETECTADAS=");serial_imprimir_dec(detectadas);
    serial_imprimir(" CPU_ARRANCADAS=");serial_imprimir_dec(arrancadas);
    serial_imprimir(" EJECUTORES=");serial_imprimir_dec(cantidad);serial_imprimir_linea(" SMT=0");
    return cantidad>1;
}
int trabajos_ejecutar(trabajo_fn f,void *p,unsigned n,void (*servicio)(void *),void *u) {
    if(!f || n>TRABAJOS_MAX_REGIONES || trabajos_en_curso())return 0;
    __atomic_store_n(&ocupado,1,__ATOMIC_RELEASE);
    trabajos_preparar(&lote,f,p,n);
    unsigned e=__atomic_add_fetch(&epoca,1,__ATOMIC_RELEASE);
    for(unsigned i=1;i<cantidad;i++)despertar(cpus[i].lapic);
    while(!trabajos_terminados(&lote)) {
        trabajos_tomar(&lote,0);if(servicio)servicio(u);
        __asm__ volatile("pause");
    }
    for(unsigned i=1;i<cantidad;i++)while(__atomic_load_n(&cpus[i].epoca_vista,__ATOMIC_ACQUIRE)!=e) {
        if(servicio)servicio(u);__asm__ volatile("pause");
    }
    __atomic_store_n(&lote.activo,0,__ATOMIC_RELEASE);
    __atomic_store_n(&ocupado,0,__ATOMIC_RELEASE);
    return !lote.cancelado;
}
void trabajos_parar(void) {
    __atomic_store_n(&parar,1,__ATOMIC_RELEASE);
    for(unsigned i=1;i<cantidad;i++)despertar(cpus[i].lapic);
    for(unsigned i=1;i<cantidad;i++)while(!__atomic_load_n(&cpus[i].detenido,__ATOMIC_ACQUIRE))__asm__ volatile("pause");
    cantidad=1;
}
static unsigned prueba[128];
static void verificar(void *u,unsigned i,unsigned cpu) {
    (void)u;(void)cpu;__atomic_fetch_add(&prueba[i],1,__ATOMIC_RELAXED);
    /* Provoca IRQ en el propio ejecutor, con su estado XMM conservado. */
    apic_enviar_self_ipi(81);
    uint64_t entrada[2]={UINT64_C(0xa18650dead123456),i^cpu},salida[2];
    /* El ensamblador conserva el valor en XMM durante una IRQ software
     * síncrona. El compilador de este archivo sigue sin generar SIMD. */
    __asm__ volatile("movdqu %1,%%xmm0;int $81;movdqu %%xmm0,%0":"=m"(salida):"m"(entrada):"memory");
    if(salida[0]!=entrada[0] || salida[1]!=entrada[1])__atomic_store_n(&prueba[i],2,__ATOMIC_RELAXED);
}
int trabajos_autoprueba(void) {
    for(unsigned i=0;i<128;i++)prueba[i]=0;
    if(!trabajos_ejecutar(verificar,0,128,0,0))return 0;
    for(unsigned i=0;i<128;i++)if(prueba[i]!=1)return 0;
    serial_imprimir_linea("SMP AUTOPRUEBA REGIONES=128 EXACTAMENTE_UNA=128 SELF_IPI_SENT=128 SOFTWARE_IRQ=128 XMM_CHECKED=128");return 1;
}
