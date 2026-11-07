/* Interprete AML pequeno para leer datos del firmware (bateria, cargador, temperatura) sin tocar el hardware.
 *
 * Una escritura a memoria del firmware se queda en una copia en sombra que luego se lee de vuelta, y una escritura a puertos o a PCI
 * detiene el metodo con su motivo. Al controlador embebido solo se escribe si quien evalua lo permite, para los metodos de consulta
 * que el firmware define asi (muchos eligen con un registro que bateria leer). Todo se comprueba contra los limites de la tabla: una
 * tabla rota da un error, no un fallo del kernel. Sin dependencias del kernel: se prueba en el PC con las mismas tablas. */
#ifndef TAEK_ACPI_AML_H
#define TAEK_ACPI_AML_H
#include <stdint.h>
#include <stddef.h>

struct aml_host {
    void *(*reservar)(size_t bytes);
    int (*leer_mem)(uint64_t fisica, unsigned bytes, uint64_t *v);
    int (*leer_io)(uint16_t puerto, unsigned bytes, uint64_t *v);
    int (*leer_pci)(uint8_t bus, uint8_t dispositivo, uint8_t funcion, uint16_t reg, unsigned bytes, uint64_t *v);
    int (*leer_ec)(uint8_t reg, uint8_t *v);
    int (*escribir_ec)(uint8_t reg, uint8_t v);
    uint64_t (*ms)(void);
    void (*esperar_ms)(unsigned ms);
};

enum aml_tipo { AML_NADA, AML_ENTERO, AML_CADENA, AML_BUFFER, AML_PAQUETE, AML_OBJETO, AML_REF, AML_REF_BYTE, AML_RUTA };
typedef struct aml_valor {
    uint8_t tipo;
    uint32_t n;                 /* Bytes o elementos */
    union {
        uint64_t entero;
        char *cadena;
        uint8_t *buffer;
        struct aml_valor *paquete;
        struct aml_valor *ref;
        uint8_t *ref_byte;
        const uint8_t *ruta;
        int nodo;
    } v;
    int ruta_ambito;
} aml_valor;

typedef struct aml aml;

/* NULL si no hay memoria. Tres arenas: nodos, datos que perduran y datos de una evaluacion. */
aml *aml_crear(const struct aml_host *host);
/* Tabla DSDT o SSDT completa, con su cabecera. 0 o negativo; aml_motivo dice por que. */
int aml_cargar(aml *a, const uint8_t *tabla, uint32_t bytes);
const char *aml_motivo(const aml *a);
int aml_nodos(const aml *a);

/* Busca desde la raiz por ruta absoluta ("\\_SB.PCI0"). -1 si no existe. */
int aml_buscar(const aml *a, const char *ruta);
/* Hijo directo de nodo con ese nombre de cuatro letras, sin subir de ambito. */
int aml_hijo(const aml *a, int nodo, const char *nombre);
/* Recorre el espacio de nombres: siguiente dispositivo con _HID igual, a partir de desde (-1 para empezar). */
int aml_siguiente_hid(aml *a, int desde, uint32_t eisa, const char *cadena);
/* Siguiente nodo con ese nombre tras desde, en cualquier ambito. */
int aml_siguiente_nombre(const aml *a, int desde, const char *nombre);
void aml_ruta(const aml *a, int nodo, char *salida, size_t bytes);

/* Evalua un objeto o metodo. Los valores devueltos viven hasta la siguiente evaluacion. 0 o negativo. */
int aml_evaluar(aml *a, int nodo, const aml_valor *args, unsigned nargs, aml_valor *salida);
/* _STA: si no existe, el dispositivo cuenta como presente (6.3.7). -1 si no se pudo leer: aml_motivo dice por que. */
int64_t aml_sta(aml *a, int nodo);

/* Desactivado por defecto. Solo alrededor de _STA, _PSR, _BST, _BIF y _BIX: nunca para _REG ni otros metodos que configuran el equipo. */
void aml_permitir_escritura_ec(aml *a, int permitir);

struct aml_estadisticas { uint64_t lecturas_ec, escrituras_ec, lecturas_mem, lecturas_io, lecturas_pci, escrituras_sombra, pasos; };
void aml_estadisticas_leer(const aml *a, struct aml_estadisticas *e);
#endif
