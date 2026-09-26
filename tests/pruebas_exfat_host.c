// Prueba de integración host: compilar junto a exfat.c y ejecutar sobre una
// COPIA desechable de una imagen exFAT, seguida de fsck.exfat -n.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nucleo/controladores/exfat.h"
#include "../nucleo/controladores/usb_msc.h"

static FILE *g_disco;
static struct usb_msc_dispositivo g_dev;
static int g_escrituras;
static int g_fallar_escritura;

const struct usb_msc_dispositivo *usb_msc_obtener_dispositivo(uint8_t id) {
    return id == 0 ? &g_dev : NULL;
}

int usb_msc_leer_sectores(uint8_t id, uint32_t lba, uint16_t n, void *buf) {
    if (id || !buf || (uint64_t)lba + n > g_dev.sectores_totales) return -1;
    if (fseek(g_disco, (long)lba * 512L, SEEK_SET)) return -1;
    return fread(buf, 512, n, g_disco) == n ? 0 : -1;
}

int usb_msc_escribir_sectores(uint8_t id, uint32_t lba, uint16_t n, const void *buf) {
    if (id || !buf || (uint64_t)lba + n > g_dev.sectores_totales) return -1;
    if (++g_escrituras == g_fallar_escritura) return -1;
    if (fseek(g_disco, (long)lba * 512L, SEEK_SET)) return -1;
    return fwrite(buf, 512, n, g_disco) == n && fflush(g_disco) == 0 ? 0 : -1;
}

static int esta_sucio(long lba) {
    uint8_t vbr[512];
    if (fseek(g_disco, lba * 512L, SEEK_SET) || fread(vbr, 1, sizeof(vbr), g_disco) != sizeof(vbr)) return -1;
    const struct exfat_vbr *b = (const struct exfat_vbr *)vbr;
    return (b->banderas_volumen & 2) != 0;
}

void consola_imprimir(const char *s) { fputs(s, stdout); }
void consola_imprimir_linea(const char *s) { puts(s); }
void consola_imprimir_color(const char *s, uint32_t color) { (void)color; fputs(s, stdout); }
void consola_imprimir_linea_color(const char *s, uint32_t color) { (void)color; puts(s); }
void consola_imprimir_dec(uint64_t n) { printf("%llu", (unsigned long long)n); }
void consola_imprimir_hex(uint64_t n) { printf("%llx", (unsigned long long)n); }
void consola_escribir_caracter(char c) { putchar(c); }

int main(int argc, char **argv) {
    if (argc != 2 && (argc != 3 || strcmp(argv[2], "--fallo-io"))) {
        fprintf(stderr, "Uso: %s copia.img [--fallo-io]\n", argv[0]); return 2;
    }
    g_disco = fopen(argv[1], "r+b");
    if (!g_disco) { perror(argv[1]); return 2; }
    if (fseek(g_disco, 0, SEEK_END)) return 2;
    long bytes = ftell(g_disco);
    if (bytes <= 0 || bytes % 512) return 2;
    g_dev.activo = g_dev.listo = 1;
    g_dev.tamano_sector = 512;
    g_dev.sectores_totales = (uint32_t)(bytes / 512);
    if (exfat_montar(0)) { fprintf(stderr, "Montaje exFAT falló\n"); return 1; }
    uint8_t datos[10000];
    for (size_t i = 0; i < sizeof(datos); i++) datos[i] = (uint8_t)('A' + i % 26);
    if (argc == 3) {
        g_fallar_escritura = 4; // Dirty principal, Dirty respaldo, Bitmap, primer cluster.
        if (exfat_crear_archivo("largo.txt", datos, sizeof(datos)) == 0 ||
            esta_sucio(0) != 1 || esta_sucio(12) != 1) {
            fprintf(stderr, "La escritura fallida no conservó VolumeDirty\n");
            return 1;
        }
        fclose(g_disco);
        return 0;
    }
    if (exfat_crear_archivo("largo.txt", datos, sizeof(datos)) ||
        exfat_crear_directorio("subdir")) {
        fprintf(stderr, "Creación exFAT falló\n");
        return 1;
    }
    if (exfat_crear_archivo("LARGO.TXT", datos, 1) == 0 ||
        exfat_crear_directorio("SUBDIR") == 0) {
        fprintf(stderr, "Se aceptó un nombre duplicado en exFAT\n");
        return 1;
    }
    if (esta_sucio(0) != 0 || esta_sucio(12) != 0) {
        fprintf(stderr, "VolumeDirty no se limpió tras completar la escritura\n");
        return 1;
    }

    // Desmontar y re-montar para verificar lectura fría persistida en disco
    exfat_desmontar();
    if (exfat_montar(0)) {
        fprintf(stderr, "Re-montaje exFAT falló\n");
        return 1;
    }

    // Validar listado de directorios
    printf("\n--- PROBANDO LISTADO exFAT ---\n");
    if (exfat_listar_directorio(NULL)) {
        fprintf(stderr, "Listado de directorio exFAT falló\n");
        return 1;
    }

    // Validar lectura completa de archivo
    printf("\n--- PROBANDO LECTURA DE ARCHIVO exFAT ---\n");
    if (exfat_leer_archivo_texto("largo.txt")) {
        fprintf(stderr, "Lectura de archivo 'largo.txt' en exFAT falló\n");
        return 1;
    }

    // Validar visualización de árbol
    printf("\n--- PROBANDO ÁRBOL exFAT ---\n");
    if (exfat_ejecutar_tree(NULL)) {
        fprintf(stderr, "Ejecución de tree en exFAT falló\n");
        return 1;
    }

    fclose(g_disco);
    return 0;
}
