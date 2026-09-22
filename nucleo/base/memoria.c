#include "memoria.h"
#include "huevo.h"
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

// Estructura de Bloque de Heap del Kernel
typedef struct bloque_heap {
    uint64_t canario_inicio;     // CANARIO_BLOQUE_INICIO (0x7AEECC05ULL)
    uint32_t esta_libre;         // 1 = libre, 0 = en uso
    uint32_t reservado;          // Relleno de alineación
    uint64_t tamano_datos;       // Tamaño útil en bytes para el usuario
    struct bloque_heap *siguiente;
    struct bloque_heap *anterior;
    uint64_t canario_fin;        // CANARIO_BLOQUE_FIN (0xCAFEBABEDEAD1000ULL)
} __attribute__((aligned(16))) bloque_heap_t;

// Estado del Heap del Kernel
static bloque_heap_t *g_heap_cabeza = NULL;
static uint64_t       g_heap_capacidad_total = 0;
static uint64_t       g_heap_bytes_en_uso = 0;
static uint64_t       g_heap_bloques_activos = 0;
static int            g_memoria_inicializada = 0;

// Tamaño inicial asignado al Heap del kernel (8 MiB)
#define TAMANO_ARENA_INICIAL (8ULL * 1024ULL * 1024ULL)

// --- FUNCIONES BÁSICAS DE COPIA Y RELLENO (FREESTANDING) ---

void *memset(void *dest, int c, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    uint8_t v = (uint8_t)c;
    for (size_t i = 0; i < n; i++) {
        d[i] = v;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
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

// --- GESTOR DE MARCOS DE PÁGINA FÍSICA (PMM - 4 KiB) ---

uint64_t pmm_asignar_pagina_fisica(void) {
    if (g_pila_marcos_libres == 0) {
        serial_imprimir_linea("[PMM FATAL] ¡Memoria física agotada! Sin marcos libres.");
        huevo_agrietar("PMM: Memoria física agotada (Out of Memory)");
        return 0;
    }

    uint64_t phys = g_pila_marcos_libres;
    uint64_t *frame_virt = (uint64_t *)FISICA_A_VIRTUAL(phys);

    // Desapilar el marco libre
    g_pila_marcos_libres = *frame_virt;

    if (g_paginas_libres > 0) g_paginas_libres--;
    g_paginas_en_uso++;

    // Limpiar el contenido del marco por seguridad
    memset(frame_virt, 0, TAMANO_PAGINA);

    return phys;
}

void *pmm_asignar_pagina_virtual(void) {
    uint64_t phys = pmm_asignar_pagina_fisica();
    if (phys == 0) return NULL;
    return FISICA_A_VIRTUAL(phys);
}

void pmm_liberar_pagina_fisica(uint64_t phys) {
    if (phys == 0 || (phys & 0xFFFULL) != 0) {
        return;
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
        bloque_heap_t *ultimo = g_heap_cabeza;
        while (ultimo->siguiente != NULL) {
            ultimo = ultimo->siguiente;
        }
        ultimo->siguiente = nuevo_bloque;
        nuevo_bloque->anterior = ultimo;
    }
}

void *asignar_memoria(uint64_t bytes) {
    if (bytes == 0) return NULL;

    // Alinear a 16 bytes para compatibilidad y rendimiento de CPU
    bytes = (bytes + 15ULL) & ~15ULL;
    if (bytes < 16) bytes = 16;

    bloque_heap_t *bloque = g_heap_cabeza;

    while (bloque != NULL) {
        // Comprobación de integridad de canarios durante el recorrido
        if (bloque->canario_inicio != CANARIO_BLOQUE_INICIO || bloque->canario_fin != CANARIO_BLOQUE_FIN) {
            serial_imprimir("[CORRUPCIÓN DETECTADA] Bloque de Heap dañado en ");
            serial_imprimir_hex((uint64_t)bloque);
            serial_imprimir_linea("!");
            huevo_quebrar("Corrupción de memoria en Heap: Canario de bloque destruido", (uint64_t)bloque, 0, 0);
        }

        if (bloque->esta_libre && bloque->tamano_datos >= bytes) {
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
                }
                bloque->siguiente = sobrante;
                bloque->tamano_datos = bytes;
            }

            bloque->esta_libre = 0;
            g_heap_bytes_en_uso += bloque->tamano_datos;
            g_heap_bloques_activos++;

            return (void *)(bloque + 1);
        }

        bloque = bloque->siguiente;
    }

    // Si no hay espacio suficiente en el Heap actual, expandir asignando nuevas páginas
    uint64_t tamano_expansion = bytes + sizeof(bloque_heap_t) + TAMANO_PAGINA;
    tamano_expansion = (tamano_expansion + TAMANO_PAGINA - 1) & ~(TAMANO_PAGINA - 1);
    if (tamano_expansion < (2ULL * 1024ULL * 1024ULL)) {
        tamano_expansion = (2ULL * 1024ULL * 1024ULL); // Mínimo 2 MiB de expansión
    }

    uint64_t paginas_necesarias = tamano_expansion / TAMANO_PAGINA;
    uint64_t primera_fisica = pmm_asignar_pagina_fisica();
    if (primera_fisica == 0) {
        huevo_agrietar("Heap: Memoria agotada al intentar expandir");
        return NULL;
    }

    // Para la expansión necesitamos un buffer contiguo. Si asignamos páginas una a una,
    // en su mayoría serán contiguas en el inicio. Verificamos contigüidad:
    uint64_t pagina_previa = primera_fisica;
    uint64_t paginas_obtenidas = 1;

    for (uint64_t i = 1; i < paginas_necesarias; i++) {
        uint64_t nueva_pag = pmm_asignar_pagina_fisica();
        if (nueva_pag == 0) break;
        if (nueva_pag == pagina_previa + TAMANO_PAGINA) {
            pagina_previa = nueva_pag;
            paginas_obtenidas++;
        } else {
            // No contigua: devolvemos la página y nos quedamos con lo obtenido hasta aquí
            pmm_liberar_pagina_fisica(nueva_pag);
            break;
        }
    }

    if (paginas_obtenidas * TAMANO_PAGINA >= bytes + sizeof(bloque_heap_t)) {
        heap_agregar_arena(FISICA_A_VIRTUAL(primera_fisica), paginas_obtenidas * TAMANO_PAGINA);
        // Reintentar recursivamente una vez con la nueva arena incorporada
        return asignar_memoria(bytes);
    }

    serial_imprimir_linea("[HEAP ADVERTENCIA] No fue posible conseguir arena contigua suficiente para asignación grande.");
    return NULL;
}

void *asignar_memoria_cero(uint64_t bytes) {
    void *ptr = asignar_memoria(bytes);
    if (ptr != NULL) {
        memset(ptr, 0, bytes);
    }
    return ptr;
}

void liberar_memoria(void *ptr) {
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
            bloque->tamano_datos += sizeof(bloque_heap_t) + bloque->siguiente->tamano_datos;
            bloque->siguiente = bloque->siguiente->siguiente;
            if (bloque->siguiente != NULL) {
                bloque->siguiente->anterior = bloque;
            }
        }
    }

    // Coalescing: Fusión con bloque anterior si es libre y contiguo
    if (bloque->anterior != NULL && bloque->anterior->esta_libre) {
        uint8_t *fin_datos_ant = (uint8_t *)(bloque->anterior + 1) + bloque->anterior->tamano_datos;
        if (fin_datos_ant == (uint8_t *)bloque) {
            bloque->anterior->tamano_datos += sizeof(bloque_heap_t) + bloque->tamano_datos;
            bloque->anterior->siguiente = bloque->siguiente;
            if (bloque->siguiente != NULL) {
                bloque->siguiente->anterior = bloque->anterior;
            }
        }
    }
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

int memoria_verificar_integridad(void) {
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

void memoria_obtener_estadisticas(memoria_estadisticas_t *est) {
    if (est == NULL) return;

    est->ram_fisica_total     = g_ram_total_bytes;
    est->ram_fisica_usable    = g_ram_usable_bytes;
    est->paginas_totales      = g_paginas_totales;
    est->paginas_libres       = g_paginas_libres;
    est->paginas_en_uso       = g_paginas_en_uso;
    est->heap_capacidad_total = g_heap_capacidad_total;
    est->heap_bytes_en_uso    = g_heap_bytes_en_uso;
    est->heap_bloques_activos = g_heap_bloques_activos;

    // Calcular bloques libres recorriendo el Heap
    uint64_t libres = 0;
    bloque_heap_t *b = g_heap_cabeza;
    while (b != NULL) {
        if (b->esta_libre) libres++;
        b = b->siguiente;
    }
    est->heap_bloques_libres  = libres;
    est->canarios_intactos    = memoria_verificar_integridad();
}

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
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= (arena_heap_tamano + 0x100000) && e->base >= 0x100000) {
            arena_heap_fisica = (e->base + 0xFFFULL) & ~0xFFFULL;
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
