/* H150: cabina tactica de Configuracion, portada del prototipo "experimental/Interfaz taek Os v1" (estilo MAGI).
 * Siete modulos a pantalla completa, cada uno con su grafica en vivo y sus datos, y un modo hexadecimal que muestra los
 * bytes tal como los entrego el nucleo, con el significado de cada uno. Se entra desde el escaneo de Configuracion.
 * Todo sale de SYS_SENSORES, SYS_INFO, SYS_AUDIO y SYS_ARCHIVOS; lo que el equipo no informa se dice ("sin dato").
 * Del prototipo NO se portaron, porque TAEK no los tiene: control de ventiladores, reinicio de la pila de red, cifras
 * fijas de rendimiento (410 FPS) ni la purga termica. Se incluye desde menu.c despues de escritorio.h. */

#define CAB_FONDO 0x050608u
#define CAB_TINTA 0x8A7A66u

enum { M_RED, M_STREAM, M_AUDIO, M_GPU, M_NUCLEO, M_VFS, M_TERMICO, M_LATENCIA, NMOD };
static const char *const mod_nombre[NMOD] = { "RED", "STREAM", "AUDIO", "GPU", "NÚCLEO", "VFS", "TÉRMICO", "LATENCIA" };
static const char *const mod_titulo[NMOD] = {
    "PILA DE RED // CONTROLADOR ETHERNET", "STREAMING LAN // CONSOLA HACIA EL PC", "AUDIO // COLA PCM DEL NÚCLEO",
    "GPU // PANTALLA Y COMPOSITOR", "GUARDIÁN DEL NÚCLEO // EL HUEVO", "ALMACENAMIENTO // DISCOS Y VOLUMEN",
    "TÉRMICO // SENSOR DIGITAL DEL PROCESADOR", "LATENCIA // CUÁNTO TARDA CPU 0 EN ATENDER" };
static const char *const mod_accion[NMOD] = { "[V] VACIAR ARP · [C] DHCP", "[R] LEER · [E] ECO AL PC", "[T] TONO · [+/-] VOLUMEN · [M] EFECTOS", "[R] LEER", "[R] AUDITAR", "[L] LEER 0x6A", "[P] PROBAR ALARMA", "[R] LEER · [Z] A CERO" };

static struct taek_superficie s_cab, s_obt[2];
static struct lienzo l_cab, l_obt[2];
static int cab_creada, cab_hay_obt, cab_mod, cab_hex, cab_cursor, cab_scroll = -1;
static long cab_ult_sen, cab_ult_graf, cab_ult_cuadro, cab_ult_alarma, cab_t0;
static float cab_cuadro_ms;
static char cab_log[3][100];
static int cab_nlog;
static char cab_arp[4][64];
static int cab_narp, cab_arp_leida;
/* 'red arp' del nucleo: una linea por vecino ("  192.168.18.1  E4:..:..  usada hace 3 s") */
static void cab_arp_leer(void) {
    cab_narp = 0; cab_arp_leida = 1;
    if (orden_texto("red arp") < 0) return;
    char lin[160]; int pos = 0;
    while (orden_linea(&pos, lin, sizeof(lin)) && cab_narp < 4) {
        int j = 0; while (lin[j] == ' ') j++;
        if (lin[j] < '0' || lin[j] > '9') continue;
        int n = 0; while (lin[j] && n < 62) { cab_arp[cab_narp][n++] = lin[j++]; } cab_arp[cab_narp][n] = 0;
        cab_narp++;
    }
}

/* H204: nombre corto de una llamada al sistema, para LATENCIA (el orden de anillo3_abi.h) */
static const char *sys_nombre(uint32_t n) {
    static const char *const k[] = { "SALIR", "ESCRIBIR", "TIEMPO", "CEDER", "SUPERFICIE", "PRESENTAR", "MOVER",
        "EVENTO", "PANTALLA", "ESPERAR", "INFO", "CAPTURA", "EQUIPO", "ELEVAR", "WEB", "MEMORIA", "ARCHIVOS",
        "LANZAR", "ARGUMENTO", "AUDIO", "TAMAÑO", "SENSORES", "ORDEN", "RED", "VIDEO", "FLUJO" };
    if (n == 255) return "sin SYS";
    return n < sizeof(k) / sizeof(k[0]) ? k[n] : "?";
}

/* Geometria: cabecera, pestanas, grafica a la izquierda, datos a la derecha y pie. */
#define CM S(36)
static int cg_x, cg_y, cg_an, cg_al, cd_x, cd_an, pie_y, pes_y, pes_al;
static void cab_geometria(void) {
    int ancho = W - 2 * CM;
    pes_y = S(116); pes_al = S(50);
    cg_x = CM; cg_y = S(186); cg_an = ancho * 55 / 100; cg_al = S(632);
    cd_x = cg_x + cg_an + S(24); cd_an = W - CM - cd_x;
    pie_y = cg_y + cg_al + S(18);
}

/* ------------------------------------------------------------------ texto */

static void cab_anotar(const char *s) {
    if (cab_nlog == 3) { memcpy(cab_log[0], cab_log[1], 100); memcpy(cab_log[1], cab_log[2], 100); cab_nlog = 2; }
    struct txt t; t_ini(&t);
    if (info.fecha_ok) { t_dos(&t, info.hora); t_add(&t, ":"); t_dos(&t, info.minuto); t_add(&t, ":"); t_dos(&t, info.segundo); t_add(&t, "  "); }
    t_add(&t, s);
    int n = 0;
    while (t.s[n] && n < 99) { cab_log[cab_nlog][n] = t.s[n]; n++; }
    cab_log[cab_nlog][n] = 0;
    cab_nlog++;
}

/* ------------------------------------------------------------------ sonido */

/* H151: el tono y la sirena salen del motor de sonido comun (sonido.h), el mismo de los efectos del menu.
 * cab_au solo se usa para leer el estado de la cola y mostrarlo (y en el modo hexadecimal). */
static struct taek_audio cab_au;
static int cab_sonido, cab_au_falta;   /* cab_sonido: 0 nada, 1 tono, 2 sirena */
static long cab_sonido_t0;

static long cab_audio(uint32_t op) { cab_au.op = op; return taek_audio(&cab_au); }
static void cab_callar(void) { if (cab_sonido) sonido_callar(); cab_sonido = 0; }
static void cab_sonar(int tipo) {
    if (vm_sonando >= 0) { cab_anotar("Hay música sonando en el escritorio: el sonido de la cabina espera."); return; }
    if (snd_sin_tarjeta || cab_au_falta) { cab_au_falta = 1; cab_anotar("Este equipo no tiene salida de sonido."); return; }
    sonar(tipo == 1 ? SND_TONO : SND_SIRENA);
    cab_sonido = sonido_suena() ? tipo : 0;
    cab_sonido_t0 = taek_ms();
}
/* Cada cuadro: el motor encola; aqui solo se mira la cola para la grafica */
static void cab_bombear(void) {
    sonido_bombear();
    if (snd_sin_tarjeta && !cab_au_falta) { cab_au_falta = 1; cab_sonido = 0; cab_anotar("Este equipo no tiene salida de sonido."); }
    if (!cab_sonido) return;
    if (!sonido_suena()) { cab_sonido = 0; return; }
    cab_audio(TAEK_AU_ESTADO);
    hist_poner(&h_audio, (float)cab_au.ocupada / 1024.0f);
}
/* ------------------------------------------------------------------ alarma */

/* Real: la temperatura medida llega a 90 grados; se apaga por debajo de 75. Prueba: 3 s, y lo dice. */
static int cab_alarma_real, cab_alarma_prueba;
static long cab_prueba_hasta;
static int cab_alarma(void) { return cab_alarma_real || cab_alarma_prueba; }
static void cab_vigilar_temperatura(void) {
    if (!sen_hay() || !(sns.temp_ok || sns.temp_paquete_ok)) return;
    int c = sns.temp_paquete_ok ? sns.temp_paquete_c : sns.temp_nucleo_c;
    if (sns.temp_ok && sns.temp_nucleo_c > c) c = sns.temp_nucleo_c;
    if (!cab_alarma_real && c >= 90) { cab_alarma_real = 1; cab_anotar("ALERTA TÉRMICA: el procesador llegó a 90 °C."); }
    else if (cab_alarma_real && c < 75) { cab_alarma_real = 0; cab_anotar("Temperatura de vuelta por debajo de 75 °C."); }
}

/* ------------------------------------------------------------------ dibujo */

static void cab_marco(struct lienzo *l, int x, int y, int an, int al, const char *titulo, const char *codigo, uint32_t tono) {
    rect(l, x, y, an, al, OPACO(0x090A0Du));
    rect(l, x, y, an, S(2), OPACO(tono)); rect(l, x, y + al - S(2), an, S(2), OPACO(tono));
    rect(l, x, y, S(2), al, OPACO(tono)); rect(l, x + an - S(2), y, S(2), al, OPACO(tono));
    esquinas(l, x - S(6), y - S(6), an + S(12), al + S(12), S(26), S(4), OPACO(tono));
    int w = texto_ancho(S(16), titulo) + S(36);
    para(l, x + S(16), y + S(14), w, S(30), S(10), OPACO(tono));
    texto(l, x + S(32), y + S(35), S(16), NEGRO, 255, 0, titulo);
    if (codigo) texto(l, x + an - texto_ancho(S(13), codigo) - S(18), y + S(34), S(13), tono, 200, 0, codigo);
}

static int cab_fila(struct lienzo *l, int x, int y, int an, const char *k, const char *v, uint32_t tono) {
    texto(l, x, y, S(15), CAB_TINTA, 255, 0, k);
    texto(l, x + an * 40 / 100, y, S(17), tono, 255, 0, v);
    rect(l, x, y + S(10), an, 1, premul(44, NARANJA));
    return y + S(33);
}

static void cab_rejilla(struct lienzo *l, int x, int y, int an, int al) {
    rect(l, x, y, an, al, OPACO(0x07080Au));
    for (int k = 0; k < an; k += S(40)) rect(l, x + k, y, 1, al, premul(28, NARANJA));
    for (int k = 0; k < al; k += S(40)) rect(l, x, y + k, an, 1, premul(28, NARANJA));
    rect(l, x, y + al - 1, an, 1, premul(140, NARANJA));
    rect(l, x, y, 1, al, premul(140, NARANJA));
}

static void cab_curva(struct lienzo *l, int x, int y, int an, int al, const struct historia *h, float min, float max, uint32_t c) {
    if (h->n < 2 || max <= min) return;
    float px = 0, py = 0;
    for (int k = 0; k < h->n; k++) {
        float v = (hist_ver(h, k) - min) / (max - min);
        if (v > 1) v = 1;
        if (v < 0) v = 0;
        float qx = (float)x + (float)an * (float)(k + HIST - h->n) / (float)(HIST - 1);
        float qy = (float)(y + al) - SF(3) - v * (float)(al - S(8));
        if (k) {
            trazo(l, px, py, qx, qy, SF(7), premul(36, c), 1);
            trazo(l, px, py, qx, qy, SF(2.4f), OPACO(c), 1);
        }
        px = qx; py = qy;
    }
    anillo(l, (int)px, (int)py, S(6), 0, OPACO(c));
    anillo(l, (int)px, (int)py, S(12), S(9), premul(120, c));
}

/* Linea horizontal de referencia, a trazos. */
static void cab_umbral(struct lienzo *l, int x, int y, int an, const char *et, uint32_t c) {
    for (int k = 0; k < an; k += S(16)) rect(l, x + k, y, S(9), S(2), premul(200, c));
    if (et) texto(l, x + an - texto_ancho(S(13), et) - S(6), y - S(6), S(13), c, 230, 0, et);
}

static void cab_leyenda(struct lienzo *l, int x, int y, uint32_t c, const char *s) {
    rect(l, x, y - S(10), S(18), S(4), OPACO(c));
    texto(l, x + S(26), y, S(15), c, 255, 0, s);
}

static void cab_sin_sensores(struct lienzo *l, int x, int y, int an) {
    parrafo(l, x, y, an, S(18), S(28), AMBAR, 255, sen_r == -TAEK_ENOSYS
            ? "Sin dato: el núcleo que corre es anterior a H150 y no tiene SYS_SENSORES. Este menú funciona igual; instala la ISO de H150 para ver los sensores."
            : "Sin dato: SYS_SENSORES no respondió.");
}

/* La grafica de cada modulo, dentro del marco izquierdo. t: ms desde que se abrio la cabina. */
static void cab_grafica(long t) {
    struct lienzo *l = &l_cab;
    int x = cg_x + S(18), y = cg_y + S(64), an = cg_an - S(36), al = cg_al - S(130);
    cab_rejilla(l, x, y, an, al);
    rect(l, x - S(4), y + al + S(2), an + S(8), S(44), OPACO(0x090A0Du));   /* La leyenda se borra con la grafica */
    struct txt s;
    int ly = y + al + S(32);
    /* Barrido vertical que nunca para: la cabina esta viva aunque los datos no cambien */
    int bx = x + (int)((t / 6) % an);
    for (int k = 0; k < S(36); k++) rect(l, bx - k, y, 1, al, premul((uint32_t)(70 - k * 70 / S(36)), NARANJA));
    if (!sen_hay() && cab_mod != M_AUDIO && cab_mod != M_GPU) { cab_sin_sensores(l, x + S(20), y + S(50), an - S(40)); return; }
    switch (cab_mod) {
    case M_RED: {
        float max = hist_max(&h_rx, 10) > hist_max(&h_tx, 10) ? hist_max(&h_rx, 10) : hist_max(&h_tx, 10);
        max *= 1.2f;
        cab_curva(l, x, y, an, al, &h_rx, 0, max, CIAN);
        cab_curva(l, x, y, an, al, &h_tx, 0, max, AMBAR);
        t_ini(&s); t_add(&s, "RX "); t_num(&s, (uint64_t)(sen_rx_s + 0.5f)); t_add(&s, " paq/s"); cab_leyenda(l, x, ly, CIAN, s.s);
        t_ini(&s); t_add(&s, "TX "); t_num(&s, (uint64_t)(sen_tx_s + 0.5f)); t_add(&s, " paq/s"); cab_leyenda(l, x + S(260), ly, AMBAR, s.s);
        t_ini(&s); t_add(&s, "escala "); t_num(&s, (uint64_t)max); t_add(&s, " paq/s");
        texto(l, x + an - texto_ancho(S(13), s.s), ly, S(13), CAB_TINTA, 255, 0, s.s);
        break;
    }
    case M_STREAM: {
        float max = hist_max(&h_env, 4) * 1.2f;
        int n = h_env.n, bw = an / HIST;
        uint32_t c = sns.stream_conectado ? VERDE : ROJO;
        for (int k = 0; k < n; k++) {
            float v = hist_ver(&h_env, k) / max;
            int h = (int)(v * (float)(al - S(10)));
            int bxk = x + (k + HIST - n) * an / HIST;
            if (h > 0) rect(l, bxk + 1, y + al - h, bw > 2 ? bw - 1 : 1, h, OPACO(c));
        }
        t_ini(&s); t_dec1(&s, sen_env_s / 1024.0f); t_add(&s, " KiB/s de consola hacia el PC");
        cab_leyenda(l, x, ly, c, s.s);
        break;
    }
    case M_AUDIO: {
        /* La onda que este programa encola, o la linea quieta si no suena nada */
        int cy = y + al / 2;
        cab_umbral(l, x, cy, an, NULL, premul(120, NARANJA));
        float px = (float)x, py = (float)cy;
        for (int k = 1; k <= 160; k++) {
            float u = (float)k / 160.0f, v = 0;
            if (cab_sonido) {
                float hz = cab_sonido == 1 ? 440.0f : (((taek_ms() - cab_sonido_t0) / 160) % 2 ? 660.0f : 880.0f);
                float fase = u * hz * 0.012f * 6.2831853f + (float)t * 0.02f;
                while (fase > 3.14159265f) fase -= 6.2831853f;
                v = sen(fase) * 0.8f;
            }
            float qx = (float)x + u * (float)an, qy = (float)cy - v * (float)(al / 2 - S(10));
            trazo(l, px, py, qx, qy, SF(6), premul(40, CIAN), 1);
            trazo(l, px, py, qx, qy, SF(2.2f), OPACO(CIAN), 1);
            px = qx; py = qy;
        }
        cab_leyenda(l, x, ly, CIAN, cab_sonido ? (cab_sonido == 1 ? "Tono de 440 Hz generado aquí y encolado al núcleo" : "Sirena de alarma") : "Sin sonido de la cabina. [T] toca un tono de prueba");
        break;
    }
    case M_GPU: {
        float max = hist_max(&h_cuadro, 20) * 1.25f;
        cab_umbral(l, x, y + al - (int)(16.7f / max * (float)(al - S(8))), an, "16,7 ms = 60 cuadros/s", AMBAR);
        cab_curva(l, x, y, an, al, &h_cuadro, 0, max, CIAN);
        t_ini(&s); t_add(&s, "Intervalo entre cuadros de esta cabina: "); t_dec1(&s, cab_cuadro_ms); t_add(&s, " ms");
        cab_leyenda(l, x, ly, CIAN, s.s);
        break;
    }
    case M_NUCLEO: {
        /* El mapa real de los primeros bloques del monton: 4 filas de 16 */
        int cols = 16, filas = 4, gap = S(6);
        int cw = (an - S(20) - (cols - 1) * gap) / cols, chh = S(54);
        int n = (int)sns.heap_nmapa, foco = (int)((t / 90) % (n > 0 ? n : 1));
        for (int i = 0; i < cols * filas; i++) {
            int cx = x + S(10) + (i % cols) * (cw + gap), cy = y + S(16) + (i / cols) * (chh + gap);
            uint32_t c = i >= n ? 0x1A1C22u : sns.heap_mapa[i] == 2 ? ROJO : sns.heap_mapa[i] ? CIAN : 0x2B5A3Fu;
            rect(l, cx, cy, cw, chh, i >= n ? premul(255, c) : premul(i == foco ? 255 : 170, c));
            if (i == foco && i < n) { rect(l, cx - S(2), cy - S(2), cw + S(4), S(2), OPACO(BLANCO)); rect(l, cx - S(2), cy + chh, cw + S(4), S(2), OPACO(BLANCO)); }
        }
        int gy = y + S(16) + filas * (chh + gap) + S(14), gh = y + al - gy - S(6);
        if (gh > S(40)) {
            rect(l, x, gy - S(6), an, 1, premul(100, NARANJA));
            float max = hist_max(&h_heap, 1) * 1.15f;
            cab_curva(l, x, gy, an, gh, &h_heap, 0, max, AMBAR);
        }
        cab_leyenda(l, x, ly, CIAN, "en uso");
        cab_leyenda(l, x + S(140), ly, 0x2B5A3Fu, "libre");
        cab_leyenda(l, x + S(270), ly, ROJO, "canario roto");
        t_ini(&s); t_add(&s, "montón en uso: "); t_dec1(&s, (float)sns.heap_en_uso / 1048576.0f); t_add(&s, " MiB");
        cab_leyenda(l, x + S(470), ly, AMBAR, s.s);
        break;
    }
    case M_VFS: {
        /* Los 16 bytes del sector de arranque alrededor de VolumeFlags (0x6A y 0x6B) */
        int cw = (an - S(20)) / 16, cy = y + S(40), chh = S(70);
        texto(l, x + S(10), y + S(28), S(15), CAB_TINTA, 255, 0, vol_ult.leido ? "Región de arranque del volumen exFAT, bytes 0x60 a 0x6F" : "Sin lectura todavía: [L] lee la región de arranque");
        for (int i = 0; i < 16; i++) {
            int cx = x + S(10) + i * cw;
            int clave = i == 0x0A || i == 0x0B;
            uint32_t c = !vol_ult.leido ? 0x1A1C22u : clave ? ((vol_ult.banderas & 6) ? ROJO : VERDE) : 0x2A2E38u;
            rect(l, cx, cy, cw - S(4), chh, OPACO(c));
            struct txt b; t_ini(&b);
            if (vol_ult.leido) t_hexn(&b, vol_ult.bytes[i], 2); else t_add(&b, "--");
            texto(l, cx + (cw - S(4) - texto_ancho(S(20), b.s)) / 2, cy + S(44), S(20), clave ? NEGRO : BLANCO, 255, 0, b.s);
            t_ini(&b); t_add(&b, "6"); t_hexn(&b, (uint64_t)i, 1);
            texto(l, cx + (cw - S(4) - texto_ancho(S(12), b.s)) / 2, cy + chh + S(18), S(12), clave ? AMBAR : CAB_TINTA, 255, 0, b.s);
        }
        /* Radar: los discos por tamano; el barrido da vueltas */
        int rcx = x + an / 2, rcy = cy + chh + S(40) + (al - (cy + chh + S(40) - y)) / 2, rr = (al - (cy + chh + S(40) - y)) / 2 - S(14);
        if (rr > S(40)) {
            anillo(l, rcx, rcy, rr, rr - S(2), premul(150, NARANJA));
            anillo(l, rcx, rcy, rr * 2 / 3, rr * 2 / 3 - S(1), premul(90, NARANJA));
            anillo(l, rcx, rcy, rr / 3, rr / 3 - S(1), premul(90, NARANJA));
            float a = (float)t * 0.0021f;
            for (int k = 0; k < 18; k++) {
                float b = a - (float)k * 0.035f;
                trazo(l, (float)rcx, (float)rcy, (float)rcx + cosen(b) * (float)rr, (float)rcy + sen(b) * (float)rr, SF(3), premul((uint32_t)(150 - k * 8), NARANJA), 0);
            }
        }
        break;
    }
    case M_LATENCIA: {
        /* H204: cuantas veces por segundo pasa CPU 0 por el anillo del USB y por la pila de red. Si baja, alguien no
         * los atiende: es el hueco que la Fase 0 mide y que las fases B y C quieren cerrar. */
        if (!sen_v3()) { parrafo(l, x + S(20), y + S(50), an - S(40), S(18), S(28), AMBAR, 255, "Sin dato: el núcleo que corre es anterior a H204 y no mide la latencia. Instala la ISO de H204."); break; }
        float max = hist_max(&h_usb_ps, 10) > hist_max(&h_red_ps, 10) ? hist_max(&h_usb_ps, 10) : hist_max(&h_red_ps, 10);
        max *= 1.2f;
        cab_curva(l, x, y, an, al, &h_usb_ps, 0, max, CIAN);
        cab_curva(l, x, y, an, al, &h_red_ps, 0, max, AMBAR);
        t_ini(&s); t_add(&s, "USB "); t_num(&s, (uint64_t)(sen_usb_ps + 0.5f)); t_add(&s, " pasadas/s"); cab_leyenda(l, x, ly, CIAN, s.s);
        t_ini(&s); t_add(&s, "RED "); t_num(&s, (uint64_t)(sen_red_ps + 0.5f)); t_add(&s, " pasadas/s"); cab_leyenda(l, x + S(300), ly, AMBAR, s.s);
        t_ini(&s); t_add(&s, "escala "); t_num(&s, (uint64_t)max); t_add(&s, " /s");
        texto(l, x + an - texto_ancho(S(13), s.s), ly, S(13), CAB_TINTA, 255, 0, s.s);
        break;
    }
    case M_TERMICO: {
        int mn = 30, mx = sns.tjmax_c > 60 ? sns.tjmax_c : 100;
        if (!sns.temp_ok && !sns.temp_paquete_ok) { parrafo(l, x + S(20), y + S(50), an - S(40), S(18), S(28), AMBAR, 255, "Sin dato: este procesador no informa su temperatura por MSR (o no es Intel con sensor digital)."); break; }
        int gw = an - S(110);
        cab_umbral(l, x, y + al - (65 - mn) * (al - S(8)) / (mx - mn), gw, "65 °C", AMBAR);
        cab_umbral(l, x, y + al - (90 - mn) * (al - S(8)) / (mx - mn), gw, "90 °C ALARMA", ROJO);
        cab_curva(l, x, y, gw, al, &h_tempp, (float)mn, (float)mx, ROJO);
        cab_curva(l, x, y, gw, al, &h_temp, (float)mn, (float)mx, NARANJA);
        /* Termometro */
        int c = sns.temp_paquete_ok ? sns.temp_paquete_c : sns.temp_nucleo_c;
        int tx = x + an - S(70), tw = S(40);
        rect(l, tx, y + S(10), tw, al - S(20), OPACO(0x15161Bu));
        int fh = (c - mn) * (al - S(20)) / (mx - mn);
        if (fh < 0) fh = 0;
        if (fh > al - S(20)) fh = al - S(20);
        rect(l, tx + S(6), y + al - S(10) - fh, tw - S(12), fh, OPACO(tono_temp(c)));
        anillo(l, tx + tw / 2, y + al - S(8), S(26), 0, OPACO(tono_temp(c)));
        if (sns.temp_ok) { t_ini(&s); t_add(&s, "núcleo 0: "); t_num(&s, (uint64_t)(sns.temp_nucleo_c > 0 ? sns.temp_nucleo_c : 0)); t_add(&s, " °C"); cab_leyenda(l, x, ly, NARANJA, s.s); }
        if (sns.temp_paquete_ok) { t_ini(&s); t_add(&s, "paquete: "); t_num(&s, (uint64_t)(sns.temp_paquete_c > 0 ? sns.temp_paquete_c : 0)); t_add(&s, " °C"); cab_leyenda(l, x + S(260), ly, ROJO, s.s); }
        break;
    }
    }
}

/* ------------------------------------------------------------------ hexadecimal */

enum { B_DATO, B_CANARIO, B_SEGURO, B_IO, B_CRITICO, B_TEXTO };
struct campo { uint16_t off, tam; uint8_t tipo, num; const char *nombre, *desc; };
#define CSN(f, t, n, d) { (uint16_t)__builtin_offsetof(struct taek_sensores, f), (uint16_t)sizeof(((struct taek_sensores *)0)->f), t, n, #f, d }
#define CAU(f, t, n, d) { (uint16_t)__builtin_offsetof(struct taek_audio, f), (uint16_t)sizeof(((struct taek_audio *)0)->f), t, n, #f, d }
#define CIN(f, t, n, d) { (uint16_t)__builtin_offsetof(struct taek_info, f), (uint16_t)sizeof(((struct taek_info *)0)->f), t, n, #f, d }
static const struct campo campos_sen[] = {
    CSN(version, B_DATO, 1, "Versión de la estructura que entregó el núcleo (1 en H150)."),
    CSN(op, B_DATO, 1, "Lo que pidió el programa: 1 = leer también la región de arranque del volumen."),
    CSN(temp_ok, B_SEGURO, 1, "1 si el bit de lectura válida (31) de IA32_THERM_STATUS estaba encendido."),
    CSN(temp_paquete_ok, B_SEGURO, 1, "1 si el procesador tiene sensor de paquete (CPUID 06h, EAX bit 6)."),
    CSN(tjmax_leido, B_SEGURO, 1, "1 si TjMax salió del MSR 0x1A2; 0 si se supuso 100 °C."),
    CSN(tjmax_c, B_CRITICO, 1, "TjMax: la temperatura a la que el procesador se frena solo (PROCHOT)."),
    CSN(temp_nucleo_c, B_DATO, 1, "Temperatura del núcleo que atendió la llamada: TjMax menos la lectura digital (bits 22:16 del MSR 0x19C)."),
    CSN(temp_paquete_c, B_DATO, 1, "Temperatura del paquete entero (MSR 0x1B1)."),
    CSN(termico_estado, B_CRITICO, 1, "16 bits bajos de IA32_THERM_STATUS: bit 0 estado térmico, bit 2 PROCHOT, bit 4 temperatura crítica."),
    CSN(paquete_estado, B_CRITICO, 1, "16 bits bajos de IA32_PACKAGE_THERM_STATUS."),
    CSN(red_presente, B_SEGURO, 1, "1 si hay un controlador de red reconocido."),
    CSN(red_enlace, B_SEGURO, 1, "1 si el cable tiene enlace."),
    CSN(red_mbps, B_DATO, 1, "Velocidad del enlace en Mb/s."),
    CSN(red_duplex, B_DATO, 1, "1 si el enlace es dúplex completo."),
    CSN(red_proveedor, B_IO, 1, "Fabricante PCI del controlador (0x8086 = Intel)."),
    CSN(red_dispositivo, B_IO, 1, "Dispositivo PCI del controlador."),
    CSN(mac, B_IO, 0, "Dirección MAC, seis bytes en orden de red."),
    CSN(por_dhcp, B_DATO, 1, "1 si la IP llegó por DHCP."),
    CSN(ip_ok, B_SEGURO, 1, "1 si la red tiene IP configurada."),
    CSN(ip, B_IO, 0, "IP propia, cuatro bytes."),
    CSN(mascara, B_IO, 0, "Máscara de red."),
    CSN(puerta, B_IO, 0, "Puerta de enlace."),
    CSN(red_modelo, B_TEXTO, 0, "Nombre del modelo, tal como lo conoce el controlador."),
    CSN(rx_paquetes, B_DATO, 1, "Paquetes recibidos desde el arranque (contador del controlador)."),
    CSN(tx_paquetes, B_DATO, 1, "Paquetes enviados desde el arranque."),
    CSN(rx_bytes, B_DATO, 1, "Bytes recibidos."),
    CSN(tx_bytes, B_DATO, 1, "Bytes enviados."),
    CSN(rx_descartados, B_CRITICO, 1, "Paquetes que llegaron y se descartaron."),
    CSN(tx_errores, B_CRITICO, 1, "Envíos que fallaron."),
    CSN(stream_activo, B_SEGURO, 1, "1 si el stream de la consola está encendido (lan= en limine.conf)."),
    CSN(stream_conectado, B_SEGURO, 1, "1 si el PC (terminal_stream.py) contestó el saludo."),
    CSN(stream_puerto, B_IO, 1, "Puerto TCP del receptor en el PC (9153 = 0x23C1)."),
    CSN(reservado0, B_DATO, 1, "Reservado, en cero."),
    CSN(stream_ip, B_IO, 0, "IP del PC que recibe la consola."),
    CSN(reservado1, B_DATO, 1, "Reservado, en cero."),
    CSN(stream_enviado, B_DATO, 1, "Posición del registro de la consola ya entregada al PC, en bytes."),
    CSN(heap_capacidad, B_DATO, 1, "Capacidad del montón del núcleo."),
    CSN(heap_en_uso, B_DATO, 1, "Bytes del montón en uso."),
    CSN(heap_bloques_activos, B_DATO, 1, "Bloques del montón en uso."),
    CSN(heap_bloques_libres, B_DATO, 1, "Bloques libres en la lista."),
    CSN(canario_inicio, B_CANARIO, 1, "Canario que abre cada bloque del montón: 0x7AEECC05 (\"TAEK OS\"). Si cambia, alguien escribió fuera de su bloque."),
    CSN(canario_fin, B_CANARIO, 1, "Canario que cierra cada bloque: 0xCAFEBABEDEAD1000."),
    CSN(huevo_canario, B_CANARIO, 1, "Canario del huevo de estabilidad: 0xDEADBEEFCAFECAFE."),
    CSN(heap_canarios_ok, B_SEGURO, 1, "1 si la auditoría recorrió todo el montón y todos los canarios están intactos."),
    CSN(huevo_magico, B_CANARIO, 1, "Número mágico del huevo: 0xEE66B007."),
    CSN(huevo_salud, B_DATO, 1, "Salud del huevo, de 100 a 0."),
    CSN(huevo_estado, B_DATO, 1, "0 intacto, 1 fisura, 2 agrietado, 3 quebrado."),
    CSN(heap_nmapa, B_DATO, 1, "Bloques que trae el mapa (hasta 64)."),
    CSN(heap_total_bloques, B_DATO, 1, "Bloques en toda la lista del montón."),
    CSN(heap_mapa, B_DATO, 0, "Mapa de los primeros bloques: 0 libre, 1 en uso, 2 canario roto."),
    CSN(paginas_totales, B_DATO, 1, "Páginas de 4 KiB que maneja el núcleo."),
    CSN(paginas_libres, B_DATO, 1, "Páginas libres."),
    CSN(vol_leido, B_SEGURO, 1, "1 si se leyó la región de arranque del volumen exFAT."),
    CSN(vol_lba, B_IO, 1, "Sector leído: el inicio de la partición (2048 en el Kingston con Ventoy)."),
    CSN(vol_banderas, B_SEGURO, 1, "VolumeFlags, offset 0x6A del sector: 00 00 es limpio. Bit 1 sucio, bit 2 fallo de medio: así TAEK no escribe."),
    CSN(vol_bytes, B_DATO, 0, "Bytes 0x60 a 0x6F del sector de arranque, crudos."),
    CSN(reservado2, B_DATO, 1, "Reservado, en cero."),
    CSN(usb_max_ms, B_CRITICO, 1, "Mayor hueco, en ms, entre dos pasadas de CPU 0 por el anillo de eventos del USB. La meta de la Fase B es menos de 5."),
    CSN(usb_max_sys_ini, B_DATO, 1, "SYS_* en curso en la pasada que abrió ese hueco; 255 = ninguna (el programa o la consola)."),
    CSN(usb_max_sys_fin, B_DATO, 1, "SYS_* en curso en la pasada que lo cerró: suele ser la que vuelve a atender, no la culpable."),
    CSN(usb_max_cuando_ms, B_DATO, 1, "Cuándo se cerró, en ms desde el arranque: sirve para buscarlo en la consola."),
    CSN(usb_pasadas, B_DATO, 1, "Pasadas por el anillo desde el arranque o desde la última puesta a cero."),
    CSN(usb_reentradas, B_CRITICO, 1, "Pasadas que empezaron dentro de otra (llamada anidada). Si queda en 0, la reentrada no es la causa."),
    CSN(usb_eventos_max, B_DATO, 1, "Más eventos sacados en una pasada. El anillo tiene 64: cerca de 64 es que casi se llenó."),
    CSN(usb_anillo_lleno, B_CRITICO, 1, "Veces que el controlador avisó Event Ring Full (código 21): se perdieron eventos."),
    CSN(usb_cambios_puerto, B_DATO, 1, "Port Status Change Events recibidos."),
    CSN(usb_reintentos, B_DATO, 1, "Reintentos de reset de puerto hechos (H204)."),
    CSN(usb_recuperados, B_SEGURO, 1, "Puertos cuyo enlace volvió con un reintento."),
    CSN(usb_perdidos, B_CRITICO, 1, "Puertos sin enlace tras 3 reintentos: hay que desenchufar y volver a enchufar."),
    CSN(red_max_ms, B_CRITICO, 1, "Mayor hueco, en ms, entre dos pasadas por la pila de red (WiFi, TCP, TLS)."),
    CSN(red_max_sys_ini, B_DATO, 1, "SYS_* en curso al abrirse el hueco de la red; 255 = ninguna."),
    CSN(red_max_sys_fin, B_DATO, 1, "SYS_* en curso al cerrarse."),
    CSN(reservado3, B_DATO, 1, "Reservado, en cero."),
    CSN(red_max_cuando_ms, B_DATO, 1, "Cuándo se cerró, en ms desde el arranque."),
    CSN(red_pasadas, B_DATO, 1, "Pasadas por la pila de red."),
    CSN(hda_max_ms, B_CRITICO, 1, "Mayor hueco del controlador HDA desde que arrancó el sonido. A unos 370 ms el audio pierde el paso."),
    CSN(hda_saltos_tardios, B_DATO, 1, "Avances atrasados del DMA que se aceptaron (H203) en vez de declarar atasco."),
    CSN(raton_perdidos, B_CRITICO, 1, "Reportes del ratón USB que no cupieron en su cola."),
    CSN(teclas_perdidas, B_CRITICO, 1, "Teclas USB que no cupieron en el búfer."),
    CSN(sys_max_us, B_DATO, 0, "32 contadores de 4 bytes: la llamada más larga de cada SYS_*, en microsegundos, por número."),
    CSN(sucesos_total, B_DATO, 1, "Sucesos de puerto desde el arranque."),
    CSN(sucesos_n, B_DATO, 1, "Cuántos trae la tabla siguiente (hasta 8)."),
    CSN(sucesos, B_IO, 0, "Últimos sucesos de puerto, de 16 bytes: ms (8), PORTSC (4), puerto, tipo, intento y reservado."),
    CSN(huecos_total, B_DATO, 1, "Huecos de 250 ms o más (USB o red) desde el arranque. El cero no los borra."),
    CSN(huecos_n, B_DATO, 1, "Cuántos trae la tabla siguiente (hasta 8)."),
    CSN(huecos, B_IO, 0, "Últimos huecos grandes, de 16 bytes: cuándo terminó (8), cuánto duró (4), qué (1 USB, 2 red), SYS_* al abrirse, al cerrarse y reservado."),
};
static const struct campo campos_au[] = {
    CAU(op, B_DATO, 1, "Operación: 1 estado, 2 encolar, 3 iniciar, 4 detener, 5 volumen."),
    CAU(volumen_db, B_DATO, 1, "Volumen en dB (con signo)."),
    CAU(datos, B_IO, 1, "Dirección del PCM en la memoria de este programa (Anillo 3)."),
    CAU(bytes, B_DATO, 1, "Bytes que se pidieron encolar."),
    CAU(aceptados, B_DATO, 1, "Bytes que el núcleo aceptó."),
    CAU(ocupada, B_DATO, 1, "Bytes en la cola del núcleo."),
    CAU(libre, B_DATO, 1, "Espacio que queda en la cola."),
    CAU(sonando, B_SEGURO, 1, "1 si la tarjeta está reproduciendo."),
    CAU(hay_audio, B_SEGURO, 1, "1 si hay controlador de sonido (HDA o AC97)."),
    CAU(tocado_ms, B_DATO, 1, "Milisegundos ya enviados a los altavoces desde INICIAR."),
};
static const struct campo campos_info[] = {
    CIN(cpu, B_TEXTO, 0, "Nombre del procesador, de CPUID 0x80000002-4."),
    CIN(hito, B_TEXTO, 0, "Hito del núcleo que corre."),
    CIN(capa, B_TEXTO, 0, "Capa con que compone el compositor."),
    CIN(gpu_proveedor, B_TEXTO, 0, "Fabricante de la tarjeta de video (clase PCI 3)."),
    CIN(cpus, B_DATO, 1, "Hilos de CPU arrancados."),
    CIN(gpu_proveedor_id, B_IO, 1, "Fabricante PCI de la GPU (0x8086 = Intel)."),
    CIN(gpu_id, B_IO, 1, "Dispositivo PCI de la GPU (0x5917 = UHD 620 del i7-8650U)."),
    CIN(gpu_video_probada, B_SEGURO, 1, "1 solo si el video H.264 por GPU se probó en este modelo exacto."),
    CIN(ram_total, B_DATO, 1, "RAM usable, en bytes."),
    CIN(ram_libre, B_DATO, 1, "RAM libre, en bytes."),
    CIN(tsc_mhz, B_DATO, 1, "Frecuencia del contador de tiempo (TSC) en MHz."),
    CIN(activo_ms, B_DATO, 1, "Milisegundos desde el arranque."),
    CIN(anio, B_DATO, 1, "Año del reloj del equipo (CMOS)."),
    CIN(mes, B_DATO, 1, "Mes."), CIN(dia, B_DATO, 1, "Día."), CIN(hora, B_DATO, 1, "Hora."),
    CIN(minuto, B_DATO, 1, "Minuto."), CIN(segundo, B_DATO, 1, "Segundo."), CIN(fecha_ok, B_SEGURO, 1, "1 si el reloj se leyó bien."),
    CIN(ip, B_IO, 0, "IP propia."),
    CIN(red_presente, B_SEGURO, 1, "1 si hay controlador de red."), CIN(red_enlace, B_SEGURO, 1, "1 si hay enlace."),
    CIN(red_mbps, B_DATO, 1, "Velocidad del enlace."), CIN(red_ip_ok, B_SEGURO, 1, "1 si hay IP."),
    CIN(bateria_pct, B_DATO, 1, "Carga de la batería, -1 sin dato."), CIN(bateria_estado, B_DATO, 1, "Bit 0 descargando, bit 1 cargando, bit 2 crítica."),
    CIN(cargador, B_DATO, 1, "-1 desconocido, 0 no, 1 sí."), CIN(discos, B_DATO, 1, "Discos USB conectados."),
    CIN(smep, B_CRITICO, 1, "SMEP: el núcleo no ejecuta código de Anillo 3."), CIN(smap, B_CRITICO, 1, "SMAP: el núcleo no lee memoria de Anillo 3 sin pedirlo."),
    CIN(ancho, B_DATO, 1, "Ancho de la pantalla."), CIN(alto, B_DATO, 1, "Alto de la pantalla."),
};
#define NCAMPOS(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Lo que muestra el modo hexadecimal de cada modulo: que estructura y que tramo. */
struct vista_hex { const uint8_t *p; int ini, fin; const struct campo *c; int nc; const char *estructura; };
static void cab_vista(struct vista_hex *v) {
    v->p = (const uint8_t *)&sns; v->c = campos_sen; v->nc = NCAMPOS(campos_sen); v->estructura = "struct taek_sensores";
    switch (cab_mod) {
    case M_RED: v->ini = __builtin_offsetof(struct taek_sensores, red_presente); v->fin = __builtin_offsetof(struct taek_sensores, stream_activo); break;
    case M_STREAM: v->ini = __builtin_offsetof(struct taek_sensores, stream_activo); v->fin = __builtin_offsetof(struct taek_sensores, heap_capacidad); break;
    case M_AUDIO: v->p = (const uint8_t *)&cab_au; v->c = campos_au; v->nc = NCAMPOS(campos_au); v->estructura = "struct taek_audio"; v->ini = 0; v->fin = sizeof(cab_au); break;
    case M_GPU: v->p = (const uint8_t *)&info; v->c = campos_info; v->nc = NCAMPOS(campos_info); v->estructura = "struct taek_info"; v->ini = 0; v->fin = sizeof(info); break;
    case M_NUCLEO: v->ini = __builtin_offsetof(struct taek_sensores, heap_capacidad); v->fin = __builtin_offsetof(struct taek_sensores, vol_leido); break;
    case M_VFS: v->ini = __builtin_offsetof(struct taek_sensores, vol_leido); v->fin = TAEK_SENSORES_V2_TAM; break;
    case M_LATENCIA: v->ini = TAEK_SENSORES_V2_TAM; v->fin = sen_v3() ? (int)sizeof(sns) : TAEK_SENSORES_V2_TAM + 16; break;
    default: v->ini = 0; v->fin = __builtin_offsetof(struct taek_sensores, red_presente); break;
    }
}
static const struct campo *cab_campo(const struct vista_hex *v, int off) {
    for (int i = 0; i < v->nc; i++) if (off >= v->c[i].off && off < v->c[i].off + v->c[i].tam) return &v->c[i];
    return NULL;
}
static uint32_t cab_color_tipo(const struct campo *c) {
    if (!c) return 0x4A4E58u;
    switch (c->tipo) {
    case B_CANARIO: return NEON;
    case B_SEGURO: return VERDE;
    case B_IO: return CIAN;
    case B_CRITICO: return ROJO;
    case B_TEXTO: return 0xD8C8B0u;
    default: return 0xB8B8C4u;
    }
}

/* Celdas del volcado: medidas comunes al dibujo y al raton */
static int hx_x0, hx_y0, hx_celda, hx_ascii_x, hx_ascii_c, hx_fila_al, hx_filas;
static void cab_hex_medidas(void) {
    int x = cd_x + S(20), an = cd_an - S(40);
    hx_x0 = x + S(78);
    hx_celda = (an - S(78) - S(24)) * 100 / (16 * 100 + 16 * 45);
    hx_ascii_c = hx_celda * 45 / 100;
    hx_ascii_x = hx_x0 + 16 * hx_celda + S(20);
    hx_y0 = cg_y + S(124);
    hx_fila_al = S(27);
    hx_filas = (cg_al - S(124) - S(176)) / hx_fila_al;
}

static void cab_volcado(struct lienzo *l) {
    struct vista_hex v;
    cab_vista(&v);
    cab_hex_medidas();
    if (cab_cursor < v.ini || cab_cursor >= v.fin) cab_cursor = v.ini;
    int fila0 = v.ini / 16, filan = (v.fin + 15) / 16, cfila = cab_cursor / 16;
    if (cab_scroll < fila0 || cfila < cab_scroll) cab_scroll = cfila < fila0 ? fila0 : cfila;
    if (cfila >= cab_scroll + hx_filas) cab_scroll = cfila - hx_filas + 1;
    if (cab_scroll < fila0) cab_scroll = fila0;
    int x = cd_x + S(20);
    struct txt s;
    t_ini(&s); t_add(&s, v.estructura); t_add(&s, " +0x"); t_hexn(&s, (uint64_t)v.ini, 4); t_add(&s, "..0x"); t_hexn(&s, (uint64_t)v.fin, 4);
    texto(l, x, cg_y + S(72), S(14), CAB_TINTA, 255, 0, s.s);
    for (int k = 0; k < 16; k++) { t_ini(&s); t_hexn(&s, (uint64_t)k, 2); texto(l, hx_x0 + k * hx_celda, cg_y + S(98), S(13), NARANJA, 200, 0, s.s); }
    rect(l, x, cg_y + S(105), cd_an - S(40), 1, premul(90, NARANJA));
    for (int r = 0; r < hx_filas && cab_scroll + r < filan; r++) {
        int fila = cab_scroll + r, y = hx_y0 + r * hx_fila_al;
        t_ini(&s); t_hexn(&s, (uint64_t)fila * 16, 4);
        texto(l, x, y, S(15), NARANJA, 255, 0, s.s);
        for (int k = 0; k < 16; k++) {
            int off = fila * 16 + k;
            if (off < v.ini || off >= v.fin) continue;
            const struct campo *c = cab_campo(&v, off);
            uint8_t b = v.p[off];
            uint32_t col = cab_color_tipo(c);
            int cx = hx_x0 + k * hx_celda;
            if (off == cab_cursor) rect(l, cx - S(4), y - S(18), hx_celda - S(2), S(24), OPACO(NARANJA));
            else if (c && cab_campo(&v, cab_cursor) == c) rect(l, cx - S(4), y - S(18), hx_celda - S(2), S(24), premul(60, NARANJA));
            t_ini(&s); t_hexn(&s, b, 2);
            texto(l, cx, y, S(16), off == cab_cursor ? NEGRO : col, c ? 255 : 120, 0, s.s);
            char a[2] = { (char)(b >= 32 && b < 127 ? b : '.'), 0 };
            texto(l, hx_ascii_x + k * hx_ascii_c, y, S(15), off == cab_cursor ? NARANJA : (b >= 32 && b < 127 ? 0xD8C8B0u : 0x4A4E58u), 255, 0, a);
        }
    }
    /* Decodificador del byte bajo el cursor */
    int dy = cg_y + cg_al - S(170);
    rect(l, cd_x + S(12), dy - S(14), cd_an - S(24), 1, premul(140, NARANJA));
    const struct campo *c = cab_campo(&v, cab_cursor);
    uint8_t b = v.p[cab_cursor];
    t_ini(&s); t_add(&s, "+0x"); t_hexn(&s, (uint64_t)cab_cursor, 4); t_add(&s, "  ");
    if (c) {
        t_add(&s, c->nombre);
        if (c->tam > 1) { t_add(&s, "  [byte "); t_num(&s, (uint64_t)(cab_cursor - c->off + 1)); t_add(&s, " de "); t_num(&s, c->tam); t_add(&s, "]"); }
    } else t_add(&s, "relleno de alineación");
    texto(l, x, dy + S(10), S(17), c ? cab_color_tipo(c) : CAB_TINTA, 255, 0, s.s);
    t_ini(&s); t_add(&s, "0x"); t_hexn(&s, b, 2); t_add(&s, "  ·  "); t_num(&s, b); t_add(&s, "  ·  ");
    for (int i = 7; i >= 0; i--) { t_add(&s, (b >> i) & 1 ? "1" : "0"); if (i == 4) t_add(&s, " "); }
    texto(l, x, dy + S(40), S(17), BLANCO, 255, 0, s.s);
    if (c && c->num && c->tam <= 8) {
        uint64_t val = 0;
        for (int i = c->tam - 1; i >= 0; i--) val = val << 8 | v.p[c->off + i];
        int neg = c->tam == 4 && (val & 0x80000000u) && (c->off == __builtin_offsetof(struct taek_info, bateria_pct) || v.c != campos_info);
        t_ini(&s); t_add(&s, "Valor del campo: ");
        if (neg && (int32_t)val < 0) { t_add(&s, "-"); t_num(&s, (uint64_t)(-(int64_t)(int32_t)val)); }
        else t_num(&s, val);
        t_add(&s, "  (0x"); t_hexn(&s, val, c->tam * 2); t_add(&s, ")");
        texto(l, x, dy + S(68), S(15), AMBAR, 255, 0, s.s);
    } else if (c && c->tipo == B_TEXTO) {
        char tx[60]; int n = 0;
        while (n < 59 && n < c->tam && v.p[c->off + n]) { uint8_t ch = v.p[c->off + n]; tx[n] = (char)(ch >= 32 && ch < 127 ? ch : '.'); n++; }
        tx[n] = 0;
        t_ini(&s); t_add(&s, "Texto: \""); t_add(&s, tx); t_add(&s, "\"");
        texto(l, x, dy + S(68), S(15), AMBAR, 255, 0, s.s);
    }
    parrafo(l, x, dy + S(96), cd_an - S(40), S(14), S(20), 0xC8C0B4u, 255, c ? c->desc : "Bytes que el compilador deja entre campos para alinearlos; no significan nada.");
}

/* Los datos de cada modulo, en el marco derecho */
static void cab_datos(struct lienzo *l) {
    int x = cd_x + S(20), y = cg_y + S(84), an = cd_an - S(40);
    struct txt s;
    if (cab_hex) { cab_volcado(l); return; }
    if (!sen_hay() && cab_mod != M_AUDIO && cab_mod != M_GPU && cab_mod != M_VFS) { cab_sin_sensores(l, x, y, an); return; }
    switch (cab_mod) {
    case M_RED:
        if (!sns.red_presente) { y = cab_fila(l, x, y, an, "Controlador", "Sin controlador reconocido", ROJO); break; }
        t_ini(&s); t_add(&s, sns.red_modelo[0] ? sns.red_modelo : "Ethernet"); t_add(&s, " "); t_hexn(&s, sns.red_proveedor, 4); t_add(&s, ":"); t_hexn(&s, sns.red_dispositivo, 4);
        y = cab_fila(l, x, y, an, "Controlador", s.s, BLANCO);
        t_ini(&s);
        if (sns.red_enlace) { t_add(&s, "ACTIVO · "); t_num(&s, sns.red_mbps); t_add(&s, sns.red_duplex ? " Mb/s dúplex" : " Mb/s semidúplex"); } else t_add(&s, "SIN CABLE");
        y = cab_fila(l, x, y, an, "Enlace", s.s, sns.red_enlace ? VERDE : ROJO);
        t_ini(&s); for (int i = 0; i < 6; i++) { if (i) t_add(&s, ":"); t_hexn(&s, sns.mac[i], 2); }
        y = cab_fila(l, x, y, an, "MAC", s.s, CIAN);
        t_ini(&s); if (sns.ip_ok) { t_ip4(&s, sns.ip); t_add(&s, sns.por_dhcp ? "  (DHCP)" : "  (fija)"); } else t_add(&s, "Sin IP");
        y = cab_fila(l, x, y, an, "IP", s.s, sns.ip_ok ? CIAN : AMBAR);
        if (sns.ip_ok) {
            t_ini(&s); t_ip4(&s, sns.mascara); y = cab_fila(l, x, y, an, "Máscara", s.s, BLANCO);
            t_ini(&s); t_ip4(&s, sns.puerta); y = cab_fila(l, x, y, an, "Puerta de enlace", s.s, BLANCO);
        }
        t_ini(&s); t_num(&s, sns.rx_paquetes); t_add(&s, " paq · "); t_bytes(&s, sns.rx_bytes); y = cab_fila(l, x, y, an, "Recibido", s.s, CIAN);
        t_ini(&s); t_num(&s, sns.tx_paquetes); t_add(&s, " paq · "); t_bytes(&s, sns.tx_bytes); y = cab_fila(l, x, y, an, "Enviado", s.s, AMBAR);
        t_ini(&s); t_num(&s, sns.rx_descartados); y = cab_fila(l, x, y, an, "Descartados (RX)", s.s, sns.rx_descartados ? AMBAR : VERDE);
        t_ini(&s); t_num(&s, sns.tx_errores); y = cab_fila(l, x, y, an, "Errores (TX)", s.s, sns.tx_errores ? ROJO : VERDE);
        /* H151: la cache ARP real ('red arp'), leida al entrar al modulo y tras [V] o [C] */
        for (int i = 0; i < cab_narp && y < cg_y + cg_al - S(30); i++) y = cab_fila(l, x, y, an, i ? "" : "Caché ARP", cab_arp[i], CIAN);
        if (!cab_narp) y = cab_fila(l, x, y, an, "Caché ARP", cab_arp_leida ? "Vacía" : "Sin leer", CAB_TINTA);
        break;
    case M_STREAM:
        y = cab_fila(l, x, y, an, "Estado", !sns.stream_activo ? "APAGADO" : sns.stream_conectado ? "CONECTADO" : "ESPERANDO AL PC",
                     !sns.stream_activo ? CAB_TINTA : sns.stream_conectado ? VERDE : AMBAR);
        t_ini(&s); if (sns.stream_activo) { t_ip4(&s, sns.stream_ip); t_add(&s, ":"); t_num(&s, sns.stream_puerto); } else t_add(&s, "Sin destino");
        y = cab_fila(l, x, y, an, "Receptor (PC)", s.s, CIAN);
        t_ini(&s); if (sns.ip_ok) t_ip4(&s, sns.ip); else t_add(&s, "Sin IP");
        y = cab_fila(l, x, y, an, "Este equipo", s.s, CIAN);
        t_ini(&s); t_bytes(&s, sns.stream_enviado); y = cab_fila(l, x, y, an, "Consola entregada", s.s, BLANCO);
        t_ini(&s); t_dec1(&s, sen_env_s / 1024.0f); t_add(&s, " KiB/s"); y = cab_fila(l, x, y, an, "Ritmo", s.s, BLANCO);
        y += S(10);
        parrafo(l, x, y, an, S(14), S(21), CAB_TINTA, 255, "Todo lo que imprime la consola viaja por TCP al PC, que lo guarda en consola.raw; las teclas que manda el PC vuelven por el mismo canal.");
        break;
    case M_AUDIO: {
        if (!cab_sonido) cab_audio(TAEK_AU_ESTADO);
        y = cab_fila(l, x, y, an, "Salida", cab_au.hay_audio && !cab_au_falta ? "Controlador HDA o AC97 listo" : "Sin salida de sonido", cab_au.hay_audio && !cab_au_falta ? VERDE : ROJO);
        y = cab_fila(l, x, y, an, "Formato", "44 100 Hz · 16 bits · estéreo", BLANCO);
        t_ini(&s); if (cab_au.volumen_db < 0) { t_add(&s, "-"); t_num(&s, (uint64_t)(-cab_au.volumen_db)); } else t_num(&s, (uint64_t)cab_au.volumen_db); t_add(&s, " dB");
        y = cab_fila(l, x, y, an, "Volumen", s.s, BLANCO);
        t_ini(&s); t_bytes(&s, cab_au.ocupada); t_add(&s, " ocupados · "); t_bytes(&s, cab_au.libre); t_add(&s, " libres");
        y = cab_fila(l, x, y, an, "Cola del núcleo", s.s, CIAN);
        y = cab_fila(l, x, y, an, "Sonando", cab_au.sonando ? "SÍ" : "NO", cab_au.sonando ? VERDE : CAB_TINTA);
        y = cab_fila(l, x, y, an, "Música del escritorio", vm_sonando >= 0 ? "Sonando (la cabina no la pisa)" : "Parada", vm_sonando >= 0 ? AMBAR : CAB_TINTA);
        y += S(10);
        parrafo(l, x, y, an, S(14), S(21), CAB_TINTA, 255, "[T] genera aquí un tono de 440 Hz y lo encola al núcleo con SYS_AUDIO, igual que la música. [+] y [-] mueven el volumen del sistema; [M] apaga o enciende los efectos de la interfaz.");
        y += S(64);
        y = cab_fila(l, x, y, an, "Efectos de la interfaz", snd_activo ? "ENCENDIDOS" : "APAGADOS", snd_activo ? VERDE : CAB_TINTA);
        break;
    }
    case M_GPU:
        t_ini(&s); if (info.gpu_proveedor_id) { t_add(&s, info.gpu_proveedor); t_add(&s, " "); t_hexn(&s, info.gpu_proveedor_id, 4); t_add(&s, ":"); t_hexn(&s, info.gpu_id, 4); } else t_add(&s, "No detectada");
        y = cab_fila(l, x, y, an, "Adaptador", s.s, CIAN);
        y = cab_fila(l, x, y, an, "Video H.264 por GPU", info.gpu_video_probada ? "Probado en este equipo (VCS)" : "Por software", info.gpu_video_probada ? VERDE : AMBAR);
        t_ini(&s); t_num(&s, info.ancho); t_add(&s, " x "); t_num(&s, info.alto); y = cab_fila(l, x, y, an, "Pantalla", s.s, BLANCO);
        y = cab_fila(l, x, y, an, "Compositor", info.capa[0] ? info.capa : "CPU", BLANCO);
        t_ini(&s); t_dec1(&s, cab_cuadro_ms); t_add(&s, " ms entre cuadros"); y = cab_fila(l, x, y, an, "Esta cabina", s.s, CIAN);
        y += S(10);
        parrafo(l, x, y, an, S(14), S(21), CAB_TINTA, 255, "La interfaz dibuja por CPU en Anillo 3 y el compositor del núcleo presenta. El video por GPU lo decodifica el núcleo al reproducir; esta cabina no lo mide.");
        break;
    case M_NUCLEO:
        y = cab_fila(l, x, y, an, "Canarios del montón", sns.heap_canarios_ok ? "INTACTOS" : "ROTO", sns.heap_canarios_ok ? VERDE : ROJO);
        t_ini(&s); t_add(&s, "0x"); t_hexn(&s, sns.canario_inicio, 8); t_add(&s, " / 0x"); t_hexn(&s, sns.canario_fin, 16);
        y = cab_fila(l, x, y, an, "Canario inicio/fin", s.s, NEON);
        t_ini(&s); t_bytes(&s, sns.heap_en_uso); t_add(&s, " de "); t_bytes(&s, sns.heap_capacidad);
        y = cab_fila(l, x, y, an, "Montón en uso", s.s, BLANCO);
        t_ini(&s); t_num(&s, sns.heap_bloques_activos); t_add(&s, " en uso · "); t_num(&s, sns.heap_bloques_libres); t_add(&s, " libres");
        y = cab_fila(l, x, y, an, "Bloques", s.s, BLANCO);
        {
            static const char *const est[] = { "INTACTO", "FISURA", "AGRIETADO", "QUEBRADO" };
            t_ini(&s); t_add(&s, est[sns.huevo_estado & 3]); t_add(&s, " · salud "); t_num(&s, sns.huevo_salud); t_add(&s, " %");
            y = cab_fila(l, x, y, an, "El huevo", s.s, sns.huevo_estado ? AMBAR : VERDE);
        }
        t_ini(&s); t_add(&s, "0x"); t_hexn(&s, sns.huevo_magico, 8); y = cab_fila(l, x, y, an, "Mágico del huevo", s.s, NEON);
        t_ini(&s); t_num(&s, sns.paginas_libres); t_add(&s, " de "); t_num(&s, sns.paginas_totales); y = cab_fila(l, x, y, an, "Páginas libres", s.s, BLANCO);
        t_ini(&s); t_add(&s, "SMEP "); t_add(&s, info.smep ? "sí" : "no"); t_add(&s, " · SMAP "); t_add(&s, info.smap ? "sí" : "no");
        y = cab_fila(l, x, y, an, "Protección", s.s, info.smep && info.smap ? VERDE : AMBAR);
        y += S(10);
        parrafo(l, x, y, an, S(14), S(21), CAB_TINTA, 255, "Cada lectura recorre toda la lista del montón y comprueba los dos canarios de cada bloque. Si uno estuviera roto, el huevo se quiebra y el núcleo se detiene con su autopsia.");
        break;
    case M_VFS: {
        static struct taek_disco_usb d[6];
        static struct taek_archivos a;
        static int nd = -1;
        static long leido_t;
        if (nd < 0 || taek_ms() - leido_t > 5000) {
            memset(&a, 0, sizeof(a));
            a.op = TAEK_AR_DISCOS; a.destino = (uint64_t)(uintptr_t)d; a.capacidad = 6;
            nd = taek_archivos(&a) == 0 ? (int)a.n : 0;
            leido_t = taek_ms();
        }
        for (int i = 0; i < nd && i < 4; i++) {
            t_ini(&s); t_num(&s, (d[i].bytes + (1ull << 29)) >> 30); t_add(&s, " GB · "); t_add(&s, d[i].listo ? (d[i].montado ? "montado" : "listo") : "sin responder");
            char k[48]; int n = 0; while (d[i].nombre[n] && n < 22) { k[n] = d[i].nombre[n]; n++; } k[n] = 0;
            y = cab_fila(l, x, y, an, k, s.s, d[i].listo ? CIAN : AMBAR);
        }
        if (!nd) y = cab_fila(l, x, y, an, "Discos USB", "Ninguno", AMBAR);
        y = cab_fila(l, x, y, an, "Volumen abierto", a.fs[0] ? a.fs : "Ninguno", BLANCO);
        if (vol_ult.leido) {
            t_ini(&s); t_num(&s, vol_ult.lba); y = cab_fila(l, x, y, an, "Sector leído (LBA)", s.s, CIAN);
            t_ini(&s); t_hexn(&s, vol_ult.bytes[0x0A], 2); t_add(&s, " "); t_hexn(&s, vol_ult.bytes[0x0B], 2);
            t_add(&s, (vol_ult.banderas & 6) ? "  SUCIO: TAEK no escribe" : "  LIMPIO");
            y = cab_fila(l, x, y, an, "Offset 0x6A", s.s, (vol_ult.banderas & 6) ? ROJO : VERDE);
            t_ini(&s); t_dos(&s, vol_ult.hora); t_add(&s, ":"); t_dos(&s, vol_ult.minuto); t_add(&s, ":"); t_dos(&s, vol_ult.segundo);
            y = cab_fila(l, x, y, an, "Leído a las", s.s, CAB_TINTA);
        } else y = cab_fila(l, x, y, an, "Offset 0x6A", sen_hay() ? "[L] para leerlo" : "Sin dato (núcleo anterior)", AMBAR);
        y += S(10);
        parrafo(l, x, y, an, S(14), S(21), CAB_TINTA, 255, "Leer solo mira el sector: no escribe nada. Con la bandera sucia, el núcleo se niega a montar para escribir, y así debe ser.");
        break;
    }
    case M_TERMICO: {
        t_ini(&s);
        if (sns.temp_ok) { t_num(&s, (uint64_t)(sns.temp_nucleo_c > 0 ? sns.temp_nucleo_c : 0)); t_add(&s, " °C"); } else t_add(&s, "Sin dato");
        y = cab_fila(l, x, y, an, "Núcleo 0", s.s, sns.temp_ok ? tono_temp(sns.temp_nucleo_c) : CAB_TINTA);
        t_ini(&s);
        if (sns.temp_paquete_ok) { t_num(&s, (uint64_t)(sns.temp_paquete_c > 0 ? sns.temp_paquete_c : 0)); t_add(&s, " °C"); } else t_add(&s, "Sin dato");
        y = cab_fila(l, x, y, an, "Paquete", s.s, sns.temp_paquete_ok ? tono_temp(sns.temp_paquete_c) : CAB_TINTA);
        t_ini(&s); t_num(&s, (uint64_t)sns.tjmax_c); t_add(&s, sns.tjmax_leido ? " °C (MSR 0x1A2)" : " °C (supuesto)");
        y = cab_fila(l, x, y, an, "TjMax", s.s, BLANCO);
        t_ini(&s); t_add(&s, "0x"); t_hexn(&s, sns.termico_estado, 4);
        if (sns.termico_estado & 4) t_add(&s, " · PROCHOT visto");
        if (sns.termico_estado & 16) t_add(&s, " · CRÍTICA");
        y = cab_fila(l, x, y, an, "IA32_THERM_STATUS", s.s, (sns.termico_estado & 0x15) ? ROJO : VERDE);
        y = cab_fila(l, x, y, an, "Alarma", cab_alarma_real ? "ACTIVA (90 °C o más)" : cab_alarma_prueba ? "PRUEBA EN CURSO" : "En guardia: suena a 90 °C", cab_alarma() ? ROJO : VERDE);
        y = cab_fila(l, x, y, an, "Ventilador", "Sin dato: lo gobierna el firmware (EC)", CAB_TINTA);
        y += S(10);
        parrafo(l, x, y, an, S(14), S(21), CAB_TINTA, 255, "La temperatura es la del sensor digital del procesador. TAEK no controla el ventilador: si llega a TjMax, el propio procesador se frena (PROCHOT).");
        break;
    }
    case M_LATENCIA: {
        if (!sen_v3()) { y = cab_fila(l, x, y, an, "Núcleo", "Anterior a H204: sin medida", AMBAR); break; }
        /* Verde hasta 5 ms (la meta de la Fase B), ambar hasta 50, rojo despues */
        #define TONO_MS(v) ((v) <= 5 ? VERDE : (v) <= 50 ? AMBAR : ROJO)
        t_ini(&s); t_num(&s, sns.usb_max_ms); t_add(&s, " ms");
        if (sns.usb_max_ms) { t_add(&s, " · "); t_add(&s, sys_nombre(sns.usb_max_sys_ini)); t_add(&s, " > "); t_add(&s, sys_nombre(sns.usb_max_sys_fin)); }
        y = cab_fila(l, x, y, an, "USB: mayor hueco", s.s, TONO_MS(sns.usb_max_ms));
        t_ini(&s); t_num(&s, sns.usb_reentradas); t_add(&s, " · eventos por pasada "); t_num(&s, sns.usb_eventos_max); t_add(&s, " de 64");
        y = cab_fila(l, x, y, an, "USB: anidadas", s.s, sns.usb_reentradas ? ROJO : VERDE);
        t_ini(&s); t_num(&s, sns.usb_anillo_lleno);
        y = cab_fila(l, x, y, an, "Anillo lleno", s.s, sns.usb_anillo_lleno ? ROJO : VERDE);
        t_ini(&s); t_num(&s, sns.usb_cambios_puerto); t_add(&s, " · reintentos "); t_num(&s, sns.usb_reintentos);
        t_add(&s, " · vuelven "); t_num(&s, sns.usb_recuperados); t_add(&s, " · perdidos "); t_num(&s, sns.usb_perdidos);
        y = cab_fila(l, x, y, an, "Cambios de puerto", s.s, sns.usb_perdidos ? ROJO : sns.usb_reintentos ? AMBAR : BLANCO);
        t_ini(&s); t_num(&s, sns.raton_perdidos); t_add(&s, " ratón · "); t_num(&s, sns.teclas_perdidas); t_add(&s, " teclas");
        y = cab_fila(l, x, y, an, "HID perdido", s.s, sns.raton_perdidos || sns.teclas_perdidas ? AMBAR : VERDE);
        t_ini(&s); t_num(&s, sns.red_max_ms); t_add(&s, " ms");
        if (sns.red_max_ms) { t_add(&s, " · "); t_add(&s, sys_nombre(sns.red_max_sys_ini)); t_add(&s, " > "); t_add(&s, sys_nombre(sns.red_max_sys_fin)); }
        y = cab_fila(l, x, y, an, "Red: mayor hueco", s.s, TONO_MS(sns.red_max_ms));
        t_ini(&s); t_num(&s, sns.hda_max_ms); t_add(&s, " ms · tardíos "); t_num(&s, sns.hda_saltos_tardios);
        y = cab_fila(l, x, y, an, "Audio HDA", s.s, sns.hda_max_ms <= 15 ? VERDE : sns.hda_max_ms <= 370 ? AMBAR : ROJO);
        /* La llamada al sistema mas larga */
        int mejor = -1;
        for (int i = 0; i < 32; i++) if (sns.sys_max_us[i] && (mejor < 0 || sns.sys_max_us[i] > sns.sys_max_us[mejor])) mejor = i;
        t_ini(&s);
        if (mejor >= 0) { t_add(&s, sys_nombre((uint32_t)mejor)); t_add(&s, " "); t_dec1(&s, (float)sns.sys_max_us[mejor] / 1000.0f); t_add(&s, " ms"); }
        else t_add(&s, "Ninguna todavía");
        y = cab_fila(l, x, y, an, "SYS más larga", s.s, BLANCO);
        /* El ultimo suceso de puerto */
        if (sns.sucesos_n) {
            static const char *const tipo[] = { "?", "evento", "conexión", "desconexión", "sin enlace", "recuperado", "reintento fallido", "perdido" };
            const struct taek_suceso_puerto *u = &sns.sucesos[sns.sucesos_n - 1];
            t_ini(&s); t_add(&s, "p"); t_num(&s, u->puerto); t_add(&s, " "); t_add(&s, tipo[u->tipo < 8 ? u->tipo : 0]);
            t_add(&s, " · PORTSC "); t_hexn(&s, u->portsc, 8);
            if (u->portsc & (1u << 20)) t_add(&s, " · SOBRECORRIENTE");
            y = cab_fila(l, x, y, an, "Último suceso USB", s.s, u->tipo == 7 ? ROJO : u->tipo >= 4 ? AMBAR : CIAN);
        }
        /* El ultimo hueco grande (250 ms o mas), del USB o de la red */
        if (sns.huecos_n) {
            const struct taek_hueco *h = &sns.huecos[sns.huecos_n - 1];
            t_ini(&s); t_add(&s, h->que == 1 ? "USB " : "red "); t_num(&s, h->ms); t_add(&s, " ms · ");
            t_add(&s, sys_nombre(h->sys_ini)); t_add(&s, " > "); t_add(&s, sys_nombre(h->sys_fin));
            t_add(&s, " · hay "); t_num(&s, sns.huecos_total);
            y = cab_fila(l, x, y, an, "Último hueco grande", s.s, AMBAR);
        }
        #undef TONO_MS
        y += S(10);
        parrafo(l, x, y, an, S(14), S(21), CAB_TINTA, 255, "Hoy CPU 0 atiende el USB, la red y el sonido cuando puede. Esto mide cuánto tarda (Fase 0 del gestor de hilos). [Z] pone los máximos a cero antes de una prueba.");
        break;
    }
    }
}

static void cab_cabecera(struct lienzo *l, int parpadeo) {
    rect(l, 0, 0, W, S(100), OPACO(0x060607u));
    texto(l, CM + S(6), S(56), S(44), NARANJA, 255, INCL, "MAGI");
    int xx = CM + texto_ancho(S(44), "MAGI") + S(30);
    texto(l, xx, S(42), S(22), BLANCO, 255, 0, "CABINA TÁCTICA DE CONFIGURACIÓN");
    texto(l, xx, S(70), S(14), CAB_TINTA, 255, 0, "TAEK OS // NEKOSAKA // DATOS REALES DEL NÚCLEO, SOLO LECTURA");
    peligro(l, 0, S(100), W, S(8), S(14), OPACO(NARANJA), OPACO(0x060607u));
    /* Tres estados a la derecha: temperatura, red y huevo */
    struct txt s;
    int bw = S(190), bh = S(64), bx = W - CM - 3 * bw - 2 * S(12), by = S(18);
    for (int i = 0; i < 3; i++) {
        uint32_t c = CAB_TINTA;
        const char *k = i == 0 ? "TEMPERATURA" : i == 1 ? "RED" : "HUEVO";
        t_ini(&s);
        if (!sen_hay()) t_add(&s, "SIN DATO");
        else if (i == 0) {
            if (sns.temp_ok || sns.temp_paquete_ok) { int cc = sns.temp_paquete_ok ? sns.temp_paquete_c : sns.temp_nucleo_c; t_num(&s, (uint64_t)(cc > 0 ? cc : 0)); t_add(&s, " °C"); c = tono_temp(cc); }
            else t_add(&s, "SIN DATO");
        } else if (i == 1) { t_add(&s, sns.red_enlace ? "ENLACE" : "SIN CABLE"); c = sns.red_enlace ? VERDE : ROJO; }
        else { t_num(&s, sns.huevo_salud); t_add(&s, " %"); c = sns.heap_canarios_ok ? VERDE : ROJO; }
        int x = bx + i * (bw + S(12));
        int roja = i == 0 && cab_alarma() && parpadeo;
        rect(l, x, by, bw, bh, roja ? OPACO(ROJO) : OPACO(0x0D0E12u));
        rect(l, x, by, S(4), bh, OPACO(roja ? BLANCO : c));
        texto(l, x + S(16), by + S(22), S(12), roja ? BLANCO : CAB_TINTA, 255, 0, cab_alarma() && i == 0 ? (cab_alarma_real ? "ALERTA TÉRMICA" : "PRUEBA DE ALARMA") : k);
        texto(l, x + S(16), by + S(52), S(22), roja ? BLANCO : c, 255, 0, s.s);
    }
}

static void cab_pestanas(struct lienzo *l) {
    rect(l, 0, pes_y - S(4), W, pes_al + S(10), OPACO(CAB_FONDO));
    int gap = S(10), pw = (W - 2 * CM - (NMOD - 1) * gap) / NMOD;
    for (int i = 0; i < NMOD; i++) {
        int x = CM + i * (pw + gap);
        int act = i == cab_mod;
        para(l, x, pes_y, pw, pes_al, S(12), act ? OPACO(NARANJA) : OPACO(0x111216u));
        if (!act) rect(l, x + S(12), pes_y + pes_al - S(2), pw - S(12), S(2), premul(150, NARANJA));
        struct txt s; t_ini(&s); t_dos(&s, (uint64_t)(i + 1)); t_add(&s, " ");
        int xx = texto(l, x + S(22), pes_y + S(33), S(14), act ? NEGRO : NARANJA, 255, 0, s.s);
        texto(l, xx, pes_y + S(33), S(18), act ? NEGRO : 0xE6DCCBu, 255, 0, mod_nombre[i]);
    }
}

static void cab_pie(struct lienzo *l) {
    rect(l, 0, pie_y - S(4), W, H - pie_y + S(4), OPACO(CAB_FONDO));
    rect(l, CM, pie_y, W - 2 * CM, 1, premul(120, NARANJA));
    texto(l, CM, pie_y + S(26), S(13), NARANJA, 255, 0, "// REGISTRO");
    for (int i = 0; i < cab_nlog; i++) texto(l, CM, pie_y + S(52) + i * S(24), S(15), i == cab_nlog - 1 ? 0xE6DCCBu : CAB_TINTA, 255, 0, cab_log[i]);
    static const char *const ks[] = { "[ESC]", "[H]", "[A/D]", "[1-8]" }, *const vs[] = { "VOLVER", "HEX", "CAMBIAR", "MÓDULO" };
    int x = W - CM, y = H - S(36);
    for (int i = 0; i < 4; i++) {
        const char *v = i == 1 && cab_hex ? "TÁCTICO" : vs[i];
        x -= texto_ancho(S(14), ks[i]) + texto_ancho(S(14), v) + S(30);
        pastilla(l, x, y, ks[i], v, 0);
        x -= S(12);
    }
    int w = texto_ancho(S(14), mod_accion[cab_mod]) + S(30);
    x -= w;
    rect(l, x, y - S(18), w, S(26), OPACO(NARANJA));
    texto(l, x + S(15), y, S(14), NEGRO, 255, 0, mod_accion[cab_mod]);
}

/* Todo de nuevo: al abrir y al cambiar de modulo o de modo */
static void cab_pintar_todo(void) {
    struct lienzo *l = &l_cab;
    limpiar(l, OPACO(CAB_FONDO));
    /* Fondo: retícula hexagonal tenue */
    for (int y = S(110); y < H; y += S(52)) for (int x = (y / S(52)) % 2 ? S(30) : 0; x < W; x += S(60)) rect(l, x, y, S(2), S(2), premul(40, NARANJA));
    cab_cabecera(l, 0);
    cab_pestanas(l);
    struct txt c; t_ini(&c); t_add(&c, "MOD-0"); t_num(&c, (uint64_t)(cab_mod + 1));
    cab_marco(l, cg_x, cg_y, cg_an, cg_al, mod_titulo[cab_mod], c.s, NARANJA);
    cab_marco(l, cd_x, cg_y, cd_an, cg_al, cab_hex ? "VOLCADO HEX // BYTES DEL NÚCLEO" : "DATOS EN VIVO", cab_hex ? "RAW" : "LIVE", cab_hex ? CIAN : NARANJA);
    cab_grafica(taek_ms() - cab_t0);
    cab_datos(l);
    cab_pie(l);
    taek_presentar(s_cab.id, 0, 0, W, H);
}
static void cab_pintar_grafica(void) {
    cab_grafica(taek_ms() - cab_t0);
    taek_presentar(s_cab.id, cg_x + S(10), cg_y + S(56), cg_an - S(20), cg_al - S(60));
}
static void cab_pintar_datos(void) {
    rect(&l_cab, cd_x + S(6), cg_y + S(52), cd_an - S(12), cg_al - S(58), OPACO(0x090A0Du));
    cab_datos(&l_cab);
    taek_presentar(s_cab.id, cd_x, cg_y, cd_an, cg_al);
}
static void cab_pintar_pie(void) { cab_pie(&l_cab); taek_presentar(s_cab.id, 0, pie_y - S(4), W, H - pie_y + S(4)); }
static void cab_pintar_cabecera(int parpadeo) {
    cab_cabecera(&l_cab, parpadeo);
    /* Borde rojo en toda la pantalla mientras suena la alarma */
    uint32_t b = cab_alarma() && parpadeo ? OPACO(ROJO) : OPACO(CAB_FONDO);
    rect(&l_cab, 0, S(108), S(8), H - S(108), b); rect(&l_cab, W - S(8), S(108), S(8), H - S(108), b);
    rect(&l_cab, 0, H - S(8), W, S(8), b);
    taek_presentar(s_cab.id, 0, 0, W, S(108));
    taek_presentar(s_cab.id, 0, S(108), S(8), H - S(108));
    taek_presentar(s_cab.id, W - S(8), S(108), S(8), H - S(108));
    taek_presentar(s_cab.id, 0, H - S(8), W, S(8));
}

/* ------------------------------------------------------------------ obturadores */

static void cab_pintar_obturadores(void) {
    for (int i = 0; i < 2; i++) {
        struct lienzo *l = &l_obt[i];
        limpiar(l, OPACO(0x0A0806u));
        for (int y = 0; y < l->al; y += S(48)) for (int x = (y / S(48)) % 2 ? S(28) : 0; x < W; x += S(56)) rect(l, x, y, S(3), S(3), premul(70, NARANJA));
        int borde = i == 0 ? l->al - S(46) : 0;
        peligro(l, 0, borde, W, S(46), S(22), OPACO(NARANJA), OPACO(0x0A0806u));
        rect(l, 0, i == 0 ? borde - S(6) : S(52), W, S(3), OPACO(ROJO));
        if (i == 0) {
            int w = texto_ancho(S(150), "MAGI");
            texto(l, (W - w) / 2, l->al - S(110), S(150), NARANJA, 255, INCL, "MAGI");
        } else {
            const char *s = "CABINA TÁCTICA // ACCESO A CONFIGURACIÓN";
            texto(l, (W - texto_ancho(S(30), s)) / 2, S(130), S(30), 0xE6DCCBu, 255, 0, s);
            const char *s2 = "SENSORES · RED · STREAM · AUDIO · GPU · NÚCLEO · VFS · LATENCIA";
            texto(l, (W - texto_ancho(S(16), s2)) / 2, S(170), S(16), CAB_TINTA, 255, 0, s2);
        }
        taek_presentar(s_obt[i].id, 0, 0, l->an, l->al);
    }
}

static void cab_animar(void);
/* Cerrar: las dos hojas se juntan en el centro. Abrir: se separan. Lo de debajo sigue animado. */
static void cab_obturar(int cerrar, long dur, void (*debajo)(void)) {
    if (!cab_hay_obt) { if (debajo) debajo(); return; }
    int mitad = l_obt[0].al;
    taek_elevar(s_obt[0].id); taek_elevar(s_obt[1].id);
    long t0 = taek_ms();
    ef_cuadros = 0; ef_peor = 0;
    for (;;) {
        long ini = taek_ms(), t = ini - t0;
        float u = limitar01((float)t / (float)dur), e = cerrar ? sale(u) : 1.0f - suave(u);
        int abierto = (int)((float)mitad * (1.0f - e));
        taek_mover(s_obt[0].id, 0, -abierto);
        taek_mover(s_obt[1].id, 0, mitad + abierto);
        if (debajo) debajo();
        ef_cuadro(ini);
        if (u >= 1) break;
    }
    if (!cerrar) { ocultar(&s_obt[0]); ocultar(&s_obt[1]); }
}

/* ------------------------------------------------------------------ entrada y salida */

static void cab_cambiar(int m) {
    if (m == cab_mod) return;
    cab_mod = (m + NMOD) % NMOD;
    cab_scroll = -1;
    if (cab_mod == M_VFS && sen_hay() && !vol_ult.leido) { sen_leer(TAEK_SEN_LEER_VOLUMEN); cab_anotar("Región de arranque del volumen leída (solo lectura)."); }
    if (cab_mod == M_RED) cab_arp_leer();
    sonar(SND_TICK);
    cab_pintar_todo();
}

static void entrar_cabina(int modulo) {
    if (!cab_creada) {
        cab_creada = 1;
        if (crear(&s_cab, &l_cab, W, H, OCULTO, 0, 0)) { cab_creada = -1; }
        cab_hay_obt = cab_creada > 0 && !crear(&s_obt[0], &l_obt[0], W, H / 2, OCULTO, 0, 0) && !crear(&s_obt[1], &l_obt[1], W, H - H / 2, OCULTO, 0, 0);
        if (cab_hay_obt) cab_pintar_obturadores();
        cab_anotar("Cabina abierta. Datos de SYS_SENSORES, SYS_INFO, SYS_AUDIO y SYS_ARCHIVOS.");
    }
    if (cab_creada < 0) return;
    cab_geometria();
    cab_mod = modulo % NMOD;
    cab_scroll = -1;
    cab_t0 = taek_ms();
    sen_leer(cab_mod == M_VFS ? TAEK_SEN_LEER_VOLUMEN : 0);
    if (cab_mod == M_RED) cab_arp_leer();
    if (!sen_hay()) cab_anotar(sen_r == -TAEK_ENOSYS ? "Núcleo anterior a H150: sin SYS_SENSORES." : "SYS_SENSORES no respondió.");
    cab_vigilar_temperatura();
    /* Las hojas bajan sobre el menu, la cabina se pone detras y se abren */
    sonar(SND_OLEADA);
    cab_obturar(1, 320, estado == E_ESCANEO ? animar_escaneo_y_menu : animar_menu);
    if (estado == E_ESCANEO) { fx_mostrar(0); ocultar(&s_scan); }
    cab_pintar_todo();
    taek_mover(s_cab.id, 0, 0);
    taek_elevar(s_cab.id);
    estado = E_CABINA;
    cab_ult_sen = cab_ult_graf = cab_ult_cuadro = taek_ms();
    cab_obturar(0, 420, cab_animar);
}

/* H151: se vuelve a los modulos de CONFIGURACION (ocho desde H204), en el que se estaba */
static void salir_cabina(void) {
    cab_callar();
    cab_alarma_prueba = 0;
    sonar(SND_REVERSO);
    cab_obturar(1, 280, cab_animar);
    ocultar(&s_cab);
    estado = E_MENU;
    if (sel != 3 || !nivel) { sel = 3; nivel = 1; subsel = cab_mod; pintar_nivel_modo(ENTRA_CASCADA); }
    else { int v = subsel; subsel = cab_mod; if (v != subsel) { dibujar_fila(v); dibujar_fila(subsel); } dibujar_inspector(); }
    dibujar_pie();
    cab_obturar(0, 380, animar_menu);
}

/* Cada cuadro: sensores 4 veces por segundo, grafica a unos 15 cuadros, cabecera y alarma cuando toca. */
static void cab_animar(void) {
    long ahora = taek_ms();
    if (cab_ult_cuadro) {
        float d = (float)(ahora - cab_ult_cuadro);
        cab_cuadro_ms = cab_cuadro_ms ? cab_cuadro_ms * 0.85f + d * 0.15f : d;
    }
    cab_ult_cuadro = ahora;
    cab_bombear();
    if (cab_alarma_prueba && ahora > cab_prueba_hasta) { cab_alarma_prueba = 0; cab_anotar("Prueba de alarma terminada. La temperatura real no cambió."); cab_pintar_pie(); }
    if (ahora - cab_ult_sen >= 250) {
        cab_ult_sen = ahora;
        sen_leer(0);
        hist_poner(&h_cuadro, cab_cuadro_ms);
        int antes = cab_alarma_real;
        cab_vigilar_temperatura();
        if (cab_alarma_real != antes) cab_pintar_pie();
        if (!cab_hex || cab_mod != M_GPU) cab_pintar_datos();
    }
    if (ahora - cab_ult_graf >= 66) { cab_ult_graf = ahora; cab_pintar_grafica(); }
    if (ahora - cab_ult_alarma >= 250) {
        cab_ult_alarma = ahora;
        int p = (int)((ahora / 250) & 1);
        cab_pintar_cabecera(p);
        /* La sirena se repite mientras dure; sin tarjeta o con musica sonando, solo se ve (sin llenar el registro) */
        if (cab_alarma() && !cab_sonido && p && vm_sonando < 0 && !cab_au_falta) cab_sonar(2);
    }
}

static int cab_tecla(uint32_t k) {
    if (k == 27 || k == 8) { salir_cabina(); return 0; }
    if (k >= '1' && k < '1' + NMOD) { cab_cambiar((int)(k - '1')); return 0; }
    if (k == 'h' || k == 'H') {
        cab_hex = !cab_hex; cab_scroll = -1;
        cab_anotar(cab_hex ? "Modo hexadecimal: los bytes tal como los entregó el núcleo." : "Modo táctico.");
        cab_pintar_todo();
        return 0;
    }
    if (k == 'a' || k == 'A' || (!cab_hex && k == TAEK_TECLA_IZQUIERDA)) { cab_cambiar(cab_mod - 1); return 0; }
    if (k == 'd' || k == 'D' || (!cab_hex && k == TAEK_TECLA_DERECHA)) { cab_cambiar(cab_mod + 1); return 0; }
    if (cab_hex && (k == TAEK_TECLA_IZQUIERDA || k == TAEK_TECLA_DERECHA || k == TAEK_TECLA_ARRIBA || k == TAEK_TECLA_ABAJO)) {
        struct vista_hex v; cab_vista(&v);
        int d = k == TAEK_TECLA_IZQUIERDA ? -1 : k == TAEK_TECLA_DERECHA ? 1 : k == TAEK_TECLA_ARRIBA ? -16 : 16;
        int n = cab_cursor + d;
        if (n >= v.ini && n < v.fin) { cab_cursor = n; cab_pintar_datos(); }
        return 0;
    }
    /* H151: acciones reales que el prototipo simulaba: vaciar la cache ARP, pedir IP por DHCP, un eco al PC */
    if ((k == 'v' || k == 'V' || k == 'c' || k == 'C') && cab_mod == M_RED) {
        long r = orden_texto(k == 'v' || k == 'V' ? "red arp vaciar" : "red dhcp");
        char lin[160]; int pos = 0, ultima = 0;
        if (r < 0) cab_anotar(orden_error(r));
        else while (orden_linea(&pos, lin, sizeof(lin))) { int j = 0; while (lin[j] == ' ') j++; if (lin[j]) { cab_anotar(lin + j); if (++ultima >= 2) break; } }
        sonar(SND_OLEADA); sen_leer(0); cab_arp_leer(); cab_pintar_datos(); cab_pintar_pie();
        return 0;
    }
    if ((k == 'e' || k == 'E') && cab_mod == M_STREAM) {
        long r = orden_texto("transmitir Eco desde la cabina MAGI de TAEK OS");
        cab_anotar(r < 0 ? orden_error(r) : "Eco enviado al PC con 'transmitir' (sale en su terminal de Windows).");
        sonar(SND_OLEADA); cab_pintar_pie();
        return 0;
    }
    if (k == 'r' || k == 'R') {
        sen_leer(0);
        cab_anotar(cab_mod == M_NUCLEO ? (sns.heap_canarios_ok ? "Auditoría del montón: todos los canarios intactos." : "Auditoría del montón: CANARIO ROTO.") : "Lectura nueva de los sensores.");
        cab_pintar_datos(); cab_pintar_pie();
        return 0;
    }
    if ((k == 't' || k == 'T') && cab_mod == M_AUDIO) { cab_sonar(1); if (cab_sonido) cab_anotar("Tono de 440 Hz encolado al núcleo."); cab_pintar_pie(); return 0; }
    if ((k == '+' || k == '-') && cab_mod == M_AUDIO) {
        struct txt s; t_ini(&s); t_add(&s, "Volumen del sistema: ");
        int v = sonido_volumen(k == '+' ? 3 : -3);
        if (v < 0) { t_add(&s, "-"); t_num(&s, (uint64_t)(-v)); } else t_num(&s, (uint64_t)v);
        t_add(&s, " dB");
        cab_anotar(s.s); sonar(SND_TICK); cab_pintar_datos(); cab_pintar_pie();
        return 0;
    }
    if ((k == 'm' || k == 'M') && cab_mod == M_AUDIO) {
        snd_activo = !snd_activo;
        cab_anotar(snd_activo ? "Efectos de sonido de la interfaz encendidos." : "Efectos de sonido de la interfaz apagados.");
        cab_pintar_datos(); cab_pintar_pie();
        return 0;
    }
    if ((k == 'l' || k == 'L') && cab_mod == M_VFS) {
        sen_leer(TAEK_SEN_LEER_VOLUMEN);
        if (!sen_hay()) cab_anotar("Sin SYS_SENSORES: el núcleo es anterior a H150.");
        else if (!vol_ult.leido || taek_ms() - vol_ult.t > 1000) cab_anotar("No hay un volumen exFAT abierto que leer.");
        else cab_anotar((vol_ult.banderas & 6) ? "Offset 0x6A: volumen SUCIO. TAEK no escribirá en él." : "Offset 0x6A = 00 00: volumen limpio.");
        cab_pintar_todo();
        return 0;
    }
    /* H204: poner a cero los maximos de latencia antes de una prueba (lo mismo que 'latencia cero' en la terminal) */
    if ((k == 'z' || k == 'Z') && cab_mod == M_LATENCIA) {
        sen_leer(TAEK_SEN_CERO_LATENCIA);
        cab_anotar(sen_v3() ? "Máximos de latencia a cero: se miden desde ahora." : "Sin medida: el núcleo es anterior a H204.");
        sonar(SND_TICK); cab_pintar_datos(); cab_pintar_pie();
        return 0;
    }
    if ((k == 'p' || k == 'P') && cab_mod == M_TERMICO) {
        cab_alarma_prueba = 1; cab_prueba_hasta = taek_ms() + 3000;
        cab_anotar("Prueba de alarma durante 3 s. La temperatura que se muestra sigue siendo la real.");
        cab_pintar_pie(); cab_pintar_datos();
        return 0;
    }
    return 0;
}

static int cab_raton(const struct taek_evento *e, int subida) {
    if (subida & 2) { salir_cabina(); return 0; }
    if (cab_hex) {
        cab_hex_medidas();
        struct vista_hex v; cab_vista(&v);
        int r = (e->y - hx_y0 + S(20)) / hx_fila_al, k = (e->x - hx_x0 + S(4)) / hx_celda;
        if (e->y >= hx_y0 - S(20) && r < hx_filas && e->x >= hx_x0 - S(4) && k >= 0 && k < 16) {
            int off = (cab_scroll + r) * 16 + k;
            if (off >= v.ini && off < v.fin && off != cab_cursor) { cab_cursor = off; cab_pintar_datos(); }
        }
    }
    if ((subida & 1) && e->y >= pes_y && e->y < pes_y + pes_al) {
        int gap = S(10), pw = (W - 2 * CM - (NMOD - 1) * gap) / NMOD, i = (e->x - CM) / (pw + gap);
        if (i >= 0 && i < NMOD) cab_cambiar(i);
    }
    return 0;
}
