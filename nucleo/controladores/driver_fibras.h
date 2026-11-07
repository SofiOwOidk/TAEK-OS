#ifndef TAEK_DRIVER_FIBRAS_H
#define TAEK_DRIVER_FIBRAS_H

/*
 * driver_fibras.h -- Servicio residente de fibras cooperativas (TaekOS, ABI 1).
 *
 * Alcance y contrato
 * ------------------
 * Motor privado de UN propietario de modulo RAM ABI 1 en BSP. No usa el
 * planificador global (g_tabla_tareas / i915_tarea_crear / i915_tareas_iniciar)
 * ni define homonimos de task_struct/mutex de Linux. Reutiliza unicamente el
 * primitivo probado de cambio de contexto real (FX/FS/IF):
 *
 *     i915_contexto_preparar()  /  i915_cambiar_contexto()  /  struct i915_contexto_marco
 *
 * declarado en nucleo/compatibilidad/linux_i915/tareas.h (H120). Este header
 * es opaco: no expone la representacion interna ni los tipos del shim Linux.
 *
 * Modelo
 * ------
 *  - Un motor estatico con FIBRA_SLOTS ranuras.
 *  - Cada fibra tiene 64 KiB de pila y su propio TLS estatico (TP puesto en
 *    fs_base sobre el marco). En TAEK_HOST_TEST el primitivo no toca FS real.
 *  - Los handles son uint64_t distintos de cero y llevan generacion; al reciclar
 *    una ranura se rechazan handles antiguos. Si la generacion se agota se deja
 *    de reutilizar la ranura en vez de admitir un handle repetido.
 *  - Ejecutar() es de la raiz: round robin justo sobre ejecutables, a lo sumo
 *    "pasos" activaciones, y vuelve a raiz tras cada yield/bloqueo/retorno.
 *  - No se declara soporte AP/SMP/IRQ por callback: permitido() debe rechazar
 *    cualquier contexto no admitido. Aun asi el motor deshabilita IRQ al nativo
 *    alrededor de transiciones y conmutacion, y respeta el IF de cada contexto.
 *
 * Autorizacion: componente experimental de TaekOS/i915. No acredita GPU ni
 * cierra M07/M08. La integracion en driver_abi/driver_ram es externa.
 */

#include <stddef.h>
#include <stdint.h>

#define FIBRA_SLOTS             24u
#define FIBRA_PILA_BYTES        (64u * 1024u)
#define FIBRA_HANDLE_NULO       UINT64_C(0)
#define FIBRA_CALLBACKS_VERSION 1u

typedef uint64_t fibra_handle_t;
typedef void (*fibra_func)(void *ctx);
typedef void (*fibra_observador_fn)(fibra_handle_t destino,void *ctx);

/* Callbacks del propietario. asignar/liberar gestionan la pila de 64 KiB;
 * tls_crear/tls_destruir gestionan la imagen TLS estatica (devolviendo el TP).
 * liberar/tls_destruir devuelven 0 solo si el recurso quedo realmente liberado;
 * cualquier otro valor obliga a conservarlo para reintento (sin doble free). */
typedef void *(*fibra_asignar_fn)(size_t bytes);
typedef int   (*fibra_liberar_fn)(void *bloque);
typedef void *(*fibra_tls_crear_fn)(void);
typedef int   (*fibra_tls_destruir_fn)(void *tp);
/* Devuelve distinto de cero solo si el contexto actual (CPU/estado) se admite. */
typedef int   (*fibra_permitido_fn)(void);

struct fibra_callbacks {
    uint32_t             bytes;   /* >= FIBRA_CALLBACKS_BYTES */
    uint32_t             version; /* == FIBRA_CALLBACKS_VERSION */
    fibra_asignar_fn     asignar;
    fibra_liberar_fn     liberar;
    fibra_tls_crear_fn   tls_crear;
    fibra_tls_destruir_fn tls_destruir;
    fibra_permitido_fn   permitido;
};

#define FIBRA_CALLBACKS_BYTES \
    ((uint32_t)(offsetof(struct fibra_callbacks, permitido) + sizeof(fibra_permitido_fn)))

enum fibra_estado {
    FIBRA_LIBRE = 0,   /* ranura sin fibra */
    FIBRA_NUEVA,       /* creada, nunca entro a ejecutarse */
    FIBRA_LISTA,       /* ejecutable (nueva en cola o reanudada tras yield) */
    FIBRA_EJECUTANDO,  /* en CPU */
    FIBRA_BLOQUEADA,   /* esperando despertar() */
    FIBRA_MUERTA,      /* fn retorno; pila/TLS aun no liberados */
    FIBRA_PARCIAL      /* creacion/liberacion parcial; nunca ejecutable */
};

/* Codigos de retorno. 0 = exito; mezcla de errores negativos y conteos >= 0. */
#define FIBRA_OK              0
#define FIBRA_E_ARGUMENTOS   -1
#define FIBRA_E_NO_INICIADO  -2
#define FIBRA_E_YA_INICIADO  -3
#define FIBRA_E_NO_PERMITIDO -4
#define FIBRA_E_AGOTADO      -5
#define FIBRA_E_MEMORIA      -6
#define FIBRA_E_TLS          -7
#define FIBRA_E_CONTEXTO     -8
#define FIBRA_E_HANDLE       -9
#define FIBRA_E_SOLO_RAIZ    -10
#define FIBRA_E_SOLO_FIBRA   -11
#define FIBRA_E_ESTADO       -12
#define FIBRA_E_REENTRANTE   -13
#define FIBRA_E_PENDIENTE    -14
#define FIBRA_E_OCUPADO      -15

struct fibra_estadisticas {
    uint64_t creadas, liberadas, terminadas;
    uint64_t cambios, yields, bloqueos, despertares;
    uint64_t pilas_liberadas, tls_liberados;
    uint64_t fallos_pila, fallos_tls, fallos;
    uint32_t slots_libres, slots_ocupados, slots_pendientes;
    uint32_t generacion_maxima;
};

/* Inicializa el motor estatico. Valida los callbacks antes de mutar y rechaza
 * reinicializar cuando queda cualquier recurso vivo. permitido() debe aprobar
 * el contexto. Devuelve FIBRA_OK o un error negativo. */
int fibra_motor_iniciar(const struct fibra_callbacks *cb);
/* Solo raiz sin fibras/recursos vivos. Notifica raiz al registrar/desregistrar
 * y cada destino antes de conmutar, con CLI en nativo. Callback infalible,
 * breve, sin cambiar FS/IF, sin bloqueo/asignaciones/APIs mutantes. Se preserva
 * FX alrededor del callback. No acredita coherencia de pila/FS ante NMI. */
int fibra_motor_observar(fibra_observador_fn fn,void *ctx);

/* Detiene el motor: exige raiz y sin ejecucion activa; recolecta ranuras
 * MUERTA/NUEVA/PARCIAL. Si queda cualquier fibra viva (LISTA/BLOQUEADA) o un
 * recurso que el backend no libero, devuelve FIBRA_E_PENDIENTE y permite retry. */
int fibra_motor_detener(void);

/* Reintenta liberar recursos pendientes (TLS/pila) sin abandonar ranuras.
 * Devuelve el numero de ranuras que aun quedan pendientes (>0 = retry). */
int fibra_recolectar_parciales(void);

/* Copia las estadisticas visibles del motor. out puede ser NULL. */
void fibra_motor_estadisticas(struct fibra_estadisticas *out);

/* Crea una fibra (pila 64 KiB + TLS). Solo desde raiz, con permitido() a favor.
 * En exito escribe out_handle != 0. Rollback completo ante fallo de pila/TLS/
 * contexto; si un free de rollback falla, la ranura queda PARCIAL y se recupera
 * con fibra_recolectar_parciales()/fibra_motor_detener(). */
int fibra_crear(fibra_func fn, void *ctx, fibra_handle_t *out_handle);

/* Ejecuta hasta "pasos" activaciones en round robin desde raiz. Rechaza llamada
 * anidada desde una fibra antes de mutar. Devuelve el numero de activaciones
 * (>=0) o un error negativo. */
int fibra_ejecutar(int pasos);

/* Handle en CPU, o 0 en raiz/fuera del contexto admitido/no iniciado. */
fibra_handle_t fibra_actual(void);

/* Cede la CPU a raiz conservando pila/FX/FS/IF. Solo desde fibra. */
int fibra_yield(void);

/* Bloquea la fibra y cede a raiz. Un despertar() previo se consume como un
 * unico token coalescido y evita el bloqueo, de modo que no se pierde el wake. */
int fibra_bloquear(void);

/* Hace ejecutable una fibra bloqueada, o deja un token pendiente si aun no se
 * bloqueo. Un solo token coalescido por fibra. */
int fibra_despertar(fibra_handle_t h);

/* Libera una fibra DEAD o NUEVA (nunca entro). Rechaza fibra activa, bloqueada
 * o suspendida (LISTA) sin tocar pila/TLS. Solo desde raiz. Si el backend falla
 * al liberar, conserva lo pendiente y admite retry sin doble free. */
int fibra_liberar(fibra_handle_t h);

/* Estado de la fibra o error negativo si el handle es invalido/antiguo. */
int fibra_estado(fibra_handle_t h);

#ifdef TAEK_HOST_TEST
/* Gancho exclusivo de pruebas host: fija la generacion de una ranura libre para
 * poder ejercitar el rechazo por agotamiento sin miles de ciclos. */
void fibra_prueba_forzar_generacion(unsigned slot, uint32_t generacion);
#endif

#endif /* TAEK_DRIVER_FIBRAS_H */
