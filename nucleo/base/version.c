#include "version.h"

const char *taek_obtener_version(void) {
    return TAEK_VERSION_STRING;
}

const char *taek_obtener_fecha_compilacion(void) {
    return COMPILACION_FECHA;
}

const char *taek_obtener_hora_compilacion(void) {
    return COMPILACION_HORA;
}

const char *taek_obtener_hito(void) {
    return TAEK_HITO_ACTUAL;
}
