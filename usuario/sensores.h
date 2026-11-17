/* H150: lectura de SYS_SENSORES y su historia, para el tablero de INICIO y la cabina de Configuracion. Se incluye desde
 * menu.c antes del inspector. Un nucleo anterior a H150 contesta -ENOSYS: entonces todo dice "sin dato". */
#define HIST 120

struct historia { float v[HIST]; int n, i; };
static void hist_poner(struct historia *h, float v) { h->v[h->i] = v; h->i = (h->i + 1) % HIST; if (h->n < HIST) h->n++; }
/* k = 0 es la mas antigua que queda */
static float hist_ver(const struct historia *h, int k) { return h->v[(h->i - h->n + k + 2 * HIST) % HIST]; }
static float hist_max(const struct historia *h, float minimo) {
    float m = minimo;
    for (int k = 0; k < h->n; k++) if (hist_ver(h, k) > m) m = hist_ver(h, k);
    return m;
}

static struct taek_sensores sns;
static long sen_r = 1, sen_t;
static float sen_rx_s, sen_tx_s, sen_env_s;
static struct historia h_rx, h_tx, h_env, h_temp, h_tempp, h_heap, h_cuadro, h_audio;
/* La region de arranque del volumen solo se lee a pedido; se guarda la ultima para mostrarla despues. */
static struct { uint32_t leido, lba; uint16_t banderas; uint8_t bytes[16]; long t; uint8_t hora, minuto, segundo; } vol_ult;

static int sen_hay(void) { return sen_r == 0 && sns.version >= 1 && sns.version <= 9; }
/* H151 (version 2): carga de cada CPU entre dos lecturas y frecuencia efectiva de la CPU 0 */
static int sen_v2(void) { return sen_hay() && sns.version >= 2; }
static float sen_carga[4], sen_ghz;
static int sen_carga_ok;
static struct historia h_cpu[4];
/* H204 (version 3, Fase 0 del gestor de hilos): pasadas por segundo del USB y de la red. Un nucleo anterior contesta
 * con la version 2 y esto queda en "sin dato". */
static int sen_v3(void) { return sen_hay() && sns.version >= 3; }
static float sen_usb_ps, sen_red_ps;
static struct historia h_usb_ps, h_red_ps;

static void sen_leer(uint32_t op) {
    long ahora = taek_ms();
    int antes = sen_hay(), antes_v2 = sen_v2(), antes_v3 = sen_v3();
    uint64_t rx = sns.rx_paquetes, tx = sns.tx_paquetes, env = sns.stream_enviado;
    uint64_t c_tsc = sns.tsc, c_esp = sns.espera_ciclos, c_ap = sns.aperf, c_mp = sns.mperf, c_oc[4];
    uint64_t c_usb = sns.usb_pasadas, c_red = sns.red_pasadas;
    for (int i = 0; i < 4; i++) c_oc[i] = sns.ocupado_ciclos[i];
    sns.op = op | TAEK_SEN_V3;
    sen_r = taek_sensores(&sns);
    if (!sen_hay()) return;
    if (antes_v3 && sen_v3() && ahora > sen_t) {
        float dt = (float)(ahora - sen_t) / 1000.0f;
        /* Tras una puesta a cero las pasadas vuelven a empezar: esa lectura no da ritmo */
        sen_usb_ps = sns.usb_pasadas >= c_usb ? (float)(sns.usb_pasadas - c_usb) / dt : 0;
        sen_red_ps = sns.red_pasadas >= c_red ? (float)(sns.red_pasadas - c_red) / dt : 0;
        hist_poner(&h_usb_ps, sen_usb_ps);
        hist_poner(&h_red_ps, sen_red_ps);
    }
    if (antes_v2 && sen_v2() && sns.tsc > c_tsc) {
        float dt = (float)(sns.tsc - c_tsc);
        float esp = (float)(sns.espera_ciclos - c_esp) / dt;
        sen_carga[0] = limitar01(1.0f - esp);
        for (int i = 1; i < 4; i++) sen_carga[i] = limitar01((float)(sns.ocupado_ciclos[i] - c_oc[i]) / dt);
        for (int i = 0; i < 4; i++) hist_poner(&h_cpu[i], sen_carga[i] * 100.0f);
        if (sns.aperf_ok && sns.mperf > c_mp) sen_ghz = (float)info.tsc_mhz / 1000.0f * (float)(sns.aperf - c_ap) / (float)(sns.mperf - c_mp);
        sen_carga_ok = 1;
    }
    if (antes && ahora > sen_t) {
        float dt = (float)(ahora - sen_t) / 1000.0f;
        sen_rx_s = sns.rx_paquetes >= rx ? (float)(sns.rx_paquetes - rx) / dt : 0;
        sen_tx_s = sns.tx_paquetes >= tx ? (float)(sns.tx_paquetes - tx) / dt : 0;
        sen_env_s = sns.stream_enviado >= env ? (float)(sns.stream_enviado - env) / dt : 0;
        hist_poner(&h_rx, sen_rx_s);
        hist_poner(&h_tx, sen_tx_s);
        hist_poner(&h_env, sen_env_s / 1024.0f);
    }
    sen_t = ahora;
    if (sns.temp_ok) hist_poner(&h_temp, (float)sns.temp_nucleo_c);
    if (sns.temp_paquete_ok) hist_poner(&h_tempp, (float)sns.temp_paquete_c);
    hist_poner(&h_heap, (float)sns.heap_en_uso / 1048576.0f);
    if (sns.vol_leido) {
        vol_ult.leido = 1; vol_ult.lba = sns.vol_lba; vol_ult.banderas = sns.vol_banderas;
        memcpy(vol_ult.bytes, sns.vol_bytes, 16);
        vol_ult.t = ahora; vol_ult.hora = info.hora; vol_ult.minuto = info.minuto; vol_ult.segundo = info.segundo;
    } else if (vol_ult.leido) {
        /* Los bytes que se ven en el modo hexadecimal son los de la ultima lectura; la cabina dice de cuando son. */
        sns.vol_leido = 1; sns.vol_lba = vol_ult.lba; sns.vol_banderas = vol_ult.banderas;
        memcpy(sns.vol_bytes, vol_ult.bytes, 16);
    }
}

/* Color de una temperatura: verde, ambar desde 65, rojo desde 90 (la alarma). */
static uint32_t tono_temp(int c) { return c >= 90 ? 0xFF003Cu : c >= 65 ? 0xFFAA00u : 0x2ECC71u; }

/* H151: ayudantes de texto y colores que comparten la cabina y los paneles */
#define NARANJA 0xFF8C00u
#define AMBAR 0xFFAA00u
#define VERDE 0x2ECC71u
static void t_hexn(struct txt *t, uint64_t v, int n) {
    static const char h[] = "0123456789ABCDEF";
    char b[17];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) b[i] = h[(v >> ((n - 1 - i) * 4)) & 15];
    b[n] = 0;
    t_add(t, b);
}
static void t_ip4(struct txt *t, const uint8_t *ip) { for (int i = 0; i < 4; i++) { if (i) t_add(t, "."); t_num(t, ip[i]); } }
static void t_dec1(struct txt *t, float v) {
    if (v < 0) { t_add(t, "-"); v = -v; }
    uint64_t e = (uint64_t)(v * 10.0f + 0.5f);
    t_num(t, e / 10); t_add(t, ","); t_num(t, e % 10);
}
static void t_bytes(struct txt *t, uint64_t b) {
    if (b >= (10ull << 30)) { t_num(t, b >> 30); t_add(t, " GiB"); }
    else if (b >= (10ull << 20)) { t_num(t, b >> 20); t_add(t, " MiB"); }
    else if (b >= (10ull << 10)) { t_num(t, b >> 10); t_add(t, " KiB"); }
    else { t_num(t, b); t_add(t, " B"); }
}

