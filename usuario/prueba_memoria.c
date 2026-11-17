/* H144: prueba de la memoria dinamica de Anillo 3 (SYS_MEMORIA y usuario/memoria.h). Se lanza con
 * 'ejecutar /prueba_memoria.elf'. Imprime [MEMORIA] por cada paso y RESULTADO=PASS o FALLO.
 * El ultimo paso es a proposito un fallo: toca una pagina ya devuelta y el nucleo tiene que matar al programa con un
 * fallo de pagina (vector 14) en esa direccion. Si el programa sigue vivo despues, la memoria no se solto de verdad. */
#include "memoria.h"

TAEK_PROGRAMA

static int fallos;

static void linea(const char *s) {
    char b[200]; int n = 0;
    const char *pre = "[MEMORIA] ";
    while (*pre) b[n++] = *pre++;
    while (*s && n < 197) b[n++] = *s++;
    b[n++] = '\n';
    taek_escribir(b, n);
}
static void numero(const char *txt, uint64_t v, const char *unidad) {
    char b[160]; int n = 0;
    while (*txt && n < 120) b[n++] = *txt++;
    char d[24]; int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[n++] = d[--k];
    while (*unidad && n < 158) b[n++] = *unidad++;
    b[n] = 0;
    linea(b);
}
static void comprobar(int ok, const char *que) {
    if (!ok) fallos++;
    char b[160] = "OK    "; if (!ok) { b[0] = 'F'; b[1] = 'A'; b[2] = 'L'; b[3] = 'L'; b[4] = 'O'; }
    int n = 6; while (*que && n < 158) b[n++] = *que++;
    b[n] = 0;
    linea(b);
}

static uint32_t azar = 12345;
static uint32_t siguiente(void) { azar = azar * 1103515245u + 12345u; return azar >> 8; }

long principal(uint64_t a, uint64_t b) {
    (void)a; (void)b;
    linea("Prueba de memoria dinamica");
    comprobar(taek_memoria(0) == (long)TAEK_MONTON, "el monton empieza vacio en TAEK_MONTON");

    /* 1. Un bloque grande: 64 MiB escritos y leidos de vuelta */
    uint64_t mib = 64;
    uint8_t *g = malloc(mib << 20);
    comprobar(g != NULL, "malloc de 64 MiB");
    if (g) {
        int cero = 1;
        for (uint64_t i = 0; i < (mib << 20); i += 4096) if (g[i]) cero = 0;
        comprobar(cero, "las paginas nuevas llegan a cero");
        for (uint64_t i = 0; i < (mib << 20); i += 8) *(uint64_t *)(g + i) = i * 0x9E3779B97F4A7C15ull;
        int bien = 1;
        for (uint64_t i = 0; i < (mib << 20); i += 8) if (*(uint64_t *)(g + i) != i * 0x9E3779B97F4A7C15ull) { bien = 0; break; }
        comprobar(bien, "64 MiB escritos y leidos sin diferencias");
        numero("monton pedido: ", taek_monton_usado() >> 20, " MiB");
        free(g);
        numero("tras free: ", taek_monton_usado() >> 20, " MiB");
        comprobar(taek_monton_usado() <= (2u << 20), "free devuelve al nucleo la RAM grande del final");
    }

    /* 2. Muchos bloques pequenos con tamanos al azar, liberados en desorden, con su contenido vigilado */
    enum { N = 4000 };
    static uint8_t *p[N]; static uint32_t tam[N];
    for (int i = 0; i < N; i++) {
        tam[i] = 1 + siguiente() % 3000;
        p[i] = malloc(tam[i]);
        if (!p[i]) { comprobar(0, "malloc pequeno"); break; }
        if (((uintptr_t)p[i] & 15) != 0) { comprobar(0, "alineacion a 16"); break; }
        memset(p[i], (int)(i & 0xFF), tam[i]);
    }
    for (int i = 0; i < N; i += 2) { free(p[i]); p[i] = NULL; }
    for (int i = 0; i < N; i += 2) { tam[i] = 1 + siguiente() % 5000; p[i] = malloc(tam[i]); if (p[i]) memset(p[i], (int)(i & 0xFF), tam[i]); }
    for (int i = 1; i < N; i += 4) { uint8_t *r = realloc(p[i], tam[i] * 3 + 100); if (r) { p[i] = r; memset(r + tam[i], (int)(i & 0xFF), tam[i] * 2 + 100); tam[i] = tam[i] * 3 + 100; } }
    int intacto = 1;
    for (int i = 0; i < N && intacto; i++) {
        if (!p[i]) { intacto = 0; break; }
        for (uint32_t k = 0; k < tam[i]; k++) if (p[i][k] != (uint8_t)(i & 0xFF)) { intacto = 0; break; }
    }
    comprobar(intacto, "4000 bloques pequenos con malloc, free y realloc sin pisarse");
    for (int i = N - 1; i >= 0; i--) free(p[i]);
    numero("tras liberarlos todos: ", taek_monton_usado() >> 10, " KiB pedidos al nucleo");

    /* 3. Limites: lo imposible se niega sin matar al programa */
    comprobar(malloc((size_t)32 << 30) == NULL, "32 GiB se niegan (mas que el tope y que la RAM)");
    comprobar(taek_memoria(-(long)((uint64_t)1 << 40)) == -TAEK_EINVAL, "encoger mas de lo que hay se niega");

    /* 4. Hasta donde deja crecer la RAM libre: de 256 MiB en 256 MiB, y luego todo de vuelta */
    uint64_t total = 0;
    static uint8_t *trozos[256]; int nt = 0;
    while (nt < 256) {
        uint8_t *t = malloc((size_t)256 << 20);
        if (!t) break;
        t[0] = 1; t[((size_t)256 << 20) - 1] = 2;
        trozos[nt++] = t;
        total += 256;
    }
    numero("crecio hasta ", total, " MiB antes de que el nucleo dijera que no");
    comprobar(total >= 256, "al menos 256 MiB disponibles");
    for (int i = nt - 1; i >= 0; i--) free(trozos[i]);
    numero("tras devolverlos: ", taek_monton_usado() >> 20, " MiB");
    comprobar(taek_monton_usado() <= (2u << 20), "toda esa RAM volvio al nucleo");

    linea(fallos ? "RESULTADO=FALLO" : "RESULTADO=PASS");

    /* 5. A proposito: tocar una pagina ya devuelta tiene que ser un fallo de pagina */
    long fin = taek_memoria(0);
    long antes = taek_memoria(8192);
    if (antes < 0) return 3;
    volatile uint8_t *x = (volatile uint8_t *)(uintptr_t)(fin + 4096);
    *x = 7;
    taek_memoria(-8192);
    linea("Ahora se toca una pagina devuelta: el nucleo debe detener el programa con vector 14.");
    *x = 8;
    linea("ERROR: la pagina devuelta seguia accesible");
    return 9;
}
