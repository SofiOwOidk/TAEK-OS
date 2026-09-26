#ifndef TAEK_VERSION_H
#define TAEK_VERSION_H

#define TAEK_VERSION_NOMBRE      "TAEK OS"
#define TAEK_VERSION_CODENAME    "TelAvivEpsteinKirkOS"
#define TAEK_VERSION_MAYOR       0
#define TAEK_VERSION_MENOR       1
#define TAEK_VERSION_PARCHE      0
#define TAEK_VERSION_STRING      "v0.1.0"
#define TAEK_HITO_ACTUAL         "Hito 61"

#ifndef COMPILACION_FECHA
#define COMPILACION_FECHA __DATE__
#endif

#ifndef COMPILACION_HORA
#define COMPILACION_HORA __TIME__
#endif

const char *taek_obtener_version(void);
const char *taek_obtener_fecha_compilacion(void);
const char *taek_obtener_hora_compilacion(void);
const char *taek_obtener_hito(void);

#endif
