#include "huevo.h"
#include "energia.h"
#include "../arquitectura/x86_64/serial.h"

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
}

void huevo_etapa_ok(void) {
    huevo_verificar();
    serial_imprimir_linea("[ OK ]");
}

void huevo_verificar(void) {
    if (g_huevo.magico != HUEVO_MAGICO || g_huevo.canario != HUEVO_CANARIO) {
        huevo_quebrar("¡Corrupcion de memoria detectada! ¡El canario ha muerto!", 0, 0, 0);
    }
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

    serial_imprimir("\n[ ADVERTENCIA DEL HUEVO ] Cascaron fisurado: ");
    serial_imprimir(motivo);
    serial_imprimir(" | Salud restante: ");
    serial_imprimir_dec(g_huevo.salud);
    serial_imprimir_linea("%");
}

void huevo_quebrar(const char *motivo_fatal, uint64_t rip, uint64_t rsp, uint64_t codigo_error) {
    g_huevo.salud = 0;
    g_huevo.estado = HUEVO_QUEBRADO;

    serial_imprimir_linea("");
    serial_imprimir_linea("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    serial_imprimir_linea("     .---.     ");
    serial_imprimir_linea("    / / \\ \\    [FATAL] ¡EL HUEVO DE LA ESTABILIDAD SE HA QUEBRADO!");
    serial_imprimir_linea("   | X   X |   [FATAL] ¡COLAPSO TOTAL DEL SISTEMA EN EJECUCION!");
    serial_imprimir_linea("    \\ \\ / /    [FATAL] Motivo del colapso: ");
    serial_imprimir_linea("     `---'     ");
    serial_imprimir("               ");
    serial_imprimir_linea(motivo_fatal);
    serial_imprimir_linea("--------------------------------------------------");
    serial_imprimir("  Etapa del fallo       : ");
    serial_imprimir_linea(g_huevo.etapa_actual ? g_huevo.etapa_actual : "Desconocida");
    serial_imprimir("  Puntero de instruccion: ");
    serial_imprimir_hex(rip);
    serial_imprimir_linea("");
    serial_imprimir("  Puntero de pila (RSP) : ");
    serial_imprimir_hex(rsp);
    serial_imprimir_linea("");
    serial_imprimir("  Codigo de error de CPU: ");
    serial_imprimir_hex(codigo_error);
    serial_imprimir_linea("");
    serial_imprimir_linea("==================================================");
    serial_imprimir_linea("[ EL HUEVO ] APAGANDO LA MAQUINA DE INMEDIATO...");
    serial_imprimir_linea("==================================================");

    apagar_equipo();
}
