#ifndef BASE_PAGINACION_H
#define BASE_PAGINACION_H

#include <stdint.h>
#include <stddef.h>

// Banderas de atributos de página en arquitectura x86_64
#define PAGINA_PRESENTE        (1ULL << 0)   // Entrada válida en memoria física
#define PAGINA_ESCRITURA       (1ULL << 1)   // Lectura y Escritura (0 = Solo Lectura)
#define PAGINA_USUARIO         (1ULL << 2)   // Accesible desde Anillo 3 (0 = Solo Supervisor Anillo 0)
#define PAGINA_ESCRITURA_DIR   (1ULL << 3)   // Write-Through (PWT)
#define PAGINA_SIN_CACHE       (1ULL << 4)   // Cache Disable / MMIO (PCD)
#define PAGINA_ACCEDIDA        (1ULL << 5)   // Bit de acceso por CPU
#define PAGINA_SUCIA           (1ULL << 6)   // Bit sucio (escritura realizada)
#define PAGINA_GIGANTE         (1ULL << 7)   // Página gigante (2 MiB en PD, 1 GiB en PDPT)
#define PAGINA_GLOBAL          (1ULL << 8)   // No invalidar en cambios de CR3
#define PAGINA_NO_EJECUTABLE   (1ULL << 63)  // Bit NX (mitigación de inyección de código)

// Atributos de conveniencia para controladores y memoria
#define PAGINA_ATRIBUTOS_KERNEL (PAGINA_PRESENTE | PAGINA_ESCRITURA)
#define PAGINA_ATRIBUTOS_MMIO   (PAGINA_PRESENTE | PAGINA_ESCRITURA | PAGINA_SIN_CACHE)
#define PAGINA_ATRIBUTOS_USUARIO (PAGINA_PRESENTE | PAGINA_ESCRITURA | PAGINA_USUARIO)

// Máscaras de extracción de direcciones
#define MASCARA_DIRECCION_FISICA 0x000FFFFFFFFFF000ULL

// Número de entradas por tabla en x86_64 (4 niveles)
#define ENTRADAS_POR_TABLA 512

typedef struct {
    uint64_t entradas[ENTRADAS_POR_TABLA];
} __attribute__((aligned(4096))) tabla_paginacion_t;

// --- API DE MEMORIA VIRTUAL EN ESPAÑOL (ANILLO 0) ---

// Inicializa el árbol PML4 soberano de TAEK OS y carga CR3
void paginacion_iniciar(void);

// Mapea una página virtual de 4 KiB a un marco físico con los atributos especificados
int  paginacion_mapear(uint64_t dir_virtual, uint64_t dir_fisica, uint64_t banderas);

// Desmapea una página virtual y limpia su entrada en la tabla correspondiente
int  paginacion_desmapear(uint64_t dir_virtual);

// Obtiene la dirección física asociada a una dirección virtual mediante recorrido de 4 niveles
uint64_t paginacion_obtener_fisica(uint64_t dir_virtual);

// Invalida la entrada correspondiente en el TLB de la CPU (instrucción invlpg)
void paginacion_invalidar_tlb(uint64_t dir_virtual);

// Obtiene el registro de control CR3 actual de la CPU
uint64_t paginacion_obtener_cr3(void);

// Obtiene la dirección física del PML4 soberano activo de TAEK OS
uint64_t paginacion_obtener_pml4_activo(void);

// Ejecuta autodiagnóstico integral del VMM (mapeo, escritura, traducción, desmapeo)
int  paginacion_ejecutar_autodiagnostico(void);

#endif // BASE_PAGINACION_H
