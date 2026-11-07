/* Interprete AML: ver aml.h. Sigue el capitulo 20 de la especificacion ACPI (gramatica) y el 19 (semantica de los operadores). */
#include "aml.h"

#define NODOS_MAX 16384
#define ARENA_FIJA (6u << 20)
#define ARENA_EVAL (4u << 20)
#define SOMBRA_MAX 8192
#define PASOS_MAX UINT64_C(20000000)
#define METODOS_MAX 24
#define TERMINOS_MAX 80

enum { N_AMBITO, N_DISPOSITIVO, N_NOMBRE, N_METODO, N_REGION, N_CAMPO, N_CAMPO_INDICE, N_CAMPO_BANCO, N_BUFCAMPO, N_MUTEX, N_ALIAS };
enum { REG_MEM = 0, REG_IO = 1, REG_PCI = 2, REG_EC = 3 };

typedef struct {
    char nombre[4];
    int padre, hijo, hermano;
    uint8_t tipo;
    union {
        aml_valor valor;
        struct { const uint8_t *ini; uint32_t n; uint8_t args; uint8_t ancho64; } metodo;
        struct { uint8_t espacio; uint64_t base, n; } region;
        struct { int region, indice, datos, banco; uint64_t valor_banco; uint32_t bit, bits; uint8_t acceso; } campo;
        struct { uint8_t *buf; uint32_t n, bit, bits; } bufcampo;
        int alias;
    } d;
} nodo;

struct sombra { int region; uint64_t off; uint8_t v; };

struct aml {
    struct aml_host h;
    nodo *nodos; int n_nodos;
    uint8_t *fija; size_t fija_usada;
    uint8_t *eval; size_t eval_usada;
    struct sombra *sombra; unsigned n_sombra;
    int error; char motivo[96];
    uint64_t pasos, pasos_limite;
    struct aml_estadisticas est;
    int profundidad, metodos;
    uint8_t ancho64;
    int en_carga;
    int ec_escritura;
};
void aml_permitir_escritura_ec(aml *a, int permitir) { a->ec_escritura = permitir; }

typedef struct marco {
    const uint8_t *p, *fin;
    int ambito;
    aml_valor local[8], arg[7];
    aml_valor retorno;
    uint8_t salida;      /* 1 return, 2 break, 3 continue */
    uint8_t es_metodo;
} marco;

/* ------------------------------------------------------------------ utilidades */
static void cero(void *d, size_t n) { uint8_t *q = d; while (n--) *q++ = 0; }
static void copia(void *d, const void *s, size_t n) { uint8_t *q = d; const uint8_t *r = s; while (n--) *q++ = *r++; }
static size_t largo(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int iguales4(const char *a, const char *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

static void texto(char *d, size_t cap, size_t *o, const char *s) { while (*s && *o + 1 < cap) d[(*o)++] = *s++; d[*o] = 0; }
static void numero(char *d, size_t cap, size_t *o, uint64_t v, unsigned base) {
    char t[24]; unsigned k = 0;
    do { unsigned c = (unsigned)(v % base); t[k++] = (char)(c < 10 ? '0' + c : 'a' + c - 10); v /= base; } while (v);
    while (k && *o + 1 < cap) d[(*o)++] = t[--k];
    d[*o] = 0;
}
static int falla(aml *a, const char *m, const char *extra, uint64_t num, int con_num) {
    if (a->error) return -1;
    a->error = 1;
    size_t o = 0;
    texto(a->motivo, sizeof(a->motivo), &o, m);
    if (extra) texto(a->motivo, sizeof(a->motivo), &o, extra);
    if (con_num) { texto(a->motivo, sizeof(a->motivo), &o, " 0x"); numero(a->motivo, sizeof(a->motivo), &o, num, 16); }
    return -1;
}
static void *de_arena(aml *a, int fija, size_t bytes) {
    bytes = (bytes + 15) & ~(size_t)15;
    if (fija) {
        if (bytes > ARENA_FIJA - a->fija_usada) { falla(a, "memoria fija del interprete agotada", 0, 0, 0); return 0; }
        void *p = a->fija + a->fija_usada; a->fija_usada += bytes; cero(p, bytes); return p;
    }
    if (bytes > ARENA_EVAL - a->eval_usada) { falla(a, "memoria de evaluacion agotada", 0, 0, 0); return 0; }
    void *p = a->eval + a->eval_usada; a->eval_usada += bytes; cero(p, bytes); return p;
}
#define ARENA (a->en_carga)

/* ------------------------------------------------------------------ espacio de nombres */
static int nuevo_nodo(aml *a, int padre, const char *nombre, uint8_t tipo) {
    if (a->n_nodos >= NODOS_MAX) { falla(a, "demasiados objetos en las tablas", 0, 0, 0); return -1; }
    int i = a->n_nodos++;
    nodo *n = &a->nodos[i];
    cero(n, sizeof(*n));
    copia(n->nombre, nombre, 4);
    n->padre = padre; n->hijo = -1; n->hermano = -1; n->tipo = tipo;
    if (padre >= 0) { n->hermano = a->nodos[padre].hijo; a->nodos[padre].hijo = i; }
    return i;
}
int aml_hijo(const aml *a, int nodo_i, const char *nombre) {
    for (int i = a->nodos[nodo_i].hijo; i >= 0; i = a->nodos[i].hermano)
        if (iguales4(a->nodos[i].nombre, nombre)) return i;
    return -1;
}
static int es_inicio_nombre(uint8_t c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
static int es_nombre(uint8_t c) { return c == '\\' || c == '^' || c == 0x2e || c == 0x2f || es_inicio_nombre(c); }

/* Salta un NameString sin resolverlo. */
static int saltar_ruta(aml *a, marco *m, const uint8_t **sale) {
    const uint8_t *p = m->p;
    if (p < m->fin && *p == '\\') p++;
    else while (p < m->fin && *p == '^') p++;
    if (p >= m->fin) return falla(a, "nombre truncado", 0, 0, 0);
    size_t seg;
    if (*p == 0) { p++; seg = 0; }
    else if (*p == 0x2e) { p++; seg = 2; }
    else if (*p == 0x2f) { if (p + 1 >= m->fin) return falla(a, "nombre truncado", 0, 0, 0); seg = p[1]; p += 2; }
    else seg = 1;
    if ((size_t)(m->fin - p) < seg * 4) return falla(a, "nombre truncado", 0, 0, 0);
    if (sale) *sale = m->p;
    m->p = p + seg * 4;
    return 0;
}
/* Resuelve una ruta. crear: ultimo segmento e intermedios como ambitos. *ult da el ultimo segmento si no existe. */
static int resolver(aml *a, const uint8_t *r, const uint8_t *fin, int ambito, int crear, int *padre_sale, const uint8_t **ult) {
    int base = ambito, absoluta = 0;
    const uint8_t *p = r;
    if (p < fin && *p == '\\') { base = 0; p++; absoluta = 1; }
    else while (p < fin && *p == '^') { if (a->nodos[base].padre >= 0) base = a->nodos[base].padre; p++; absoluta = 1; }
    if (p >= fin) return -1;
    size_t seg;
    if (*p == 0) return base;
    if (*p == 0x2e) { p++; seg = 2; }
    else if (*p == 0x2f) { if (p + 1 >= fin) return -1; seg = p[1]; p += 2; }
    else seg = 1;
    if ((size_t)(fin - p) < seg * 4 || !seg) return -1;
    /* Un solo segmento sin prefijo sube por los ambitos (5.3); uno creado siempre va en el actual. */
    if (seg == 1 && !absoluta && !crear) {
        for (int b = base; b >= 0; b = a->nodos[b].padre) {
            int h = aml_hijo(a, b, (const char *)p);
            if (h >= 0) return h;
        }
        return -1;
    }
    int cur = base;
    for (size_t i = 0; i < seg; i++, p += 4) {
        int h = aml_hijo(a, cur, (const char *)p);
        if (h < 0) {
            if (!crear) {
                /* Una ruta de varios segmentos tambien busca su primer segmento hacia arriba. */
                if (i == 0 && !absoluta && a->nodos[cur].padre >= 0) { cur = a->nodos[cur].padre; i--; p -= 4; continue; }
                return -1;
            }
            if (i + 1 == seg) { if (padre_sale) *padre_sale = cur; if (ult) *ult = p; return -1; }
            h = nuevo_nodo(a, cur, (const char *)p, N_AMBITO);
            if (h < 0) return -1;
        }
        cur = h;
    }
    while (a->nodos[cur].tipo == N_ALIAS) cur = a->nodos[cur].d.alias;
    return cur;
}
/* Nombre de un objeto que se define aqui: lo crea, o reutiliza el que ya existia. */
static int definir(aml *a, marco *m, uint8_t tipo, int ambito_extra) {
    const uint8_t *r;
    (void)ambito_extra;
    if (saltar_ruta(a, m, &r)) return -1;
    int padre = -1; const uint8_t *ult = 0;
    int i = resolver(a, r, m->p, m->ambito, 1, &padre, &ult);
    if (i >= 0) {
        /* Las tablas se reabren y los metodos se repiten: un ambito declarado como externo adopta el tipo real. */
        if (tipo != N_AMBITO) a->nodos[i].tipo = tipo;
        else if (a->nodos[i].tipo == N_AMBITO) a->nodos[i].tipo = N_AMBITO;
        return i;
    }
    if (padre < 0 || !ult) { falla(a, "ruta de definicion invalida", 0, 0, 0); return -1; }
    return nuevo_nodo(a, padre, (const char *)ult, tipo);
}
int aml_buscar(const aml *a, const char *ruta) {
    int cur = 0;
    const char *p = ruta;
    if (*p == '\\') p++;
    while (*p) {
        char s[4] = {'_', '_', '_', '_'};
        for (unsigned i = 0; i < 4 && *p && *p != '.'; i++) s[i] = *p++;
        if (*p == '.') p++;
        cur = aml_hijo(a, cur, s);
        if (cur < 0) return -1;
    }
    return cur;
}
void aml_ruta(const aml *a, int i, char *d, size_t cap) {
    int pila[32]; unsigned k = 0; size_t o = 0;
    while (i > 0 && k < 32) { pila[k++] = i; i = a->nodos[i].padre; }
    if (cap) d[0] = 0;
    texto(d, cap, &o, "\\");
    while (k) {
        const nodo *n = &a->nodos[pila[--k]];
        for (unsigned j = 0; j < 4 && o + 1 < cap; j++) if (n->nombre[j] != '_' || j == 0) d[o++] = n->nombre[j];
        if (o < cap) d[o] = 0;
        if (k) texto(d, cap, &o, ".");
    }
}
int aml_nodos(const aml *a) { return a->n_nodos; }
const char *aml_motivo(const aml *a) { return a->motivo; }
void aml_estadisticas_leer(const aml *a, struct aml_estadisticas *e) { *e = a->est; e->pasos = a->pasos; }

/* ------------------------------------------------------------------ parser basico */
static int pkg_longitud(aml *a, marco *m, const uint8_t **fin_sale) {
    const uint8_t *ini = m->p;
    if (m->p >= m->fin) return falla(a, "longitud de paquete truncada", 0, 0, 0);
    uint8_t b = *m->p++;
    unsigned extra = b >> 6;
    uint32_t n = extra ? b & 15 : b & 63;
    if ((size_t)(m->fin - m->p) < extra) return falla(a, "longitud de paquete truncada", 0, 0, 0);
    for (unsigned i = 0; i < extra; i++) n |= (uint32_t)*m->p++ << (4 + 8 * i);
    if (n > (size_t)(m->fin - ini)) return falla(a, "paquete mas largo que su contenedor", 0, 0, 0);
    *fin_sale = ini + n;
    return 0;
}
static int leer_le(aml *a, marco *m, unsigned bytes, uint64_t *v) {
    if ((size_t)(m->fin - m->p) < bytes) return falla(a, "constante truncada", 0, 0, 0);
    uint64_t x = 0;
    for (unsigned i = 0; i < bytes; i++) x |= (uint64_t)m->p[i] << (8 * i);
    m->p += bytes; *v = x;
    return 0;
}
static uint64_t mascara(const aml *a, uint64_t v) { return a->ancho64 ? v : v & 0xffffffffu; }

static int termino(aml *a, marco *m, aml_valor *sale);
static int lista(aml *a, marco *m);
static int llamar(aml *a, int metodo, const aml_valor *args, unsigned nargs, aml_valor *sale);
static int leer_nodo(aml *a, int i, aml_valor *sale);

/* ------------------------------------------------------------------ conversiones */
static aml_valor entero(uint64_t v) { aml_valor x; cero(&x, sizeof(x)); x.tipo = AML_ENTERO; x.v.entero = v; return x; }
static int desref(aml *a, const aml_valor *v, aml_valor *sale) {
    aml_valor x = *v;
    for (unsigned g = 0; g < 16; g++) {
        if (x.tipo == AML_REF) { x = *x.v.ref; continue; }
        if (x.tipo == AML_REF_BYTE) { x = entero(*x.v.ref_byte); break; }
        if (x.tipo == AML_RUTA) {
            const uint8_t *fin = x.v.ruta + 512;
            /* La ruta salio de una tabla ya validada: se mide de nuevo sin pasar del prefijo mas largo. */
            int i = resolver(a, x.v.ruta, fin, x.ruta_ambito, 0, 0, 0);
            if (i < 0) return falla(a, "elemento de paquete con nombre sin definir", 0, 0, 0);
            if (leer_nodo(a, i, &x)) return -1;
            continue;
        }
        break;
    }
    *sale = x;
    return 0;
}
static int a_entero(aml *a, const aml_valor *v0, uint64_t *e) {
    aml_valor v;
    if (desref(a, v0, &v)) return -1;
    if (v.tipo == AML_ENTERO) { *e = v.v.entero; return 0; }
    if (v.tipo == AML_BUFFER) {
        uint64_t x = 0;
        for (unsigned i = 0; i < v.n && i < (a->ancho64 ? 8u : 4u); i++) x |= (uint64_t)v.v.buffer[i] << (8 * i);
        *e = x;
        return 0;
    }
    if (v.tipo == AML_CADENA) {
        uint64_t x = 0;
        const char *s = v.v.cadena;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        for (; *s; s++) {
            unsigned d = *s >= '0' && *s <= '9' ? (unsigned)(*s - '0') : *s >= 'a' && *s <= 'f' ? (unsigned)(*s - 'a' + 10) : *s >= 'A' && *s <= 'F' ? (unsigned)(*s - 'A' + 10) : 99;
            if (d > 15) break;
            x = x * 16 + d;
        }
        *e = mascara(a, x);
        return 0;
    }
    return falla(a, "se esperaba un entero", 0, v.tipo, 1);
}
static int evaluar_entero(aml *a, marco *m, uint64_t *e) {
    aml_valor v;
    if (termino(a, m, &v)) return -1;
    return a_entero(a, &v, e);
}
/* Copia profunda: un Store no comparte datos con su origen. */
static int duplicar(aml *a, int fija, const aml_valor *o, aml_valor *d) {
    *d = *o;
    if (o->tipo == AML_CADENA) {
        size_t n = largo(o->v.cadena);
        char *c = de_arena(a, fija, n + 1);
        if (!c) return -1;
        copia(c, o->v.cadena, n + 1);
        d->v.cadena = c; d->n = (uint32_t)n;
    } else if (o->tipo == AML_BUFFER) {
        uint8_t *b = de_arena(a, fija, o->n ? o->n : 1);
        if (!b) return -1;
        copia(b, o->v.buffer, o->n);
        d->v.buffer = b;
    } else if (o->tipo == AML_PAQUETE) {
        aml_valor *e = de_arena(a, fija, sizeof(aml_valor) * (o->n ? o->n : 1));
        if (!e) return -1;
        for (uint32_t i = 0; i < o->n; i++) if (duplicar(a, fija, &o->v.paquete[i], &e[i])) return -1;
        d->v.paquete = e;
    }
    return 0;
}

/* ------------------------------------------------------------------ regiones y campos */
static int sombra_leer(const aml *a, int region, uint64_t off, uint8_t *v) {
    for (unsigned i = a->n_sombra; i; i--)
        if (a->sombra[i - 1].region == region && a->sombra[i - 1].off == off) { *v = a->sombra[i - 1].v; return 1; }
    return 0;
}
static int region_adr(aml *a, int region, uint8_t *bus, uint8_t *dis, uint8_t *fun) {
    for (int p = a->nodos[region].padre; p > 0; p = a->nodos[p].padre) {
        int adr = aml_hijo(a, p, "_ADR");
        if (adr < 0) continue;
        aml_valor v; uint64_t e;
        if (leer_nodo(a, adr, &v) || a_entero(a, &v, &e)) return -1;
        *dis = (uint8_t)(e >> 16); *fun = (uint8_t)e;
        /* Solo el bus raiz: un puente exigiria leer y seguir su configuracion. */
        int abuelo = a->nodos[p].padre;
        if (abuelo > 0 && aml_hijo(a, abuelo, "_ADR") >= 0) return falla(a, "region PCI detras de un puente", 0, 0, 0);
        *bus = 0;
        return 0;
    }
    return falla(a, "region PCI sin _ADR", 0, 0, 0);
}
static int region_leer_byte(aml *a, int region, uint64_t off, unsigned ancho, uint8_t *sale) {
    const nodo *r = &a->nodos[region];
    if (off >= r->d.region.n) return falla(a, "lectura fuera de la region", 0, off, 1);
    if (sombra_leer(a, region, off, sale)) return 0;
    uint64_t v = 0;
    uint64_t al = off & ~(uint64_t)(ancho - 1);
    switch (r->d.region.espacio) {
        case REG_MEM:
            a->est.lecturas_mem++;
            if (!a->h.leer_mem || a->h.leer_mem(r->d.region.base + al, ancho, &v)) return falla(a, "lectura de memoria rechazada", 0, r->d.region.base + al, 1);
            break;
        case REG_IO:
            a->est.lecturas_io++;
            if (!a->h.leer_io || a->h.leer_io((uint16_t)(r->d.region.base + al), ancho > 4 ? 4 : ancho, &v)) return falla(a, "lectura de puerto rechazada", 0, r->d.region.base + al, 1);
            break;
        case REG_PCI: {
            uint8_t bus = 0, dis = 0, fun = 0;
            if (region_adr(a, region, &bus, &dis, &fun)) return -1;
            a->est.lecturas_pci++;
            if (!a->h.leer_pci || a->h.leer_pci(bus, dis, fun, (uint16_t)(r->d.region.base + al), ancho > 4 ? 4 : ancho, &v)) return falla(a, "lectura PCI rechazada", 0, 0, 0);
            break;
        }
        case REG_EC: {
            uint8_t b;
            a->est.lecturas_ec++;
            if (!a->h.leer_ec || r->d.region.base + off > 255 || a->h.leer_ec((uint8_t)(r->d.region.base + off), &b)) return falla(a, "el controlador embebido no respondio", 0, r->d.region.base + off, 1);
            *sale = b;
            return 0;
        }
        default:
            return falla(a, "espacio de region sin servicio", 0, r->d.region.espacio, 1);
    }
    *sale = (uint8_t)(v >> (8 * (off - al)));
    return 0;
}
static int region_escribir_byte(aml *a, int region, uint64_t off, uint8_t v) {
    const nodo *r = &a->nodos[region];
    if (off >= r->d.region.n) return falla(a, "escritura fuera de la region", 0, off, 1);
    if (r->d.region.espacio == REG_EC && a->ec_escritura && a->h.escribir_ec) {
        a->est.escrituras_ec++;
        if (r->d.region.base + off > 255 || a->h.escribir_ec((uint8_t)(r->d.region.base + off), v)) return falla(a, "el controlador embebido no acepto la escritura", 0, r->d.region.base + off, 1);
        return 0;
    }
    /* Memoria del firmware: en sombra. El resto cambia el estado del equipo y este interprete no lo hace. */
    if (r->d.region.espacio != REG_MEM) {
        char ruta[40]; aml_ruta(a, region, ruta, sizeof(ruta));
        return falla(a, r->d.region.espacio == REG_EC ? "pide escribir en el controlador embebido: " :
                        r->d.region.espacio == REG_IO ? "pide escribir en un puerto: " : "pide escribir en hardware: ", ruta, 0, 0);
    }
    a->est.escrituras_sombra++;
    for (unsigned i = 0; i < a->n_sombra; i++)
        if (a->sombra[i].region == region && a->sombra[i].off == off) { a->sombra[i].v = v; return 0; }
    if (a->n_sombra >= SOMBRA_MAX) return falla(a, "demasiadas escrituras en sombra", 0, 0, 0);
    a->sombra[a->n_sombra++] = (struct sombra){region, off, v};
    return 0;
}
static unsigned ancho_acceso(uint8_t acceso) {
    switch (acceso & 15) { case 2: return 2; case 3: return 4; case 4: return 8; default: return 1; }
}
static int campo_a_buffer(aml *a, int i, uint8_t *bytes, uint32_t cap);
static int campo_leer_bits(aml *a, const nodo *c, uint8_t *d, uint32_t cap) {
    uint32_t bit = c->d.campo.bit, bits = c->d.campo.bits;
    unsigned ancho = ancho_acceso(c->d.campo.acceso);
    if (cap * 8 < bits) return falla(a, "campo mayor que su destino", 0, 0, 0);
    cero(d, cap);
    uint8_t x = 0;
    uint64_t leido = UINT64_MAX;
    for (uint32_t k = 0; k < bits; k++) {
        uint32_t b = bit + k;
        /* Una lectura por byte: el controlador embebido tarda, y un bit no vale una transaccion. */
        if (b / 8 != leido) {
            if (region_leer_byte(a, c->d.campo.region, b / 8, ancho, &x)) return -1;
            leido = b / 8;
        }
        if ((x >> (b % 8)) & 1) d[k / 8] |= (uint8_t)(1u << (k % 8));
    }
    return 0;
}
static int campo_a_buffer(aml *a, int i, uint8_t *bytes, uint32_t cap) {
    const nodo *c = &a->nodos[i];
    if (c->tipo == N_CAMPO) return campo_leer_bits(a, c, bytes, cap);
    if (c->tipo == N_CAMPO_INDICE) {
        /* Hay que escribir el indice: solo es posible si sus datos estan en memoria en sombra, y entonces no sirve de nada. */
        char ruta[40]; aml_ruta(a, i, ruta, sizeof(ruta));
        return falla(a, "campo indexado pide escribir hardware: ", ruta, 0, 0);
    }
    char ruta[40]; aml_ruta(a, i, ruta, sizeof(ruta));
    return falla(a, "campo por bancos pide escribir hardware: ", ruta, 0, 0);
}
static int campo_escribir(aml *a, int i, const uint8_t *src, uint32_t n) {
    const nodo *c = &a->nodos[i];
    if (c->tipo != N_CAMPO) { char ruta[40]; aml_ruta(a, i, ruta, sizeof(ruta)); return falla(a, "campo indexado pide escribir hardware: ", ruta, 0, 0); }
    uint32_t bit = c->d.campo.bit, bits = c->d.campo.bits;
    for (uint32_t b0 = bit / 8; b0 * 8 < bit + bits; b0++) {
        uint8_t v = 0;
        /* Conserva lo que no se escribe; lee antes solo si el byte no se cubre entero. */
        int entero_byte = bit <= b0 * 8 && (b0 + 1) * 8 <= bit + bits;
        if (!entero_byte && region_leer_byte(a, c->d.campo.region, b0, 1, &v)) return -1;
        for (unsigned j = 0; j < 8; j++) {
            uint32_t b = b0 * 8 + j;
            if (b < bit || b >= bit + bits) continue;
            uint32_t k = b - bit;
            unsigned val = k / 8 < n ? (src[k / 8] >> (k % 8)) & 1 : 0;
            v = (uint8_t)((v & ~(1u << j)) | (val << j));
        }
        if (region_escribir_byte(a, c->d.campo.region, b0, v)) return -1;
    }
    return 0;
}
static uint64_t bits_de(const uint8_t *b, uint32_t n, uint32_t bit, uint32_t bits) {
    uint64_t v = 0;
    for (uint32_t k = 0; k < bits && k < 64; k++) {
        uint32_t x = bit + k;
        if (x / 8 < n && ((b[x / 8] >> (x % 8)) & 1)) v |= UINT64_C(1) << k;
    }
    return v;
}
static int leer_nodo(aml *a, int i, aml_valor *sale) {
    nodo *n = &a->nodos[i];
    switch (n->tipo) {
        case N_NOMBRE: *sale = n->d.valor; return 0;
        case N_CAMPO: case N_CAMPO_INDICE: case N_CAMPO_BANCO: {
            uint32_t bits = n->tipo == N_CAMPO ? n->d.campo.bits : n->d.campo.bits;
            uint32_t cap = (bits + 7) / 8;
            if (bits <= 64) {
                uint8_t t[8];
                if (campo_a_buffer(a, i, t, 8)) return -1;
                *sale = entero(bits_de(t, 8, 0, bits));
                return 0;
            }
            uint8_t *b = de_arena(a, 0, cap);
            if (!b || campo_a_buffer(a, i, b, cap)) return -1;
            cero(sale, sizeof(*sale)); sale->tipo = AML_BUFFER; sale->n = cap; sale->v.buffer = b;
            return 0;
        }
        case N_BUFCAMPO:
            if (n->d.bufcampo.bits <= 64) { *sale = entero(bits_de(n->d.bufcampo.buf, n->d.bufcampo.n, n->d.bufcampo.bit, n->d.bufcampo.bits)); return 0; }
            else {
                uint32_t cap = (n->d.bufcampo.bits + 7) / 8;
                uint8_t *b = de_arena(a, 0, cap);
                if (!b) return -1;
                for (uint32_t k = 0; k < n->d.bufcampo.bits; k++) {
                    uint32_t x = n->d.bufcampo.bit + k;
                    if (x / 8 < n->d.bufcampo.n && ((n->d.bufcampo.buf[x / 8] >> (x % 8)) & 1)) b[k / 8] |= (uint8_t)(1u << (k % 8));
                }
                cero(sale, sizeof(*sale)); sale->tipo = AML_BUFFER; sale->n = cap; sale->v.buffer = b;
                return 0;
            }
        case N_ALIAS: return leer_nodo(a, n->d.alias, sale);
        default:
            cero(sale, sizeof(*sale)); sale->tipo = AML_OBJETO; sale->v.nodo = i;
            return 0;
    }
}
/* Store a un objeto con nombre: convierte al tipo que ya tiene (19.3.5). */
static int escribir_nodo(aml *a, int i, const aml_valor *v0) {
    aml_valor v;
    if (desref(a, v0, &v)) return -1;
    nodo *n = &a->nodos[i];
    while (n->tipo == N_ALIAS) { i = n->d.alias; n = &a->nodos[i]; }
    if (n->tipo == N_NOMBRE) {
        aml_valor *d = &n->d.valor;
        if (d->tipo == AML_ENTERO) { uint64_t e; if (a_entero(a, &v, &e)) return -1; d->v.entero = mascara(a, e); return 0; }
        if (d->tipo == AML_BUFFER && (v.tipo == AML_BUFFER || v.tipo == AML_ENTERO || v.tipo == AML_CADENA)) {
            uint8_t t[8]; const uint8_t *s = t; uint32_t k = 8;
            if (v.tipo == AML_BUFFER) { s = v.v.buffer; k = v.n; }
            else if (v.tipo == AML_CADENA) { s = (const uint8_t *)v.v.cadena; k = (uint32_t)largo(v.v.cadena); }
            else for (unsigned j = 0; j < 8; j++) t[j] = (uint8_t)(v.v.entero >> (8 * j));
            for (uint32_t j = 0; j < d->n; j++) d->v.buffer[j] = j < k ? s[j] : 0;
            return 0;
        }
        /* Una cadena que cabe en la que ya hay no gasta memoria: un firmware puede repetir esto en cada lectura. */
        if (d->tipo == AML_CADENA && v.tipo == AML_CADENA && largo(v.v.cadena) <= d->n) { copia(d->v.cadena, v.v.cadena, largo(v.v.cadena) + 1); return 0; }
        /* Los datos de un nombre sobreviven a la evaluacion. */
        return duplicar(a, 1, &v, d);
    }
    if (n->tipo == N_CAMPO || n->tipo == N_CAMPO_INDICE || n->tipo == N_CAMPO_BANCO) {
        uint8_t t[8]; const uint8_t *s = t; uint32_t k = 8;
        if (v.tipo == AML_BUFFER) { s = v.v.buffer; k = v.n; }
        else { uint64_t e; if (a_entero(a, &v, &e)) return -1; for (unsigned j = 0; j < 8; j++) t[j] = (uint8_t)(e >> (8 * j)); }
        return campo_escribir(a, i, s, k);
    }
    if (n->tipo == N_BUFCAMPO) {
        uint8_t t[8]; const uint8_t *s = t; uint32_t k = 8;
        if (v.tipo == AML_BUFFER) { s = v.v.buffer; k = v.n; }
        else { uint64_t e; if (a_entero(a, &v, &e)) return -1; for (unsigned j = 0; j < 8; j++) t[j] = (uint8_t)(e >> (8 * j)); }
        for (uint32_t b = 0; b < n->d.bufcampo.bits; b++) {
            uint32_t x = n->d.bufcampo.bit + b;
            if (x / 8 >= n->d.bufcampo.n) break;
            unsigned bitv = b / 8 < k ? (s[b / 8] >> (b % 8)) & 1 : 0;
            n->d.bufcampo.buf[x / 8] = (uint8_t)((n->d.bufcampo.buf[x / 8] & ~(1u << (x % 8))) | (bitv << (x % 8)));
        }
        return 0;
    }
    return falla(a, "escritura a un objeto que no admite datos", 0, n->tipo, 1);
}
static int guardar_en_ref(aml *a, const aml_valor *ref, const aml_valor *v0) {
    aml_valor v;
    if (desref(a, v0, &v)) return -1;
    if (ref->tipo == AML_REF_BYTE) { uint64_t e; if (a_entero(a, &v, &e)) return -1; *ref->v.ref_byte = (uint8_t)e; return 0; }
    if (ref->tipo == AML_REF) {
        aml_valor *d = ref->v.ref;
        if (d->tipo == AML_REF || d->tipo == AML_REF_BYTE) return guardar_en_ref(a, d, &v);
        return duplicar(a, a->en_carga, &v, d);
    }
    if (ref->tipo == AML_OBJETO) return escribir_nodo(a, ref->v.nodo, &v);
    return falla(a, "destino de escritura invalido", 0, ref->tipo, 1);
}

/* ------------------------------------------------------------------ destinos (SuperName) */
enum { D_NADA, D_LOCAL, D_ARG, D_NODO, D_REF };
typedef struct { uint8_t tipo; int n; aml_valor ref; } destino;
static int leer_destino(aml *a, marco *m, destino *d) {
    cero(d, sizeof(*d));
    if (m->p >= m->fin) return falla(a, "destino truncado", 0, 0, 0);
    uint8_t c = *m->p;
    if (c == 0) { m->p++; return 0; }
    if (c >= 0x60 && c <= 0x67) { m->p++; d->tipo = D_LOCAL; d->n = c - 0x60; return 0; }
    if (c >= 0x68 && c <= 0x6e) { m->p++; d->tipo = D_ARG; d->n = c - 0x68; return 0; }
    if (c == 0x5b && m->p + 1 < m->fin && m->p[1] == 0x31) { m->p += 2; return 0; }   /* Debug */
    if (es_nombre(c)) {
        const uint8_t *r;
        if (saltar_ruta(a, m, &r)) return -1;
        int i = resolver(a, r, m->p, m->ambito, 0, 0, 0);
        if (i < 0) return falla(a, "destino sin definir", 0, 0, 0);
        d->tipo = D_NODO; d->n = i;
        return 0;
    }
    if (termino(a, m, &d->ref)) return -1;
    d->tipo = D_REF;
    return 0;
}
static int guardar(aml *a, marco *m, const destino *d, const aml_valor *v0) {
    aml_valor v;
    switch (d->tipo) {
        case D_NADA: return 0;
        case D_LOCAL: if (desref(a, v0, &v)) return -1; return duplicar(a, 0, &v, &m->local[d->n]);
        case D_ARG:
            if (m->arg[d->n].tipo == AML_REF || m->arg[d->n].tipo == AML_REF_BYTE || m->arg[d->n].tipo == AML_OBJETO) return guardar_en_ref(a, &m->arg[d->n], v0);
            if (desref(a, v0, &v)) return -1;
            return duplicar(a, 0, &v, &m->arg[d->n]);
        case D_NODO: return escribir_nodo(a, d->n, v0);
        default: return guardar_en_ref(a, &d->ref, v0);
    }
}

/* ------------------------------------------------------------------ definiciones */
static int leer_campos(aml *a, marco *m, const uint8_t *fin, uint8_t tipo, int region, int indice, int datos, int banco, uint64_t valor_banco, uint8_t flags) {
    uint32_t bit = 0; uint8_t acceso = flags & 15;
    const uint8_t *fin_prev = m->fin;
    m->fin = fin;
    while (m->p < fin && !a->error) {
        uint8_t c = *m->p;
        if (c == 0) {               /* ReservedField: la longitud cuenta bits, no bytes */
            m->p++;
            const uint8_t *ini = m->p;
            if (m->p >= fin) { falla(a, "campo truncado", 0, 0, 0); break; }
            uint8_t b0 = ini[0]; unsigned ex = b0 >> 6; uint32_t n = ex ? b0 & 15 : b0 & 63;
            if ((size_t)(fin - ini) < 1 + ex) { falla(a, "campo truncado", 0, 0, 0); break; }
            for (unsigned i = 0; i < ex; i++) n |= (uint32_t)ini[1 + i] << (4 + 8 * i);
            m->p = ini + 1 + ex;
            bit += n;
        } else if (c == 1) {        /* AccessField */
            if (fin - m->p < 3) { falla(a, "campo truncado", 0, 0, 0); break; }
            acceso = m->p[1] & 15;
            m->p += 3;
        } else if (c == 2) {        /* ConnectField: GPIO y serie; sin servicio */
            m->p++;
            if (saltar_ruta(a, m, 0)) break;
        } else if (c == 3) {        /* ExtendedAccessField */
            if (fin - m->p < 4) { falla(a, "campo truncado", 0, 0, 0); break; }
            acceso = m->p[1] & 15;
            m->p += 4;
        } else {
            if (fin - m->p < 5) { falla(a, "campo truncado", 0, 0, 0); break; }
            const char *nombre = (const char *)m->p;
            m->p += 4;
            const uint8_t *ini = m->p;
            if (m->p >= fin) { falla(a, "campo truncado", 0, 0, 0); break; }
            uint8_t b0 = ini[0]; unsigned ex = b0 >> 6; uint32_t n = ex ? b0 & 15 : b0 & 63;
            if ((size_t)(fin - ini) < 1 + ex) { falla(a, "campo truncado", 0, 0, 0); break; }
            for (unsigned i = 0; i < ex; i++) n |= (uint32_t)ini[1 + i] << (4 + 8 * i);
            m->p = ini + 1 + ex;
            int h = aml_hijo(a, m->ambito, nombre);
            if (h < 0) h = nuevo_nodo(a, m->ambito, nombre, tipo);
            if (h < 0) break;
            nodo *x = &a->nodos[h];
            x->tipo = tipo;
            x->d.campo.region = region; x->d.campo.indice = indice; x->d.campo.datos = datos; x->d.campo.banco = banco; x->d.campo.valor_banco = valor_banco;
            x->d.campo.bit = bit; x->d.campo.bits = n; x->d.campo.acceso = acceso;
            bit += n;
        }
    }
    m->fin = fin_prev;
    if (a->error) return -1;
    m->p = fin;
    return 0;
}
static int def_ambito(aml *a, marco *m, uint8_t tipo, unsigned extra) {
    const uint8_t *fin;
    if (pkg_longitud(a, m, &fin)) return -1;
    const uint8_t *fin_prev = m->fin;
    int ambito_prev = m->ambito;
    m->fin = fin;
    int i = definir(a, m, tipo, 0);
    if (i < 0 || (size_t)(fin - m->p) < extra) { m->fin = fin_prev; return i < 0 ? -1 : falla(a, "definicion truncada", 0, 0, 0); }
    m->p += extra;
    m->ambito = i;
    int r = lista(a, m);
    m->ambito = ambito_prev;
    m->fin = fin_prev;
    m->p = fin;
    return r;
}
static int def_campo_buffer(aml *a, marco *m, uint32_t bits_fijos, int en_bytes) {
    aml_valor b;
    uint64_t off, bits = bits_fijos;
    if (termino(a, m, &b) || desref(a, &b, &b)) return -1;
    if (evaluar_entero(a, m, &off)) return -1;
    if (!bits_fijos && evaluar_entero(a, m, &bits)) return -1;
    int i = definir(a, m, N_BUFCAMPO, 0);
    if (i < 0) return -1;
    if (b.tipo != AML_BUFFER) return falla(a, "campo creado sobre algo que no es un buffer", 0, b.tipo, 1);
    uint64_t bit = en_bytes ? off * 8 : off;
    if (bit + bits > (uint64_t)b.n * 8 || bits > 1u << 20) return falla(a, "campo creado fuera del buffer", 0, 0, 0);
    nodo *x = &a->nodos[i];
    x->d.bufcampo.buf = b.v.buffer; x->d.bufcampo.n = b.n; x->d.bufcampo.bit = (uint32_t)bit; x->d.bufcampo.bits = (uint32_t)bits;
    return 0;
}

/* ------------------------------------------------------------------ operadores */
static int es_verdad(aml *a, marco *m, int *v) {
    uint64_t e;
    if (evaluar_entero(a, m, &e)) return -1;
    *v = e != 0;
    return 0;
}
static int comparar(aml *a, const aml_valor *x0, const aml_valor *y0, int *cmp) {
    aml_valor x, y;
    if (desref(a, x0, &x) || desref(a, y0, &y)) return -1;
    if (x.tipo == AML_ENTERO) {
        uint64_t b; if (a_entero(a, &y, &b)) return -1;
        *cmp = x.v.entero < b ? -1 : x.v.entero > b;
        return 0;
    }
    if (x.tipo == AML_CADENA || x.tipo == AML_BUFFER) {
        const uint8_t *p = x.tipo == AML_CADENA ? (const uint8_t *)x.v.cadena : x.v.buffer;
        uint32_t np = x.tipo == AML_CADENA ? (uint32_t)largo(x.v.cadena) : x.n;
        uint8_t t[8]; const uint8_t *q; uint32_t nq;
        if (y.tipo == AML_CADENA) { q = (const uint8_t *)y.v.cadena; nq = (uint32_t)largo(y.v.cadena); }
        else if (y.tipo == AML_BUFFER) { q = y.v.buffer; nq = y.n; }
        else { uint64_t e; if (a_entero(a, &y, &e)) return -1; for (unsigned j = 0; j < 8; j++) t[j] = (uint8_t)(e >> (8 * j)); q = t; nq = 8; }
        for (uint32_t i = 0; i < np && i < nq; i++) if (p[i] != q[i]) { *cmp = p[i] < q[i] ? -1 : 1; return 0; }
        *cmp = np < nq ? -1 : np > nq;
        return 0;
    }
    return falla(a, "comparacion de tipos no admitidos", 0, x.tipo, 1);
}
static int op_binario(aml *a, marco *m, uint8_t op, aml_valor *sale) {
    uint64_t x, y, r = 0;
    destino d;
    if (evaluar_entero(a, m, &x) || evaluar_entero(a, m, &y) || leer_destino(a, m, &d)) return -1;
    switch (op) {
        case 0x72: r = x + y; break;
        case 0x74: r = x - y; break;
        case 0x77: r = x * y; break;
        case 0x79: r = y >= 64 ? 0 : x << y; break;
        case 0x7a: r = y >= 64 ? 0 : x >> y; break;
        case 0x7b: r = x & y; break;
        case 0x7c: r = ~(x & y); break;
        case 0x7d: r = x | y; break;
        case 0x7e: r = ~(x | y); break;
        case 0x7f: r = x ^ y; break;
        case 0x85: if (!y) return falla(a, "modulo por cero", 0, 0, 0); r = x % y; break;
    }
    *sale = entero(mascara(a, r));
    return guardar(a, m, &d, sale);
}
static int texto_de(aml *a, const aml_valor *v0, aml_valor *sale, int base) {
    aml_valor v;
    if (desref(a, v0, &v)) return -1;
    if (v.tipo == AML_CADENA) { *sale = v; return 0; }
    char *c = de_arena(a, 0, v.tipo == AML_BUFFER ? v.n * 5 + 8 : 32);
    if (!c) return -1;
    size_t o = 0, cap = v.tipo == AML_BUFFER ? v.n * 5 + 8 : 32;
    if (v.tipo == AML_BUFFER) {
        for (uint32_t i = 0; i < v.n; i++) {
            if (i) texto(c, cap, &o, ",");
            if (base == 16) texto(c, cap, &o, "0x");
            numero(c, cap, &o, v.v.buffer[i], (unsigned)base);
        }
    } else {
        uint64_t e;
        if (a_entero(a, &v, &e)) return -1;
        if (base == 16) texto(c, cap, &o, "0x");
        numero(c, cap, &o, e, (unsigned)base);
    }
    cero(sale, sizeof(*sale)); sale->tipo = AML_CADENA; sale->v.cadena = c; sale->n = (uint32_t)o;
    return 0;
}
static int concatenar(aml *a, const aml_valor *x0, const aml_valor *y0, aml_valor *sale) {
    aml_valor x, y;
    if (desref(a, x0, &x) || desref(a, y0, &y)) return -1;
    cero(sale, sizeof(*sale));
    if (x.tipo == AML_CADENA) {
        aml_valor t;
        if (texto_de(a, &y, &t, 16)) return -1;
        size_t nx = largo(x.v.cadena), ny = largo(t.v.cadena);
        char *c = de_arena(a, 0, nx + ny + 1);
        if (!c) return -1;
        copia(c, x.v.cadena, nx); copia(c + nx, t.v.cadena, ny + 1);
        sale->tipo = AML_CADENA; sale->v.cadena = c; sale->n = (uint32_t)(nx + ny);
        return 0;
    }
    uint8_t tx[8], ty[8]; const uint8_t *px = tx, *py = ty; uint32_t nx = a->ancho64 ? 8 : 4, ny = nx;
    if (x.tipo == AML_BUFFER) { px = x.v.buffer; nx = x.n; } else { uint64_t e; if (a_entero(a, &x, &e)) return -1; for (unsigned j = 0; j < 8; j++) tx[j] = (uint8_t)(e >> (8 * j)); }
    if (y.tipo == AML_BUFFER) { py = y.v.buffer; ny = y.n; } else if (y.tipo == AML_CADENA) { py = (const uint8_t *)y.v.cadena; ny = (uint32_t)largo(y.v.cadena); } else { uint64_t e; if (a_entero(a, &y, &e)) return -1; for (unsigned j = 0; j < 8; j++) ty[j] = (uint8_t)(e >> (8 * j)); }
    uint8_t *b = de_arena(a, 0, nx + ny + 1);
    if (!b) return -1;
    copia(b, px, nx); copia(b + nx, py, ny);
    sale->tipo = AML_BUFFER; sale->v.buffer = b; sale->n = nx + ny;
    return 0;
}
static int coincide(aml *a, unsigned op, const aml_valor *e, const aml_valor *ref) {
    int c;
    if (e->tipo == AML_NADA) return 0;
    if (comparar(a, e, ref, &c)) { a->error = 0; return 0; }
    switch (op) { case 0: return 1; case 1: return c == 0; case 2: return c <= 0; case 3: return c < 0; case 4: return c >= 0; case 5: return c > 0; }
    return 0;
}
static int es_osi(aml *a, const char *s) {
    (void)a;
    /* Lo que responde un Windows actual: algunos firmwares solo ofrecen sus metodos a quien se identifica asi. Igual que Linux. */
    static const char *const si[] = {"Windows 2000", "Windows 2001", "Windows 2001 SP1", "Windows 2001 SP2", "Windows 2001.1", "Windows 2006", "Windows 2006 SP1",
        "Windows 2006.1", "Windows 2009", "Windows 2012", "Windows 2013", "Windows 2015", "Windows 2016", "Windows 2017", "Windows 2017.2", "Windows 2018", "Windows 2018.2", "Windows 2019", "Windows 2020", 0};
    for (unsigned i = 0; si[i]; i++) {
        const char *p = si[i], *q = s;
        while (*p && *p == *q) { p++; q++; }
        if (!*p && !*q) return 1;
    }
    return 0;
}

static int termino_ext(aml *a, marco *m, aml_valor *sale) {
    if (m->p >= m->fin) return falla(a, "operador truncado", 0, 0, 0);
    uint8_t op = *m->p++;
    cero(sale, sizeof(*sale));
    switch (op) {
        case 0x01: {  /* Mutex */
            int i = definir(a, m, N_MUTEX, 0);
            if (i < 0 || m->p >= m->fin) return i < 0 ? -1 : falla(a, "mutex truncado", 0, 0, 0);
            m->p++;
            return 0;
        }
        case 0x02: { int i = definir(a, m, N_MUTEX, 0); return i < 0 ? -1 : 0; }  /* Event */
        case 0x12: {  /* CondRefOf */
            const uint8_t *r;
            destino d;
            if (m->p < m->fin && es_nombre(*m->p)) {
                if (saltar_ruta(a, m, &r)) return -1;
                int i = resolver(a, r, m->p, m->ambito, 0, 0, 0);
                if (leer_destino(a, m, &d)) return -1;
                if (i < 0) { *sale = entero(0); return 0; }
                aml_valor o; cero(&o, sizeof(o)); o.tipo = AML_OBJETO; o.v.nodo = i;
                if (guardar(a, m, &d, &o)) return -1;
                *sale = entero(1);
                return 0;
            }
            aml_valor t;
            if (termino(a, m, &t) || leer_destino(a, m, &d)) return -1;
            if (guardar(a, m, &d, &t)) return -1;
            *sale = entero(1);
            return 0;
        }
        case 0x13: return def_campo_buffer(a, m, 0, 0);
        case 0x1f: case 0x20: return falla(a, "carga dinamica de tablas sin servicio", 0, 0, 0);
        case 0x21: { uint64_t us; if (evaluar_entero(a, m, &us)) return -1; if (a->h.esperar_ms && us >= 1000) a->h.esperar_ms((unsigned)(us / 1000)); return 0; }
        case 0x22: { uint64_t ms; if (evaluar_entero(a, m, &ms)) return -1; if (a->h.esperar_ms) a->h.esperar_ms(ms > 100 ? 100 : (unsigned)ms); return 0; }
        case 0x23: {  /* Acquire: un solo hilo, siempre se concede */
            aml_valor t;
            if (termino(a, m, &t)) return -1;
            if ((size_t)(m->fin - m->p) < 2) return falla(a, "Acquire truncado", 0, 0, 0);
            m->p += 2;
            *sale = entero(0);
            return 0;
        }
        case 0x24: case 0x26: case 0x27: { aml_valor t; return termino(a, m, &t); }  /* Signal, Reset, Release */
        case 0x25: { aml_valor t; uint64_t e; if (termino(a, m, &t) || evaluar_entero(a, m, &e)) return -1; *sale = entero(1); return 0; }  /* Wait: sin eventos */
        case 0x28: { uint64_t e, r = 0, f = 1; destino d; if (evaluar_entero(a, m, &e) || leer_destino(a, m, &d)) return -1; for (; e; e >>= 4, f *= 10) r += (e & 15) * f; *sale = entero(r); return guardar(a, m, &d, sale); }
        case 0x29: { uint64_t e, r = 0; unsigned s = 0; destino d; if (evaluar_entero(a, m, &e) || leer_destino(a, m, &d)) return -1; for (; e && s < 64; e /= 10, s += 4) r |= (e % 10) << s; *sale = entero(r); return guardar(a, m, &d, sale); }
        case 0x30: *sale = entero(2); return 0;   /* Revision */
        case 0x31: return 0;                      /* Debug */
        case 0x32: return falla(a, "el firmware ejecuto Fatal", 0, 0, 0);
        case 0x33: *sale = entero(a->h.ms ? a->h.ms() * 10000 : 0); return 0;
        case 0x80: {  /* OperationRegion */
            int i = definir(a, m, N_REGION, 0);
            if (i < 0 || m->p >= m->fin) return i < 0 ? -1 : falla(a, "region truncada", 0, 0, 0);
            uint8_t esp = *m->p++;
            uint64_t base = 0, n = 0;
            /* Sus limites pueden nombrar algo aun sin cargar: entonces queda sin tamano y leerla falla con su nombre. */
            if (evaluar_entero(a, m, &base)) { if (!a->en_carga) return -1; a->error = 0; a->motivo[0] = 0; base = 0; }
            if (evaluar_entero(a, m, &n)) { if (!a->en_carga) return -1; a->error = 0; a->motivo[0] = 0; n = 0; }
            nodo *x = &a->nodos[i];
            x->d.region.espacio = esp; x->d.region.base = base; x->d.region.n = n;
            return 0;
        }
        case 0x81: {  /* Field */
            const uint8_t *fin;
            if (pkg_longitud(a, m, &fin)) return -1;
            const uint8_t *r, *fin_p = m->fin;
            m->fin = fin;
            int e = saltar_ruta(a, m, &r);
            int reg = e ? -1 : resolver(a, r, m->p, m->ambito, 0, 0, 0);
            m->fin = fin_p;
            if (e) return -1;
            if (m->p >= fin) return falla(a, "campo truncado", 0, 0, 0);
            uint8_t fl = *m->p++;
            if (reg < 0 || a->nodos[reg].tipo != N_REGION) { m->p = fin; return 0; }   /* Region en una tabla aun sin cargar: se ignora */
            return leer_campos(a, m, fin, N_CAMPO, reg, -1, -1, -1, 0, fl);
        }
        case 0x82: return def_ambito(a, m, N_DISPOSITIVO, 0);
        case 0x83: return def_ambito(a, m, N_AMBITO, 6);       /* Processor */
        case 0x84: return def_ambito(a, m, N_AMBITO, 3);       /* PowerResource */
        case 0x85: return def_ambito(a, m, N_DISPOSITIVO, 0);  /* ThermalZone */
        case 0x86: {  /* IndexField */
            const uint8_t *fin, *fin_p = m->fin, *r1, *r2;
            if (pkg_longitud(a, m, &fin)) return -1;
            m->fin = fin;
            if (saltar_ruta(a, m, &r1)) { m->fin = fin_p; return -1; }
            const uint8_t *f1 = m->p;
            if (saltar_ruta(a, m, &r2)) { m->fin = fin_p; return -1; }
            m->fin = fin_p;
            if (m->p >= fin) return falla(a, "campo truncado", 0, 0, 0);
            uint8_t fl = *m->p++;
            int i1 = resolver(a, r1, f1, m->ambito, 0, 0, 0);
            int i2 = resolver(a, r2, m->p - 1, m->ambito, 0, 0, 0);
            return leer_campos(a, m, fin, N_CAMPO_INDICE, -1, i1, i2, -1, 0, fl);
        }
        case 0x87: {  /* BankField */
            const uint8_t *fin, *fin_p = m->fin, *r1, *r2;
            if (pkg_longitud(a, m, &fin)) return -1;
            m->fin = fin;
            if (saltar_ruta(a, m, &r1) || saltar_ruta(a, m, &r2)) { m->fin = fin_p; return -1; }
            uint64_t v;
            if (evaluar_entero(a, m, &v)) { m->fin = fin_p; return -1; }
            m->fin = fin_p;
            if (m->p >= fin) return falla(a, "campo truncado", 0, 0, 0);
            uint8_t fl = *m->p++;
            return leer_campos(a, m, fin, N_CAMPO_BANCO, -1, -1, -1, 0, v, fl);
        }
        case 0x88: {  /* DataTableRegion */
            aml_valor t;
            int i = definir(a, m, N_REGION, 0);
            if (i < 0 || termino(a, m, &t) || termino(a, m, &t) || termino(a, m, &t)) return -1;
            a->nodos[i].d.region.espacio = 0xff;
            return 0;
        }
        default:
            return falla(a, "operador extendido sin servicio", 0, op, 1);
    }
}

static int evaluar_ruta(aml *a, marco *m, aml_valor *sale) {
    const uint8_t *r;
    if (saltar_ruta(a, m, &r)) return -1;
    int i = resolver(a, r, m->p, m->ambito, 0, 0, 0);
    if (i < 0) {
        char t[8]; unsigned k = 0;
        const uint8_t *q = r;
        while (q < m->p && !es_inicio_nombre(*q)) q++;
        while (q < m->p && k < 4) t[k++] = (char)*q++;
        t[k] = 0;
        return falla(a, "nombre sin definir: ", t, 0, 0);
    }
    const nodo *n = &a->nodos[i];
    if (n->tipo == N_METODO) {
        aml_valor args[7];
        unsigned k = n->d.metodo.args;
        for (unsigned j = 0; j < k; j++) if (termino(a, m, &args[j])) return -1;
        return llamar(a, i, args, k, sale);
    }
    return leer_nodo(a, i, sale);
}

static int termino(aml *a, marco *m, aml_valor *sale) {
    if (a->error) return -1;
    if (++a->pasos > a->pasos_limite) return falla(a, "el metodo no termino: limite de pasos", 0, 0, 0);
    if (m->p >= m->fin) return falla(a, "fin inesperado de la tabla", 0, 0, 0);
    if (++a->profundidad > TERMINOS_MAX) { a->profundidad--; return falla(a, "demasiado anidamiento", 0, 0, 0); }
    int rc = 0;
    uint8_t op = *m->p;
    cero(sale, sizeof(*sale));
    if (es_nombre(op)) { rc = evaluar_ruta(a, m, sale); a->profundidad--; return rc; }
    m->p++;
    switch (op) {
        case 0x00: *sale = entero(0); break;
        case 0x01: *sale = entero(1); break;
        case 0xff: *sale = entero(a->ancho64 ? UINT64_MAX : 0xffffffffu); break;
        case 0x0a: { uint64_t v; rc = leer_le(a, m, 1, &v); *sale = entero(v); break; }
        case 0x0b: { uint64_t v; rc = leer_le(a, m, 2, &v); *sale = entero(v); break; }
        case 0x0c: { uint64_t v; rc = leer_le(a, m, 4, &v); *sale = entero(v); break; }
        case 0x0e: { uint64_t v; rc = leer_le(a, m, 8, &v); *sale = entero(mascara(a, v)); break; }
        case 0x0d: {
            const uint8_t *q = m->p;
            while (q < m->fin && *q) q++;
            if (q >= m->fin) { rc = falla(a, "cadena sin terminar", 0, 0, 0); break; }
            size_t n = (size_t)(q - m->p);
            char *c = de_arena(a, ARENA, n + 1);
            if (!c) { rc = -1; break; }
            copia(c, m->p, n);
            sale->tipo = AML_CADENA; sale->v.cadena = c; sale->n = (uint32_t)n;
            m->p = q + 1;
            break;
        }
        case 0x06: {  /* Alias */
            const uint8_t *r;
            if (saltar_ruta(a, m, &r)) { rc = -1; break; }
            const uint8_t *fr = m->p;
            int d = definir(a, m, N_ALIAS, 0);
            if (d < 0) { rc = -1; break; }
            int o = resolver(a, r, fr, m->ambito, 0, 0, 0);
            if (o < 0) a->nodos[d].tipo = N_AMBITO;
            else a->nodos[d].d.alias = o;
            break;
        }
        case 0x08: {  /* Name */
            int i = definir(a, m, N_NOMBRE, 0);
            if (i < 0) { rc = -1; break; }
            aml_valor v;
            if (termino(a, m, &v)) { rc = -1; break; }
            /* Fuera de la carga es un objeto local de un metodo: vive lo que dure esta evaluacion (19.6.87). */
            a->nodos[i].d.valor = v;
            break;
        }
        case 0x10: rc = def_ambito(a, m, N_AMBITO, 0); break;
        case 0x11: {  /* Buffer */
            const uint8_t *fin, *fin_p = m->fin;
            if (pkg_longitud(a, m, &fin)) { rc = -1; break; }
            m->fin = fin;
            uint64_t tam;
            if (evaluar_entero(a, m, &tam)) { m->fin = fin_p; rc = -1; break; }
            m->fin = fin_p;
            size_t hay = (size_t)(fin - m->p);
            if (tam > 1u << 20 || hay > 1u << 20) { rc = falla(a, "buffer excesivo", 0, tam, 1); break; }
            uint32_t n = (uint32_t)(tam > hay ? tam : hay);
            uint8_t *b = de_arena(a, ARENA, n ? n : 1);
            if (!b) { rc = -1; break; }
            copia(b, m->p, hay);
            m->p = fin;
            sale->tipo = AML_BUFFER; sale->v.buffer = b; sale->n = n;
            break;
        }
        case 0x12: case 0x13: {  /* Package, VarPackage */
            const uint8_t *fin, *fin_p = m->fin;
            if (pkg_longitud(a, m, &fin)) { rc = -1; break; }
            m->fin = fin;
            uint64_t tam;
            if (op == 0x12) { if (m->p >= fin) { m->fin = fin_p; rc = falla(a, "paquete truncado", 0, 0, 0); break; } tam = *m->p++; }
            else if (evaluar_entero(a, m, &tam)) { m->fin = fin_p; rc = -1; break; }
            if (tam > 65536) { m->fin = fin_p; rc = falla(a, "paquete excesivo", 0, tam, 1); break; }
            aml_valor *e = de_arena(a, ARENA, sizeof(aml_valor) * (tam ? tam : 1));
            if (!e) { m->fin = fin_p; rc = -1; break; }
            for (uint64_t i = 0; m->p < fin && !rc; i++) {
                if (i >= tam) { rc = falla(a, "paquete con mas elementos que su tamano", 0, 0, 0); break; }
                /* Un nombre en un paquete no se evalua: se resuelve al usarlo. */
                if (es_nombre(*m->p)) {
                    const uint8_t *r;
                    rc = saltar_ruta(a, m, &r);
                    if (rc) break;
                    e[i].tipo = AML_RUTA; e[i].v.ruta = r; e[i].ruta_ambito = m->ambito;
                } else rc = termino(a, m, &e[i]);
            }
            m->fin = fin_p;
            if (rc) break;
            m->p = fin;
            sale->tipo = AML_PAQUETE; sale->v.paquete = e; sale->n = (uint32_t)tam;
            break;
        }
        case 0x14: {  /* Method */
            const uint8_t *fin, *fin_p = m->fin;
            if (pkg_longitud(a, m, &fin)) { rc = -1; break; }
            m->fin = fin;
            int i = definir(a, m, N_METODO, 0);
            m->fin = fin_p;
            if (i < 0 || m->p >= fin) { rc = i < 0 ? -1 : falla(a, "metodo truncado", 0, 0, 0); break; }
            uint8_t fl = *m->p++;
            nodo *x = &a->nodos[i];
            x->d.metodo.args = fl & 7; x->d.metodo.ini = m->p; x->d.metodo.n = (uint32_t)(fin - m->p); x->d.metodo.ancho64 = a->ancho64;
            m->p = fin;
            break;
        }
        case 0x15: {  /* External: solo informa */
            if (saltar_ruta(a, m, 0)) { rc = -1; break; }
            if ((size_t)(m->fin - m->p) < 2) { rc = falla(a, "External truncado", 0, 0, 0); break; }
            m->p += 2;
            break;
        }
        case 0x5b: rc = termino_ext(a, m, sale); break;
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
            if (!m->es_metodo) { rc = falla(a, "local fuera de un metodo", 0, 0, 0); break; }
            sale->tipo = AML_REF; sale->v.ref = &m->local[op - 0x60];
            break;
        case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: case 0x6e:
            if (!m->es_metodo) { rc = falla(a, "argumento fuera de un metodo", 0, 0, 0); break; }
            sale->tipo = AML_REF; sale->v.ref = &m->arg[op - 0x68];
            break;
        case 0x70: {  /* Store */
            aml_valor v; destino d;
            if (termino(a, m, &v) || leer_destino(a, m, &d) || guardar(a, m, &d, &v)) { rc = -1; break; }
            *sale = v;
            break;
        }
        case 0x71: {  /* RefOf */
            if (m->p < m->fin && es_nombre(*m->p)) {
                const uint8_t *r;
                if (saltar_ruta(a, m, &r)) { rc = -1; break; }
                int i = resolver(a, r, m->p, m->ambito, 0, 0, 0);
                if (i < 0) { rc = falla(a, "RefOf de un nombre sin definir", 0, 0, 0); break; }
                sale->tipo = AML_OBJETO; sale->v.nodo = i;
            } else rc = termino(a, m, sale);
            break;
        }
        case 0x72: case 0x74: case 0x77: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: case 0x85:
            rc = op_binario(a, m, op, sale);
            break;
        case 0x73: case 0x84: {  /* Concat, ConcatRes */
            aml_valor x, y, r; destino d;
            if (termino(a, m, &x) || termino(a, m, &y) || leer_destino(a, m, &d) || concatenar(a, &x, &y, &r) || guardar(a, m, &d, &r)) { rc = -1; break; }
            *sale = r;
            break;
        }
        case 0x75: case 0x76: {  /* Increment, Decrement */
            size_t pos = (size_t)(m->fin - m->p);
            (void)pos;
            const uint8_t *ini = m->p;
            destino d; uint64_t e;
            if (leer_destino(a, m, &d)) { rc = -1; break; }
            const uint8_t *tras = m->p;
            m->p = ini;
            if (evaluar_entero(a, m, &e)) { rc = -1; break; }
            m->p = tras;
            *sale = entero(mascara(a, op == 0x75 ? e + 1 : e - 1));
            rc = guardar(a, m, &d, sale);
            break;
        }
        case 0x78: {  /* Divide */
            uint64_t x, y; destino r, q;
            if (evaluar_entero(a, m, &x) || evaluar_entero(a, m, &y) || leer_destino(a, m, &r) || leer_destino(a, m, &q)) { rc = -1; break; }
            if (!y) { rc = falla(a, "division por cero", 0, 0, 0); break; }
            aml_valor vr = entero(x % y);
            *sale = entero(x / y);
            if (guardar(a, m, &r, &vr) || guardar(a, m, &q, sale)) rc = -1;
            break;
        }
        case 0x80: { uint64_t x; destino d; if (evaluar_entero(a, m, &x) || leer_destino(a, m, &d)) { rc = -1; break; } *sale = entero(mascara(a, ~x)); rc = guardar(a, m, &d, sale); break; }
        case 0x81: case 0x82: {
            uint64_t x; destino d; unsigned r = 0;
            if (evaluar_entero(a, m, &x) || leer_destino(a, m, &d)) { rc = -1; break; }
            if (x) { if (op == 0x81) { r = 64; while (!(x >> (r - 1))) r--; } else { r = 1; while (!((x >> (r - 1)) & 1)) r++; } }
            *sale = entero(r);
            rc = guardar(a, m, &d, sale);
            break;
        }
        case 0x83: {  /* DerefOf */
            aml_valor v;
            if (termino(a, m, &v)) { rc = -1; break; }
            int era_ref = v.tipo == AML_REF || v.tipo == AML_REF_BYTE;
            if (desref(a, &v, sale)) { rc = -1; break; }
            if (sale->tipo == AML_OBJETO) { int i = sale->v.nodo; rc = leer_nodo(a, i, sale); }
            else if (sale->tipo == AML_CADENA && !era_ref) {
                /* Una cadena escrita aqui mismo nombra un objeto; la que sale de un paquete o de un local es solo su valor. */
                int i = aml_buscar(a, sale->v.cadena);
                if (i < 0) { rc = falla(a, "DerefOf de una ruta sin definir", 0, 0, 0); break; }
                rc = leer_nodo(a, i, sale);
            }
            break;
        }
        case 0x86: {  /* Notify: nadie escucha, y su destino puede estar en una tabla que no se cargo */
            aml_valor o; uint64_t v;
            if (m->p < m->fin && es_nombre(*m->p)) { if (saltar_ruta(a, m, 0)) { rc = -1; break; } }
            else if (termino(a, m, &o)) { rc = -1; break; }
            if (evaluar_entero(a, m, &v)) rc = -1;
            break;
        }
        case 0x87: {  /* SizeOf */
            aml_valor v;
            if (termino(a, m, &v) || desref(a, &v, &v)) { rc = -1; break; }
            if (v.tipo == AML_CADENA) *sale = entero(largo(v.v.cadena));
            else if (v.tipo == AML_BUFFER || v.tipo == AML_PAQUETE) *sale = entero(v.n);
            else rc = falla(a, "SizeOf de un tipo no admitido", 0, v.tipo, 1);
            break;
        }
        case 0x88: {  /* Index */
            aml_valor o; uint64_t i; destino d;
            if (termino(a, m, &o) || desref(a, &o, &o) || evaluar_entero(a, m, &i) || leer_destino(a, m, &d)) { rc = -1; break; }
            if (o.tipo == AML_PAQUETE) {
                if (i >= o.n) { rc = falla(a, "Index fuera del paquete", 0, i, 1); break; }
                sale->tipo = AML_REF; sale->v.ref = &o.v.paquete[i];
            } else if (o.tipo == AML_BUFFER || o.tipo == AML_CADENA) {
                uint32_t n = o.tipo == AML_BUFFER ? o.n : (uint32_t)largo(o.v.cadena);
                if (i >= n) { rc = falla(a, "Index fuera del buffer", 0, i, 1); break; }
                sale->tipo = AML_REF_BYTE; sale->v.ref_byte = (o.tipo == AML_BUFFER ? o.v.buffer : (uint8_t *)o.v.cadena) + i;
            } else { rc = falla(a, "Index de un tipo no admitido", 0, o.tipo, 1); break; }
            if (d.tipo == D_LOCAL) m->local[d.n] = *sale;
            else if (d.tipo != D_NADA) rc = guardar(a, m, &d, sale);
            break;
        }
        case 0x89: {  /* Match */
            aml_valor o, r1, r2; uint64_t ini;
            if (termino(a, m, &o) || desref(a, &o, &o)) { rc = -1; break; }
            if ((size_t)(m->fin - m->p) < 1) { rc = falla(a, "Match truncado", 0, 0, 0); break; }
            unsigned op1 = *m->p++;
            if (termino(a, m, &r1)) { rc = -1; break; }
            if ((size_t)(m->fin - m->p) < 1) { rc = falla(a, "Match truncado", 0, 0, 0); break; }
            unsigned op2 = *m->p++;
            if (termino(a, m, &r2) || evaluar_entero(a, m, &ini)) { rc = -1; break; }
            if (o.tipo != AML_PAQUETE) { rc = falla(a, "Match sobre algo que no es un paquete", 0, 0, 0); break; }
            *sale = entero(mascara(a, UINT64_MAX));
            for (uint64_t i = ini; i < o.n; i++) if (coincide(a, op1, &o.v.paquete[i], &r1) && coincide(a, op2, &o.v.paquete[i], &r2)) { *sale = entero(i); break; }
            break;
        }
        case 0x8a: rc = def_campo_buffer(a, m, 32, 1); break;
        case 0x8b: rc = def_campo_buffer(a, m, 16, 1); break;
        case 0x8c: rc = def_campo_buffer(a, m, 8, 1); break;
        case 0x8d: rc = def_campo_buffer(a, m, 1, 0); break;
        case 0x8f: rc = def_campo_buffer(a, m, 64, 1); break;
        case 0x8e: {  /* ObjectType */
            aml_valor v;
            if (termino(a, m, &v) || desref(a, &v, &v)) { rc = -1; break; }
            static const uint8_t t[] = {0, 1, 2, 3, 4, 6};
            *sale = entero(v.tipo <= AML_OBJETO ? t[v.tipo] : 0);
            break;
        }
        case 0x90: case 0x91: {
            int x, y;
            if (es_verdad(a, m, &x) || es_verdad(a, m, &y)) { rc = -1; break; }
            *sale = entero((op == 0x90 ? x && y : x || y) ? mascara(a, UINT64_MAX) : 0);
            break;
        }
        case 0x92: {
            int x;
            if (es_verdad(a, m, &x)) { rc = -1; break; }
            *sale = entero(!x ? mascara(a, UINT64_MAX) : 0);
            break;
        }
        case 0x93: case 0x94: case 0x95: {
            aml_valor x, y; int c;
            if (termino(a, m, &x) || termino(a, m, &y) || comparar(a, &x, &y, &c)) { rc = -1; break; }
            int r = op == 0x93 ? c == 0 : op == 0x94 ? c > 0 : c < 0;
            *sale = entero(r ? mascara(a, UINT64_MAX) : 0);
            break;
        }
        case 0x96: {  /* ToBuffer */
            aml_valor v; destino d;
            if (termino(a, m, &v) || desref(a, &v, &v) || leer_destino(a, m, &d)) { rc = -1; break; }
            if (v.tipo == AML_BUFFER) *sale = v;
            else if (v.tipo == AML_CADENA) { sale->tipo = AML_BUFFER; sale->v.buffer = (uint8_t *)v.v.cadena; sale->n = (uint32_t)largo(v.v.cadena) + 1; }
            else {
                uint64_t e;
                if (a_entero(a, &v, &e)) { rc = -1; break; }
                uint32_t n = a->ancho64 ? 8 : 4;
                uint8_t *b = de_arena(a, 0, n);
                if (!b) { rc = -1; break; }
                for (uint32_t j = 0; j < n; j++) b[j] = (uint8_t)(e >> (8 * j));
                sale->tipo = AML_BUFFER; sale->v.buffer = b; sale->n = n;
            }
            rc = guardar(a, m, &d, sale);
            break;
        }
        case 0x97: case 0x98: {  /* ToDecimalString, ToHexString */
            aml_valor v; destino d;
            if (termino(a, m, &v) || leer_destino(a, m, &d) || texto_de(a, &v, sale, op == 0x97 ? 10 : 16)) { rc = -1; break; }
            rc = guardar(a, m, &d, sale);
            break;
        }
        case 0x99: { uint64_t e; destino d; if (evaluar_entero(a, m, &e) || leer_destino(a, m, &d)) { rc = -1; break; } *sale = entero(e); rc = guardar(a, m, &d, sale); break; }
        case 0x9c: {  /* ToString */
            aml_valor v; uint64_t lim; destino d;
            if (termino(a, m, &v) || desref(a, &v, &v) || evaluar_entero(a, m, &lim) || leer_destino(a, m, &d)) { rc = -1; break; }
            if (v.tipo != AML_BUFFER) { rc = falla(a, "ToString de algo que no es un buffer", 0, 0, 0); break; }
            uint32_t n = 0;
            while (n < v.n && n < lim && v.v.buffer[n]) n++;
            char *c = de_arena(a, 0, n + 1);
            if (!c) { rc = -1; break; }
            copia(c, v.v.buffer, n);
            sale->tipo = AML_CADENA; sale->v.cadena = c; sale->n = n;
            rc = guardar(a, m, &d, sale);
            break;
        }
        case 0x9d: {  /* CopyObject: sin conversion de tipo */
            aml_valor v; destino d;
            if (termino(a, m, &v) || desref(a, &v, &v) || leer_destino(a, m, &d)) { rc = -1; break; }
            if (d.tipo == D_NODO && a->nodos[d.n].tipo == N_NOMBRE) rc = duplicar(a, 1, &v, &a->nodos[d.n].d.valor);
            else rc = guardar(a, m, &d, &v);
            *sale = v;
            break;
        }
        case 0x9e: {  /* Mid */
            aml_valor v; uint64_t i, n; destino d;
            if (termino(a, m, &v) || desref(a, &v, &v) || evaluar_entero(a, m, &i) || evaluar_entero(a, m, &n) || leer_destino(a, m, &d)) { rc = -1; break; }
            uint32_t tot = v.tipo == AML_BUFFER ? v.n : v.tipo == AML_CADENA ? (uint32_t)largo(v.v.cadena) : 0;
            if (i > tot) i = tot;
            if (n > tot - i) n = tot - i;
            uint8_t *b = de_arena(a, 0, n + 1);
            if (!b) { rc = -1; break; }
            copia(b, (v.tipo == AML_BUFFER ? v.v.buffer : (const uint8_t *)v.v.cadena) + i, n);
            sale->tipo = v.tipo == AML_BUFFER ? AML_BUFFER : AML_CADENA;
            if (v.tipo == AML_BUFFER) sale->v.buffer = b; else sale->v.cadena = (char *)b;
            sale->n = (uint32_t)n;
            rc = guardar(a, m, &d, sale);
            break;
        }
        case 0x9f: m->salida = 3; break;
        case 0xa0: {  /* If, y su Else si lo hay */
            const uint8_t *fin;
            if (pkg_longitud(a, m, &fin)) { rc = -1; break; }
            const uint8_t *fin_p = m->fin;
            m->fin = fin;
            int c;
            if (es_verdad(a, m, &c)) {
                m->fin = fin_p;
                /* Durante la carga un If puede depender de algo aun sin definir: se salta y sigue. */
                if (a->en_carga) { a->error = 0; a->motivo[0] = 0; m->p = fin; c = -1; }
                else { rc = -1; break; }
            }
            if (c == 1) {
                rc = lista(a, m);
                m->fin = fin_p;
                if (rc) break;
                m->p = fin;
            } else if (c == 0) { m->fin = fin_p; m->p = fin; }
            if (m->salida) {
                /* Salta el Else para seguir la salida. */
                if (m->p < m->fin && *m->p == 0xa1) { const uint8_t *f2; m->p++; if (pkg_longitud(a, m, &f2)) { rc = -1; break; } m->p = f2; }
                break;
            }
            if (m->p < m->fin && *m->p == 0xa1) {
                m->p++;
                const uint8_t *f2;
                if (pkg_longitud(a, m, &f2)) { rc = -1; break; }
                if (c == 0) {
                    const uint8_t *fp = m->fin;
                    m->fin = f2;
                    rc = lista(a, m);
                    m->fin = fp;
                    if (rc) break;
                }
                m->p = f2;
            }
            break;
        }
        case 0xa1: { const uint8_t *fin; if (pkg_longitud(a, m, &fin)) { rc = -1; break; } m->p = fin; break; }  /* Else suelto */
        case 0xa2: {  /* While */
            const uint8_t *fin;
            if (pkg_longitud(a, m, &fin)) { rc = -1; break; }
            const uint8_t *cuerpo = m->p, *fin_p = m->fin;
            for (;;) {
                m->p = cuerpo; m->fin = fin;
                int c;
                if (es_verdad(a, m, &c)) { rc = -1; break; }
                if (!c) break;
                if (lista(a, m)) { rc = -1; break; }
                if (m->salida == 2) { m->salida = 0; break; }
                if (m->salida == 3) m->salida = 0;
                if (m->salida == 1) break;
            }
            m->fin = fin_p;
            if (!rc) m->p = fin;
            break;
        }
        case 0xa3: case 0xcc: break;     /* Noop, BreakPoint */
        case 0xa4: {
            aml_valor v;
            if (termino(a, m, &v)) { rc = -1; break; }
            if (desref(a, &v, &m->retorno)) { rc = -1; break; }
            m->salida = 1;
            break;
        }
        case 0xa5: m->salida = 2; break;
        default:
            rc = falla(a, "operador sin servicio", 0, op, 1);
            break;
    }
    a->profundidad--;
    return rc ? -1 : a->error ? -1 : 0;
}

static int lista(aml *a, marco *m) {
    while (m->p < m->fin && !m->salida) {
        aml_valor v;
        if (termino(a, m, &v)) return -1;
    }
    return a->error ? -1 : 0;
}

static int metodo_interno(aml *a, const nodo *n, const aml_valor *args, unsigned nargs, aml_valor *sale, int *hecho) {
    /* Los que el sistema operativo aporta. */
    *hecho = 1;
    if (a->nodos[0].hijo >= 0 && iguales4(n->nombre, "_OSI")) {
        aml_valor v;
        if (!nargs || desref(a, &args[0], &v) || v.tipo != AML_CADENA) return falla(a, "_OSI sin cadena", 0, 0, 0);
        *sale = entero(es_osi(a, v.v.cadena) ? mascara(a, UINT64_MAX) : 0);
        return 0;
    }
    *hecho = 0;
    return 0;
}
static int llamar(aml *a, int metodo, const aml_valor *args, unsigned nargs, aml_valor *sale) {
    const nodo *n = &a->nodos[metodo];
    if (n->d.metodo.ini == 0) {
        int hecho;
        int r = metodo_interno(a, n, args, nargs, sale, &hecho);
        if (hecho) return r;
        return falla(a, "metodo interno sin servicio", 0, 0, 0);
    }
    if (++a->metodos > METODOS_MAX) { a->metodos--; return falla(a, "demasiadas llamadas anidadas", 0, 0, 0); }
    marco *f = de_arena(a, 0, sizeof(marco));
    if (!f) { a->metodos--; return -1; }
    f->p = n->d.metodo.ini; f->fin = n->d.metodo.ini + n->d.metodo.n; f->ambito = metodo; f->es_metodo = 1;
    for (unsigned i = 0; i < nargs && i < 7; i++) {
        /* Se pasa el valor actual: lo que apunte a un local del que llama no puede sobrevivir a su evaluacion de otra forma. */
        if (args[i].tipo == AML_REF) { if (desref(a, &args[i], &f->arg[i])) { a->metodos--; return -1; } }
        else f->arg[i] = args[i];
    }
    uint8_t ancho = a->ancho64;
    a->ancho64 = n->d.metodo.ancho64;
    int r = lista(a, f);
    a->ancho64 = ancho;
    a->metodos--;
    if (r) return -1;
    if (f->salida == 1) *sale = f->retorno;
    else cero(sale, sizeof(*sale));
    return 0;
}

/* ------------------------------------------------------------------ interfaz */
aml *aml_crear(const struct aml_host *host) {
    aml *a = host->reservar(sizeof(aml));
    if (!a) return 0;
    cero(a, sizeof(*a));
    a->h = *host;
    a->nodos = host->reservar(sizeof(nodo) * NODOS_MAX);
    a->fija = host->reservar(ARENA_FIJA);
    a->eval = host->reservar(ARENA_EVAL);
    a->sombra = host->reservar(sizeof(struct sombra) * SOMBRA_MAX);
    if (!a->nodos || !a->fija || !a->eval || !a->sombra) return 0;
    a->pasos_limite = PASOS_MAX;
    int raiz = nuevo_nodo(a, -1, "\\___", N_AMBITO);
    if (raiz != 0) return 0;
    static const char *const base[] = {"_GPE", "_PR_", "_SB_", "_SI_", "_TZ_", 0};
    for (unsigned i = 0; base[i]; i++) nuevo_nodo(a, 0, base[i], N_AMBITO);
    /* Lo predefinido (5.7): _OS, _REV y _OSI. */
    int os = nuevo_nodo(a, 0, "_OS_", N_NOMBRE);
    static char nombre_os[] = "Microsoft Windows NT";
    a->nodos[os].d.valor.tipo = AML_CADENA; a->nodos[os].d.valor.v.cadena = nombre_os; a->nodos[os].d.valor.n = sizeof(nombre_os) - 1;
    int rev = nuevo_nodo(a, 0, "_REV", N_NOMBRE);
    a->nodos[rev].d.valor = entero(2);
    int osi = nuevo_nodo(a, 0, "_OSI", N_METODO);
    a->nodos[osi].d.metodo.args = 1;
    a->nodos[osi].d.metodo.ancho64 = 1;
    return a;
}
int aml_cargar(aml *a, const uint8_t *tabla, uint32_t bytes) {
    if (bytes < 36) return falla(a, "tabla sin cabecera", 0, 0, 0);
    marco *m = de_arena(a, 1, sizeof(marco));
    if (!m) return -1;
    m->p = tabla + 36; m->fin = tabla + bytes; m->ambito = 0;
    a->ancho64 = tabla[8] >= 2;
    a->en_carga = 1;
    a->error = 0; a->motivo[0] = 0;
    a->pasos = 0;
    a->eval_usada = 0;
    a->profundidad = 0; a->metodos = 0;
    int r = lista(a, m);
    a->en_carga = 0;
    return r;
}
int aml_evaluar(aml *a, int i, const aml_valor *args, unsigned nargs, aml_valor *sale) {
    a->error = 0; a->motivo[0] = 0;
    a->pasos = 0; a->profundidad = 0; a->metodos = 0;
    a->eval_usada = 0;
    if (i < 0 || i >= a->n_nodos) return falla(a, "objeto inexistente", 0, 0, 0);
    while (a->nodos[i].tipo == N_ALIAS) i = a->nodos[i].d.alias;
    const nodo *n = &a->nodos[i];
    if (n->tipo == N_METODO) {
        if (n->d.metodo.args != nargs) return falla(a, "numero de argumentos distinto", 0, n->d.metodo.args, 1);
        return llamar(a, i, args, nargs, sale);
    }
    return leer_nodo(a, i, sale);
}
int64_t aml_sta(aml *a, int i) {
    int s = aml_hijo(a, i, "_STA");
    if (s < 0) return 0xf;
    aml_valor v; uint64_t e;
    if (aml_evaluar(a, s, 0, 0, &v) || a_entero(a, &v, &e)) return -1;
    return (int64_t)(e & 0xffffffffu);
}
int aml_siguiente_nombre(const aml *a, int desde, const char *nombre) {
    for (int i = desde + 1; i < a->n_nodos; i++) if (iguales4(a->nodos[i].nombre, nombre)) return i;
    return -1;
}
int aml_siguiente_hid(aml *a, int desde, uint32_t eisa, const char *cadena) {
    for (int i = desde + 1; i < a->n_nodos; i++) {
        /* Sus hijos van detras de el: su propio _HID no cuenta como el siguiente. */
        if (!iguales4(a->nodos[i].nombre, "_HID") || a->nodos[i].padre == desde) continue;
        aml_valor v;
        if (aml_evaluar(a, i, 0, 0, &v)) continue;
        if (v.tipo == AML_ENTERO && eisa && v.v.entero == eisa) return a->nodos[i].padre;
        if (v.tipo == AML_CADENA && cadena) {
            const char *p = v.v.cadena, *q = cadena;
            while (*p && *p == *q) { p++; q++; }
            if (!*p && !*q) return a->nodos[i].padre;
        }
    }
    a->error = 0;
    return -1;
}
