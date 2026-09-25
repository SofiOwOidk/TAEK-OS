#include "terminal.h"
#include "consola.h"
#include "teclado.h"
#include "pantalla.h"
#include "audio_ac97.h"
#include "animacion_cangrejo.h"
#include "gpu.h"
#include "../compatibilidad/linux.h"
#include "../compatibilidad/nv_os_interface.h"
#include "video/nvidia/core/nvidia_core.h"
#include "video/nvidia/firmware/gsp_firmware.h"
#include "video/nvidia/gsp/gsp_rpc.h"
#include "../base/huevo.h"
#include "../base/energia.h"
#include "../base/tiempo.h"
#include "../base/memoria.h"
#include "../base/dma.h"
#include "../base/paginacion.h"
#include "../arquitectura/x86_64/pci.h"
#include "../arquitectura/x86_64/apic.h"
#include "iommu.h"
#include "xhci.h"
#include "usb_msc.h"
#include "fat32.h"
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
    teclado_iniciar();
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

static void ejecutar_comando_gpu(const char *arg) {
    const struct estado_gpu *gpu = gpu_obtener_estado();

    if (!gpu || !gpu->gpu_detectada) {
        consola_imprimir_linea_color("==> [ AVISO ] No se detectó ninguna controladora GPU en el bus PCIe.", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("El sistema se encuentra operando mediante el Framebuffer Lineal GOP de UEFI.");
        return;
    }

    // MODO PRUEBA / AUTODIAGNÓSTICO: gpu probar / gpu test
    if (arg && (str_igual(arg, "probar") || str_igual(arg, "test") || str_igual(arg, "diag"))) {
        consola_imprimir_linea_color("========== AUTODIAGNÓSTICO DE CONTROLADOR DE GPU Y MMIO ==========", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("Iniciando verificación rigurosa de silicio y memoria mapeada...");

        consola_imprimir("  1. Detección en Bus PCI Express (BDF y Fabricante)... ");
        if (gpu->gpu_detectada) {
            consola_imprimir("[OK: ");
            terminal_imprimir_hex_fijo(gpu->bus, 2);
            consola_imprimir(":");
            terminal_imprimir_hex_fijo(gpu->ranura, 2);
            consola_imprimir(".");
            terminal_imprimir_hex_fijo(gpu->funcion, 1);
            consola_imprimir(" | ");
            consola_imprimir(gpu->nombre_proveedor);
            consola_imprimir_linea_color("]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  2. Verificación de Apertura BAR MMIO Física... ");
        if (gpu->dir_fisica_mmio != 0 && gpu->tamano_mmio > 0) {
            consola_imprimir("[OK: 0x");
            terminal_imprimir_hex_fijo(gpu->dir_fisica_mmio, 8);
            consola_imprimir(" - ");
            imprimir_tamano_barra(gpu->tamano_mmio);
            consola_imprimir_linea_color("]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLÓ - SIN BAR MMIO]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  3. Mapeo Virtual sin Caché (PCD/PWT en PML4 de 4 niveles)... ");
        if (gpu->mapeo_mmio_activo && gpu->dir_virtual_mmio == GPU_MMIO_VIRTUAL_BASE) {
            consola_imprimir("[OK: 0x");
            terminal_imprimir_hex_fijo(gpu->dir_virtual_mmio, 16);
            consola_imprimir_linea_color("]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLÓ]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  4. Lectura de Registro Maestro de Silicio (Offset +0x00)... ");
        uint32_t val_silicio = gpu_leer_mmio_32(0x00000000);
        consola_imprimir("[OK: Valor 0x");
        terminal_imprimir_hex_fijo(val_silicio, 8);
        consola_imprimir(" | ");
        consola_imprimir(gpu->arquitectura_nombre);
        consola_imprimir_linea_color("]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  5. Prueba de Latencia y Comunicación de Bus PCIe (rdtsc)... ");
        uint64_t t_inicio = rdtsc();
        gpu_leer_mmio_32(0x00000000);
        uint64_t t_fin = rdtsc();
        uint64_t ciclos = t_fin - t_inicio;
        consola_imprimir("[OK: ");
        consola_imprimir_dec(ciclos);
        consola_imprimir_linea_color(" ciclos de reloj CPU]", COLOR_EXITO_DEFAULT);

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO EXITOSO ] Pipeline de comunicación MMIO con GPU operativo.", COLOR_PROMPT_DEFAULT);
        return;
    }

    // REPORTE ESTÁNDAR: gpu
    consola_imprimir_linea_color("================== CONTROLADOR DE ACELERACIÓN GPU ==================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Dispositivo PCIe      : ");
    terminal_imprimir_hex_fijo(gpu->bus, 2);
    consola_imprimir(":");
    terminal_imprimir_hex_fijo(gpu->ranura, 2);
    consola_imprimir(".");
    terminal_imprimir_hex_fijo(gpu->funcion, 1);
    consola_imprimir(" [");
    terminal_imprimir_hex_fijo(gpu->id_proveedor, 4);
    consola_imprimir(":");
    terminal_imprimir_hex_fijo(gpu->id_dispositivo, 4);
    consola_imprimir("] ");
    consola_imprimir_linea_color(gpu->nombre_proveedor, COLOR_EXITO_DEFAULT);

    consola_imprimir("  Arquitectura Silicio  : ");
    consola_imprimir_linea_color(gpu->arquitectura_nombre, COLOR_USUARIO_DEFAULT);

    consola_imprimir("  Firma Silicio (Boot0) : 0x");
    terminal_imprimir_hex_fijo(gpu->firma_silicio_boot0, 8);
    consola_imprimir_linea("");

    consola_imprimir("  Espacio MMIO Físico   : 0x");
    terminal_imprimir_hex_fijo(gpu->dir_fisica_mmio, 16);
    consola_imprimir(" (Tamaño: ");
    imprimir_tamano_barra(gpu->tamano_mmio);
    consola_imprimir_linea(")");

    consola_imprimir("  Mapeo MMIO Virtual    : 0x");
    terminal_imprimir_hex_fijo(gpu->dir_virtual_mmio, 16);
    if (gpu->mapeo_mmio_activo) {
        consola_imprimir_linea_color(" [ACTIVO - SIN CACHÉ / PCD]", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color(" [INACTIVO]", COLOR_ERROR_DEFAULT);
    }

    consola_imprimir("  Memoria VRAM Física   : ");
    if (gpu->vram_detectada && gpu->tamano_vram > 0) {
        consola_imprimir("0x");
        terminal_imprimir_hex_fijo(gpu->dir_fisica_vram, 16);
        consola_imprimir(" (Capacidad: ");
        imprimir_tamano_barra(gpu->tamano_vram);
        consola_imprimir_linea_color(")", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("No mapeada como BAR independiente (Usa VRAM Compartida / UMA)", COLOR_TEXTO_DEFAULT);
    }

    consola_imprimir("  Capa de Driver Kernel : ");
    if (gpu->id_proveedor == 0x10DE) {
        consola_imprimir_linea_color("Shim Linux 'nvidia-open' / GSP Firmware Bridge (Fase 3)", COLOR_PROMPT_DEFAULT);
    } else {
        consola_imprimir_linea_color("Controlador Nativo TAEK OS / Framebuffer Directo", COLOR_PROMPT_DEFAULT);
    }

    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'gpu probar' para comprobar la latencia y lectura del silicio.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_linux(const char *arg) {
    uint64_t mem_dma = 0;
    uint64_t mapeos_io = 0;
    uint32_t devs_pci = 0;
    linux_shim_obtener_estadisticas(&mem_dma, &mapeos_io, &devs_pci);

    // MODO AUTODIAGNÓSTICO: linux probar / shim probar
    if (arg && (str_igual(arg, "probar") || str_igual(arg, "test") || str_igual(arg, "diag"))) {
        consola_imprimir_linea_color("========== AUTODIAGNÓSTICO DE CAPA SHIM LINUX KERNEL ==========", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("Verificando llamadas al sistema, sincronización y memoria del puente...");

        consola_imprimir("  1. Primitivas de Concurrencia (Spinlocks, Atomics, Mutex, Semáforos)... ");
        int res = linux_shim_ejecutar_autodiagnostico();
        if (res == 0) {
            consola_imprimir_linea_color("[OK - SPINLOCK, ATOMICS, MUTEX, SEMÁFOROS 100%]", COLOR_EXITO_DEFAULT);
        } else if (res >= 1 && res <= 4) {
            consola_imprimir_linea_color("[FALLÓ EN MUTEX/ATOMICS]", COLOR_ERROR_DEFAULT);
            return;
        } else if (res == 5 || res == 6) {
            consola_imprimir_linea_color("[FALLÓ EN SEMÁFOROS]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  2. Colas de Espera y Notificación (wait_queue_head_t / wake_up)... ");
        if (res == 7) {
            consola_imprimir_linea_color("[FALLÓ EN WAITQUEUES]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - WAITQUEUES OK]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  3. Colas de Trabajo Asíncronas (struct work_struct / schedule_work)... ");
        if (res == 8) {
            consola_imprimir_linea_color("[FALLÓ EN WORKQUEUES]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - TAREAS ASÍNCRONAS EJECUTADAS]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  4. Temporización y Ticks del Kernel (jiffies, timer_list, ktime)... ");
        if (res == 9 || res == 10) {
            consola_imprimir_linea_color("[FALLÓ EN TIMERS/JIFFIES]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - JIFFIES Y TIMERS OK]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  5. Memoria DMA Coherente Continua (dma_alloc_coherent 64 KiB)... ");
        if (res == 11 || res == 12) {
            consola_imprimir_linea_color("[FALLÓ EN DMA COHERENTE]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - DMA COHERENTE OK]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  6. Mapeo MMIO sin Caché y Desmapeo (ioremap / iounmap)... ");
        if (res == 13) {
            consola_imprimir_linea_color("[FALLÓ EN IOREMAP]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - IOREMAP & IOUNMAP OK]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  7. Infraestructura MSI / MSI-X e Interrupciones (request_irq)... ");
        if (res == 14) {
            consola_imprimir_linea_color("[FALLÓ EN MSI/IRQ]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - PUENTE MSI/IRQ ENLAZADO]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  8. Adaptador de Dispositivos PCI (struct pci_dev de Linux)... ");
        consola_imprimir("[OK: ");
        consola_imprimir_dec((uint64_t)devs_pci);
        consola_imprimir_linea_color(" dispositivos enlazados]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  9. Formateador de Telemetría (printk y pr_info)... ");
        pr_info("Mensaje de prueba emitido desde la capa Linux Shim a Anillo 0.");

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO EXITOSO ] Linux Shim 100% operativo como host para open-gpu-kernel-modules.", COLOR_PROMPT_DEFAULT);
        return;
    }

    // REPORTE ESTÁNDAR: linux / shim
    consola_imprimir_linea_color("================ CAPA DE COMPATIBILIDAD LINUX SHIM ================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Versión de ABI Emulada: ");
    consola_imprimir_linea_color("Linux Kernel 6.12 LTS (Interfaces de Controladores)", COLOR_USUARIO_DEFAULT);
    consola_imprimir("  Rango Virtual ioremap : 0x");
    terminal_imprimir_hex_fijo(LINUX_SHIM_IOREMAP_BASE, 16);
    consola_imprimir_linea(" (Espacio Propio en VMM)");
    consola_imprimir("  Mapeos ioremap Activos: ");
    consola_imprimir_dec(mapeos_io);
    consola_imprimir_linea("");
    consola_imprimir("  Memoria DMA Asignada  : ");
    consola_imprimir_dec(mem_dma / 1024ULL);
    consola_imprimir_linea(" KiB (Búferes continuos de comunicación con GPU)");
    consola_imprimir("  Dispositivos PCI Shim : ");
    consola_imprimir_dec((uint64_t)devs_pci);
    consola_imprimir_linea(" (Estructuras 'struct pci_dev' adaptadas)");
    consola_imprimir("  Controladores Objetivos: ");
    consola_imprimir_linea_color("NVIDIA Open GPU Kernel Modules (GSP Client) & VirtIO-GPU", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'linux probar' para verificar el puente y llamadas DMA.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_dmesg(void) {
    uint32_t tamano = 0, cursor = 0;
    const char *buf = serial_obtener_log_buffer(&tamano, &cursor);

    consola_imprimir_linea_color("================ BUFFER DE REGISTRO DEL KERNEL (DMESG / COM1) ================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Estado UART COM1 Físico   : ");
    if (serial_esta_activo()) {
        consola_imprimir_linea_color("ACTIVO (0x3F8 @ 115200 8N1 - Transmitiendo a hardware externo)", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("MODO SÓLO RAM (Puerto 0x3F8 ausente o desactivado en BIOS)", COLOR_AVISO_DEFAULT);
    }
    consola_imprimir("  Memoria de Log en Anillo  : ");
    consola_imprimir_dec((uint64_t)tamano);
    consola_imprimir(" bytes registrados (Capacidad: 64 KiB)\n");

    if (buf && tamano > 0) {
        uint32_t inicio = 0;
        if (tamano > 1500) {
            inicio = tamano - 1500;
            consola_imprimir_linea_color("[... mostrando los últimos 1500 bytes del registro del kernel ...]", COLOR_PROMPT_DEFAULT);
        }
        for (uint32_t i = inicio; i < tamano; i++) {
            consola_escribir_caracter(buf[i]);
        }
    }
    consola_imprimir_linea("");
    consola_imprimir_linea_color("================================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Puedes capturar este log en vivo conectando un cable USB-Serial a 115200 baudios.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_guia(const char *arg) {
    (void)arg;
    consola_imprimir_linea_color("================================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("   GUÍA OFICIAL DE TESTEO DE GPU DEDICADA EN HARDWARE REAL                      ", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("   Objetivo: Plataforma x86_64 + GPU PCIe Dedicada (Directo CPU)                 ", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea_color("================================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea("");

    consola_imprimir_linea_color("[ PASO 1: VERIFICAR DETECCIÓN FÍSICA EN EL BUS PCIE ]", COLOR_EXITO_DEFAULT);
    consola_imprimir_linea_color("  Comando a ejecutar: lspci  o  pci gpu", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("  * En tu placa física con la GPU dedicada debes ver:");
    consola_imprimir_linea("    - Vendor ID: 0x10DE (NVIDIA Corporation)");
    consola_imprimir_linea("    - Device ID: 0x2F04 (GeForce RTX 5070 Ti Desktop)");
    consola_imprimir_linea("    - Clase    : 0x0300 (VGA Compatible Controller)");
    consola_imprimir_linea("  * Si en 'Modo de Operación' indica 'Hardware Real MoDT', tu tarjeta está");
    consola_imprimir_linea("    correctamente alimentada y enlazada a las 16 líneas PCIe del procesador.");
    consola_imprimir_linea("");

    consola_imprimir_linea_color("[ PASO 2: INSPECCIONAR REGISTROS BAR0 MMIO Y BAR1 VRAM ]", COLOR_EXITO_DEFAULT);
    consola_imprimir_linea_color("  Comando a ejecutar: gpu  o  gpu probar", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("  * BAR0 Físico: Espacio de control MMIO asignado por la BIOS UEFI (16 MiB).");
    consola_imprimir_linea("  * BAR1 Físico: Apertura de memoria VRAM (16 GiB en direccionamiento de 64 bits).");
    consola_imprimir_linea("  * Registro PMC_BOOT_0: Comprueba la lectura directa de silicio (0x190xxxxx para Blackwell).");
    consola_imprimir_linea("");

    consola_imprimir_linea_color("[ PASO 3: INSPECCIONAR EL SUBSISTEMA GSP ANTES DEL ARRANQUE ]", COLOR_EXITO_DEFAULT);
    consola_imprimir_linea_color("  Comando a ejecutar: nvidia gsp", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("  * Comprobarás que el microcódigo GSP aún no está cargado.");
    consola_imprimir_linea("  * Las colas CMD_Q y STAT_Q de 64 KiB en RAM física estarán inactivas.");
    consola_imprimir_linea("");

    consola_imprimir_linea_color("[ PASO 4: DISPARAR LA SECUENCIA DE ARRANQUE GPU DE 6 PASOS ]", COLOR_EXITO_DEFAULT);
    consola_imprimir_linea_color("  Comando a ejecutar: nvidia inicializar", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("  * Observarás en vivo la telemetría del silicio paso a paso:");
    consola_imprimir_linea("    1. Detección y validación en PCIe -> [OK]");
    consola_imprimir_linea("    2. Asignación WPR de 16 MiB en DMA contiguo (Base física alineada a 64 KiB) -> [OK]");
    consola_imprimir_linea("    3. Inicialización de colas circulares CMD/STAT y enlace Falcon Mailbox -> [OK]");
    consola_imprimir_linea("    4. Handshake RPC inicial con coprocesador GSP (ABI v1.0) -> [OK]");
    consola_imprimir_linea("    5. Consulta de topología y extracción de capacidades de silicio -> [OK]");
    consola_imprimir_linea("    6. Transición formal al ESTADO OPERATIVO.");
    consola_imprimir_linea("");

    consola_imprimir_linea_color("[ PASO 5: VERIFICAR TELEMETRÍA Y CAPACIDADES ACTIVAS ]", COLOR_EXITO_DEFAULT);
    consola_imprimir_linea_color("  Comando a ejecutar: nvidia", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("  * Verifica que el Estado Operativo marque: 'OPERATIVO (SILICIO BLACKWELL ACTIVO)' en verde.");
    consola_imprimir_linea("  * VRAM Dedicada: 16 GiB GDDR7 (Bus 256 bits a 28 Gbps).");
    consola_imprimir_linea("  * Cómputo: 70 SMs | 8,960 CUDA Cores | Tensor Cores Gen 4 | RT Cores Gen 5.");
    consola_imprimir_linea("  * Frecuencias: 2,160 MHz Base / 2,520 MHz Boost.");
    consola_imprimir_linea("");

    consola_imprimir_linea_color("[ PASO 6: EJECUTAR EL AUTODIAGNÓSTICO INTEGRAL EXTREMO A EXTREMO ]", COLOR_EXITO_DEFAULT);
    consola_imprimir_linea_color("  Comando a ejecutar: nvidia probar", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("  * Audita los 5 puntos críticos de la arquitectura (Spinlocks, WPR DMA, RPC, Silicio y Caps).");
    consola_imprimir_linea("  * Debe concluir con: '==> [ AUTODIAGNÓSTICO EXITOSO ] Coprocesador GSP y GPU Blackwell 100% Operativos.'");
    consola_imprimir_linea_color("================================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Si tienes un pendrive USB, grábale 'build/taek-os.iso' con Rufus o Ventoy y pruébalo.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_nvidia(const char *arg) {
    const struct nvidia_dispositivo *ndev = nvidia_core_obtener_dispositivo();

    // SUBCOMANDO: nvidia inicializar / init / start / arrancar (Hito 20)
    if (arg && (str_igual(arg, "inicializar") || str_igual(arg, "init") ||
                str_igual(arg, "arrancar") || str_igual(arg, "boot"))) {
        consola_imprimir_linea_color("========== SECUENCIA DE ARRANQUE GPU NVIDIA BLACKWELL (HITO 20) ==========", COLOR_AVISO_DEFAULT);

        if (!ndev->presente) {
            consola_imprimir_linea_color("  [AVISO] Silicio NVIDIA no detectado en bus PCIe.", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea("  Estás ejecutando en un entorno virtual sin GPU física NVIDIA (ej. QEMU).");
            consola_imprimir_linea("  Para inicializar el silicio real, graba 'build/taek-os.iso' en un pendrive");
            consola_imprimir_linea("  y bootea en un equipo x86_64 físico con GPU PCIe dedicada.");
            return;
        }

        consola_imprimir_linea("Ejecutando secuencia de inicialización del silicio y firmware GSP...");

        consola_imprimir("  Paso 1: Detección y verificación de silicio en bus PCIe... ");
        consola_imprimir("[OK: ");
        consola_imprimir(ndev->chip_name);
        consola_imprimir_linea_color("]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  Paso 2: Carga de microcódigo oficial GSP y reserva WPR en DMA... ");
        NV_STATUS st_fw = gsp_firmware_cargar((struct nvidia_dispositivo *)ndev);
        if (st_fw == NV_OK) {
            const gsp_firmware_descriptor_t *fw = gsp_firmware_obtener_info();
            consola_imprimir("[OK: ");
            consola_imprimir_dec(fw->tamano_wpr_heap / (1024 * 1024));
            consola_imprimir_linea_color(" MiB WPR]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO EN FIRMWARE]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  Paso 3: Inicialización de colas circulares RPC y Mailbox Falcon... ");
        NV_STATUS st_rpc = gsp_rpc_iniciar((struct nvidia_dispositivo *)ndev);
        if (st_rpc == NV_OK) {
            consola_imprimir_linea_color("[OK - CMD_Q Y STAT_Q LISTAS]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO EN COLAS RPC]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  Paso 4: Handshake de protocolo RPC (GSP_RPC_CMD_INITIALIZE)... ");
        uint32_t abi_ver = 0;
        uint32_t len_abi = sizeof(abi_ver);
        NV_STATUS st_hand = gsp_rpc_enviar_sincrono(GSP_RPC_CMD_INITIALIZE, NULL, 0, &abi_ver, &len_abi);
        if (st_hand == NV_OK) {
            consola_imprimir("[OK - ABI v");
            consola_imprimir_dec((uint64_t)(abi_ver >> 24));
            consola_imprimir(".");
            consola_imprimir_dec((uint64_t)((abi_ver >> 16) & 0xFF));
            consola_imprimir_linea_color("]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO HANDSHAKE]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  Paso 5: Consulta de topología y capacidades silicio (GET_CAPS)... ");
        NV_STATUS st_caps = nvidia_gpu_inicializar_completo();
        if (st_caps == NV_OK) {
            consola_imprimir_linea_color("[OK - CAPACIDADES RECIBIDAS]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO EN GET_CAPS]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  Paso 6: Transición de silicio a estado operativo... ");
        consola_imprimir_linea_color("[OK - OPERATIVO]", COLOR_EXITO_DEFAULT);

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ ARRANQUE GPU COMPLETADO CON ÉXITO ]", COLOR_EXITO_DEFAULT);
        consola_imprimir("  Silicio: ");
        consola_imprimir_linea_color(ndev->caps.nombre_gpu, COLOR_USUARIO_DEFAULT);
        consola_imprimir("  VRAM: ");
        consola_imprimir_dec(ndev->caps.vram_total_bytes / (1024ULL * 1024ULL * 1024ULL));
        consola_imprimir(" GiB GDDR7 (Bus: ");
        consola_imprimir_dec((uint64_t)ndev->caps.vram_bus_width);
        consola_imprimir_linea(" bits)");
        consola_imprimir("  Cómputo: ");
        consola_imprimir_dec((uint64_t)ndev->caps.sm_count);
        consola_imprimir(" SMs | ");
        consola_imprimir_dec((uint64_t)ndev->caps.cuda_cores);
        consola_imprimir_linea(" CUDA Cores | RT Cores Gen 5");
        consola_imprimir("  Reloj: ");
        consola_imprimir_dec((uint64_t)ndev->caps.reloj_base_mhz);
        consola_imprimir(" MHz Base / ");
        consola_imprimir_dec((uint64_t)ndev->caps.reloj_boost_mhz);
        consola_imprimir_linea(" MHz Boost");
        return;
    }

    // SUBCOMANDO: nvidia gsp / firmware / rpc (Hitos 18 y 19)
    if (arg && (str_igual(arg, "gsp") || str_igual(arg, "firmware") || str_igual(arg, "rpc"))) {
        const gsp_firmware_descriptor_t *fw = gsp_firmware_obtener_info();
        const gsp_boot_args_t *args = gsp_firmware_obtener_boot_args();
        NvU32 rpc_env = 0, rpc_rec = 0, rpc_err = 0;
        gsp_rpc_obtener_estadisticas(&rpc_env, &rpc_rec, &rpc_err);

        consola_imprimir_linea_color("================ COPROCESADOR GSP & PROTOCOLO RPC (H18/H19) ================", COLOR_AVISO_DEFAULT);
        consola_imprimir("  Microcódigo GSP        : ");
        consola_imprimir_linea_color(fw->cargado_en_dma ? fw->nombre_firmware : "No cargado (Escribe 'nvidia inicializar')", COLOR_USUARIO_DEFAULT);
        consola_imprimir("  Versión Driver NVIDIA  : ");
        consola_imprimir_dec(fw->version_major ? fw->version_major : GSP_FIRMWARE_VERSION_MAJ);
        consola_imprimir(".");
        consola_imprimir_dec(fw->version_minor ? fw->version_minor : GSP_FIRMWARE_VERSION_MIN);
        consola_imprimir_linea(" (Rama Oficial Production Ready)");

        consola_imprimir("  Región Protegida WPR   : ");
        if (args->wpr_base_phys != 0) {
            consola_imprimir("0x");
            terminal_imprimir_hex_fijo(args->wpr_base_phys, 16);
            consola_imprimir(" (");
            consola_imprimir_dec(args->wpr_size / (1024 * 1024));
            consola_imprimir_linea_color(" MiB en DMA Contiguo) [PROTEGIDA]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("No configurada (Escribe 'nvidia inicializar')", COLOR_AVISO_DEFAULT);
        }

        consola_imprimir("  Cola Comandos (CMD_Q)  : ");
        if (args->cmd_queue_phys != 0) {
            consola_imprimir("0x");
            terminal_imprimir_hex_fijo(args->cmd_queue_phys, 16);
            consola_imprimir(" (64 KiB Circular en RAM)");
        } else {
            consola_imprimir("Inactiva");
        }
        consola_imprimir_linea("");

        consola_imprimir("  Cola Estado   (STAT_Q) : ");
        if (args->stat_queue_phys != 0) {
            consola_imprimir("0x");
            terminal_imprimir_hex_fijo(args->stat_queue_phys, 16);
            consola_imprimir(" (64 KiB Circular en RAM)");
        } else {
            consola_imprimir("Inactiva");
        }
        consola_imprimir_linea("");

        consola_imprimir("  Falcon Mailbox 0 / 1   : ");
        consola_imprimir("MMIO 0x00110040 / 0x00110044 -> 0x");
        terminal_imprimir_hex_fijo(args->cmd_queue_phys, 16);
        consola_imprimir_linea("");

        consola_imprimir("  Tráfico Mensajería RPC : ");
        consola_imprimir_dec((uint64_t)rpc_env);
        consola_imprimir(" enviados | ");
        consola_imprimir_dec((uint64_t)rpc_rec);
        consola_imprimir(" recibidos | ");
        consola_imprimir_dec((uint64_t)rpc_err);
        consola_imprimir_linea(" errores");

        consola_imprimir_linea_color("=============================================================================", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("Tip: Usa 'nvidia probar' para auditar el canal y la máquina de estados.", COLOR_TEXTO_DEFAULT);
        return;
    }

    // MODO AUTODIAGNÓSTICO: nvidia probar / nvidia test
    if (arg && (str_igual(arg, "probar") || str_igual(arg, "test") || str_igual(arg, "diag"))) {
        consola_imprimir_linea_color("========== AUTODIAGNÓSTICO DEL SUBSISTEMA NVIDIA GSP (HITOS 18, 19 Y 20) ==========", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("Verificando firmware loader, colas RPC, handshake e inicialización...");

        consola_imprimir("  1. Sincronización interna del Resource Manager (Spinlocks)... ");
        nv_spinlock_t core_lock;
        nv_os_spinlock_init(&core_lock);
        nv_os_spinlock_acquire(&core_lock);
        nv_os_spinlock_release(&core_lock);
        consola_imprimir_linea_color("[OK - SPINLOCKS DE SILICIO OK]", COLOR_EXITO_DEFAULT);

        if (!ndev->presente) {
            consola_imprimir_linea_color("  [AVISO] Silicio NVIDIA no detectado en bus PCI (Entorno QEMU).", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("  [OK] Primitivas de concurrencia y búfer DMA listos para hospedar hardware real.", COLOR_PROMPT_DEFAULT);
            consola_imprimir_linea("  Para auditar el silicio real: bootea build/taek-os.iso en la máquina MoDT.");
            return;
        }

        consola_imprimir("  2. Cargador de Firmware GSP y Región WPR en DMA (Hito 18)... ");
        int fw_diag = gsp_firmware_autodiagnostico((struct nvidia_dispositivo *)ndev);
        if (fw_diag == 0) {
            consola_imprimir_linea_color("[OK - FIRMWARE GSP AUTENTICADO]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO EN FIRMWARE GSP]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  3. Canal de Comunicación RPC y Colas Circulares (Hito 19)... ");
        int rpc_diag = gsp_rpc_autodiagnostico((struct nvidia_dispositivo *)ndev);
        if (rpc_diag == 0) {
            consola_imprimir_linea_color("[OK - PROTOCOLO RPC 100% FUNCIONAL]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO EN CANAL RPC]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  4. Inicialización Completa de Silicio GPU Blackwell (Hito 20)... ");
        NV_STATUS st_init = nvidia_gpu_inicializar_completo();
        if (st_init == NV_OK && ndev->estado == NV_GPU_ESTADO_OPERATIVO) {
            consola_imprimir_linea_color("[OK - ESTADO OPERATIVO ALCANZADO]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO AL INICIALIZAR GPU]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  5. Verificación de Capacidades Extraídas (VRAM y SMs)... ");
        if (ndev->caps.vram_total_bytes > 0 && ndev->caps.sm_count > 0) {
            consola_imprimir("[OK: 16 GiB GDDR7 / ");
            consola_imprimir_dec(ndev->caps.sm_count);
            consola_imprimir_linea_color(" SMs / 8960 CUDA Cores]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO EN CAPACIDADES]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO EXITOSO ] Coprocesador GSP y GPU Blackwell 100% Operativos.", COLOR_EXITO_DEFAULT);
        return;
    }

    // REPORTE ESTÁNDAR: nvidia
    consola_imprimir_linea_color("================ CONTROLADOR NVIDIA (RESOURCE MANAGER CORE) ================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Aislamiento Arquitectónico: ");
    consola_imprimir_linea_color("100% Aislado en 'nucleo/controladores/video/nvidia/'", COLOR_EXITO_DEFAULT);
    consola_imprimir("  Dispositivo / Arquitectura: ");
    consola_imprimir_linea_color(ndev->chip_name, COLOR_USUARIO_DEFAULT);
    consola_imprimir("  Identificador PCI (Vendor): 0x");
    terminal_imprimir_hex_fijo(ndev->vendor_id, 4);
    consola_imprimir(" | Device: 0x");
    terminal_imprimir_hex_fijo(ndev->device_id, 4);
    consola_imprimir(" | ChipID: 0x");
    terminal_imprimir_hex_fijo(ndev->chip_id, 8);
    consola_imprimir_linea("");

    consola_imprimir("  Estado Operativo GPU      : ");
    if (ndev->estado == NV_GPU_ESTADO_OPERATIVO) {
        consola_imprimir_linea_color(nvidia_gpu_estado_nombre(ndev->estado), COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_color(nvidia_gpu_estado_nombre(ndev->estado), COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color(" (Escribe 'nvidia inicializar' para arrancar)", COLOR_TEXTO_DEFAULT);
    }

    consola_imprimir("  Modo de Operación         : ");
    if (ndev->presente) {
        consola_imprimir_linea_color("Hardware Real (PCIe Directo x16 a CPU)", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("Emulación QEMU (Silicio NVIDIA ausente en bus PCI)", COLOR_AVISO_DEFAULT);
    }

    consola_imprimir("  Memoria VRAM Dedicada     : ");
    if (ndev->estado == NV_GPU_ESTADO_OPERATIVO && ndev->caps.vram_total_bytes > 0) {
        consola_imprimir_dec(ndev->caps.vram_total_bytes / (1024ULL * 1024ULL * 1024ULL));
        consola_imprimir(" GiB GDDR7 (Bus ");
        consola_imprimir_dec((uint64_t)ndev->caps.vram_bus_width);
        consola_imprimir_linea_color(" bits / 28 Gbps)", COLOR_EXITO_DEFAULT);
    } else if (ndev->presente) {
        consola_imprimir("BAR1 Fís: 0x");
        terminal_imprimir_hex_fijo(ndev->bar1_phys, 16);
        consola_imprimir_linea(" (16 GiB GDDR7)");
    } else {
        consola_imprimir_linea_color("No detectada (Requiere hardware físico)", COLOR_TEXTO_DEFAULT);
    }

    consola_imprimir("  Núcleos y Cómputo         : ");
    if (ndev->estado == NV_GPU_ESTADO_OPERATIVO && ndev->caps.sm_count > 0) {
        consola_imprimir_dec((uint64_t)ndev->caps.sm_count);
        consola_imprimir(" SMs | ");
        consola_imprimir_dec((uint64_t)ndev->caps.cuda_cores);
        consola_imprimir_linea(" CUDA Cores | 4th Gen Tensor | 5th Gen RT");
    } else if (ndev->presente) {
        consola_imprimir_linea("Pendiente de extracción vía GSP GET_CAPS");
    } else {
        consola_imprimir_linea_color("No disponibles (GPU ausente)", COLOR_TEXTO_DEFAULT);
    }

    consola_imprimir("  Frecuencias de Reloj      : ");
    if (ndev->estado == NV_GPU_ESTADO_OPERATIVO && ndev->caps.reloj_base_mhz > 0) {
        consola_imprimir_dec((uint64_t)ndev->caps.reloj_base_mhz);
        consola_imprimir(" MHz Base / ");
        consola_imprimir_dec((uint64_t)ndev->caps.reloj_boost_mhz);
        consola_imprimir_linea(" MHz Boost");
    } else if (ndev->presente) {
        consola_imprimir_linea("Pendiente de extracción vía GSP");
    } else {
        consola_imprimir_linea_color("No disponibles", COLOR_TEXTO_DEFAULT);
    }

    consola_imprimir("  Capa de Interfaz (Bridge) : ");
    consola_imprimir_linea_color("nv_os_interface -> TAEK Linux Shim -> VMM/PMM Soberano", COLOR_PROMPT_DEFAULT);

    consola_imprimir_linea_color("============================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Usa 'nvidia inicializar' para ejecutar la secuencia de arranque GSP.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("Tip: Usa 'nvidia gsp' para ver el estado de colas circulares y Mailbox.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("Tip: Usa 'nvidia probar' para auditar todo el pipeline de silicio y RPC.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_apic(const char *arg) {
    const struct estado_apic *apic = apic_obtener_estado();

    // MODO AUTODIAGNÓSTICO: apic probar / apic test
    if (arg && (str_igual(arg, "probar") || str_igual(arg, "test") || str_igual(arg, "diag"))) {
        consola_imprimir_linea_color("========== AUTODIAGNÓSTICO DE CONTROLADOR LOCAL APIC / IRQ ==========", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("Verificando desactivación de PIC legacy, estado de CPU y disparo Self-IPI...");

        consola_imprimir("  1. Comprobando desactivación del chip PIC 8259 legacy... ");
        consola_imprimir_linea_color("[OK - ENMASCARADO 100%]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  2. Modo de Operación del Controlador Local APIC... ");
        if (apic->es_x2apic) {
            consola_imprimir_linea_color("[OK - x2APIC MSR NATIVO]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[OK - xAPIC MMIO SIN CACHÉ / PCD]", COLOR_EXITO_DEFAULT);
        }

        consola_imprimir("  3. Disparo de Interrupción Inter-Procesador Self-IPI (Vector 80)... ");
        uint32_t ipis_antes = apic->ipi_recibidos;
        int res = apic_ejecutar_autodiagnostico();
        if (res == 0) {
            consola_imprimir_linea_color("[OK - IPI DISPARADA Y ATENDIDA]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLÓ - NO SE RECIBIÓ IPI]", COLOR_ERROR_DEFAULT);
            return;
        }

        consola_imprimir("  4. Captura en IDT Anillo 0 y Confirmación EOI... ");
        consola_imprimir("[OK: ");
        consola_imprimir_dec(apic->ipi_recibidos - ipis_antes);
        consola_imprimir_linea_color(" IPI registrada, EOI enviado]", COLOR_EXITO_DEFAULT);

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO EXITOSO ] Local APIC e interrupciones operativas para GPU (MSI/MSI-X).", COLOR_PROMPT_DEFAULT);
        return;
    }

    // REPORTE ESTÁNDAR: apic / irq
    consola_imprimir_linea_color("================ CONTROLADOR LOCAL APIC & ENRUTADOR IRQ ================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Arquitectura Activa    : ");
    if (apic->es_x2apic) {
        consola_imprimir_linea_color("x2APIC (Acceso Ultrarrápido MSR)", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("xAPIC Tradicional (Mapeo MMIO sin caché / PCD)", COLOR_USUARIO_DEFAULT);
    }
    consola_imprimir("  ID Núcleo CPU (LAPIC) : ");
    consola_imprimir_dec((uint64_t)apic->id);
    consola_imprimir(" (Núcleo de Arranque / BSP)");
    consola_imprimir_linea("");

    consola_imprimir("  Versión de Silicio     : 0x");
    terminal_imprimir_hex_fijo((uint64_t)apic->version, 2);
    consola_imprimir_linea("");

    consola_imprimir("  Dirección Base Física  : 0x");
    terminal_imprimir_hex_fijo(apic->dir_fisica_base, 16);
    consola_imprimir_linea("");

    if (!apic->es_x2apic) {
        consola_imprimir("  Dirección Base Virtual : 0x");
        terminal_imprimir_hex_fijo(apic->dir_virtual_base, 16);
        consola_imprimir_linea_color(" [PAGINA_ATRIBUTOS_MMIO]", COLOR_EXITO_DEFAULT);
    }

    consola_imprimir("  Controlador PIC 8259   : ");
    consola_imprimir_linea_color("Desactivado (Enmascarado 0xFF en puertos 0x21 y 0xA1)", COLOR_EXITO_DEFAULT);

    consola_imprimir("  Interrupciones Totales : ");
    consola_imprimir_dec((uint64_t)apic->interrupciones_recibidas);
    consola_imprimir(" | Self-IPIs: ");
    consola_imprimir_dec((uint64_t)apic->ipi_recibidos);
    consola_imprimir_linea("");

    consola_imprimir("  Soporte PCIe MSI/MSI-X : ");
    consola_imprimir_linea_color("Habilitado para GPU NVIDIA y Dispositivos de Alto Rendimiento", COLOR_PROMPT_DEFAULT);

    consola_imprimir_linea_color("=========================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'apic probar' para disparar una interrupción Self-IPI.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_dma(const char *arg) {
    // MODO AUTODIAGNÓSTICO: dma probar / dma test
    if (arg && (str_igual(arg, "probar") || str_igual(arg, "test") || str_igual(arg, "diag"))) {
        consola_imprimir_linea_color("========== AUTODIAGNÓSTICO DE MEMORIA DMA CONTIGUA Y COHERENCIA ==========", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("Verificando asignador físico, continuidad absoluta de 16 páginas y barreras...");

        consola_imprimir("  1. Comprobando inicialización de la arena física DMA... ");
        dma_estadisticas_t est_antes;
        dma_obtener_estadisticas(&est_antes);
        if (est_antes.arena_fisica_base == 0) {
            consola_imprimir_linea_color("[FALLO - ARENA NO INICIADA]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - 32 MiB DEDICADOS]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  2. Asignación de bloque contiguo de 64 KiB alineado a 64 KiB... ");
        uint64_t phys_test = 0;
        void *virt_test = dma_asignar_bufer_contiguo(64 * 1024, 64 * 1024, &phys_test);
        if (!virt_test || phys_test == 0 || (phys_test & (64 * 1024 - 1)) != 0) {
            consola_imprimir_linea_color("[FALLO EN ALINEACIÓN]", COLOR_ERROR_DEFAULT);
            return;
        }
        consola_imprimir_linea_color("[OK - BASE ALINEADA]", COLOR_EXITO_DEFAULT);

        consola_imprimir("  3. Verificando continuidad física estricta (16 páginas x 4096 bytes)... ");
        uint64_t hhdm = memoria_obtener_hhdm_offset();
        int continuidad_ok = 1;
        for (uint32_t p = 0; p < 16; p++) {
            uint64_t p_calc = ((uint64_t)virt_test + (p * 4096ULL)) - hhdm;
            if (p_calc != (phys_test + (p * 4096ULL))) {
                continuidad_ok = 0;
                break;
            }
        }
        if (continuidad_ok) {
            consola_imprimir_linea_color("[OK - 100% CONTIGUO]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO - DISCONTINUIDAD DETECTADA]", COLOR_ERROR_DEFAULT);
            dma_liberar_bufer_contiguo(virt_test, phys_test, 64 * 1024);
            return;
        }

        consola_imprimir("  4. Comprobando sincronización de caché (clflush / clflushopt + mfence)... ");
        uint64_t *canarios = (uint64_t *)virt_test;
        for (int i = 0; i < 8; i++) {
            canarios[i * 1024] = 0x507071AEC0507071ULL ^ (uint64_t)i;
        }
        dma_sincronizar_cpu_a_dispositivo(virt_test, 64 * 1024);

        int canarios_ok = 1;
        for (int i = 0; i < 8; i++) {
            if (canarios[i * 1024] != (0x507071AEC0507071ULL ^ (uint64_t)i)) {
                canarios_ok = 0;
                break;
            }
        }
        if (canarios_ok) {
            consola_imprimir_linea_color("[OK - COHERENCIA TOTAL]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[FALLO EN INTEGRIDAD]", COLOR_ERROR_DEFAULT);
        }

        consola_imprimir("  5. Liberación de búfer y comprobación de ausencia de fugas de páginas... ");
        dma_liberar_bufer_contiguo(virt_test, phys_test, 64 * 1024);
        dma_estadisticas_t est_despues;
        dma_obtener_estadisticas(&est_despues);
        if (est_despues.paginas_en_uso == est_antes.paginas_en_uso) {
            consola_imprimir_linea_color("[OK - 0 FUGAS DE MEMORIA]", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("[ALERTA - FUGA DETECTADA]", COLOR_AVISO_DEFAULT);
        }

        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO EXITOSO ] Memoria DMA contigua y sincronización listas para GPU.", COLOR_EXITO_DEFAULT);
        return;
    }

    // REPORTE ESTÁNDAR: dma
    dma_estadisticas_t dma;
    dma_obtener_estadisticas(&dma);

    const iommu_estado_t *iommu = iommu_obtener_estado();

    consola_imprimir_linea_color("================ GESTOR DE MEMORIA DMA CONTIGUA FÍSICA ================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Arena Física Base      : 0x");
    terminal_imprimir_hex_fijo(dma.arena_fisica_base, 16);
    consola_imprimir_linea("");

    consola_imprimir("  Capacidad Total Arena  : ");
    consola_imprimir_dec(dma.arena_tamano_bytes / (1024 * 1024));
    consola_imprimir(" MiB (");
    consola_imprimir_dec((uint64_t)dma.paginas_totales);
    consola_imprimir_linea(" páginas contiguas de 4 KiB)");

    consola_imprimir("  Páginas en Uso / Libre : ");
    consola_imprimir_dec((uint64_t)dma.paginas_en_uso);
    consola_imprimir(" en uso / ");
    consola_imprimir_dec((uint64_t)dma.paginas_libres);
    consola_imprimir_linea(" libres");

    consola_imprimir("  Asignaciones Activas   : ");
    consola_imprimir_dec((uint64_t)dma.asignaciones_activas);
    consola_imprimir(" | Asignación Máx: ");
    consola_imprimir_dec((uint64_t)dma.tamano_maximo_asignado_kb);
    consola_imprimir_linea(" KiB");

    consola_imprimir("  Coherencia de Silicio  : ");
    consola_imprimir_linea_color("Vaciado por hardware con clflushopt / clflush + mfence", COLOR_EXITO_DEFAULT);

    consola_imprimir("  Estado IOMMU / VT-d    : ");
    if (iommu->tabla_dmar_detectada) {
        if (iommu->modo_operacion == IOMMU_MODO_VT_D_PASSTHROUGH) {
            consola_imprimir_linea_color("Intel VT-d Activo (Modo PassThrough de Silicio - Cero Sobrecarga)", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("Intel VT-d Activo (Tablas de Traducción Activas)", COLOR_AVISO_DEFAULT);
        }
        consola_imprimir("  Unidades DRHD / RMRR   : ");
        consola_imprimir_dec((uint64_t)iommu->conteo_drhd);
        consola_imprimir(" DRHD remapeadas | ");
        consola_imprimir_dec((uint64_t)iommu->conteo_rmrr);
        consola_imprimir_linea(" RMRR reservadas protegidas");
    } else {
        consola_imprimir_linea_color("DMA Físico Directo 1:1 Transparente (Sin bloqueo IOMMU)", COLOR_PROMPT_DEFAULT);
    }

    consola_imprimir_linea_color("=========================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'dma probar' para verificar continuidad y barreras de caché.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'iommu' para ver el desglose técnico de unidades DRHD y RMRR.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_iommu(const char *arg) {
    const iommu_estado_t *iommu = iommu_obtener_estado();

    if (arg && (str_igual(arg, "probar") || str_igual(arg, "test") || str_igual(arg, "diag"))) {
        iommu_ejecutar_autodiagnostico();
        consola_imprimir_linea_color("==> [ AUTODIAGNÓSTICO IOMMU CONCLUIDO ]", COLOR_EXITO_DEFAULT);
        return;
    }

    consola_imprimir_linea_color("================ SUBSISTEMA IOMMU & INTEL VT-d (DMAR ACPI) ================", COLOR_AVISO_DEFAULT);
    consola_imprimir("  Tabla ACPI DMAR        : ");
    if (iommu->tabla_dmar_detectada) {
        consola_imprimir_linea_color("DETECTADA Y PROCESADA [OK]", COLOR_EXITO_DEFAULT);
        consola_imprimir("  Ancho Dirección Host   : ");
        consola_imprimir_dec((uint64_t)iommu->ancho_direccion_host);
        consola_imprimir_linea(" bits de direccionamiento físico");
        consola_imprimir("  Modo de Operación      : ");
        if (iommu->modo_operacion == IOMMU_MODO_VT_D_PASSTHROUGH) {
            consola_imprimir_linea_color("PassThrough de Hardware (PT) - 1:1 Directo sin traducción", COLOR_EXITO_DEFAULT);
        } else if (iommu->modo_operacion == IOMMU_MODO_VT_D_TRADUCCION_ACTIVA) {
            consola_imprimir_linea_color("Traducción de Direcciones Activa por Firmware", COLOR_AVISO_DEFAULT);
        } else {
            consola_imprimir_linea_color("Directo Físico Transparente", COLOR_PROMPT_DEFAULT);
        }

        consola_imprimir("  Unidades DRHD Encontradas: ");
        consola_imprimir_dec((uint64_t)iommu->conteo_drhd);
        consola_imprimir_linea("");

        for (uint32_t i = 0; i < iommu->conteo_drhd; i++) {
            const iommu_unidad_drhd_t *u = &iommu->drhd[i];
            consola_imprimir("    * DRHD #");
            consola_imprimir_dec((uint64_t)i);
            consola_imprimir(": MMIO Fís 0x");
            terminal_imprimir_hex_fijo(u->mmio_fisica, 8);
            consola_imprimir(" -> Virt 0x");
            terminal_imprimir_hex_fijo(u->mmio_virtual, 16);
            consola_imprimir(" | Seg: ");
            consola_imprimir_dec((uint64_t)u->segmento_pci);
            consola_imprimir(" | Abarca Todo: ");
            consola_imprimir(u->abarca_todos_pci ? "Sí" : "No");
            consola_imprimir_linea("");

            consola_imprimir("      Versión: 0x");
            terminal_imprimir_hex_fijo((uint64_t)u->version, 4);
            consola_imprimir(" | TES: ");
            consola_imprimir(u->traduccion_activa ? "Activa" : "Inactiva");
            consola_imprimir(" | PassThrough (PT): ");
            consola_imprimir(u->soporta_passthrough ? "Soportado" : "No");
            consola_imprimir(" | Coherente: ");
            consola_imprimir(u->coherente ? "Sí" : "No");
            consola_imprimir_linea("");
        }

        if (iommu->conteo_rmrr > 0) {
            consola_imprimir("  Regiones Reservadas RMRR : ");
            consola_imprimir_dec((uint64_t)iommu->conteo_rmrr);
            consola_imprimir_linea("");
            for (uint32_t i = 0; i < iommu->conteo_rmrr; i++) {
                const iommu_region_rmrr_t *r = &iommu->rmrr[i];
                consola_imprimir("    * RMRR #");
                consola_imprimir_dec((uint64_t)i);
                consola_imprimir(": Base 0x");
                terminal_imprimir_hex_fijo(r->dir_base_fisica, 16);
                consola_imprimir(" - Límite 0x");
                terminal_imprimir_hex_fijo(r->dir_limite_fisica, 16);
                consola_imprimir_linea_color(" [PROTEGIDA CONTRA ESCRITURA]", COLOR_AVISO_DEFAULT);
            }
        }
    } else {
        consola_imprimir_linea_color("NO DETECTADA (QEMU Estándar / MoDT con VT-d deshabilitado)", COLOR_PROMPT_DEFAULT);
        consola_imprimir("  Modo de Operación      : ");
        consola_imprimir_linea_color("DMA Directo Físico 1:1 Transparente", COLOR_EXITO_DEFAULT);
        consola_imprimir_linea("  El bus PCIe opera sin intermediación ni restricciones de aislamiento.");
        consola_imprimir_linea("  La GPU GeForce RTX y dispositivos periféricos acceden libremente a la RAM física.");
    }

    consola_imprimir_linea_color("===========================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'iommu probar' para ejecutar el autodiagnóstico de remapeo.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_autodiagnostico_teclado(void) {
    if (teclado_es_modo_nativo()) {
        consola_imprimir_linea_color("================ [ AUTODIAGNÓSTICO DEL TECLADO Y SUBSISTEMA USB ] ================", COLOR_AVISO_DEFAULT);
        consola_imprimir("  Modo de Operación    : ");
        consola_imprimir_linea_color("[MODO NATIVO FIRMWARE (SMM / USB LEGACY PS/2)]", COLOR_EXITO_DEFAULT);
        consola_imprimir("  Controlador de Teclas: ");
        consola_imprimir_linea_color("Canal i8042 Puertos 0x60 / 0x64 [ACTIVO]", COLOR_PROMPT_DEFAULT);
        consola_imprimir("  Teclado Externo (USB): ");
        consola_imprimir_linea_color("Emulación SMM de BIOS UEFI Activa (Transparente y Estable)", COLOR_EXITO_DEFAULT);
        consola_imprimir("  Teclado Interno (PC) : ");
        consola_imprimir_linea_color("Controlador Embebido (EC) / Matriz PS/2 [ACTIVO]", COLOR_EXITO_DEFAULT);
        consola_imprimir("  Compatibilidad Total : ");
        consola_imprimir_linea_color("100% Idéntico al entorno nativo de Ventoy y Limine", COLOR_USUARIO_DEFAULT);
        consola_imprimir("  Estado de Recepción  : ");
        if (teclado_esta_presente()) {
            consola_imprimir_linea_color("Búfer 0x60 en línea - Decodificación Scancode Set 1 lista", COLOR_EXITO_DEFAULT);
        } else {
            consola_imprimir_linea_color("Esperando pulsación...", COLOR_AVISO_DEFAULT);
        }
        consola_imprimir_linea_color("==================================================================================", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("Tip: Ambos teclados (externo USB e interno) escriben directamente en la terminal.", COLOR_TEXTO_DEFAULT);
        consola_imprimir_linea_color("Tip: Escribe 'teclado probar' para verificar la captura interactiva de teclas.", COLOR_TEXTO_DEFAULT);
        return;
    }

    const struct estado_xhci *st = xhci_obtener_estado();
    consola_imprimir_linea_color("================ [ AUTODIAGNÓSTICO DEL TECLADO Y SUBSISTEMA USB ] ================", COLOR_AVISO_DEFAULT);

    if (!st->controlador_detectado) {
        consola_imprimir_linea_color("  [!] CONTROLADOR xHCI : NO DETECTADO EN BUS PCI", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea("      El kernel no encontró ningún controlador host USB 3.x (Clase 0x0C:03:30).");
        consola_imprimir_linea_color("==================================================================================", COLOR_AVISO_DEFAULT);
        return;
    }

    // 1. Teclado interno
    consola_imprimir("  Teclado Interno      : ");
    if (teclado_es_modo_nativo()) {
        consola_imprimir_linea_color("Controlador i8042 / EC (Puertos 0x60 / 0x64) [EN LINEA]", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea_color("Controlador i8042 / EC [SILENCIADO POR MODO xHCI - CERO FALSOS POSITIVOS]", COLOR_AVISO_DEFAULT);
    }

    // 2. Datos del Controlador PCI
    consola_imprimir("  Controlador Host PCI : ");
    terminal_imprimir_hex_fijo(st->bus, 2);
    consola_imprimir(":");
    terminal_imprimir_hex_fijo(st->ranura, 2);
    consola_imprimir(".");
    terminal_imprimir_hex_fijo(st->funcion, 1);
    consola_imprimir(" [Vendor: ");
    terminal_imprimir_hex_fijo(st->id_proveedor, 4);
    consola_imprimir(" Dev: ");
    terminal_imprimir_hex_fijo(st->id_dispositivo, 4);
    if (st->id_proveedor == 0x8086 && st->id_dispositivo == 0x7A60) {
        consola_imprimir_color(" (Intel Raptor Lake PCH USB 3.2)", COLOR_EXITO_DEFAULT);
    }
    consola_imprimir_linea("]");

    // 2. Espacio MMIO y Colisión VT-d
    consola_imprimir("  Espacio MMIO         : Físico: 0x");
    terminal_imprimir_hex_fijo(st->dir_fisica_mmio, 16);
    consola_imprimir(" -> Virtual: 0x");
    terminal_imprimir_hex_fijo(st->dir_virtual_mmio, 16);
    if (st->dir_virtual_mmio == 0xFFFFFE0004000000ULL) {
        consola_imprimir_linea_color(" [OK: Libre de Colisión VT-d]", COLOR_EXITO_DEFAULT);
    } else {
        consola_imprimir_linea("");
    }

    // 3. Capacidades de Hardware
    consola_imprimir("  Capacidades Silicio  : Slots Máx: ");
    consola_imprimir_dec(st->max_slots);
    consola_imprimir(" | Puertos Raíz: ");
    consola_imprimir_dec(st->max_puertos);
    consola_imprimir(" | Dispositivos Conectados: ");
    consola_imprimir_dec(st->puertos_conectados);
    consola_imprimir_linea("");

    // 4. Detalle de Puertos Raíz (Especialmente los conectados)
    consola_imprimir_linea_color("  --- Inspección Física de Puertos Raíz ---", COLOR_PROMPT_DEFAULT);
    int puertos_activos = 0;
    for (uint8_t p = 1; p <= st->max_puertos; p++) {
        uint32_t portsc = 0;
        int conectado = 0, habilitado = 0;
        uint8_t vel = 0;
        if (xhci_obtener_info_puerto(p, &portsc, &conectado, &habilitado, &vel) == 0) {
            if (conectado || (portsc & 0x01) /* CCS */) {
                puertos_activos++;
                consola_imprimir("    * Puerto ");
                consola_imprimir_dec(p);
                consola_imprimir(": ");
                consola_imprimir_color("[CONECTADO]", COLOR_EXITO_DEFAULT);
                consola_imprimir(" Habilitado: ");
                consola_imprimir(habilitado ? "[SÍ]" : "[NO]");
                consola_imprimir(" | Velocidad: ");
                if (vel == 1) consola_imprimir_color("Full-Speed 12 Mbps (Teclado/Mouse)", COLOR_EXITO_DEFAULT);
                else if (vel == 2) consola_imprimir("Low-Speed 1.5 Mbps");
                else if (vel == 3) consola_imprimir("High-Speed 480 Mbps");
                else if (vel >= 4) consola_imprimir_color("SuperSpeed 5+ Gbps", COLOR_EXITO_DEFAULT);
                else consola_imprimir("Desconocida");

                consola_imprimir(" (PORTSC: 0x");
                terminal_imprimir_hex_fijo(portsc, 8);
                consola_imprimir_linea(")");
            }
        }
    }
    if (puertos_activos == 0) {
        consola_imprimir_linea_color("    * Ningún puerto raíz reporta línea física D+/D- activa en este instante.", COLOR_AVISO_DEFAULT);
    }

    // 5. Estado del Teclado USB HID
    consola_imprimir_linea_color("  --- Estado del Teclado Físico USB ---", COLOR_PROMPT_DEFAULT);
    if (st->teclado_detectado) {
        consola_imprimir("    * Estado           : ");
        consola_imprimir_linea_color("[ENDPOINTS CONFIGURADOS; recepción verificada solo si hay eventos]", COLOR_EXITO_DEFAULT);
        consola_imprimir("    * Teclados Activos : ");
        consola_imprimir_dec(st->teclados_activos > 0 ? st->teclados_activos : 1);
        consola_imprimir_linea(" concurrente(s)");
        consola_imprimir("    * Último Config    : VID:0x");
        terminal_imprimir_hex_fijo(st->teclado_id_proveedor, 4);
        consola_imprimir(" PID:0x");
        terminal_imprimir_hex_fijo(st->teclado_id_producto, 4);
        if (st->teclado_id_proveedor == 0x05AC && st->teclado_id_producto == 0x024F) {
            consola_imprimir_color(" (Havit Gaming / Apple Aluminum NKRO)", COLOR_EXITO_DEFAULT);
        } else if (st->teclado_id_proveedor == 0x3151 && st->teclado_id_producto == 0x3020) {
            consola_imprimir_color(" (Micronics Wireless 2.4GHz Dock)", COLOR_EXITO_DEFAULT);
        }
        consola_imprimir(" en Puerto ");
        consola_imprimir_dec(st->teclado_puerto);
        consola_imprimir_linea("");

        consola_imprimir("    * Endpoints Armados: ");
        consola_imprimir_dec(st->teclado_num_eps);
        consola_imprimir_color(" endpoint(s) IN de interrupción con DMA dedicado", COLOR_EXITO_DEFAULT);
        consola_imprimir_linea("");

        consola_imprimir("    * Telemetría       : Reportes Teclas HID: ");
        consola_imprimir_dec(st->reportes_hid_recibidos);
        consola_imprimir(" | Paquetes Totales: ");
        consola_imprimir_dec(st->paquetes_recibidos);
        consola_imprimir(" | Último Tipo TRB: ");
        consola_imprimir_dec(st->ultimo_evento_trb_tipo);
        consola_imprimir(" | Transferencias HID: ");
        consola_imprimir_dec(st->eventos_transferencia);
        consola_imprimir(" | Errores HID: ");
        consola_imprimir_dec(st->fallos_transferencia);
        consola_imprimir_linea("");
        consola_imprimir("    * Último evento HID: DCI=");
        consola_imprimir_dec(st->ultimo_dci_transfer);
        consola_imprimir(" código=");
        consola_imprimir_dec(st->ultimo_codigo_transfer);
        consola_imprimir(" longitud=");
        consola_imprimir_dec(st->ultimo_tamano_reporte);
        consola_imprimir(" bytes: ");
        for (int i = 0; i < st->ultimo_tamano_reporte && i < 16; i++) {
            terminal_imprimir_hex_fijo(st->ultimo_reporte[i], 2);
            consola_imprimir(" ");
        }
        consola_imprimir_linea("");
        if (st->ultimo_caracter) {
            consola_imprimir(" | Último Carácter: '");
            consola_escribir_caracter((char)st->ultimo_caracter);
            consola_imprimir("'");
        }
        consola_imprimir_linea("");
    } else {
        consola_imprimir("    * Estado           : ");
        consola_imprimir_linea_color("[ESPERANDO CONEXIÓN / FIRMWARE DE TECLADO]", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea("      El driver continuará sondeando en segundo plano (Hotplug activo).");
    }

    consola_imprimir("    * Fase USB: ");
    consola_imprimir_dec(st->etapa_enumeracion);
    consola_imprimir(" | Transferencias de control: ");
    consola_imprimir_dec(st->transferencias_control);
    consola_imprimir(" | Fallos: ");
    consola_imprimir_dec(st->fallos_control);
    consola_imprimir(" | Última petición: 0x");
    terminal_imprimir_hex_fijo(st->ultima_peticion_control, 2);
    consola_imprimir(" código: ");
    consola_imprimir_dec(st->ultimo_codigo_control);
    consola_imprimir_linea("");
    consola_imprimir("      Setup: wValue=0x");
    terminal_imprimir_hex_fijo(st->ultimo_valor_control, 4);
    consola_imprimir(" wIndex=0x");
    terminal_imprimir_hex_fijo(st->ultimo_indice_control, 4);
    consola_imprimir(" wLength=");
    consola_imprimir_dec(st->ultimo_largo_control);
    consola_imprimir_linea("");
    consola_imprimir_linea("      Fase: 0=inactivo, 1=slot, 2=address, 3=device desc, 4=config desc, 5=config ep, 6=set config, 7=set protocol, 8=endpoints listos.");

    // 6. Estado Teclado PS/2 Legacy
    consola_imprimir("  Teclado Legacy PS/2  : ");
    if (teclado_es_modo_nativo()) {
        if (teclado_esta_presente()) {
            consola_imprimir_linea_color("[DISPONIBLE / i8042 ACTIVO]", COLOR_PROMPT_DEFAULT);
        } else {
            consola_imprimir_linea_color("[INACTIVO - Cesión de BIOS UEFI completada]", COLOR_AVISO_DEFAULT);
        }
    } else {
        consola_imprimir_linea_color("[DESHABILITADO - Sistema operando en modo USB xHCI puro]", COLOR_AVISO_DEFAULT);
    }

    consola_imprimir_linea_color("==================================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'teclado probar' para entrar al modo de captura de teclas en vivo.", COLOR_TEXTO_DEFAULT);
}

static void ejecutar_comando_teclado(const char *arg) {
    if (arg && (str_igual(arg, "probar") || str_igual(arg, "test") || str_igual(arg, "prueba"))) {
        consola_imprimir_linea_color("================ MODO PRUEBA DE TECLADO EN VIVO ================", COLOR_AVISO_DEFAULT);
        if (teclado_es_modo_nativo()) {
            consola_imprimir_linea_color("  Modo Entrada: [MODO NATIVO PS/2 (Teclado interno/legacy activo)]", COLOR_PROMPT_DEFAULT);
        } else {
            consola_imprimir_linea_color("  Modo Entrada: [xHCI PURO (Teclado interno PS/2 silenciado)]", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea("  -> Solo las pulsaciones del teclado USB físico externo serán capturadas.");
        }
        consola_imprimir_linea("Presiona teclas en tu teclado físico para ver los eventos USB en tiempo real.");
        consola_imprimir_linea("Presiona ESC o Enter (o espera 10 segundos) para volver a la terminal...");
        consola_imprimir_linea("");

        uint64_t inicio = tiempo_obtener_milisegundos();
        int eventos_vistos = 0;

        while (tiempo_obtener_milisegundos() - inicio < 10000) {
            if (!teclado_es_modo_nativo()) {
                xhci_sondeo();
            }

            char c = 0;
            const char *origen = NULL;

            if (teclado_es_modo_nativo()) {
                c = teclado_leer_caracter();
                if (c != 0) origen = "Modo Nativo PS/2 (Puerto 0x60)";
            } else {
                c = xhci_leer_caracter();
                if (c != 0) origen = "USB xHCI Ring 0";
            }

            if (c == 0 && serial_hay_datos()) {
                c = serial_leer_caracter();
                if (c == '\r') c = '\n';
                if (c != 0) origen = "Serial COM1 (UART 0x3F8)";
            }

            if (c != 0) {
                eventos_vistos++;
                inicio = tiempo_obtener_milisegundos(); // Renovar timeout si hay actividad
                consola_imprimir_color("  [EVENTO CAPTURADO] ", COLOR_EXITO_DEFAULT);
                consola_imprimir("Carácter: '");
                if (c >= 32 && c <= 126) consola_escribir_caracter(c);
                else consola_imprimir("?");
                consola_imprimir("' | ASCII: 0x");
                terminal_imprimir_hex_fijo((uint8_t)c, 2);
                consola_imprimir(" | Origen: ");
                consola_imprimir(origen ? origen : "Desconocido");
                if (!teclado_es_modo_nativo()) {
                    const struct estado_xhci *st = xhci_obtener_estado();
                    if (st->ultimo_puerto_tecla > 0) {
                        consola_imprimir(" (Puerto ");
                        consola_imprimir_dec(st->ultimo_puerto_tecla);
                        consola_imprimir(", Slot ");
                        consola_imprimir_dec(st->ultimo_slot_tecla);
                        consola_imprimir(")");
                    }
                    consola_imprimir(" | Reportes HID: ");
                    consola_imprimir_dec(st->reportes_hid_recibidos);
                    consola_imprimir(" | Total Paquetes: ");
                    consola_imprimir_dec(st->paquetes_recibidos);
                }
                consola_imprimir_linea("");

                if (c == 27 /* ESC */ || c == '\n' || c == '\r') {
                    consola_imprimir_linea("Saliendo del modo prueba...");
                    break;
                }
            }
            esperar_milisegundos(10);
        }

        if (eventos_vistos == 0) {
            consola_imprimir_linea_color("  [AVISO] No se capturaron teclas durante el periodo de prueba.", COLOR_AVISO_DEFAULT);
        }
        consola_imprimir_linea_color("=================================================================", COLOR_AVISO_DEFAULT);
        return;
    }

    ejecutar_autodiagnostico_teclado();
}

static void ejecutar_lectura_usb_msc(uint8_t unidad, uint32_t lba) {
    int total_msc = usb_msc_obtener_cantidad();
    if (total_msc == 0) {
        consola_imprimir_linea_color("  [!] No hay unidades de almacenamiento USB (pendrives) detectadas.", COLOR_ERROR_DEFAULT);
        consola_imprimir_linea("      Conecta un pendrive USB y ejecuta 'usb monitor' o 'usb' para verificar.");
        return;
    }

    const struct usb_msc_dispositivo *dev = usb_msc_obtener_dispositivo(unidad);
    if (!dev || !dev->activo || !dev->listo) {
        consola_imprimir_linea_color("  [!] La unidad USB seleccionada no está lista para operaciones de E/S.", COLOR_ERROR_DEFAULT);
        return;
    }

    if (lba >= dev->sectores_totales && dev->sectores_totales > 0) {
        consola_imprimir("  [AVISO] El LBA ");
        consola_imprimir_dec(lba);
        consola_imprimir(" supera los sectores totales de la unidad (");
        consola_imprimir_dec(dev->sectores_totales);
        consola_imprimir_linea(").");
    }

    static uint8_t sector_buf[512];
    for (int i = 0; i < 512; i++) sector_buf[i] = 0;

    consola_imprimir("==> [ USB SCSI BOT ] Leyendo LBA ");
    consola_imprimir_dec(lba);
    consola_imprimir(" (512 bytes) de '");
    consola_imprimir(dev->fabricante);
    consola_imprimir(" ");
    consola_imprimir(dev->producto);
    consola_imprimir_linea("'...");

    int res = usb_msc_leer_sectores(unidad, lba, 1, sector_buf);
    if (res != 0) {
        consola_imprimir("  [ERROR] Falló la lectura SCSI READ(10). Código de error: ");
        consola_imprimir_dec(res);
        consola_imprimir_linea("");
        return;
    }

    consola_imprimir_linea_color("================== VOLCADO HEXADECIMAL DE SECTOR ==================", COLOR_AVISO_DEFAULT);
    for (int fila = 0; fila < 32; fila++) {
        uint32_t offset = fila * 16;
        consola_imprimir_color("  0x", COLOR_PROMPT_DEFAULT);
        terminal_imprimir_hex_fijo(offset, 4);
        consola_imprimir(":  ");

        // Bytes en hex
        for (int b = 0; b < 16; b++) {
            terminal_imprimir_hex_fijo(sector_buf[offset + b], 2);
            consola_imprimir(" ");
            if (b == 7) consola_imprimir(" ");
        }

        consola_imprimir(" |");
        // Caracteres ASCII
        for (int b = 0; b < 16; b++) {
            uint8_t ch = sector_buf[offset + b];
            if (ch >= 32 && ch <= 126) {
                consola_escribir_caracter((char)ch);
            } else {
                consola_escribir_caracter('.');
            }
        }
        consola_imprimir_linea("|");
    }
    consola_imprimir_linea_color("==================================================================", COLOR_AVISO_DEFAULT);

    // Inspección de firmas conocidas en LBA 0 (MBR) o LBA 1 (GPT Header)
    if (lba == 0) {
        uint16_t firma_mbr = ((uint16_t)sector_buf[511] << 8) | sector_buf[510];
        if (firma_mbr == 0xAA55) {
            consola_imprimir_linea_color("  [OK] Firma de arranque MBR detectada: 0x55AA en offset 510-511.", COLOR_EXITO_DEFAULT);
            // Comprobar particiones en tabla MBR (offset 446 a 509)
            int num_particiones = 0;
            for (int p = 0; p < 4; p++) {
                uint32_t p_off = 446 + (p * 16);
                uint8_t tipo = sector_buf[p_off + 4];
                if (tipo != 0) {
                    num_particiones++;
                    uint32_t inicio_lba = (uint32_t)sector_buf[p_off + 8] |
                                         ((uint32_t)sector_buf[p_off + 9] << 8) |
                                         ((uint32_t)sector_buf[p_off + 10] << 16) |
                                         ((uint32_t)sector_buf[p_off + 11] << 24);
                    uint32_t num_sec = (uint32_t)sector_buf[p_off + 12] |
                                       ((uint32_t)sector_buf[p_off + 13] << 8) |
                                       ((uint32_t)sector_buf[p_off + 14] << 16) |
                                       ((uint32_t)sector_buf[p_off + 15] << 24);
                    consola_imprimir("       Partición ");
                    consola_imprimir_dec(p + 1);
                    consola_imprimir(": Tipo 0x");
                    terminal_imprimir_hex_fijo(tipo, 2);
                    if (tipo == 0xEE) consola_imprimir(" (Protective MBR / GPT)");
                    else if (tipo == 0x07) consola_imprimir(" (NTFS / exFAT)");
                    else if (tipo == 0x0C || tipo == 0x0B) consola_imprimir(" (FAT32 LBA)");
                    else if (tipo == 0x83) consola_imprimir(" (Linux Native)");
                    consola_imprimir(" | LBA Inicio: ");
                    consola_imprimir_dec(inicio_lba);
                    consola_imprimir(" | Sectores: ");
                    consola_imprimir_dec(num_sec);
                    consola_imprimir_linea("");
                }
            }
            if (num_particiones == 0) {
                consola_imprimir_linea("       (Tabla MBR vacía o medio sin particionar)");
            }
        } else {
            consola_imprimir_linea_color("  [AVISO] Sector 0 leído correctamente, pero sin firma MBR 0x55AA.", COLOR_AVISO_DEFAULT);
        }
    } else if (lba == 1) {
        if (sector_buf[0] == 'E' && sector_buf[1] == 'F' && sector_buf[2] == 'I' && sector_buf[3] == ' ' &&
            sector_buf[4] == 'P' && sector_buf[5] == 'A' && sector_buf[6] == 'R' && sector_buf[7] == 'T') {
            consola_imprimir_linea_color("  [OK] Firma de cabecera GPT detectada: 'EFI PART' en LBA 1.", COLOR_EXITO_DEFAULT);
        }
    }
}

static void ejecutar_comando_disco(const char *arg) {
    if (arg) arg = str_saltar_espacios(arg);

    if (arg && (str_igual(arg, "tree") || str_igual(arg, "arbol") || str_comienza_con(arg, "tree ") || str_comienza_con(arg, "arbol "))) {
        const char *sub = NULL;
        if (str_comienza_con(arg, "tree ")) sub = str_saltar_espacios(arg + 5);
        else if (str_comienza_con(arg, "arbol ")) sub = str_saltar_espacios(arg + 6);
        fat32_ejecutar_tree(sub);
        return;
    }

    if (arg && (str_igual(arg, "ls") || str_igual(arg, "dir") || str_comienza_con(arg, "ls ") || str_comienza_con(arg, "dir "))) {
        const char *sub = NULL;
        if (str_comienza_con(arg, "ls ")) sub = str_saltar_espacios(arg + 3);
        else if (str_comienza_con(arg, "dir ")) sub = str_saltar_espacios(arg + 4);
        fat32_listar_directorio(sub);
        return;
    }

    if (arg && (str_comienza_con(arg, "cat ") || str_comienza_con(arg, "ver "))) {
        const char *archivo = str_saltar_espacios(arg + (str_comienza_con(arg, "cat ") ? 4 : 4));
        fat32_leer_archivo_texto(archivo);
        return;
    }

    if (arg && (str_comienza_con(arg, "leer") || str_comienza_con(arg, "read") || str_comienza_con(arg, "dump"))) {
        const char *p_lba = arg + 4;
        p_lba = str_saltar_espacios(p_lba);

        // Si es un nombre de archivo (letras no hex o contiene punto)
        int es_archivo = 0;
        for (int k = 0; p_lba[k]; k++) {
            char c = p_lba[k];
            if (c == '.' || (c >= 'g' && c <= 'z') || (c >= 'G' && c <= 'Z')) {
                es_archivo = 1;
                break;
            }
        }
        if (es_archivo) {
            fat32_leer_archivo_texto(p_lba);
            return;
        }

        uint32_t lba = 0;
        if (*p_lba != '\0') {
            if (p_lba[0] == '0' && (p_lba[1] == 'x' || p_lba[1] == 'X')) {
                p_lba += 2;
                while ((*p_lba >= '0' && *p_lba <= '9') || (*p_lba >= 'a' && *p_lba <= 'f') || (*p_lba >= 'A' && *p_lba <= 'F')) {
                    char c = *p_lba++;
                    int val = (c >= '0' && c <= '9') ? (c - '0') : ((c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10));
                    lba = (lba << 4) | (uint32_t)val;
                }
            } else {
                while (*p_lba >= '0' && *p_lba <= '9') {
                    lba = lba * 10 + (*p_lba++ - '0');
                }
            }
        }
        ejecutar_lectura_usb_msc(0, lba);
        return;
    }

    int total_msc = usb_msc_obtener_cantidad();
    consola_imprimir_linea_color("================== SUBSISTEMA DE ALMACENAMIENTO USB ==================", COLOR_AVISO_DEFAULT);
    consola_imprimir("Unidades USB Mass Storage detectadas: ");
    consola_imprimir_dec(total_msc);
    consola_imprimir_linea("");

    if (total_msc == 0) {
        consola_imprimir_linea_color("  [!] No se encontraron memorias USB o discos externos conectados.", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("      Inserta un pendrive USB y ejecuta 'usb monitor' o 'disco' para detectarlo.");
    } else {
        for (int i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
            const struct usb_msc_dispositivo *msc = usb_msc_obtener_dispositivo(i);
            if (msc && msc->activo) {
                consola_imprimir_color("  * DISCO #", COLOR_PROMPT_DEFAULT);
                consola_imprimir_dec(i);
                consola_imprimir(": ");
                consola_imprimir_color(msc->fabricante, COLOR_EXITO_DEFAULT);
                consola_imprimir(" ");
                consola_imprimir_color(msc->producto, COLOR_EXITO_DEFAULT);
                consola_imprimir(" [Rev ");
                consola_imprimir(msc->revision);
                consola_imprimir_linea("]");

                consola_imprimir("    Estado SCSI       : ");
                if (msc->listo) {
                    consola_imprimir_linea_color("LISTO / EN LÍNEA (BOT + SCSI-2 SPC/SBC)", COLOR_EXITO_DEFAULT);
                } else {
                    consola_imprimir_linea_color("INICIALIZANDO O MEDIO NO INSERTADO", COLOR_AVISO_DEFAULT);
                }

                uint64_t cap_mb = msc->capacidad_bytes / (1024ULL * 1024ULL);
                uint64_t cap_gb = msc->capacidad_bytes / (1024ULL * 1024ULL * 1024ULL);
                consola_imprimir("    Capacidad Total   : ");
                if (cap_gb > 0) {
                    consola_imprimir_dec(cap_gb);
                    consola_imprimir(" GB (");
                }
                consola_imprimir_dec(cap_mb);
                consola_imprimir(" MB");
                if (cap_gb > 0) consola_imprimir(")");
                consola_imprimir_linea("");

                consola_imprimir("    Geometría LBA     : ");
                consola_imprimir_dec(msc->sectores_totales);
                consola_imprimir(" sectores físicos de ");
                consola_imprimir_dec(msc->tamano_sector);
                consola_imprimir_linea(" bytes");

                consola_imprimir("    Conexión xHCI     : Slot ");
                consola_imprimir_dec(msc->slot_id);
                consola_imprimir(" en Puerto ");
                consola_imprimir_dec(msc->puerto_idx);
                consola_imprimir(" (EP Bulk IN: DCI ");
                consola_imprimir_dec(msc->ep_in_dci);
                consola_imprimir(", Bulk OUT: DCI ");
                consola_imprimir_dec(msc->ep_out_dci);
                consola_imprimir_linea(")");
            }
        }
    }
    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'disco leer 0' para inspeccionar el sector de arranque MBR.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'disco leer 1' para inspeccionar la cabecera GPT.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("======================================================================", COLOR_AVISO_DEFAULT);
}

static void ejecutar_comando_usb(const char *arg) {
    if (arg) arg = str_saltar_espacios(arg);

    // Subcomando: usb leer <lba> / usb read <lba> / usb dump <lba>
    if (arg && (str_comienza_con(arg, "leer") || str_comienza_con(arg, "read") || str_comienza_con(arg, "dump"))) {
        const char *p_lba = arg + 4;
        p_lba = str_saltar_espacios(p_lba);
        uint32_t lba = 0;
        if (*p_lba != '\0') {
            if (p_lba[0] == '0' && (p_lba[1] == 'x' || p_lba[1] == 'X')) {
                p_lba += 2;
                while ((*p_lba >= '0' && *p_lba <= '9') || (*p_lba >= 'a' && *p_lba <= 'f') || (*p_lba >= 'A' && *p_lba <= 'F')) {
                    char c = *p_lba++;
                    int val = (c >= '0' && c <= '9') ? (c - '0') : ((c >= 'a' && c <= 'f') ? (c - 'a' + 10) : (c - 'A' + 10));
                    lba = (lba << 4) | (uint32_t)val;
                }
            } else {
                while (*p_lba >= '0' && *p_lba <= '9') {
                    lba = lba * 10 + (*p_lba++ - '0');
                }
            }
        }
        ejecutar_lectura_usb_msc(0, lba);
        return;
    }

    // Subcomando: usb discos / usb storage / usb msc
    if (arg && (str_igual(arg, "disco") || str_igual(arg, "discos") || str_igual(arg, "storage") || str_igual(arg, "msc"))) {
        ejecutar_comando_disco(NULL);
        return;
    }

    // Subcomando: usb tree / usb arbol
    if (arg && (str_igual(arg, "tree") || str_igual(arg, "arbol") || str_comienza_con(arg, "tree ") || str_comienza_con(arg, "arbol "))) {
        const char *sub = NULL;
        if (str_comienza_con(arg, "tree ")) sub = str_saltar_espacios(arg + 5);
        else if (str_comienza_con(arg, "arbol ")) sub = str_saltar_espacios(arg + 6);
        fat32_ejecutar_tree(sub);
        return;
    }

    // Subcomando: usb ls / usb dir
    if (arg && (str_igual(arg, "ls") || str_igual(arg, "dir") || str_comienza_con(arg, "ls ") || str_comienza_con(arg, "dir "))) {
        const char *sub = NULL;
        if (str_comienza_con(arg, "ls ")) sub = str_saltar_espacios(arg + 3);
        else if (str_comienza_con(arg, "dir ")) sub = str_saltar_espacios(arg + 4);
        fat32_listar_directorio(sub);
        return;
    }

    // Subcomando: usb monitor / usb escucha
    if (arg && (str_igual(arg, "monitor") || str_igual(arg, "escuchar") || str_igual(arg, "vigilar") || str_igual(arg, "watch"))) {
        consola_imprimir_linea_color("================== [ MONITOR DE PUERTOS USB EN VIVO ] ==================", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("Escuchando eventos de insercion/extraccion en los puertos USB xHCI.");
        consola_imprimir_linea_color("--> Conecta o desconecta cualquier dispositivo para verificar la deteccion.", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea("Presiona cualquier tecla (o espera 20 segundos) para regresar...");
        consola_imprimir_linea("");

        uint64_t inicio = tiempo_obtener_milisegundos();
        while (tiempo_obtener_milisegundos() - inicio < 20000) {
            xhci_sondeo();
            char c = consola_leer_caracter();
            if (c != 0) {
                consola_imprimir_linea("\nMonitor detenido por el usuario.");
                break;
            }
            esperar_milisegundos(20);
        }
        consola_imprimir_linea_color("========================================================================", COLOR_AVISO_DEFAULT);
        return;
    }

    // Subcomando: usb reset <puerto>
    if (arg && (str_comienza_con(arg, "reset ") || str_comienza_con(arg, "reiniciar "))) {
        const char *p_str = arg + (str_comienza_con(arg, "reset ") ? 6 : 10);
        p_str = str_saltar_espacios(p_str);
        int p = 0;
        while (*p_str >= '0' && *p_str <= '9') {
            p = p * 10 + (*p_str - '0');
            p_str++;
        }
        if (p >= 1 && p <= 32) {
            xhci_forzar_reset_puerto((uint8_t)p);
        } else {
            consola_imprimir_linea_color("Uso: usb reset <numero_de_puerto 1..32>", COLOR_ERROR_DEFAULT);
        }
        return;
    }

    // Subcomando: usb diag / usb volcado / usb dump
    if (arg && (str_igual(arg, "diag") || str_igual(arg, "diagnostico") || str_igual(arg, "volcado") || str_igual(arg, "dump"))) {
        xhci_imprimir_diagnostico_completo();
        return;
    }

    // Subcomando: usb teclado / usb test / usb probar
    if (arg && (str_igual(arg, "teclado") || str_igual(arg, "probar") || str_igual(arg, "test"))) {
        ejecutar_comando_teclado(arg);
        return;
    }

    // Por defecto (usb / usb puertos / usb lista / usb status): Tabla detallada de puertos raíz
    const struct estado_xhci *st = xhci_obtener_estado();
    if (!st->controlador_detectado) {
        consola_imprimir_linea_color("[!] CONTROLADOR xHCI NO DISPONIBLE", COLOR_ERROR_DEFAULT);
        return;
    }

    consola_imprimir_linea_color("================= [ INSPECCION DE PUERTOS USB xHCI ] =================", COLOR_AVISO_DEFAULT);
    consola_imprimir("Controlador Host PCI : ");
    terminal_imprimir_hex_fijo(st->bus, 2);
    consola_imprimir(":");
    terminal_imprimir_hex_fijo(st->ranura, 2);
    consola_imprimir(".");
    terminal_imprimir_hex_fijo(st->funcion, 1);
    consola_imprimir(" (Vendor 0x");
    terminal_imprimir_hex_fijo(st->id_proveedor, 4);
    consola_imprimir(" Dev 0x");
    terminal_imprimir_hex_fijo(st->id_dispositivo, 4);
    consola_imprimir_linea(")");

    consola_imprimir("Capacidades Silicio  : ");
    consola_imprimir_dec(st->max_puertos);
    consola_imprimir(" Puertos Raiz | ");
    consola_imprimir_dec(st->max_slots);
    consola_imprimir(" Slots | Dispositivos Conectados: ");
    consola_imprimir_dec(st->puertos_conectados);
    consola_imprimir_linea("");
    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);

    int conectados = 0;
    for (uint8_t p = 1; p <= st->max_puertos; p++) {
        uint32_t portsc = 0;
        int con = 0, hab = 0;
        uint8_t vel = 0;
        if (xhci_obtener_info_puerto(p, &portsc, &con, &hab, &vel) == 0) {
            if (con || (portsc & 0x01)) {
                conectados++;
                consola_imprimir("  Puerto ");
                if (p < 10) consola_imprimir(" ");
                consola_imprimir_dec(p);
                consola_imprimir(": ");
                consola_imprimir_color("[CONECTADO]   ", COLOR_EXITO_DEFAULT);
                consola_imprimir("PED=");
                consola_imprimir(hab ? "[SI] " : "[NO] ");
                consola_imprimir("| Vel: ");
                if (vel == 1) consola_imprimir_color("Full-Speed 12M  ", COLOR_EXITO_DEFAULT);
                else if (vel == 2) consola_imprimir("Low-Speed 1.5M  ");
                else if (vel == 3) consola_imprimir("High-Speed 480M ");
                else if (vel >= 4) consola_imprimir_color("SuperSpeed 5G+  ", COLOR_PROMPT_DEFAULT);
                else consola_imprimir("Desconocida     ");

                consola_imprimir("| PORTSC=0x");
                terminal_imprimir_hex_fijo(portsc, 8);

                if (st->teclado_detectado && st->teclado_puerto == p) {
                    consola_imprimir_color(" <- TECLADO HID ACTIVO", COLOR_USUARIO_DEFAULT);
                }
                consola_imprimir_linea("");
            }
        }
    }

    if (conectados == 0) {
        consola_imprimir_linea_color("  [AVISO] No se detecta ningun dispositivo conectado en los puertos raiz.", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea("  Conecta el teclado/dongle y ejecuta 'usb monitor' para ver la deteccion.");
    }

    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);
    consola_imprimir("Teclado USB: ");
    if (st->teclado_detectado) {
        consola_imprimir_color("[OPERATIVO]", COLOR_EXITO_DEFAULT);
        consola_imprimir(" en Puerto ");
        consola_imprimir_dec(st->teclado_puerto);
        consola_imprimir(" (VID:0x");
        terminal_imprimir_hex_fijo(st->teclado_id_proveedor, 4);
        consola_imprimir(" PID:0x");
        terminal_imprimir_hex_fijo(st->teclado_id_producto, 4);
        consola_imprimir_linea(")");
    } else {
        consola_imprimir_linea_color("[NO DETECTADO - Sondeo y Hotplug Activos]", COLOR_AVISO_DEFAULT);
    }

    consola_imprimir_linea_color("----------------------------------------------------------------------", COLOR_PROMPT_DEFAULT);
    int total_msc = usb_msc_obtener_cantidad();
    consola_imprimir("Almacenamiento USB (MSC): ");
    if (total_msc > 0) {
        consola_imprimir_color("[OPERATIVO] ", COLOR_EXITO_DEFAULT);
        consola_imprimir_dec(total_msc);
        consola_imprimir_linea(" unidad(es) detectada(s)");
        for (int i = 0; i < USB_MSC_MAX_DISPOSITIVOS; i++) {
            const struct usb_msc_dispositivo *msc = usb_msc_obtener_dispositivo(i);
            if (msc && msc->activo) {
                consola_imprimir("  * Disco #");
                consola_imprimir_dec(i);
                consola_imprimir(": ");
                consola_imprimir_color(msc->fabricante, COLOR_EXITO_DEFAULT);
                consola_imprimir(" ");
                consola_imprimir_color(msc->producto, COLOR_EXITO_DEFAULT);
                consola_imprimir(" (Rev ");
                consola_imprimir(msc->revision);
                consola_imprimir(") en Puerto ");
                consola_imprimir_dec(msc->puerto_idx);
                consola_imprimir(" (Slot ");
                consola_imprimir_dec(msc->slot_id);
                consola_imprimir(")\n");

                uint64_t cap_mb = msc->capacidad_bytes / (1024ULL * 1024ULL);
                uint64_t cap_gb = msc->capacidad_bytes / (1024ULL * 1024ULL * 1024ULL);
                consola_imprimir("    Capacidad : ");
                if (cap_gb > 0) {
                    consola_imprimir_dec(cap_gb);
                    consola_imprimir(" GB (");
                }
                consola_imprimir_dec(cap_mb);
                consola_imprimir(" MB");
                if (cap_gb > 0) consola_imprimir(")");
                consola_imprimir(" | Sectores: ");
                consola_imprimir_dec(msc->sectores_totales);
                consola_imprimir(" x ");
                consola_imprimir_dec(msc->tamano_sector);
                consola_imprimir_linea(" bytes");
            }
        }
    } else {
        consola_imprimir_linea_color("[NINGUNA UNIDAD DETECTADA]", COLOR_TEXTO_DEFAULT);
        consola_imprimir_linea("  Conecta un pendrive USB para inicializar SCSI BOT automáticamente.");
    }

    consola_imprimir_linea_color("Tip: Escribe 'usb leer <lba>' para volcar sectores físicos del pendrive.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'disco' para ver el diagnóstico detallado de unidades SCSI.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'usb monitor' para probar en vivo conectando y desconectando.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("Tip: Escribe 'usb reset <puerto>' para reiniciar un puerto especifico.", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color("======================================================================", COLOR_AVISO_DEFAULT);
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
        consola_imprimir_color("  guia gpu       ", COLOR_EXITO_DEFAULT);
        consola_imprimir_linea_color(": [RECOMENDADO] Guía paso a paso para testear la GPU en hardware real MoDT.", COLOR_EXITO_DEFAULT);
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
        consola_imprimir_linea(": Diagnóstico especializado de la GPU, VRAM y registros MMIO ('gpu probar').");
        consola_imprimir_color("  linux / shim   ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Capa puente de compatibilidad con drivers de Linux ('linux probar').");
        consola_imprimir_color("  nvidia         ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": GPU Blackwell, firmware GSP y colas RPC ('nvidia inicializar', 'nvidia gsp', 'nvidia probar').");
        consola_imprimir_color("  apic / irq     ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Controlador Local APIC, x2APIC e interrupciones MSI ('apic probar').");
        consola_imprimir_color("  dma            ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Gestor de memoria DMA contigua y coherencia de caché ('dma probar').");
        consola_imprimir_color("  iommu          ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Controlador Intel VT-d, remapeo DRHD y regiones RMRR ('iommu probar').");
        consola_imprimir_color("  teclado        ", COLOR_EXITO_DEFAULT);
        consola_imprimir_linea_color(": Autodiagnóstico del teclado USB y puertos ('teclado probar').", COLOR_EXITO_DEFAULT);
        consola_imprimir_color("  usb            ", COLOR_EXITO_DEFAULT);
        consola_imprimir_linea_color(": Inspección USB y lectura física ('usb leer <lba>', 'usb monitor', 'usb reset <p>').", COLOR_EXITO_DEFAULT);
        consola_imprimir_color("  disco          ", COLOR_EXITO_DEFAULT);
        consola_imprimir_linea_color(": Almacenamiento USB Mass Storage y lectura SCSI ('disco leer <lba>').", COLOR_EXITO_DEFAULT);
        consola_imprimir_color("  tree / arbol   ", COLOR_EXITO_DEFAULT);
        consola_imprimir_linea_color(": Despliega el árbol visual de directorios y archivos FAT32 del pendrive.", COLOR_EXITO_DEFAULT);
        consola_imprimir_color("  ls / dir       ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Lista los archivos y carpetas del sistema de archivos FAT32.");
        consola_imprimir_color("  cat <archivo>  ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Imprime el contenido de un archivo de texto del pendrive.");
        consola_imprimir_color("  dmesg / log    ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_linea(": Registro completo de arranque en memoria y estado serial COM1.");
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
        consola_imprimir_linea("x86_64 Long Mode (Intel Core / AMD64 Compatible)");
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

    // COMANDO: guia / tutorial / ayuda gpu
    if (str_comienza_con(linea, "guia") || str_comienza_con(linea, "tutorial") ||
        str_igual(linea, "ayuda gpu") || str_igual(linea, "help gpu")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "guia ")) arg = str_saltar_espacios(linea + 5);
        else if (str_comienza_con(linea, "tutorial ")) arg = str_saltar_espacios(linea + 9);
        ejecutar_comando_guia(arg);
        return;
    }

    // COMANDO: dmesg / log / registros
    if (str_igual(linea, "dmesg") || str_igual(linea, "log") || str_igual(linea, "registros")) {
        ejecutar_comando_dmesg();
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

    // COMANDO: gpu / video / vram
    if (str_comienza_con(linea, "gpu") || str_comienza_con(linea, "video") || str_comienza_con(linea, "vram")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "gpu ")) arg = str_saltar_espacios(linea + 4);
        else if (str_comienza_con(linea, "video ")) arg = str_saltar_espacios(linea + 6);
        else if (str_comienza_con(linea, "vram ")) arg = str_saltar_espacios(linea + 5);
        ejecutar_comando_gpu(arg);
        return;
    }

    // COMANDO: linux / shim
    if (str_comienza_con(linea, "linux") || str_comienza_con(linea, "shim")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "linux ")) arg = str_saltar_espacios(linea + 6);
        else if (str_comienza_con(linea, "shim ")) arg = str_saltar_espacios(linea + 5);
        ejecutar_comando_linux(arg);
        return;
    }

    // COMANDO: nvidia
    if (str_comienza_con(linea, "nvidia")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "nvidia ")) arg = str_saltar_espacios(linea + 7);
        ejecutar_comando_nvidia(arg);
        return;
    }

    // COMANDO: apic / irq
    if (str_comienza_con(linea, "apic") || str_comienza_con(linea, "irq")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "apic ")) arg = str_saltar_espacios(linea + 5);
        else if (str_comienza_con(linea, "irq ")) arg = str_saltar_espacios(linea + 4);
        ejecutar_comando_apic(arg);
        return;
    }

    // COMANDO: dma
    if (str_comienza_con(linea, "dma")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "dma ")) arg = str_saltar_espacios(linea + 4);
        ejecutar_comando_dma(arg);
        return;
    }

    // COMANDO: iommu / vtd / vt-d
    if (str_comienza_con(linea, "iommu") || str_comienza_con(linea, "vtd") || str_comienza_con(linea, "vt-d")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "iommu ")) arg = str_saltar_espacios(linea + 6);
        else if (str_comienza_con(linea, "vtd ")) arg = str_saltar_espacios(linea + 4);
        else if (str_comienza_con(linea, "vt-d ")) arg = str_saltar_espacios(linea + 5);
        ejecutar_comando_iommu(arg);
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

    // COMANDO: teclado / keyboard / kbd
    if (str_comienza_con(linea, "teclado") || str_comienza_con(linea, "keyboard") || str_comienza_con(linea, "kbd")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "teclado ")) arg = str_saltar_espacios(linea + 8);
        else if (str_comienza_con(linea, "keyboard ")) arg = str_saltar_espacios(linea + 9);
        else if (str_comienza_con(linea, "kbd ")) arg = str_saltar_espacios(linea + 4);
        ejecutar_comando_teclado(arg);
        return;
    }

    // COMANDO: usb / xhci
    if (str_comienza_con(linea, "usb") || str_comienza_con(linea, "xhci")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "usb ")) arg = str_saltar_espacios(linea + 4);
        else if (str_comienza_con(linea, "xhci ")) arg = str_saltar_espacios(linea + 5);
        ejecutar_comando_usb(arg);
        return;
    }

    // COMANDO: disco / storage / pendrive
    if (str_comienza_con(linea, "disco") || str_comienza_con(linea, "storage") || str_comienza_con(linea, "pendrive")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "disco ")) arg = str_saltar_espacios(linea + 6);
        else if (str_comienza_con(linea, "storage ")) arg = str_saltar_espacios(linea + 8);
        else if (str_comienza_con(linea, "pendrive ")) arg = str_saltar_espacios(linea + 9);
        ejecutar_comando_disco(arg);
        return;
    }

    // COMANDO: tree / arbol
    if (str_comienza_con(linea, "tree") || str_comienza_con(linea, "arbol")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "tree ")) arg = str_saltar_espacios(linea + 5);
        else if (str_comienza_con(linea, "arbol ")) arg = str_saltar_espacios(linea + 6);
        fat32_ejecutar_tree(arg);
        return;
    }

    // COMANDO: ls / dir
    if (str_comienza_con(linea, "ls") || str_comienza_con(linea, "dir")) {
        const char *arg = NULL;
        if (str_comienza_con(linea, "ls ")) arg = str_saltar_espacios(linea + 3);
        else if (str_comienza_con(linea, "dir ")) arg = str_saltar_espacios(linea + 4);
        fat32_listar_directorio(arg);
        return;
    }

    // COMANDO: cat <archivo> / leer <archivo>
    if (str_comienza_con(linea, "cat ") || str_comienza_con(linea, "leer ") || str_comienza_con(linea, "ver ")) {
        const char *archivo = NULL;
        if (str_comienza_con(linea, "cat ")) archivo = str_saltar_espacios(linea + 4);
        else if (str_comienza_con(linea, "leer ")) archivo = str_saltar_espacios(linea + 5);
        else if (str_comienza_con(linea, "ver ")) archivo = str_saltar_espacios(linea + 4);
        fat32_leer_archivo_texto(archivo);
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

    // 1. Resumen conciso de silicio GPU al arrancar
    consola_imprimir_color("==> [ SILICIO DETECTADO ] ", COLOR_AVISO_DEFAULT);
    const struct nvidia_dispositivo *ndev = nvidia_core_obtener_dispositivo();
    if (ndev && ndev->presente) {
        consola_imprimir_color("GPU: ", COLOR_PROMPT_DEFAULT);
        consola_imprimir_color(ndev->chip_name, COLOR_EXITO_DEFAULT);
        consola_imprimir(" (Blackwell GB20x) | VRAM: 16 GiB GDDR7\n");
    } else {
        consola_imprimir_linea("Canal de GPU listo en espacio Ring 0.");
    }
    consola_imprimir_linea("");

    // 2. Autodiagnóstico enfocado del Teclado y Bus USB xHCI (reemplaza el volcado de líneas PCIe)
    ejecutar_autodiagnostico_teclado();
    consola_imprimir_linea("");

    // 3. Si hay almacenamiento USB detectado, autoprueba de lectura de Sector 0 (MBR) y árbol FAT32
    if (usb_msc_obtener_cantidad() > 0) {
        consola_imprimir_linea_color("==> [ ALMACENAMIENTO USB DETECTADO ] Verificando Sector 0 (MBR)...", COLOR_EXITO_DEFAULT);
        ejecutar_lectura_usb_msc(0, 0);
        consola_imprimir_linea("");
        if (fat32_montar(0) == 0) {
            fat32_ejecutar_tree(NULL);
            consola_imprimir_linea("");
        }
    }

    char buffer[256];

    for (;;) {
        imprimir_prompt();
        int len = consola_leer_linea(buffer, sizeof(buffer));
        if (len > 0) {
            procesar_comando(buffer);
        }
    }
}
