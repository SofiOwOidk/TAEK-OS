#include "cache_fuente.h"

// ============================================================================
// TAEK OS - CACHE SECUENCIAL / READ-AHEAD PARA FUENTES VFS
// Desacopla la lectura logica del demux de los READ(10) fisicos: tras varias
// peticiones contiguas se adelanta una ventana completa; un salto la desactiva.
// ============================================================================

static void cache_invalidar(cache_fuente *c) {
    c->validos[0] = c->validos[1] = 0;
}

static int cache_buscar(const cache_fuente *c, uint64_t pos) {
    for (int s = 0; s < CACHE_FUENTE_RANURAS; s++) {
        if (c->validos[s] && pos >= c->offset_ranura[s] &&
            pos - c->offset_ranura[s] < c->validos[s]) {
            return s;
        }
    }
    return -1;
}

int64_t cache_fuente_leer(void *u, int pista, uint64_t off, void *dest, size_t n) {
    cache_fuente *c = u;
    if (!c || !c->leer || !dest || !c->ventana || c->ventana > CACHE_FUENTE_MAX ||
        off > c->tamano || n > c->tamano - off) return MP4_DATOS_INVALIDOS;
    if (!n) return 0;

    int p = (pista >= 0 && pista < CACHE_FUENTE_PISTAS) ? pista : 0;
    cache_readahead *ra = &c->ra[p];
    if (!ra->ventana) ra->ventana = c->ventana;

    // Deteccion de secuencialidad a nivel de lectura logica, por pista.
    // Se considera avance secuencial el que va hacia delante sin saltar mas de
    // una ventana (los chunks del contenedor dejan huecos, pero no seeks). Un
    // retroceso o un salto mayor resetea el estado.
    if (ra->activo) {
        if (off >= ra->offset_esperado && off - ra->offset_esperado <= ra->ventana)
            ra->consecutivas++;
        else { ra->consecutivas = 0; ra->secuencial = 0; }
    } else {
        ra->activo = 1;
    }
    if (ra->consecutivas >= CACHE_FUENTE_UMBRAL_SECUENCIAL) ra->secuencial = 1;

    c->lecturas++;
    uint8_t *dst = dest;
    size_t hechos = 0;
    while (hechos < n) {
        int r = c->validar ? c->validar(c->usuario_validar) : 0;
        if (r) { cache_invalidar(c); return r; }

        uint64_t pos = off + hechos;
        int s = cache_buscar(c, pos);
        if (s < 0) {
            // Fallo: se pide lo solicitado, o se adelanta una ventana si el
            // acceso es secuencial. La base se alinea a sector para que el
            // lector de FS agregue la peticion en un unico READ(10).
            uint64_t base = pos & ~(uint64_t)511u;
            size_t bytes = ra->secuencial ? ra->ventana
                                          : (size_t)(n - hechos) + (size_t)(pos - base);
            if (bytes > CACHE_FUENTE_MAX) bytes = CACHE_FUENTE_MAX;
            if (bytes > c->tamano - base) bytes = (size_t)(c->tamano - base);
            if (!bytes) return MP4_ERROR_LECTURA;

            s = (int)(c->reemplazo++ % CACHE_FUENTE_RANURAS);
            c->validos[s] = 0;
            int64_t leidos = c->leer(c->contexto, pista, base, c->datos[s], bytes);
            c->fallos++;
            if (ra->secuencial) c->adelantos++;
            if (leidos != (int64_t)bytes) {
                cache_invalidar(c);
                return leidos < 0 ? leidos : MP4_ERROR_LECTURA;
            }
            r = c->validar ? c->validar(c->usuario_validar) : 0;
            if (r) { cache_invalidar(c); return r; }
            c->offset_ranura[s] = base;
            c->validos[s] = bytes;
        } else {
            c->aciertos++;
        }

        uint64_t dentro = pos - c->offset_ranura[s];
        size_t k = c->validos[s] - (size_t)dentro;
        if (k > n - hechos) k = n - hechos;
        for (size_t i = 0; i < k; i++) dst[hechos + i] = c->datos[s][dentro + i];
        hechos += k;
    }

    ra->offset_esperado = off + n;
    return (int64_t)hechos;
}
