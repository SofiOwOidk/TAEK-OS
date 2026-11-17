#ifndef TAEK_ACPI_BATERIA_H
#define TAEK_ACPI_BATERIA_H
#include <stdint.h>
#include "../../arquitectura/x86_64/anillo3_abi.h"
void bateria_comando(void);
void acpi_comando(const char *argumento);
/* pct -1 si no hay dato. estado: bit 0 descargando, bit 1 cargando, bit 2 critica. cargador: -1 desconocido. */
int bateria_resumen(int32_t *pct, int32_t *estado, int32_t *cargador);
void bateria_detalle(struct taek_equipo *t);

/* El interprete ya cargado con DSDT y SSDT, para otros controladores (el trackpad por I2C). NULL si no se pudo. */
#include "aml.h"
aml *acpi_interprete(void);
/* Texto de un _HID o _CID: cadena tal cual o codigo EISA. 1 si hay texto. */
int acpi_id_texto(const aml_valor *v, char *s, unsigned tam);
#endif
