#include "huevo.h"
#include "energia.h"
#include "../controladores/animacion_cangrejo.h"
#include "../controladores/pantalla.h"
#include "../controladores/consola.h"
#include "../controladores/teclado.h"
#include "../base/tiempo.h"
#include "../arquitectura/x86_64/serial.h"
#include "../arquitectura/x86_64/idt.h"

static huevo_estabilidad_t g_huevo;

void huevo_iniciar(void) {
    g_huevo.magico = HUEVO_MAGICO;
    g_huevo.salud = 100;
    g_huevo.estado = HUEVO_INTACTO;
    g_huevo.etapa_actual = "Arranque";
    g_huevo.canario = HUEVO_CANARIO;

    serial_imprimir_linea("");
    serial_imprimir_linea("==================================================");
    serial_imprimir_linea("       EL HUEVO DE LA ESTABILIDAD - TAEK OS       ");
    serial_imprimir_linea("==================================================");
    serial_imprimir_linea("     .---.     ");
    serial_imprimir_linea("    /     \\    [EL HUEVO] Estado: INTACTO (100% Integridad)");
    serial_imprimir_linea("   |  100% |   [EL HUEVO] Firma: 0xEE66B007 | Canario: ACTIVO");
    serial_imprimir_linea("    \\     /    [EL HUEVO] \"Si el huevo se quiebra, el universo muere.\"");
    serial_imprimir_linea("     `---'     ");
    serial_imprimir_linea("==================================================");
    serial_imprimir_linea("");
}

void huevo_etapa(const char *nombre_etapa) {
    g_huevo.etapa_actual = nombre_etapa;
    huevo_verificar();

    serial_imprimir("[ EL HUEVO ] >> Iniciando etapa: ");
    serial_imprimir(nombre_etapa);
    serial_imprimir(" ... ");

    consola_imprimir_color("[ EL HUEVO ] ", COLOR_PROMPT_DEFAULT);
    consola_imprimir(">> ");
    consola_imprimir(nombre_etapa);
    consola_imprimir(" ... ");
}

void huevo_etapa_ok(void) {
    huevo_verificar();
    serial_imprimir_linea("[ OK ]");
    consola_imprimir_linea_color("[ OK ]", COLOR_EXITO_DEFAULT);
}

int memoria_verificar_integridad(void);

void huevo_verificar(void) {
    if (g_huevo.magico != HUEVO_MAGICO || g_huevo.canario != HUEVO_CANARIO) {
        huevo_quebrar("¡Corrupción de memoria detectada! ¡El canario ha muerto!", 0, 0, 0);
    }
    memoria_verificar_integridad();
}

void huevo_agrietar(const char *motivo) {
    if (g_huevo.salud > 25) {
        g_huevo.salud -= 25;
    } else {
        g_huevo.salud = 0;
    }

    if (g_huevo.salud == 0) {
        huevo_quebrar(motivo, 0, 0, 0);
    }

    serial_imprimir("\n[ ADVERTENCIA DEL HUEVO ] Cascarón fisurado: ");
    serial_imprimir(motivo);
    serial_imprimir(" | Salud restante: ");
    serial_imprimir_dec(g_huevo.salud);
    serial_imprimir_linea("%");

    consola_imprimir_linea("");
    consola_imprimir_color("[ ADVERTENCIA DEL HUEVO ] ", COLOR_AVISO_DEFAULT);
    consola_imprimir("Cascarón fisurado: ");
    consola_imprimir(motivo);
    consola_imprimir(" | Salud: ");
    consola_imprimir_dec(g_huevo.salud);
    consola_imprimir_linea("%");
}

static void huevo_autopsia_forense(const char *motivo_fatal, const struct marco_interrupcion *marco, uint64_t rip, uint64_t rsp, uint64_t codigo_error) __attribute__((noreturn));

static void huevo_autopsia_forense(const char *motivo_fatal, const struct marco_interrupcion *marco, uint64_t rip, uint64_t rsp, uint64_t codigo_error) {
    g_huevo.salud = 0;
    g_huevo.estado = HUEVO_QUEBRADO;

    if (marco != NULL) {
        rip = marco->rip;
        rsp = marco->rsp;
        codigo_error = marco->codigo_error;
    }

    // 1. Registro Serial Forense
    serial_imprimir_linea("");
    serial_imprimir_linea("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    serial_imprimir_linea("     .---.     ");
    serial_imprimir_linea("    / / \\ \\    [FATAL] ¡EL HUEVO DE LA ESTABILIDAD SE HA QUEBRADO!");
    serial_imprimir_linea("   | X   X |   [FATAL] ¡COLAPSO TOTAL DEL SISTEMA DETECTADO!");
    serial_imprimir_linea("    \\ \\ / /    [FATAL] Motivo del colapso: ");
    serial_imprimir_linea("     `---'     ");
    serial_imprimir("               ");
    serial_imprimir_linea(motivo_fatal);
    serial_imprimir_linea("--------------------------------------------------");
    serial_imprimir("  Etapa del fallo       : ");
    serial_imprimir_linea(g_huevo.etapa_actual ? g_huevo.etapa_actual : "Desconocida");
    serial_imprimir("  Puntero de instrucción: 0x");
    serial_imprimir_hex(rip);
    serial_imprimir_linea("");
    serial_imprimir("  Puntero de pila (RSP) : 0x");
    serial_imprimir_hex(rsp);
    serial_imprimir_linea("");
    serial_imprimir("  Código de error CPU   : 0x");
    serial_imprimir_hex(codigo_error);
    serial_imprimir_linea("");

    if (marco != NULL) {
        serial_imprimir("  RAX: 0x"); serial_imprimir_hex(marco->rax);
        serial_imprimir("  RBX: 0x"); serial_imprimir_hex(marco->rbx);
        serial_imprimir("  RCX: 0x"); serial_imprimir_hex(marco->rcx);
        serial_imprimir("  RDX: 0x"); serial_imprimir_hex(marco->rdx);
        serial_imprimir_linea("");
        serial_imprimir("  RSI: 0x"); serial_imprimir_hex(marco->rsi);
        serial_imprimir("  RDI: 0x"); serial_imprimir_hex(marco->rdi);
        serial_imprimir("  RBP: 0x"); serial_imprimir_hex(marco->rbp);
        serial_imprimir("  RFL: 0x"); serial_imprimir_hex(marco->rflags);
        serial_imprimir_linea("");
    }
    serial_imprimir_linea("==================================================");

    // 2. Pantalla Roja de la Muerte (RSOD) en el Framebuffer Físico
    if (pantalla_obtener_ancho() > 0) {
        #define COLOR_RSOD_FONDO 0x00770000 // Rojo oscuro
        #define COLOR_RSOD_TEXTO 0x00FFFFFF // Blanco puro
        #define COLOR_RSOD_AVISO 0x00FFD700 // Oro brillante
        #define COLOR_RSOD_CYAN  0x0000E5FF // Cyan eléctrico

        pantalla_limpiar(COLOR_RSOD_FONDO);
        consola_establecer_color_fondo(COLOR_RSOD_FONDO);
        consola_establecer_color_texto(COLOR_RSOD_TEXTO);
        consola_limpiar();

        consola_imprimir_linea_color("================================================================================", COLOR_RSOD_AVISO);
        consola_imprimir_linea_color("  [!] AUTOPSIA FORENSE DE ANILLO 0 - EL HUEVO DE LA ESTABILIDAD SE HA QUEBRADO  ", COLOR_RSOD_AVISO);
        consola_imprimir_linea_color("================================================================================", COLOR_RSOD_AVISO);
        consola_imprimir_linea("");

        consola_imprimir_color("  MOTIVO FATAL : ", COLOR_RSOD_CYAN);
        consola_imprimir_linea_color(motivo_fatal, COLOR_RSOD_TEXTO);

        consola_imprimir_color("  ETAPA ACTIVA : ", COLOR_RSOD_CYAN);
        consola_imprimir_linea_color(g_huevo.etapa_actual ? g_huevo.etapa_actual : "Desconocida", COLOR_RSOD_AVISO);
        consola_imprimir_linea("");

        consola_imprimir_linea_color("  --- REGISTROS DE CONTROL Y PUNTEROS ---", COLOR_RSOD_AVISO);
        consola_imprimir("  RIP: "); consola_imprimir_hex(rip);
        consola_imprimir("  RSP: "); consola_imprimir_hex(rsp);
        consola_imprimir("  ERR: "); consola_imprimir_hex(codigo_error);
        consola_imprimir_linea("");

        uint64_t cr3_val = 0;
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_val));
        consola_imprimir("  CR3: "); consola_imprimir_hex(cr3_val);

        if (marco != NULL) {
            uint64_t cr2_val = 0;
            if (marco->num_interrupcion == 14) {
                __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2_val));
                consola_imprimir("  CR2 (Fallo Pag): "); consola_imprimir_hex(cr2_val);
            }
            consola_imprimir("  RFL: "); consola_imprimir_hex(marco->rflags);
            consola_imprimir_linea("");
            consola_imprimir("  CS : "); consola_imprimir_hex(marco->cs);
            consola_imprimir("  SS : "); consola_imprimir_hex(marco->ss);
            consola_imprimir_linea("");
            consola_imprimir_linea("");

            consola_imprimir_linea_color("  --- REGISTROS GENERALES ---", COLOR_RSOD_AVISO);
            consola_imprimir("  RAX: "); consola_imprimir_hex(marco->rax);
            consola_imprimir("  RBX: "); consola_imprimir_hex(marco->rbx);
            consola_imprimir("  RCX: "); consola_imprimir_hex(marco->rcx);
            consola_imprimir("  RDX: "); consola_imprimir_hex(marco->rdx);
            consola_imprimir_linea("");
            consola_imprimir("  RSI: "); consola_imprimir_hex(marco->rsi);
            consola_imprimir("  RDI: "); consola_imprimir_hex(marco->rdi);
            consola_imprimir("  RBP: "); consola_imprimir_hex(marco->rbp);
            consola_imprimir("  R8 : "); consola_imprimir_hex(marco->r8);
            consola_imprimir_linea("");
            consola_imprimir("  R9 : "); consola_imprimir_hex(marco->r9);
            consola_imprimir("  R10: "); consola_imprimir_hex(marco->r10);
            consola_imprimir("  R11: "); consola_imprimir_hex(marco->r11);
            consola_imprimir("  R12: "); consola_imprimir_hex(marco->r12);
            consola_imprimir_linea("");
            consola_imprimir("  R13: "); consola_imprimir_hex(marco->r13);
            consola_imprimir("  R14: "); consola_imprimir_hex(marco->r14);
            consola_imprimir("  R15: "); consola_imprimir_hex(marco->r15);
            consola_imprimir_linea("");
        } else {
            consola_imprimir_linea("");
        }

        consola_imprimir_linea("");
        consola_imprimir_linea_color("================================================================================", COLOR_RSOD_AVISO);
        consola_imprimir_linea_color("  >>> POR FAVOR TOMA UNA FOTO DE ESTA PANTALLA PARA EL DIAGNÓSTICO <<<        ", COLOR_RSOD_TEXTO);
        consola_imprimir_linea_color("  Detonación Don Cangrejo y apagado ACPI en curso...                          ", COLOR_RSOD_AVISO);
        consola_imprimir_linea_color("  (Pulsa cualquier tecla [Espacio/Enter] para detonar de inmediato)           ", COLOR_RSOD_TEXTO);
        consola_imprimir_linea_color("================================================================================", COLOR_RSOD_AVISO);

        // Cuenta regresiva de 25 segundos
        for (int seg = 25; seg > 0; seg--) {
            serial_imprimir("  [AUTOPSIA FORENSE] Tiempo restante: ");
            serial_imprimir_dec(seg);
            serial_imprimir_linea(" s...");

            consola_imprimir("\r  >> Detonación Don Cangrejo en: ");
            consola_imprimir_dec(seg);
            consola_imprimir(" segundos... (pulsa tecla para saltar)    ");

            if (teclado_hay_datos()) {
                teclado_leer_caracter();
                serial_imprimir_linea("  [AUTOPSIA] Pulsación detectada: saltando a detonación.");
                break;
            }

            esperar_milisegundos(1000);
        }
        consola_imprimir_linea("");
    }

    // Invocar el protocolo Don Cangrejo del Hipervisor
    animacion_don_cangrejo_explotar(2);

    apagar_equipo();

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void huevo_quebrar(const char *motivo_fatal, uint64_t rip, uint64_t rsp, uint64_t codigo_error) {
    huevo_autopsia_forense(motivo_fatal, NULL, rip, rsp, codigo_error);
}

void huevo_quebrar_con_marco(const char *motivo_fatal, const struct marco_interrupcion *marco) {
    huevo_autopsia_forense(motivo_fatal, marco, 0, 0, 0);
}
