#include "memoria.h"
#include "dma.h"
#include "huevo.h"
#include "trabajos.h"
#include "cerrojo.h"
#include "tiempo.h"
#include "../arquitectura/x86_64/serial.h"

#define LIMINE_API_REVISION 2
#include "../../boot/limine/limine.h"

// Petición del mapa de memoria UEFI entregado por Limine
__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request g_peticion_mapa_memoria = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

// Petición del desplazamiento Higher Half Direct Map (HHDM)
__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request g_peticion_hhdm = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

#define FISICA_A_VIRTUAL(phys) ((void *)((uint64_t)(phys) + g_hhdm_offset))
#define VIRTUAL_A_FISICA(virt) ((uint64_t)(virt) - g_hhdm_offset)

// Estado del Gestor de Páginas Físicas (PMM)
static uint64_t g_hhdm_offset = 0;
static uint64_t g_ram_total_bytes = 0;
static uint64_t g_ram_usable_bytes = 0;
static uint64_t g_pila_marcos_libres = 0; // Dirección física del tope de la pila de marcos libres
static uint64_t g_paginas_totales = 0;
static uint64_t g_paginas_libres = 0;
static uint64_t g_paginas_en_uso = 0;

// Estado por marco utilizable: 0 reservado, 1 libre, 2 asignado por PMM.
// Los marcos de heap/DMA se mantienen reservados fuera de la pila PMM.
#define PMM_MAX_RANGOS 128
struct pmm_rango_estado {
    uint64_t inicio;
    uint64_t paginas;
    uint8_t *estado;
};
static struct pmm_rango_estado g_pmm_rangos[PMM_MAX_RANGOS];
static uint32_t g_pmm_num_rangos = 0;
static int g_pmm_estado_listo = 0;

static int pmm_estado_de(uint64_t phys, uint8_t **byte, uint8_t *desplazamiento) {
    for (uint32_t i = 0; i < g_pmm_num_rangos; i++) {
        const struct pmm_rango_estado *r = &g_pmm_rangos[i];
        if (phys >= r->inicio && (phys - r->inicio) / TAMANO_PAGINA < r->paginas) {
            uint64_t idx = (phys - r->inicio) / TAMANO_PAGINA;
            *byte = &r->estado[idx / 4];
            *desplazamiento = (uint8_t)((idx % 4) * 2);
            return 0;
        }
    }
    return -1;
}

static int pmm_estado_leer(uint64_t phys) {
    uint8_t *byte, desplazamiento;
    if (pmm_estado_de(phys, &byte, &desplazamiento) != 0) return -1;
    return (*byte >> desplazamiento) & 3;
}

static void pmm_estado_escribir(uint64_t phys, uint8_t valor) {
    uint8_t *byte, desplazamiento;
    if (pmm_estado_de(phys, &byte, &desplazamiento) != 0) return;
    *byte = (uint8_t)((*byte & ~(3U << desplazamiento)) |
                      ((valor & 3U) << desplazamiento));
}

// Estructura de Bloque de Heap del Kernel
typedef struct bloque_heap {
    uint64_t canario_inicio;     // CANARIO_BLOQUE_INICIO (0x7AEECC05ULL)
    uint32_t esta_libre;         // 1 = libre, 0 = en uso
    uint32_t reservado;          // Relleno de alineación
    uint64_t tamano_datos;       // Tamaño útil en bytes para el usuario
    struct bloque_heap *siguiente;
    struct bloque_heap *anterior;
    struct bloque_heap *libre_sig;   // H207: lista de libres de su tamaño (solo si esta_libre)
    struct bloque_heap *libre_ant;
    uint64_t canario_fin;        // CANARIO_BLOQUE_FIN (0xCAFEBABEDEAD1000ULL)
} __attribute__((aligned(16))) bloque_heap_t;

// Estado del Heap del Kernel
static bloque_heap_t *g_heap_cabeza = NULL;
static bloque_heap_t *g_heap_cola = NULL;   /* H207: ultimo bloque de la lista, para agregar arenas sin recorrerla */

/* H207, antes de la Fase A2 del gestor de hilos: en H206 el cerrojo del monton llego a 1,6 ms en uso real porque el
 * primer ajuste recorria la lista entera (miles de bloques, la mayoria en uso). Ahora los bloques libres van ademas en
 * listas por tamano (potencias de 2, desde 16 bytes): pedir memoria mira solo libres de un tamano que sirve. La lista
 * de todos los bloques, en orden de arena, sigue igual: la usan las fusiones y las auditorias del huevo. */
#define HEAP_CLASES 28
static bloque_heap_t *g_libres[HEAP_CLASES];
static unsigned clase_de(uint64_t bytes) {
    unsigned c = 63u - (unsigned)__builtin_clzll(bytes | 16u) ;   /* floor(log2) */
    c = c >= 4 ? c - 4 : 0;
    return c < HEAP_CLASES ? c : HEAP_CLASES - 1;
}
static uint64_t g_heap_libres_n;   /* H214: bloques libres, llevado al dia aqui en vez de recorrer la lista */
static void libre_poner(bloque_heap_t *b) {
    unsigned c = clase_de(b->tamano_datos);
    g_heap_libres_n++;
    b->libre_ant = NULL;
    b->libre_sig = g_libres[c];
    if (g_libres[c]) g_libres[c]->libre_ant = b;
    g_libres[c] = b;
}
static void libre_quitar(bloque_heap_t *b) {
    unsigned c = clase_de(b->tamano_datos);
    if (g_heap_libres_n) g_heap_libres_n--;
    if (b->libre_ant) b->libre_ant->libre_sig = b->libre_sig;
    else if (g_libres[c] == b) g_libres[c] = b->libre_sig;
    if (b->libre_sig) b->libre_sig->libre_ant = b->libre_ant;
    b->libre_sig = b->libre_ant = NULL;
}
static uint64_t       g_heap_capacidad_total = 0;
static uint64_t       g_heap_bytes_en_uso = 0;
static uint64_t       g_heap_bloques_activos = 0;
static int            g_memoria_inicializada = 0;

// Tamaño inicial asignado al Heap del kernel (8 MiB)
#define TAMANO_ARENA_INICIAL (8ULL * 1024ULL * 1024ULL)

// Arena contigua de DMA físico, alineada a 2 MiB. De aquí salen las superficies de la GPU, que a 1080p
// son 3 MiB cada una: se pide la máxima si la RAM lo permite y se baja hasta la mínima de siempre.
#define TAMANO_ARENA_DMA     (32ULL * 1024ULL * 1024ULL)
#define TAMANO_ARENA_DMA_MAX (256ULL * 1024ULL * 1024ULL)
// Arena de los objetos de la GPU, por encima de 4 GiB: bajo 4 GiB suele no quedar un bloque así de grande.
#define TAMANO_ARENA_GPU     (256ULL * 1024ULL * 1024ULL)
static uint64_t g_arena_gpu_fisica = 0;
static uint64_t g_arena_gpu_tamano = 0;
static uint64_t g_arena_dma_fisica = 0;
static uint64_t g_arena_dma_tamano = 0;

// --- FUNCIONES BÁSICAS DE COPIA Y RELLENO (FREESTANDING) ---

/* H206: rep stosb y rep movsb (ERMSB en el i7-8650U: el microcodigo los hace por lineas de cache). Antes eran bucles
 * de un byte: poner a cero 2 MiB al crecer el monton tardaba milisegundos con el cerrojo de memoria tomado. */
void *memset(void *dest, int c, size_t n) {
    void *d = dest;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    void *d = dest;
    const void *s = src;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    return 0;
}

/* H205, Fase 0.5 del gestor de hilos: un solo cerrojo para el PMM y el monton (el monton pide paginas al crecer).
 * Antes estas funciones devolvian vacio fuera de CPU 0 (trabajos_es_coordinador). Es recursivo por CPU: el monton se
 * llama a si mismo tras incorporar una arena. Las funciones _sc son el cuerpo de siempre, sin cerrojo. */
static cerrojo_t g_cerrojo_memoria = CERROJO("memoria");
#define CON_CERROJO(tipo, llamada) { uint64_t f_ = cerrojo_tomar_irq(&g_cerrojo_memoria); tipo r_ = llamada; cerrojo_soltar_irq(&g_cerrojo_memoria, f_); return r_; }
#define CON_CERROJO_VOID(llamada) { uint64_t f_ = cerrojo_tomar_irq(&g_cerrojo_memoria); llamada; cerrojo_soltar_irq(&g_cerrojo_memoria, f_); }

// --- GESTOR DE MARCOS DE PÁGINA FÍSICA (PMM - 4 KiB) ---

static uint64_t pmm_asignar_pagina_fisica_sc(void);
/* H206: la pagina se pone a cero despues de soltar el cerrojo. Si quien llama ya tiene el cerrojo (el monton al
 * crecer), la pagina sale sin limpiar: el monton no promete memoria a cero (asignar_memoria_cero limpia lo suyo). */
uint64_t pmm_asignar_pagina_fisica(void) {
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo_memoria);
    int anidada = g_cerrojo_memoria.profundidad > 1;
    uint64_t phys = pmm_asignar_pagina_fisica_sc();
    cerrojo_soltar_irq(&g_cerrojo_memoria, f);
    if (phys && !anidada) memset(FISICA_A_VIRTUAL(phys), 0, TAMANO_PAGINA);
    return phys;
}
static uint64_t pmm_asignar_pagina_fisica_sc(void) {
    if (g_pila_marcos_libres == 0) {
        serial_imprimir_linea("[PMM FATAL] ¡Memoria física agotada! Sin marcos libres.");
        huevo_agrietar("PMM: Memoria física agotada (Out of Memory)");
        return 0;
    }

    uint64_t phys = g_pila_marcos_libres;
    uint64_t *frame_virt = (uint64_t *)FISICA_A_VIRTUAL(phys);
    if (g_pmm_estado_listo) {
        if (pmm_estado_leer(phys) != 1) {
            huevo_quebrar("PMM: pila de marcos corrupta", phys, 0, 0);
            return 0;
        }
        pmm_estado_escribir(phys, 2);
    }

    // Desapilar el marco libre
    g_pila_marcos_libres = *frame_virt;

    if (g_paginas_libres > 0) g_paginas_libres--;
    g_paginas_en_uso++;


    return phys;
}

void *pmm_asignar_pagina_virtual(void) {
    uint64_t phys = pmm_asignar_pagina_fisica();
    if (phys == 0) return NULL;
    return FISICA_A_VIRTUAL(phys);
}

static void pmm_liberar_pagina_fisica_sc(uint64_t phys);
void pmm_liberar_pagina_fisica(uint64_t phys) CON_CERROJO_VOID(pmm_liberar_pagina_fisica_sc(phys))
static void pmm_liberar_pagina_fisica_sc(uint64_t phys) {
    if (phys == 0 || (phys & 0xFFFULL) != 0) {
        huevo_quebrar("PMM: dirección de liberación inválida", phys, 0, 0);
        return;
    }
    if (g_pmm_estado_listo) {
        if (pmm_estado_leer(phys) != 2) {
            huevo_quebrar("PMM: double free o página reservada", phys, 0, 0);
            return;
        }
        pmm_estado_escribir(phys, 1);
    }

    uint64_t *frame_virt = (uint64_t *)FISICA_A_VIRTUAL(phys);

    // Apilar en la lista intrusiva de marcos libres
    *frame_virt = g_pila_marcos_libres;
    g_pila_marcos_libres = phys;

    g_paginas_libres++;
    if (g_paginas_en_uso > 0) g_paginas_en_uso--;
}

// --- GESTOR DE HEAP DEL KERNEL (VIGILADO POR EL HUEVO) ---

// Inicializa una región de memoria continua como una arena de Heap
static void heap_agregar_arena(void *direccion_virtual, uint64_t tamano_bytes) {
    if (tamano_bytes < sizeof(bloque_heap_t) + 64) return;

    bloque_heap_t *nuevo_bloque = (bloque_heap_t *)direccion_virtual;
    nuevo_bloque->canario_inicio = CANARIO_BLOQUE_INICIO;
    nuevo_bloque->esta_libre = 1;
    nuevo_bloque->reservado = 0;
    nuevo_bloque->tamano_datos = tamano_bytes - sizeof(bloque_heap_t);
    nuevo_bloque->siguiente = NULL;
    nuevo_bloque->anterior = NULL;
    nuevo_bloque->canario_fin = CANARIO_BLOQUE_FIN;

    g_heap_capacidad_total += tamano_bytes;

    if (g_heap_cabeza == NULL) {
        g_heap_cabeza = nuevo_bloque;
    } else {
        // Enlazar al final de la lista de arenas
        g_heap_cola->siguiente = nuevo_bloque;
        nuevo_bloque->anterior = g_heap_cola;
    }
    g_heap_cola = nuevo_bloque;
    libre_poner(nuevo_bloque);
}

static void heap_canario_roto(bloque_heap_t *bloque) {
    serial_imprimir("[CORRUPCIÓN DETECTADA] Bloque de Heap dañado en ");
    serial_imprimir_hex((uint64_t)bloque);
    serial_imprimir_linea("!");
    huevo_quebrar("Corrupción de memoria en Heap: Canario de bloque destruido", (uint64_t)bloque, 0, 0);
}

/* El primer libre que sirve: en la clase de la peticion puede haber bloques mas chicos; en las de arriba, todos sirven.
 * H214: en H212 el cerrojo del monton llego a 1,8 ms con CPU 0 y CPU 3 pidiendo a la vez, porque la clase propia se
 * recorria entera (muchos bloques apenas mas chicos que lo pedido). Ahora: hasta 16 intentos en la clase propia (asi
 * se siguen reusando los bloques de ese tamano y no se fragmenta mas), despues el primero de una clase mayor (sirve
 * seguro, O(1) por clase) y solo si no hay ninguno, el resto de la clase propia. */
static inline void heap_vigilar_libre(bloque_heap_t *b) {
    if (b->canario_inicio != CANARIO_BLOQUE_INICIO || b->canario_fin != CANARIO_BLOQUE_FIN || !b->esta_libre)
        heap_canario_roto(b);
}
static bloque_heap_t *heap_buscar_libre(uint64_t bytes) {
    unsigned c = clase_de(bytes), n = 0;
    bloque_heap_t *b = g_libres[c];
    for (; b && n < 16; b = b->libre_sig, n++) {
        heap_vigilar_libre(b);
        if (b->tamano_datos >= bytes) return b;
    }
    for (unsigned k = c + 1; k < HEAP_CLASES; k++) {
        bloque_heap_t *m = g_libres[k];
        if (!m) continue;
        heap_vigilar_libre(m);
        if (m->tamano_datos >= bytes) return m;
    }
    for (; b; b = b->libre_sig) {
        heap_vigilar_libre(b);
        if (b->tamano_datos >= bytes) return b;
    }
    return NULL;
}

/* H214: crecer el monton ya no pasa dentro del cerrojo. En H212-H214 el cerrojo 'memoria' llego a 1,5-1,8 ms, con las
 * interrupciones apagadas, mientras juntaba hasta 512 paginas contiguas (y hasta 128 intentos si el PMM estaba
 * fragmentado). Ahora asignar_memoria_sc solo toma de lo libre; si no hay, heap_crecer junta las paginas tomando y
 * soltando el cerrojo por cada una (el PMM usa el mismo) y solo vuelve a tenerlo para agregar la arena. */
static void *asignar_memoria_sc(uint64_t bytes);
static int heap_crecer(uint64_t bytes);
void *asignar_memoria(uint64_t bytes) {
    if (bytes == 0) return NULL;
    uint64_t f = cerrojo_tomar_irq(&g_cerrojo_memoria);
    void *r = asignar_memoria_sc(bytes);
    cerrojo_soltar_irq(&g_cerrojo_memoria, f);
    for (unsigned intento = 0; !r && intento < 128; intento++) {
        int c = heap_crecer(bytes);
        if (c < 0) break;
        f = cerrojo_tomar_irq(&g_cerrojo_memoria);
        r = asignar_memoria_sc(bytes);
        cerrojo_soltar_irq(&g_cerrojo_memoria, f);
    }
    if (!r) serial_imprimir_linea("[HEAP ADVERTENCIA] No fue posible conseguir arena contigua suficiente para asignación grande.");
    return r;
}

/* Agrega una arena nueva. 1 si alcanza para la peticion, 0 si quedo chica (el PMM no tenia tanto contiguo; la arena
 * igual queda para peticiones menores), -1 sin memoria. */
static int heap_crecer(uint64_t bytes) {
    bytes = (bytes + 15ULL) & ~15ULL;
    uint64_t tamano_expansion = bytes + sizeof(bloque_heap_t) + TAMANO_PAGINA;
    tamano_expansion = (tamano_expansion + TAMANO_PAGINA - 1) & ~(TAMANO_PAGINA - 1);
    if (tamano_expansion < (2ULL * 1024ULL * 1024ULL)) tamano_expansion = 2ULL * 1024ULL * 1024ULL;
    uint64_t paginas_necesarias = tamano_expansion / TAMANO_PAGINA;

    uint64_t f = cerrojo_tomar_irq(&g_cerrojo_memoria);
    uint64_t primera_fisica = g_pila_marcos_libres ? pmm_asignar_pagina_fisica_sc() : 0;
    cerrojo_soltar_irq(&g_cerrojo_memoria, f);
    if (primera_fisica == 0) {
        huevo_agrietar("Heap: Memoria agotada al intentar expandir");
        return -1;
    }
    // La pila intrusiva suele entregar páginas en orden DESCENDENTE. Ambas
    // direcciones forman una arena válida; primera_fisica conserva el mínimo.
    uint64_t pagina_previa = primera_fisica, paginas_obtenidas = 1;
    for (uint64_t i = 1; i < paginas_necesarias; i++) {
        f = cerrojo_tomar_irq(&g_cerrojo_memoria);
        uint64_t nueva_pag = g_pila_marcos_libres ? pmm_asignar_pagina_fisica_sc() : 0;
        cerrojo_soltar_irq(&g_cerrojo_memoria, f);
        if (nueva_pag == 0) break;
        if (nueva_pag == pagina_previa + TAMANO_PAGINA) {
            pagina_previa = nueva_pag;
            paginas_obtenidas++;
        } else if (nueva_pag + TAMANO_PAGINA == primera_fisica) {
            primera_fisica = nueva_pag;
            paginas_obtenidas++;
        } else {
            pmm_liberar_pagina_fisica(nueva_pag);   /* No contigua: se devuelve y se usa lo juntado hasta aqui */
            break;
        }
    }
    f = cerrojo_tomar_irq(&g_cerrojo_memoria);
    heap_agregar_arena(FISICA_A_VIRTUAL(primera_fisica), paginas_obtenidas * TAMANO_PAGINA);
    cerrojo_soltar_irq(&g_cerrojo_memoria, f);
    return paginas_obtenidas * TAMANO_PAGINA >= bytes + sizeof(bloque_heap_t) ? 1 : 0;
}

static void *asignar_memoria_sc(uint64_t bytes) {
    if (bytes == 0) return NULL;

    // Alinear a 16 bytes para compatibilidad y rendimiento de CPU
    bytes = (bytes + 15ULL) & ~15ULL;
    if (bytes < 16) bytes = 16;

    bloque_heap_t *bloque = heap_buscar_libre(bytes);
    if (bloque != NULL) {
        libre_quitar(bloque);
        // Comprobar si vale la pena dividir el bloque (splitting)
        if (bloque->tamano_datos >= bytes + sizeof(bloque_heap_t) + 32) {
            bloque_heap_t *sobrante = (bloque_heap_t *)((uint8_t *)(bloque + 1) + bytes);
            sobrante->canario_inicio = CANARIO_BLOQUE_INICIO;
            sobrante->esta_libre = 1;
            sobrante->reservado = 0;
            sobrante->tamano_datos = bloque->tamano_datos - bytes - sizeof(bloque_heap_t);
            sobrante->siguiente = bloque->siguiente;
            sobrante->anterior = bloque;
            sobrante->canario_fin = CANARIO_BLOQUE_FIN;

            if (bloque->siguiente != NULL) {
                bloque->siguiente->anterior = sobrante;
            } else {
                g_heap_cola = sobrante;
            }
            bloque->siguiente = sobrante;
            bloque->tamano_datos = bytes;
            libre_poner(sobrante);
        }

        bloque->esta_libre = 0;
        g_heap_bytes_en_uso += bloque->tamano_datos;
        g_heap_bloques_activos++;

        return (void *)(bloque + 1);
    }
    /* H214: sin hueco, asignar_memoria hace crecer el monton fuera del cerrojo (heap_crecer) y vuelve a pedir */
    return NULL;
}

void *asignar_memoria_cero(uint64_t bytes) {
    void *ptr = asignar_memoria(bytes);
    if (ptr != NULL) {
        memset(ptr, 0, bytes);
    }
    return ptr;
}

static void liberar_memoria_sc(void *ptr);
void liberar_memoria(void *ptr) CON_CERROJO_VOID(liberar_memoria_sc(ptr))
static void liberar_memoria_sc(void *ptr) {
    if (ptr == NULL) return;

    bloque_heap_t *bloque = ((bloque_heap_t *)ptr) - 1;

    // --- VERIFICACIÓN DE INTEGRIDAD POR EL HUEVO DE LA ESTABILIDAD ---
    if (bloque->canario_inicio != CANARIO_BLOQUE_INICIO || bloque->canario_fin != CANARIO_BLOQUE_FIN) {
        serial_imprimir("[CORRUPCIÓN DE MEMORIA] Canario violado en liberación de bloque ");
        serial_imprimir_hex((uint64_t)bloque);
        serial_imprimir_linea("!");
        huevo_quebrar("Corrupción de memoria en Heap: Canario violado (Buffer Overflow)", (uint64_t)ptr, 0, 0);
    }

    if (bloque->esta_libre) {
        serial_imprimir("[DOBLE LIBERACIÓN] Se intentó liberar un bloque ya desocupado en ");
        serial_imprimir_hex((uint64_t)bloque);
        serial_imprimir_linea("!");
        huevo_quebrar("Doble liberación detectada en el Heap (Double Free)", (uint64_t)ptr, 0, 0);
    }

    bloque->esta_libre = 1;
    if (g_heap_bytes_en_uso >= bloque->tamano_datos) {
        g_heap_bytes_en_uso -= bloque->tamano_datos;
    } else {
        g_heap_bytes_en_uso = 0;
    }
    if (g_heap_bloques_activos > 0) g_heap_bloques_activos--;

    // Coalescing: Fusión con bloque siguiente si es libre y contiguo
    if (bloque->siguiente != NULL && bloque->siguiente->esta_libre) {
        uint8_t *fin_datos_bloque = (uint8_t *)(bloque + 1) + bloque->tamano_datos;
        if (fin_datos_bloque == (uint8_t *)bloque->siguiente) {
            libre_quitar(bloque->siguiente);
            bloque->tamano_datos += sizeof(bloque_heap_t) + bloque->siguiente->tamano_datos;
            bloque->siguiente = bloque->siguiente->siguiente;
            if (bloque->siguiente != NULL) {
                bloque->siguiente->anterior = bloque;
            } else {
                g_heap_cola = bloque;
            }
        }
    }

    // Coalescing: Fusión con bloque anterior si es libre y contiguo
    if (bloque->anterior != NULL && bloque->anterior->esta_libre) {
        uint8_t *fin_datos_ant = (uint8_t *)(bloque->anterior + 1) + bloque->anterior->tamano_datos;
        if (fin_datos_ant == (uint8_t *)bloque) {
            bloque_heap_t *ant = bloque->anterior;
            libre_quitar(ant);   /* Cambia de tamano: vuelve a su lista al final */
            ant->tamano_datos += sizeof(bloque_heap_t) + bloque->tamano_datos;
            ant->siguiente = bloque->siguiente;
            if (bloque->siguiente != NULL) {
                bloque->siguiente->anterior = ant;
            } else {
                g_heap_cola = ant;
            }
            bloque = ant;
        }
    }
    libre_poner(bloque);
}

void *reasignar_memoria(void *ptr, uint64_t nuevo_tamano) {
    if (ptr == NULL) {
        return asignar_memoria(nuevo_tamano);
    }
    if (nuevo_tamano == 0) {
        liberar_memoria(ptr);
        return NULL;
    }

    bloque_heap_t *bloque = ((bloque_heap_t *)ptr) - 1;
    if (bloque->canario_inicio != CANARIO_BLOQUE_INICIO || bloque->canario_fin != CANARIO_BLOQUE_FIN) {
        huevo_quebrar("Corrupción de memoria en Heap detectada en reasignar_memoria", (uint64_t)ptr, 0, 0);
    }

    if (bloque->tamano_datos >= nuevo_tamano) {
        return ptr;
    }

    void *nuevo_ptr = asignar_memoria(nuevo_tamano);
    if (nuevo_ptr == NULL) return NULL;

    memcpy(nuevo_ptr, ptr, bloque->tamano_datos);
    liberar_memoria(ptr);
    return nuevo_ptr;
}

/* H205: quien recorre la lista entera tambien toma el cerrojo; otra CPU podria estar partiendo o fundiendo bloques. */
static int memoria_verificar_integridad_sc(void);
int memoria_verificar_integridad(void) CON_CERROJO(int, memoria_verificar_integridad_sc())
static int memoria_verificar_integridad_sc(void) {
    if (!g_memoria_inicializada || g_heap_cabeza == NULL) return 1;

    bloque_heap_t *curr = g_heap_cabeza;
    while (curr != NULL) {
        if (curr->canario_inicio != CANARIO_BLOQUE_INICIO || curr->canario_fin != CANARIO_BLOQUE_FIN) {
            serial_imprimir("[CANARIO ROTO] Bloque corrompido en ");
            serial_imprimir_hex((uint64_t)curr);
            serial_imprimir_linea("");
            huevo_quebrar("Auditoría de integridad: Canario destruido en bloque de Heap", (uint64_t)curr, 0, 0);
            return 0;
        }

        if (curr->siguiente != NULL && curr->siguiente->anterior != curr) {
            serial_imprimir_linea("[ESTRUCTURA ROTA] Inconsistencia en lista doblemente enlazada de Heap.");
            huevo_quebrar("Inconsistencia en punteros de lista enlazada del Heap", (uint64_t)curr, 0, 0);
            return 0;
        }

        curr = curr->siguiente;
    }

    return 1;
}

static void memoria_obtener_estadisticas_sc(memoria_estadisticas_t *est);
void memoria_obtener_estadisticas(memoria_estadisticas_t *est) CON_CERROJO_VOID(memoria_obtener_estadisticas_sc(est))
static void memoria_obtener_estadisticas_sc(memoria_estadisticas_t *est) {
    if (est == NULL) return;

    est->ram_fisica_total     = g_ram_total_bytes;
    est->ram_fisica_usable    = g_ram_usable_bytes;
    est->paginas_totales      = g_paginas_totales;
    est->paginas_libres       = g_paginas_libres;
    est->paginas_en_uso       = g_paginas_en_uso;
    est->heap_capacidad_total = g_heap_capacidad_total;
    est->heap_bytes_en_uso    = g_heap_bytes_en_uso;
    est->heap_bloques_activos = g_heap_bloques_activos;

    /* H214: en H214 este recorrido (contar libres y auditar todos los canarios) tuvo el cerrojo 672 us con las
     * interrupciones apagadas: NekoTube lo pide por SYS_SENSORES para su barra. Los libres ya se cuentan al ponerlos
     * y quitarlos de las listas; la auditoria completa se repite como mucho una vez por segundo para estadisticas
     * (el huevo y quien llame a memoria_verificar_integridad siguen teniendo la auditoria al momento). */
    est->heap_bloques_libres  = g_heap_libres_n;
    static uint64_t auditoria_ms;
    static int auditoria;
    uint64_t ahora = tiempo_obtener_milisegundos();
    if (!auditoria_ms || ahora < auditoria_ms || ahora - auditoria_ms >= 1000) {
        auditoria = memoria_verificar_integridad();
        auditoria_ms = ahora ? ahora : 1;
    }
    est->canarios_intactos    = auditoria;
}

/* H150: mapa de los primeros bloques del heap para el panel de Configuracion: 0 libre, 1 en uso, 2 canario roto.
 * Solo mira; no quiebra el huevo (de eso se encarga memoria_verificar_integridad). Devuelve los bloques de la lista. */
static uint32_t memoria_mapa_heap_sc(uint8_t *mapa, uint32_t max, uint32_t *escritos);
uint32_t memoria_mapa_heap(uint8_t *mapa, uint32_t max, uint32_t *escritos) CON_CERROJO(uint32_t, memoria_mapa_heap_sc(mapa, max, escritos))
static uint32_t memoria_mapa_heap_sc(uint8_t *mapa, uint32_t max, uint32_t *escritos) {
    uint32_t n = 0, total = 0;
    for (bloque_heap_t *b = g_heap_cabeza; b != NULL && total < 1000000u; b = b->siguiente, total++) {
        if (n >= max) continue;
        int roto = b->canario_inicio != CANARIO_BLOQUE_INICIO || b->canario_fin != CANARIO_BLOQUE_FIN;
        mapa[n++] = roto ? 2 : b->esta_libre ? 0 : 1;
        if (roto) { total++; break; }   /* Con un canario roto, el enlace siguiente tampoco es de fiar */
    }
    if (escritos) *escritos = n;
    return total;
}

/* H144: sin recorrer el heap; lo usa cada peticion de memoria de un programa de Anillo 3 */
uint64_t pmm_paginas_libres(void) { return g_paginas_libres; }

uint64_t memoria_obtener_hhdm_offset(void) {
    return g_hhdm_offset;
}

// --- INICIALIZACIÓN COMPLETA DEL SUBSISTEMA DE MEMORIA ---

void memoria_iniciar(void) {
    if (g_peticion_mapa_memoria.response == NULL) {
        serial_imprimir_linea("[MEMORIA FATAL] Limine no entregó respuesta del mapa de memoria.");
        huevo_quebrar("Limine no entregó respuesta del mapa de memoria UEFI", 0, 0, 0);
    }

    if (g_peticion_hhdm.response == NULL) {
        serial_imprimir_linea("[MEMORIA FATAL] Limine no entregó respuesta de HHDM.");
        huevo_quebrar("Limine no entregó respuesta de desplazamiento HHDM", 0, 0, 0);
    }

    g_hhdm_offset = g_peticion_hhdm.response->offset;

    struct limine_memmap_response *mapa = g_peticion_mapa_memoria.response;
    uint64_t cantidad_entradas = mapa->entry_count;

    serial_imprimir("[HHDM Offset: ");
    serial_imprimir_hex(g_hhdm_offset);
    serial_imprimir("] [Entradas Memmap: ");
    serial_imprimir_dec(cantidad_entradas);
    serial_imprimir_linea("]");

    // Encontrar una región utilizable inicial para la arena del Heap (8 MiB)
    uint64_t arena_heap_fisica = 0;
    uint64_t arena_heap_tamano = TAMANO_ARENA_INICIAL;

    for (uint64_t i = 0; i < cantidad_entradas; i++) {
        struct limine_memmap_entry *e = mapa->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->base >= 0x100000) {
            uint64_t candidato = (e->base + 0xFFFULL) & ~0xFFFULL;
            if (candidato >= e->base && candidato <= e->base + e->length &&
                arena_heap_tamano <= e->base + e->length - candidato) {
                arena_heap_fisica = candidato;
                break;
            }
        }
    }

    // Encontrar región utilizable para la arena contigua de DMA físico. Una arena ampliada no pasa de
    // 1/8 de la RAM y queda bajo 4 GiB, donde la exigen el shim de la GPU y los controladores de 32 bits.
    uint64_t ram_usable = 0;
    for (uint64_t i = 0; i < cantidad_entradas; i++) {
        if (mapa->entries[i]->type == LIMINE_MEMMAP_USABLE) ram_usable += mapa->entries[i]->length;
    }
    for (uint64_t arena_dma_tamano = TAMANO_ARENA_DMA_MAX;
         arena_dma_tamano >= TAMANO_ARENA_DMA && g_arena_dma_fisica == 0;
         arena_dma_tamano /= 2) {
        int ampliada = arena_dma_tamano > TAMANO_ARENA_DMA;
        if (ampliada && arena_dma_tamano > ram_usable / 8) continue;

        for (uint64_t i = 0; i < cantidad_entradas; i++) {
            struct limine_memmap_entry *e = mapa->entries[i];
            if (e->type != LIMINE_MEMMAP_USABLE) continue;

            uint64_t base_candidata = (e->base + 0x1FFFFFULL) & ~0x1FFFFFULL;
            if (base_candidata < 0x200000) base_candidata = 0x200000;

            // Si solapa con la arena del heap, buscar inmediatamente después
            if (arena_heap_fisica != 0 &&
                base_candidata < arena_heap_fisica + arena_heap_tamano &&
                arena_heap_fisica < base_candidata + arena_dma_tamano) {
                base_candidata = ((arena_heap_fisica + arena_heap_tamano) + 0x1FFFFFULL) & ~0x1FFFFFULL;
            }

            if (ampliada && base_candidata + arena_dma_tamano > 0x100000000ULL) continue;

            if (base_candidata >= e->base && (base_candidata + arena_dma_tamano) <= (e->base + e->length)) {
                g_arena_dma_fisica = base_candidata;
                g_arena_dma_tamano = arena_dma_tamano;
                break;
            }
        }
    }

    // Arena de los objetos de la GPU: sin RAM por encima de 4 GiB, o con poca RAM, no hay.
    for (uint64_t i = 0; i < cantidad_entradas && TAMANO_ARENA_GPU <= ram_usable / 8; i++) {
        struct limine_memmap_entry *e = mapa->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t base_candidata = (e->base + 0x1FFFFFULL) & ~0x1FFFFFULL;
        if (base_candidata < 0x100000000ULL) base_candidata = 0x100000000ULL;
        if (base_candidata >= e->base && base_candidata + TAMANO_ARENA_GPU <= e->base + e->length) {
            g_arena_gpu_fisica = base_candidata;
            g_arena_gpu_tamano = TAMANO_ARENA_GPU;
            break;
        }
    }

    // Inicializar la pila intrusiva del PMM con todas las páginas libres
    for (uint64_t i = 0; i < cantidad_entradas; i++) {
        struct limine_memmap_entry *e = mapa->entries[i];
        g_ram_total_bytes += e->length;

        if (e->type == LIMINE_MEMMAP_USABLE) {
            g_ram_usable_bytes += e->length;

            uint64_t inicio = (e->base + 0xFFFULL) & ~0xFFFULL;
            uint64_t fin    = (e->base + e->length) & ~0xFFFULL;

            for (uint64_t p = inicio; p < fin; p += TAMANO_PAGINA) {
                // Preservar el primer MiB para estructuras de arquitectura y modo real
                if (p < 0x100000) continue;

                // Si esta página pertenece a la arena reservada para el Heap, omitirla del PMM
                if (arena_heap_fisica != 0 && p >= arena_heap_fisica && p < (arena_heap_fisica + arena_heap_tamano)) {
                    continue;
                }

                // Si esta página pertenece a la arena reservada para DMA contiguo, omitirla del PMM
                if (g_arena_dma_fisica != 0 && p >= g_arena_dma_fisica && p < (g_arena_dma_fisica + g_arena_dma_tamano)) {
                    continue;
                }

                if (g_arena_gpu_fisica != 0 && p >= g_arena_gpu_fisica && p < (g_arena_gpu_fisica + g_arena_gpu_tamano)) {
                    continue;
                }

                // Apilar marco físico libre en la lista intrusiva
                uint64_t *frame_virt = (uint64_t *)FISICA_A_VIRTUAL(p);
                *frame_virt = g_pila_marcos_libres;
                g_pila_marcos_libres = p;
                g_paginas_libres++;
            }
        }
    }

    g_paginas_totales = g_paginas_libres;
    g_paginas_en_uso  = 0;

    // Inicializar el Heap del Kernel en la arena reservada
    if (arena_heap_fisica != 0) {
        void *arena_virt = FISICA_A_VIRTUAL(arena_heap_fisica);
        heap_agregar_arena(arena_virt, arena_heap_tamano);
    } else {
        // En caso extremo de no hallar una región contigua inicial grande, asignar página a página
        void *arena_pag = pmm_asignar_pagina_virtual();
        if (arena_pag != NULL) {
            heap_agregar_arena(arena_pag, TAMANO_PAGINA);
        }
    }

    // Construir el mapa después del heap: así sus metadatos no se asignan
    // recursivamente desde el PMM antes de conocer los marcos libres.
    for (uint64_t i = 0; i < cantidad_entradas; i++) {
        struct limine_memmap_entry *e = mapa->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t inicio = (e->base + 0xFFFULL) & ~0xFFFULL;
        uint64_t fin = (e->base + e->length) & ~0xFFFULL;
        if (fin <= inicio) continue;
        if (g_pmm_num_rangos == PMM_MAX_RANGOS) {
            huevo_quebrar("PMM: demasiados rangos físicos", i, 0, 0);
            return;
        }
        struct pmm_rango_estado *r = &g_pmm_rangos[g_pmm_num_rangos++];
        r->inicio = inicio;
        r->paginas = (fin - inicio) / TAMANO_PAGINA;
        r->estado = (uint8_t *)asignar_memoria_cero((r->paginas + 3) / 4);
        if (!r->estado) {
            huevo_quebrar("PMM: sin memoria para estados de marcos", inicio, 0, 0);
            return;
        }
    }
    uint64_t marco = g_pila_marcos_libres;
    for (uint64_t i = 0; marco && i < g_paginas_libres; i++) {
        if (pmm_estado_leer(marco) != 0) {
            huevo_quebrar("PMM: pila inicial corrupta", marco, 0, 0);
            return;
        }
        pmm_estado_escribir(marco, 1);
        marco = *(uint64_t *)FISICA_A_VIRTUAL(marco);
    }
    if (marco) {
        huevo_quebrar("PMM: ciclo en pila inicial", marco, 0, 0);
        return;
    }
    g_pmm_estado_listo = 1;
    g_memoria_inicializada = 1;

    serial_imprimir("[RAM Total: ");
    serial_imprimir_dec(g_ram_total_bytes / (1024 * 1024));
    serial_imprimir(" MiB] [RAM Usable: ");
    serial_imprimir_dec(g_ram_usable_bytes / (1024 * 1024));
    serial_imprimir(" MiB] [Páginas PMM: ");
    serial_imprimir_dec(g_paginas_libres);
    serial_imprimir("] [Heap Inicial: ");
    serial_imprimir_dec(g_heap_capacidad_total / 1024);
    serial_imprimir_linea(" KiB]");
}

uint64_t memoria_obtener_dma_arena(uint64_t *tamano_out) {
    if (tamano_out) *tamano_out = g_arena_dma_tamano;
    return g_arena_dma_fisica;
}

uint64_t memoria_obtener_gpu_arena(uint64_t *tamano_out) {
    if (tamano_out) *tamano_out = g_arena_gpu_tamano;
    return g_arena_gpu_fisica;
}

uint64_t pmm_asignar_bloque_contiguo(uint32_t num_paginas, uint64_t alineacion) {
    if (num_paginas == 0) return 0;
    uint64_t phys = 0;
    void *v = dma_asignar_bufer_contiguo((uint64_t)num_paginas * TAMANO_PAGINA, alineacion, &phys);
    (void)v;
    return phys;
}

void pmm_liberar_bloque_contiguo(uint64_t dir_fisica, uint32_t num_paginas) {
    if (dir_fisica == 0 || num_paginas == 0) return;
    if (dma_gpu_es_suya(dir_fisica)) { dma_gpu_liberar(dir_fisica, num_paginas); return; }
    void *v = (void *)FISICA_A_VIRTUAL(dir_fisica);
    dma_liberar_bufer_contiguo(v, dir_fisica, (uint64_t)num_paginas * TAMANO_PAGINA);
}
