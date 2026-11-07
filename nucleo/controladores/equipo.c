/* Comando 'especificaciones': los datos del equipo, de CPUID, SMBIOS y PCI. No muestra numeros de serie ni identificadores unicos. */
#define LIMINE_API_REVISION 2
#include "../../boot/limine/limine.h"
#include "equipo.h"
#include "consola.h"
#include "iommu.h"
#include "pantalla.h"
#include "usb_msc.h"
#include "../arquitectura/x86_64/pci.h"
#include "../base/memoria.h"
#include "../base/tiempo.h"
#include "../base/trabajos.h"
#include "../base/version.h"

__attribute__((used, section(".requests")))
static volatile struct limine_smbios_request g_peticion_smbios = {
    .id = LIMINE_SMBIOS_REQUEST,
    .revision = 0
};

static void fila(const char *n) {
    consola_imprimir("  ");
    consola_imprimir(n);
    unsigned vis = 0;
    for (unsigned i = 0; n[i]; i++) if ((n[i] & 0xc0) != 0x80) vis++;
    for (; vis < 20; vis++) consola_imprimir(" ");
    consola_imprimir(": ");
}

/* ------------------------------------------------------------------ CPU */
static void cpu_info(void) {
    uint32_t a, b, c, d;
    char marca[49];
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    fila("Procesador");
    if (a >= 0x80000004u) {
        uint32_t *m = (uint32_t *)marca;
        for (uint32_t i = 0; i < 3; i++)
            __asm__ volatile("cpuid" : "=a"(m[i * 4]), "=b"(m[i * 4 + 1]), "=c"(m[i * 4 + 2]), "=d"(m[i * 4 + 3]) : "a"(0x80000002u + i), "c"(0));
        marca[48] = 0;
        const char *p = marca;
        while (*p == ' ') p++;
        consola_imprimir_linea(p);
    } else consola_imprimir_linea("sin marca informada");
    fila("Núcleos en uso");
    consola_imprimir_dec(trabajos_cpu_arrancadas());
    consola_imprimir_linea(" procesadores lógicos arrancados");
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1u), "c"(0));
    unsigned fam = (a >> 8) & 15, mod = (a >> 4) & 15, step = a & 15;
    if (fam == 6 || fam == 15) mod |= ((a >> 16) & 15) << 4;
    if (fam == 15) fam += (a >> 20) & 255;
    fila("Familia/modelo");
    consola_imprimir_dec(fam); consola_imprimir("/"); consola_imprimir_dec(mod); consola_imprimir(" paso "); consola_imprimir_dec(step); consola_imprimir_linea("");
    fila("Frecuencia TSC");
    consola_imprimir_dec(tiempo_ciclos_por_ms() / 1000);
    consola_imprimir_linea(" MHz (medida)");
}

/* ------------------------------------------------------------------ SMBIOS */
static const uint8_t *g_sm; static uint32_t g_sm_n;
static void smbios_abrir(void) {
    if (g_sm || !g_peticion_smbios.response) return;
    const struct limine_smbios_response *r = g_peticion_smbios.response;
    uint64_t f = 0; uint32_t n = 0;
    /* Segun la revision del protocolo llega como direccion fisica o ya en el HHDM. */
    uint64_t hhdm = memoria_obtener_hhdm_offset(), e64 = r->entry_64, e32 = r->entry_32;
    if (e64 >= UINT64_C(0xFFFF800000000000)) e64 -= hhdm;
    if (e32 >= UINT64_C(0xFFFF800000000000)) e32 -= hhdm;
    if (e64) {
        const uint8_t *e = acpi_mapear_lectura(e64);
        if (e && e[0] == '_' && e[1] == 'S' && e[2] == 'M' && e[3] == '3') { for (unsigned i = 0; i < 4; i++) n |= (uint32_t)e[0xc + i] << (8 * i); for (unsigned i = 0; i < 8; i++) f |= (uint64_t)e[0x10 + i] << (8 * i); }
    }
    if (!f && e32) {
        const uint8_t *e = acpi_mapear_lectura(e32);
        if (e && e[0] == '_' && e[1] == 'S' && e[2] == 'M') { n = (uint32_t)e[0x16] | (uint32_t)e[0x17] << 8; for (unsigned i = 0; i < 4; i++) f |= (uint64_t)e[0x18 + i] << (8 * i); }
    }
    if (!f || !n || n > 1024 * 1024) return;
    /* La tabla se copia para no depender de cuantas paginas ocupe en memoria de firmware. */
    uint8_t *c = asignar_memoria(n);
    if (!c) return;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *q = acpi_mapear_lectura(f + i);
        if (!q) { liberar_memoria(c); return; }
        uint32_t en_pag = 4096 - (uint32_t)((f + i) & 4095);
        if (en_pag > n - i) en_pag = n - i;
        for (uint32_t j = 0; j < en_pag; j++) c[i + j] = q[j];
        i += en_pag - 1;
    }
    g_sm = c; g_sm_n = n;
}
/* Texto n de la estructura; "" si no hay. */
static const char *sm_txt(const uint8_t *e, uint32_t resto, uint8_t i) {
    if (!i) return "";
    uint32_t o = e[1];
    uint8_t k = 1;
    while (o < resto && e[o]) {
        if (k == i) return (const char *)e + o;
        while (o < resto && e[o]) o++;
        o++; k++;
    }
    return "";
}
static const uint8_t *sm_siguiente(const uint8_t *e) {
    if (!e) e = g_sm;
    else {
        uint32_t o = e[1];
        uint32_t resto = (uint32_t)(g_sm + g_sm_n - e);
        while (o + 1 < resto && (e[o] || e[o + 1])) o++;
        e += o + 2;
    }
    if (!g_sm || e + 4 > g_sm + g_sm_n || e[1] < 4 || e[0] == 127) return 0;
    return e;
}
static int sm_valido(const char *s) {
    /* Los fabricantes rellenan con esto lo que no quieren decir. */
    static const char *const nada[] = {"", "To Be Filled By O.E.M.", "Default string", "Not Specified", "System Product Name", "None", "Unknown", 0};
    for (unsigned i = 0; nada[i]; i++) {
        const char *p = s, *q = nada[i];
        while (*p && *p == *q) { p++; q++; }
        if (!*p && !*q) return 0;
    }
    return 1;
}
static void dos(const char *a, const char *b) {
    if (sm_valido(a)) { consola_imprimir(a); if (sm_valido(b)) consola_imprimir(" "); }
    if (sm_valido(b)) consola_imprimir(b);
    consola_imprimir_linea(sm_valido(a) || sm_valido(b) ? "" : "no informado");
}
static const char *chasis(uint8_t t) {
    switch (t & 0x7f) {
        case 3: return "sobremesa"; case 4: return "sobremesa bajo"; case 6: return "minitorre"; case 7: return "torre";
        case 8: return "portátil"; case 9: return "portátil"; case 10: return "portátil"; case 14: return "subportátil";
        case 13: return "todo en uno"; case 17: return "servidor"; case 23: return "servidor"; case 30: return "tableta"; case 31: return "convertible";
        case 32: return "desmontable"; case 35: return "minipc"; case 36: return "bloque"; default: return 0;
    }
}
static const char *tipo_ram(uint8_t t) {
    switch (t) {
        case 0x12: return "DDR"; case 0x13: return "DDR2"; case 0x18: return "DDR3"; case 0x1a: return "DDR4"; case 0x1b: return "LPDDR";
        case 0x1c: return "LPDDR2"; case 0x1d: return "LPDDR3"; case 0x1e: return "LPDDR4"; case 0x22: return "DDR5"; case 0x23: return "LPDDR5";
        default: return 0;
    }
}
static void equipo_smbios(void) {
    smbios_abrir();
    if (!g_sm) { fila("Modelo"); consola_imprimir_linea("el firmware no entrega SMBIOS"); return; }
    for (const uint8_t *e = sm_siguiente(0); e; e = sm_siguiente(e)) {
        uint32_t resto = (uint32_t)(g_sm + g_sm_n - e);
        if (e[0] == 1 && e[1] >= 8) { fila("Equipo"); dos(sm_txt(e, resto, e[4]), sm_txt(e, resto, e[5])); }
    }
    for (const uint8_t *e = sm_siguiente(0); e; e = sm_siguiente(e)) {
        uint32_t resto = (uint32_t)(g_sm + g_sm_n - e);
        if (e[0] == 3 && e[1] >= 6 && chasis(e[5])) { fila("Formato"); consola_imprimir_linea(chasis(e[5])); }
        if (e[0] == 2 && e[1] >= 8) { fila("Placa"); dos(sm_txt(e, resto, e[4]), sm_txt(e, resto, e[5])); }
    }
    for (const uint8_t *e = sm_siguiente(0); e; e = sm_siguiente(e)) {
        uint32_t resto = (uint32_t)(g_sm + g_sm_n - e);
        if (e[0] == 0 && e[1] >= 9) {
            fila("Firmware");
            dos(sm_txt(e, resto, e[4]), sm_txt(e, resto, e[5]));
            fila("Fecha del firmware");
            consola_imprimir_linea(sm_valido(sm_txt(e, resto, e[8])) ? sm_txt(e, resto, e[8]) : "no informada");
        }
    }
}
static void ram(void) {
    memoria_estadisticas_t m;
    memoria_obtener_estadisticas(&m);
    fila("Memoria");
    /* ram_fisica_total cuenta tambien los huecos del mapa de direcciones: no es memoria instalada. */
    consola_imprimir_dec(m.ram_fisica_usable / (1024 * 1024));
    consola_imprimir(" MiB usables, ");
    consola_imprimir_dec(m.paginas_libres * 4096 / (1024 * 1024));
    consola_imprimir_linea(" MiB libres");
    if (!g_sm) return;
    for (const uint8_t *e = sm_siguiente(0); e; e = sm_siguiente(e)) {
        if (e[0] != 17 || e[1] < 0x15) continue;
        uint16_t tam = (uint16_t)(e[0xc] | e[0xd] << 8);
        if (!tam || tam == 0xffff) continue;
        uint64_t mb = tam == 0x7fff && e[1] >= 0x20 ? (uint64_t)((uint32_t)e[0x1c] | (uint32_t)e[0x1d] << 8 | (uint32_t)e[0x1e] << 16 | (uint32_t)e[0x1f] << 24) : (tam & 0x8000 ? (tam & 0x7fffu) / 1024u : tam);
        uint32_t resto = (uint32_t)(g_sm + g_sm_n - e);
        fila("  Módulo");
        consola_imprimir_dec(mb);
        consola_imprimir(" MiB");
        if (tipo_ram(e[0x12])) { consola_imprimir(" "); consola_imprimir(tipo_ram(e[0x12])); }
        if (e[1] >= 0x17) {
            uint16_t mhz = (uint16_t)(e[0x15] | e[0x16] << 8);
            if (mhz && mhz != 0xffff) { consola_imprimir(" "); consola_imprimir_dec(mhz); consola_imprimir(" MT/s"); }
        }
        if (sm_valido(sm_txt(e, resto, e[0x10]))) { consola_imprimir(" en "); consola_imprimir(sm_txt(e, resto, e[0x10])); }
        consola_imprimir_linea("");
    }
}

/* ------------------------------------------------------------------ PCI */
static void hex4(uint16_t v) {
    static const char h[] = "0123456789abcdef";
    char t[5] = {h[v >> 12], h[(v >> 8) & 15], h[(v >> 4) & 15], h[v & 15], 0};
    consola_imprimir(t);
}
static void pci_clase(const char *nombre, uint8_t clase) {
    for (int i = 0; i < pci_obtener_conteo(); i++) {
        const struct dispositivo_pci *d = pci_obtener_dispositivo(i);
        if (!d || d->clase != clase) continue;
        fila(nombre);
        consola_imprimir(pci_nombre_proveedor(d->id_proveedor));
        consola_imprimir(" ");
        hex4(d->id_proveedor); consola_imprimir(":"); hex4(d->id_dispositivo);
        consola_imprimir(" (");
        consola_imprimir(pci_nombre_clase(d->clase, d->subclase));
        consola_imprimir_linea(")");
    }
}
static void discos(void) {
    for (uint8_t i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
        const struct usb_msc_dispositivo *d = usb_msc_obtener_dispositivo(i);
        if (!d || !d->activo || !d->listo) continue;
        fila("Disco USB");
        consola_imprimir(d->fabricante); consola_imprimir(" "); consola_imprimir(d->producto);
        consola_imprimir(" ");
        consola_imprimir_dec(d->capacidad_bytes / (1024 * 1024 * 1024));
        consola_imprimir_linea(" GiB");
    }
}

void equipo_especificaciones(void) {
    consola_imprimir_linea_color("--- ESPECIFICACIONES DEL SISTEMA ---", COLOR_AVISO_DEFAULT);
    equipo_smbios();
    cpu_info();
    ram();
    pci_clase("Gráficos", 3);
    fila("Pantalla");
    consola_imprimir_dec(pantalla_obtener_ancho()); consola_imprimir("x"); consola_imprimir_dec(pantalla_obtener_alto()); consola_imprimir_linea(" (modo del firmware)");
    pci_clase("Red", 2);
    pci_clase("Sonido o multimedia", 4);
    pci_clase("Almacenamiento", 1);
    discos();
    fila("Sistema");
    consola_imprimir("TAEK OS "); consola_imprimir_linea(taek_obtener_version());
}
