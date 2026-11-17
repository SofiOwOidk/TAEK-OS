/* Pantallas de Especificaciones y Bateria. Se incluye desde menu.c. Los paneles entran uno a uno, con rebote, y las barras se llenan. */

static long pan_t0;
static int pan_quieta;

/* Avance de 0 a 1 con un poco de rebote al final. */
static float avance(long t, long desde, long dur) {
    if (t <= desde) return 0;
    if (t >= desde + dur) return 1;
    float f = (float)(t - desde) / (float)dur - 1.0f;
    return 1.0f + f * f * (2.7f * f + 1.7f);
}

static void panel(struct lienzo *l, int x, int y, int an, int al, const char *titulo, uint32_t tono) {
    int corte = S(22);
    chaflan(l, x + S(8), y + S(8), an, al, corte, premul(110, tono));
    chaflan(l, x, y, an, al, corte, OPACO(tono));
    chaflan(l, x + S(2), y + S(2), an - S(4), al - S(4), corte - S(1), 0xF4000000u | NEGRO);
    esquinas(l, x + S(10), y + S(10), an - S(20), al - S(20), S(16), S(2), premul(170, CIAN));
    int w = texto_ancho(S(18), titulo) + S(26);
    para(l, x + S(20), y + S(16), w, S(28), S(8), OPACO(tono));
    texto(l, x + S(33), y + S(37), S(18), tono == NEON ? NEGRO : BLANCO, 255, 0, titulo);
}

static int dato(struct lienzo *l, int x, int y, int ancho, const char *k, const char *v) {
    texto(l, x, y, S(22), GRIS, 255, 0, k);
    texto(l, x + S(200), y, S(22), BLANCO, 255, 0, v[0] ? v : "No informado");
    rect(l, x, y + S(12), ancho, 1, premul(40, CIAN));
    return y + S(38);
}

static void barra(struct lienzo *l, int x, int y, int an, int al, float f, uint32_t tono) {
    if (f < 0) f = 0;
    if (f > 1.05f) f = 1.05f;
    para(l, x, y, an, al, S(8), premul(70, BLANCO));
    int n = 24, hueco = S(3), ancho = (an - S(8)) / n;
    for (int i = 0; i < n; i++)
        if ((float)i / (float)n < f) para(l, x + S(4) + i * ancho, y + S(3), ancho - hueco, al - S(6), S(6), OPACO(tono));
}

/* taek_info sin bateria la deja en "sin dato": se rellena con lo que acaba de leer taek_equipo. */
static void leer_equipo(void) {
    taek_equipo(&eq);
    taek_info(&info, 0);
    info.bateria_pct = eq.bat_pct; info.bateria_estado = eq.bat_estado; info.cargador = eq.bat_cargador;
    eq_leido = 1;
}

static void preparar_pantalla(int nuevo) {
    ocultar_capas_menu();
    ocultar(&s_crono); ocultar(&s_estado);
    fx_mostrar(0);
    estado = nuevo;
    leer_equipo();
    limpiar(&l_pan, 0);
    taek_mover(s_pan.id, 0, S(102));
    taek_presentar(s_pan.id, 0, 0, l_pan.an, l_pan.al);
    pan_t0 = taek_ms();
    pan_quieta = 0;
    dibujar_pie();
}

/* ---------------------------------------------------------- Especificaciones */

#define PX0 S(30)
#define PX1 (W / 2 + S(10))
#define PAN_AN (W / 2 - S(40))
#define PY(i) (S(26) + (i) * S(290))
#define PAN_AL S(270)

static int desliza(long t, int i) {
    float a = avance(t, 40 + i * 70, 340);
    return (int)((1.0f - a) * (float)(i % 2 ? W : -W) * 0.6f);
}

static void pantalla_especificaciones(long t) {
    if (pan_quieta) return;
    struct lienzo *l = &l_pan;
    limpiar(l, 0);
    struct txt s;
    int x, y, ancho = PAN_AN - S(70);

    x = PX0 + desliza(t, 0); y = PY(0);
    panel(l, x, y, PAN_AN, PAN_AL, "EQUIPO", ROJO);
    x += S(32); y += S(82);
    y = dato(l, x, y, ancho, "Fabricante", eq.fabricante);
    y = dato(l, x, y, ancho, "Modelo", eq.modelo);
    y = dato(l, x, y, ancho, "Formato", eq.formato);
    y = dato(l, x, y, ancho, "Firmware", eq.firmware);
    dato(l, x, y, ancho, "Fecha", eq.fecha_firmware);

    x = PX1 + desliza(t, 1); y = PY(0);
    panel(l, x, y, PAN_AN, PAN_AL, "PROCESADOR", NEON);
    x += S(32); y += S(82);
    cadena_cpu(&s, 46);
    texto(l, x, y, S(22), BLANCO, 255, 0, s.s);
    y += S(44);
    /* Un cuadro por hilo, encendiendose por turnos */
    int hilos = (int)info.cpus > 16 ? 16 : (int)info.cpus;
    for (int i = 0; i < hilos; i++) {
        float a = avance(t, 260 + i * 60, 160);
        para(l, x + i * S(56), y, S(48), S(34), S(8), a >= 1.0f ? OPACO(CIAN) : premul((uint32_t)(a < 0 ? 0 : a * 200.0f) + 30, CIAN));
    }
    y += S(68);
    t_ini(&s); t_num(&s, info.cpus); t_add(&s, " en uso");
    y = dato(l, x, y, ancho, "Hilos", s.s);
    t_ini(&s); t_num(&s, info.tsc_mhz); t_add(&s, " MHz medidos");
    y = dato(l, x, y, ancho, "Reloj", s.s);

    x = PX0 + desliza(t, 2); y = PY(1);
    panel(l, x, y, PAN_AN, PAN_AL, "MEMORIA", CIAN);
    x += S(32); y += S(78);
    uint64_t usada = info.ram_total > info.ram_libre ? info.ram_total - info.ram_libre : 0;
    float fm = info.ram_total ? (float)usada / (float)info.ram_total : 0;
    barra(l, x, y, ancho, S(34), fm * avance(t, 300, 500), CIAN);
    y += S(76);
    t_ini(&s); t_num(&s, usada >> 20); t_add(&s, " MiB en uso, "); t_num(&s, info.ram_libre >> 20); t_add(&s, " libres");
    y = dato(l, x, y, ancho, "Ahora", s.s);
    t_ini(&s);
    if (eq.ram_instalada_mib) { t_num(&s, eq.ram_instalada_mib); t_add(&s, " MiB en "); t_num(&s, eq.ram_modulos); t_add(&s, eq.ram_modulos == 1 ? " módulo" : " módulos"); }
    y = dato(l, x, y, ancho, "Instalada", s.s);
    t_ini(&s); t_add(&s, eq.ram_tipo);
    if (eq.ram_mts) { t_add(&s, " "); t_num(&s, eq.ram_mts); t_add(&s, " MT/s"); }
    dato(l, x, y, ancho, "Tipo", s.s);

    x = PX1 + desliza(t, 3); y = PY(1);
    panel(l, x, y, PAN_AN, PAN_AL, "GRÁFICOS", ROJO);
    x += S(32); y += S(82);
    cadena_gpu(&s);
    y = dato(l, x, y, ancho, "GPU", s.s);
    t_ini(&s); t_num(&s, info.ancho); t_add(&s, " x "); t_num(&s, info.alto);
    y = dato(l, x, y, ancho, "Pantalla", s.s);
    y = dato(l, x, y, ancho, "Video H.264", info.gpu_video_probada ? "GPU, probada en este equipo" : "Por software");
    dato(l, x, y, ancho, "Compositor", info.capa[0] ? info.capa : "CPU");

    x = PX0 + desliza(t, 4); y = PY(2);
    panel(l, x, y, PAN_AN, PAN_AL, "ALMACENAMIENTO", NEON);
    x += S(32); y += S(82);
    if (!eq.ndiscos) texto(l, x, y, S(22), GRIS, 255, 0, "Ningún disco USB listo.");
    for (uint32_t i = 0; i < eq.ndiscos; i++) {
        t_ini(&s); t_num(&s, eq.discos[i].gib); t_add(&s, " GiB");
        texto(l, x, y, S(22), BLANCO, 255, 0, eq.discos[i].nombre);
        texto(l, x + ancho - texto_ancho(S(22), s.s), y, S(22), CIAN, 255, 0, s.s);
        rect(l, x, y + S(12), ancho, 1, premul(40, CIAN));
        y += S(38);
    }

    x = PX1 + desliza(t, 5); y = PY(2);
    panel(l, x, y, PAN_AN, PAN_AL, "RED Y SISTEMA", CIAN);
    x += S(32); y += S(82);
    y = dato(l, x, y, ancho, "Controlador", eq.red);
    cadena_ip(&s);
    y = dato(l, x, y, ancho, "Red", s.s);
    t_ini(&s); t_add(&s, "SMEP "); t_add(&s, info.smep ? "sí" : "no"); t_add(&s, ", SMAP "); t_add(&s, info.smap ? "sí" : "no");
    y = dato(l, x, y, ancho, "Protección", s.s);
    t_ini(&s); uint64_t m = info.activo_ms / 60000; t_num(&s, m / 60); t_add(&s, " h "); t_dos(&s, m % 60); t_add(&s, " min");
    dato(l, x, y, ancho, "Encendido", s.s);

    /* Linea de escaneo que baja al entrar */
    if (t < 620) {
        int ys = (int)(t * (long)l->al / 620);
        rect(l, 0, ys, W, S(3), OPACO(CIAN));
        rect(l, 0, ys - S(40), W, S(40), premul(26, CIAN));
    }
    taek_presentar(s_pan.id, 0, 0, l->an, l->al);
    if (t > 1200) pan_quieta = 1;
}

static void entrar_especificaciones(void) {
    preparar_pantalla(E_ESPECIFICACIONES);
    pantalla_especificaciones(0);
}

/* ------------------------------------------------------------------ Bateria */

#define GX S(70)
#define GY S(190)
#define GAN S(680)
#define GAL S(300)

static uint32_t tono_carga(int pct) { return pct < 20 ? ROJO : pct < 50 ? NEON : CIAN; }

static void pintar_indicador(long t) {
    struct lienzo *l = &l_pan;
    /* Solo su propia zona: el cargador la anima todo el rato. El relleno imita el panel: casi negro sobre su tono. */
    int m = S(14);
    uint32_t fondo_panel = sobre(OPACO(tono_carga(eq.bat_pct < 0 ? 0 : eq.bat_pct)), 0xF4000000u | NEGRO);
    for (int f = GY - m; f < GY + GAL + m; f++) {
        uint32_t *p = l->px + (uint64_t)f * l->an;
        for (int k = GX - m; k < GX + GAN + S(50) + m; k++) p[k] = fondo_panel;
    }
    int pct = eq.bat_pct < 0 ? 0 : eq.bat_pct;
    uint32_t tono = tono_carga(pct);
    chaflan(l, GX, GY, GAN, GAL, S(30), OPACO(tono));
    chaflan(l, GX + S(5), GY + S(5), GAN - S(10), GAL - S(10), S(26), OPACO(NEGRO));
    rect(l, GX + GAN, GY + GAL / 3, S(34), GAL / 3, OPACO(tono));
    float f = (float)pct / 100.0f * avance(t, 150, 700);
    int n = 20, ancho = (GAN - S(44)) / n;
    for (int i = 0; i < n; i++) {
        if ((float)i / (float)n >= f) break;
        para(l, GX + S(22) + i * ancho, GY + S(24), ancho - S(6), GAL - S(48), S(14), OPACO(tono));
    }
    /* Cargando: un destello recorre las celdas */
    if ((eq.bat_estado & 2) && t > 800) {
        int celda = (int)((t / 70) % n);
        if ((float)celda / (float)n < 1.0f)
            para(l, GX + S(22) + celda * ancho, GY + S(24), ancho - S(6), GAL - S(48), S(14), premul(210, BLANCO));
    }
    struct txt s; t_ini(&s);
    if (eq.bat_pct >= 0) t_num(&s, (uint64_t)((float)pct * (avance(t, 150, 700) > 1.0f ? 1.0f : avance(t, 150, 700))));
    else t_add(&s, "0");
    int w = texto_ancho(S(240), s.s);
    int xn = GX + (GAN - w - texto_ancho(S(72), "%")) / 2, yn = GY + GAL / 2 + S(84);
    texto(l, xn + S(5), yn + S(5), S(240), NEGRO, 220, INCL, s.s);
    int fin = texto(l, xn, yn, S(240), BLANCO, 255, INCL, s.s);
    texto(l, fin + S(12), yn, S(48), NEON, 255, 0, "%");
    taek_presentar(s_pan.id, GX - m, GY - m, GAN + S(50) + 2 * m, GAL + 2 * m);
}

static void mil(struct txt *s, uint32_t v) { t_num(s, v / 1000); t_add(s, "."); t_num(s, (v % 1000) / 100); }

static void paneles_bateria(long t) {
    struct lienzo *l = &l_pan;
    limpiar(l, 0);
    struct txt s;
    const uint32_t NADA = 0xFFFFFFFFu;
    const char *u = eq.bat_unidad_mah ? " mAh" : " mWh";
    int ax = PX0 + desliza(t, 0), ay = S(26), aan = S(860), aal = l->al - S(52);
    panel(l, ax, ay, aan, aal, "BATERÍA", tono_carga(eq.bat_pct < 0 ? 0 : eq.bat_pct));
    int x = ax + S(40), y = S(112);
    const char *est = !eq.bat_presente ? "Este equipo no informa batería" : (eq.bat_estado & 4) ? "Carga crítica" : (eq.bat_estado & 2) ? "Cargando" :
                      (eq.bat_estado & 1) ? "Descargando" : eq.bat_cargador == 1 ? "Con cargador, sin cargar ahora" : "En reposo";
    titulo_rgb(l, x, y, S(36), BLANCO, S(2), est);
    y = S(530);
    t_ini(&s); t_add(&s, eq.bat_cargador == 1 ? "Conectado" : eq.bat_cargador == 0 ? "Desconectado" : "");
    y = dato(l, x, y, aan - S(90), "Cargador", s.s);
    y = dato(l, x, y, aan - S(90), "Modelo", eq.bat_modelo);
    dato(l, x, y, aan - S(90), "Química", eq.bat_tipo);

    int bx = S(930) + desliza(t, 1), bw = W - S(930) - S(30), by = S(26);
    panel(l, bx, by, bw, S(250), "ENERGÍA", CIAN);
    x = bx + S(32); y = by + S(82);
    t_ini(&s); if (eq.bat_resto != NADA) { t_num(&s, eq.bat_resto); t_add(&s, u); }
    y = dato(l, x, y, bw - S(70), "Ahora", s.s);
    t_ini(&s); if (eq.bat_lleno != NADA) { t_num(&s, eq.bat_lleno); t_add(&s, u); }
    y = dato(l, x, y, bw - S(70), "Llena", s.s);
    t_ini(&s); if (eq.bat_diseno != NADA) { t_num(&s, eq.bat_diseno); t_add(&s, u); }
    y = dato(l, x, y, bw - S(70), "De fábrica", s.s);
    t_ini(&s); if (eq.bat_ciclos != NADA && eq.bat_ciclos) t_num(&s, eq.bat_ciclos);
    dato(l, x, y, bw - S(70), "Ciclos", s.s);

    bx = S(930) + desliza(t, 3); by = S(296);
    panel(l, bx, by, bw, S(170), "SALUD", NEON);
    x = bx + S(32); y = by + S(66);
    if (eq.bat_diseno && eq.bat_diseno != NADA && eq.bat_lleno != NADA) {
        uint32_t salud = (uint32_t)((uint64_t)eq.bat_lleno * 100 / eq.bat_diseno);
        barra(l, x, y, bw - S(70), S(34), (float)salud / 100.0f * avance(t, 400, 500), salud < 50 ? ROJO : salud < 80 ? NEON : CIAN);
        t_ini(&s); t_num(&s, salud); t_add(&s, " % de la capacidad de fábrica");
        texto(l, x, y + S(72), S(22), BLANCO, 255, 0, s.s);
    } else texto(l, x, y + S(20), S(22), GRIS, 255, 0, "No informado");

    bx = S(930) + desliza(t, 5); by = S(486);
    panel(l, bx, by, bw, l->al - by - S(26), "CONSUMO", ROJO);
    x = bx + S(32); y = by + S(82);
    /* El firmware da mW, o mA y hay que multiplicar por la tension */
    uint32_t mw = NADA;
    if (eq.bat_tasa != NADA) {
        if (!eq.bat_unidad_mah) mw = eq.bat_tasa;
        else if (eq.bat_mv != NADA) mw = (uint32_t)((uint64_t)eq.bat_tasa * eq.bat_mv / 1000);
    }
    t_ini(&s); if (mw != NADA) { mil(&s, mw); t_add(&s, " W"); }
    y = dato(l, x, y, bw - S(70), (eq.bat_estado & 2) ? "Potencia de carga" : "Potencia", s.s);
    t_ini(&s); if (eq.bat_mv != NADA) { mil(&s, eq.bat_mv); t_add(&s, " V"); }
    y = dato(l, x, y, bw - S(70), "Tensión", s.s);
    t_ini(&s);
    if ((eq.bat_estado & 3) && eq.bat_tasa && eq.bat_tasa != NADA && eq.bat_resto != NADA) {
        uint32_t objetivo = (eq.bat_estado & 2) ? (eq.bat_lleno != NADA && eq.bat_lleno > eq.bat_resto ? eq.bat_lleno - eq.bat_resto : 0) : eq.bat_resto;
        uint32_t min = (uint32_t)((uint64_t)objetivo * 60 / eq.bat_tasa);
        t_num(&s, min / 60); t_add(&s, " h "); t_dos(&s, min % 60); t_add(&s, " min");
    }
    dato(l, x, y, bw - S(70), (eq.bat_estado & 2) ? "Hasta llenarse" : "Restante", s.s);
    taek_presentar(s_pan.id, 0, 0, l->an, l->al);
}

static long bat_lectura;
static void pantalla_bateria(long t) {
    if (t < 900) {
        paneles_bateria(t);
        /* El indicador espera a que su panel haya llegado */
        if (eq.bat_presente && t > 380) pintar_indicador(t);
        return;
    }
    if (!pan_quieta) {
        pan_quieta = 1;
        paneles_bateria(t);
        if (eq.bat_presente) pintar_indicador(t);
        bat_lectura = t;
    }
    /* Lo unico que sigue vivo: el destello de carga y una lectura nueva cada 10 s. */
    if (t - bat_lectura > 10000) {
        bat_lectura = t;
        taek_equipo(&eq);
        paneles_bateria(t);
        if (eq.bat_presente) pintar_indicador(t);
    } else if (eq.bat_presente && (eq.bat_estado & 2) && !(t / 70 % 2)) pintar_indicador(t);
}

static void entrar_bateria(void) {
    preparar_pantalla(E_BATERIA);
    pantalla_bateria(0);
}
