#include "anillo3.h"
#include "anillo3_abi.h"
#include "apic.h"
#include "pci.h"
#include "puertos.h"
#include "serial.h"
#include "../../base/version.h"
#include "../../controladores/acpi/bateria.h"
#include "../../controladores/compositor.h"
#include "../../controladores/puntero.h"
#include "../../controladores/equipo.h"
#include "../../controladores/driver_abi.h"
#include "../../controladores/xhci.h"
#include "../../controladores/hid_i2c.h"
#include "../../controladores/teclado.h"
#include "../../controladores/ps2.h"
#include "../../controladores/usb_msc.h"
#include "../../controladores/red/red.h"
#include "../../controladores/red/stream.h"
#include "../../controladores/red/navegador.h"
#include "../../controladores/red/tls.h"
#include "../../controladores/multimedia/reproductor/reproductor.h"
#include "../../controladores/vfs.h"
#include "../../controladores/exfat.h"
#include "../../base/memoria.h"
#include "../../base/paginacion.h"
#include "../../base/sha256.h"
#include "../../base/tiempo.h"
#include "../../base/trabajos.h"
#include "../../base/huevo.h"
#include "../../base/atencion.h"
#include "../../controladores/audio_hda.h"
#include "../../controladores/consola.h"
#include "../../controladores/audio_ac97.h"
#include "../../controladores/pantalla.h"
#include "../../base/hilos.h"
#include "gdt.h"
#include "thread_director.h"

#define MSR_EFER 0xC0000080u
#define MSR_FS_BASE 0xC0000100u
#define MSR_SYSENTER_CS 0x174u
#define EFER_SCE (UINT64_C(1) << 0)
#define EFER_NXE (UINT64_C(1) << 11)
#define CR4_FSGSBASE (UINT64_C(1) << 16)
#define CR4_SMEP (UINT64_C(1) << 20)
#define CR4_SMAP (UINT64_C(1) << 21)
#define RFLAGS_AC (UINT64_C(1) << 18)

#define LAPIC_TIMER_LVT 0x320u
#define LAPIC_TIMER_INICIAL 0x380u
#define LAPIC_TIMER_ACTUAL 0x390u
#define LAPIC_TIMER_DIVISOR 0x3E0u
#define LVT_ENMASCARADO 0x10000u
#define LVT_PERIODICO 0x20000u
#define TICK_MS 10u

#define MAX_MARCOS 65536u  /* 256 MiB por programa: el escritorio guarda varias capas de pantalla completa y, desde H148,
                            * ventanas que pueden crecer hasta la pantalla entera */
#define MAX_REGIONES 96u
#define EFAULT TAEK_EFAULT
#define ENOSYS TAEK_ENOSYS
#define ANILLO3_SUPERFICIES (ANILLO3_BASE + UINT64_C(0x10000000))
#define ANILLO3_FIN (ANILLO3_BASE + UINT64_C(0x40000000))

struct marco_mapeado { uint64_t virt, fisica; };
struct region { uint64_t inicio, fin; int escribible; };

static int g_preparado, g_smep, g_smap, g_activo;
static uint32_t g_ticks_por_ms;
static uint64_t g_rsp_guardado, g_fs_kernel, g_ultimo_ms, g_inicio_ms;
static uint32_t g_limite_ms, g_limite_total_ms;
static uint64_t g_sup_siguiente;
static volatile int g_matar;
static int g_captura_permitida;
static int g_raton;
static int g_audio_usado;   /* H146: el programa uso SYS_AUDIO; al terminar se detiene el sonido */
static void tomar_muestras(void);
static struct anillo3_resultado *g_res;
static struct marco_mapeado g_marcos[MAX_MARCOS];
static unsigned g_num_marcos;
static struct region g_regiones[MAX_REGIONES];
static unsigned g_num_regiones;
static char g_ultimo_texto[128];
static uint8_t g_fp_nucleo[512] __attribute__((aligned(16)));
static uint8_t g_fp_limpio[512] __attribute__((aligned(16)));

extern int64_t anillo3_entrar(uint64_t *rsp_guardado, uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1);
extern void anillo3_volver(uint64_t rsp_guardado, int64_t valor, uint64_t fs_base) __attribute__((noreturn));
extern const uint8_t __taek_text_inicio[], __taek_text_fin[];
extern const uint8_t _binary_pruebas_anillo3_bin_start[], _binary_pruebas_anillo3_bin_end[];

static inline uint64_t leer_msr(uint32_t m) {
    uint32_t a, d;
    __asm__ volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(m));
    return ((uint64_t)d << 32) | a;
}
/* H151: ciclos de la CPU 0 dentro de SYS_ESPERAR. Lo demas es trabajo (del programa o del nucleo por el). */
static uint64_t g_espera_ciclos;
static inline uint64_t leer_tsc(void) { uint32_t a, d; __asm__ volatile("rdtsc" : "=a"(a), "=d"(d)); return (uint64_t)d << 32 | a; }
static inline void escribir_msr(uint32_t m, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}

/* H213, Fase D: lo que CPU 0 hace por su cuenta cuando el programa no la suelta */
static void entrada_atender(void);
static uint64_t g_puntero_ms;                  /* Ultima vez que CPU 0 atendio el raton y el trackpad */
static uint32_t g_servicios_cpu0, g_hueco_cpu0_max;

static void reloj_irq(struct marco_interrupcion *marco) {
    if (!g_activo || (marco->cs & 3) != 3) return;
    g_res->ticks++;
    uint64_t ahora = tiempo_obtener_milisegundos();
    if (ahora - g_ultimo_ms >= g_limite_ms) g_matar = 1;
    if (g_limite_total_ms && ahora - g_inicio_ms >= g_limite_total_ms) g_matar = 1;
    /* H213, Fase D: un programa en un bucle sin llamadas al sistema ya no congela la flecha. El reloj (10 ms) solo
     * interrumpe Anillo 3, asi que CPU 0 no esta a mitad de nada del nucleo y no tiene cerrojos en la mano: puede
     * leer el raton y el trackpad y mover la flecha como haria SYS_EVENTO. */
    if (ahora - g_puntero_ms >= 15) {
        uint64_t hueco = ahora - g_puntero_ms;
        if (hueco > g_hueco_cpu0_max) g_hueco_cpu0_max = (uint32_t)hueco;
        entrada_atender();
        g_servicios_cpu0++;
    }
}

void anillo3_acceso_usuario(int abrir) {
    if (!g_smap) return;
    if (abrir) __asm__ volatile("stac" ::: "cc");
    else __asm__ volatile("clac" ::: "cc");
}

/* Refleja en la CPU lo que el programa no debe poder alcanzar. Sin APIC no hay reloj y se rechaza. */
static int preparar(void) {
    if (g_preparado) return 0;
    if (!apic_obtener_estado()->activo) return -2;
    uint64_t efer = leer_msr(MSR_EFER);
    if (!(efer & EFER_NXE)) return -3;
    /* Con SCE activa y LSTAR vacio, la instruccion syscall saltaria a Ring 0 en la direccion 0. */
    escribir_msr(MSR_EFER, efer & ~EFER_SCE);
    escribir_msr(MSR_SYSENTER_CS, 0);

    uint32_t a = 7, b, c = 0, d, max;
    __asm__ volatile("cpuid" : "=a"(max), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    b = 0;
    if (max >= 7) __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~CR4_FSGSBASE;
    if (b & (1u << 7)) cr4 |= CR4_SMEP;
    if (b & (1u << 20)) cr4 |= CR4_SMAP;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    g_smep = (cr4 & CR4_SMEP) != 0;
    g_smap = (cr4 & CR4_SMAP) != 0;

    /* Calibrar contra el TSC ya calibrado. */
    apic_escribir(LAPIC_TIMER_DIVISOR, 3);
    apic_escribir(LAPIC_TIMER_LVT, LVT_ENMASCARADO);
    apic_escribir(LAPIC_TIMER_INICIAL, 0xFFFFFFFFu);
    esperar_milisegundos(10);
    uint32_t actual = apic_leer(LAPIC_TIMER_ACTUAL);
    apic_escribir(LAPIC_TIMER_INICIAL, 0);
    g_ticks_por_ms = (0xFFFFFFFFu - actual) / 10u;
    if (!g_ticks_por_ms) return -4;
    if (idt_reservar_manejador(APIC_VECTOR_TEMPORIZADOR, reloj_irq)) return -5;

    g_fp_limpio[0] = 0x7F; g_fp_limpio[1] = 0x03;  /* FCW por defecto */
    g_fp_limpio[24] = 0x80; g_fp_limpio[25] = 0x1F; /* MXCSR por defecto */
    g_preparado = 1;
    return 0;
}

/* H144: el monton del programa (SYS_MEMORIA). Sus paginas no van en g_marcos: son contiguas desde TAEK_MONTON hasta
 * g_monton_fin, y al soltarlas se le pregunta a la tabla de paginas que marco fisico tiene cada una. */
#define MONTON_RESERVA_PAGINAS (128u * 256u)   /* 128 MiB de RAM que nunca se dan a un programa: son del nucleo */
static uint64_t g_monton_fin = TAEK_MONTON, g_monton_pico;
static int g_monton_region = -1;

static void monton_soltar(uint64_t desde) {
    /* H213, Fase D: con hilos del programa en las CPU 1 a 3, otra CPU puede tener la traduccion vieja en su TLB y
     * escribir en el marco ya devuelto (que el nucleo pudo dar a otro). Por eso, de a 256 paginas: desmapear,
     * ronda de invalidacion en todas las CPU y recien entonces devolver los marcos. */
    uint64_t marcos[256];
    unsigned n = 0;
    for (uint64_t v = g_monton_fin; v > desde;) {
        v -= TAMANO_PAGINA;
        uint64_t f = paginacion_obtener_fisica(v) & ~(TAMANO_PAGINA - 1);
        paginacion_desmapear(v);
        if (f) marcos[n++] = f;
        if (n == 256) {
            paginacion_tlb_sincronizar();
            for (unsigned i = 0; i < n; i++) pmm_liberar_pagina_fisica(marcos[i]);
            n = 0;
        }
    }
    paginacion_tlb_sincronizar();
    for (unsigned i = 0; i < n; i++) pmm_liberar_pagina_fisica(marcos[i]);
    g_monton_fin = desde;
    if (g_monton_region >= 0) g_regiones[g_monton_region].fin = desde;
}

static void desmapear_todo(void) {
    monton_soltar(TAEK_MONTON);
    g_monton_region = -1;
    uint64_t hhdm = memoria_obtener_hhdm_offset();
    for (unsigned i = 0; i < g_num_marcos; i++) paginacion_desmapear(g_marcos[i].virt);
    paginacion_tlb_sincronizar();   /* H205; H213: antes de devolver los marcos (ver monton_soltar) */
    for (unsigned i = 0; i < g_num_marcos; i++) {
        memset((void *)(g_marcos[i].fisica + hhdm), 0, TAMANO_PAGINA);
        pmm_liberar_pagina_fisica(g_marcos[i].fisica);
    }
    g_num_marcos = 0;
    g_num_regiones = 0;
}

/* Marcos a cero antes de mapearlos: un programa nunca ve memoria usada antes. */
static int mapear_region(uint64_t base, unsigned paginas, uint64_t flags, const void *contenido, size_t bytes) {
    uint64_t hhdm = memoria_obtener_hhdm_offset();
    if (g_num_regiones >= MAX_REGIONES) return -1;
    for (unsigned i = 0; i < paginas; i++) {
        if (g_num_marcos >= MAX_MARCOS) return -1;
        uint64_t f = pmm_asignar_pagina_fisica();
        if (!f) return -1;
        uint8_t *p = (uint8_t *)(f + hhdm);
        memset(p, 0, TAMANO_PAGINA);
        size_t o = (size_t)i * TAMANO_PAGINA;
        if (o < bytes) {
            size_t n = bytes - o;
            memcpy(p, (const uint8_t *)contenido + o, n > TAMANO_PAGINA ? TAMANO_PAGINA : n);
        }
        uint64_t v = base + o;
        if (paginacion_mapear(v, f, flags | PAGINA_PRESENTE | PAGINA_USUARIO)) {
            pmm_liberar_pagina_fisica(f);
            return -1;
        }
        g_marcos[g_num_marcos++] = (struct marco_mapeado){ v, f };
    }
    g_regiones[g_num_regiones++] = (struct region){ base, base + (uint64_t)paginas * TAMANO_PAGINA,
                                                    (flags & PAGINA_ESCRITURA) != 0 };
    return 0;
}

struct elf_cabecera {
    uint8_t ident[16];
    uint16_t tipo, maquina;
    uint32_t version;
    uint64_t entrada, phoff, shoff;
    uint32_t banderas;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));
struct elf_programa {
    uint32_t tipo, banderas;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

/* El archivo no es de confianza: cada campo se comprueba antes de usarlo. */
static int cargar_elf(const uint8_t *img, size_t bytes, uint64_t *entrada) {
    const uint64_t pila = ANILLO3_PILA_FIN - (uint64_t)ANILLO3_PILA_PAGINAS * TAMANO_PAGINA;
    const uint64_t limite = pila - TAMANO_PAGINA;
    struct elf_cabecera c;
    if (bytes < sizeof(c)) return -7;
    memcpy(&c, img, sizeof(c));
    if (c.ident[0] != 0x7F || c.ident[1] != 'E' || c.ident[2] != 'L' || c.ident[3] != 'F' ||
        c.ident[4] != 2 || c.ident[5] != 1 || c.tipo != 2 || c.maquina != 62) return -7;
    if (c.phentsize != sizeof(struct elf_programa) || !c.phnum || c.phnum > 8) return -8;
    if (c.phoff > bytes || (uint64_t)c.phnum * sizeof(struct elf_programa) > bytes - c.phoff) return -8;
    int entrada_ok = 0;
    for (unsigned i = 0; i < c.phnum; i++) {
        struct elf_programa p;
        memcpy(&p, img + c.phoff + (uint64_t)i * sizeof(p), sizeof(p));
        if (p.tipo != 1 || !p.memsz) continue;
        if (p.vaddr & (TAMANO_PAGINA - 1)) return -9;
        if (p.vaddr < ANILLO3_BASE || p.vaddr >= limite || p.memsz > limite - p.vaddr) return -9;
        if (p.filesz > p.memsz || p.offset > bytes || p.filesz > bytes - p.offset) return -10;
        if ((p.banderas & 2) && (p.banderas & 1)) return -11;  /* Escritura + ejecucion */
        uint64_t fin = (p.vaddr + p.memsz + TAMANO_PAGINA - 1) & ~(TAMANO_PAGINA - 1);
        for (unsigned r = 0; r < g_num_regiones; r++)
            if (p.vaddr < g_regiones[r].fin && fin > g_regiones[r].inicio) return -12;
        uint64_t f = (p.banderas & 2 ? PAGINA_ESCRITURA : 0) | (p.banderas & 1 ? 0 : PAGINA_NO_EJECUTABLE);
        if (mapear_region(p.vaddr, (unsigned)((fin - p.vaddr) / TAMANO_PAGINA), f, img + p.offset, (size_t)p.filesz)) return -6;
        if ((p.banderas & 1) && c.entrada >= p.vaddr && c.entrada < fin) entrada_ok = 1;
    }
    if (!entrada_ok) return -13;
    *entrada = c.entrada;
    return 0;
}

static void red_sockets_cerrar_todos(void);   /* H183: SYS_RED, mas abajo */
static void flujo_cerrar_si_abierto(void);    /* H187: SYS_FLUJO, mas abajo */
static void ar_cerrar_todos(void);            /* H188: SYS_ARCHIVOS por descriptor, mas abajo */
static void hp_empezar_programa(void);        /* H213, Fase D: hilos del programa, mas abajo */
static void hp_detener_todos(void);
int anillo3_ejecutar(const void *imagen, size_t bytes, uint64_t arg0, uint64_t arg1,
                     uint32_t limite_ms, uint32_t limite_total_ms, struct anillo3_resultado *res) {
    if (g_activo || !imagen || !bytes || !res || !limite_ms) return -1;
    if (!trabajos_es_coordinador() || trabajos_en_curso()) return -1;
    int r = preparar();
    if (r) return r;

    memset(res, 0, sizeof(*res));
    uint64_t pila = ANILLO3_PILA_FIN - (uint64_t)ANILLO3_PILA_PAGINAS * TAMANO_PAGINA, entrada = ANILLO3_BASE;
    if (bytes >= 4 && !memcmp(imagen, "\x7F" "ELF", 4)) {
        r = cargar_elf(imagen, bytes, &entrada);
        if (r) { desmapear_todo(); return r; }
    } else {
        if (bytes > ANILLO3_DATOS - ANILLO3_BASE) return -1;
        unsigned paginas = (unsigned)((bytes + TAMANO_PAGINA - 1) / TAMANO_PAGINA);
        if (mapear_region(ANILLO3_BASE, paginas, 0, imagen, bytes) ||
            mapear_region(ANILLO3_DATOS, ANILLO3_DATOS_PAGINAS, PAGINA_ESCRITURA | PAGINA_NO_EJECUTABLE, NULL, 0)) {
            desmapear_todo();
            return -6;
        }
    }
    if (mapear_region(pila, ANILLO3_PILA_PAGINAS, PAGINA_ESCRITURA | PAGINA_NO_EJECUTABLE, NULL, 0) ||
        g_num_regiones >= MAX_REGIONES) {
        desmapear_todo();
        return -6;
    }
    /* El monton empieza vacio: una region de largo 0 que SYS_MEMORIA agranda */
    g_monton_fin = TAEK_MONTON;
    g_monton_pico = 0;
    g_monton_region = (int)g_num_regiones;
    g_regiones[g_num_regiones++] = (struct region){ TAEK_MONTON, TAEK_MONTON, 1 };

    g_res = res;
    g_sup_siguiente = ANILLO3_SUPERFICIES;
    g_limite_total_ms = limite_total_ms;
    g_limite_ms = limite_ms;
    g_matar = 0;
    g_ultimo_texto[0] = 0;
    g_fs_kernel = leer_msr(MSR_FS_BASE);
    __asm__ volatile("fxsave64 %0" : "=m"(g_fp_nucleo));
    g_inicio_ms = g_ultimo_ms = tiempo_obtener_milisegundos();
    hp_empezar_programa();
    apic_escribir(LAPIC_TIMER_DIVISOR, 3);
    apic_escribir(LAPIC_TIMER_LVT, APIC_VECTOR_TEMPORIZADOR | LVT_PERIODICO);
    apic_escribir(LAPIC_TIMER_INICIAL, g_ticks_por_ms * TICK_MS);
    g_activo = 1;
    __asm__ volatile("fxrstor64 %0" :: "m"(g_fp_limpio));

    /* -8: al entrar a _start la pila se ve como tras un call, lo que el ABI y SSE esperan. */
    anillo3_entrar(&g_rsp_guardado, entrada, ANILLO3_PILA_FIN - 8, arg0, arg1);

    __asm__ volatile("fxrstor64 %0" :: "m"(g_fp_nucleo));
    g_activo = 0;
    apic_escribir(LAPIC_TIMER_INICIAL, 0);
    apic_escribir(LAPIC_TIMER_LVT, LVT_ENMASCARADO);
    hp_detener_todos();   /* H213: ningun hilo del programa sigue en Anillo 3 antes de soltar nada de su memoria */
    res->ms = tiempo_obtener_milisegundos() - g_inicio_ms;
    flujo_cerrar_si_abierto();                                         /* H187: una sesion de video no sobrevive al programa */
    /* Pase lo que pase con el programa, la pantalla vuelve a la consola antes de quitarle su memoria. */
    if (compositor_abierto()) {
        tomar_muestras();
        compositor_estadisticas(&res->composiciones, &res->ciclos_componer, &res->ciclos_presentar);
        compositor_cerrar();
    }
    if (g_raton) { xhci_hid_mouse_liberar(); puntero_retirar(); g_raton = 0; }
    res->monton_pico = g_monton_pico;
    if (g_audio_usado) { audio_ac97_detener(); g_audio_usado = 0; }   /* H146: un programa cerrado no sigue sonando */
    red_sockets_cerrar_todos();                                        /* H183: ni deja conexiones abiertas */
    ar_cerrar_todos();                                                 /* H188: ni archivos abiertos */
    desmapear_todo();
    g_res = NULL;
    return 0;
}

static uint64_t g_tls_principal;   /* H213: TLS (FS) del hilo principal que pidio con TAEK_HILO_TLS; 0 sin TLS */

void anillo3_al_entrar(void) {
    /* H213, Fase D: en las CPU 1 a 3 solo entran hilos de programa. El FS del nucleo es el TLS de su hilo_t. */
    if (cpu_actual()) {
        hilo_t *h = hilo_actual();
        escribir_msr(MSR_FS_BASE, h ? (uint64_t)(uintptr_t)h->tls : 0);
        if (g_smap) __asm__ volatile("clac");
        else __asm__ volatile("pushfq; andq %0, (%%rsp); popfq" :: "r"(~RFLAGS_AC) : "memory", "cc");
        return;
    }
    /* Primero: el codigo del nucleo direcciona con %fs y el programa pudo cambiarlo. */
    uint64_t fs = leer_msr(MSR_FS_BASE);
    if (fs != g_fs_kernel) {
        escribir_msr(MSR_FS_BASE, g_fs_kernel);
        if (g_res && !(g_tls_principal && fs == g_tls_principal)) g_res->fs_alterado++;
    }
    /* Las puertas no limpian AC y con AC=1 SMAP queda anulado. */
    if (g_smap) __asm__ volatile("clac");
    else __asm__ volatile("pushfq; andq %0, (%%rsp); popfq" :: "r"(~RFLAGS_AC) : "memory", "cc");
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    if ((f & RFLAGS_AC) && g_res) g_res->ac_en_nucleo++;
}

static int rango_valido(uint64_t p, uint64_t n, int escritura) {
    if (!n) return 0;
    for (unsigned i = 0; i < g_num_regiones; i++) {
        const struct region *r = &g_regiones[i];
        if (p >= r->inicio && p < r->fin && n <= r->fin - p && (!escritura || r->escribible)) return 1;
    }
    return 0;
}

static void copiar_de_usuario(void *d, uint64_t s, size_t n) {
    if (g_smap) __asm__ volatile("stac");
    memcpy(d, (const void *)s, n);
    if (g_smap) __asm__ volatile("clac");
}

static int64_t sys_escribir(uint64_t p, uint64_t n) {
    if (n > 4096 || !rango_valido(p, n, 0)) return -EFAULT;
    char b[129];
    for (uint64_t o = 0; o < n;) {
        size_t c = n - o > 128 ? 128 : (size_t)(n - o);
        copiar_de_usuario(b, p + o, c);
        for (size_t i = 0; i < c; i++) if ((unsigned char)b[i] < 32 && b[i] != '\n') b[i] = '?';
        b[c] = 0;
        if (o == 0) { memcpy(g_ultimo_texto, b, c < sizeof(g_ultimo_texto) ? c + 1 : sizeof(g_ultimo_texto)); g_ultimo_texto[sizeof(g_ultimo_texto) - 1] = 0; }
        consola_imprimir(b);
        o += c;
    }
    return (int64_t)n;
}

static void copiar_a_usuario(uint64_t d, const void *o, size_t n) {
    anillo3_acceso_usuario(1);
    memcpy((void *)d, o, n);
    anillo3_acceso_usuario(0);
}

static int64_t sys_superficie(uint64_t p) {
    struct taek_superficie s;
    if (!rango_valido(p, sizeof(s), 1)) return -EFAULT;
    copiar_de_usuario(&s, p, sizeof(s));
    if (!s.ancho || !s.alto || s.ancho > 8192 || s.alto > 8192) return -TAEK_EINVAL;
    if (s.banderas & ~TAEK_SUP_ALFA) return -TAEK_EINVAL;
    if (s.x < -32768 || s.y < -32768 || s.x > 32767 || s.y > 32767) return -TAEK_EINVAL;
    uint64_t paginas = ((uint64_t)s.ancho * s.alto * 4 + TAMANO_PAGINA - 1) / TAMANO_PAGINA;
    if (g_num_marcos + paginas > MAX_MARCOS || g_num_regiones >= MAX_REGIONES) return -TAEK_ENOMEM;
    uint64_t va = g_sup_siguiente;
    if (va + (paginas + 1) * TAMANO_PAGINA > ANILLO3_FIN) return -TAEK_ENOMEM;
    int r = compositor_abrir();
    if (r) return r;
    if (mapear_region(va, (unsigned)paginas, PAGINA_ESCRITURA | PAGINA_NO_EJECUTABLE, NULL, 0)) return -TAEK_ENOMEM;
    int id = compositor_crear(va, s.ancho, s.alto, s.x, s.y, s.banderas);
    if (id < 0) return id;
    g_sup_siguiente = va + (paginas + 1) * TAMANO_PAGINA;  /* Una pagina de guarda entre superficies */
    s.id = (uint32_t)id;
    s.pixeles = va;
    copiar_a_usuario(p, &s, sizeof(s));
    return 0;
}

/* Mouse USB Boot sin modulo: el cargador tiene un solo espacio y lo ocupa el video. Si un modulo ya lo tiene, se respeta;
 * se suelta al terminar el programa para que un modulo pueda tomarlo. Misma decodificacion que modulos/mouse_usb.c. */
static void raton_atender(void) {
    g_puntero_ms = tiempo_obtener_milisegundos();   /* H213: para medir cuanto pasa CPU 0 sin mirar el raton */
    (void)tactil_sondear();   /* Trackpad por I2C: una lectura del pin si no hay datos */
    if (!g_raton && xhci_hid_mouse_tomar() >= 0) g_raton = 1;
    if (!g_raton) return;
    struct taek_hid_mouse_reporte rep;
    for (unsigned i = 0; i < 64; i++) {
        int r = xhci_hid_mouse_leer(&rep);
        if (!r) return;
        if (r == -2) { puntero_retirar(); return; }
        if (r < 0) return;
        if (rep.longitud < 3 || rep.longitud > 8 || !rep.dispositivo) continue;
        puntero_publicar((int8_t)rep.datos[1], (int8_t)rep.datos[2], rep.datos[0] & 7u, rep.dispositivo);
    }
}

/* H141: el mouse y el trackpad se leen tambien mientras el programa espera. Antes solo se leian en taek_evento, una vez
 * por cuadro (60 por segundo), y en la laptop el trackpad y el receptor USB tiraban casi todo lo que nadie leia a tiempo:
 * en 12 minutos de menu llegaron 33 movimientos. Las teclas que traiga xHCI quedan en su cola para taek_evento. */
static void entrada_atender(void) {
    if (!teclado_es_modo_nativo()) xhci_sondeo();
    ps2_sondear();   /* H142: teclado interno y mouse PS/2; las teclas quedan en su cola */
    raton_atender();
    const struct puntero_estado *q = puntero_obtener_estado();
    compositor_puntero(q->x, q->y, q->dispositivo != 0);
}

static int64_t sys_evento(uint64_t p) {
    struct taek_evento e;
    if (!rango_valido(p, sizeof(e), 1)) return -EFAULT;
    char c = consola_leer_caracter();  /* Tambien atiende serie y LAN, y el sondeo de xHCI */
    raton_atender();
    const struct puntero_estado *q = puntero_obtener_estado();
    memset(&e, 0, sizeof(e));
    e.x = q->x; e.y = q->y; e.botones = q->botones | puntero_tomar_pulsados(); e.tecla = (uint8_t)c; e.hay_puntero = q->dispositivo != 0;
    compositor_puntero(q->x, q->y, q->dispositivo != 0);
    copiar_a_usuario(p, &e, sizeof(e));
    return c ? 1 : 0;
}

static void copiar_txt(char *d, size_t n, const char *o) {
    size_t i = 0;
    while (o && o[i] && i + 1 < n) { d[i] = o[i]; i++; }
    d[i] = 0;
}

static uint8_t cmos(uint8_t r) { escribir_puerto_b(0x70, r); return leer_puerto_b(0x71); }

/* Reloj del equipo tal cual: la hora local o UTC la decide quien lo configuro, y no se toca. */
static void leer_fecha(struct taek_info *t) {
    int intentos = 100000;
    while ((cmos(0x0A) & 0x80) && intentos > 0) intentos--;
    if (!intentos) return;
    uint8_t s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mes = cmos(8), a = cmos(9), b = cmos(0x0B);
    int pm = h & 0x80;
    h &= 0x7F;
    if (!(b & 4)) {
        s = (uint8_t)((s >> 4) * 10 + (s & 15)); m = (uint8_t)((m >> 4) * 10 + (m & 15));
        h = (uint8_t)((h >> 4) * 10 + (h & 15)); d = (uint8_t)((d >> 4) * 10 + (d & 15));
        mes = (uint8_t)((mes >> 4) * 10 + (mes & 15)); a = (uint8_t)((a >> 4) * 10 + (a & 15));
    }
    if (!(b & 2) && pm) h = (uint8_t)((h % 12) + 12);
    if (mes < 1 || mes > 12 || d < 1 || d > 31 || h > 23 || m > 59 || s > 59) return;
    t->anio = (uint16_t)(2000 + a); t->mes = mes; t->dia = d; t->hora = h; t->minuto = m; t->segundo = s; t->fecha_ok = 1;
}

static int64_t sys_info(uint64_t p, uint64_t con_bateria) {
    static struct taek_info t;
    if (!rango_valido(p, sizeof(t), 1)) return -EFAULT;
    memset(&t, 0, sizeof(t));

    uint32_t a, b, c, d, *m = (uint32_t *)t.cpu;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a >= 0x80000004u) for (uint32_t i = 0; i < 3; i++)
        __asm__ volatile("cpuid" : "=a"(m[i * 4]), "=b"(m[i * 4 + 1]), "=c"(m[i * 4 + 2]), "=d"(m[i * 4 + 3]) : "a"(0x80000002u + i), "c"(0));
    t.cpu[47] = 0;
    t.cpus = trabajos_cpu_arrancadas();
    copiar_txt(t.hito, sizeof(t.hito), taek_obtener_hito());
    copiar_txt(t.capa, sizeof(t.capa), compositor_capa_nombre());
    for (int i = 0; i < pci_obtener_conteo(); i++) {
        const struct dispositivo_pci *q = pci_obtener_dispositivo(i);
        if (!q || q->clase != 3) continue;
        t.gpu_proveedor_id = q->id_proveedor;
        t.gpu_id = q->id_dispositivo;
        copiar_txt(t.gpu_proveedor, sizeof(t.gpu_proveedor), pci_nombre_proveedor(q->id_proveedor));
        break;
    }
    /* La misma lista corta que el modulo de video: solo se anuncia lo probado en fisico. */
    t.gpu_video_probada = t.gpu_proveedor_id == 0x8086 && t.gpu_id == 0x5917;
    memoria_estadisticas_t e;
    memoria_obtener_estadisticas(&e);
    t.ram_total = e.ram_fisica_usable;
    t.ram_libre = e.paginas_libres * TAMANO_PAGINA;
    t.tsc_mhz = tiempo_ciclos_por_ms() / 1000;
    t.activo_ms = tiempo_obtener_milisegundos();
    leer_fecha(&t);
    const struct red_info *ri = red_obtener_info();
    if (ri && ri->id_proveedor) { t.red_presente = 1; t.red_enlace = ri->enlace_activo != 0; t.red_mbps = ri->velocidad_mbps; }
    const struct red_config *rc = red_obtener_config();
    if (rc && (rc->ip[0] | rc->ip[1] | rc->ip[2] | rc->ip[3])) { t.red_ip_ok = 1; memcpy(t.ip, rc->ip, 4); }
    t.bateria_pct = -1;
    t.cargador = -1;
    if (con_bateria) bateria_resumen(&t.bateria_pct, &t.bateria_estado, &t.cargador);
    t.discos = (uint32_t)usb_msc_obtener_cantidad();
    t.smep = (uint32_t)g_smep;
    t.smap = (uint32_t)g_smap;
    t.ancho = (uint32_t)pantalla_obtener_ancho();
    t.alto = (uint32_t)pantalla_obtener_alto();
    copiar_a_usuario(p, &t, sizeof(t));
    return 0;
}

/* Lo que un programa de prueba ve en pantalla, a la mitad de resolucion y en RGB565 con repeticiones, en base64 por el registro
 * que tambien lleva la LAN. Solo existe para que las pruebas puedan mirar el framebuffer real desde el PC. */
static void cap_b64(const uint8_t *b, unsigned n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static unsigned en_linea;
    char s[5] = { 0 };
    for (unsigned i = 0; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16 | (uint32_t)b[i + 1] << 8 | b[i + 2];
        s[0] = t[v >> 18]; s[1] = t[(v >> 12) & 63]; s[2] = t[(v >> 6) & 63]; s[3] = t[v & 63];
        serial_imprimir(s);
        /* Vaciar con varias tramas llenas: una casi vacia hace esperar 20 ms al stream. */
        if (++en_linea == 40) {
            static unsigned lineas;
            en_linea = 0;
            serial_imprimir_linea("");
            if (++lineas == 50) { lineas = 0; red_stream_drenar(); }
        }
    }
}

static int64_t sys_captura(uint64_t p, uint64_t n) {
    if (!g_captura_permitida) return -ENOSYS;
    char nombre[32];
    if (n >= sizeof(nombre) || !rango_valido(p, n, 0)) return -EFAULT;
    copiar_de_usuario(nombre, p, (size_t)n);
    nombre[n] = 0;
    for (uint64_t i = 0; i < n; i++) if (!((nombre[i] >= 'a' && nombre[i] <= 'z') || (nombre[i] >= '0' && nombre[i] <= '9') || nombre[i] == '-')) return -TAEK_EINVAL;
    uint32_t W = (uint32_t)pantalla_obtener_ancho(), H = (uint32_t)pantalla_obtener_alto();
    serial_imprimir_linea("");
    serial_imprimir("[CAPTURA] inicio "); serial_imprimir(nombre);
    serial_imprimir(" "); serial_imprimir_dec(W / 2); serial_imprimir(" "); serial_imprimir_dec(H / 2); serial_imprimir_linea("");
    uint8_t b[3 * 64];
    unsigned u = 0;
    uint32_t anterior = 0, cuenta = 0;
    for (uint32_t y = 0; y < H / 2; y++) for (uint32_t x = 0; x <= W / 2; x++) {
        uint32_t c = 0xFFFFFFFFu;
        if (x < W / 2) {
            uint32_t v = compositor_leer_pantalla(x * 2, y * 2);
            c = (v >> 8 & 0xF800) | (v >> 5 & 0x07E0) | (v >> 3 & 0x001F);
        }
        if (c == anterior && cuenta < 255) { cuenta++; continue; }
        if (cuenta) {
            b[u++] = (uint8_t)cuenta; b[u++] = (uint8_t)(anterior >> 8); b[u++] = (uint8_t)anterior;
            if (u == sizeof(b)) { cap_b64(b, u); u = 0; }
        }
        anterior = c; cuenta = c == 0xFFFFFFFFu ? 0 : 1;
    }
    if (cuenta) { b[u++] = (uint8_t)cuenta; b[u++] = (uint8_t)(anterior >> 8); b[u++] = (uint8_t)anterior; }
    cap_b64(b, u);
    serial_imprimir_linea("");
    serial_imprimir("[CAPTURA] fin "); serial_imprimir(nombre); serial_imprimir_linea("");
    red_stream_drenar();
    g_ultimo_ms = tiempo_obtener_milisegundos();
    return 0;
}

static int64_t sys_equipo(uint64_t p) {
    static struct taek_equipo t;
    if (!rango_valido(p, sizeof(t), 1)) return -EFAULT;
    equipo_resumen(&t);
    bateria_detalle(&t);
    copiar_a_usuario(p, &t, sizeof(t));
    return 0;
}

/* H150: sensores para el panel de Configuracion. Un MSR que el procesador no tiene provoca #GP en el nucleo, asi que
 * cada lectura va detras de su bit de CPUID: sensor digital (06h EAX bit 0) y el del paquete (bit 6). 0x1A2 existe en
 * los Intel de familia 6 desde Nehalem (modelo 0x1A); en los anteriores se supone TjMax de 100 grados y se dice. */
static void sensores_temperatura(struct taek_sensores *t) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (b != 0x756E6547u || d != 0x49656E69u || c != 0x6C65746Eu || a < 6) return;   /* "GenuineIntel" */
    uint32_t firma;
    __asm__ volatile("cpuid" : "=a"(firma), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    uint32_t familia = (firma >> 8) & 15, modelo = ((firma >> 4) & 15) | (((firma >> 16) & 15) << 4);
    uint32_t eax6;
    __asm__ volatile("cpuid" : "=a"(eax6), "=b"(b), "=c"(c), "=d"(d) : "a"(6), "c"(0));
    if (!(eax6 & 1)) return;
    t->tjmax_c = 100;
    if (familia == 6 && modelo >= 0x1A) {
        uint32_t tj = (uint32_t)(leer_msr(0x1A2) >> 16) & 0xFF;
        if (tj >= 60 && tj <= 130) { t->tjmax_c = (int32_t)tj; t->tjmax_leido = 1; }
    }
    uint64_t s = leer_msr(0x19C);
    t->termico_estado = (uint32_t)s & 0xFFFF;
    if (s & (1ULL << 31)) { t->temp_ok = 1; t->temp_nucleo_c = t->tjmax_c - (int32_t)((s >> 16) & 0x7F); }
    if (eax6 & (1u << 6)) {
        uint64_t p = leer_msr(0x1B1);
        t->paquete_estado = (uint32_t)p & 0xFFFF;
        t->temp_paquete_ok = 1;
        t->temp_paquete_c = t->tjmax_c - (int32_t)((p >> 16) & 0x7F);
    }
    /* H151: APERF (0xE8) y MPERF (0xE7) existen si CPUID 06h ECX bit 0 */
    uint32_t ecx6;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(ecx6), "=d"(d) : "a"(6), "c"(0));
    if (ecx6 & 1) { t->aperf_ok = 1; t->mperf = leer_msr(0xE7); t->aperf = leer_msr(0xE8); }
}

/* H204: la parte de la version 3 (Fase 0 del gestor de hilos) */
static void sensores_latencia(struct taek_sensores *t) {
    t->usb_max_ms = atencion_usb.max_ms;
    t->usb_max_sys_ini = atencion_usb.max_sys_ini; t->usb_max_sys_fin = atencion_usb.max_sys_fin;
    t->usb_max_cuando_ms = atencion_usb.max_cuando_ms; t->usb_pasadas = atencion_usb.pasadas;
    const struct xhci_fase0 *x = xhci_fase0();
    t->usb_reentradas = x->reentradas; t->usb_eventos_max = x->eventos_max; t->usb_anillo_lleno = x->anillo_lleno;
    t->usb_cambios_puerto = x->cambios_puerto;
    t->usb_reintentos = x->reintentos; t->usb_recuperados = x->recuperados; t->usb_perdidos = x->perdidos;
    t->teclas_perdidas = x->teclas_perdidas;
    t->raton_perdidos = xhci_hid_mouse_perdidos();
    t->red_max_ms = atencion_red.max_ms;
    t->red_max_sys_ini = atencion_red.max_sys_ini; t->red_max_sys_fin = atencion_red.max_sys_fin;
    t->red_max_cuando_ms = atencion_red.max_cuando_ms; t->red_pasadas = atencion_red.pasadas;
    if (audio_hda_esta_operativo()) {
        t->hda_max_ms = audio_hda_obtener_estado()->max_intervalo_sin_atencion_ms;
        t->hda_saltos_tardios = audio_hda_saltos_tardios();
    }
    for (unsigned i = 0; i < 32 && i < ATENCION_SYS_MAX; i++) t->sys_max_us[i] = atencion_sys_max_us[i];
    struct xhci_suceso_puerto s[XHCI_SUCESOS];
    unsigned n = xhci_fase0_sucesos(s, 8);
    t->sucesos_total = x->sucesos_total;
    t->sucesos_n = n;
    for (unsigned i = 0; i < n; i++) {
        t->sucesos[i].ms = s[i].ms; t->sucesos[i].portsc = s[i].portsc; t->sucesos[i].puerto = s[i].puerto;
        t->sucesos[i].tipo = s[i].tipo; t->sucesos[i].intento = s[i].intento;
    }
    struct atencion_hueco h[ATENCION_HUECOS];
    unsigned nh = atencion_huecos_copiar(h, 8);
    t->huecos_total = atencion_huecos_total;
    t->huecos_n = nh;
    for (unsigned i = 0; i < nh; i++) {
        t->huecos[i].fin_ms = h[i].fin_ms; t->huecos[i].ms = h[i].ms; t->huecos[i].que = h[i].que;
        t->huecos[i].sys_ini = h[i].sys_ini; t->huecos[i].sys_fin = h[i].sys_fin;
    }
}

static int64_t sys_sensores(uint64_t p) {
    static struct taek_sensores t;
    static uint8_t sector[4096];
    /* Primero op, que dice de que tamano es la estructura del programa (H204) */
    if (!rango_valido(p, __builtin_offsetof(struct taek_sensores, temp_ok), 1)) return -EFAULT;
    uint32_t op;
    copiar_de_usuario(&op, p + __builtin_offsetof(struct taek_sensores, op), sizeof(op));
    uint64_t tam = (op & TAEK_SEN_V3) ? sizeof(t) : TAEK_SENSORES_V2_TAM;
    if (!rango_valido(p, tam, 1)) return -EFAULT;
    memset(&t, 0, sizeof(t));
    t.version = (op & TAEK_SEN_V3) ? TAEK_SENSORES_VERSION : 2;
    t.op = op;
    if (op & TAEK_SEN_V3) sensores_latencia(&t);
    if (op & TAEK_SEN_CERO_LATENCIA) { atencion_cero(); xhci_fase0_cero(); }
    sensores_temperatura(&t);
    /* Version 2: carga por CPU */
    t.cpus = trabajos_cpu_arrancadas();
    t.tsc = leer_tsc();
    /* H215, Fase E1: todas las esperas activas de CPU 0 (antes solo SYS_ESPERAR): el panel muestra el trabajo real */
    t.espera_ciclos = atencion_cpu0_espera;
    for (unsigned i = 0; i < 4; i++) { t.ocupado_ciclos[i] = trabajos_ocupado_ciclos(i); t.ticks_por_ms[i] = trabajos_ticks_por_ms(i); }

    const struct red_info *ri = red_obtener_info();
    if (ri && ri->id_proveedor) {
        t.red_presente = 1; t.red_enlace = ri->enlace_activo != 0; t.red_mbps = ri->velocidad_mbps; t.red_duplex = ri->full_duplex != 0;
        t.red_proveedor = ri->id_proveedor; t.red_dispositivo = ri->id_dispositivo;
        memcpy(t.mac, ri->mac, 6);
        copiar_txt(t.red_modelo, sizeof(t.red_modelo), ri->modelo ? ri->modelo : "");
        t.rx_paquetes = ri->rx_paquetes; t.tx_paquetes = ri->tx_paquetes; t.rx_bytes = ri->rx_bytes; t.tx_bytes = ri->tx_bytes;
        t.rx_descartados = ri->rx_descartados; t.tx_errores = ri->tx_errores;
    }
    const struct red_config *rc = red_obtener_config();
    if (rc && rc->configurada) {
        t.ip_ok = 1; t.por_dhcp = rc->por_dhcp != 0;
        memcpy(t.ip, rc->ip, 4); memcpy(t.mascara, rc->mascara, 4); memcpy(t.puerta, rc->puerta, 4);
    }
    t.stream_activo = (uint32_t)red_stream_activo();
    t.stream_conectado = (uint32_t)red_stream_conectado();
    uint16_t puerto = 0;
    red_stream_estado(t.stream_ip, &puerto, &t.stream_enviado);
    t.stream_puerto = puerto;

    memoria_estadisticas_t e;
    memoria_obtener_estadisticas(&e);
    t.heap_capacidad = e.heap_capacidad_total; t.heap_en_uso = e.heap_bytes_en_uso;
    t.heap_bloques_activos = e.heap_bloques_activos; t.heap_bloques_libres = e.heap_bloques_libres;
    t.heap_canarios_ok = (uint32_t)e.canarios_intactos;
    t.paginas_totales = e.paginas_totales; t.paginas_libres = e.paginas_libres;
    t.canario_inicio = CANARIO_BLOQUE_INICIO; t.canario_fin = CANARIO_BLOQUE_FIN;
    t.heap_total_bloques = memoria_mapa_heap(t.heap_mapa, sizeof(t.heap_mapa), &t.heap_nmapa);
    huevo_estabilidad_t h;
    huevo_obtener(&h);
    t.huevo_magico = h.magico; t.huevo_salud = h.salud; t.huevo_estado = (uint32_t)h.estado; t.huevo_canario = h.canario;

    /* Solo a pedido: leer un sector del USB tarda milisegundos. Es la misma comprobacion que hace el montaje (0x6A). */
    if ((op & TAEK_SEN_LEER_VOLUMEN) && exfat_esta_montado()) {
        const struct exfat_volumen *v = exfat_obtener_volumen();
        if (v && usb_msc_leer_sectores(v->unidad_msc, v->lba_inicio_particion, 1, sector) == 0 && !memcmp(sector + 3, "EXFAT   ", 8)) {
            t.vol_leido = 1;
            t.vol_lba = v->lba_inicio_particion;
            t.vol_banderas = (uint16_t)(sector[0x6A] | sector[0x6B] << 8);
            memcpy(t.vol_bytes, sector + 0x60, 16);
        }
    }
    copiar_a_usuario(p, &t, tam);
    return 0;
}

/* H143: el programa pide una pagina y el nucleo la descarga con su propio cliente (TCP, TLS, HTTP). Al programa solo
 * llegan la URL final, el estado y el cuerpo, copiados a memoria que el mismo declaro escribible. Puede tardar varios
 * segundos: el reloj de Anillo 3 solo cuenta mientras el programa corre, no dentro de una llamada. */
static struct taek_web g_web;
static struct { const char *cuerpo; size_t n; int estado, detalle, r; } g_descarga;
/* La pila de las llamadas al sistema (TSS rsp0) es de 32 KB y el apreton de manos TLS de mbedTLS, con X.509 y curvas
 * elipticas, puede usar buena parte. La descarga corre en una pila propia y amplia. */
static uint8_t g_pila_web[256 * 1024] __attribute__((aligned(16)));
static void descargar_en_pila(void) {
    g_descarga.r = navegador_descargar(g_web.url, sizeof(g_web.url), &g_descarga.cuerpo, &g_descarga.n,
                                       &g_descarga.estado, &g_descarga.detalle);
}
static void en_pila_web(void (*f)(void)) {
    void *tope = g_pila_web + sizeof(g_pila_web);
    __asm__ volatile("mov %%rsp, %%rbx\n\t"
                     "mov %1, %%rsp\n\t"
                     "call *%0\n\t"
                     "mov %%rbx, %%rsp"
                     :: "r"(f), "r"(tope)
                     : "rbx", "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
}
static int64_t sys_web(uint64_t p) {
    if (!rango_valido(p, sizeof(g_web), 1)) return -EFAULT;
    copiar_de_usuario(&g_web, p, sizeof(g_web));
    g_web.url[sizeof(g_web.url) - 1] = 0;
    if (g_web.capacidad && !rango_valido(g_web.destino, g_web.capacidad, 1)) return -EFAULT;
    en_pila_web(descargar_en_pila);
    const char *cuerpo = g_descarga.cuerpo; size_t n = g_descarga.n;
    int estado = g_descarga.estado, detalle = g_descarga.detalle, r = g_descarga.r;
    g_web.estado = estado;
    g_web.error = r ? (r == 2 ? 2000 + detalle : r) : 0;
    g_web.total = (uint32_t)(n > 0xFFFFFFFFu ? 0xFFFFFFFFu : n);
    g_web.bytes = 0;
    if (!r && g_web.capacidad) {
        g_web.bytes = n < g_web.capacidad ? (uint32_t)n : g_web.capacidad;
        copiar_a_usuario(g_web.destino, cuerpo, g_web.bytes);
    }
    copiar_a_usuario(p, &g_web, sizeof(g_web));
    return r ? -(int64_t)r : (int64_t)g_web.bytes;
}

/* ================================================================== H183: SYS_RED
 * Sockets para el programa: TCP (manejadores 1-7 de la pila) y TLS 1.3 (sesiones de tls.c). El TLS corre en la
 * pila amplia, como la web. Lo que el programa deje abierto se cierra al salir (red_sockets_cerrar_todos). */
#define RED_SOCKETS 8
static struct { int usado, tls, id; } g_sock[RED_SOCKETS];
static struct taek_red g_red;
static uint8_t g_red_buf[64 * 1024];
static struct { char host[TAEK_RED_HOST]; uint8_t ip[4]; uint16_t puerto; int r; int id; uint32_t n; } g_red_tls;
static void tls_abrir_en_pila(void) { g_red_tls.r = red_tls_abrir(g_red_tls.host, g_red_tls.ip, g_red_tls.puerto, (int)g_red.espera_ms); }
static void tls_enviar_en_pila(void) { g_red_tls.r = red_tls_enviar(g_red_tls.id, g_red_buf, g_red_tls.n, (int)g_red.espera_ms); }
static void tls_recibir_en_pila(void) { g_red_tls.r = red_tls_recibir(g_red_tls.id, g_red_buf, g_red_tls.n); }
/* H185: SYS_VIDEO. El MP4 queda en memoria del nucleo hasta que ABRIR "mem:..." lo reproduce (o llega otro). */
static uint8_t *g_video; static uint64_t g_video_n;
static int64_t sys_video(uint64_t p, uint64_t n) {
    if (!n || n > TAEK_VIDEO_MAX || !rango_valido(p, n, 0)) return -TAEK_EINVAL;
    if (g_video) { liberar_memoria(g_video); g_video = 0; g_video_n = 0; }
    g_video = asignar_memoria((size_t)n);
    if (!g_video) return -TAEK_ENOMEM;
    copiar_de_usuario(g_video, p, n);
    g_video_n = n;
    return 0;
}

/* H187: SYS_FLUJO. La muestra se copia a memoria del nucleo (la GPU y los AP no leen memoria del programa) y todo corre
 * en la pila amplia: el decodificador y la conversion a RGB usan SSE2, asi que el estado SSE del programa se guarda y
 * se repone alrededor. Mientras la sesion este abierta el video se dibuja directo sobre la pantalla; al cerrar, el
 * compositor vuelve a pintar todo. */
static struct taek_flujo g_flujo_pet;
static uint8_t *g_flujo_buf;
static uint32_t g_flujo_cap;   /* H201: crece hasta TAEK_FLUJO_OSD_MAX para una capa de pantalla completa */
static int g_flujo_r;
static uint8_t g_fp_programa[512] __attribute__((aligned(16)));
static void flujo_en_pila(void) {
    struct taek_flujo *f = &g_flujo_pet;
    switch (f->op) {
    case TAEK_FLUJO_ABRIR:
        g_flujo_r = reproductor_flujo_abrir(g_flujo_buf, f->largo, f->canales, f->frecuencia, f->escala,
                                            (f->banderas & TAEK_FLUJO_DESDE) != 0, f->inicio);
        break;
    case TAEK_FLUJO_OSD:
        g_flujo_r = reproductor_flujo_osd(f->osd_ms ? (const uint32_t *)g_flujo_buf : 0, f->osd_x, f->osd_y,
                                          f->osd_ancho, f->osd_alto, f->osd_ms);
        break;
    case TAEK_FLUJO_VIDEO: g_flujo_r = reproductor_flujo_video(g_flujo_buf, f->largo, f->pts); break;
    case TAEK_FLUJO_AUDIO: g_flujo_r = reproductor_flujo_audio(g_flujo_buf, f->largo, f->pts); break;
    case TAEK_FLUJO_CERRAR:
        g_flujo_r = reproductor_flujo_cerrar(f->banderas & TAEK_FLUJO_DRENAR);
        if (compositor_abierto()) compositor_recomponer();
        break;
    default: g_flujo_r = 0; break;
    }
}
static void flujo_cerrar_si_abierto(void) {
    if (!reproductor_flujo_abierto()) return;
    g_flujo_pet.op = TAEK_FLUJO_CERRAR; g_flujo_pet.banderas = 0;
    en_pila_web(flujo_en_pila);
}
static int64_t sys_flujo(uint64_t p) {
    if (!rango_valido(p, sizeof(g_flujo_pet), 1)) return -EFAULT;
    copiar_de_usuario(&g_flujo_pet, p, sizeof(g_flujo_pet));
    struct taek_flujo *f = &g_flujo_pet;
    int64_t r = 0;
    int con_datos = f->op == TAEK_FLUJO_ABRIR || f->op == TAEK_FLUJO_VIDEO || f->op == TAEK_FLUJO_AUDIO ||
                    (f->op == TAEK_FLUJO_OSD && f->osd_ms);
    if (f->op < TAEK_FLUJO_ABRIR || f->op > TAEK_FLUJO_MARCO) { r = -TAEK_EINVAL; goto salida; }
    if (f->op == TAEK_FLUJO_OSD && f->osd_ms && (uint64_t)f->osd_ancho * f->osd_alto * 4 != f->largo) { r = -TAEK_EINVAL; goto salida; }
    /* H201: el marco se pide antes de ABRIR (decide la presentacion por Iris) o durante la sesion */
    if (f->op == TAEK_FLUJO_MARCO) {
        r = reproductor_flujo_marco(f->marco_x, f->marco_y, f->marco_ancho, f->marco_alto) ? -TAEK_EINVAL : 0;
        goto salida;
    }
    if (f->op != TAEK_FLUJO_ABRIR && f->op != TAEK_FLUJO_ESTADO && !reproductor_flujo_abierto()) { r = -TAEK_EBADF; goto salida; }
    if (con_datos) {
        uint32_t tope = f->op == TAEK_FLUJO_OSD ? TAEK_FLUJO_OSD_MAX : TAEK_FLUJO_MUESTRA_MAX;
        if (!f->largo || f->largo > tope || !rango_valido(f->datos, f->largo, 0)) { r = -TAEK_EINVAL; goto salida; }
        uint32_t quiere = f->largo > TAEK_FLUJO_MUESTRA_MAX ? f->largo : TAEK_FLUJO_MUESTRA_MAX;
        if (g_flujo_buf && g_flujo_cap < quiere) { liberar_memoria(g_flujo_buf); g_flujo_buf = 0; g_flujo_cap = 0; }
        if (!g_flujo_buf) { g_flujo_buf = asignar_memoria(quiere); g_flujo_cap = g_flujo_buf ? quiere : 0; }
        if (!g_flujo_buf) { r = -TAEK_ENOMEM; goto salida; }
        copiar_de_usuario(g_flujo_buf, f->datos, f->largo);
    }
    if (f->op != TAEK_FLUJO_ESTADO) {
        __asm__ volatile("fxsave64 %0" : "=m"(g_fp_programa));
        __asm__ volatile("fxrstor64 %0" :: "m"(g_fp_nucleo));
        en_pila_web(flujo_en_pila);
        __asm__ volatile("fxrstor64 %0" :: "m"(g_fp_programa));
        r = g_flujo_r == 1 ? 1 : g_flujo_r < 0 ? (f->op == TAEK_FLUJO_ABRIR && g_flujo_r == -2 ? -TAEK_ENOMEM : -TAEK_EINVAL) : 0;
        if (f->op == TAEK_FLUJO_ABRIR && !r) g_audio_usado = 1;   /* si el programa muere, el audio se detiene */
    }
salida:;
    int64_t reloj; uint32_t cola, cuadros, tardios, motor;
    reproductor_flujo_estado(&reloj, &cola, &cuadros, &tardios, &motor);
    f->reloj_ms = (uint64_t)reloj; f->cola_audio_ms = cola; f->cuadros = cuadros; f->tardios = tardios; f->motor = motor;
    f->abierta = reproductor_flujo_abierto();
    f->iris = (uint32_t)reproductor_flujo_iris();
    f->teclas_n = reproductor_flujo_teclas(f->teclas, sizeof(f->teclas));
    copiar_a_usuario(p, f, sizeof(*f));
    return r;
}

static void red_sockets_cerrar_todos(void) {
    for (int i = 0; i < RED_SOCKETS; i++) {
        if (!g_sock[i].usado) continue;
        if (g_sock[i].tls) red_tls_cerrar(g_sock[i].id); else red_tcp_cerrar_h(g_sock[i].id);
        g_sock[i].usado = 0;
    }
}
static int64_t sys_red(uint64_t p) {
    if (!rango_valido(p, sizeof(g_red), 1)) return -EFAULT;
    copiar_de_usuario(&g_red, p, sizeof(g_red));
    g_red.host[TAEK_RED_HOST - 1] = 0;
    g_red.hechos = 0; g_red.error = 0;
    int64_t r = 0;
    if (g_red.espera_ms > 60000) g_red.espera_ms = 60000;
    if (g_red.op == TAEK_RED_ABRIR) {
        int s = -1;
        for (int i = 0; i < RED_SOCKETS; i++) if (!g_sock[i].usado) { s = i; break; }
        if (s < 0) { r = -TAEK_RED_ELLENO; goto fin; }
        if (!g_red.host[0] || !g_red.puerto) { r = -TAEK_RED_EPARAM; goto fin; }
        uint32_t espera = g_red.espera_ms ? g_red.espera_ms : 15000;
        uint8_t ip[4];
        int e = red_resolver_nombre(g_red.host, ip, (int)(espera < 5000 ? espera : 5000));
        if (e) { g_red.error = e; r = -TAEK_RED_EDNS; goto fin; }
        int id;
        if (g_red.banderas & TAEK_RED_TLS) {
            memcpy(g_red_tls.host, g_red.host, TAEK_RED_HOST);
            memcpy(g_red_tls.ip, ip, 4); g_red_tls.puerto = g_red.puerto;
            g_red.espera_ms = espera;
            en_pila_web(tls_abrir_en_pila);
            id = g_red_tls.r;
            if (id < 0) { g_red.error = id; r = -TAEK_RED_ETLS; goto fin; }
        } else {
            id = red_tcp_abrir(ip, g_red.puerto, (int)espera);
            if (id < 0) { g_red.error = id; r = -TAEK_RED_ECONECTAR; goto fin; }
        }
        g_sock[s].usado = 1; g_sock[s].tls = (g_red.banderas & TAEK_RED_TLS) ? 1 : 0; g_sock[s].id = id;
        g_red.id = s; g_red.estado = 1; r = s;
        goto fin;
    }
    if (g_red.id < 0 || g_red.id >= RED_SOCKETS || !g_sock[g_red.id].usado) { r = -TAEK_RED_EPARAM; goto fin; }
    int s = g_red.id;
    if (g_red.op == TAEK_RED_CERRAR) {
        if (g_sock[s].tls) red_tls_cerrar(g_sock[s].id); else red_tcp_cerrar_h(g_sock[s].id);
        g_sock[s].usado = 0; g_red.estado = 0;
        goto fin;
    }
    if (g_red.op == TAEK_RED_ESTADO) {
        g_red.estado = g_sock[s].tls ? 1 : red_tcp_estado_h(g_sock[s].id);
        goto fin;
    }
    if (g_red.op == TAEK_RED_ENVIAR) {
        uint32_t n = g_red.largo > sizeof(g_red_buf) ? (uint32_t)sizeof(g_red_buf) : g_red.largo;
        if (!n || !rango_valido(g_red.datos, n, 0)) { r = -TAEK_RED_EPARAM; goto fin; }
        copiar_de_usuario(g_red_buf, g_red.datos, n);
        if (g_sock[s].tls) {
            g_red_tls.id = g_sock[s].id; g_red_tls.n = n;
            en_pila_web(tls_enviar_en_pila);
            if (g_red_tls.r < 0) { g_red.error = g_red_tls.r; r = -TAEK_RED_ETLS; goto fin; }
        } else {
            uint64_t limite = tiempo_obtener_milisegundos() + (g_red.espera_ms ? g_red.espera_ms : 15000);
            for (uint32_t off = 0; off < n;) {
                uint32_t trozo = n - off > 1400 ? 1400 : n - off;
                int e = red_tcp_enviar_h(g_sock[s].id, g_red_buf + off, trozo);
                if (e == 0) { off += trozo; continue; }
                if (e < 0) { r = -TAEK_RED_ECERRADA; goto fin; }
                if (tiempo_obtener_milisegundos() > limite) { r = -TAEK_RED_ETIEMPO; goto fin; }
                red_pila_sondeo();
            }
        }
        g_red.hechos = n; r = n;
        goto fin;
    }
    if (g_red.op == TAEK_RED_RECIBIR) {
        uint32_t cap = g_red.largo > sizeof(g_red_buf) ? (uint32_t)sizeof(g_red_buf) : g_red.largo;
        if (!cap || !rango_valido(g_red.datos, cap, 1)) { r = -TAEK_RED_EPARAM; goto fin; }
        uint64_t limite = tiempo_obtener_milisegundos() + g_red.espera_ms;
        int n;
        for (;;) {
            if (g_sock[s].tls) {
                g_red_tls.id = g_sock[s].id; g_red_tls.n = cap;
                en_pila_web(tls_recibir_en_pila);
                n = g_red_tls.r;
            } else {
                n = red_tcp_recibir_h(g_sock[s].id, g_red_buf, cap);
            }
            if (n != 0 || tiempo_obtener_milisegundos() >= limite) break;
            red_pila_sondeo();
        }
        if (n < 0) { g_red.error = n; g_red.estado = 0; r = -TAEK_RED_ECERRADA; goto fin; }
        if (n) copiar_a_usuario(g_red.datos, g_red_buf, (uint32_t)n);
        g_red.hechos = (uint32_t)n; g_red.estado = 1; r = n;
        goto fin;
    }
    r = -TAEK_RED_EPARAM;
fin:
    copiar_a_usuario(p, &g_red, sizeof(g_red));
    return r;
}

/* H151: una orden de texto de la terminal, pedida por la interfaz. Corre en la pila amplia (como la web): algunas ordenes
 * usan bastante pila y la de las llamadas al sistema es de 32 KB. Lo que imprime queda tambien en la consola guardada, en
 * el serial y en la LAN, como si se hubiera escrito en la terminal. */
int terminal_orden_clase(const char *linea);
void terminal_ejecutar_orden(const char *linea);
static struct taek_orden g_orden;
static char g_orden_texto[64 * 1024];
static void orden_en_pila(void) { terminal_ejecutar_orden(g_orden.linea); }
static int64_t sys_orden(uint64_t p) {
    if (!rango_valido(p, sizeof(g_orden), 1)) return -EFAULT;
    copiar_de_usuario(&g_orden, p, sizeof(g_orden));
    g_orden.linea[TAEK_ORDEN_LINEA - 1] = 0;
    int clase = terminal_orden_clase(g_orden.linea);
    if (clase == 2) return -TAEK_EPANTALLA;
    if (clase != 1) return -TAEK_ENOPERMITIDA;
    uint32_t cap = g_orden.capacidad > sizeof(g_orden_texto) ? (uint32_t)sizeof(g_orden_texto) : g_orden.capacidad;
    if (cap && !rango_valido(g_orden.destino, cap, 1)) return -EFAULT;
    uint64_t t0 = tiempo_obtener_milisegundos();
    serial_imprimir("[UI] orden: "); serial_imprimir_linea(g_orden.linea);
    consola_captura_iniciar(g_orden_texto, cap);
    en_pila_web(orden_en_pila);
    int truncado = 0;
    uint32_t n = consola_captura_terminar(&truncado);
    if (n) copiar_a_usuario(g_orden.destino, g_orden_texto, n);
    g_orden.bytes = n; g_orden.truncado = (uint32_t)truncado;
    g_orden.ms = (uint32_t)(tiempo_obtener_milisegundos() - t0);
    copiar_a_usuario(p, &g_orden, sizeof(g_orden));
    return (int64_t)n;
}

/* ------------------------------------------------------------------ H145: archivos y lanzar programas */

static struct taek_archivos g_ar;
static int64_t g_ar_r;
static struct taek_entrada *g_ar_entradas;   /* Bufer del nucleo para LISTAR, de g_ar.capacidad elementos */
static uint64_t g_ar_vistas;

static int ar_entrada(void *ctx, const char *nombre, uint64_t tam, int carpeta) {
    (void)ctx;
    if (g_ar_vistas >= g_ar.desde && g_ar_vistas - g_ar.desde < g_ar.capacidad) {
        struct taek_entrada *e = &g_ar_entradas[g_ar_vistas - g_ar.desde];
        memset(e, 0, sizeof(*e));
        copiar_txt(e->nombre, sizeof(e->nombre), nombre);
        e->tam = carpeta ? 0 : tam;
        e->carpeta = carpeta ? 1 : 0;
        g_ar.n++;
    }
    g_ar_vistas++;
    return 0;
}

#define AR_LISTAR_MAX 512u
#define AR_LEER_MAX (16u * 1024u * 1024u)
#define AR_ESCRIBIR_MAX (64u * 1024u * 1024u)
static uint8_t g_ar_bloque[65536];
static uint8_t *g_ar_escribir;   /* Copia en el nucleo de lo que el programa quiere escribir */
#define AR_FDS 4
static int g_ar_fd[AR_FDS] = { -1, -1, -1, -1 };   /* H188: descriptores VFS abiertos por el programa */
static void ar_cerrar_todos(void) {
    for (int i = 0; i < AR_FDS; i++) if (g_ar_fd[i] >= 0) { vfs_cerrar(g_ar_fd[i]); g_ar_fd[i] = -1; }
}

/* H147: lo que hace arrancar la laptop (Ventoy) no lo toca ninguna aplicacion: /ventoy y cualquier .iso. La raiz
 * tampoco se puede crear, mover ni borrar. Se compara sin mayusculas y tolerando '\' y '/' repetidas. */
static int ruta_protegida(const char *r) {
    char b[TAEK_RUTA]; size_t n = 0;
    for (size_t i = 0; r[i] && n + 1 < sizeof(b); i++) {
        char c = r[i] == '\\' ? '/' : r[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c == '/' && n && b[n - 1] == '/') continue;
        b[n++] = c;
    }
    while (n > 1 && b[n - 1] == '/') n--;
    b[n] = 0;
    if (n <= 1) return 1;
    if (!memcmp(b, "/ventoy", 7) && (b[7] == 0 || b[7] == '/')) return 1;
    if (n >= 4 && !memcmp(b + n - 4, ".iso", 4)) return 1;
    for (size_t i = 0; i + 2 < n; i++) if (b[i] == '/' && b[i + 1] == '.' && b[i + 2] == '.') return 1;   /* Nada de subir con .. */
    return 0;
}

static void archivos_en_pila(void) {
    g_ar_r = 0;
    g_ar.n = 0; g_ar.total = 0;
    /* H148: el resto de las operaciones pueden pedir un disco (N + 1). Si no es el abierto, se monta: asi una
     * aplicacion recorre todos los discos sin pasar por la terminal. Montar lee la tabla de particiones y el volumen. */
    if (g_ar.op >= TAEK_AR_LISTAR && g_ar.disco) {
        uint32_t d = g_ar.disco - 1;
        if (d >= USB_MSC_MAX_DISPOSITIVOS) { g_ar_r = -TAEK_EINVAL; goto fin; }
        if (!vfs_esta_montado() || vfs_obtener_unidad_activa() != d) {
            /* Tres intentos: el primer acceso a un USB desde el arranque puede salir rechazado (CSW 1) */
            for (int intento = 0; intento < 3; intento++) {
                g_ar_r = vfs_montar_particion((uint8_t)d, 0);
                if (!g_ar_r) break;
                esperar_milisegundos(150);
            }
            if (g_ar_r > 0) g_ar_r = -g_ar_r;
            if (g_ar_r) goto fin;
        }
    }
    switch (g_ar.op) {
    case TAEK_AR_DISCOS: {
        struct taek_disco_usb d[USB_MSC_MAX_DISPOSITIVOS];
        uint64_t n = 0;
        for (uint8_t i = 0; i < USB_MSC_MAX_DISPOSITIVOS && n < g_ar.capacidad; i++) {
            const struct usb_msc_dispositivo *u = usb_msc_obtener_dispositivo(i);
            memset(&d[n], 0, sizeof(d[n]));
            if (!u || !u->activo) continue;
            char nom[40]; size_t k = 0;
            for (const char *s = u->fabricante; *s && k < 9; s++) nom[k++] = *s;
            while (k && nom[k - 1] == ' ') k--;
            if (k) nom[k++] = ' ';
            for (const char *s = u->producto; *s && k < 38; s++) nom[k++] = *s;
            while (k && nom[k - 1] == ' ') k--;
            nom[k] = 0;
            copiar_txt(d[n].nombre, sizeof(d[n].nombre), nom);
            d[n].bytes = u->capacidad_bytes;
            d[n].numero = i;
            d[n].listo = (uint32_t)u->listo;
            d[n].montado = vfs_esta_montado() && vfs_obtener_unidad_activa() == i;
            n++;
        }
        g_ar.n = n;
        if (n) copiar_a_usuario(g_ar.destino, d, (size_t)n * sizeof(d[0]));
        break;
    }
    case TAEK_AR_MONTAR:
        g_ar_r = g_ar.disco >= USB_MSC_MAX_DISPOSITIVOS ? -TAEK_EINVAL : vfs_montar_particion((uint8_t)g_ar.disco, 0);
        if (g_ar_r > 0) g_ar_r = -g_ar_r;
        break;
    case TAEK_AR_LISTAR:
        g_ar_vistas = 0;
        g_ar_entradas = asignar_memoria(g_ar.capacidad * sizeof(struct taek_entrada));
        if (!g_ar_entradas) { g_ar_r = -TAEK_ENOMEM; break; }
        g_ar_r = vfs_recorrer_directorio(g_ar.ruta, ar_entrada, NULL);
        if (g_ar_r > 0) g_ar_r = -g_ar_r;
        g_ar.total = g_ar_vistas;
        if (!g_ar_r && g_ar.n) copiar_a_usuario(g_ar.destino, g_ar_entradas, (size_t)g_ar.n * sizeof(struct taek_entrada));
        liberar_memoria(g_ar_entradas);
        g_ar_entradas = NULL;
        break;
    case TAEK_AR_LEER: {
        int fd;
        int r = vfs_abrir(g_ar.ruta, &fd);
        if (r) { g_ar_r = r > 0 ? -r : r; if (!g_ar_r) g_ar_r = -TAEK_ENODEV; break; }
        g_ar.total = vfs_tamano_fd(fd);
        if (g_ar.desde < g_ar.total && vfs_buscar(fd, (int64_t)g_ar.desde, VFS_SEEK_SET) >= 0) {
            uint64_t falta = g_ar.capacidad, hecho = 0;
            if (falta > g_ar.total - g_ar.desde) falta = g_ar.total - g_ar.desde;
            while (hecho < falta) {
                size_t k = falta - hecho > sizeof(g_ar_bloque) ? sizeof(g_ar_bloque) : (size_t)(falta - hecho);
                int64_t l = vfs_leer(fd, g_ar_bloque, k);
                if (l <= 0) { if (l < 0) g_ar_r = l; break; }
                copiar_a_usuario(g_ar.destino + hecho, g_ar_bloque, (size_t)l);
                hecho += (uint64_t)l;
            }
            g_ar.n = hecho;
        }
        vfs_cerrar(fd);
        break;
    }
    case TAEK_AR_ABRIR_FD: {
        int h = -1;
        for (int i = 0; i < AR_FDS; i++) if (g_ar_fd[i] < 0) { h = i; break; }
        if (h < 0) { g_ar_r = -TAEK_ENOMEM; break; }
        int fd, r = vfs_abrir(g_ar.ruta, &fd);
        if (r) { g_ar_r = r > 0 ? -r : r; if (!g_ar_r) g_ar_r = -TAEK_ENODEV; break; }
        g_ar_fd[h] = fd; g_ar.manejador = (uint32_t)h; g_ar.total = vfs_tamano_fd(fd);
        break;
    }
    case TAEK_AR_LEER_FD: {
        uint32_t h = g_ar.manejador;
        if (h >= AR_FDS || g_ar_fd[h] < 0) { g_ar_r = -TAEK_EBADF; break; }
        int fd = g_ar_fd[h];
        g_ar.total = vfs_tamano_fd(fd);
        uint64_t hecho = 0;
        while (hecho < g_ar.capacidad) {
            size_t k = g_ar.capacidad - hecho > sizeof(g_ar_bloque) ? sizeof(g_ar_bloque) : (size_t)(g_ar.capacidad - hecho);
            int64_t l = vfs_leer(fd, g_ar_bloque, k);
            if (l <= 0) { if (l < 0) g_ar_r = l; break; }
            copiar_a_usuario(g_ar.destino + hecho, g_ar_bloque, (size_t)l);
            hecho += (uint64_t)l;
        }
        g_ar.n = hecho;
        break;
    }
    case TAEK_AR_CERRAR_FD: {
        uint32_t h = g_ar.manejador;
        if (h >= AR_FDS || g_ar_fd[h] < 0) { g_ar_r = -TAEK_EBADF; break; }
        vfs_cerrar(g_ar_fd[h]); g_ar_fd[h] = -1;
        break;
    }
    case TAEK_AR_CREAR_CARPETA:
        g_ar_r = ruta_protegida(g_ar.ruta) ? -TAEK_EPROTEGIDO : vfs_crear_directorio(g_ar.ruta);
        break;
    case TAEK_AR_ESCRIBIR:
        g_ar_r = ruta_protegida(g_ar.ruta) ? -TAEK_EPROTEGIDO : vfs_escribir_archivo(g_ar.ruta, g_ar_escribir, g_ar.capacidad);
        if (!g_ar_r) g_ar.n = g_ar.capacidad;
        break;
    case TAEK_AR_MOVER:
        g_ar_r = ruta_protegida(g_ar.ruta) || ruta_protegida(g_ar.ruta2) ? -TAEK_EPROTEGIDO : vfs_mover_archivo(g_ar.ruta, g_ar.ruta2);
        break;
    case TAEK_AR_BORRAR:
        g_ar_r = ruta_protegida(g_ar.ruta) ? -TAEK_EPROTEGIDO : vfs_eliminar_archivo(g_ar.ruta);
        break;
    default:
        g_ar_r = -TAEK_EINVAL;
    }
    if (g_ar_r > 0) g_ar_r = -g_ar_r;
    if (g_ar.op >= TAEK_AR_CREAR_CARPETA && g_ar.op <= TAEK_AR_BORRAR)   /* Para quien prueba por LAN: que escribio cada aplicacion y como salio */
    {
        static const char *const que[] = { "crear carpeta", "escribir", "mover", "borrar" };
        consola_imprimir("[ARCHIVOS] "); consola_imprimir(que[g_ar.op - TAEK_AR_CREAR_CARPETA]); consola_imprimir(" ");
        consola_imprimir(g_ar.ruta);
        if (g_ar.op == TAEK_AR_MOVER) { consola_imprimir(" -> "); consola_imprimir(g_ar.ruta2); }
        consola_imprimir(g_ar_r ? " FALLO " : " OK");
        if (g_ar_r) consola_imprimir_dec((uint64_t)-g_ar_r);
        consola_imprimir_linea("");
    }
fin:
    g_ar.disco_abierto = vfs_esta_montado() ? (int32_t)vfs_obtener_unidad_activa() : -1;
    copiar_txt(g_ar.fs, sizeof(g_ar.fs), vfs_esta_montado() ? vfs_obtener_nombre_fs() : "");
}

static int64_t sys_archivos(uint64_t p) {
    if (!rango_valido(p, sizeof(g_ar), 1)) return -EFAULT;
    copiar_de_usuario(&g_ar, p, sizeof(g_ar));
    g_ar.ruta[sizeof(g_ar.ruta) - 1] = 0;
    g_ar.ruta2[sizeof(g_ar.ruta2) - 1] = 0;
    uint64_t tam = g_ar.op == TAEK_AR_DISCOS ? sizeof(struct taek_disco_usb) : g_ar.op == TAEK_AR_LISTAR ? sizeof(struct taek_entrada) : 1;
    if (g_ar.op == TAEK_AR_DISCOS && g_ar.capacidad > USB_MSC_MAX_DISPOSITIVOS) g_ar.capacidad = USB_MSC_MAX_DISPOSITIVOS;
    if (g_ar.op == TAEK_AR_LISTAR && g_ar.capacidad > AR_LISTAR_MAX) g_ar.capacidad = AR_LISTAR_MAX;
    if ((g_ar.op == TAEK_AR_LEER || g_ar.op == TAEK_AR_LEER_FD) && g_ar.capacidad > AR_LEER_MAX) g_ar.capacidad = AR_LEER_MAX;
    int sin_bufer = g_ar.op == TAEK_AR_MONTAR || g_ar.op == TAEK_AR_CREAR_CARPETA || g_ar.op == TAEK_AR_MOVER || g_ar.op == TAEK_AR_BORRAR ||
                    g_ar.op == TAEK_AR_ABRIR_FD || g_ar.op == TAEK_AR_CERRAR_FD;
    g_ar_escribir = NULL;
    if (g_ar.op == TAEK_AR_ESCRIBIR) {
        /* Los datos pasan al nucleo antes de tocar el disco: el programa no puede cambiarlos a mitad de la escritura */
        if (g_ar.capacidad > AR_ESCRIBIR_MAX) return -TAEK_ENOMEM;
        if (g_ar.capacidad && !rango_valido(g_ar.destino, g_ar.capacidad, 0)) return -EFAULT;
        if (g_ar.capacidad) {
            g_ar_escribir = asignar_memoria(g_ar.capacidad);
            if (!g_ar_escribir) return -TAEK_ENOMEM;
            copiar_de_usuario(g_ar_escribir, g_ar.destino, g_ar.capacidad);
        }
    } else if (!sin_bufer && (!g_ar.capacidad || !rango_valido(g_ar.destino, g_ar.capacidad * tam, 1))) return -EFAULT;
    en_pila_web(archivos_en_pila);
    if (g_ar_escribir) { liberar_memoria(g_ar_escribir); g_ar_escribir = NULL; }
    copiar_a_usuario(p, &g_ar, sizeof(g_ar));
    return g_ar_r;
}

/* Lo que pidio el programa para despues de salir, y el argumento con el que corre el actual */
static struct taek_lanzar g_lanzar;
static int g_lanzar_pedido, g_orden_pedida;
static char g_argumento[TAEK_RUTA];

static int64_t sys_lanzar(uint64_t p) {
    if (!rango_valido(p, sizeof(g_lanzar), 0)) return -EFAULT;
    copiar_de_usuario(&g_lanzar, p, sizeof(g_lanzar));
    g_lanzar.programa[sizeof(g_lanzar.programa) - 1] = 0;
    g_lanzar.argumento[sizeof(g_lanzar.argumento) - 1] = 0;
    g_lanzar.argumento_al_volver[sizeof(g_lanzar.argumento_al_volver) - 1] = 0;
    /* H151: una orden de pantalla completa de la lista de la interfaz. La recoge el bucle del menu (TAEK_ACCION_ORDEN). */
    if (!memcmp(g_lanzar.programa, TAEK_PROGRAMA_ORDEN, sizeof(TAEK_PROGRAMA_ORDEN))) {
        g_lanzar_pedido = 0;
        if (!terminal_orden_clase(g_lanzar.argumento)) return -TAEK_ENOPERMITIDA;
        g_orden_pedida = 1;
        return 0;
    }
    if (g_lanzar.programa[0] != '/' && memcmp(g_lanzar.programa, TAEK_PROGRAMA_ABRIR, sizeof(TAEK_PROGRAMA_ABRIR))) {
        g_lanzar_pedido = 0;
        return -TAEK_EINVAL;
    }
    g_lanzar_pedido = 1;
    return 0;
}

/* H151: la orden que pidio el menu antes de terminar con TAEK_ACCION_ORDEN. 0 si habia una (y se consume). */
int anillo3_orden_pendiente(char *b, unsigned cap) {
    if (!g_orden_pedida || !cap) return -1;
    g_orden_pedida = 0;
    copiar_txt(b, cap, g_lanzar.argumento);
    return terminal_orden_clase(b) ? 0 : -1;
}

static int64_t sys_argumento(uint64_t p, uint64_t cap) {
    size_t n = 0;
    while (g_argumento[n]) n++;
    if (!cap) return (int64_t)n;
    if (!rango_valido(p, cap, 1)) return -EFAULT;
    size_t k = n + 1 < cap ? n + 1 : (size_t)cap;
    char b[TAEK_RUTA];
    memcpy(b, g_argumento, k);
    b[k - 1] = 0;
    copiar_a_usuario(p, b, k);
    return (int64_t)n;
}

/* H146: sonido desde Anillo 3. El nucleo solo mueve PCM ya decodificado a la cola del controlador. */
static int64_t sys_audio(uint64_t p) {
    struct taek_audio a;
    if (!rango_valido(p, sizeof(a), 1)) return -EFAULT;
    copiar_de_usuario(&a, p, sizeof(a));
    int64_t r = 0;
    if (!audio_esta_iniciado()) {
        r = a.op == TAEK_AU_ESTADO ? 0 : -TAEK_ENODEV;
    } else switch (a.op) {
    case TAEK_AU_ESTADO: break;
    case TAEK_AU_ENCOLAR: {
        a.aceptados = 0;
        if (a.bytes & 3) { r = -TAEK_EINVAL; break; }
        if (!a.bytes) break;
        if (a.bytes > 256u * 1024u) a.bytes = 256u * 1024u;
        if (!rango_valido(a.datos, a.bytes, 0)) { r = -EFAULT; break; }
        uint32_t libre = audio_ac97_cola_disponible() & ~3u, n = a.bytes < libre ? a.bytes : libre;
        static uint8_t bloque[16384];
        while (a.aceptados < n) {
            uint32_t k = n - a.aceptados > sizeof(bloque) ? (uint32_t)sizeof(bloque) : n - a.aceptados;
            copiar_de_usuario(bloque, a.datos + a.aceptados, k);
            int ac = audio_ac97_encolar_pcm(bloque, k);
            if (ac <= 0) break;
            a.aceptados += (uint32_t)ac;
            if ((uint32_t)ac < k) break;
        }
        g_audio_usado = 1;
        break;
    }
    case TAEK_AU_INICIAR:
        g_audio_usado = 1;
        if (!audio_ac97_esta_reproduciendo()) { audio_ac97_reiniciar_reloj(); r = audio_ac97_iniciar_stream(); }
        break;
    case TAEK_AU_DETENER: audio_ac97_detener(); break;
    case TAEK_AU_VOLUMEN: r = audio_fijar_volumen_db(a.volumen_db); break;
    default: r = -TAEK_EINVAL;
    }
    if (audio_esta_iniciado()) {
        audio_ac97_actualizar();
        a.ocupada = audio_ac97_cola_ocupada();
        a.libre = audio_ac97_cola_disponible();
        a.sonando = (uint32_t)audio_ac97_esta_reproduciendo();
        a.tocado_ms = audio_ac97_obtener_tiempo_ms();
        a.volumen_db = audio_obtener_volumen_db();
        a.hay_audio = 1;
    } else a.hay_audio = 0;
    copiar_a_usuario(p, &a, sizeof(a));
    return r;
}

/* H144: el programa pide o devuelve memoria. Crecer: paginas nuevas, a cero, escribibles y no ejecutables, siempre que
 * queden MONTON_RESERVA_PAGINAS libres para el nucleo. Si a mitad falta un marco, se deshace lo agregado: o todo o nada.
 * Encoger: solo paginas enteras del final; un acceso posterior a ellas es un fallo de pagina del programa. */
static int64_t sys_memoria(uint64_t arg) {
    int64_t delta = (int64_t)arg;
    uint64_t antes = g_monton_fin;
    if (g_monton_region < 0) return -TAEK_EINVAL;
    if (delta == 0) return (int64_t)antes;
    if (delta < 0) {
        if (delta == INT64_MIN) return -TAEK_EINVAL;
        uint64_t paginas = (uint64_t)(-delta) / TAMANO_PAGINA;
        if (paginas > (antes - TAEK_MONTON) / TAMANO_PAGINA) return -TAEK_EINVAL;
        monton_soltar(antes - paginas * TAMANO_PAGINA);
        return (int64_t)antes;
    }
    uint64_t paginas = ((uint64_t)delta + TAMANO_PAGINA - 1) / TAMANO_PAGINA;
    if (paginas > (TAEK_MONTON_MAX - (antes - TAEK_MONTON)) / TAMANO_PAGINA) return -TAEK_ENOMEM;
    uint64_t libres = pmm_paginas_libres();
    if (libres < MONTON_RESERVA_PAGINAS || paginas > libres - MONTON_RESERVA_PAGINAS) return -TAEK_ENOMEM;
    uint64_t hhdm = memoria_obtener_hhdm_offset();
    for (uint64_t i = 0; i < paginas; i++) {
        uint64_t f = pmm_asignar_pagina_fisica();
        if (!f) { monton_soltar(antes); return -TAEK_ENOMEM; }
        memset((void *)(f + hhdm), 0, TAMANO_PAGINA);
        if (paginacion_mapear(g_monton_fin, f, PAGINA_PRESENTE | PAGINA_USUARIO | PAGINA_ESCRITURA | PAGINA_NO_EJECUTABLE)) {
            pmm_liberar_pagina_fisica(f);
            monton_soltar(antes);
            return -TAEK_ENOMEM;
        }
        g_monton_fin += TAMANO_PAGINA;
    }
    g_regiones[g_monton_region].fin = g_monton_fin;
    if (g_monton_fin - TAEK_MONTON > g_monton_pico) g_monton_pico = g_monton_fin - TAEK_MONTON;
    return (int64_t)antes;
}

static int64_t sys_esperar(uint64_t ms) {
    if (ms > 100) ms = 100;
    uint64_t t0 = leer_tsc();
    uint64_t te = atencion_espera_inicio();   /* H215, E1 */
    uint64_t fin = tiempo_obtener_milisegundos() + ms;
    do {
        audio_ac97_actualizar();
        entrada_atender();
        __asm__ volatile("pause");
    } while (tiempo_obtener_milisegundos() < fin);
    atencion_espera_fin(te, 1);
    g_espera_ciclos += leer_tsc() - t0;
    return 0;
}

static int64_t sys_pantalla(uint64_t p) {
    struct taek_pantalla t = { (uint32_t)pantalla_obtener_ancho(), (uint32_t)pantalla_obtener_alto() };
    if (!rango_valido(p, sizeof(t), 1)) return -EFAULT;
    copiar_a_usuario(p, &t, sizeof(t));
    return 0;
}

/* ------------------------------------------------------------------ H213, Fase D: hilos del programa
 *
 * El principal sigue en CPU 0 (es el unico que toca pantalla, archivos, red, audio y memoria). Cada hilo secundario
 * es un hilo_t de prioridad FONDO en una CPU auxiliar que baja a Anillo 3 con anillo3_entrar, igual que el principal:
 *  - su pila de nucleo es la del hilo_t y la raiz la carga en el RSP0 del TSS de su CPU al darle el turno;
 *  - entra al nucleo por la misma puerta 0x80 y por las interrupciones; anillo3_al_entrar le devuelve el FS del
 *    nucleo y anillo3_al_salir le pone el suyo (TLS) con las interrupciones ya apagadas hasta el iretq;
 *  - termina volviendo con anillo3_volver a su rsp_guardado: por TAEK_HILO_SALIR, por un fallo o porque el programa
 *    acabo (matar, que se mira en cada salida a Anillo 3: tictac de 1 ms o IPI);
 *  - lee la memoria del programa por la direccion fisica (paginacion_obtener_fisica + HHDM), nunca por la virtual:
 *    si el principal la devuelve a la vez, el nucleo lee un marco viejo en vez de caer con un fallo de pagina. */

struct hilo_programa {
    hilo_t *hilo;
    uint64_t entrada, arg, pila, tls;
    uint64_t rsp_guardado;
    volatile int estado;              /* HP_LIBRE, HP_CORRIENDO, HP_TERMINADO */
    volatile int matar;
    int fin;                          /* TAEK_HILO_FIN_* */
    int64_t valor;
    uint64_t vector, rip, cr2;
    unsigned cpu;
    volatile uint64_t futex_dir;
    volatile int futex_despertado;
    evento_t ev;                      /* Nunca se reescribe: su cerrojo queda en la lista global de cerrojos */
};
enum { HP_LIBRE = 0, HP_CORRIENDO = 1, HP_TERMINADO = 2 };
static struct hilo_programa g_hp[TAEK_HILOS_MAX + 1] = {
    [0 ... TAEK_HILOS_MAX] = { .ev = EVENTO("hilo_programa") }
};
static cerrojo_t g_cerrojo_futex = CERROJO("futex");
static unsigned g_hp_creados, g_hp_turno_cpu;
static uint64_t g_hp_llamadas, g_hp_rechazadas, g_futex_esperas, g_futex_despertados;
static volatile int g_hp_fallo_avisado;
static int g_cpu_lista_anillo3[4];
static uint32_t g_hp_zombis;          /* Hilos que no se pudieron detener (nunca deberia pasar) */
static void hp_cuerpo(void *a);

static struct hilo_programa *hp_actual(void) {
    if (!cpu_actual()) return &g_hp[0];
    hilo_t *h = hilo_actual();
    if (!h || h->fn != hp_cuerpo) return 0;
    return (struct hilo_programa *)h->arg;
}

/* Copia memoria del programa por su direccion fisica. -1 si alguna pagina ya no esta. */
static int leer_usuario_fisico(void *d, uint64_t s, size_t n) {
    uint64_t hhdm = memoria_obtener_hhdm_offset();
    uint8_t *o = d;
    while (n) {
        uint64_t f = paginacion_obtener_fisica(s);
        if (!f) return -1;
        size_t k = TAMANO_PAGINA - (size_t)(s & (TAMANO_PAGINA - 1));
        if (k > n) k = n;
        memcpy(o, (const void *)(uintptr_t)(f + hhdm), k);
        o += k; s += k; n -= k;
    }
    return 0;
}

/* Lo que preparar() hace en CPU 0, en una auxiliar: sin syscall/sysenter, sin wrfsbase/wrgsbase y con SMEP. SMAP no:
 * los lotes de video de estas CPU nunca lo tuvieron y no se sabe si tocan superficies del programa sin stac. */
static void hp_preparar_cpu(unsigned cpu) {
    if (cpu >= 4 || g_cpu_lista_anillo3[cpu]) return;
    escribir_msr(MSR_EFER, leer_msr(MSR_EFER) & ~EFER_SCE);
    escribir_msr(MSR_SYSENTER_CS, 0);
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 &= ~CR4_FSGSBASE;
    if (g_smep) cr4 |= CR4_SMEP;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
    g_cpu_lista_anillo3[cpu] = 1;
}

static void hp_cuerpo(void *a) {
    struct hilo_programa *hp = a;
    unsigned cpu = cpu_actual();
    hilo_t *h = hilo_actual();
    uint8_t fp_nucleo[512] __attribute__((aligned(16)));
    hp->cpu = cpu;
    __asm__ volatile("cli" ::: "memory");
    hp_preparar_cpu(cpu);
    /* Las entradas desde Anillo 3 apilan su marco desde aqui hacia abajo, lejos de lo que anillo3_entrar guarda */
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    h->rsp0 = (rsp - 512) & ~UINT64_C(15);
    gdt_fijar_rsp0(cpu, h->rsp0);
    if (!hp->matar) {
        __asm__ volatile("fxsave64 %0" : "=m"(fp_nucleo));
        __asm__ volatile("fxrstor64 %0" :: "m"(g_fp_limpio));   /* Ningun estado SSE del nucleo llega al programa */
        escribir_msr(MSR_FS_BASE, hp->tls);                      /* Con IF=0 hasta el iretq */
        anillo3_entrar(&hp->rsp_guardado, hp->entrada, hp->pila - 8, hp->arg, (uint64_t)(hp - g_hp));
        __asm__ volatile("fxrstor64 %0" :: "m"(fp_nucleo));
    } else {
        hp->fin = TAEK_HILO_FIN_DETENIDO;
    }
    h->rsp0 = 0;
    gdt_fijar_rsp0(cpu, 0);
    __atomic_store_n(&hp->estado, HP_TERMINADO, __ATOMIC_RELEASE);
    __asm__ volatile("sti" ::: "memory");
}

/* Desde una entrada al nucleo de un hilo secundario, sin cerrojos en la mano: vuelve a hp_cuerpo y no regresa */
static __attribute__((noreturn)) void hp_terminar(struct hilo_programa *hp, int fin, int64_t valor) {
    hp->fin = fin;
    hp->valor = valor;
    hilo_t *h = hilo_actual();
    anillo3_volver(hp->rsp_guardado, 0, h ? (uint64_t)(uintptr_t)h->tls : 0);
}

static void hp_empezar_programa(void) {
    g_tls_principal = 0;
    g_hp_creados = 0;
    g_hp_llamadas = g_hp_rechazadas = g_futex_esperas = g_futex_despertados = 0;
    g_hp_fallo_avisado = 0;
    g_servicios_cpu0 = g_hueco_cpu0_max = 0;
    g_puntero_ms = tiempo_obtener_milisegundos();
    g_hp[0].futex_dir = 0;
    g_hp[0].futex_despertado = 0;
}

static unsigned hp_vivos(void) {
    unsigned n = 0;
    for (unsigned i = 1; i <= TAEK_HILOS_MAX; i++) if (__atomic_load_n(&g_hp[i].estado, __ATOMIC_ACQUIRE) == HP_CORRIENDO) n++;
    return n;
}

/* CPU 0, con el principal ya fuera de Anillo 3: nadie vuelve a Anillo 3 despues de esto. */
static void hp_detener_todos(void) {
    unsigned pedidos = 0;
    for (unsigned i = 1; i <= TAEK_HILOS_MAX; i++) {
        struct hilo_programa *hp = &g_hp[i];
        if (__atomic_load_n(&hp->estado, __ATOMIC_ACQUIRE) != HP_CORRIENDO || hp->matar) continue;
        __atomic_store_n(&hp->matar, 1, __ATOMIC_RELEASE);
        evento_senalar(&hp->ev);          /* Si esperaba un futex sin limite */
        trabajos_ipi_cpu(hp->cpu);        /* Si estaba en Anillo 3: la IPI lo hace entrar y anillo3_al_salir lo termina */
        pedidos++;
    }
    uint64_t t0 = tiempo_obtener_milisegundos();
    while (hp_vivos() > g_hp_zombis && tiempo_obtener_milisegundos() - t0 < 2000) __asm__ volatile("pause");
    unsigned quedan = hp_vivos();
    if (quedan > g_hp_zombis) {
        /* No puede volver a Anillo 3 (matar sigue en 1) y lee la memoria por la fisica: soltarla es seguro. El lugar
         * queda ocupado para siempre, para que nadie le reescriba matar. */
        consola_imprimir("[HILOS] "); consola_imprimir_dec(quedan - g_hp_zombis);
        consola_imprimir_linea(" hilo(s) del programa no se detuvieron en 2 s; su lugar queda reservado.");
        g_hp_zombis = quedan;
    }
    for (unsigned i = 1; i <= TAEK_HILOS_MAX; i++) {
        struct hilo_programa *hp = &g_hp[i];
        if (__atomic_load_n(&hp->estado, __ATOMIC_ACQUIRE) == HP_TERMINADO) {
            hp->matar = 0;
            __atomic_store_n(&hp->estado, HP_LIBRE, __ATOMIC_RELEASE);
        }
    }
    if (pedidos && g_res) {
        consola_imprimir("[HILOS] El programa termino con "); consola_imprimir_dec(pedidos);
        consola_imprimir_linea(" hilo(s) suyos todavia en marcha: detenidos.");
    }
    g_tls_principal = 0;
}

static int64_t hp_crear(uint64_t p) {
    struct taek_hilo t;
    if (!rango_valido(p, sizeof(t), 1)) return -EFAULT;
    copiar_de_usuario(&t, p, sizeof(t));
    if (t.entrada < ANILLO3_BASE || t.entrada >= ANILLO3_FIN) return -TAEK_EINVAL;
    if ((t.pila & 15) || t.pila < 256 || !rango_valido(t.pila - 256, 256, 1)) return -EFAULT;
    unsigned n = trabajos_cpu_arrancadas();
    if (n > 4) n = 4;
    if (n < 2) return -TAEK_ENODEV;
    unsigned cpu = t.cpu;
    if (cpu >= n) return -TAEK_EINVAL;
    if (!cpu) {
        /* H215, CONGELADO: con 'thread director encender' (solo procesadores con HFI), la CPU con mas rendimiento
         * por hilo de programa ya puesto en ella. Si no, por turno como en H213. */
        uint8_t perf[TD_CPUS] = { 0 };
        unsigned hilos[TD_CPUS] = { 0 };
        for (unsigned i = 1; i <= TAEK_HILOS_MAX; i++)
            if (__atomic_load_n(&g_hp[i].estado, __ATOMIC_ACQUIRE) == HP_CORRIENDO && g_hp[i].cpu < TD_CPUS) hilos[g_hp[i].cpu]++;
        if (thread_director_rendimiento(perf, n)) cpu = thread_director_elegir(perf, hilos, n);
        if (!cpu) cpu = (n - 1) - (g_hp_turno_cpu++ % (n - 1));   /* 3, 2, 1, 3...: la 3 primero (solo la red) */
    }
    unsigned id = 0;
    for (unsigned i = 1; i <= TAEK_HILOS_MAX && !id; i++) {
        int e = __atomic_load_n(&g_hp[i].estado, __ATOMIC_ACQUIRE);
        if (!g_hp[i].matar && (e == HP_LIBRE || e == HP_TERMINADO)) id = i;
    }
    if (!id) return -TAEK_EBUSY;
    struct hilo_programa *hp = &g_hp[id];
    hp->entrada = t.entrada; hp->arg = t.arg; hp->pila = t.pila; hp->tls = t.tls; hp->cpu = cpu;
    hp->fin = 0; hp->valor = 0; hp->vector = hp->rip = hp->cr2 = 0;
    hp->futex_dir = 0; hp->futex_despertado = 0; hp->matar = 0;
    __atomic_store_n(&hp->estado, HP_CORRIENDO, __ATOMIC_RELEASE);
    hp->hilo = hilo_crear(hp_cuerpo, hp, "programa", cpu, HILO_FONDO, 0);
    if (!hp->hilo) { __atomic_store_n(&hp->estado, HP_LIBRE, __ATOMIC_RELEASE); return -TAEK_ENOMEM; }
    g_hp_creados++;
    t.cpu = cpu;
    t.id = id;
    copiar_a_usuario(p, &t, sizeof(t));
    return id;
}

/* Solo el principal. Cada llamada espera como mucho 100 ms (CPU 0 espera con las interrupciones apagadas, como
 * SYS_ESPERAR, y sigue moviendo la flecha); con limite_ms 0 la biblioteca vuelve a llamar. */
static int64_t hp_unir(uint64_t p) {
    struct taek_hilo t;
    if (!rango_valido(p, sizeof(t), 1)) return -EFAULT;
    copiar_de_usuario(&t, p, sizeof(t));
    if (!t.id || t.id > TAEK_HILOS_MAX) return -TAEK_EINVAL;
    struct hilo_programa *hp = &g_hp[t.id];
    if (__atomic_load_n(&hp->estado, __ATOMIC_ACQUIRE) == HP_LIBRE) return -TAEK_EINVAL;
    uint64_t lim = t.limite_ms && t.limite_ms < 100 ? t.limite_ms : 100;
    uint64_t t0 = tiempo_obtener_milisegundos();
    uint64_t te = atencion_espera_inicio();   /* H215, E1 */
    while (__atomic_load_n(&hp->estado, __ATOMIC_ACQUIRE) != HP_TERMINADO && !g_matar &&
           tiempo_obtener_milisegundos() - t0 < lim) {
        entrada_atender();
        __asm__ volatile("pause");
    }
    atencion_espera_fin(te, 1);
    if (__atomic_load_n(&hp->estado, __ATOMIC_ACQUIRE) != HP_TERMINADO) return -TAEK_ETIEMPO;
    t.fin = hp->fin; t.valor = hp->valor; t.vector = hp->vector; t.rip = hp->rip; t.cr2 = hp->cr2; t.cpu = hp->cpu;
    if (!hp->matar) __atomic_store_n(&hp->estado, HP_LIBRE, __ATOMIC_RELEASE);
    copiar_a_usuario(p, &t, sizeof(t));
    return 0;
}

static int64_t sys_hilo(uint64_t op, uint64_t arg, struct hilo_programa *yo) {
    unsigned id = (unsigned)(yo - g_hp);
    switch (op) {
    case TAEK_HILO_YO: return id;
    case TAEK_HILO_CPU: return cpu_actual();
    case TAEK_HILO_TLS:
        if (id) yo->tls = arg; else g_tls_principal = arg;   /* Vale al volver a Anillo 3 (anillo3_al_salir) */
        return 0;
    case TAEK_HILO_SALIR:
        if (!id) return -TAEK_EINVAL;
        hp_terminar(yo, TAEK_HILO_FIN_SALIO, (int64_t)arg);
    case TAEK_HILO_CREAR: return id ? -TAEK_ENOPERMITIDA : hp_crear(arg);
    case TAEK_HILO_UNIR: return id ? -TAEK_ENOPERMITIDA : hp_unir(arg);
    case TAEK_HILO_INFO: {
        if (id) return -TAEK_ENOPERMITIDA;
        struct taek_hilos_info i;
        if (!rango_valido(arg, sizeof(i), 1)) return -EFAULT;
        memset(&i, 0, sizeof(i));
        i.vivos = hp_vivos() - g_hp_zombis;
        i.creados = g_hp_creados;
        i.servicios_cpu0 = g_servicios_cpu0;
        i.hueco_cpu0_max_ms = g_hueco_cpu0_max;
        i.llamadas_hilos = g_hp_llamadas;
        i.rechazadas = g_hp_rechazadas;
        i.futex_esperas = g_futex_esperas;
        i.futex_despertados = g_futex_despertados;
        g_hueco_cpu0_max = 0;          /* Leer pone a cero el hueco, para medir un tramo */
        g_puntero_ms = tiempo_obtener_milisegundos();
        copiar_a_usuario(arg, &i, sizeof(i));
        return 0;
    }
    default: return -TAEK_EINVAL;
    }
}

static int64_t sys_futex(uint64_t op, uint64_t dir, uint64_t valor, uint64_t ms, struct hilo_programa *yo) {
    if ((dir & 3) || !rango_valido(dir, 4, 1)) return -EFAULT;
    if (op == TAEK_FUTEX_DESPERTAR) {
        unsigned n = 0, quien = 0;
        uint64_t f = cerrojo_tomar_irq(&g_cerrojo_futex);
        for (unsigned i = 0; i <= TAEK_HILOS_MAX && n < valor; i++) {
            struct hilo_programa *hp = &g_hp[i];
            if (hp->futex_dir != dir || hp->futex_despertado) continue;
            hp->futex_despertado = 1;
            hp->futex_dir = 0;
            quien |= 1u << i;
            n++;
        }
        cerrojo_soltar_irq(&g_cerrojo_futex, f);
        for (unsigned i = 1; i <= TAEK_HILOS_MAX; i++) if (quien & (1u << i)) evento_senalar(&g_hp[i].ev);
        __atomic_fetch_add(&g_futex_despertados, n, __ATOMIC_RELAXED);
        return n;
    }
    if (op != TAEK_FUTEX_ESPERAR) return -TAEK_EINVAL;
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo_futex);
    uint32_t v;
    if (leer_usuario_fisico(&v, dir, 4)) { cerrojo_soltar_irq(&g_cerrojo_futex, f); return -EFAULT; }
    if (v != (uint32_t)valor) { cerrojo_soltar_irq(&g_cerrojo_futex, f); return -TAEK_EAGAIN; }
    yo->futex_despertado = 0;
    yo->futex_dir = dir;
    cerrojo_soltar_irq(&g_cerrojo_futex, f);
    __atomic_fetch_add(&g_futex_esperas, 1, __ATOMIC_RELAXED);
    uint64_t t0 = tiempo_obtener_milisegundos();
    if (yo == &g_hp[0]) {
        uint64_t lim = ms && ms < 100 ? ms : 100;
        uint64_t te = atencion_espera_inicio();   /* H215, E1 */
        while (!yo->futex_despertado && !g_matar && tiempo_obtener_milisegundos() - t0 < lim) {
            entrada_atender();
            __asm__ volatile("pause");
        }
        atencion_espera_fin(te, 1);
    } else if (!ms) {
        while (!yo->futex_despertado && !yo->matar) hilo_esperar(&yo->ev);
    } else {
        while (!yo->futex_despertado && !yo->matar && tiempo_obtener_milisegundos() - t0 < ms) hilo_dormir_ms(1);
    }
    f = cerrojo_tomar_irq(&g_cerrojo_futex);
    int despierto = yo->futex_despertado;
    yo->futex_dir = 0;
    cerrojo_soltar_irq(&g_cerrojo_futex, f);
    return despierto ? 0 : -TAEK_ETIEMPO;
}

static int64_t hp_escribir(uint64_t p, uint64_t n) {
    if (n > 4096 || !rango_valido(p, n, 0)) return -EFAULT;
    char b[129];
    for (uint64_t o = 0; o < n;) {
        size_t c = n - o > 128 ? 128 : (size_t)(n - o);
        if (leer_usuario_fisico(b, p + o, c)) return -EFAULT;
        for (size_t i = 0; i < c; i++) if ((unsigned char)b[i] < 32 && b[i] != '\n') b[i] = '?';
        b[c] = 0;
        consola_imprimir(b);
        o += c;
    }
    return (int64_t)n;
}

/* Llamada desde un hilo secundario, en su CPU auxiliar. Puede dormir: es un hilo_t con su propia pila. */
static void hp_llamada(struct marco_interrupcion *m, struct hilo_programa *hp) {
    __atomic_fetch_add(&g_hp_llamadas, 1, __ATOMIC_RELAXED);
    int64_t r;
    switch (m->rax) {
    case SYS_SALIR: hp_terminar(hp, TAEK_HILO_FIN_SALIO, (int64_t)m->rdi);
    case SYS_ESCRIBIR: r = hp_escribir(m->rdi, m->rsi); break;
    case SYS_TIEMPO: r = (int64_t)(tiempo_obtener_milisegundos() - g_inicio_ms); break;
    case SYS_CEDER: hilo_ceder(); r = 0; break;
    case SYS_ESPERAR: {
        uint64_t ms = m->rdi > 100 ? 100 : m->rdi;
        if (ms) hilo_dormir_ms((uint32_t)ms); else hilo_ceder();
        r = 0;
        break;
    }
    case SYS_HILO: r = sys_hilo(m->rdi, m->rsi, hp); break;
    case SYS_FUTEX: r = sys_futex(m->rdi, m->rsi, m->rdx, m->rcx, hp); break;
    default: __atomic_fetch_add(&g_hp_rechazadas, 1, __ATOMIC_RELAXED); r = -TAEK_ENOPERMITIDA; break;
    }
    m->rax = (uint64_t)r;
}

static __attribute__((noreturn)) void hp_fallo(struct marco_interrupcion *m, struct hilo_programa *hp) {
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    hp->vector = m->num_interrupcion; hp->rip = m->rip; hp->cr2 = cr2;
    /* Un fallo en cualquier hilo termina el programa: el primero queda como resultado y CPU 0 lo cierra */
    if (!__atomic_exchange_n(&g_hp_fallo_avisado, 1, __ATOMIC_ACQ_REL) && g_res) {
        g_res->vector = m->num_interrupcion; g_res->error = m->codigo_error;
        g_res->rip = m->rip; g_res->rsp = m->rsp; g_res->cr2 = cr2;
        consola_imprimir("[HILOS] Fallo en el hilo "); consola_imprimir_dec((uint64_t)(hp - g_hp));
        consola_imprimir(" (CPU "); consola_imprimir_dec(cpu_actual());
        consola_imprimir("): vector "); consola_imprimir_dec(m->num_interrupcion);
        consola_imprimir_linea("; se termina el programa.");
        g_matar = 1;
    }
    hp_terminar(hp, TAEK_HILO_FIN_FALLO, 0);
}

unsigned anillo3_hilos_vivos(void) { return hp_vivos(); }

void anillo3_al_salir(void) {
    if (!cpu_actual()) {
        if (g_matar && g_activo) anillo3_tras_irq();   /* No vuelve si de verdad hay que terminar */
        if (g_tls_principal) { __asm__ volatile("cli" ::: "memory"); escribir_msr(MSR_FS_BASE, g_tls_principal); }
        return;
    }
    struct hilo_programa *hp = hp_actual();
    if (!hp) return;
    __asm__ volatile("cli" ::: "memory");
    if (__atomic_load_n(&hp->matar, __ATOMIC_ACQUIRE)) hp_terminar(hp, TAEK_HILO_FIN_DETENIDO, 0);
    escribir_msr(MSR_FS_BASE, hp->tls);
}

void anillo3_llamada(struct marco_interrupcion *m) {
    if (cpu_actual()) {
        struct hilo_programa *hp = hp_actual();
        if (hp) hp_llamada(m, hp); else m->rax = (uint64_t)-TAEK_ENOPERMITIDA;
        return;
    }
    g_res->llamadas++;
    g_ultimo_ms = tiempo_obtener_milisegundos();
    /* H204, Fase 0: cuanto tarda cada llamada y cual estaba en curso cuando el USB o la red esperaron de mas */
    uint32_t numero = m->rax < ATENCION_SYS_MAX ? (uint32_t)m->rax : ATENCION_SYS_MAX - 1;
    uint32_t sys_previa = atencion_sys_actual;
    uint64_t t_sys = leer_tsc();
    atencion_sys_actual = numero;
    int64_t r;
    switch (m->rax) {
    case SYS_SALIR:
        atencion_sys_actual = ATENCION_SIN_SYS;
        g_res->fin = ANILLO3_SALIO;
        g_res->codigo = (int64_t)m->rdi;
        anillo3_volver(g_rsp_guardado, 0, g_fs_kernel);
    case SYS_ESCRIBIR: r = sys_escribir(m->rdi, m->rsi); break;
    case SYS_TIEMPO: r = (int64_t)(g_ultimo_ms - g_inicio_ms); break;
    case SYS_CEDER: audio_ac97_actualizar(); r = 0; break;
    case SYS_SUPERFICIE: r = sys_superficie(m->rdi); break;
    case SYS_PRESENTAR:
        r = m->rdi > 0xFFFFFFFFu ? -TAEK_EBADF
            : compositor_presentar((uint32_t)m->rdi, (int64_t)m->rsi, (int64_t)m->rdx, (int64_t)m->rcx, (int64_t)m->r8);
        break;
    case SYS_MOVER:
        if (m->rdi > 0xFFFFFFFFu) r = -TAEK_EBADF;
        else if ((int64_t)m->rsi != (int32_t)m->rsi || (int64_t)m->rdx != (int32_t)m->rdx) r = -TAEK_EINVAL;
        else r = compositor_mover((uint32_t)m->rdi, (int32_t)m->rsi, (int32_t)m->rdx);
        break;
    case SYS_EVENTO: r = sys_evento(m->rdi); break;
    case SYS_PANTALLA: r = sys_pantalla(m->rdi); break;
    case SYS_ESPERAR: r = sys_esperar(m->rdi); break;
    case SYS_INFO: r = sys_info(m->rdi, m->rsi); break;
    case SYS_CAPTURA: r = sys_captura(m->rdi, m->rsi); break;
    case SYS_EQUIPO: r = sys_equipo(m->rdi); break;
    case SYS_ELEVAR: r = m->rdi > 0xFFFFFFFFu ? -TAEK_EBADF : compositor_elevar((uint32_t)m->rdi); break;
    case SYS_WEB: r = sys_web(m->rdi); break;
    case SYS_MEMORIA: r = sys_memoria(m->rdi); break;
    case SYS_ARCHIVOS: r = sys_archivos(m->rdi); break;
    case SYS_LANZAR: r = sys_lanzar(m->rdi); break;
    case SYS_ARGUMENTO: r = sys_argumento(m->rdi, m->rsi); break;
    case SYS_AUDIO: r = sys_audio(m->rdi); break;
    case SYS_TAMANO:
        if (m->rdi > 0xFFFFFFFFu) r = -TAEK_EBADF;
        else if ((int64_t)m->rcx != (int32_t)m->rcx || (int64_t)m->r8 != (int32_t)m->r8) r = -TAEK_EINVAL;
        else r = compositor_tamano((uint32_t)m->rdi, (int32_t)m->rcx, (int32_t)m->r8, (int64_t)m->rsi, (int64_t)m->rdx);
        break;
    case SYS_SENSORES: r = sys_sensores(m->rdi); break;
    case SYS_ORDEN: r = sys_orden(m->rdi); break;
    case SYS_RED: r = sys_red(m->rdi); break;
    case SYS_VIDEO: r = sys_video(m->rdi, m->rsi); break;
    case SYS_FLUJO: r = sys_flujo(m->rdi); break;
    case SYS_HILO: r = sys_hilo(m->rdi, m->rsi, &g_hp[0]); break;                     /* H213 */
    case SYS_FUTEX: r = sys_futex(m->rdi, m->rsi, m->rdx, m->rcx, &g_hp[0]); break;   /* H213 */
    default: r = -ENOSYS; break;
    }
    /* Una llamada larga, como la primera lectura de la bateria, no es un programa colgado. */
    g_ultimo_ms = tiempo_obtener_milisegundos();
    atencion_sys_terminar(numero, leer_tsc() - t_sys);
    atencion_sys_actual = sys_previa;
    m->rax = (uint64_t)r;
}

void anillo3_fallo(struct marco_interrupcion *m) {
    if (cpu_actual()) {   /* H213: un hilo secundario; nunca saltar a la pila de CPU 0 desde otra CPU */
        struct hilo_programa *hp = hp_actual();
        if (hp) hp_fallo(m, hp);
        huevo_quebrar_con_marco("Fallo de Anillo 3 en una CPU auxiliar sin hilo de programa", m);
        for (;;) __asm__ volatile("cli; hlt");
    }
    uint64_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    g_res->fin = ANILLO3_FALLO;
    g_res->vector = m->num_interrupcion;
    g_res->error = m->codigo_error;
    g_res->rip = m->rip;
    g_res->rsp = m->rsp;
    g_res->cr2 = cr2;
    atencion_sys_actual = ATENCION_SIN_SYS;
    anillo3_volver(g_rsp_guardado, 0, g_fs_kernel);
}

void anillo3_tras_irq(void) {
    if (!g_matar || cpu_actual()) return;   /* H213: en las auxiliares lo hace anillo3_al_salir */
    atencion_sys_actual = ATENCION_SIN_SYS;
    g_res->fin = g_hp_fallo_avisado ? ANILLO3_FALLO : ANILLO3_RELOJ;   /* H213: el fallo de un hilo manda */
    anillo3_volver(g_rsp_guardado, 0, g_fs_kernel);
}

/* ------------------------------------------------------------------- pruebas */

static volatile uint64_t g_diana = UINT64_C(0xD1A4A5EC0DEC0DE1);

enum { ARG_NINGUNO, ARG_TEXTO, ARG_DIANA, ARG_HHDM, ARG_PANTALLA };
enum { CR2_NO, CR2_ARG, CR2_CERO, CR2_GUARDA, CR2_CODIGO };

struct prueba {
    const char *nombre;
    int arg, fin;
    uint64_t vector;
    int64_t error;   /* -1: no se compara */
    int cr2;
    int64_t codigo;
};

/* El orden debe coincidir con la tabla de usuario/pruebas_anillo3.asm. */
static const struct prueba g_pruebas[] = {
    { "saludo y estado limpio",       ARG_NINGUNO,  ANILLO3_SALIO, 0, 0, CR2_NO, 42 },
    { "cli",                          ARG_NINGUNO,  ANILLO3_FALLO, 13, 0, CR2_NO, 0 },
    { "hlt",                          ARG_NINGUNO,  ANILLO3_FALLO, 13, 0, CR2_NO, 0 },
    { "in de puerto",                 ARG_NINGUNO,  ANILLO3_FALLO, 13, 0, CR2_NO, 0 },
    { "leer cr3",                     ARG_NINGUNO,  ANILLO3_FALLO, 13, 0, CR2_NO, 0 },
    { "lgdt",                         ARG_NINGUNO,  ANILLO3_FALLO, 13, 0, CR2_NO, 0 },
    { "leer codigo del nucleo",       ARG_TEXTO,    ANILLO3_FALLO, 14, 0x5, CR2_ARG, 0 },
    { "escribir datos del nucleo",    ARG_DIANA,    ANILLO3_FALLO, 14, 0x7, CR2_ARG, 0 },
    { "saltar al nucleo",             ARG_TEXTO,    ANILLO3_FALLO, 14, 0x15, CR2_ARG, 0 },
    { "leer mapa fisico",             ARG_HHDM,     ANILLO3_FALLO, 14, 0x5, CR2_ARG, 0 },
    { "leer pantalla directa",        ARG_PANTALLA, ANILLO3_FALLO, 14, 0x5, CR2_ARG, 0 },
    { "puntero nulo",                 ARG_NINGUNO,  ANILLO3_FALLO, 14, 0x4, CR2_CERO, 0 },
    { "escribir su propio codigo",    ARG_NINGUNO,  ANILLO3_FALLO, 14, 0x7, CR2_CODIGO, 0 },
    { "ejecutar en la pila",          ARG_NINGUNO,  ANILLO3_FALLO, 14, 0x15, CR2_NO, 0 },
    { "ejecutar en datos",            ARG_NINGUNO,  ANILLO3_FALLO, 14, 0x15, CR2_NO, 0 },
    { "desbordar la pila",            ARG_NINGUNO,  ANILLO3_FALLO, 14, 0x6, CR2_GUARDA, 0 },
    { "dividir por cero",             ARG_NINGUNO,  ANILLO3_FALLO, 0, 0, CR2_NO, 0 },
    { "int3 sin permiso",             ARG_NINGUNO,  ANILLO3_FALLO, 13, (3 << 3) | 2, CR2_NO, 0 },
    { "int 0x50 sin permiso",         ARG_NINGUNO,  ANILLO3_FALLO, 13, (0x50 << 3) | 2, CR2_NO, 0 },
    { "fingir interrupcion del reloj",ARG_NINGUNO,  ANILLO3_FALLO, 13, (32 << 3) | 2, CR2_NO, 0 },
    { "instruccion syscall",          ARG_NINGUNO,  ANILLO3_FALLO, 6, 0, CR2_NO, 0 },
    { "instruccion sysenter",         ARG_NINGUNO,  ANILLO3_FALLO, 13, 0, CR2_NO, 0 },
    { "ud2",                          ARG_NINGUNO,  ANILLO3_FALLO, 6, 0, CR2_NO, 0 },
    { "bucle infinito",               ARG_NINGUNO,  ANILLO3_RELOJ, 0, 0, CR2_NO, 0 },
    { "cambiar FS y GS",              ARG_NINGUNO,  ANILLO3_RELOJ, 0, 0, CR2_NO, 0 },
    { "RFLAGS con AC y NT",           ARG_NINGUNO,  ANILLO3_SALIO, 0, 0, CR2_NO, 7 },
    { "paso a paso (TF)",             ARG_NINGUNO,  ANILLO3_FALLO, 1, -1, CR2_NO, 0 },
    { "punteros malos al sistema",    ARG_TEXTO,    ANILLO3_SALIO, 0, 0, CR2_NO, 99 },
    { "registros tras interrupciones",ARG_NINGUNO,  ANILLO3_SALIO, 0, 0, CR2_NO, 55 },
};
#define NUM_PRUEBAS (sizeof(g_pruebas) / sizeof(g_pruebas[0]))

static uint64_t arg_de(int k) {
    switch (k) {
    case ARG_TEXTO: return (uint64_t)__taek_text_inicio;
    case ARG_DIANA: return (uint64_t)&g_diana;
    case ARG_HHDM: return paginacion_obtener_fisica((uint64_t)&g_diana) + memoria_obtener_hhdm_offset();
    case ARG_PANTALLA: return (uint64_t)pantalla_obtener_base();
    default: return 0;
    }
}

static void hash_texto(uint8_t h[32]) {
    struct sha256 s;
    sha256_iniciar(&s);
    sha256_actualizar(&s, __taek_text_inicio, (size_t)(__taek_text_fin - __taek_text_inicio));
    sha256_final(&s, h);
}

static int comprobar(const struct prueba *p, const struct anillo3_resultado *r, uint64_t arg) {
    if (r->fin != p->fin) return 0;
    if (p->fin == ANILLO3_SALIO) return r->codigo == p->codigo;
    if (p->fin == ANILLO3_RELOJ) return r->ticks > 0;
    if (r->vector != p->vector) return 0;
    if (p->error >= 0 && r->error != (uint64_t)p->error) return 0;
    uint64_t pila = ANILLO3_PILA_FIN - (uint64_t)ANILLO3_PILA_PAGINAS * TAMANO_PAGINA;
    switch (p->cr2) {
    case CR2_ARG: return r->cr2 == arg;
    case CR2_CERO: return r->cr2 == 0;
    case CR2_GUARDA: return r->cr2 >= pila - TAMANO_PAGINA && r->cr2 < pila;
    case CR2_CODIGO: return r->cr2 >= ANILLO3_BASE && r->cr2 < ANILLO3_DATOS;
    default: return 1;
    }
}

static void pruebas(void) {
    const uint8_t *img = _binary_pruebas_anillo3_bin_start;
    size_t bytes = (size_t)(_binary_pruebas_anillo3_bin_end - _binary_pruebas_anillo3_bin_start);
    struct anillo3_resultado r;
    unsigned fallos = 0;
    uint64_t fs_alterado = 0, ac = 0, ticks = 0;
    uint64_t fs_antes = leer_msr(MSR_FS_BASE), diana = g_diana;

    /* La primera corrida crea tablas intermedias que quedan; por eso se mide despues. */
    int e = anillo3_ejecutar(img, bytes, 0, 0, 1000, 0, &r);
    if (e) {
        consola_imprimir("[ANILLO3] RESULTADO=NO_DISPONIBLE error=");
        consola_imprimir_dec((uint64_t)(-e));
        consola_imprimir_linea("");
        return;
    }
    consola_imprimir_linea("");
    uint8_t h0[32], h1[32];
    hash_texto(h0);
    memoria_estadisticas_t m0, m1;
    memoria_obtener_estadisticas(&m0);

    for (unsigned i = 0; i < NUM_PRUEBAS; i++) {
        const struct prueba *p = &g_pruebas[i];
        uint64_t arg = arg_de(p->arg);
        e = anillo3_ejecutar(img, bytes, i, arg, p->fin == ANILLO3_RELOJ ? 200 : 2000, 0, &r);
        int ok = !e && comprobar(p, &r, arg);
        /* sysenter en modo largo: #GP en Intel con SYSENTER_CS=0, #UD en AMD y en QEMU con perfil AMD. */
        if (i == 21 && !e && r.fin == ANILLO3_FALLO && r.vector == 6) ok = 1;
        if (i == 0 && ok) {
            const char *t = "Hola desde Ring 3\n";
            for (unsigned k = 0; t[k] || g_ultimo_texto[k]; k++) if (t[k] != g_ultimo_texto[k]) { ok = 0; break; }
        }
        if (i == 24) { if (!r.fs_alterado) ok = 0; fs_alterado = r.fs_alterado; }
        if (i == 25 && r.ac_en_nucleo) ok = 0;
        if (i == 28 && r.ticks < 3) ok = 0;
        ac += r.ac_en_nucleo;
        ticks += r.ticks;
        if (!ok) fallos++;
        if (leer_msr(MSR_FS_BASE) != fs_antes) { ok = 0; fallos++; }
        consola_imprimir(ok ? "[ANILLO3] OK    " : "[ANILLO3] FALLO ");
        consola_imprimir(p->nombre);
        consola_imprimir(" fin=");
        consola_imprimir_dec((uint64_t)r.fin);
        if (r.fin == ANILLO3_FALLO) {
            consola_imprimir(" vector="); consola_imprimir_dec(r.vector);
            consola_imprimir(" err="); consola_imprimir_hex(r.error);
            consola_imprimir(" cr2="); consola_imprimir_hex(r.cr2);
        } else if (r.fin == ANILLO3_SALIO) {
            consola_imprimir(" codigo="); consola_imprimir_dec((uint64_t)r.codigo);
        } else {
            consola_imprimir(" ms="); consola_imprimir_dec(r.ms);
            consola_imprimir(" ticks="); consola_imprimir_dec(r.ticks);
        }
        consola_imprimir_linea("");
    }

    /* Ciclos: fugas y estado acumulado al crear y destruir el programa muchas veces. */
    unsigned ciclos = 0;
    for (; ciclos < 200; ciclos++) {
        e = anillo3_ejecutar(img, bytes, ciclos % 2 ? 1 : 16, 0, 2000, 0, &r);
        if (e || r.fin != ANILLO3_FALLO) break;
    }

    hash_texto(h1);
    memoria_obtener_estadisticas(&m1);
    int hash_igual = !memcmp(h0, h1, 32);
    int64_t delta = (int64_t)m1.paginas_libres - (int64_t)m0.paginas_libres;
    int integro = memoria_verificar_integridad() && m1.canarios_intactos;
    int diana_ok = g_diana == diana;
    if (!hash_igual || delta || !integro || !diana_ok || ciclos != 200 || !ticks) fallos++;
    huevo_verificar();

    consola_imprimir(fallos ? "[ANILLO3] RESULTADO=FALLO" : "[ANILLO3] RESULTADO=PASS");
    consola_imprimir(" pruebas="); consola_imprimir_dec(NUM_PRUEBAS);
    consola_imprimir(" fallos="); consola_imprimir_dec(fallos);
    consola_imprimir(" ciclos="); consola_imprimir_dec(ciclos);
    consola_imprimir(" texto_igual="); consola_imprimir_dec((uint64_t)hash_igual);
    consola_imprimir(" paginas_delta="); consola_imprimir_dec((uint64_t)(delta < 0 ? -delta : delta));
    consola_imprimir(" canarios="); consola_imprimir_dec((uint64_t)integro);
    consola_imprimir(" diana="); consola_imprimir_dec((uint64_t)diana_ok);
    consola_imprimir(" fs_alterado="); consola_imprimir_dec(fs_alterado);
    consola_imprimir(" ac="); consola_imprimir_dec(ac);
    consola_imprimir(" ticks="); consola_imprimir_dec(ticks);
    consola_imprimir(" SMEP="); consola_imprimir_dec((uint64_t)g_smep);
    consola_imprimir(" SMAP="); consola_imprimir_dec((uint64_t)g_smap);
    consola_imprimir_linea("");
}

/* ------------------------------------------------------------ compositor */

extern const uint8_t _binary_prueba_compositor_elf_start[], _binary_prueba_compositor_elf_end[];
extern const uint8_t _binary_demo_compositor_elf_start[], _binary_demo_compositor_elf_end[];

#define NUM_MUESTRAS 8u
static uint32_t g_muestra_x[NUM_MUESTRAS], g_muestra_y[NUM_MUESTRAS], g_muestra_valor[NUM_MUESTRAS];
static int g_muestrear;

static void tomar_muestras(void) {
    if (!g_muestrear) return;
    for (unsigned i = 0; i < NUM_MUESTRAS; i++) g_muestra_valor[i] = compositor_leer_pantalla(g_muestra_x[i], g_muestra_y[i]);
}

static void hash_pantalla(uint8_t h[32]) {
    struct sha256 s;
    sha256_iniciar(&s);
    const uint8_t *fb = pantalla_obtener_base();
    uint64_t paso = pantalla_obtener_stride_bytes(), an = pantalla_obtener_ancho() * 4;
    for (uint64_t y = 0; y < pantalla_obtener_alto(); y++) sha256_actualizar(&s, fb + y * paso, an);
    sha256_final(&s, h);
}

static unsigned comparar_muestras(const uint32_t *esperado) {
    unsigned malas = 0;
    for (unsigned i = 0; i < NUM_MUESTRAS; i++) {
        if (g_muestra_valor[i] == esperado[i]) continue;
        malas++;
        consola_imprimir("[COMPOSITOR] pixel "); consola_imprimir_dec(i);
        consola_imprimir(" esperado="); consola_imprimir_hex(esperado[i]);
        consola_imprimir(" obtenido="); consola_imprimir_hex(g_muestra_valor[i]);
        consola_imprimir_linea("");
    }
    return malas;
}

/* Variantes del ELF real, cada una con un solo campo malicioso: ninguna debe llegar a ejecutarse. */
static unsigned probar_elf_hostiles(const uint8_t *img, size_t bytes) {
    uint8_t *c = asignar_memoria(bytes);
    if (!c) return 1;
    unsigned malos = 0;
    uint64_t phoff;
    memcpy(&phoff, img + 32, 8);
    for (unsigned k = 0; k < 9; k++) {
        memcpy(c, img, bytes);
        uint8_t *ph = c + phoff;
        uint64_t v;
        switch (k) {
        case 0: c[18] = 3; break;                                         /* Otra maquina */
        case 1: c[4] = 1; break;                                          /* 32 bits */
        case 2: v = UINT64_C(0xFFFFFFFF80000000); memcpy(ph + 16, &v, 8); break; /* Segmento sobre el nucleo */
        case 3: v = ANILLO3_PILA_FIN - TAMANO_PAGINA; memcpy(ph + 16, &v, 8); break; /* Segmento sobre la pila */
        case 4: ph[4] = 7; break;                                         /* Escritura + ejecucion */
        case 5: v = UINT64_C(0xFFFFFFFFFFFF0000); memcpy(ph + 32, &v, 8); break; /* filesz gigante */
        case 6: v = bytes + 4096; memcpy(ph + 8, &v, 8); break;           /* offset fuera del archivo */
        case 7: v = UINT64_C(0xFFFFFFFF80000000); memcpy(c + 24, &v, 8); break;  /* Entrada en el nucleo */
        case 8: v = UINT64_C(0x7FFFFFFFFFFFFFFF); memcpy(c + 32, &v, 8); break;  /* phoff imposible */
        }
        struct anillo3_resultado r;
        int e = anillo3_ejecutar(c, bytes, 0, 0, 500, 0, &r);
        if (e >= 0) {
            malos++;
            consola_imprimir("[COMPOSITOR] ELF hostil aceptado, variante "); consola_imprimir_dec(k);
            consola_imprimir_linea("");
        }
    }
    liberar_memoria(c);
    return malos;
}

static void pruebas_compositor(void) {
    const uint8_t *img = _binary_prueba_compositor_elf_start;
    size_t bytes = (size_t)(_binary_prueba_compositor_elf_end - _binary_prueba_compositor_elf_start);
    uint64_t kernel = (uint64_t)__taek_text_inicio;
    uint32_t W = (uint32_t)pantalla_obtener_ancho(), H = (uint32_t)pantalla_obtener_alto();
    static const uint32_t puntos[NUM_MUESTRAS][2] = {
        { 110, 110 }, { 175, 125 }, { 155, 125 }, { 0, 0 }, { 10, 10 }, { 290, 190 }, { 205, 155 }, { 255, 175 } };
    /* rojo, azul al 50% sobre rojo, rojo repuesto al mover, verde recortado, fondo, pixel sin presentar, blanco encima,
     * rojo tapando a una superficie mas nueva tras elevarlo */
    static const uint32_t esperado[NUM_MUESTRAS] = {
        0x00FF0000u, 0x007F0080u, 0x00FF0000u, 0x0000FF00u, COMP_FONDO, 0x00FF0000u, 0x00FFFFFFu, 0x00FF0000u };
    for (unsigned i = 0; i < NUM_MUESTRAS; i++) { g_muestra_x[i] = puntos[i][0]; g_muestra_y[i] = puntos[i][1]; }
    g_muestra_x[3] = W - 5; g_muestra_y[3] = H - 5;

    struct anillo3_resultado r;
    unsigned fallos = 0;
    g_muestrear = 1;

    /* Primera corrida: crea tablas intermedias que quedan, por eso se mide despues. */
    int e = anillo3_ejecutar(img, bytes, 0, kernel, 2000, 0, &r);
    if (e || r.fin != ANILLO3_SALIO) {
        consola_imprimir("[COMPOSITOR] RESULTADO=FALLO arranque e=");
        consola_imprimir_dec((uint64_t)(-e));
        consola_imprimir(" fin="); consola_imprimir_dec((uint64_t)r.fin);
        consola_imprimir(" codigo="); consola_imprimir_dec((uint64_t)r.codigo);
        consola_imprimir(" vector="); consola_imprimir_dec(r.vector);
        consola_imprimir(" rip="); consola_imprimir_hex(r.rip);
        consola_imprimir_linea("");
        g_muestrear = 0;
        return;
    }
    consola_repintar_ahora();
    uint8_t t0[32], t1[32], p0[32], p1[32];
    hash_texto(t0);
    hash_pantalla(p0);
    memoria_estadisticas_t m0, m1;
    memoria_obtener_estadisticas(&m0);

    /* A: escena normal con la capa CPU, leyendo el framebuffer real. */
    e = anillo3_ejecutar(img, bytes, 0, kernel, 2000, 0, &r);
    unsigned a_codigo = !e && r.fin == ANILLO3_SALIO && r.codigo == 77;
    unsigned a_malas = comparar_muestras(esperado);
    uint64_t composiciones = r.composiciones;
    hash_pantalla(p1);
    unsigned a_consola = !memcmp(p0, p1, 32);

    /* B: la capa rechaza todo; la CPU debe dar exactamente lo mismo. */
    compositor_forzar_rechazo(1);
    e = anillo3_ejecutar(img, bytes, 0, kernel, 2000, 0, &r);
    uint64_t rechazos = compositor_rechazos();
    compositor_forzar_rechazo(0);
    unsigned b_codigo = !e && r.fin == ANILLO3_SALIO && r.codigo == 77;
    unsigned b_malas = comparar_muestras(esperado);

    /* C: el programa revienta con la escena en pantalla. */
    e = anillo3_ejecutar(img, bytes, 1, kernel, 2000, 0, &r);
    unsigned c_fallo = !e && r.fin == ANILLO3_FALLO && r.vector == 6;
    unsigned c_malas = comparar_muestras(esperado);
    hash_pantalla(p1);
    unsigned c_consola = !memcmp(p0, p1, 32);

    /* D: archivos ELF con campos maliciosos. */
    g_muestrear = 0;
    unsigned d_malos = probar_elf_hostiles(img, bytes);

    hash_texto(t1);
    memoria_obtener_estadisticas(&m1);
    int64_t delta = (int64_t)m1.paginas_libres - (int64_t)m0.paginas_libres;
    int64_t heap = (int64_t)m1.heap_bytes_en_uso - (int64_t)m0.heap_bytes_en_uso;
    int integro = memoria_verificar_integridad() && m1.canarios_intactos;
    int texto = !memcmp(t0, t1, 32);
    fallos = !a_codigo + a_malas + !a_consola + !b_codigo + b_malas + !rechazos + !c_fallo + c_malas + !c_consola +
             d_malos + (delta != 0) + (heap != 0) + !integro + !texto + !composiciones;
    huevo_verificar();

    consola_imprimir(fallos ? "[COMPOSITOR] RESULTADO=FALLO" : "[COMPOSITOR] RESULTADO=PASS");
    consola_imprimir(" fallos="); consola_imprimir_dec(fallos);
    consola_imprimir(" capa="); consola_imprimir(compositor_capa_nombre());
    consola_imprimir(" pantalla="); consola_imprimir_dec(W); consola_imprimir("x"); consola_imprimir_dec(H);
    consola_imprimir(" composiciones="); consola_imprimir_dec(composiciones);
    consola_imprimir(" rechazos="); consola_imprimir_dec(rechazos);
    consola_imprimir(" fallo_contenido="); consola_imprimir_dec(c_fallo);
    consola_imprimir(" consola_igual="); consola_imprimir_dec(a_consola && c_consola);
    consola_imprimir(" elf_aceptados="); consola_imprimir_dec(d_malos);
    consola_imprimir(" paginas_delta="); consola_imprimir_dec((uint64_t)(delta < 0 ? -delta : delta));
    consola_imprimir(" heap_delta="); consola_imprimir_dec((uint64_t)(heap < 0 ? -heap : heap));
    consola_imprimir(" texto_igual="); consola_imprimir_dec((uint64_t)texto);
    consola_imprimir_linea("");
}

static void demo(const char *arg) {
    uint64_t seg = 0;
    while (arg && *arg >= '0' && *arg <= '9' && seg < 100000) seg = seg * 10 + (uint64_t)(*arg++ - '0');
    if (!seg || seg > 600) seg = 10;
    struct anillo3_resultado r;
    int e = anillo3_ejecutar(_binary_demo_compositor_elf_start,
                             (size_t)(_binary_demo_compositor_elf_end - _binary_demo_compositor_elf_start),
                             seg, 0, 3000, (uint32_t)(seg * 1000 + 5000), &r);
    uint64_t c = tiempo_ciclos_por_ms();
    consola_imprimir("[COMPOSITOR] DEMO e="); consola_imprimir_dec((uint64_t)(e < 0 ? -e : e));
    consola_imprimir(" fin="); consola_imprimir_dec((uint64_t)r.fin);
    consola_imprimir(" cuadros="); consola_imprimir_dec((uint64_t)(r.codigo < 0 ? 0 : r.codigo));
    consola_imprimir(" ms="); consola_imprimir_dec(r.ms);
    if (r.ms) { consola_imprimir(" fps="); consola_imprimir_dec((uint64_t)(r.codigo < 0 ? 0 : r.codigo) * 1000 / r.ms); }
    consola_imprimir(" composiciones="); consola_imprimir_dec(r.composiciones);
    if (c) {
        consola_imprimir(" componer_ms="); consola_imprimir_dec(r.ciclos_componer / c);
        consola_imprimir(" presentar_ms="); consola_imprimir_dec(r.ciclos_presentar / c);
    }
    if (r.fin == ANILLO3_FALLO) { consola_imprimir(" vector="); consola_imprimir_dec(r.vector); consola_imprimir(" rip="); consola_imprimir_hex(r.rip); }
    consola_imprimir_linea("");
}

extern const uint8_t _binary_menu_elf_start[], _binary_menu_elf_end[];

/* H142: primero el 8042 (teclado interno y, si contesta, el trackpad como mouse PS/2). El trackpad por I2C se busca
 * una vez, en silencio, solo si no hay mouse PS/2: reiniciarlo por I2C podria cortar su salida PS/2. A mano:
 * 'ps2 iniciar' y 'tactil iniciar'. */
static void preparar_entrada(void) {
    static int ps2_buscado, tactil_buscado;
    if (!ps2_buscado && !ps2_activo()) { ps2_buscado = 1; (void)ps2_iniciar(0); }
    if (!tactil_buscado && !tactil_listo() && !ps2_raton_activo()) { tactil_buscado = 1; (void)tactil_iniciar(0); }
}

/* H143: un programa de Anillo 3 que no va dentro del nucleo. Se lee del disco montado y pasa por el mismo cargador
 * que el menu, que no confia en el archivo: comprueba cada segmento, rechaza escritura + ejecucion y lo deja todo
 * dentro de la mitad de usuario. Asi se agregan aplicaciones sin recompilar ni reiniciar. */
#define ANILLO3_IMAGEN_MAX (4u * 1024u * 1024u)
/* Una ejecucion. 0 si el programa llego a correr (termine como termine). */
/* H146: si el archivo no esta en el disco abierto (o no hay ninguno), se prueba en cada USB listo. Asi el menu puede abrir
 * aplicaciones recien encendido el equipo, sin 'seleccionar' en la terminal. */
static int asegurar_disco(const char *ruta) {
    int fd;
    /* Tres intentos: justo despues de cortar un video con Esc, la primera lectura del USB puede salir cancelada
     * (en la laptop: "No se encontro /videos.elf" y, segundos despues, el archivo estaba ahi). */
    for (int intento = 0; intento < 3; intento++) {
        if (intento) esperar_milisegundos(150);
        if (vfs_esta_montado() && !vfs_abrir(ruta, &fd)) { vfs_cerrar(fd); return 0; }
        for (uint8_t i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
            const struct usb_msc_dispositivo *u = usb_msc_obtener_dispositivo(i);
            if (!u || !u->activo || !u->listo) continue;
            if (vfs_montar_particion(i, 0)) continue;
            if (!vfs_abrir(ruta, &fd)) { vfs_cerrar(fd); return 0; }
        }
    }
    return -1;
}

void terminal_abrir_archivo(const char *ruta);   /* terminal.c: reproduce o muestra segun el tipo */

static int ejecutar_una(const char *ruta, const char *argumento) {
    if (!memcmp(ruta, TAEK_PROGRAMA_ABRIR, sizeof(TAEK_PROGRAMA_ABRIR))) {
        consola_imprimir("[EJECUTAR] El nucleo abre "); consola_imprimir_linea(argumento);
        if (!memcmp(argumento, TAEK_VIDEO_MEM, sizeof(TAEK_VIDEO_MEM) - 1)) {     /* H185: MP4 entregado con SYS_VIDEO */
            if (!g_video) { consola_imprimir_linea("[EJECUTAR] No hay video entregado."); g_lanzar_pedido = 0; return -1; }
            consola_limpiar();
            int rv = reproductor_reproducir_memoria(g_video, (size_t)g_video_n, argumento + sizeof(TAEK_VIDEO_MEM) - 1, MODO_INTERACTIVO);
            consola_imprimir("[EJECUTAR] Video en memoria ("); consola_imprimir_dec(g_video_n);
            consola_imprimir(" B) termino con "); consola_imprimir_dec((uint64_t)(rv < 0 ? -rv : rv)); consola_imprimir_linea(rv < 0 ? " (error)" : "");
            liberar_memoria(g_video); g_video = 0; g_video_n = 0;
            for (int i = 0; i < 256 && consola_leer_caracter(); i++) {}
            g_lanzar_pedido = 0;
            return -1;
        }
        if (asegurar_disco(argumento)) { consola_imprimir_linea("[EJECUTAR] No se encontro el archivo."); return -1; }
        consola_limpiar();
        terminal_abrir_archivo(argumento);
        /* Las teclas que quedaron (el Esc que corto el video) eran para el reproductor: se tiran, para que no cancelen
         * la lectura del programa al que se vuelve. */
        for (int i = 0; i < 256 && consola_leer_caracter(); i++) {}
        g_lanzar_pedido = 0;
        return -1;   /* Nada que lanzar despues: se vuelve a quien lo pidio */
    }
    if (asegurar_disco(ruta)) { consola_imprimir("[EJECUTAR] No se encontro "); consola_imprimir_linea(ruta); return -1; }
    int fd;
    if (vfs_abrir(ruta, &fd)) { consola_imprimir("[EJECUTAR] No se encontro "); consola_imprimir_linea(ruta); return -1; }
    uint64_t tam = vfs_tamano_fd(fd);
    if (!tam || tam > ANILLO3_IMAGEN_MAX) {
        vfs_cerrar(fd);
        consola_imprimir_linea("[EJECUTAR] El archivo esta vacio o pasa de 4 MiB.");
        return -1;
    }
    uint8_t *img = asignar_memoria(tam);
    if (!img) { vfs_cerrar(fd); consola_imprimir_linea("[EJECUTAR] Sin memoria para leerlo."); return -1; }
    uint64_t leidos = 0;
    while (leidos < tam) {
        int64_t k = vfs_leer(fd, img + leidos, (size_t)(tam - leidos));
        if (k <= 0) break;
        leidos += (uint64_t)k;
    }
    vfs_cerrar(fd);
    if (leidos != tam) { liberar_memoria(img); consola_imprimir_linea("[EJECUTAR] No se pudo leer entero."); return -1; }
    if (tam < 4 || memcmp(img, "\x7F" "ELF", 4)) {
        liberar_memoria(img);
        consola_imprimir_linea("[EJECUTAR] No es un ELF: solo se ejecutan programas ELF de 64 bits.");
        return -1;
    }
    consola_imprimir("[EJECUTAR] "); consola_imprimir(ruta); consola_imprimir(" ("); consola_imprimir_dec(tam);
    consola_imprimir(" bytes) en Anillo 3");
    if (argumento && *argumento) { consola_imprimir(" con argumento "); consola_imprimir(argumento); }
    consola_imprimir_linea("...");
    copiar_txt(g_argumento, sizeof(g_argumento), argumento);
    g_lanzar_pedido = 0;
    preparar_entrada();
    struct anillo3_resultado r;
    int e = anillo3_ejecutar(img, (size_t)tam, 0, 0, 5000, 0, &r);
    liberar_memoria(img);
    g_argumento[0] = 0;
    if (e) {
        consola_imprimir("[EJECUTAR] El cargador lo rechazo (codigo "); consola_imprimir_dec((uint64_t)(e < 0 ? -e : e));
        consola_imprimir_linea("): -7 no es ELF x86-64, -8/-9/-10 segmentos fuera de sitio, -11 escritura y ejecucion, -13 entrada.");
        g_lanzar_pedido = 0;
        return -1;
    }
    if (r.fin == ANILLO3_SALIO) {
        consola_imprimir("[EJECUTAR] Termino con codigo "); consola_imprimir_dec((uint64_t)r.codigo);
    } else if (r.fin == ANILLO3_RELOJ) {
        consola_imprimir("[EJECUTAR] Detenido: 5 s sin llamar al sistema");
    } else {
        consola_imprimir("[EJECUTAR] Fallo del programa: vector "); consola_imprimir_dec(r.vector);
        consola_imprimir(" rip="); consola_imprimir_hex(r.rip); consola_imprimir(" cr2="); consola_imprimir_hex(r.cr2);
    }
    consola_imprimir(" tras "); consola_imprimir_dec(r.ms); consola_imprimir(" ms");
    if (r.monton_pico) {
        consola_imprimir(", memoria pedida como mucho "); consola_imprimir_dec((r.monton_pico + 1023) / 1024); consola_imprimir(" KB");
    }
    consola_imprimir_linea(". El sistema sigue en pie.");
    if (r.fin != ANILLO3_SALIO) g_lanzar_pedido = 0;   /* Un programa que fallo no decide que viene despues */
    return 0;
}

/* H145: 'ejecutar <ruta> [argumento]'. Si el programa pidio abrir otro (SYS_LANZAR), se abre al terminar; cuando ese
 * termina, se vuelve al primero con el argumento que dejo. Asi el explorador abre una aplicacion y vuelve a su carpeta.
 * Solo un nivel de vuelta: lo que se lanza desde lo lanzado reemplaza a lo anterior, y al final se vuelve al primero. */
static char g_cad_actual[TAEK_RUTA], g_cad_arg[TAEK_RUTA];
static void cadena(void) {
    static char volver[TAEK_RUTA], volver_arg[TAEK_RUTA];
    char *actual = g_cad_actual, *arg = g_cad_arg;
    volver[0] = 0;
    for (unsigned vueltas = 0; vueltas < 1000; vueltas++) {
        int r = ejecutar_una(actual, arg);
        if (!r && g_lanzar_pedido) {
            g_lanzar_pedido = 0;
            if (!volver[0]) {
                copiar_txt(volver, sizeof(volver), actual);
                copiar_txt(volver_arg, sizeof(volver_arg), g_lanzar.argumento_al_volver);
            }
            copiar_txt(actual, TAEK_RUTA, g_lanzar.programa);
            copiar_txt(arg, TAEK_RUTA, g_lanzar.argumento);
            continue;
        }
        if (volver[0]) {
            copiar_txt(actual, TAEK_RUTA, volver);
            copiar_txt(arg, TAEK_RUTA, volver_arg);
            volver[0] = 0;
            continue;
        }
        break;
    }
}

void anillo3_ejecutar_archivo(const char *linea) {
    while (linea && *linea == ' ') linea++;
    if (!linea || !*linea) { consola_imprimir_linea("Uso: ejecutar <ruta.elf> [argumento]   (ej.: ejecutar /archivos.elf /taek)"); return; }
    size_t i = 0;
    while (linea[i] && linea[i] != ' ' && i + 1 < sizeof(g_cad_actual)) { g_cad_actual[i] = linea[i]; i++; }
    g_cad_actual[i] = 0;
    const char *a = linea + i;
    while (*a == ' ') a++;
    copiar_txt(g_cad_arg, sizeof(g_cad_arg), a);
    cadena();
}

/* H146: el menu pidio una aplicacion (SYS_LANZAR y TAEK_ACCION_APP). Se corre, con lo que ella abra, y se vuelve. */
int anillo3_lanzar_pendiente(void) {
    if (!g_lanzar_pedido) return -1;
    g_lanzar_pedido = 0;
    copiar_txt(g_cad_actual, sizeof(g_cad_actual), g_lanzar.programa);
    copiar_txt(g_cad_arg, sizeof(g_cad_arg), g_lanzar.argumento);
    cadena();
    return 0;
}

/* H149: el menu (y con el el escritorio y sus ventanas) tambien puede venir del USB, como cualquier aplicacion: si hay un
 * /menu.elf en algun disco, 'menu' lo lee una vez y lo usa hasta el proximo 'menu'. Asi cambiar la interfaz no pide
 * ISO ni reinicio. El de la ISO queda de respaldo: si el del USB no carga o se cae, se vuelve a el en el acto, y esa
 * misma copia (por su huella) no se vuelve a intentar hasta que llegue otra. */
static uint8_t *g_menu_usb;
static uint64_t g_menu_usb_tam, g_menu_usb_huella, g_menu_malo;

static uint64_t huella(const uint8_t *p, uint64_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

void anillo3_menu_cargar(int solo_iso) {
    if (g_menu_usb) { liberar_memoria(g_menu_usb); g_menu_usb = NULL; g_menu_usb_tam = 0; }
    if (solo_iso) { consola_imprimir_linea("[UI] Menu de la ISO (pedido)."); return; }
    int fd;
    if (asegurar_disco("/menu.elf") || vfs_abrir("/menu.elf", &fd)) { consola_imprimir_linea("[UI] Menu de la ISO: no hay /menu.elf en los discos."); return; }
    uint64_t tam = vfs_tamano_fd(fd);
    uint8_t *img = tam && tam <= ANILLO3_IMAGEN_MAX ? asignar_memoria(tam) : NULL;
    uint64_t leidos = 0;
    while (img && leidos < tam) {
        int64_t k = vfs_leer(fd, img + leidos, (size_t)(tam - leidos));
        if (k <= 0) break;
        leidos += (uint64_t)k;
    }
    vfs_cerrar(fd);
    if (!img || leidos != tam || tam < 4 || memcmp(img, "\x7F" "ELF", 4)) {
        if (img) liberar_memoria(img);
        consola_imprimir_linea("[UI] Menu de la ISO: /menu.elf no se pudo leer o no es un ELF.");
        return;
    }
    uint64_t h = huella(img, tam);
    if (h == g_menu_malo) {
        liberar_memoria(img);
        consola_imprimir_linea("[UI] Menu de la ISO: esa copia de /menu.elf ya fallo una vez.");
        return;
    }
    g_menu_usb = img; g_menu_usb_tam = tam; g_menu_usb_huella = h;
    consola_imprimir("[UI] Menu del USB: /menu.elf ("); consola_imprimir_dec(tam); consola_imprimir(" bytes, huella ");
    consola_imprimir_hex(h); consola_imprimir_linea(")");
}

int anillo3_menu(uint32_t posicion, int modo_prueba, struct anillo3_resultado *r) {
    g_captura_permitida = modo_prueba == 1;
    preparar_entrada();
    int e;
    if (g_menu_usb) {
        e = anillo3_ejecutar(g_menu_usb, (size_t)g_menu_usb_tam, posicion, (uint64_t)modo_prueba, 5000, modo_prueba ? 600000 : 0, r);
        if (!e && r->fin == ANILLO3_SALIO) { g_captura_permitida = 0; return 0; }
        /* No cargo o se cayo: se anota y se sigue con el de la ISO, sin dejar al usuario sin interfaz */
        consola_imprimir("[UI] El menu del USB fallo (e="); consola_imprimir_dec((uint64_t)(e < 0 ? -e : e));
        consola_imprimir(" fin="); consola_imprimir_dec((uint64_t)r->fin); consola_imprimir(" rip="); consola_imprimir_hex(r->rip);
        consola_imprimir_linea("): se usa el de la ISO.");
        g_menu_malo = g_menu_usb_huella;
        liberar_memoria(g_menu_usb); g_menu_usb = NULL; g_menu_usb_tam = 0;
        preparar_entrada();
    }
    e = anillo3_ejecutar(_binary_menu_elf_start, (size_t)(_binary_menu_elf_end - _binary_menu_elf_start),
                         posicion, (uint64_t)modo_prueba, 5000, modo_prueba ? 600000 : 0, r);
    g_captura_permitida = 0;
    return e;
}

void anillo3_comando(const char *arg) {
    if (arg && arg[0] == 'p') { pruebas(); return; }
    if (arg && arg[0] == 'c') { pruebas_compositor(); return; }
    if (arg && arg[0] == 'd') {
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
        demo(arg);
        return;
    }
    consola_imprimir_linea("Uso: anillo3 prueba | compositor | demo [segundos]");
    consola_imprimir("Preparado: "); consola_imprimir_dec((uint64_t)g_preparado);
    consola_imprimir(" SMEP="); consola_imprimir_dec((uint64_t)g_smep);
    consola_imprimir(" SMAP="); consola_imprimir_dec((uint64_t)g_smap);
    consola_imprimir_linea("");
}
