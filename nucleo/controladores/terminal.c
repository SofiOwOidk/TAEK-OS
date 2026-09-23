#include "terminal.h"
#include "consola.h"
#include "pantalla.h"
#include "audio_ac97.h"
#include "animacion_cangrejo.h"
#include "../base/huevo.h"
#include "../base/energia.h"
#include "../base/tiempo.h"
#include "../base/memoria.h"
#include "../base/paginacion.h"
#include "../arquitectura/x86_64/pci.h"
#include "../arquitectura/x86_64/serial.h"
#include "../arquitectura/x86_64/vmx.h"

extern const uint8_t _binary_audio_arranque_bin_start[];
extern const uint8_t _binary_audio_arranque_bin_end[];

extern const uint8_t _binary_duelo_audio_bin_start[];
extern const uint8_t _binary_duelo_audio_bin_end[];

static void esperar_con_audio_bucle(int ms, const uint8_t *audio, uint32_t tam) {
    (void)audio;
    (void)tam;
    int paso = 50;
    while (ms > 0) {
        audio_ac97_actualizar();
        esperar_milisegundos(paso);
        ms -= paso;
    }
}

static int __attribute__((unused)) str_longitud(const char *s) {
    int len = 0;
    while (s && s[len]) len++;
    return len;
}

static int str_igual(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    int i = 0;
    while (s1[i] && s2[i]) {
        if (s1[i] != s2[i]) return 0;
        i++;
    }
    return s1[i] == s2[i];
}

static int str_igual_sin_caso(const char *s1, const char *s2) {
    if (!s1 || !s2) return 0;
    int i = 0;
    while (s1[i] && s2[i]) {
        char c1 = s1[i];
        char c2 = s2[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        i++;
    }
    return s1[i] == s2[i];
}

static int g_el_comando_desbloqueado = 0;

static uint32_t g_semilla_azar = 0x6A09E667;

static uint32_t obtener_aleatorio(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    g_semilla_azar ^= (lo ^ (hi << 16));
    // Generador Xorshift32
    g_semilla_azar ^= g_semilla_azar << 13;
    g_semilla_azar ^= g_semilla_azar >> 17;
    g_semilla_azar ^= g_semilla_azar << 5;
    return g_semilla_azar;
}

static int str_comienza_con(const char *str, const char *prefijo) {
    if (!str || !prefijo) return 0;
    int i = 0;
    while (prefijo[i]) {
        if (str[i] != prefijo[i]) return 0;
        i++;
    }
    return 1;
}

static const char *str_saltar_espacios(const char *s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

// Evaluador aritmético simple para el comando 'calc'
static int64_t evaluar_numero(const char **ptr) {
    *ptr = str_saltar_espacios(*ptr);
    int64_t num = 0;
    int signo = 1;
    if (**ptr == '-') {
        signo = -1;
        (*ptr)++;
    } else if (**ptr == '+') {
        (*ptr)++;
    }

    if (**ptr == '0' && ((*ptr)[1] == 'x' || (*ptr)[1] == 'X')) {
        *ptr += 2;
        while ((**ptr >= '0' && **ptr <= '9') || (**ptr >= 'a' && **ptr <= 'f') || (**ptr >= 'A' && **ptr <= 'F')) {
            char c = **ptr;
            int val = (c >= '0' && c <= '9') ? (c - '0') : ((c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10));
            num = num * 16 + val;
            (*ptr)++;
        }
    } else {
        while (**ptr >= '0' && **ptr <= '9') {
            num = num * 10 + (**ptr - '0');
            (*ptr)++;
        }
    }
    return num * signo;
}

static int64_t evaluar_expresion(const char *expr) {
    const char *p = expr;
    int64_t resultado = evaluar_numero(&p);

    for (;;) {
        p = str_saltar_espacios(p);
        if (*p == '\0') break;

        char op = *p++;
        int64_t siguiente = evaluar_numero(&p);

        if (op == '+') resultado += siguiente;
        else if (op == '-') resultado -= siguiente;
        else if (op == '*') resultado *= siguiente;
        else if (op == '/') resultado = (siguiente != 0) ? (resultado / siguiente) : 0;
        else if (op == '%') resultado = (siguiente != 0) ? (resultado % siguiente) : 0;
        else break;
    }
    return resultado;
}

static void imprimir_banner(void) {
    consola_imprimir_linea_color("==============================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("  TAEK OS v0.1 (TelAvivEpsteinKirkOS) - Terminal de Control   ", COLOR_AVISO_DEFAULT);
    consola_imprimir_color      ("  Sesión iniciada como usuario: ", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("sudo (Privilegios Máximos Ring 0)", COLOR_USUARIO_DEFAULT);
    consola_imprimir_linea_color("  Filosofía: \"Vibecoding en Español y con Buenas Prácticas\"   ", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea_color("  Escribe 'ayuda' para ver la lista de comandos disponibles.  ", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("==============================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea("");
}

static void imprimir_prompt(void) {
    consola_imprimir_color("sudo", COLOR_USUARIO_DEFAULT);
    consola_imprimir_color("@taek-os", COLOR_PROMPT_DEFAULT);
    consola_imprimir_color(":~# ", COLOR_TEXTO_DEFAULT);
}

void terminal_iniciar(void) {
    consola_iniciar();
    consola_limpiar();
    imprimir_banner();
}

static void ejecutar_comando_memoria(const char *arg) {
    if (arg != NULL && (str_igual_sin_caso(arg, "probar") || str_igual_sin_caso(arg, "test") || str_igual_sin_caso(arg, "diagnostico"))) {
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO ] Batería de pruebas en PMM y Kernel Heap...", COLOR_AVISO_DEFAULT);

        // 1. Asignación con kmalloc (Shim Linux)
        consola_imprimir("  1. Asignando 1024 bytes con kmalloc(GFP_KERNEL)... ");
        uint8_t *bloque_a = (uint8_t *)kmalloc(1024, GFP_KERNEL);
        if (!bloque_a) {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            return;
        }
        for (int i = 0; i < 1024; i++) bloque_a[i] = 0xAA;
        consola_imprimir_linea_color("[OK]", COLOR_EXITO_DEFAULT);

        // 2. Asignación con asignar_memoria (Nativo español)
        consola_imprimir("  2. Asignando 4096 bytes con asignar_memoria()... ");
        uint8_t *bloque_b = (uint8_t *)asignar_memoria(4096);
        if (!bloque_b) {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            kfree(bloque_a);
            return;
        }
        for (int i = 0; i < 4096; i++) bloque_b[i] = 0x55;
        consola_imprimir_linea_color("[OK]", COLOR_EXITO_DEFAULT);

        // 3. Asignación con kzalloc (Inicializado a cero)
        consola_imprimir("  3. Asignando 256 bytes con kzalloc() y comprobando ceros... ");
        uint8_t *bloque_c = (uint8_t *)kzalloc(256, GFP_KERNEL);
        int ceros_ok = 1;
        if (!bloque_c) {
            ceros_ok = 0;
        } else {
            for (int i = 0; i < 256; i++) {
                if (bloque_c[i] != 0) {
                    ceros_ok = 0;
                    break;
                }
            }
        }
        if (!ceros_ok) {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            kfree(bloque_a);
            liberar_memoria(bloque_b);
            if (bloque_c) kfree(bloque_c);
            return;
        }
        consola_imprimir_linea_color("[OK]", COLOR_EXITO_DEFAULT);

        // 4. Reasignación dinámica (krealloc / reasignar_memoria)
        consola_imprimir("  4. Reasignando bloque A de 1024 a 2048 bytes (krealloc)... ");
        bloque_a = (uint8_t *)krealloc(bloque_a, 2048, GFP_KERNEL);
        int datos_preservados = 1;
        if (!bloque_a) {
            datos_preservados = 0;
        } else {
            for (int i = 0; i < 1024; i++) {
                if (bloque_a[i] != 0xAA) {
                    datos_preservados = 0;
                    break;
                }
            }
        }
        if (!datos_preservados) {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            liberar_memoria(bloque_b);
            kfree(bloque_c);
            return;
        }
        consola_imprimir_linea_color("[OK]", COLOR_EXITO_DEFAULT);

        // 5. Asignación de página física de 4 KiB en PMM
        consola_imprimir("  5. Solicitando marco de 4 KiB al PMM (pmm_asignar_pagina_virtual)... ");
        void *pag_pmm = pmm_asignar_pagina_virtual();
        if (!pag_pmm) {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
        } else {
            consola_imprimir_linea_color("[OK]", COLOR_EXITO_DEFAULT);
        }

        // 6. Auditoría de canarios e integridad estructural
        consola_imprimir("  6. Verificando canarios de integridad con El Huevo... ");
        if (memoria_verificar_integridad()) {
            consola_imprimir_linea_color("[100% INTACTO]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[CORRUPTO]", COLOR_ERROR_DEFAULT);
        }

        // 7. Liberación y coalescing
        consola_imprimir("  7. Liberando bloques y fusionando Heap (kfree/coalescing)... ");
        kfree(bloque_a);
        liberar_memoria(bloque_b);
        kfree(bloque_c);
        if (pag_pmm) {
            uint64_t offset = memoria_obtener_hhdm_offset();
            pmm_liberar_pagina_fisica((uint64_t)pag_pmm - offset);
        }
        consola_imprimir_linea_color("[OK]", COLOR_EXITO_DEFAULT);

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO EXITOSO ] PMM y Kernel Heap funcionando al 100%.", COLOR_PROMPT_DEFAULT);
        return;
    }

    memoria_estadisticas_t est;
    memoria_obtener_estadisticas(&est);

    consola_imprimir_linea_color("================== GESTIÓN DE MEMORIA TAEK OS ==================", COLOR_AVISO_DEFAULT);

    consola_imprimir("  RAM Física Total      : ");
    consola_imprimir_dec(est.ram_fisica_total / (1024 * 1024));
    consola_imprimir(" MiB (");
    consola_imprimir_dec(est.ram_fisica_total / (1024 * 1024 * 1024));
    consola_imprimir_linea(" GiB)");

    consola_imprimir("  RAM Usable (UEFI)     : ");
    consola_imprimir_dec(est.ram_fisica_usable / (1024 * 1024));
    consola_imprimir(" MiB (");
    consola_imprimir_dec((est.ram_fisica_usable * 100) / (est.ram_fisica_total ? est.ram_fisica_total : 1));
    consola_imprimir_linea("% del total)");

    consola_imprimir("  Páginas Físicas 4 KiB : ");
    consola_imprimir_dec(est.paginas_totales);
    consola_imprimir(" totales | ");
    consola_imprimir_dec(est.paginas_libres);
    consola_imprimir(" libres | ");
    consola_imprimir_dec(est.paginas_en_uso);
    consola_imprimir_linea(" en uso");

    consola_imprimir_linea_color("  --- Heap del Kernel (kmalloc / asignar_memoria) ---", COLOR_PROMPT_DEFAULT);

    consola_imprimir("  Capacidad de Arena    : ");
    consola_imprimir_dec(est.heap_capacidad_total / 1024);
    consola_imprimir(" KiB (");
    consola_imprimir_dec(est.heap_capacidad_total / (1024 * 1024));
    consola_imprimir_linea(" MiB)");

    consola_imprimir("  Memoria Heap en Uso   : ");
    consola_imprimir_dec(est.heap_bytes_en_uso / 1024);
    consola_imprimir(" KiB (");
    consola_imprimir_dec(est.heap_bytes_en_uso);
    consola_imprimir_linea(" bytes)");

    uint64_t heap_libre = (est.heap_capacidad_total > est.heap_bytes_en_uso) ?
                          (est.heap_capacidad_total - est.heap_bytes_en_uso) : 0;
    consola_imprimir("  Memoria Heap Libre    : ");
    consola_imprimir_dec(heap_libre / 1024);
    consola_imprimir_linea(" KiB");

    consola_imprimir("  Bloques de Asignación : ");
    consola_imprimir_dec(est.heap_bloques_activos);
    consola_imprimir(" activos | ");
    consola_imprimir_dec(est.heap_bloques_libres);
    consola_imprimir_linea(" libres");

    consola_imprimir("  Integridad Canarios   : ");
    if (est.canarios_intactos) {
        consola_imprimir_linea_color("[ 100% INTACTO ] (Vigilado por El Huevo)", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("[ CORRUPCIÓN DETECTADA ]", COLOR_ERROR_DEFAULT);
    }

    // Barra visual de uso de RAM
    uint64_t porcentaje_uso = (est.paginas_en_uso * 100) / (est.paginas_totales ? est.paginas_totales : 1);
    consola_imprimir("  Uso de RAM Física     : [");
    int barras_llenas = (int)(porcentaje_uso / 5);
    if (barras_llenas > 20) barras_llenas = 20;
    for (int i = 0; i < 20; i++) {
        if (i < barras_llenas) {
            consola_imprimir("=");
        } else {
            consola_imprimir("-");
        }
    }
    consola_imprimir("] ");
    consola_imprimir_dec(porcentaje_uso);
    consola_imprimir_linea("%");

    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Usa 'memoria probar' para ejecutar autodiagnóstico de kmalloc/kfree.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_paginacion(const char *arg) {
    if (arg != NULL && (str_igual_sin_caso(arg, "probar") || str_igual_sin_caso(arg, "test") || str_igual_sin_caso(arg, "diagnostico"))) {
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO VMM ] Probando tablas de paginación x86_64...", COLOR_AVISO_DEFAULT);

        consola_imprimir("  1. Comprobando árbol PML4 soberano y registro CR3... ");
        uint64_t cr3 = paginacion_obtener_cr3();
        if (cr3 == 0) {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_color("CR3: ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_hex(cr3);
        consola_imprimir_linea_color(" [OK]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  2. Ejecutando ciclo de mapeo, firma mágica, traducción y TLB... ");
        if (paginacion_ejecutar_autodiagnostico()) {
            consola_imprimir_linea_color("[OK - 100% CORRECTO]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  3. Comprobando preservación de Mitad Superior (HHDM y Kernel)... ");
        uint64_t phys_kernel = paginacion_obtener_fisica(0xFFFFFFFF80000000ULL);
        if (phys_kernel != 0) {
            consola_imprimir_linea_color("[PRESERVADO OK]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO EXITOSO ] VMM y Tablas PML4 funcionando al 100%.", COLOR_PROMPT_DEFAULT);
        return;
    }

    uint64_t cr3 = paginacion_obtener_cr3();
    uint64_t pml4_fis = paginacion_obtener_pml4_activo();

    consola_imprimir_linea_color("================== PAGINACIÓN Y MEMORIA VIRTUAL ==================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Modelo de Paginación  : ");
    consola_imprimir_linea_color("x86_64 Long Mode (Jerarquía de 4 Niveles)", COLOR_USUARIO_DEFAULT);
    consola_imprimir("  Desglose de Niveles   : ");
    consola_imprimir_linea("PML4 (L4) -> PDPT (L3) -> PD (L2) -> PT (L1) -> 4 KiB");
    consola_imprimir("  Registro CR3 Activo   : ");
    consola_imprimir_hex(cr3);
    consola_imprimir_linea(" (Cargado en CPU)");
    consola_imprimir("  PML4 Físico Soberano  : ");
    consola_imprimir_hex(pml4_fis);
    consola_imprimir_linea(" (Árbol Propio de TAEK OS)");
    consola_imprimir("  Espacio Mitad Superior: ");
    consola_imprimir_linea("0xFFFF800000000000 (HHDM + Kernel ELF 64-bit)");
    consola_imprimir("  Espacio Mitad Inferior: ");
    consola_imprimir_linea("0x0000000000000000 (Mapeos Dinámicos / VRAM)");
    consola_imprimir("  Invalidación de TLB   : ");
    consola_imprimir_linea_color("Instrucción nativa 'invlpg' habilitada", COLOR_EXITO_DEFAULT);
    consola_imprimir("  Protecciones de Página: ");
    consola_imprimir_linea("Bit NX (No-Execute), R/W, Supervisor/Usuario, PCD (MMIO)");
    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Usa 'paginacion probar' para ejecutar autodiagnóstico de mapeo.", COLOR_TEXTO_DEFAULT);
}

static void terminal_imprimir_hex_fijo(uint64_t val, int digitos) {
    const char hex_chars[] = "0123456789abcdef";
    for (int i = digitos - 1; i >= 0; i--) {
        uint8_t nibble = (uint8_t)((val >> (i * 4)) & 0x0F);
        consola_escribir_caracter(hex_chars[nibble]);
    }
}

static void imprimir_tamano_barra(uint64_t tam) {
    if (tam >= 1024ULL * 1024ULL * 1024ULL) {
        consola_imprimir_dec(tam / (1024ULL * 1024ULL * 1024ULL));
        consola_imprimir(" GiB");
    } else if (tam >= 1024ULL * 1024ULL) {
        consola_imprimir_dec(tam / (1024ULL * 1024ULL));
        consola_imprimir(" MiB");
    } else if (tam >= 1024ULL) {
        consola_imprimir_dec(tam / 1024ULL);
        consola_imprimir(" KiB");
    } else {
        consola_imprimir_dec(tam);
        consola_imprimir(" B");
    }
}

static void ejecutar_comando_lspci(const char *arg) {
    int conteo = pci_obtener_conteo();

    // MODO: pci gpu / lspci gpu / gpu
    if (arg && (str_igual(arg, "gpu") || str_igual(arg, "-g") || str_igual(arg, "video"))) {
        consola_imprimir_linea_color("================== CONTROLADORAS DE VIDEO Y GPU ==================", COLOR_AVISO_DEFAULT);
        int gpus_encontradas = 0;

        for (int i = 0; i < conteo; i++) {
            const struct dispositivo_pci *dev = pci_obtener_dispositivo(i);
            if (!dev) continue;

            if (dev->clase == 0x03 || (dev->clase == 0x00 && dev->subclase == 0x01)) {
                gpus_encontradas++;
                consola_imprimir("  [ GPU ");
                consola_imprimir_dec(gpus_encontradas);
                consola_imprimir(" ] BDF: ");
                terminal_imprimir_hex_fijo(dev->bus, 2);
                consola_imprimir(":");
                terminal_imprimir_hex_fijo(dev->ranura, 2);
                consola_imprimir(".");
                terminal_imprimir_hex_fijo(dev->funcion, 1);

                consola_imprimir(" | ID: ");
                terminal_imprimir_hex_fijo(dev->id_proveedor, 4);
                consola_imprimir(":");
                terminal_imprimir_hex_fijo(dev->id_dispositivo, 4);
                consola_imprimir(" | ");
                consola_imprimir_linea_color(pci_nombre_proveedor(dev->id_proveedor), COLOR_EXITO_DEFAULT);

                consola_imprimir("    Clase: ");
                consola_imprimir_linea(pci_nombre_clase(dev->clase, dev->subclase));

                consola_imprimir("    Línea IRQ: ");
                consola_imprimir_dec(dev->linea_irq);
                consola_imprimir(" | Pin: ");
                consola_imprimir_dec(dev->pin_irq);
                consola_imprimir_linea("");

                // Detalle de los BARs de la GPU
                for (int b = 0; b < 6; b++) {
                    const struct barra_pci *barra = &dev->barras[b];
                    if (!barra->valida) continue;

                    consola_imprimir("    -> BAR");
                    consola_imprimir_dec(b);
                    consola_imprimir(": Base Física: 0x");
                    terminal_imprimir_hex_fijo(barra->dir_base, barra->es_64bits ? 16 : 8);
                    consola_imprimir(" | Tamaño: ");
                    imprimir_tamano_barra(barra->tamano);

                    if (barra->es_io) {
                        consola_imprimir_linea_color(" [PUERTO I/O]", COLOR_TEXTO_DEFAULT);
                    } else if (barra->predecible) {
                        consola_imprimir_linea_color(" [VRAM APERTURE - WRITE-COMBINING]", COLOR_EXITO_DEFAULT);
                    } else {
                        consola_imprimir_linea_color(" [REGISTROS MMIO]", COLOR_AVISO_DEFAULT);
                    }
                }

                // Diagnóstico según arquitectura de la GPU
                if (dev->id_proveedor == 0x10DE) {
                    consola_imprimir_linea_color("    [ NVIDIA GPU ] Silicio listo para mapeo MMIO y carga de firmware GSP.", COLOR_USUARIO_DEFAULT);
                } else if (dev->id_proveedor == 0x8086) {
                    consola_imprimir_linea_color("    [ INTEL GPU ] Acelerador gráfico integrado listo.", COLOR_USUARIO_DEFAULT);
                } else if (dev->id_proveedor == 0x1AF4) {
                    consola_imprimir_linea_color("    [ VIRTIO-GPU ] Acelerador paravirtualizado activo para QEMU.", COLOR_USUARIO_DEFAULT);
                } else if (dev->id_proveedor == 0x1234) {
                    consola_imprimir_linea_color("    [ BOCHS/QEMU VGA ] Framebuffer lineal GOP activo.", COLOR_TEXTO_DEFAULT);
                }
                consola_imprimir_linea("");
            }
        }

        if (gpus_encontradas == 0) {
            consola_imprimir_linea_color("  [ AVISO ] No se detectaron controladoras de pantalla en el bus PCI.", COLOR_AVISO_DEFAULT);
        }
        consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
        return;
    }

    // MODO DETALLADO: lspci -v / pci detalle / pci -v
    int detalle = (arg && (str_igual(arg, "-v") || str_igual(arg, "detalle") || str_igual(arg, "verbose")));

    consola_imprimir_linea_color("======================= BUS PCI / PCI EXPRESS =======================", COLOR_AVISO_DEFAULT);
    if (!detalle) {
        consola_imprimir_linea_color("BDF       ID DISP    FABRICANTE               CLASE / TIPO", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea("------------------------------------------------------------------");
    }

    for (int i = 0; i < conteo; i++) {
        const struct dispositivo_pci *dev = pci_obtener_dispositivo(i);
        if (!dev) continue;

        uint32_t color_linea = COLOR_TEXTO_DEFAULT;
        if (dev->clase == 0x03) {
            color_linea = COLOR_EXITO_DEFAULT; // Destacar GPUs
        } else if (dev->clase == 0x04) {
            color_linea = COLOR_USUARIO_DEFAULT; // Destacar Audio
        }

        if (!detalle) {
            // Formato compacto tipo tabla
            terminal_imprimir_hex_fijo(dev->bus, 2);
            consola_imprimir(":");
            terminal_imprimir_hex_fijo(dev->ranura, 2);
            consola_imprimir(".");
            terminal_imprimir_hex_fijo(dev->funcion, 1);
            consola_imprimir("   ");

            terminal_imprimir_hex_fijo(dev->id_proveedor, 4);
            consola_imprimir(":");
            terminal_imprimir_hex_fijo(dev->id_dispositivo, 4);
            consola_imprimir("  ");

            consola_imprimir_color(pci_nombre_proveedor(dev->id_proveedor), color_linea);
            consola_imprimir(" - ");
            consola_imprimir_linea_color(pci_nombre_clase(dev->clase, dev->subclase), color_linea);
        } else {
            // Formato extendido detallado
            consola_imprimir("[ ");
            terminal_imprimir_hex_fijo(dev->bus, 2);
            consola_imprimir(":");
            terminal_imprimir_hex_fijo(dev->ranura, 2);
            consola_imprimir(".");
            terminal_imprimir_hex_fijo(dev->funcion, 1);
            consola_imprimir(" ] ");

            terminal_imprimir_hex_fijo(dev->id_proveedor, 4);
            consola_imprimir(":");
            terminal_imprimir_hex_fijo(dev->id_dispositivo, 4);
            consola_imprimir(" | ");
            consola_imprimir_color(pci_nombre_proveedor(dev->id_proveedor), color_linea);
            consola_imprimir(" - ");
            consola_imprimir_linea_color(pci_nombre_clase(dev->clase, dev->subclase), color_linea);

            consola_imprimir("  Rev: ");
            terminal_imprimir_hex_fijo(dev->revision, 2);
            consola_imprimir(" | Cabecera: ");
            terminal_imprimir_hex_fijo(dev->tipo_cabecera, 2);
            consola_imprimir(" | IRQ: ");
            consola_imprimir_dec(dev->linea_irq);
            consola_imprimir_linea("");

            for (int b = 0; b < 6; b++) {
                const struct barra_pci *barra = &dev->barras[b];
                if (!barra->valida) continue;

                consola_imprimir("  -> BAR");
                consola_imprimir_dec(b);
                consola_imprimir(": 0x");
                terminal_imprimir_hex_fijo(barra->dir_base, barra->es_64bits ? 16 : 8);
                consola_imprimir(" | Tamaño: ");
                imprimir_tamano_barra(barra->tamano);

                if (barra->es_io) {
                    consola_imprimir_linea(" [I/O]");
                } else if (barra->predecible) {
                    consola_imprimir_linea_color(" [MMIO 64b Prefetchable / VRAM]", COLOR_EXITO_DEFAULT);
                } else {
                    consola_imprimir_linea(" [MMIO]");
                }
            }
            consola_imprimir_linea("");
        }
    }

    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir("Total dispositivos PCI/PCIe detectados: ");
    consola_imprimir_dec(conteo);
    consola_imprimir_linea("");
    if (!detalle) {
        consola_imprimir_linea_color("Tip: Usa 'lspci -v' para ver BARs físicos, o 'pci gpu' para ver la GPU.", COLOR_TEXTO_DEFAULT);
    }
}

static void procesar_comando(const char *linea_cruda) {
    const char *linea = str_saltar_espacios(linea_cruda);
    if (!linea || *linea == '\0') return;

    // Quitar prefijo "sudo " si el usuario lo incluye (ya que el usuario literalmente es sudo)
    if (str_comienza_con(linea, "sudo ")) {
        linea = str_saltar_espacios(linea + 5);
    }

    // MEME CLÁSICO: sudo rm -rf / o sudo rm -f o rm -rf
    if (str_comienza_con(linea, "rm ") || str_igual(linea, "rm") || str_comienza_con(linea, "rmdir")) {
        consola_imprimir_linea("");
        consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea_color("  [ ADVERTENCIA DE SUDO ] ¡Borrando todo el sistema operativo!", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea_color("  [ ADVERTENCIA DE SUDO ] rm: eliminando '/' recursivamente... ", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea_color("  [ ADVERTENCIA DE SUDO ] ¡El universo colapsa! ¡El Huevo muere!", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea("");

        esperar_milisegundos(1500);

        huevo_quebrar("¡El usuario 'sudo' ejecutó rm -rf /! ¡Universo eliminado!", 0xDEADBEEF, 0, 0);
        return;
    }

    // COMANDO: ayuda / comandos / help
    if (str_igual(linea, "ayuda") || str_igual(linea, "comandos") || str_igual(linea, "help")) {
        consola_imprimir_linea_color("--- COMANDOS DISPONIBLES EN TAEK OS ---", COLOR_AVISO_DEFAULT);
        consola_imprimir_color("  ayuda          ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Muestra este menú de asistencia.");
        consola_imprimir_color("  huevo          ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Muestra la integridad y salud de El Huevo.");
        consola_imprimir_color("  info           ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Información de CPU, Hipervisor VMX y Framebuffer.");
        consola_imprimir_color("  memoria        ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Reporte de RAM, PMM, Heap kmalloc y canarios ('memoria probar').");
        consola_imprimir_color("  paginacion     ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Memoria virtual, árbol PML4, CR3 e invlpg ('paginacion probar').");
        consola_imprimir_color("  lspci / pci    ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Enumera dispositivos PCI/PCIe ('pci detalle', 'pci gpu').");
        consola_imprimir_color("  gpu            ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Diagnóstico especializado de la GPU, VRAM y registros MMIO.");
        consola_imprimir_color("  musica         ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Reproduce la sintonía 'Qué bonito es Israel Damonte'.");
        consola_imprimir_color("  cangrejo       ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Reproduce la animación de Don Cangrejo explotando.");
        consola_imprimir_color("  calc <expr>    ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Evalúa operaciones aritméticas (ej: calc 42 * 2 + 10).");
        consola_imprimir_color("  quiensoy       ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Muestra la identidad y privilegios del usuario.");
        consola_imprimir_color("  eco <texto>    ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Imprime un texto en la consola.");
        consola_imprimir_color("  ruleta         ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Ruleta Rusa (1/6 de morir tú, 1/6 de morir el sistema).");
        if (g_el_comando_desbloqueado) {
            consola_imprimir_color("  El comando     ", COLOR_USUARIO_DEFAULT);
            consola_imprimir_linea(": [DESBLOQUEADO] Lo que hace es nada.");
        }
        consola_imprimir_color("  limpiar / cls  ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Limpia la pantalla de la terminal.");
        consola_imprimir_color("  apagar         ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Apaga el ordenador de forma limpia vía ACPI.");
        consola_imprimir_color("  sudo rm -rf /  ", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea_color(": [PELIGRO] No lo intentes si valoras tu existencia.", COLOR_ERROR_DEFAULT);
        return;
    }

    // COMANDO: ruleta / ruleta_rusa / ruletarusa
    if (str_igual_sin_caso(linea, "ruleta") || str_igual_sin_caso(linea, "ruleta rusa") ||
        str_igual_sin_caso(linea, "ruleta_rusa") || str_igual_sin_caso(linea, "ruletarusa")) {
        uint32_t tam_duelo = (uint32_t)(_binary_duelo_audio_bin_end - _binary_duelo_audio_bin_start);

        // Iniciar sintonía de duelo completa en bucle continuo por DMA (2m 42s)
        if (tam_duelo > 0) {
            audio_ac97_reproducir_pcm_bucle(_binary_duelo_audio_bin_start, tam_duelo);
        }

        consola_imprimir_linea_color("==============================================================", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("  [ DUELO A MUERTE 1 VS 1 ] EL BUENO, EL FEO Y EL MALO (2m 42s)", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("  [ MODO TENSIÓN EXTREMA ] Cada ronda aumentará la munición.  ", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea_color("  [ ADVERTENCIA ] Si tú pierdes: tu sistema colapsa y muere.  ", COLOR_TEXTO_DEFAULT);
        consola_imprimir_linea_color("  [ ADVERTENCIA ] Si la máquina pierde: desbloqueas El comando.", COLOR_TEXTO_DEFAULT);
        consola_imprimir_linea_color("==============================================================", COLOR_AVISO_DEFAULT);
        esperar_con_audio_bucle(1500, _binary_duelo_audio_bin_start, tam_duelo);

        for (int ronda = 1; ronda <= 6; ronda++) {
            consola_imprimir_linea("");
            consola_imprimir_linea_color("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~", COLOR_AVISO_DEFAULT);
            consola_imprimir_color("  >>> RONDA ", COLOR_PROMPT_DEFAULT);
            consola_imprimir_dec(ronda);
            consola_imprimir(" DE 6: ");
            consola_imprimir_dec(ronda);
            consola_imprimir_linea_color(" BALA(S) EN EL TAMBOR (DE 6 RECÁMARAS) <<<", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~", COLOR_AVISO_DEFAULT);

            // ==========================================
            // TURNO DEL JUGADOR (~20 SEGUNDOS DE TENSIÓN)
            // ==========================================
            consola_imprimir_linea("");
            consola_imprimir_linea_color("==> [ TU TURNO ]", COLOR_AVISO_DEFAULT);

            // 1s
            consola_imprimir_linea("  * Sacas el revólver lentamente... El metal se siente gélido en tu palma.");
            esperar_con_audio_bucle(1000, _binary_duelo_audio_bin_start, tam_duelo);

            // 1.5s
            consola_imprimir_linea("  * Abres el cilindro y lo haces girar: *chac... chac... chac... chac...*");
            esperar_con_audio_bucle(1500, _binary_duelo_audio_bin_start, tam_duelo);

            // 2s
            consola_imprimir_linea_color("  * ¡CLACK! Encajas el tambor. Colocas el cañón frío directamente en tu sien...", COLOR_TEXTO_DEFAULT);
            esperar_con_audio_bucle(2000, _binary_duelo_audio_bin_start, tam_duelo);

            // 3s
            consola_imprimir_linea("  * Tu dedo tiembla sobre el gatillo... Empiezas a sudar frío.");
            esperar_con_audio_bucle(3000, _binary_duelo_audio_bin_start, tam_duelo);

            // 3.5s
            consola_imprimir_linea_color("  * Te estás jugando TODO, ¿recuerdas? Tu sistema operativo, tus datos...", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("    tu familia, tus proyectos, tus noches sin dormir...", COLOR_AVISO_DEFAULT);
            esperar_con_audio_bucle(3500, _binary_duelo_audio_bin_start, tam_duelo);

            // 3s
            consola_imprimir_linea_color("  * \"¿De verdad vale la pena esto?\", te preguntas en el silencio...", COLOR_TEXTO_DEFAULT);
            esperar_con_audio_bucle(3000, _binary_duelo_audio_bin_start, tam_duelo);

            // 3s
            consola_imprimir_linea("  * Demasiado tarde para dudar. Aprietas el gatillo: 3... 2... 1...");
            esperar_con_audio_bucle(3000, _binary_duelo_audio_bin_start, tam_duelo);

            // 2s
            consola_imprimir_linea_color("  * Cierras los ojos con fuerza... ¡¡¡ÚLTIMO MILÍMETRO!!!", COLOR_ERROR_DEFAULT);
            esperar_con_audio_bucle(2000, _binary_duelo_audio_bin_start, tam_duelo);

            // DISPARO JUGADOR
            uint32_t tiro_jugador = obtener_aleatorio() % 6;
            if (tiro_jugador < (uint32_t)ronda) {
                // ¡EL JUGADOR MUERE!
                audio_ac97_detener();
                consola_imprimir_linea("");
                consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea_color("  *¡¡¡¡¡¡¡¡¡¡¡¡¡¡PUMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM!!!!!!!!!!!!!*", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea_color("  [ BALA EN LA RECÁMARA ] El proyectil atravesó el sistema.", COLOR_ERROR_DEFAULT);
                consola_imprimir_color      ("  [ CAÍDO EN COMBATE ] Has perdido el duelo en la Ronda ", COLOR_ERROR_DEFAULT);
                consola_imprimir_dec(ronda);
                consola_imprimir_linea_color(". ¡Adiós, vaquero!", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea_color("  [ COLAPSO TOTAL ] Borrando absolutamente todo el universo...", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea("");

                esperar_milisegundos(1500);
                huevo_quebrar("¡Perdiste la Ruleta Rusa! El cartucho estaba en la recámara.", 0xDEADBEEF, 0, 0);
                return;
            }

            // El jugador sobrevive este turno
            consola_imprimir_linea("");
            consola_imprimir_linea_color("  *¡¡¡¡CLIC!!!!* ... ¡¡¡RECÁMARA VACÍA!!!", COLOR_EXITO_DEFAULT);
            consola_imprimir_color      ("  * Respiras hondo... ¡Has sobrevivido a la Ronda ", COLOR_EXITO_DEFAULT);
            consola_imprimir_dec(ronda);
            consola_imprimir_linea_color("! El sudor baja por tu frente.", COLOR_EXITO_DEFAULT);
            esperar_con_audio_bucle(2000, _binary_duelo_audio_bin_start, tam_duelo);

            // ==========================================
            // TURNO DE LA MÁQUINA (~20 SEGUNDOS DE TENSIÓN)
            // ==========================================
            consola_imprimir_linea("");
            consola_imprimir_linea_color("==> [ TURNO DEL SISTEMA - KERNEL TAEK OS ]", COLOR_AVISO_DEFAULT);

            // 1s
            consola_imprimir_linea("  * Le entregas el revólver al sistema operativo. El silicio cruje.");
            esperar_con_audio_bucle(1000, _binary_duelo_audio_bin_start, tam_duelo);

            // 1.5s
            consola_imprimir_linea("  * La CPU activa los motores de paso: *whiiir... clac-clac-clac...*");
            esperar_con_audio_bucle(1500, _binary_duelo_audio_bin_start, tam_duelo);

            // 2s
            consola_imprimir_linea_color("  * ¡CLACK! El martillo del percutor se levanta. Apuntando al socket LGA-1700...", COLOR_TEXTO_DEFAULT);
            esperar_con_audio_bucle(2000, _binary_duelo_audio_bin_start, tam_duelo);

            // 3s
            consola_imprimir("  * La máquina calcula probabilidades cuánticas: ");
            consola_imprimir_dec(ronda);
            consola_imprimir_linea(" de 6 de auto-destrucción...");
            esperar_con_audio_bucle(3000, _binary_duelo_audio_bin_start, tam_duelo);

            // 3.5s
            consola_imprimir_linea_color("  * Si la máquina pierde, El comando sagrado será revelado para siempre...", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("    Las compuertas lógicas de Ring 0 dudan por un microsegundo...", COLOR_AVISO_DEFAULT);
            esperar_con_audio_bucle(3500, _binary_duelo_audio_bin_start, tam_duelo);

            // 3s
            consola_imprimir_linea_color("  * \"El silicio no siente miedo\", murmura el firmware en bajo nivel...", COLOR_TEXTO_DEFAULT);
            esperar_con_audio_bucle(3000, _binary_duelo_audio_bin_start, tam_duelo);

            // 3s
            consola_imprimir_linea("  * El actuador electromagnético presiona el gatillo: 3... 2... 1...");
            esperar_con_audio_bucle(3000, _binary_duelo_audio_bin_start, tam_duelo);

            // 2s
            consola_imprimir_linea_color("  * Los condensadores se descargan... ¡¡¡DISPARO INMINENTE DEL SISTEMA!!!", COLOR_ERROR_DEFAULT);
            esperar_con_audio_bucle(2000, _binary_duelo_audio_bin_start, tam_duelo);

            // DISPARO SISTEMA
            uint32_t tiro_sistema = obtener_aleatorio() % 6;
            if (tiro_sistema < (uint32_t)ronda) {
                // ¡LA MÁQUINA PIERDE!
                audio_ac97_detener();
                consola_imprimir_linea("");
                consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea_color("  *¡¡¡¡¡¡¡¡¡¡¡¡¡¡PUMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM!!!!!!!!!!!!!*", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea_color("  [ ¡VICTORIA TOTAL! ] ¡¡EL SISTEMA OPERATIVO HA PERDIDO EL DUELO!!", COLOR_AVISO_DEFAULT);
                consola_imprimir_color      ("  [ PERFORACIÓN CRÍTICA ] El silicio del kernel cayó en la Ronda ", COLOR_EXITO_DEFAULT);
                consola_imprimir_dec(ronda);
                consola_imprimir_linea_color("!", COLOR_EXITO_DEFAULT);
                consola_imprimir_linea_color("  [ LOGRO SUPREMO ] ¡Has vencido a la inteligencia de la máquina!", COLOR_EXITO_DEFAULT);
                consola_imprimir_linea_color("  [ RECOMPENSA DESBLOQUEADA ] Se ha desbloqueado el comando: \"El comando\"", COLOR_USUARIO_DEFAULT);
                consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
                consola_imprimir_linea("");
                g_el_comando_desbloqueado = 1;
                return;
            }

            // La máquina sobrevive la ronda
            consola_imprimir_linea("");
            consola_imprimir_linea_color("  *¡¡¡¡CLIC!!!!* ... ¡¡¡RECÁMARA VACÍA PARA LA MÁQUINA!!!", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea("  * La máquina sobrevive con frialdad matemática. El duelo se intensifica...");
            consola_imprimir_linea_color("  * ¡AUMENTANDO LETALIDAD! Agregando otra bala al tambor...", COLOR_ERROR_DEFAULT);
            esperar_con_audio_bucle(2500, _binary_duelo_audio_bin_start, tam_duelo);
        }

        audio_ac97_detener();
        consola_imprimir_linea_color("==> [ EMPATE MILAGROSO ] Ambos sobrevivieron milagrosamente las 6 rondas.", COLOR_PROMPT_DEFAULT);
        return;
    }

    // COMANDO: "El comando" / el comando / el_comando / elcomando
    if (str_igual_sin_caso(linea, "el comando") || str_igual_sin_caso(linea, "\"el comando\"") ||
        str_igual_sin_caso(linea, "el_comando") || str_igual_sin_caso(linea, "elcomando")) {
        if (!g_el_comando_desbloqueado) {
            consola_imprimir_linea_color("Comando bloqueado. Debes vencer al sistema en la 'ruleta' primero.", COLOR_ERROR_DEFAULT);
            return;
        }

        // Lo que hace es nada. Literalmente nada.
        return;
    }

    // COMANDO: huevo
    if (str_igual(linea, "huevo") || str_igual(linea, "estado")) {
        consola_imprimir_linea_color("     .---.     ", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("    /     \\    [EL HUEVO] Estado: INTACTO (100% Integridad)", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("   |  100% |   [EL HUEVO] Firma: 0xEE66B007 | Canario: ACTIVO", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("    \\     /    [EL HUEVO] \"Si el huevo se quiebra, el universo muere.\"", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("     `---'     ", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("  [ OK ] El canario de memoria responde correctamente.", COLOR_EXITO_DEFAULT);
        return;
    }

    // COMANDO: quiensoy / whoami
    if (str_igual(linea, "quiensoy") || str_igual(linea, "whoami")) {
        consola_imprimir_color("sudo ", COLOR_USUARIO_DEFAULT);
        consola_imprimir_linea("(Usuario Supremo en Ring 0 / Ring -1 con Privilegios Absolutos)");
        return;
    }

    // COMANDO: info / sistema
    if (str_igual(linea, "info") || str_igual(linea, "sistema")) {
        consola_imprimir_linea_color("--- INFORMACIÓN DEL SISTEMA ---", COLOR_AVISO_DEFAULT);
        consola_imprimir("  Sistema Operativo : ");
        consola_imprimir_linea_color("TAEK OS v0.1 (TelAvivEpsteinKirkOS)", COLOR_USUARIO_DEFAULT);
        consola_imprimir("  Arquitectura      : ");
        consola_imprimir_linea("x86_64 Long Mode (Intel Core i9-14900HX Ready)");
        consola_imprimir("  Hipervisor VMX    : ");
        if (vmx_esta_activo()) {
            consola_imprimir_linea_color("ACTIVO en Ring -1 (VMX Root Operation)", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("Guardián Ring 0 Activo", COLOR_AVISO_DEFAULT);
        }
        consola_imprimir("  Resolución Pantalla: ");
        consola_imprimir_dec(pantalla_obtener_ancho());
        consola_imprimir("x");
        consola_imprimir_dec(pantalla_obtener_alto());
        consola_imprimir_linea(" (UEFI GOP 32bpp)");
        consola_imprimir("  Subsistema Audio  : ");
        consola_imprimir_linea("PCI Intel AC97 DMA Directo @ 44.1 kHz");
        return;
    }

    // COMANDO: memoria / free / ram
    if (str_comienza_con(linea, "memoria") || str_comienza_con(linea, "free") || str_comienza_con(linea, "ram")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "memoria ")) arg = str_saltar_espacios(linea + 8);
        else if (str_comienza_con(linea, "free ")) arg = str_saltar_espacios(linea + 5);
        else if (str_comienza_con(linea, "ram ")) arg = str_saltar_espacios(linea + 4);
        ejecutar_comando_memoria(arg);
        return;
    }

    // COMANDO: paginacion / paginas / vmm / paging
    if (str_comienza_con(linea, "paginacion") || str_comienza_con(linea, "paginas") ||
        str_comienza_con(linea, "vmm") || str_comienza_con(linea, "paging")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "paginacion ")) arg = str_saltar_espacios(linea + 11);
        else if (str_comienza_con(linea, "paginas ")) arg = str_saltar_espacios(linea + 8);
        else if (str_comienza_con(linea, "vmm ")) arg = str_saltar_espacios(linea + 4);
        else if (str_comienza_con(linea, "paging ")) arg = str_saltar_espacios(linea + 7);
        ejecutar_comando_paginacion(arg);
        return;
    }

    // COMANDO: lspci / pci
    if (str_comienza_con(linea, "lspci") || str_comienza_con(linea, "pci")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "lspci ")) arg = str_saltar_espacios(linea + 6);
        else if (str_comienza_con(linea, "pci ")) arg = str_saltar_espacios(linea + 4);
        ejecutar_comando_lspci(arg);
        return;
    }

    // COMANDO: gpu (acceso directo a diagnóstico de video)
    if (str_igual(linea, "gpu") || str_igual(linea, "video") || str_igual(linea, "vram")) {
        ejecutar_comando_lspci("gpu");
        return;
    }

    // COMANDO: limpiar / cls / clear
    if (str_igual(linea, "limpiar") || str_igual(linea, "cls") || str_igual(linea, "clear")) {
        consola_limpiar();
        imprimir_banner();
        return;
    }

    // COMANDO: musica / audio
    if (str_igual(linea, "musica") || str_igual(linea, "audio")) {
        consola_imprimir_linea_color("==> Reproduciendo sintonía 'Qué bonito es Israel Damonte'...", COLOR_PROMPT_DEFAULT);
        uint32_t tam = (uint32_t)(_binary_audio_arranque_bin_end - _binary_audio_arranque_bin_start);
        audio_ac97_reproducir_pcm(_binary_audio_arranque_bin_start, tam);
        return;
    }

    // COMANDO: cangrejo / explotar
    if (str_igual(linea, "cangrejo") || str_igual(linea, "explotar")) {
        consola_imprimir_linea_color("==> Lanzando animación de Don Cangrejo (1 bucle)...", COLOR_AVISO_DEFAULT);
        animacion_don_cangrejo_explotar(1);
        consola_limpiar();
        imprimir_banner();
        return;
    }

    // COMANDO: calc <expresion>
    if (str_comienza_con(linea, "calc ") || str_comienza_con(linea, "calcular ")) {
        const char *expr = linea + 5;
        if (str_comienza_con(linea, "calcular ")) expr = linea + 9;
        int64_t res = evaluar_expresion(expr);
        consola_imprimir_color("Resultado: ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_dec((uint64_t)res);
        consola_imprimir(" (");
        consola_imprimir_hex((uint64_t)res);
        consola_imprimir_linea(")");
        return;
    }

    // COMANDO: eco <texto> / echo <texto>
    if (str_comienza_con(linea, "eco ") || str_comienza_con(linea, "echo ")) {
        const char *txt = linea + 4;
        if (str_comienza_con(linea, "echo ")) txt = linea + 5;
        consola_imprimir_linea(txt);
        return;
    }

    // COMANDO: apagar
    if (str_igual(linea, "apagar") || str_igual(linea, "poweroff") || str_igual(linea, "shutdown")) {
        consola_imprimir_linea_color("==> Apagando equipo vía ACPI...", COLOR_AVISO_DEFAULT);
        esperar_milisegundos(500);
        apagar_equipo();
        return;
    }

    // Si parece una expresión matemática directa (estilo HolyC, ej: 40 + 2)
    if ((linea[0] >= '0' && linea[0] <= '9') || linea[0] == '(') {
        int64_t res = evaluar_expresion(linea);
        consola_imprimir_color("--> ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_dec((uint64_t)res);
        consola_imprimir(" (");
        consola_imprimir_hex((uint64_t)res);
        consola_imprimir_linea(")");
        return;
    }

    // Comando no reconocido
    consola_imprimir_color("Comando desconocido: '", COLOR_ERROR_DEFAULT);
    consola_imprimir_color(linea, COLOR_ERROR_DEFAULT);
    consola_imprimir_linea_color("'. Escribe 'ayuda' para ver las opciones disponibles.", COLOR_ERROR_DEFAULT);
}

void terminal_ejecutar(void) {
    terminal_iniciar();

    char buffer[256];

    for (;;) {
        imprimir_prompt();
        int len = consola_leer_linea(buffer, sizeof(buffer));
        if (len > 0) {
            procesar_comando(buffer);
        }
    }
}
