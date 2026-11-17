/* H147: prueba de escritura desde Anillo 3 (SYS_ARCHIVOS: crear carpeta, escribir, mover, borrar) sobre el disco abierto.
 * 'ejecutar /prueba_escritura.elf'. Imprime [ESCRITURA] por paso y RESULTADO=PASS o FALLO. Lo que deja en /taekprueba se
 * revisa despues desde el PC con fsck.vfat / fsck.exfat: el volumen tiene que quedar sano. */
#pragma clang diagnostic ignored "-Wunused-function"
#include "taek.h"
#include "memoria.h"
#include "apps.h"

TAEK_PROGRAMA

static int fallos;
static void linea(const char *s) { ap_marca("ESCRITURA", s, ""); }
static void comprobar(int ok, const char *que) { if (!ok) fallos++; char b[200] = ""; ap_copiar(b, ok ? "OK    " : "FALLO ", sizeof(b)); ap_agregar(b, que, sizeof(b)); linea(b); }

static struct taek_archivos a;
static long op(uint32_t o, const char *ruta, const char *ruta2, const void *datos, uint64_t n) {
    memset(&a, 0, sizeof(a));
    a.op = o; ap_copiar(a.ruta, ruta, sizeof(a.ruta));
    if (ruta2) ap_copiar(a.ruta2, ruta2, sizeof(a.ruta2));
    a.destino = (uint64_t)(uintptr_t)datos; a.capacidad = n;
    return taek_archivos(&a);
}
static long escribir(const char *ruta, const void *d, uint64_t n) { return op(TAEK_AR_ESCRIBIR, ruta, 0, d, n); }

/* Lee el archivo entero y lo compara con lo esperado */
static int igual(const char *ruta, const uint8_t *esperado, uint64_t n) {
    uint64_t m = 0;
    uint8_t *b = n ? ap_leer_todo(ruta, &m, 64u << 20) : NULL;
    if (!n) { uint64_t total = 1; uint8_t x; long r = ap_leer(ruta, 0, &x, 1, &total); return r >= 0 && total == 0; }
    if (!b) return 0;
    int ok = m == n && !memcmp(b, esperado, n);
    free(b);
    return ok;
}
static int existe(const char *ruta) { uint64_t t; uint8_t x; return ap_leer(ruta, 0, &x, 1, &t) >= 0; }

static uint32_t azar = 7;
static uint8_t rnd(void) { azar = azar * 1664525u + 1013904223u; return (uint8_t)(azar >> 24); }

long principal(uint64_t x, uint64_t y) {
    (void)x; (void)y;
    linea("Prueba de escritura");
    comprobar(op(TAEK_AR_CREAR_CARPETA, "/taekprueba", 0, 0, 0) == 0, "crear /taekprueba");
    comprobar(op(TAEK_AR_CREAR_CARPETA, "/taekprueba", 0, 0, 0) == -31, "crearla otra vez da 'ya existe'");
    comprobar(op(TAEK_AR_CREAR_CARPETA, "/taekprueba/sub", 0, 0, 0) == 0, "crear /taekprueba/sub");
    comprobar(op(TAEK_AR_CREAR_CARPETA, "/taekprueba/sub/mas adentro", 0, 0, 0) == 0, "crear una carpeta dentro de otra, con espacio");

    comprobar(escribir("/taekprueba/vacio.txt", "", 0) == 0 && igual("/taekprueba/vacio.txt", 0, 0), "archivo vacio");
    const char *hola = "Hola desde Anillo 3 de TAEK OS.\n";
    comprobar(escribir("/taekprueba/hola.txt", hola, (uint64_t)ap_largo(hola)) == 0 && igual("/taekprueba/hola.txt", (const uint8_t *)hola, (uint64_t)ap_largo(hola)), "archivo pequeno");
    uint64_t gn = 3u * 1024 * 1024 + 777;
    uint8_t *g = malloc(gn);
    for (uint64_t i = 0; i < gn; i++) g[i] = rnd();
    comprobar(escribir("/taekprueba/grande.bin", g, gn) == 0 && igual("/taekprueba/grande.bin", g, gn), "archivo de 3 MB (muchos clusters)");
    uint64_t rn = 100 * 1024;
    for (uint64_t i = 0; i < rn; i++) g[i] = (uint8_t)('a' + i % 26);
    comprobar(escribir("/taekprueba/hola.txt", g, rn) == 0 && igual("/taekprueba/hola.txt", g, rn), "reemplazar hola.txt por 100 KB");
    comprobar(op(TAEK_AR_MOVER, "/taekprueba/hola.txt", "/taekprueba/sub/hola2.txt", 0, 0) == 0 && igual("/taekprueba/sub/hola2.txt", g, rn) && !existe("/taekprueba/hola.txt"), "mover a otra carpeta y renombrar");
    comprobar(op(TAEK_AR_BORRAR, "/taekprueba/grande.bin", 0, 0, 0) == 0 && !existe("/taekprueba/grande.bin"), "borrar el de 3 MB");
    const char *tilde = "Canción del niño — pingüino.txt";
    char r[TAEK_RUTA]; ap_unir(r, sizeof(r), "/taekprueba", tilde);
    comprobar(escribir(r, tilde, (uint64_t)ap_largo(tilde)) == 0 && igual(r, (const uint8_t *)tilde, (uint64_t)ap_largo(tilde)), "nombre largo con tildes y raya");
    /* Muchos archivos: la carpeta tiene que crecer mas alla de su primer cluster (4 KB = 128 entradas) */
    int bien = 1;
    for (int i = 0; i < 60 && bien; i++) {
        char n[TAEK_RUTA] = "/taekprueba/sub/archivo con nombre largo numero "; ap_num(n, sizeof(n), (uint64_t)i); ap_agregar(n, ".txt", sizeof(n));
        char d[64] = "contenido "; ap_num(d, sizeof(d), (uint64_t)i);
        bien = escribir(n, d, (uint64_t)ap_largo(d)) == 0 && igual(n, (const uint8_t *)d, (uint64_t)ap_largo(d));
    }
    comprobar(bien, "60 archivos con nombre largo en una carpeta (crece)");
    struct taek_entrada *e; int n = ap_listar("/taekprueba/sub", &e, 512);
    comprobar(n == 62, "la carpeta lista 60 archivos, hola2.txt y 'mas adentro'");
    if (n > 0) free(e);
    /* Lo que hace arrancar el equipo no se toca */
    comprobar(escribir("/ventoy/ventoy.json", "x", 1) == -TAEK_EPROTEGIDO, "escribir en /ventoy se niega");
    comprobar(escribir("/VENTOY//otro.json", "x", 1) == -TAEK_EPROTEGIDO, "tambien con mayusculas y barras dobles");
    comprobar(escribir("/taekprueba/falsa.ISO", "x", 1) == -TAEK_EPROTEGIDO, "crear una .iso se niega");
    comprobar(op(TAEK_AR_MOVER, "/taekprueba/vacio.txt", "/ventoy/vacio.txt", 0, 0) == -TAEK_EPROTEGIDO, "mover hacia /ventoy se niega");
    comprobar(op(TAEK_AR_BORRAR, "/ventoy/ventoy.json", 0, 0, 0) == -TAEK_EPROTEGIDO, "borrar en /ventoy se niega");
    comprobar(op(TAEK_AR_BORRAR, "/taekprueba/../ventoy/ventoy.json", 0, 0, 0) == -TAEK_EPROTEGIDO, "y tampoco subiendo con ..");
    comprobar(existe("/ventoy/ventoy.json"), "ventoy.json sigue ahi");
    comprobar(op(TAEK_AR_BORRAR, "/taekprueba/no existe.txt", 0, 0, 0) == -26, "borrar lo que no existe da 'no existe'");
    linea(fallos ? "RESULTADO=FALLO" : "RESULTADO=PASS");
    return fallos;
}
