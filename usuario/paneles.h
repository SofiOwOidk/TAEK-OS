/* H151: el panel de cada opcion de los submenus, dentro de la tarjeta inspectora. Portados del prototype
 * "experimental/Interfaz taek Os v1" (sus paneles de RealOSApps y MotionMaster), con datos de verdad:
 *   CPU      carga de cada hilo (ciclos ocupados de SYS_SENSORES v2), frecuencia efectiva (APERF/MPERF) y caches (CPUID)
 *   HUEVO    'memoria probar' de verdad, por SYS_ORDEN          PAGINAS  'paginacion' y direcciones de este programa
 *   ORACULO  'orar' del nucleo (pasajes y God Words)           STREAM   'transmitir' manda un mensaje al PC
 *   TERMINAL el widget de ordenes.h                            el resto lee sensores, discos o describe y abre
 * Se incluye desde menu.c antes de dibujar_inspector. */

/* Lo que se leyo la ultima vez, para no repetir ordenes en cada cuadro */
static char pan_huevo[1600], pan_paginas[2400], ora_ref[64], ora_texto[400], ora_palabras[200];
static int pan_huevo_ok, pan_paginas_ok, ora_ok;
static char stm_ent[160];
static int stm_len;
static char stm_log[8][120];
static int stm_nlog;
static long pan_t_huevo;

static void stm_anotar(const char *s) {
    if (stm_nlog == 8) { for (int i = 1; i < 8; i++) memcpy(stm_log[i - 1], stm_log[i], sizeof(stm_log[0])); stm_nlog = 7; }
    int n = 0; while (s[n] && n < 119) { stm_log[stm_nlog][n] = s[n]; n++; } stm_log[stm_nlog][n] = 0;
    stm_nlog++;
}

/* Copia la salida de la ultima orden sin colores, con un tope de lineas */
static void pan_guardar(char *d, int cap, int max_lineas) {
    int pos = 0, n = 0, k = 0;
    char lin[240];
    d[0] = 0;
    while (k < max_lineas && orden_linea(&pos, lin, sizeof(lin))) {
        int j = 0;
        while (lin[j] == ' ') j++;
        if (!lin[j]) continue;
        while (lin[j] && n < cap - 2) d[n++] = lin[j++];
        if (n < cap - 1) d[n++] = '\n';
        k++;
    }
    d[n] = 0;
}

static void pan_huevo_correr(void) {
    long r = orden_texto("memoria probar");
    if (r < 0) { int n = 0; const char *e = orden_error(r); while (e[n] && n < 300) { pan_huevo[n] = e[n]; n++; } pan_huevo[n] = 0; }
    else pan_guardar(pan_huevo, sizeof(pan_huevo), 14);
    pan_huevo_ok = 1; pan_t_huevo = taek_ms();
}
static void pan_paginas_correr(void) {
    long r = orden_texto("paginacion");
    if (r < 0) { int n = 0; const char *e = orden_error(r); while (e[n] && n < 300) { pan_paginas[n] = e[n]; n++; } pan_paginas[n] = 0; }
    else pan_guardar(pan_paginas, sizeof(pan_paginas), 14);
    pan_paginas_ok = 1;
}
static void ora_correr(void) {
    long r = orden_texto("orar");
    ora_ok = 1;
    if (r < 0) { ora_ref[0] = 0; ap_copiar(ora_texto, orden_error(r), sizeof(ora_texto)); ora_palabras[0] = 0; return; }
    /* La referencia puede tener espacios ("Juan 3:16"): hasta el corchete */
    orden_buscar("[ Pasaje: ", ora_ref, sizeof(ora_ref), ']');
    for (int n = 0; ora_ref[n]; n++) if (ora_ref[n] == ' ' && !ora_ref[n + 1]) ora_ref[n] = 0;
    orden_buscar("  \"", ora_texto, sizeof(ora_texto), '"');
    orden_buscar("(God Words): ", ora_palabras, sizeof(ora_palabras), 0);
}

/* Lineas de texto guardado en un area, cortadas a lo ancho */
static int pan_texto(struct lienzo *l, int x, int y, int ancho, int y_max, const char *s, uint32_t color) {
    char lin[260];
    while (*s && y < y_max) {
        int n = 0;
        while (*s && *s != '\n' && n < 258) lin[n++] = *s++;
        lin[n] = 0;
        if (*s == '\n') s++;
        uint32_t c = color;
        if (lin[0] == '[' && lin[1] == ' ' && lin[2] == 'O' && lin[3] == 'K') c = 0x2ECC71u;
        if (lin[0] == '[' && (lin[1] == 'F' || lin[1] == 'E' || (lin[1] == ' ' && lin[2] == 'E'))) c = ROJO;
        y = parrafo(l, x, y, ancho, S(14), S(20), c, 255, lin);
    }
    return y;
}

static void pan_pista(struct lienzo *l, int x, int y, int ancho, const char *s, int roja) {
    int w = texto_ancho(S(14), s) + S(24);
    if (w > ancho) w = ancho;
    para(l, x, y - S(18), w, S(26), S(8), roja ? OPACO(ROJO) : OPACO(NEON));
    texto(l, x + S(12), y, S(14), roja ? BLANCO : NEGRO, 255, 0, s);
}

/* CPUID desde Anillo 3 (la instruccion no es privilegiada): caches de la hoja 4 */
static void pan_caches(struct txt *t) {
    t_ini(t);
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 4) { t_add(t, "Sin dato (CPUID 4)"); return; }
    for (uint32_t i = 0; i < 6; i++) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(4), "c"(i));
        uint32_t tipo = a & 31;
        if (!tipo) break;
        uint32_t nivel_c = (a >> 5) & 7;
        uint64_t tam = (uint64_t)(((b >> 22) & 0x3FF) + 1) * (((b >> 12) & 0x3FF) + 1) * ((b & 0xFFF) + 1) * (c + 1);
        if (t->n) t_add(t, " · ");
        t_add(t, "L"); t_num(t, nivel_c); t_add(t, tipo == 1 ? "d " : tipo == 2 ? "i " : " ");
        if (tam >= (1u << 20)) { t_num(t, tam >> 20); t_add(t, " MiB"); } else { t_num(t, tam >> 10); t_add(t, " KiB"); }
    }
}

static void pan_cpu(struct lienzo *l, int x, int y, int ancho, int y_max) {
    struct txt t, d;
    (void)y_max;
    cpu_corto(&t);
    t_ini(&d); t_num(&d, info.cpus); t_add(&d, " hilos arrancados · TSC "); t_num(&d, info.tsc_mhz); t_add(&d, " MHz");
    baldosa(l, x, y, ancho, S(88), "PROCESADOR", t.s, d.s, -1, CIAN);
    y += S(102);
    /* El osciloscopio de nucleos del prototipo, con la carga medida */
    texto(l, x, y + S(4), S(14), NEON, 255, 0, "// CARGA POR HILO (ciclos ocupados entre lecturas):");
    y += S(16);
    int cols = 4, gap = S(16), cw = (ancho - (cols - 1) * gap) / cols, ch = S(150);
    for (int i = 0; i < cols; i++) {
        int cx = x + i * (cw + gap);
        rect(l, cx, y, cw, ch, premul(200, 0x14161Eu));
        rect(l, cx, y, cw, 1, premul(80, CIAN));
        int hay = sen_carga_ok && (uint32_t)i < sns.cpus;
        float c = hay ? sen_carga[i] : 0;
        int h = (int)(c * (float)(ch - S(36)));
        uint32_t tono = c > 0.85f ? ROJO : c > 0.5f ? NEON : CIAN;
        if (hay) rect(l, cx + S(10), y + ch - S(26) - h, cw - S(20), h, OPACO(tono));
        /* Historia de esa CPU, en linea fina */
        if (hay && h_cpu[i].n > 1) {
            float px = 0, py = 0;
            for (int k = 0; k < h_cpu[i].n; k++) {
                float qx = (float)cx + (float)cw * (float)(k + HIST - h_cpu[i].n) / (float)(HIST - 1);
                float qy = (float)(y + ch - S(26)) - hist_ver(&h_cpu[i], k) / 100.0f * (float)(ch - S(36));
                if (k) trazo(l, px, py, qx, qy, SF(1.4f), premul(200, BLANCO), 0);
                px = qx; py = qy;
            }
        }
        struct txt e; t_ini(&e); t_add(&e, "C"); t_num(&e, (uint64_t)i);
        texto(l, cx + S(8), y + ch - S(8), S(13), GRIS, 255, 0, e.s);
        t_ini(&e);
        if (hay) { t_num(&e, (uint64_t)(c * 100.0f + 0.5f)); t_add(&e, " %"); }
        else t_add(&e, sen_v2() ? (i ? "dormida" : "--") : "sin dato");
        texto(l, cx + cw - texto_ancho(S(13), e.s) - S(8), y + ch - S(8), S(13), hay ? tono : GRIS, 255, 0, e.s);
    }
    y += ch + S(30);
    t_ini(&t);
    if (sen_v2() && sns.aperf_ok && sen_ghz > 0.1f) { t_num(&t, (uint64_t)sen_ghz); t_add(&t, ","); t_num(&t, (uint64_t)(sen_ghz * 100) % 100 / 10); t_num(&t, (uint64_t)(sen_ghz * 100) % 10); t_add(&t, " GHz (APERF/MPERF)"); }
    else t_add(&t, sen_v2() ? "Sin dato: sin APERF/MPERF" : "Sin dato (núcleo anterior a H151)");
    y = fila_dato(l, x, y, ancho, "Frecuencia real", t.s);
    pan_caches(&t);
    y = fila_dato(l, x, y, ancho, "Cachés", t.s);
    parrafo(l, x, y - S(6), ancho, S(13), S(18), 0x8E94A8u, 255, "C0 corre esta interfaz: lo que no pasa esperando es trabajo. C1-C3 solo trabajan en lotes (decodificar video); el resto duermen con hlt.");
}

static void pan_discos(struct lienzo *l, int x, int y, int ancho) {
    static struct taek_disco_usb dd[6];
    static struct taek_archivos a;
    static int nd = -1;
    static long t_leido;
    if (nd < 0 || taek_ms() - t_leido > 5000) {
        memset(&a, 0, sizeof(a));
        a.op = TAEK_AR_DISCOS; a.destino = (uint64_t)(uintptr_t)dd; a.capacidad = 6;
        nd = taek_archivos(&a) == 0 ? (int)a.n : 0;
        t_leido = taek_ms();
    }
    struct txt t;
    for (int i = 0; i < nd && i < 4; i++) {
        char k[24]; int n = 0; while (dd[i].nombre[n] && n < 22) { k[n] = dd[i].nombre[n]; n++; } k[n] = 0;
        t_ini(&t); t_add(&t, "[hd"); t_num(&t, (uint64_t)i); t_add(&t, "] "); t_num(&t, (dd[i].bytes + (1ull << 29)) >> 30); t_add(&t, " GB · ");
        t_add(&t, dd[i].listo ? (dd[i].montado ? "montado" : "listo") : "sin responder");
        y = fila_dato(l, x, y, ancho, k, t.s);
    }
    if (!nd) y = fila_dato(l, x, y, ancho, "Discos USB", "Ninguno");
    y = fila_dato(l, x, y, ancho, "Volumen abierto", a.fs[0] ? a.fs : "Ninguno");
    t_ini(&t);
    if (vol_ult.leido) { t_hexn(&t, vol_ult.bytes[0x0A], 2); t_add(&t, " "); t_hexn(&t, vol_ult.bytes[0x0B], 2); t_add(&t, (vol_ult.banderas & 6) ? " · SUCIO" : " · LIMPIO"); }
    else t_add(&t, "En CONFIGURACIÓN > ALMACENAMIENTO, [L]");
    fila_dato(l, x, y, ancho, "Offset 0x6A", t.s);
}

static void pan_lan(struct lienzo *l, int x, int y, int ancho) {
    struct txt t;
    if (!sen_hay()) { parrafo(l, x, y, ancho, S(18), S(28), AMBAR, 255, "Sin dato: el núcleo que corre no tiene SYS_SENSORES."); return; }
    y = fila_dato(l, x, y, ancho, "Estado", !sns.stream_activo ? "Apagado" : sns.stream_conectado ? "CONECTADO" : "Esperando al PC");
    t_ini(&t); if (sns.stream_activo) { t_ip4(&t, sns.stream_ip); t_add(&t, ":"); t_num(&t, sns.stream_puerto); } else t_add(&t, "Sin destino");
    y = fila_dato(l, x, y, ancho, "PC receptor", t.s);
    t_ini(&t); if (sns.ip_ok) t_ip4(&t, sns.ip); else t_add(&t, "Sin IP");
    y = fila_dato(l, x, y, ancho, "Esta laptop", t.s);
    t_ini(&t); t_bytes(&t, sns.stream_enviado); y = fila_dato(l, x, y, ancho, "Consola entregada", t.s);
    t_ini(&t); t_dec1(&t, sen_env_s / 1024.0f); t_add(&t, " KiB/s"); y = fila_dato(l, x, y, ancho, "Ritmo", t.s);
    t_ini(&t); t_num(&t, (uint64_t)(sen_rx_s + 0.5f)); t_add(&t, " / "); t_num(&t, (uint64_t)(sen_tx_s + 0.5f)); t_add(&t, " paq/s");
    y = fila_dato(l, x, y, ancho, "Red RX / TX", t.s);
    /* Barras del ritmo de consola, como el histograma del prototipo */
    int gh = S(90), bw = ancho / HIST;
    float max = hist_max(&h_env, 4) * 1.2f;
    rect(l, x, y, ancho, gh, premul(200, 0x14161Eu));
    for (int k = 0; k < h_env.n; k++) {
        int h = (int)(hist_ver(&h_env, k) / max * (float)(gh - S(6)));
        if (h > 0) rect(l, x + (k + HIST - h_env.n) * ancho / HIST, y + gh - h, bw > 1 ? bw : 1, h, OPACO(sns.stream_conectado ? 0x2ECC71u : ROJO));
    }
}

static void pan_stream(struct lienzo *l, int x, int y, int ancho, int y_max, int con_foco) {
    struct txt t;
    int conectado = sen_hay() && sns.stream_conectado;
    rect(l, x, y, ancho, S(30), premul(220, 0x101418u));
    texto(l, x + S(10), y + S(21), S(14), CIAN, 255, 0, "TCP 9153 // STREAM LAN");
    t_ini(&t); t_add(&t, !sen_hay() ? "SIN DATO" : conectado ? "CONECTADO " : sns.stream_activo ? "ESPERANDO" : "APAGADO");
    if (conectado) t_ip4(&t, sns.stream_ip);
    texto(l, x + ancho - texto_ancho(S(14), t.s) - S(10), y + S(21), S(14), conectado ? 0x2ECC71u : AMBAR, 255, 0, t.s);
    y += S(44);
    int alto_feed = y_max - y - S(80);
    rect(l, x, y, ancho, alto_feed, premul(230, 0x050608u));
    int yy = y + S(20);
    for (int i = 0; i < stm_nlog && yy < y + alto_feed; i++, yy += S(22)) texto(l, x + S(10), yy, S(13), i == stm_nlog - 1 ? NEON : 0xB8C4D0u, 255, 0, stm_log[i]);
    if (!stm_nlog) parrafo(l, x + S(10), yy, ancho - S(20), S(13), S(20), 0x8E94A8u, 255, "Lo que escribas aquí llega a la terminal de Windows del PC con 'transmitir'. Lo que el PC escribe por la LAN entra como teclas a TAEK.");
    y += alto_feed + S(10);
    rect(l, x, y, ancho, S(34), con_foco ? premul(255, 0x101820u) : premul(255, 0x0B0C10u));
    rect(l, x, y, ancho, 1, premul(con_foco ? 220 : 90, NEON));
    int xx = texto(l, x + S(10), y + S(23), S(14), NEON, 255, 0, "PC >");
    xx = texto(l, xx + S(8), y + S(23), S(14), BLANCO, 255, 0, stm_ent);
    if (con_foco && (taek_ms() / 500) % 2) rect(l, xx + S(2), y + S(9), S(8), S(16), OPACO(NEON));
    if (!con_foco && !stm_len) texto(l, xx + S(8), y + S(23), S(13), 0x6A6E7Cu, 255, 0, "[INTRO] para escribir un mensaje al PC");
}
static void stm_enviar(void) {
    if (!stm_len) return;
    char c[200]; struct txt t;
    ap_copiar(c, "transmitir ", sizeof(c)); ap_agregar(c, stm_ent, sizeof(c));
    long r = orden_texto(c);
    t_ini(&t); t_add(&t, "[TX -> PC] "); t_add(&t, stm_ent); stm_anotar(t.s);
    if (r < 0) stm_anotar(orden_error(r));
    else { char lin[160]; int pos = 0; if (orden_linea(&pos, lin, sizeof(lin))) { t_ini(&t); t_add(&t, "[NÚCLEO] "); t_add(&t, lin); stm_anotar(t.s); } }
    stm_len = 0; stm_ent[0] = 0;
}
static int stm_tecla(uint32_t k) {
    if (k == 27) return 1;
    if (k == '\n' || k == '\r') { stm_enviar(); return 0; }
    if (k == 8 || k == 127) { if (stm_len) { do stm_len--; while (stm_len && ((unsigned char)stm_ent[stm_len] & 0xC0) == 0x80); stm_ent[stm_len] = 0; } return 0; }
    if (k >= 32 && k < 127 && stm_len < 150) { stm_ent[stm_len++] = (char)k; stm_ent[stm_len] = 0; }
    return 0;
}

static void pan_oraculo(struct lienzo *l, int x, int y, int ancho, int y_max) {
    /* La cruz del prototipo, dibujada */
    int cx = x + ancho / 2;
    rect(l, cx - S(7), y, S(14), S(84), OPACO(NEON));
    rect(l, cx - S(30), y + S(20), S(60), S(14), OPACO(NEON));
    rect(l, cx - S(7) + S(4), y + S(4), S(14), S(84), premul(90, ROJO));
    y += S(110);
    const char *tit = "ORÁCULO DIVINO // HOMENAJE A TERRY A. DAVIS";
    texto(l, cx - texto_ancho(S(14), tit) / 2, y, S(14), NEON, 255, 0, tit);
    y += S(34);
    if (!ora_ok) { parrafo(l, x, y, ancho, S(18), S(28), 0xE4E4EEu, 255, "\"Intro consulta la entropía divina del rdtsc del procesador.\""); return; }
    if (ora_ref[0]) { struct txt t; t_ini(&t); t_add(&t, "- "); t_add(&t, ora_ref); texto(l, cx - texto_ancho(S(16), t.s) / 2, y, S(16), 0x2ECC71u, 255, 0, t.s); y += S(32); }
    struct txt q; t_ini(&q); t_add(&q, "\""); t_add(&q, ora_texto); t_add(&q, "\"");
    y = parrafo(l, x, y, ancho, S(18), S(28), BLANCO, 255, ora_texto[0] ? ora_texto : q.s);
    if (ora_palabras[0] && y + S(60) < y_max) {
        y += S(18);
        texto(l, x, y, S(13), NEON, 255, 0, "GOD WORDS:");
        parrafo(l, x, y + S(24), ancho, S(16), S(24), CIAN, 255, ora_palabras);
    }
}

static void pan_ruleta(struct lienzo *l, int x, int y, int ancho) {
    int cx = x + ancho / 2, cy = y + S(110), r = S(96);
    float giro = (float)(taek_ms() % 6000) / 6000.0f * 6.2831853f * (confirmar ? 4.0f : 1.0f);
    anillo(l, cx, cy, r, 0, OPACO(0x1A1A1Au));
    anillo(l, cx, cy, r, r - S(4), OPACO(NEON));
    anillo(l, cx, cy, S(24), 0, OPACO(NEGRO));
    anillo(l, cx, cy, S(24), S(21), OPACO(BLANCO));
    for (int i = 0; i < 6; i++) {
        float b = giro + (float)i * 1.0471976f;
        int px = cx + (int)(cosen(b) * (float)r * 0.62f), py = cy + (int)(sen(b) * (float)r * 0.62f);
        anillo(l, px, py, S(17), 0, OPACO(0x333333u));
        anillo(l, px, py, S(17), S(14), OPACO(i == 0 ? ROJO : NEON));
    }
    y = cy + r + S(36);
    parrafo(l, x, y, ancho, S(16), S(24), BLANCO, 255, "Es la ruleta del núcleo: seis rondas con el duelo de fondo y más balas en cada una. Si pierdes, el sistema colapsa de verdad (el huevo se quiebra). Si gana el jugador, se desbloquea 'el comando'.");
}

/* El panel de la opcion; devuelve 1 si dibujo algo (si no, se usa el texto de detalle) */
static int pan_dibujar(struct lienzo *l, const struct opcion *o, int x, int y, int ancho, int y_max) {
    struct txt t;
    uint32_t c = 0xE4E4EEu, g = 0xB8B8C4u;
    switch (o->panel) {
    case P_TABLERO: inspector_inicio(l, x, y, ancho); return 1;
    case P_CPU: pan_cpu(l, x, y, ancho, y_max); pan_pista(l, x, y_max, ancho, "[INTRO] TEST DE ESTRÉS (PANTALLA COMPLETA)", 0); return 1;
    case P_GPU:
        cadena_gpu(&t); y = fila_dato(l, x, y, ancho, "Adaptador", t.s);
        y = fila_dato(l, x, y, ancho, "Video H.264", info.gpu_video_probada ? "Por GPU (probado en este equipo)" : "Por software");
        t_ini(&t); t_num(&t, info.ancho); t_add(&t, " x "); t_num(&t, info.alto); y = fila_dato(l, x, y, ancho, "Pantalla", t.s);
        y = fila_dato(l, x, y, ancho, "Compositor", info.capa[0] ? info.capa : "CPU");
        y += S(10);
        parrafo(l, x, y, ancho, S(16), S(24), g, 255, "Intro corre la prueba del núcleo: decodifica el video de prueba de la ISO a toda velocidad, por la GPU si está probada aquí, y dice los cuadros por segundo de verdad. El prototipo mostraba un número fijo; aquí sale de la medición.");
        pan_pista(l, x, y_max, ancho, "[INTRO] PRUEBA DE VIDEO H.264 360p", 0);
        return 1;
    case P_HUEVO:
        t_ini(&t);
        if (sen_hay()) { t_add(&t, sns.heap_canarios_ok ? "INTACTOS · 0x7AEECC05 / 0xCAFEBABEDEAD1000" : "ROTO"); }
        else t_add(&t, "Sin dato");
        y = fila_dato(l, x, y, ancho, "Canarios", t.s);
        t_ini(&t); if (sen_hay()) { t_num(&t, sns.huevo_salud); t_add(&t, " % · mágico 0x"); t_hexn(&t, sns.huevo_magico, 8); } else t_add(&t, "Sin dato");
        y = fila_dato(l, x, y, ancho, "El huevo", t.s);
        y += S(6);
        texto(l, x, y, S(14), NEON, 255, 0, pan_huevo_ok ? "// 'memoria probar' (salida real):" : "// 'memoria probar' todavía no se ejecutó");
        y += S(24);
        if (pan_huevo_ok) pan_texto(l, x, y, ancho, y_max - S(40), pan_huevo, c);
        pan_pista(l, x, y_max, ancho, "[INTRO] EJECUTAR 'memoria probar'", 0);
        return 1;
    case P_PAGINAS: {
        /* Direcciones de este mismo programa, partidas en sus cuatro indices */
        uint64_t dirs[3] = { (uint64_t)(uintptr_t)&pan_dibujar, (uint64_t)(uintptr_t)&t, TAEK_MONTON };
        static const char *const nom[3] = { "Código", "Pila", "Montón" };
        for (int i = 0; i < 3; i++) {
            uint64_t v = dirs[i];
            t_ini(&t); t_add(&t, "0x"); t_hexn(&t, v, 12); t_add(&t, " -> ");
            t_num(&t, (v >> 39) & 511); t_add(&t, "/"); t_num(&t, (v >> 30) & 511); t_add(&t, "/"); t_num(&t, (v >> 21) & 511); t_add(&t, "/"); t_num(&t, (v >> 12) & 511);
            y = fila_dato(l, x, y, ancho, nom[i], t.s);
        }
        parrafo(l, x, y - S(4), ancho, S(13), S(18), 0x8E94A8u, 255, "Índices PML4 / PDP / PD / PT de direcciones reales de esta interfaz (Anillo 3, mitad baja).");
        y += S(42);
        if (pan_paginas_ok) { texto(l, x, y, S(14), NEON, 255, 0, "// 'paginacion' (salida real):"); pan_texto(l, x, y + S(24), ancho, y_max - S(40), pan_paginas, c); }
        pan_pista(l, x, y_max, ancho, "[INTRO] EJECUTAR 'paginacion'", 0);
        return 1;
    }
    case P_DISCOS: pan_discos(l, x, y, ancho); pan_pista(l, x, y_max, ancho, "[INTRO] ABRIR EL EXPLORADOR (archivos.elf)", 0); return 1;
    case P_LAN: pan_lan(l, x, y, ancho); return 1;
    case P_ESTRES:
        y = parrafo(l, x, y, ancho, S(18), S(28), c, 255, "La prueba de estrés del núcleo ('stress'), de verdad:");
        y += S(8);
        y = parrafo(l, x, y, ancho, S(15), S(23), g, 255, "64 rondas que piden páginas físicas, bloques del montón y búferes DMA, los llenan con un patrón y los comprueban; mientras tanto lee sectores del USB, atiende el ratón y deja sonando el audio en bucle. Al final dice si hubo un solo error.");
        y += S(8);
        parrafo(l, x, y, ancho, S(15), S(23), NEON, 255, "El prototipo pintaba 4,20 GHz y 8 hilos al 100 %. Aquí el resultado es el que dé el equipo.");
        pan_pista(l, x, y_max, ancho, "[INTRO] DISPARAR (PANTALLA COMPLETA)", 1);
        return 1;
    case P_TERMINAL: term_bienvenida(); term_dibujar(l, x - S(8), y - S(20), ancho + S(16), y_max - y + S(30), panel_foco); return 1;
    case P_NAVEGADOR: case P_MEDIA: case P_VFS: {
        static const char *const txt[3] = {
            "El navegador de TAEK: HTTP y HTTPS con el cliente del núcleo (TCP propio y mbedTLS), con historial y búsqueda. Es navegador.elf, una aplicación de Anillo 3 del USB.",
            "El reproductor: videos H.264 por la GPU si está probada en este equipo, MP3 y las imágenes de todos los discos, con búsqueda al escribir. Es videos.elf, del USB.",
            "El explorador: carpetas de todos los discos, abrir con el reproductor que toque, copiar, mover y la papelera. Escribe en FAT32 y exFAT, nunca en /ventoy ni en una .iso." };
        y = parrafo(l, x, y, ancho, S(18), S(28), c, 255, txt[o->panel - P_NAVEGADOR]);
        y += S(16);
        parrafo(l, x, y, ancho, S(14), S(21), g, 255, "Al cerrarla vuelves aquí, a esta misma opción.");
        t_ini(&t); t_add(&t, "[INTRO] ABRIR "); t_add(&t, o->arg);
        pan_pista(l, x, y_max, ancho, t.s, 0);
        return 1;
    }
    case P_STREAM: pan_stream(l, x, y, ancho, y_max, panel_foco); return 1;
    case P_ORACULO: pan_oraculo(l, x, y, ancho, y_max); pan_pista(l, x, y_max, ancho, "[INTRO] INVOCAR PALABRA SAGRADA ('orar')", 0); return 1;
    case P_RULETA: pan_ruleta(l, x, y, ancho); break;
    case P_APAGAR: parrafo(l, x, y, ancho, S(18), S(28), c, 255, "Apaga el equipo de inmediato por ACPI. Lo que no esté guardado se pierde."); pan_pista(l, x, y_max, ancho, "[INTRO] APAGAR", 1); return 1;
    case P_REINICIO: parrafo(l, x, y, ancho, S(18), S(28), c, 255, "Reinicia el equipo de inmediato. Ventoy vuelve a arrancar la ISO que diga ventoy.json (la que deja actualizar_taek.py)."); pan_pista(l, x, y_max, ancho, "[INTRO] REINICIAR", 1); return 1;
    case P_TRIPLE:
        y = parrafo(l, x, y, ancho, S(18), S(28), c, 255, "Harakiri: el núcleo destruye su IDT, su TSS y su pila y ejecuta una instrucción inválida: triple fault.");
        y += S(12);
        y = parrafo(l, x, y, ancho, S(15), S(23), g, 255, "Solo se dispara si el hipervisor VMX está listo para atraparlo (entonces aparece Don Cangrejo); si no, el núcleo se niega y lo dice, y se vuelve al menú.");
        y += S(12);
        parrafo(l, x, y, ancho, S(15), S(23), ROJO, 255, "No es una simulación: es la orden 'harakiri' del núcleo.");
        break;
    case P_CANCELAR: parrafo(l, x, y, ancho, S(18), S(28), c, 255, "Regresa al menú sin apagar nada."); return 1;
    default:
        if (o->panel >= P_MOD0 && o->panel <= P_MOD7) {
            int m = o->panel - P_MOD0;
            static const char *const des[8] = {
                "Controlador Ethernet, IP, contadores de paquetes y la caché ARP de 8 vecinos. Desde la cabina: [V] vacía la caché y [C] pide IP por DHCP.",
                "La consola de TAEK viaja por TCP al PC, que la guarda en consola.raw; las teclas del PC vuelven por el mismo canal.",
                "La cola PCM del núcleo hacia HDA o AC97, el volumen del sistema, un tono de 440 Hz y los efectos de la interfaz.",
                "El adaptador, el video por GPU, el compositor y cuánto tarda cada cuadro de la cabina.",
                "El mapa real de los primeros bloques del montón, sus canarios, el huevo y la protección SMEP/SMAP.",
                "Los discos USB y la bandera de limpieza del volumen exFAT (offset 0x6A), leída a pedido.",
                "La temperatura del sensor digital del procesador, la alarma a 90 °C y los bits de IA32_THERM_STATUS.",
                "Cuánto tarda CPU 0 en volver a atender al USB, la red y el sonido, las llamadas al sistema más largas y los sucesos de los puertos USB. Es la Fase 0 del gestor de hilos." };
            y = parrafo(l, x, y, ancho, S(17), S(26), c, 255, des[m]);
            y += S(14);
            if (sen_hay()) {
                switch (m) {
                case 0: t_ini(&t); t_num(&t, sns.rx_paquetes); t_add(&t, " / "); t_num(&t, sns.tx_paquetes); y = fila_dato(l, x, y, ancho, "Paquetes RX / TX", t.s); break;
                case 1: y = fila_dato(l, x, y, ancho, "Stream", sns.stream_conectado ? "CONECTADO" : sns.stream_activo ? "Esperando" : "Apagado"); break;
                case 4: y = fila_dato(l, x, y, ancho, "Canarios", sns.heap_canarios_ok ? "Intactos" : "ROTO"); break;
                case 6: t_ini(&t); if (sns.temp_ok) { t_num(&t, (uint64_t)(sns.temp_nucleo_c > 0 ? sns.temp_nucleo_c : 0)); t_add(&t, " °C"); } else t_add(&t, "Sin dato"); y = fila_dato(l, x, y, ancho, "Núcleo 0", t.s); break;
                case 7: t_ini(&t); if (sen_v3()) { t_num(&t, sns.usb_max_ms); t_add(&t, " ms"); } else t_add(&t, "Núcleo anterior a H204"); y = fila_dato(l, x, y, ancho, "USB: mayor hueco", t.s); break;
                default: break;
                }
            }
            t_ini(&t); t_add(&t, "[INTRO] PANTALLA COMPLETA // MAGI 0"); t_num(&t, (uint64_t)(m + 1));
            pan_pista(l, x, y_max, ancho, t.s, 0);
            return 1;
        }
        return 0;
    }
    /* Opciones que confirman con dos Intro */
    pan_pista(l, x, y_max, ancho, confirmar ? "[INTRO] OTRA VEZ PARA CONFIRMAR · [ESC] CANCELA" : "[INTRO] DOS VECES PARA CONFIRMAR", 1);
    return 1;
}

/* Paneles que cambian solos: se redibujan cada segundo (o mas seguido si tienen el cursor de texto) */
static int pan_vivo(int panel) {
    return panel == P_TABLERO || panel == P_CPU || panel == P_LAN || panel == P_STREAM || panel == P_HUEVO || panel == P_RULETA ||
           panel == P_TERMINAL || (panel >= P_MOD0 && panel <= P_MOD7);
}
