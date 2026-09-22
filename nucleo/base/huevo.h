#ifndef BASE_HUEVO_H
#define BASE_HUEVO_H

#include <stdint.h>

#define HUEVO_MAGICO   0xEE66B007
#define HUEVO_CANARIO  0xDEADBEEFCAFECAFEULL

typedef enum {
    HUEVO_INTACTO = 0,
    HUEVO_FISURA,
    HUEVO_AGRIETADO,
    HUEVO_QUEBRADO
} estado_huevo_t;

typedef struct {
    uint32_t       magico;
    uint32_t       salud;          // 100% hasta 0%
    estado_huevo_t estado;
    const char    *etapa_actual;
    uint64_t       canario;
} huevo_estabilidad_t;

void huevo_iniciar(void);
void huevo_etapa(const char *nombre_etapa);
void huevo_etapa_ok(void);
void huevo_verificar(void);
void huevo_agrietar(const char *motivo);
void huevo_quebrar(const char *motivo_fatal, uint64_t rip, uint64_t rsp, uint64_t codigo_error) __attribute__((noreturn));

#endif // BASE_HUEVO_H
