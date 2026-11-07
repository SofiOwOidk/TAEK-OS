#include "upstream_mm.h"
#include "cpu_runtime.h"
#include "base/trabajos.h"
#include "controladores/consola.h"
#include "arquitectura/x86_64/serial.h"
static unsigned avisos;
static unsigned char ocupado;
/* drm_mm emite aqui el mensaje literal de takedown no limpio. El adaptador
 * __WARN_FLAGS conserva el valor condicional y registra el sitio en TaekOS. */
void __warn_printk(const char *formato,...){
    serial_imprimir("[UPSTREAM_MM WARN] ");serial_imprimir_linea(formato);
}
void taek_linux_warn_site(const char *archivo,unsigned linea,unsigned flags){
    __atomic_fetch_add(&avisos,1,__ATOMIC_RELAXED);
    serial_imprimir("[UPSTREAM_MM WARN SITE] ");serial_imprimir(archivo);
    serial_imprimir(" linea=");serial_imprimir_dec(linea);
    serial_imprimir(" flags=");serial_imprimir_hex(flags);serial_imprimir_linea("");
}
int intel_diagnostico_upstream_mm(void){
    if(!trabajos_es_coordinador() || trabajos_en_curso() || i915_scheduler_cpu_actual()!=0 ||
       __atomic_test_and_set(&ocupado,__ATOMIC_ACQUIRE))return -1;
    unsigned antes=__atomic_load_n(&avisos,__ATOMIC_RELAXED);
    struct taek_upstream_mm_resultado r={0};int code=taek_upstream_drm_mm_probar(&r);
    r.avisos=__atomic_load_n(&avisos,__ATOMIC_RELAXED)-antes;
    if(r.avisos!=1)code=-1; /* Un takedown no limpio deliberado en el ensayo. */
    consola_imprimir("[UPSTREAM_MM] RESULTADO=");consola_imprimir(code?"FAIL":"OK");
    consola_imprimir(" checks=");consola_imprimir_dec(r.checks);
    consola_imprimir(" mask=");consola_imprimir_hex(r.fallos);
    consola_imprimir(" limpio=");consola_imprimir_dec(r.limpio);
    consola_imprimir(" avisos=");consola_imprimir_dec(r.avisos);consola_imprimir_linea("");
    consola_imprimir_linea("[UPSTREAM_MM] drm_mm/rbtree reales, WARN deliberado sin liberar nodos; GGTT/GPU pendientes.");
    __atomic_clear(&ocupado,__ATOMIC_RELEASE);return code;
}
