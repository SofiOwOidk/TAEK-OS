// Prueba de integración host para FAT32: compilar junto a fat32.c y ejecutar sobre
// una imagen FAT32 creada con mkfs.fat, seguida de fsck.fat -n.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../nucleo/controladores/fat32.h"
#include "../nucleo/controladores/usb_msc.h"

static FILE *g_disco;
static struct usb_msc_dispositivo g_dev;

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
    if (fseek(g_disco, (long)lba * 512L, SEEK_SET)) return -1;
    return fwrite(buf, 512, n, g_disco) == n && fflush(g_disco) == 0 ? 0 : -1;
}

void consola_imprimir(const char *s) { fputs(s, stdout); }
void consola_imprimir_linea(const char *s) { puts(s); }
void consola_imprimir_color(const char *s, uint32_t color) { (void)color; fputs(s, stdout); }
void consola_imprimir_linea_color(const char *s, uint32_t color) { (void)color; puts(s); }
void consola_imprimir_dec(uint64_t n) { printf("%llu", (unsigned long long)n); }
void consola_imprimir_hex(uint64_t n) { printf("%llx", (unsigned long long)n); }
void consola_escribir_caracter(char c) { putchar(c); }

void serial_imprimir(const char *s) { (void)s; }
void serial_imprimir_linea(const char *s) { (void)s; }
void serial_imprimir_dec(uint64_t n) { (void)n; }

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s fat32.img\n", argv[0]);
        return 2;
    }
    g_disco = fopen(argv[1], "r+b");
    if (!g_disco) {
        perror(argv[1]);
        return 2;
    }
    if (fseek(g_disco, 0, SEEK_END)) return 2;
    long bytes = ftell(g_disco);
    if (bytes <= 0 || bytes % 512) return 2;

    g_dev.activo = g_dev.listo = 1;
    g_dev.tamano_sector = 512;
    g_dev.sectores_totales = (uint32_t)(bytes / 512);

    printf("==> Probando fat32_montar()...\n");
    if (fat32_montar(0) != 0) {
        fprintf(stderr, "ERROR: Montaje FAT32 falló.\n");
        return 1;
    }
    printf("==> fat32_montar() exitoso!\n");

    printf("\n--- PROBANDO LISTADO FAT32 ---\n");
    if (fat32_listar_directorio(NULL) != 0) {
        fprintf(stderr, "ERROR: fat32_listar_directorio falló.\n");
        return 1;
    }

    printf("\n--- PROBANDO LECTURA DE ARCHIVO 'leeme.txt' ---\n");
    if (fat32_leer_archivo_texto("leeme.txt") != 0) {
        fprintf(stderr, "ERROR: Lectura de 'leeme.txt' falló.\n");
        return 1;
    }

    printf("\n--- PROBANDO LECTURA DE ARCHIVO 'largo.txt' ---\n");
    if (fat32_leer_archivo_texto("largo.txt") != 0) {
        fprintf(stderr, "ERROR: Lectura de 'largo.txt' falló.\n");
        return 1;
    }

    printf("\n--- PROBANDO ÁRBOL FAT32 ---\n");
    if (fat32_ejecutar_tree(NULL) != 0) {
        fprintf(stderr, "ERROR: fat32_ejecutar_tree falló.\n");
        return 1;
    }

    printf("\n--- PROBANDO ESCRITURA EN FAT32 ---\n");
    const char *nuevo_texto = "Hola desde TAEK OS en FAT32!";
    if (fat32_crear_archivo("nuevo.txt", (const uint8_t *)nuevo_texto, (uint32_t)strlen(nuevo_texto)) != 0) {
        fprintf(stderr, "ERROR: Creación de archivo 'nuevo.txt' falló.\n");
        return 1;
    }
    if (fat32_crear_directorio("nuevadir") != 0) {
        fprintf(stderr, "ERROR: Creación de directorio 'nuevadir' falló.\n");
        return 1;
    }

    printf("\n--- DESMONTANDO Y RE-MONTANDO PARA COMPROBAR PERSISTENCIA Y RE-LECTURA ---\n");
    fat32_desmontar();
    if (fat32_montar(0) != 0) {
        fprintf(stderr, "ERROR: Re-montaje FAT32 falló.\n");
        return 1;
    }

    printf("\n--- PROBANDO LECTURA DE ARCHIVO RECIÉN ESCRITO 'nuevo.txt' ---\n");
    if (fat32_leer_archivo_texto("nuevo.txt") != 0) {
        fprintf(stderr, "ERROR: Lectura de 'nuevo.txt' recién creado falló.\n");
        return 1;
    }

    fclose(g_disco);
    printf("\n==> [PRUEBA FAT32 COMPLETADA EXITOSAMENTE]\n");
    return 0;
}
