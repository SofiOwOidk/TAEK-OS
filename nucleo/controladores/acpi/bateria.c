/* Comando 'bateria': lee una vez lo que informa el firmware, al escribirlo. No hay vigilancia ni nada corriendo en segundo plano. */
#include "bateria.h"
#include "aml.h"
#include "../consola.h"
#include "../iommu.h"
#include "../../arquitectura/x86_64/pci.h"
#include "../../arquitectura/x86_64/puertos.h"
#include "../../base/memoria.h"
#include "../../base/tiempo.h"

#define EISA_BATERIA UINT32_C(0x0A0CD041)   /* PNP0C0A */
#define EISA_EC      UINT32_C(0x090CD041)   /* PNP0C09 */
#define DESCONOCIDO  UINT64_C(0xFFFFFFFF)

static aml *g_aml;
static const char *g_error;
static char g_motivo[96];

/* ------------------------------------------------------------------ acceso del interprete al equipo */
static void *h_reservar(size_t n) { return asignar_memoria(n); }
static int h_mem(uint64_t f, unsigned b, uint64_t *v) {
    if (((f & 4095) + b) > 4096) return -1;
    const void *p = acpi_mapear_lectura(f);
    if (!p) return -1;
    switch (b) {
        case 1: *v = *(volatile const uint8_t *)p; break;
        case 2: *v = *(volatile const uint16_t *)p; break;
        case 4: *v = *(volatile const uint32_t *)p; break;
        case 8: *v = *(volatile const uint64_t *)p; break;
        default: return -1;
    }
    return 0;
}
static int h_io(uint16_t pu, unsigned b, uint64_t *v) {
    if (b == 1) *v = leer_puerto_b(pu);
    else if (b == 2) *v = leer_puerto_w(pu);
    else if (b == 4) *v = leer_puerto_l(pu);
    else return -1;
    return 0;
}
static int h_pci(uint8_t bus, uint8_t d, uint8_t f, uint16_t r, unsigned b, uint64_t *v) {
    if (r > 255 || d > 31 || f > 7) return -1;
    if (b == 1) *v = pci_leer_config_8(bus, d, f, (uint8_t)r);
    else if (b == 2) *v = pci_leer_config_16(bus, d, f, (uint8_t)r);
    else if (b == 4) *v = pci_leer_config_32(bus, d, f, (uint8_t)r);
    else return -1;
    return 0;
}
/* Controlador embebido (12.2). Los puertos son estos salvo que su _CRS diga otros: una Dell Latitude usa 0x930 y 0x934. */
#define EC_IBF 2
#define EC_OBF 1
static uint16_t g_ec_datos = 0x62, g_ec_orden = 0x66;
static int ec_esperar(uint8_t bit, int activo) {
    uint64_t t = tiempo_obtener_milisegundos();
    do {
        uint8_t s = leer_puerto_b(g_ec_orden);
        if (s == 0xff) return -1;
        if (!!(s & bit) == activo) return 0;
    } while (tiempo_obtener_milisegundos() - t < 200);
    return -1;
}
static int h_ec(uint8_t r, uint8_t *v) {
    /* Si quedo un dato sin recoger, la respuesta seria la de otra lectura. */
    for (unsigned i = 0; i < 16 && (leer_puerto_b(g_ec_orden) & EC_OBF); i++) (void)leer_puerto_b(g_ec_datos);
    if (ec_esperar(EC_IBF, 0)) return -1;
    escribir_puerto_b(g_ec_orden, 0x80);
    if (ec_esperar(EC_IBF, 0)) return -1;
    escribir_puerto_b(g_ec_datos, r);
    if (ec_esperar(EC_OBF, 1)) return -1;
    *v = leer_puerto_b(g_ec_datos);
    return 0;
}
static int h_ec_escribir(uint8_t r, uint8_t v) {
    if (ec_esperar(EC_IBF, 0)) return -1;
    escribir_puerto_b(g_ec_orden, 0x81);
    if (ec_esperar(EC_IBF, 0)) return -1;
    escribir_puerto_b(g_ec_datos, r);
    if (ec_esperar(EC_IBF, 0)) return -1;
    escribir_puerto_b(g_ec_datos, v);
    return ec_esperar(EC_IBF, 0);
}
static void h_esperar(unsigned ms) { esperar_milisegundos(ms); }

/* Puertos del controlador segun su _CRS: dos descriptores de E/S, primero datos y luego ordenes (12.11). */
static void puertos_ec(aml *a, int ec) {
    int c = aml_hijo(a, ec, "_CRS");
    aml_valor v;
    if (c < 0 || aml_evaluar(a, c, 0, 0, &v) || v.tipo != AML_BUFFER) return;
    uint16_t p[2]; unsigned k = 0;
    for (uint32_t i = 0; i + 8 <= v.n && k < 2;) {
        uint8_t tag = v.v.buffer[i];
        if (tag == 0x79) break;
        if (tag == 0x47) { p[k++] = (uint16_t)(v.v.buffer[i + 2] | v.v.buffer[i + 3] << 8); i += 8; continue; }
        if (tag & 0x80) break;   /* Descriptor largo: no es lo que declara un controlador embebido. */
        i += 1u + (tag & 7);
    }
    if (k == 2 && p[0] && p[1]) { g_ec_datos = p[0]; g_ec_orden = p[1]; }
}

/* Carga las tablas la primera vez. Lo que falte queda dicho. */
static int preparar(void) {
    if (g_aml) return 0;
    if (g_error) return -1;
    static const struct aml_host host = {h_reservar, h_mem, h_io, h_pci, h_ec, h_ec_escribir, tiempo_obtener_milisegundos, h_esperar};
    aml *a = aml_crear(&host);
    if (!a) { g_error = "sin memoria para el interprete ACPI"; return -1; }
    const uint8_t *t; uint32_t n;
    if (acpi_tabla_buscar("DSDT", 0, &t, &n)) { g_error = "el firmware no entrega DSDT"; return -1; }
    if (aml_cargar(a, t, n)) {
        unsigned o = 0; const char *m = aml_motivo(a);
        while (m[o] && o + 1 < sizeof(g_motivo)) { g_motivo[o] = m[o]; o++; }
        g_motivo[o] = 0;
        g_error = g_motivo;
        return -1;
    }
    /* Las SSDT se cargan todas; una que no se entienda no impide leer la bateria si esta en otra. */
    for (unsigned i = 0; !acpi_tabla_buscar("SSDT", i, &t, &n); i++) {
        if (aml_cargar(a, t, n)) { consola_imprimir("  [ACPI] SSDT "); consola_imprimir_dec(i); consola_imprimir(" ignorada: "); consola_imprimir_linea(aml_motivo(a)); }
    }
    /* El firmware suele esperar a que el sistema anuncie que el controlador embebido ya se puede usar (6.5.4). */
    for (int ec = aml_siguiente_hid(a, -1, EISA_EC, 0); ec >= 0; ec = aml_siguiente_hid(a, ec, EISA_EC, 0)) {
        /* Algunos firmwares declaran un segundo controlador apagado: su _STA lo descarta. */
        int64_t sta = aml_sta(a, ec);
        if (sta >= 0 && !(sta & 1)) continue;
        puertos_ec(a, ec);
        int r = aml_hijo(a, ec, "_REG");
        if (r < 0) continue;
        aml_valor args[2], sale;
        for (unsigned i = 0; i < 2; i++) { args[i].tipo = AML_ENTERO; args[i].n = 0; args[i].ruta_ambito = 0; args[i].v.entero = i ? 1 : 3; }
        /* Sin permiso de escritura: su primera linea anuncia el controlador y lo que venga despues configura el equipo. Que se detenga
         * ahi es lo esperado, y lo que importa ya quedo hecho. */
        (void)aml_evaluar(a, r, args, 2, &sale);
    }
    g_aml = a;
    return 0;
}

/* ------------------------------------------------------------------ presentacion */
static void imprimir_fijo(uint64_t milesimas) {
    /* Unidades con dos decimales: 12340 -> 12.34 */
    consola_imprimir_dec(milesimas / 1000);
    consola_imprimir(".");
    uint64_t c = (milesimas % 1000) / 10;
    if (c < 10) consola_imprimir("0");
    consola_imprimir_dec(c);
}
static void fila(const char *n) {
    consola_imprimir("  ");
    consola_imprimir(n);
    unsigned k = 0;
    while (n[k]) k++;
    /* Sin contar los bytes de continuacion de UTF-8, para que las columnas cuadren con acentos. */
    unsigned vis = 0;
    for (unsigned i = 0; i < k; i++) if ((n[i] & 0xc0) != 0x80) vis++;
    for (; vis < 22; vis++) consola_imprimir(" ");
    consola_imprimir(": ");
}
static uint64_t elem(const aml_valor *p, unsigned i) {
    if (i >= p->n) return DESCONOCIDO;
    const aml_valor *e = &p->v.paquete[i];
    /* 0xFFFFFFFF es "desconocido" (10.2.2); con enteros de 64 bits el firmware lo escribe con todos los bits a uno. */
    return e->tipo == AML_ENTERO && e->v.entero < DESCONOCIDO ? e->v.entero : DESCONOCIDO;
}
/* El firmware puede dar los textos como cadena o como buffer terminado en cero. */
static const char *elem_txt(const aml_valor *p, unsigned i) {
    if (i >= p->n) return 0;
    const aml_valor *e = &p->v.paquete[i];
    if (e->tipo == AML_CADENA) return e->v.cadena[0] ? e->v.cadena : 0;
    if (e->tipo == AML_BUFFER && e->n && e->v.buffer[0]) {
        for (uint32_t k = 0; k < e->n; k++) if (!e->v.buffer[k]) return (const char *)e->v.buffer;
    }
    return 0;
}
static void nombre_de(const aml *a, int nodo, char *t, size_t n) {
    aml_ruta(a, nodo, t, n);
    /* Basta el ultimo segmento: la ruta completa no aporta al usuario. */
    size_t ult = 0;
    for (size_t i = 0; t[i]; i++) if (t[i] == '.' || t[i] == '\\') ult = i + 1;
    if (ult) { size_t j = 0; while (t[ult + j]) { t[j] = t[ult + j]; j++; } t[j] = 0; }
}

/* 1 conectado, 0 desconectado, -1 no se sabe. */
static int cargadores(aml *a) {
    int hubo = 0, conectado = -1;
    for (int d = aml_siguiente_hid(a, -1, 0, "ACPI0003"); d >= 0; d = aml_siguiente_hid(a, d, 0, "ACPI0003")) {
        int p = aml_hijo(a, d, "_PSR");
        if (p < 0) continue;
        aml_valor v;
        char n[40] = "Cargador (";
        char c[12]; nombre_de(a, d, c, sizeof(c));
        unsigned o = 10; for (unsigned i = 0; c[i] && o < 37; i++) n[o++] = c[i];
        n[o++] = ')'; n[o] = 0;
        fila(n);
        if (aml_evaluar(a, p, 0, 0, &v) || v.tipo != AML_ENTERO) { consola_imprimir("no se pudo leer: "); consola_imprimir_linea(aml_motivo(a)); }
        else {
            consola_imprimir_linea(v.v.entero ? "conectado" : "desconectado");
            if (conectado != 1) conectado = v.v.entero != 0;
        }
        hubo = 1;
        /* _PIF (10.3.3) es opcional: potencia, corriente y tension maximas del cargador. */
        int pif = aml_hijo(a, d, "_PIF");
        if (pif >= 0 && !aml_evaluar(a, pif, 0, 0, &v) && v.tipo == AML_PAQUETE && v.n >= 4) {
            uint64_t mw = elem(&v, 1), ma = elem(&v, 2), mv = elem(&v, 3);
            if (mw && mw != DESCONOCIDO) { fila("Potencia del cargador"); imprimir_fijo(mw); consola_imprimir_linea(" W"); }
            if (ma && ma != DESCONOCIDO) { fila("Corriente máx. cargador"); imprimir_fijo(ma); consola_imprimir_linea(" A"); }
            if (mv && mv != DESCONOCIDO) { fila("Tensión del cargador"); imprimir_fijo(mv); consola_imprimir_linea(" V"); }
        }
    }
    if (!hubo) { fila("Cargador"); consola_imprimir_linea("el firmware no lo informa"); }
    return conectado;
}

static void una_bateria(aml *a, int d, int conectado) {
    char nom[12]; nombre_de(a, d, nom, sizeof(nom));
    int64_t sta = aml_sta(a, d);
    consola_imprimir("  Batería "); consola_imprimir_linea(nom);
    if (sta < 0) { fila("Presente"); consola_imprimir("sin comprobar: "); consola_imprimir_linea(aml_motivo(a)); return; }
    if (!(sta & 0x10)) { fila("Presente"); consola_imprimir_linea("no"); return; }

    /* Datos fijos: _BIX si existe, si no _BIF. Se copian: la siguiente evaluacion reutiliza su memoria. */
    uint64_t unidad = DESCONOCIDO, diseno = DESCONOCIDO, lleno = DESCONOCIDO, v_diseno = DESCONOCIDO, ciclos = DESCONOCIDO, tecno = DESCONOCIDO;
    static char modelo[40], tipo[24], fabricante[40];
    modelo[0] = tipo[0] = fabricante[0] = 0;
    int bi = aml_hijo(a, d, "_BIX"), es_bix = bi >= 0;
    if (bi < 0) bi = aml_hijo(a, d, "_BIF");
    const char *mal_info = 0;
    if (bi >= 0) {
        aml_valor v;
        if (aml_evaluar(a, bi, 0, 0, &v) || v.tipo != AML_PAQUETE) mal_info = aml_motivo(a);
        else {
            /* _BIX (10.2.2.2) lleva delante su revision y, tras la tension, ciclos y exactitud. */
            unsigned o = es_bix ? 1 : 0;
            unidad = elem(&v, o); diseno = elem(&v, o + 1); lleno = elem(&v, o + 2); tecno = elem(&v, o + 3); v_diseno = elem(&v, o + 4);
            if (es_bix) ciclos = elem(&v, 8);
            unsigned b = es_bix ? 16 : 9;
            const char *s;
            if ((s = elem_txt(&v, b))) { unsigned k = 0; while (s[k] && k + 1 < sizeof(modelo)) { modelo[k] = s[k]; k++; } modelo[k] = 0; }
            if ((s = elem_txt(&v, b + 2))) { unsigned k = 0; while (s[k] && k + 1 < sizeof(tipo)) { tipo[k] = s[k]; k++; } tipo[k] = 0; }
            if ((s = elem_txt(&v, b + 3))) { unsigned k = 0; while (s[k] && k + 1 < sizeof(fabricante)) { fabricante[k] = s[k]; k++; } fabricante[k] = 0; }
        }
    }
    int bs = aml_hijo(a, d, "_BST");
    uint64_t estado = DESCONOCIDO, tasa = DESCONOCIDO, resto = DESCONOCIDO, volt = DESCONOCIDO;
    const char *mal_bst = 0;
    if (bs < 0) mal_bst = "el firmware no tiene _BST";
    else {
        aml_valor v;
        if (aml_evaluar(a, bs, 0, 0, &v) || v.tipo != AML_PAQUETE || v.n < 4) mal_bst = aml_motivo(a)[0] ? aml_motivo(a) : "respuesta de _BST invalida";
        else { estado = elem(&v, 0); tasa = elem(&v, 1); resto = elem(&v, 2); volt = elem(&v, 3); }
    }
    const char *u_e = unidad == 0 ? "mWh" : unidad == 1 ? "mAh" : "?";
    if (mal_bst) { fila("Estado"); consola_imprimir("no se pudo leer: "); consola_imprimir_linea(mal_bst); }
    else {
        fila("Estado");
        if (estado & 4) consola_imprimir("crítica, ");
        if (estado & 2) consola_imprimir_linea("cargando");
        else if (estado & 1) consola_imprimir_linea(conectado == 1 ? "descargando aunque el cargador está conectado" : "descargando");
        else if (conectado == 1) consola_imprimir_linea(resto != DESCONOCIDO && lleno != DESCONOCIDO && resto >= lleno ? "llena, con cargador" : "con cargador, sin cargar ahora");
        else consola_imprimir_linea("en reposo");
        fila("Carga");
        if (resto != DESCONOCIDO && lleno && lleno != DESCONOCIDO) {
            consola_imprimir_dec(resto * 100 / lleno);
            consola_imprimir(" % (");
            consola_imprimir_dec(resto); consola_imprimir(" de "); consola_imprimir_dec(lleno);
            consola_imprimir(" "); consola_imprimir(u_e); consola_imprimir_linea(")");
        } else if (resto != DESCONOCIDO) { consola_imprimir_dec(resto); consola_imprimir(" "); consola_imprimir_linea(u_e); }
        else consola_imprimir_linea("desconocida");
        fila("Tensión");
        if (volt != DESCONOCIDO) { imprimir_fijo(volt); consola_imprimir_linea(" V"); } else consola_imprimir_linea("desconocida");
        /* mW directamente; con mA hace falta la tension. */
        uint64_t mw = DESCONOCIDO;
        if (tasa != DESCONOCIDO && unidad == 0) mw = tasa;
        else if (tasa != DESCONOCIDO && unidad == 1 && volt != DESCONOCIDO) mw = tasa * volt / 1000;
        fila(estado & 2 ? "Potencia de carga" : "Consumo");
        if (mw != DESCONOCIDO) { imprimir_fijo(mw); consola_imprimir_linea(" W"); } else consola_imprimir_linea("desconocido");
        /* Lo mismo en corriente: es la unidad en la que muchos firmwares lo miden. */
        uint64_t ma = DESCONOCIDO;
        if (tasa != DESCONOCIDO && unidad == 1) ma = tasa;
        else if (tasa != DESCONOCIDO && unidad == 0 && volt != DESCONOCIDO && volt) ma = tasa * 1000 / volt;
        if (ma != DESCONOCIDO && (estado & 3)) { fila("Corriente"); consola_imprimir_dec(ma); consola_imprimir_linea(" mA"); }
        if (tasa && tasa != DESCONOCIDO && lleno && lleno != DESCONOCIDO && (estado & 3)) {
            uint64_t decimas = tasa * 1000 / lleno;
            fila(estado & 2 ? "Velocidad de carga" : "Velocidad de descarga");
            consola_imprimir_dec(decimas / 10); consola_imprimir("."); consola_imprimir_dec(decimas % 10); consola_imprimir_linea(" % por hora");
        }
        if (tasa && tasa != DESCONOCIDO && resto != DESCONOCIDO && (estado & 3)) {
            uint64_t objetivo = estado & 2 ? (lleno != DESCONOCIDO && lleno > resto ? lleno - resto : 0) : resto;
            uint64_t min = objetivo * 60 / tasa;
            fila(estado & 2 ? "Hasta cargarse" : "Tiempo restante");
            consola_imprimir_dec(min / 60); consola_imprimir(" h "); consola_imprimir_dec(min % 60); consola_imprimir_linea(" min (estimado)");
        }
    }
    if (mal_info) { fila("Datos fijos"); consola_imprimir("no se pudieron leer: "); consola_imprimir_linea(mal_info); }
    else {
        if (diseno && diseno != DESCONOCIDO && lleno != DESCONOCIDO) {
            fila("Salud");
            consola_imprimir_dec(lleno * 100 / diseno);
            consola_imprimir(" % (llena ");
            consola_imprimir_dec(lleno); consola_imprimir(" de "); consola_imprimir_dec(diseno);
            consola_imprimir(" "); consola_imprimir(u_e); consola_imprimir_linea(" de diseño)");
        }
        if (v_diseno != DESCONOCIDO) { fila("Tensión de diseño"); imprimir_fijo(v_diseno); consola_imprimir_linea(" V"); }
        if (ciclos != DESCONOCIDO && ciclos) { fila("Ciclos"); consola_imprimir_dec(ciclos); consola_imprimir_linea(""); }
        if (tecno != DESCONOCIDO) { fila("Recargable"); consola_imprimir_linea(tecno ? "sí" : "no"); }
        if (modelo[0]) { fila("Modelo"); consola_imprimir_linea(modelo); }
        if (tipo[0]) { fila("Tipo"); consola_imprimir_linea(tipo); }
        if (fabricante[0]) { fila("Fabricante"); consola_imprimir_linea(fabricante); }
    }
}

/* Lo mismo que muestra el comando, para quien solo quiere el porcentaje: _BST y _BIF/_BIX de la primera bateria. */
int bateria_resumen(int32_t *pct, int32_t *estado, int32_t *cargador) {
    *pct = -1; *estado = 0; *cargador = -1;
    if (preparar()) return -1;
    aml *a = g_aml;
    aml_permitir_escritura_ec(a, 1);
    *cargador = cargadores(a);
    int d = aml_siguiente_hid(a, -1, EISA_BATERIA, 0);
    int r = -1;
    if (d >= 0) {
        uint64_t lleno = DESCONOCIDO;
        int es_bix = 1, bi = aml_hijo(a, d, "_BIX");
        if (bi < 0) { es_bix = 0; bi = aml_hijo(a, d, "_BIF"); }
        aml_valor v;
        if (bi >= 0 && !aml_evaluar(a, bi, 0, 0, &v) && v.tipo == AML_PAQUETE && v.n >= 9) lleno = elem(&v, es_bix ? 3 : 2);
        int bs = aml_hijo(a, d, "_BST");
        if (bs >= 0 && !aml_evaluar(a, bs, 0, 0, &v) && v.tipo == AML_PAQUETE && v.n >= 4) {
            uint64_t est = elem(&v, 0), resto = elem(&v, 2);
            if (est != DESCONOCIDO) *estado = (int32_t)(est & 7);
            if (resto != DESCONOCIDO && lleno && lleno != DESCONOCIDO) {
                uint64_t p = resto * 100 / lleno;
                *pct = p > 100 ? 100 : (int32_t)p;
                r = 0;
            }
        }
    }
    aml_permitir_escritura_ec(a, 0);
    return r;
}

void bateria_comando(void) {
    consola_imprimir_linea_color("--- BATERÍA ---", COLOR_AVISO_DEFAULT);
    if (preparar()) {
        consola_imprimir("  No se pudo leer el firmware: ");
        consola_imprimir_linea(g_error);
        return;
    }
    aml *a = g_aml;
    struct aml_estadisticas e0, e1;
    aml_estadisticas_leer(a, &e0);
    /* Son los metodos de consulta del firmware, los que usa cualquier sistema operativo: pueden elegir con un registro que leer. */
    aml_permitir_escritura_ec(a, 1);
    int conectado = cargadores(a);
    int hay = 0;
    for (int d = aml_siguiente_hid(a, -1, EISA_BATERIA, 0); d >= 0; d = aml_siguiente_hid(a, d, EISA_BATERIA, 0)) {
        una_bateria(a, d, conectado);
        hay = 1;
    }
    aml_permitir_escritura_ec(a, 0);
    if (!hay) consola_imprimir_linea("  Este equipo no informa de ninguna batería.");
    aml_estadisticas_leer(a, &e1);
    consola_imprimir("  [ACPI] objetos="); consola_imprimir_dec((uint64_t)aml_nodos(a));
    consola_imprimir(" lecturas_ec="); consola_imprimir_dec(e1.lecturas_ec - e0.lecturas_ec);
    consola_imprimir(" escrituras_ec="); consola_imprimir_dec(e1.escrituras_ec - e0.escrituras_ec);
    consola_imprimir(" memoria="); consola_imprimir_dec(e1.lecturas_mem - e0.lecturas_mem);
    consola_imprimir(" puertos="); consola_imprimir_dec(e1.lecturas_io - e0.lecturas_io);
    consola_imprimir(" sombra="); consola_imprimir_dec(e1.escrituras_sombra);
    consola_imprimir_linea("");
}

static void hex2(uint8_t b) {
    static const char h[] = "0123456789abcdef";
    char t[3] = {h[b >> 4], h[b & 15], 0};
    consola_imprimir(t);
}
/* Solo lectura del estado y de los primeros registros, para ver si el controlador embebido contesta y en que punto se queda. */
static void diagnostico_ec(void) {
    preparar();
    uint8_t s = leer_puerto_b(g_ec_orden);
    consola_imprimir("  Puertos: datos "); consola_imprimir_hex(g_ec_datos); consola_imprimir(" ordenes "); consola_imprimir_hex(g_ec_orden); consola_imprimir_linea("");
    consola_imprimir("  Estado: "); hex2(s);
    consola_imprimir_linea(s == 0xff ? " (nadie responde en este puerto)" : "");
    for (unsigned r = 0; r < 16; r++) {
        uint8_t v = 0;
        int rc = h_ec((uint8_t)r, &v);
        consola_imprimir("  EC["); hex2((uint8_t)r); consola_imprimir("] = ");
        if (rc) { consola_imprimir("sin respuesta, estado "); hex2(leer_puerto_b(g_ec_orden)); consola_imprimir_linea(""); break; }
        hex2(v); consola_imprimir_linea("");
    }
}
/* Para estudiar el firmware en el PC: 'acpi tablas', 'acpi ec' y 'acpi volcar <firma> <n> <desde>'. Una tabla no cabe entera en el
 * buffer del stream: se pide por trozos, 'desde' en kilobytes. */
#define VOLCADO_KIB 12
static uint32_t decimal(const char **p) {
    uint32_t v = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') v = v * 10 + (uint32_t)(*(*p)++ - '0');
    return v;
}
/* Todo lo de la bateria para una pantalla, de los mismos metodos del comando. Sin numero de serie. */
void bateria_detalle(struct taek_equipo *t) {
    t->bat_pct = -1; t->bat_cargador = -1;
    t->bat_resto = t->bat_lleno = t->bat_diseno = t->bat_tasa = t->bat_mv = t->bat_ciclos = (uint32_t)DESCONOCIDO;
    if (preparar()) return;
    aml *a = g_aml;
    aml_permitir_escritura_ec(a, 1);
    t->bat_cargador = cargadores(a);
    int d = aml_siguiente_hid(a, -1, EISA_BATERIA, 0);
    if (d >= 0 && aml_sta(a, d) >= 0) {
        int64_t sta = aml_sta(a, d);
        t->bat_presente = (sta & 0x10) != 0;
        int bi = aml_hijo(a, d, "_BIX"), es_bix = bi >= 0;
        if (bi < 0) bi = aml_hijo(a, d, "_BIF");
        aml_valor v;
        if (t->bat_presente && bi >= 0 && !aml_evaluar(a, bi, 0, 0, &v) && v.tipo == AML_PAQUETE && v.n >= 9) {
            unsigned o = es_bix ? 1 : 0;
            t->bat_unidad_mah = elem(&v, o) == 1;
            t->bat_diseno = (uint32_t)elem(&v, o + 1);
            t->bat_lleno = (uint32_t)elem(&v, o + 2);
            if (es_bix) t->bat_ciclos = (uint32_t)elem(&v, 8);
            unsigned b = es_bix ? 16 : 9;
            const char *s;
            if ((s = elem_txt(&v, b))) { unsigned k = 0; while (s[k] && k + 1 < sizeof(t->bat_modelo)) { t->bat_modelo[k] = s[k]; k++; } t->bat_modelo[k] = 0; }
            if ((s = elem_txt(&v, b + 2))) { unsigned k = 0; while (s[k] && k + 1 < sizeof(t->bat_tipo)) { t->bat_tipo[k] = s[k]; k++; } t->bat_tipo[k] = 0; }
        }
        int bs = aml_hijo(a, d, "_BST");
        if (t->bat_presente && bs >= 0 && !aml_evaluar(a, bs, 0, 0, &v) && v.tipo == AML_PAQUETE && v.n >= 4) {
            uint64_t est = elem(&v, 0);
            if (est != DESCONOCIDO) t->bat_estado = (int32_t)(est & 7);
            t->bat_tasa = (uint32_t)elem(&v, 1);
            t->bat_resto = (uint32_t)elem(&v, 2);
            t->bat_mv = (uint32_t)elem(&v, 3);
            if (t->bat_resto != (uint32_t)DESCONOCIDO && t->bat_lleno && t->bat_lleno != (uint32_t)DESCONOCIDO) {
                uint64_t p = (uint64_t)t->bat_resto * 100 / t->bat_lleno;
                t->bat_pct = p > 100 ? 100 : (int32_t)p;
            }
        }
    }
    aml_permitir_escritura_ec(a, 0);
}

/* Texto de un _HID o _CID: cadena tal cual, o entero con el codigo EISA de tres letras y cuatro cifras. */
static int id_texto(const aml_valor *v, char *s, unsigned tam) {
    static const char h[] = "0123456789ABCDEF";
    if (v->tipo == AML_CADENA && v->v.cadena) {
        unsigned i = 0;
        while (v->v.cadena[i] && i + 1 < tam) { s[i] = v->v.cadena[i]; i++; }
        s[i] = 0;
        return i != 0;
    }
    if (v->tipo != AML_ENTERO || tam < 8) return 0;
    uint32_t e = (uint32_t)v->v.entero;
    uint32_t b = (e & 0xFF) << 8 | ((e >> 8) & 0xFF);
    s[0] = (char)('@' + ((b >> 10) & 31)); s[1] = (char)('@' + ((b >> 5) & 31)); s[2] = (char)('@' + (b & 31));
    s[3] = h[(e >> 20) & 15]; s[4] = h[(e >> 16) & 15]; s[5] = h[(e >> 28) & 15]; s[6] = h[(e >> 24) & 15]; s[7] = 0;
    return 1;
}
int acpi_id_texto(const aml_valor *v, char *s, unsigned tam) { return id_texto(v, s, tam); }
aml *acpi_interprete(void) { return preparar() ? 0 : g_aml; }

static int empieza(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int id_de_puntero(const char *s) {
    static const char *const prefijos[] = { "PNP0F", "PNP0C50", "ACPI0C50", "SYN", "ELAN", "ELN", "ATML", "MSFT", "CYAP", "ALP", "DLL", "DELL", "FTE", "ETD", "HPQ", "LEN", "ASUE", "TPD", "WCOM" };
    for (unsigned i = 0; i < sizeof(prefijos) / sizeof(prefijos[0]); i++) if (empieza(s, prefijos[i])) return 1;
    return 0;
}
/* Solo lectura: que dice el firmware del touchpad o raton integrado, y si los controladores I2C del chipset se ven por PCI. */
static void acpi_punteros(void) {
    if (preparar()) { consola_imprimir("  No se pudo leer el firmware: "); consola_imprimir_linea(g_error); return; }
    aml *a = g_aml;
    unsigned vistos = 0, total = 0;
    static const char *const nombres[] = { "_HID", "_CID" };
    for (unsigned k = 0; k < 2; k++) {
        for (int d = aml_siguiente_nombre(a, -1, nombres[k]); d >= 0; d = aml_siguiente_nombre(a, d, nombres[k])) {
            aml_valor v;
            char id[32], ruta[96];
            total++;
            if (aml_evaluar(a, d, 0, 0, &v) || !id_texto(&v, id, sizeof(id)) || !id_de_puntero(id)) continue;
            aml_ruta(a, d, ruta, sizeof(ruta));
            consola_imprimir("  [PUNTERO] "); consola_imprimir(nombres[k]); consola_imprimir(" "); consola_imprimir(id);
            consola_imprimir(" en "); consola_imprimir(ruta);
            /* _STA del dispositivo: bit 0 presente, bit 1 activo. Quita el nombre final para llegar al padre. */
            unsigned largo = 0;
            while (ruta[largo]) largo++;
            if (largo > 5) {
                ruta[largo - 5] = 0;
                int padre = aml_buscar(a, ruta);
                if (padre >= 0) { consola_imprimir(" _STA="); int64_t st = aml_sta(a, padre); if (st < 0) consola_imprimir("error"); else consola_imprimir_dec((uint64_t)st); }
            }
            consola_imprimir_linea("");
            vistos++;
        }
    }
    consola_imprimir("  [PUNTERO] identificadores revisados="); consola_imprimir_dec(total);
    consola_imprimir(" de puntero="); consola_imprimir_dec(vistos); consola_imprimir_linea("");
    for (int i = 0; i < pci_obtener_conteo(); i++) {
        const struct dispositivo_pci *q = pci_obtener_dispositivo(i);
        /* Sunrise Point: I2C 9D60-9D65 */
        if (q && q->id_proveedor == 0x8086 && q->id_dispositivo >= 0x9D60 && q->id_dispositivo <= 0x9D65) {
            consola_imprimir("  [PUNTERO] I2C PCI 8086:"); consola_imprimir_hex(q->id_dispositivo); consola_imprimir_linea("");
        }
    }
    consola_imprimir_linea("  [PUNTERO] FIN");
}

/* 'acpi evaluar <ruta> [enteros]': lo que devuelve un objeto o metodo del firmware. Es seguro porque el interprete
 * no escribe al equipo: memoria en sombra, puertos y PCI lo detienen, y el controlador embebido queda cerrado. */
static void valor_imprimir(const aml_valor *v, unsigned prof) {
    switch (v->tipo) {
    case AML_ENTERO: consola_imprimir("entero "); consola_imprimir_hex(v->v.entero); break;
    case AML_CADENA: consola_imprimir("cadena \""); consola_imprimir(v->v.cadena ? v->v.cadena : ""); consola_imprimir("\""); break;
    case AML_BUFFER:
        consola_imprimir("buffer de "); consola_imprimir_dec(v->n); consola_imprimir(" bytes:");
        for (uint32_t i = 0; i < v->n && i < 96; i++) { consola_imprimir(" "); hex2(v->v.buffer[i]); }
        if (v->n > 96) consola_imprimir(" ...");
        break;
    case AML_PAQUETE:
        consola_imprimir("paquete de "); consola_imprimir_dec(v->n); consola_imprimir(" { ");
        for (uint32_t i = 0; i < v->n && i < 16 && prof < 3; i++) { valor_imprimir(&v->v.paquete[i], prof + 1); consola_imprimir("; "); }
        consola_imprimir("}");
        break;
    default: consola_imprimir("tipo "); consola_imprimir_dec(v->tipo); break;
    }
}
static uint64_t numero(const char **p) {
    const char *s = *p;
    while (*s == ' ') s++;
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        for (;; s++) {
            int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + (uint64_t)d;
        }
    } else while (*s >= '0' && *s <= '9') v = v * 10 + (uint64_t)(*s++ - '0');
    *p = s;
    return v;
}
static void acpi_evaluar(const char *p) {
    if (preparar()) { consola_imprimir("  No se pudo leer el firmware: "); consola_imprimir_linea(g_error); return; }
    while (*p == ' ') p++;
    char ruta[96]; unsigned k = 0;
    while (*p && *p != ' ' && k + 1 < sizeof(ruta)) ruta[k++] = *p++;
    ruta[k] = 0;
    if (!k) { consola_imprimir_linea("  Uso: acpi evaluar <ruta absoluta> [enteros]   ej.: acpi evaluar \\GPDI"); return; }
    aml_valor args[4]; unsigned na = 0;
    while (na < 4) {
        while (*p == ' ') p++;
        if (!*p) break;
        args[na].tipo = AML_ENTERO; args[na].n = 0; args[na].ruta_ambito = 0; args[na].v.entero = numero(&p);
        na++;
    }
    int nodo = aml_buscar(g_aml, ruta);
    if (nodo < 0) { consola_imprimir("  [ACPI] No existe "); consola_imprimir_linea(ruta); return; }
    aml_valor v;
    consola_imprimir("  [ACPI] "); consola_imprimir(ruta); consola_imprimir(" = ");
    if (aml_evaluar(g_aml, nodo, args, na, &v)) { consola_imprimir("no se pudo evaluar: "); consola_imprimir_linea(aml_motivo(g_aml)); return; }
    valor_imprimir(&v, 0);
    consola_imprimir_linea("");
}

void acpi_comando(const char *arg) {
    const uint8_t *t; uint32_t n;
    if (arg[0] == 'e' && arg[1] == 'v') { const char *q = arg; while (*q && *q != ' ') q++; acpi_evaluar(q); return; }
    if (arg[0] == 'e' && arg[1] == 'c') { diagnostico_ec(); return; }
    if (arg[0] == 'p') { acpi_punteros(); return; }
    if (arg[0] == 't') {
        char f[5];
        for (unsigned i = 0; !acpi_tabla_listar(i, f, &t, &n); i++) {
            consola_imprimir("  "); consola_imprimir(f); consola_imprimir(" "); consola_imprimir_dec(n); consola_imprimir_linea(" bytes");
        }
        return;
    }
    if (arg[0] != 'v' || arg[1] != 'o' || arg[6] != ' ') { consola_imprimir_linea("acpi tablas | acpi ec | acpi volcar <firma> <n> <desde en KiB>"); return; }
    const char *f = arg + 7;
    if (!f[0] || !f[1] || !f[2] || !f[3]) { consola_imprimir_linea("  Firma de cuatro letras."); return; }
    /* Solo el firmware de plataforma: las tablas de licencia guardan datos del propietario. */
    if ((f[0] == 'M' && f[1] == 'S' && f[2] == 'D' && f[3] == 'M') || (f[0] == 'S' && f[1] == 'L' && f[2] == 'I' && f[3] == 'C')) { consola_imprimir_linea("  Tabla de licencia: no se vuelca."); return; }
    const char *q = f + 4;
    uint32_t idx = decimal(&q), desde = decimal(&q) * 1024;
    if (acpi_tabla_buscar(f, idx, &t, &n)) { consola_imprimir_linea("  Tabla no encontrada."); return; }
    uint32_t hasta = desde + VOLCADO_KIB * 1024;
    if (hasta > n) hasta = n;
    consola_imprimir("[ACPI_VOLCADO] INICIO bytes="); consola_imprimir_dec(n);
    consola_imprimir(" desde="); consola_imprimir_dec(desde);
    consola_imprimir(" hasta="); consola_imprimir_dec(hasta); consola_imprimir_linea("");
    for (uint32_t i = desde; i < hasta; i += 64) {
        consola_imprimir("[AX] ");
        for (uint32_t j = i; j < i + 64 && j < hasta; j++) hex2(t[j]);
        consola_imprimir_linea("");
    }
    consola_imprimir_linea("[ACPI_VOLCADO] FIN");
}
