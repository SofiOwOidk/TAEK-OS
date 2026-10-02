#ifndef H264_INVARIANTE_SMP_H
#define H264_INVARIANTE_SMP_H
#include <stdint.h>

/* Lógica pura de coherencia SMP de reconstrucción, aislada para poder
 * verificarla en host sin montar un decodificador (Encargo A1).
 *
 * cpu_total: suma de la pared de fila de los trabajadores, en una base de
 *            reloj común (si el TSC difiere por CPU, normalizar antes).
 * pared:     pared del lote medida en el BSP.
 * participantes: número de ejecutores (BSP + APs) que pueden tomar regiones.
 * ratio_miles (salida opcional): cpu_total*1000/pared; 0 si pared==0.
 *
 * Devuelve 1 (incoherente) si cpu_total > participantes*pared, es decir, si el
 * cómputo acumulado no cabe en el intervalo real de los ejecutores. Un lote de
 * cuatro ejecutores con relación 6,49 debe marcarse como incoherente. */
static inline int h264_smp_invariante(uint64_t cpu_total, uint64_t pared,
                                      unsigned participantes, uint64_t *ratio_miles) {
    uint64_t r = pared ? (cpu_total * 1000) / pared : 0;
    if (ratio_miles) *ratio_miles = r;
    if (!pared || !participantes) return 0;
    return cpu_total > (uint64_t)participantes * pared;
}

/* Cuenta regiones por identificador. vista[k] es el número de veces que se
 * ejecutó la región k. Una región duplicada (vista>1) o faltante (vista==0)
 * invalida la comprobación de "exactamente una vez". Devuelve las ejecutadas
 * exactamente una vez y escribe los recuentos de duplicadas y faltantes. */
static inline unsigned h264_regiones_contar(const uint8_t *vista, unsigned filas,
                                            unsigned *duplicadas, unsigned *faltantes) {
    unsigned unicas = 0, dup = 0, fal = 0;
    for (unsigned k = 0; k < filas; k++) {
        if (vista[k] == 1) unicas++;
        else if (vista[k] == 0) fal++;
        else dup++;
    }
    if (duplicadas) *duplicadas = dup;
    if (faltantes) *faltantes = fal;
    return unicas;
}
#endif
