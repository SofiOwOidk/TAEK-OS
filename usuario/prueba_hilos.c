/* H213, Fase D del gestor de hilos: prueba de los hilos de Anillo 3 (SYS_HILO, SYS_FUTEX, TLS y el reloj de CPU 0).
 * Se lanza con 'ejecutar /prueba_hilos.elf [modo]'. Imprime [HILOS3] por cada paso y RESULTADO=PASS o FALLO.
 *   (sin modo)  todas las comprobaciones;
 *   fallo       un hilo toca la direccion 8: el nucleo tiene que terminar el programa (vector 14) y seguir en pie;
 *   abandono    el principal termina con 4 hilos en marcha (3 girando sin llamar al sistema y 1 en un futex): el nucleo
 *               tiene que detenerlos antes de soltar la memoria; despues 'hilos' debe decir 0 hilos de programa. */
#include "memoria.h"
#include "hilos.h"

TAEK_PROGRAMA

static int fallos;

static void linea(const char *s) {
    char b[220]; int n = 0;
    const char *pre = "[HILOS3] ";
    while (*pre) b[n++] = *pre++;
    while (*s && n < 217) b[n++] = *s++;
    b[n++] = '\n';
    taek_escribir(b, n);
}
static int poner_num(char *b, int n, int64_t v) {
    if (v < 0) { b[n++] = '-'; v = -v; }
    char d[24]; int k = 0;
    do { d[k++] = (char)('0' + (uint64_t)v % 10); v = (int64_t)((uint64_t)v / 10); } while (v);
    while (k) b[n++] = d[--k];
    return n;
}
static int poner_txt(char *b, int n, const char *s) { while (*s && n < 200) b[n++] = *s++; return n; }
/* Texto con hasta cuatro numeros: cada '#' se reemplaza por el siguiente */
static void dato(const char *fmt, int64_t a, int64_t b2, int64_t c, int64_t d) {
    char b[210]; int n = 0; int64_t v[4] = { a, b2, c, d }; int i = 0;
    while (*fmt && n < 190) { if (*fmt == '#' && i < 4) n = poner_num(b, n, v[i++]); else b[n++] = *fmt; fmt++; }
    b[n] = 0;
    linea(b);
}
static void comprobar(int ok, const char *que) {
    if (!ok) fallos++;
    char b[200]; int n = poner_txt(b, 0, ok ? "OK    " : "FALLO ");
    n = poner_txt(b, n, que); b[n] = 0;
    linea(b);
}

/* Trabajo de calculo: una congruencia lineal mezclada. Mismo resultado en cualquier CPU. */
static uint64_t calcular(uint64_t semilla, uint64_t vueltas) {
    uint64_t x = semilla * 0x9E3779B97F4A7C15ull + 1;
    for (uint64_t i = 0; i < vueltas; i++) { x = x * 6364136223846793005ull + 1442695040888963407ull; x ^= x >> 29; }
    return x;
}

#define PILA (64u * 1024u)
static void *pilas[TAEK_HILOS_MAX];
static void preparar_pilas(void) { for (int i = 0; i < TAEK_HILOS_MAX; i++) if (!pilas[i]) pilas[i] = malloc(PILA); }

/* --------------------------------------------------------------- 1. crear, unir, CPU y valor de salida */
struct tarea { uint64_t semilla, vueltas, resultado; long cpu; };
static long hilo_calculo(void *p) {
    struct tarea *t = p;
    t->cpu = taek_hilo_cpu();
    t->resultado = calcular(t->semilla, t->vueltas);
    return (long)(t->resultado & 0x7FFFFFFF);
}

/* --------------------------------------------------------------- 3. mutex sobre futex */
static taek_mutex g_mutex;
static volatile uint64_t g_contador;
static long hilo_sumar(void *p) {
    long n = (long)(uintptr_t)p;
    for (long i = 0; i < n; i++) { taek_mutex_tomar(&g_mutex); g_contador = g_contador + 1; taek_mutex_soltar(&g_mutex); }
    return 0;
}

/* --------------------------------------------------------------- 4. TLS por hilo */
struct tls_bloque { uint64_t yo, marca, relleno[6]; } __attribute__((aligned(64)));
static struct tls_bloque g_tls[TAEK_HILOS_MAX + 1];
static long hilo_tls(void *p) {
    struct tls_bloque *b = p;
    long malos = 0;
    for (int i = 0; i < 200000; i++) {
        if (taek_tls_leer(0) != (uint64_t)(uintptr_t)b || taek_tls_leer(8) != b->marca) malos++;
        if ((i & 4095) == 0) taek_ceder();          /* Entrar y salir del nucleo, y dejar que otro hilo corra */
        if ((i & 32767) == 0) taek_esperar(1);
    }
    return malos;
}

/* --------------------------------------------------------------- 5. y 6. futex con limite y permisos desde un hilo */
static volatile uint32_t g_nunca;
struct medida { long r_espera, ms, r_pantalla, r_memoria, r_crear; };
static long hilo_permisos(void *p) {
    struct medida *m = p;
    long t0 = taek_ms();
    m->r_espera = taek_futex(TAEK_FUTEX_ESPERAR, &g_nunca, 0, 50);
    m->ms = taek_ms() - t0;
    struct taek_pantalla pan;
    m->r_pantalla = taek_pantalla(&pan);
    m->r_memoria = taek_memoria(0);
    m->r_crear = taek_hilo_crear(hilo_permisos, p, pilas[7], PILA, 0, 0);
    return 0;
}

/* --------------------------------------------------------------- 7. CPU 0 en un bucle sin llamadas */
static volatile uint32_t g_parar;
static volatile uint64_t g_vueltas[TAEK_HILOS_MAX + 1];
static long hilo_girar(void *p) {
    long i = (long)(uintptr_t)p;
    uint64_t x = 1;
    while (!g_parar) { x = x * 6364136223846793005ull + 1; g_vueltas[i]++; }
    return (long)(x & 1);
}

/* --------------------------------------------------------------- 9. sin lugar: 8 hilos esperando */
static volatile uint32_t g_puerta;
static long hilo_esperar_puerta(void *p) {
    (void)p;
    while (__atomic_load_n(&g_puerta, __ATOMIC_ACQUIRE) == 0) taek_futex(TAEK_FUTEX_ESPERAR, &g_puerta, 0, 0);
    return 7;
}

static long modo_fallo(void);
static long modo_abandono(void);
static long hilo_fallar(void *p) {
    (void)p;
    taek_esperar(20);
    volatile uint64_t *nulo = (volatile uint64_t *)(uintptr_t)8;
    *nulo = 1;
    return 0;
}

static int igual(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

long principal(uint64_t a, uint64_t b) {
    (void)a; (void)b;
    char arg[64]; arg[0] = 0;
    long na = taek_argumento(arg, sizeof(arg));
    if (na > 0 && (size_t)na < sizeof(arg)) arg[na] = 0; else arg[0] = 0;
    preparar_pilas();
    for (int i = 0; i < TAEK_HILOS_MAX; i++) if (!pilas[i]) { linea("sin memoria para las pilas"); return 2; }
    if (igual(arg, "fallo")) return modo_fallo();
    if (igual(arg, "abandono")) return modo_abandono();

    linea("Prueba de hilos de Anillo 3 (H213, Fase D)");
    long cpu0 = taek_hilo_cpu();
    comprobar(cpu0 == 0 && taek_hilo_yo() == 0, "el principal corre en la CPU 0 y es el hilo 0");

    /* 1 */
    static struct tarea t[6];
    long id[6];
    for (int i = 0; i < 6; i++) {
        t[i].semilla = 1000 + (uint64_t)i; t[i].vueltas = 2000000; t[i].cpu = -1;
        id[i] = taek_hilo_crear(hilo_calculo, &t[i], pilas[i], PILA, 0, 0);
    }
    int creados = 1, bien = 1, cpus_ok = 1; unsigned vistas = 0;
    for (int i = 0; i < 6; i++) {
        if (id[i] <= 0) { creados = 0; dato("crear devolvio #", id[i], 0, 0, 0); continue; }
        struct taek_hilo r;
        long u = taek_hilo_unir(id[i], &r);
        uint64_t esperado = calcular(1000 + (uint64_t)i, 2000000);
        if (u || r.fin != TAEK_HILO_FIN_SALIO || t[i].resultado != esperado || r.valor != (long)(esperado & 0x7FFFFFFF)) bien = 0;
        if (t[i].cpu < 1 || t[i].cpu > 3 || (long)r.cpu != t[i].cpu) cpus_ok = 0; else vistas |= 1u << t[i].cpu;
        dato("hilo # en CPU #: fin # (1 = salio)", id[i], t[i].cpu, r.fin, 0);
    }
    comprobar(creados, "6 hilos creados");
    comprobar(bien, "los 6 calculos y sus valores de salida coinciden con los del principal");
    comprobar(cpus_ok, "cada hilo corrio en una CPU auxiliar (1 a 3)");
    comprobar(vistas == 0xE, "se usaron las tres CPU auxiliares");

    /* 2: un hilo con tres unidades de trabajo contra tres hilos con una cada uno */
    {
        static struct tarea u[3];
        const uint64_t V = 60000000;
        long t0 = taek_ms();
        u[0].semilla = 1; u[0].vueltas = 3 * V;
        long h = taek_hilo_crear(hilo_calculo, &u[0], pilas[0], PILA, 0, 3);
        taek_hilo_unir(h, 0);
        long uno = taek_ms() - t0;
        t0 = taek_ms();
        long hs[3];
        for (int i = 0; i < 3; i++) { u[i].semilla = 2 + (uint64_t)i; u[i].vueltas = V; hs[i] = taek_hilo_crear(hilo_calculo, &u[i], pilas[i], PILA, 0, (uint32_t)(i + 1)); }
        for (int i = 0; i < 3; i++) taek_hilo_unir(hs[i], 0);
        long tres = taek_ms() - t0;
        dato("mismo trabajo: 1 hilo # ms, 3 hilos en CPU 1, 2 y 3 # ms (x# decimas)", uno, tres, tres ? uno * 10 / tres : 0, 0);
        comprobar(tres > 0 && uno * 10 >= tres * 20, "con tres hilos el trabajo va al menos 2 veces mas rapido");
    }

    /* 3: 4 hilos y el principal suman 100000 veces cada uno con el mutex */
    {
        long hs[4];
        g_contador = 0;
        for (int i = 0; i < 4; i++) hs[i] = taek_hilo_crear(hilo_sumar, (void *)(uintptr_t)100000, pilas[i], PILA, 0, 0);
        hilo_sumar((void *)(uintptr_t)100000);
        for (int i = 0; i < 4; i++) taek_hilo_unir(hs[i], 0);
        struct taek_hilos_info inf; taek_hilo(TAEK_HILO_INFO, (long)&inf);
        dato("contador # de 500000; esperas en futex #, despertares #", (int64_t)g_contador, (int64_t)inf.futex_esperas, (int64_t)inf.futex_despertados, 0);
        comprobar(g_contador == 500000, "mutex sobre futex: 500000 sumas exactas entre 5 hilos en 4 CPU");
    }

    /* 4: TLS */
    {
        long hs[3]; long malos = 0;
        for (int i = 0; i < 3; i++) {
            g_tls[i + 1].yo = (uint64_t)(uintptr_t)&g_tls[i + 1];
            g_tls[i + 1].marca = 1000u * (uint64_t)(i + 1);
            hs[i] = taek_hilo_crear(hilo_tls, &g_tls[i + 1], pilas[i], PILA, (uint64_t)(uintptr_t)&g_tls[i + 1], 0);
        }
        /* El principal tambien: si el nucleo no le devuelve su FS al salir de cada llamada, esto falla */
        g_tls[0].yo = (uint64_t)(uintptr_t)&g_tls[0]; g_tls[0].marca = 4242;
        taek_hilo_tls((uint64_t)(uintptr_t)&g_tls[0]);
        long malos_principal = 0;
        for (int i = 0; i < 20000; i++) {
            if (taek_tls_leer(0) != g_tls[0].yo || taek_tls_leer(8) != 4242) malos_principal++;
            if ((i & 255) == 0) taek_ms();
        }
        taek_hilo_tls(0);
        for (int i = 0; i < 3; i++) { struct taek_hilo r; taek_hilo_unir(hs[i], &r); malos += r.valor; }
        dato("lecturas de TLS equivocadas: hilos #, principal #", malos, malos_principal, 0, 0);
        comprobar(malos == 0 && malos_principal == 0, "cada hilo ve su propio TLS (FS) tras ceder, dormir y desalojos");
    }

    /* 5 y 6 */
    {
        g_nunca = 5;
        comprobar(taek_futex(TAEK_FUTEX_ESPERAR, &g_nunca, 4, 10) == -TAEK_EAGAIN, "futex con el valor ya cambiado: -EAGAIN");
        long t0 = taek_ms();
        long r = taek_futex(TAEK_FUTEX_ESPERAR, &g_nunca, 5, 30);
        long ms = taek_ms() - t0;
        dato("futex del principal con limite de 30 ms: # en # ms", r, ms, 0, 0);
        comprobar(r == -TAEK_ETIEMPO && ms >= 25 && ms < 200, "el principal despierta por limite");
        g_nunca = 0;
        static struct medida m;
        long h = taek_hilo_crear(hilo_permisos, &m, pilas[0], PILA, 0, 0);
        taek_hilo_unir(h, 0);
        dato("hilo: futex 50 ms -> # en # ms; pantalla #; memoria #", m.r_espera, m.ms, m.r_pantalla, m.r_memoria);
        comprobar(m.r_espera == -TAEK_ETIEMPO && m.ms >= 45 && m.ms < 300, "un hilo despierta por limite");
        comprobar(m.r_pantalla == -TAEK_ENOPERMITIDA && m.r_memoria == -TAEK_ENOPERMITIDA && m.r_crear == -TAEK_ENOPERMITIDA,
                  "pantalla, memoria y crear hilos no se permiten desde un hilo secundario");
    }

    /* 7: el principal gira 2,5 s sin llamar al sistema mientras 3 hilos giran en las auxiliares */
    {
        long hs[3];
        g_parar = 0;
        for (int i = 0; i < 3; i++) { g_vueltas[i + 1] = 0; hs[i] = taek_hilo_crear(hilo_girar, (void *)(uintptr_t)(i + 1), pilas[i], PILA, 0, (uint32_t)(i + 1)); }
        /* Calibrar: cuantas vueltas de este bucle hay en un ms */
        long t0 = taek_ms(); uint64_t x = 1, n = 0;
        while (taek_ms() - t0 < 200) { for (int k = 0; k < 100000; k++) x = x * 6364136223846793005ull + 1; n += 100000; }
        uint64_t por_ms = n / (uint64_t)(taek_ms() - t0);
        struct taek_hilos_info inf;
        taek_esperar(1);                                  /* Deja la flecha recien atendida */
        taek_hilo(TAEK_HILO_INFO, (long)&inf);            /* Pone a cero el hueco */
        uint32_t serv0 = inf.servicios_cpu0;
        t0 = taek_ms();
        for (uint64_t i = 0; i < por_ms * 2500u; i++) x = x * 6364136223846793005ull + 1;   /* Sin llamadas */
        long giro = taek_ms() - t0;
        taek_hilo(TAEK_HILO_INFO, (long)&inf);
        g_parar = 1;
        for (int i = 0; i < 3; i++) taek_hilo_unir(hs[i], 0);
        dato("principal sin llamar al sistema # ms: CPU 0 atendio la flecha # veces, mayor hueco # ms", giro, (int64_t)(inf.servicios_cpu0 - serv0), inf.hueco_cpu0_max_ms, 0);
        dato("vueltas de los hilos mientras tanto: CPU1 # CPU2 # CPU3 # (x unidades)", (int64_t)g_vueltas[1], (int64_t)g_vueltas[2], (int64_t)g_vueltas[3], (int64_t)(x & 1));
        comprobar(giro >= 1500 && inf.servicios_cpu0 - serv0 >= 50 && inf.hueco_cpu0_max_ms <= 40,
                  "con el principal en un bucle, CPU 0 sigue moviendo la flecha (huecos de 40 ms o menos)");
        comprobar(g_vueltas[1] && g_vueltas[2] && g_vueltas[3], "los tres hilos avanzaron a la vez que el principal");
    }

    /* 9: 8 hilos esperando en un futex; el noveno no tiene lugar; despues se despiertan todos */
    {
        long hs[TAEK_HILOS_MAX]; int n = 0;
        g_puerta = 0;
        for (int i = 0; i < TAEK_HILOS_MAX; i++) { hs[i] = taek_hilo_crear(hilo_esperar_puerta, 0, pilas[i], PILA, 0, 0); if (hs[i] > 0) n++; }
        static uint8_t pila_extra[4096] __attribute__((aligned(16)));
        long noveno = taek_hilo_crear(hilo_esperar_puerta, 0, pila_extra, sizeof(pila_extra), 0, 0);
        taek_esperar(20);
        __atomic_store_n(&g_puerta, 1, __ATOMIC_RELEASE);
        long desp = taek_futex(TAEK_FUTEX_DESPERTAR, &g_puerta, 100, 0);
        int siete = 1;
        for (int i = 0; i < TAEK_HILOS_MAX; i++) if (hs[i] > 0) { struct taek_hilo r; taek_hilo_unir(hs[i], &r); if (r.valor != 7) siete = 0; }
        dato("hilos a la vez #; el noveno devolvio #; despertados de una vez #", n, noveno, desp, 0);
        comprobar(n == TAEK_HILOS_MAX && noveno == -TAEK_EBUSY, "8 hilos a la vez y el noveno se niega con -EBUSY");
        comprobar(siete, "los 8 despertaron del futex y terminaron bien");
    }

    struct taek_hilos_info inf; taek_hilo(TAEK_HILO_INFO, (long)&inf);
    dato("al final: vivos #, creados #, llamadas desde hilos #, rechazadas #", inf.vivos, inf.creados, (int64_t)inf.llamadas_hilos, (int64_t)inf.rechazadas);
    comprobar(inf.vivos == 0, "ningun hilo queda vivo");
    linea(fallos ? "RESULTADO=FALLO" : "RESULTADO=PASS");
    return fallos;
}

static long modo_fallo(void) {
    linea("Modo fallo: el hilo 1 va a tocar la direccion 8. El nucleo debe terminar el programa con vector 14.");
    long h = taek_hilo_crear(hilo_fallar, 0, pilas[0], PILA, 0, 0);
    dato("hilo creado: #", h, 0, 0, 0);
    struct taek_hilo r;
    taek_hilo_unir(h, &r);
    taek_esperar(100);
    linea("ERROR: el programa siguio despues del fallo del hilo");
    return 9;
}

static long modo_abandono(void) {
    linea("Modo abandono: el principal termina con 3 hilos girando y 1 esperando un futex sin limite.");
    g_parar = 0; g_puerta = 0;
    for (int i = 0; i < 3; i++) taek_hilo_crear(hilo_girar, (void *)(uintptr_t)(i + 1), pilas[i], PILA, 0, (uint32_t)(i + 1));
    taek_hilo_crear(hilo_esperar_puerta, 0, pilas[3], PILA, 0, 0);
    taek_esperar(50);
    struct taek_hilos_info inf; taek_hilo(TAEK_HILO_INFO, (long)&inf);
    dato("hilos vivos antes de salir: #", inf.vivos, 0, 0, 0);
    linea("El principal sale ahora.");
    return 0;
}
