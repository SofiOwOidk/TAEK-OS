#include "anillo_almacenamiento.h"

// ============================================================================
// TAEK OS - ANILLO DE ALMACENAMIENTO
// ============================================================================

void anillo_iniciar(anillo_almacenamiento *a,
                    int64_t (*leer)(void *, int, uint64_t, void *, size_t),
                    void *contexto,
                    int (*validar)(void *), void *usuario_validar,
                    uint64_t tamano) {
    if (!a) return;
    uint8_t *b = (uint8_t *)a;
    for (unsigned i = 0; i < (unsigned)sizeof(*a); i++) b[i] = 0;
    a->leer = leer;
    a->contexto = contexto;
    a->validar = validar;
    a->usuario_validar = usuario_validar;
    a->tamano = tamano;
    a->bloque_bytes = ANILLO_BLOQUE_BYTES;
    a->margen_min = UINT64_MAX;
}

static uint64_t anillo_min_leido(const anillo_almacenamiento *a);

// Margen = bytes prefetched por delante del consumidor mas lento.
static void anillo_muestrear_margen(anillo_almacenamiento *a) {
    uint64_t min_leido = anillo_min_leido(a);
    uint64_t m = a->productor_off > min_leido ? a->productor_off - min_leido : 0;
    if (m < a->margen_min) a->margen_min = m;
    if (m > a->margen_max) a->margen_max = m;
    a->margen_suma += m;
    a->margen_muestras++;
}

// Menor offset ya leido por alguna pista activa: protege los bloques que un
// consumidor lento aun necesita. Sin pistas activas se protege la ventana
// entera (no adelantar el productor mas alla de lo ya retenido).
static uint64_t anillo_min_leido(const anillo_almacenamiento *a) {
    uint64_t m = 0;
    int hay = 0;
    for (int p = 0; p < ANILLO_PISTAS; p++) {
        if (!a->pista_activa[p]) continue;
        if (!hay || a->leido[p] < m) { m = a->leido[p]; hay = 1; }
    }
    if (hay) return m;
    return a->cantidad ? a->bloques[a->cabeza].offset : a->productor_off;
}

static int anillo_buscar(const anillo_almacenamiento *a, uint64_t pos) {
    for (unsigned i = 0; i < a->cantidad; i++) {
        unsigned s = (a->cabeza + i) % ANILLO_BLOQUES;
        const anillo_bloque *bl = &a->bloques[s];
        if (bl->estado != ANILLO_VACIO && pos >= bl->offset &&
            pos - bl->offset < bl->bytes) return (int)s;
    }
    return -1;
}

static void anillo_descartar_todo(anillo_almacenamiento *a) {
    for (unsigned i = 0; i < a->cantidad; i++) {
        unsigned s = (a->cabeza + i) % ANILLO_BLOQUES;
        if (a->tocado[s]) a->bytes_utiles += a->bloques[s].bytes;
        else a->bytes_inutiles += a->bloques[s].bytes;
    }
    for (unsigned i = 0; i < ANILLO_BLOQUES; i++) a->bloques[i].estado = ANILLO_VACIO;
    a->cabeza = 0; a->cola = 0; a->cantidad = 0;
}

int anillo_rellenar(anillo_almacenamiento *a, unsigned max_bloques) {
    if (!a || !a->leer) return -1;
    for (unsigned k = 0; k < max_bloques; k++) {
        if (a->productor_off >= a->tamano) return 0; // EOF
        if (a->cantidad == ANILLO_BLOQUES) {
            anillo_bloque *viejo = &a->bloques[a->cabeza];
            // No pisar datos que alguna pista aun pueda necesitar.
            if (viejo->offset + viejo->bytes > anillo_min_leido(a)) {
                a->productor_paradas++;
                return 1;
            }
            if (a->tocado[a->cabeza]) a->bytes_utiles += viejo->bytes;
            else a->bytes_inutiles += viejo->bytes;
            viejo->estado = ANILLO_VACIO;
            a->cabeza = (a->cabeza + 1) % ANILLO_BLOQUES;
            a->cantidad--;
            a->evicciones++;
        }
        anillo_bloque *bl = &a->bloques[a->cola];
        uint64_t resta = a->tamano - a->productor_off;
        size_t n = resta < a->bloque_bytes ? (size_t)resta : a->bloque_bytes;
        bl->estado = ANILLO_LLENANDO;
        int64_t r = a->leer(a->contexto, 0, a->productor_off, a->datos[a->cola], n);
        if (r != (int64_t)n) { bl->estado = ANILLO_VACIO; return r < 0 ? (int)r : -1; }
        bl->offset = a->productor_off;
        bl->bytes = n;
        bl->estado = ANILLO_LISTO;
        a->tocado[a->cola] = 0;
        a->productor_off += n;
        a->cola = (a->cola + 1) % ANILLO_BLOQUES;
        a->cantidad++;
        a->bloques_leidos++;
        a->bytes_leidos += n;
        anillo_muestrear_margen(a);
    }
    return 1;
}

int64_t anillo_leer(void *contexto, int pista, uint64_t off, void *dst, size_t n) {
    anillo_almacenamiento *a = contexto;
    if (!a || !a->leer || !dst || off > a->tamano || n > a->tamano - off)
        return MP4_DATOS_INVALIDOS;
    if (!n) return 0;

    int p = (pista >= 0 && pista < ANILLO_PISTAS) ? pista : 0;
    a->lecturas++;
    uint8_t *d = dst;
    size_t hechos = 0;
    while (hechos < n) {
        if (a->validar) {
            int r = a->validar(a->usuario_validar);
            if (r) return r;
        }
        uint64_t pos = off + hechos;
        int s = anillo_buscar(a, pos);
        if (s < 0) {
            // No esta prefetched: reposicionar ante retroceso o salto, y
            // producir bajo demanda (esta es la espera sincrona a minimizar).
            uint64_t ini = a->cantidad ? a->bloques[a->cabeza].offset : a->productor_off;
            if (pos < ini || pos > a->productor_off) {
                anillo_descartar_todo(a);
                a->productor_off = pos;
                a->reposiciones++;
            }
            a->fallos++;
            a->starvations++;
            while (anillo_buscar(a, pos) < 0) {
                if (a->productor_off >= a->tamano) break;
                if (a->cantidad == ANILLO_BLOQUES) {
                    a->bloques[a->cabeza].estado = ANILLO_VACIO;
                    a->cabeza = (a->cabeza + 1) % ANILLO_BLOQUES;
                    a->cantidad--;
                    a->evicciones++;
                }
                int r = anillo_rellenar(a, 1);
                if (r < 0) return r;
                if (r == 0) break;
            }
            s = anillo_buscar(a, pos);
            if (s < 0) return MP4_ERROR_LECTURA; // EOF o discontinuidad
        } else {
            a->aciertos++;
            a->bloques[s].estado = ANILLO_CONSUMIENDO;
        }

        a->tocado[s] = 1;
        uint64_t dentro = pos - a->bloques[s].offset;
        size_t k = a->bloques[s].bytes - (size_t)dentro;
        if (k > n - hechos) k = n - hechos;
        for (size_t i = 0; i < k; i++) d[hechos + i] = a->datos[s][dentro + i];
        hechos += k;
    }

    if (off + n > a->leido[p]) a->leido[p] = off + n;
    a->pista_activa[p] = 1;
    anillo_muestrear_margen(a);
    return (int64_t)hechos;
}
