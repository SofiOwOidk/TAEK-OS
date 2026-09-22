#include "terminal.h"
#include "consola.h"
#include "pantalla.h"
#include "audio_ac97.h"
#include "animacion_cangrejo.h"
#include "../base/huevo.h"
#include "../base/energia.h"
#include "../base/tiempo.h"
#include "../arquitectura/x86_64/vmx.h"

extern const uint8_t _binary_audio_arranque_bin_start[];
extern const uint8_t _binary_audio_arranque_bin_end[];

extern const uint8_t _binary_duelo_audio_bin_start[];
extern const uint8_t _binary_duelo_audio_bin_end[];

static void esperar_con_audio_bucle(int ms, const uint8_t *audio, uint32_t tam) {
    int paso = 50;
    while (ms > 0) {
        if (!audio_ac97_esta_reproduciendo() && tam > 0) {
            audio_ac97_reproducir_pcm(audio, tam);
        }
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

        // Iniciar sintonía de duelo del Spaghetti Western en bucle por DMA
        if (tam_duelo > 0) {
            audio_ac97_reproducir_pcm(_binary_duelo_audio_bin_start, tam_duelo);
        }

        consola_imprimir_linea_color("==============================================================", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("  [ DUELO 1 VS 1 ] El Bueno, El Feo y El Malo...              ", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("  [ RULETA RUSA ] El tambor tiene 6 recámaras y 1 sola bala...", COLOR_TEXTO_DEFAULT);
        consola_imprimir_linea_color("  [ RULETA RUSA ] Girando el cilindro: *chac-chac-chac-chac*...", COLOR_TEXTO_DEFAULT);
        consola_imprimir_linea_color("==============================================================", COLOR_AVISO_DEFAULT);
        esperar_con_audio_bucle(1200, _binary_duelo_audio_bin_start, tam_duelo);

        // Turno del jugador (1 en 6 de morir)
        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ TU TURNO ] Mirada fija... Colocas el revólver en tu sien y aprietas...", COLOR_AVISO_DEFAULT);
        esperar_con_audio_bucle(2000, _binary_duelo_audio_bin_start, tam_duelo);

        uint32_t tiro_jugador = obtener_aleatorio() % 6;
        if (tiro_jugador == 0) {
            audio_ac97_detener();
            consola_imprimir_linea("");
            consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  *¡¡¡PUMMMMMMMMMMMMMMMMMMMMM!!!* ¡Bala en la recámara!", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  [ RULETA RUSA ] Has perdido el duelo. El proyectil atravesó el sistema.", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  [ RULETA RUSA ] Borrando el sistema operativo... ¡Adiós!", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea("");

            esperar_milisegundos(1500);
            huevo_quebrar("¡Perdiste el duelo de Ruleta Rusa! El cartucho estaba cargado.", 0xDEADBEEF, 0, 0);
            return;
        }

        consola_imprimir_linea_color("  *¡CLIC!* ... Recámara vacía. ¡Has sobrevivido!", COLOR_EXITO_DEFAULT);
        esperar_con_audio_bucle(1200, _binary_duelo_audio_bin_start, tam_duelo);

        // Turno del sistema (1 en 6 de perder)
        consola_imprimir_linea("");
        consola_imprimir_linea_color("==> [ TURNO DEL SISTEMA ] El kernel TAEK OS toma el revólver...", COLOR_AVISO_DEFAULT);
        consola_imprimir_linea_color("==> [ TURNO DEL SISTEMA ] Silicio contra plomo. Aprieta el gatillo...", COLOR_TEXTO_DEFAULT);
        esperar_con_audio_bucle(2000, _binary_duelo_audio_bin_start, tam_duelo);

        uint32_t tiro_sistema = obtener_aleatorio() % 6;
        if (tiro_sistema == 0) {
            audio_ac97_detener();
            consola_imprimir_linea("");
            consola_imprimir_linea_color("  *¡¡¡PUMMMMMMMMMMMMMMMMMMMMM!!!*", COLOR_ERROR_DEFAULT);
            consola_imprimir_linea_color("  [ RULETA RUSA ] ¡EL SISTEMA OPERATIVO HA PERDIDO EL DUELO!", COLOR_AVISO_DEFAULT);
            consola_imprimir_linea_color("  [ RULETA RUSA ] La bala perforó el kernel, pero el Huevo resistió.", COLOR_TEXTO_DEFAULT);
            consola_imprimir_linea_color("  [ LOGRO DESBLOQUEADO ] ¡Has derrotado al sistema!", COLOR_EXITO_DEFAULT);
            consola_imprimir_linea_color("  [ RECOMPENSA ] Se ha desbloqueado el comando: \"El comando\"", COLOR_USUARIO_DEFAULT);
            consola_imprimir_linea("");
            g_el_comando_desbloqueado = 1;
            return;
        }

        audio_ac97_detener();
        consola_imprimir_linea_color("  *¡CLIC!* ... Recámara vacía. El sistema también sobrevive.", COLOR_TEXTO_DEFAULT);
        consola_imprimir_linea_color("==> [ TABLAS ] Ambos siguen vivos. Vuelve a jugar si te atreves.", COLOR_PROMPT_DEFAULT);
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
