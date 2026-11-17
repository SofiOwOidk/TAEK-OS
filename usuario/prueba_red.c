/* H183: prueba de SYS_RED (sockets TCP y TLS 1.3 para programas de Anillo 3). Se lanza con 'ejecutar /prueba_red.elf'
 * o 'ejecutar /prueba_red.elf <busqueda>'. Todo el HTTP vive aqui, en el programa; el nucleo solo da la conexion.
 *   1. POST a la API interna de YouTube (InnerTube, cliente ANDROID, anonimo): /youtubei/v1/search.
 *   2. /youtubei/v1/player del primer video: URL directa del itag 18 (MP4 360p con H.264 + AAC).
 *   3. GET con Range de los primeros 4 MiB del MP4 desde googlevideo, midiendo la velocidad.
 * Imprime [RED] por paso y RESULTADO=PASS o FALLO. Mientras corre, el stream de consola sigue vivo. */
#include "memoria.h"

TAEK_PROGRAMA

static int fallos;

/* ------------------------------------------------------------------ texto */
static int largo(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void linea(const char *s) {
    char b[300]; int n = 0;
    const char *pre = "[RED] ";
    while (*pre) b[n++] = *pre++;
    while (*s && n < 296) b[n++] = *s++;
    b[n++] = '\n';
    taek_escribir(b, n);
}
static char *poner(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *poner_num(char *d, int64_t v) {
    if (v < 0) { *d++ = '-'; v = -v; }
    char t[24]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) *d++ = t[--k];
    *d = 0;
    return d;
}
static void comprobar(int ok, const char *que) {
    if (!ok) fallos++;
    char b[260]; char *p = poner(b, ok ? "OK    " : "FALLO ");
    poner(p, que);
    linea(b);
}
static const char *buscar(const char *h, const char *fin, const char *aguja) {
    int n = largo(aguja);
    for (const char *p = h; p + n <= fin; p++) {
        int i = 0;
        while (i < n && p[i] == aguja[i]) i++;
        if (i == n) return p;
    }
    return 0;
}

/* ------------------------------------------------------------------ sockets */
static long abrir_puerto(const char *host, int tls, uint16_t puerto);
static long abrir(const char *host, int tls) { return abrir_puerto(host, tls, tls ? 443 : 80); }
static long abrir_puerto(const char *host, int tls, uint16_t puerto) {
    struct taek_red r; memset(&r, 0, sizeof(r));
    r.op = TAEK_RED_ABRIR;
    for (int i = 0; host[i] && i < TAEK_RED_HOST - 1; i++) r.host[i] = host[i];
    r.puerto = puerto;
    r.banderas = tls ? TAEK_RED_TLS : 0;
    r.espera_ms = 15000;
    long x = taek_red(&r);
    if (x < 0) {
        char b[200]; char *p = poner(b, "abrir "); p = poner(p, host); p = poner(p, " fallo: ");
        p = poner_num(p, x); p = poner(p, " detalle "); poner_num(p, r.error);
        linea(b);
    }
    return x;
}
static int enviar(long id, const char *d, uint32_t n) {
    for (uint32_t off = 0; off < n;) {
        struct taek_red r; memset(&r, 0, sizeof(r));
        r.op = TAEK_RED_ENVIAR; r.id = (int32_t)id; r.datos = (uint64_t)(d + off);
        r.largo = n - off > 65536 ? 65536 : n - off; r.espera_ms = 15000;
        long x = taek_red(&r);
        if (x <= 0) return -1;
        off += (uint32_t)x;
    }
    return 0;
}
/* Lee hasta que el servidor cierra (Connection: close) o se llena el bufer. Devuelve bytes. */
static long recibir_todo(long id, char *buf, uint32_t cap, long *ms) {
    long t0 = taek_ms(), total = 0;
    for (;;) {
        if ((uint32_t)total >= cap) break;
        struct taek_red r; memset(&r, 0, sizeof(r));
        r.op = TAEK_RED_RECIBIR; r.id = (int32_t)id; r.datos = (uint64_t)(buf + total);
        r.largo = cap - (uint32_t)total > 65536 ? 65536 : cap - (uint32_t)total; r.espera_ms = 10000;
        long x = taek_red(&r);
        if (x < 0) break;                       /* cerrada: fin del cuerpo */
        if (x == 0) { linea("sin datos en 10 s"); break; }
        total += x;
    }
    if (ms) *ms = taek_ms() - t0;
    return total;
}
static void cerrar(long id) {
    struct taek_red r; memset(&r, 0, sizeof(r));
    r.op = TAEK_RED_CERRAR; r.id = (int32_t)id;
    taek_red(&r);
}
static int estado_http(const char *b, long n) {
    if (n < 12 || b[0] != 'H' || b[1] != 'T') return 0;
    return (b[9] - '0') * 100 + (b[10] - '0') * 10 + (b[11] - '0');
}

/* ------------------------------------------------------------------ InnerTube */
#define VERSION "21.26.364"
static char peticion[8192];
static long innertube(const char *ruta, const char *cuerpo, char *salida, uint32_t cap, long *ms) {
    long id = abrir("youtubei.googleapis.com", 1);
    if (id < 0) return -1;
    char *p = poner(peticion, "POST /youtubei/v1/");
    p = poner(p, ruta);
    p = poner(p, "?prettyPrint=false HTTP/1.1\r\nHost: youtubei.googleapis.com\r\nContent-Type: application/json\r\n"
                 "User-Agent: com.google.android.youtube/" VERSION " (Linux; U; Android 11) gzip\r\n"
                 "X-Youtube-Client-Name: 3\r\nX-Youtube-Client-Version: " VERSION "\r\n"
                 "Accept-Encoding: identity\r\nConnection: close\r\nContent-Length: ");
    p = poner_num(p, largo(cuerpo));
    p = poner(p, "\r\n\r\n");
    p = poner(p, cuerpo);
    long n = -1;
    if (!enviar(id, peticion, (uint32_t)(p - peticion))) n = recibir_todo(id, salida, cap, ms);
    cerrar(id);
    return n;
}

long principal(uint64_t a, uint64_t b) {
    (void)a; (void)b;
    char consulta[64] = "gatos";
    long na = taek_argumento(consulta, sizeof(consulta) - 1);
    if (na <= 0) { consulta[0] = 'g'; consulta[1] = 'a'; consulta[2] = 't'; consulta[3] = 'o'; consulta[4] = 's'; consulta[5] = 0; }
    else { consulta[na] = 0; for (long i = 0; i < na; i++) if (consulta[i] == '"' || consulta[i] == '\\') consulta[i] = ' '; }
    /* 'vel <ip> [MiB]': TCP sin TLS contra herramientas/intel/fuente_velocidad.py (puerto 9156), para separar la red del TLS. */
    if (na > 4 && consulta[0] == 'v' && consulta[1] == 'e' && consulta[2] == 'l' && consulta[3] == ' ') {
        char ip[40]; int k = 0; const char *s = consulta + 4;
        while (*s && *s != ' ' && k < 39) ip[k++] = *s++;
        ip[k] = 0;
        uint64_t mib = 32; if (*s == ' ') { mib = 0; for (s++; *s >= '0' && *s <= '9'; s++) mib = mib * 10 + (uint64_t)(*s - '0'); }
        if (!mib || mib > 1024) mib = 32;
        uint8_t *v = malloc(1u << 20);
        long id = abrir_puerto(ip, 0, 9156);
        comprobar(v && id >= 0, "TCP con la fuente del PC");
        if (!v || id < 0) goto fin;
        uint8_t ped[16] = { 'T','A','E','K','V','E','L','1' };
        uint64_t total = mib << 20;
        for (int i = 0; i < 8; i++) ped[8 + i] = (uint8_t)(total >> (56 - 8 * i));
        enviar(id, (const char *)ped, 16);
        long t0 = taek_ms(); uint64_t recibido = 0;
        while (recibido < total) {
            struct taek_red r; memset(&r, 0, sizeof(r));
            r.op = TAEK_RED_RECIBIR; r.id = (int32_t)id; r.datos = (uint64_t)v; r.largo = 65536; r.espera_ms = 5000;
            long x = taek_red(&r);
            if (x <= 0) { linea(x == 0 ? "sin datos en 5 s" : "cerrada"); break; }
            recibido += (uint64_t)x;
        }
        long ms = taek_ms() - t0;
        cerrar(id);
        char l[160]; char *q = poner(l, "TCP por SYS_RED: "); q = poner_num(q, (int64_t)recibido); q = poner(q, " B en ");
        q = poner_num(q, ms); q = poner(q, " ms = "); q = poner_num(q, ms ? (int64_t)recibido * 8 / ms / 1000 : 0); poner(q, " Mbit/s");
        linea(l);
        comprobar(recibido == total, "llegaron todos los bytes");
        goto fin;
    }
    /* 'get <host> <ruta> [tls]': descarga cualquiera, para comparar TCP por internet con y sin TLS. */
    if (na > 4 && consulta[0] == 'g' && consulta[1] == 'e' && consulta[2] == 't' && consulta[3] == ' ') {
        static char gh[128], gr[256]; int k = 0; const char *s = consulta + 4;
        while (*s && *s != ' ' && k < 127) gh[k++] = *s++;
        gh[k] = 0; k = 0; if (*s == ' ') s++;
        while (*s && *s != ' ' && k < 255) gr[k++] = *s++;
        gr[k] = 0;
        int tls = *s == ' ' && s[1] == 't';
        uint32_t gcap = 64u << 20;
        char *g = malloc(gcap);
        long id = abrir(gh, tls);
        comprobar(g && id >= 0, tls ? "conexion TLS" : "conexion TCP");
        if (!g || id < 0) goto fin;
        char *q = poner(peticion, "GET "); q = poner(q, gr); q = poner(q, " HTTP/1.1\r\nHost: "); q = poner(q, gh);
        q = poner(q, "\r\nUser-Agent: TAEK-OS/1.0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
        long ms2 = 0, n2 = enviar(id, peticion, (uint32_t)(q - peticion)) ? -1 : recibir_todo(id, g, gcap, &ms2);
        cerrar(id);
        char l[200]; q = poner(l, tls ? "HTTPS " : "HTTP "); q = poner(q, gh); q = poner(q, ": estado "); q = poner_num(q, estado_http(g, n2));
        q = poner(q, ", "); q = poner_num(q, n2); q = poner(q, " B en "); q = poner_num(q, ms2); q = poner(q, " ms = ");
        q = poner_num(q, ms2 ? n2 * 8 / ms2 / 1000 : 0); poner(q, " Mbit/s");
        linea(l);
        goto fin;
    }
    linea("Prueba de SYS_RED: YouTube por HTTPS desde Anillo 3");

    uint32_t cap = 8u << 20;
    char *buf = malloc(cap);
    comprobar(buf != 0, "malloc de 8 MiB");
    if (!buf) goto fin;

    /* 1. Busqueda */
    char cuerpo[512];
    char *p = poner(cuerpo, "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"" VERSION
                            "\",\"androidSdkVersion\":30,\"hl\":\"es\",\"gl\":\"PE\"}},\"query\":\"");
    p = poner(p, consulta); poner(p, "\"}");
    long ms = 0, n = innertube("search", cuerpo, buf, cap, &ms);
    {
        char l[200]; char *q = poner(l, "busqueda '"); q = poner(q, consulta); q = poner(q, "': HTTP ");
        q = poner_num(q, estado_http(buf, n)); q = poner(q, ", "); q = poner_num(q, n); q = poner(q, " B en ");
        q = poner_num(q, ms); poner(q, " ms");
        linea(l);
    }
    comprobar(estado_http(buf, n) == 200, "search responde 200");
    char ids[8][12]; int nids = 0;
    for (const char *s = buf; nids < 8 && (s = buscar(s, buf + n, "\"videoId\":\"")); ) {
        s += 11;
        if (s + 11 > buf + n || s[11] != '"') continue;
        int repetido = 0;
        for (int k = 0; k < nids; k++) { int i = 0; while (i < 11 && ids[k][i] == s[i]) i++; if (i == 11) repetido = 1; }
        if (repetido) continue;
        for (int i = 0; i < 11; i++) ids[nids][i] = s[i];
        ids[nids][11] = 0;
        nids++;
    }
    comprobar(nids > 0, "la busqueda trae videoId");
    for (int k = 0; k < nids && k < 5; k++) { char l[40]; char *q = poner(l, "  video "); poner(q, ids[k]); linea(l); }
    if (!nids) goto fin;

    /* 2. Player del primero: itag 18 con URL directa */
    p = poner(cuerpo, "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"" VERSION
                      "\",\"androidSdkVersion\":30,\"hl\":\"es\",\"gl\":\"PE\"}},\"videoId\":\"");
    p = poner(p, ids[0]); poner(p, "\",\"contentCheckOk\":true,\"racyCheckOk\":true}");
    n = innertube("player", cuerpo, buf, cap, &ms);
    {
        char l[120]; char *q = poner(l, "player "); q = poner(q, ids[0]); q = poner(q, ": HTTP ");
        q = poner_num(q, estado_http(buf, n)); q = poner(q, ", "); q = poner_num(q, n); poner(q, " B");
        linea(l);
    }
    comprobar(buscar(buf, buf + n, "\"status\":\"OK\"") != 0, "playabilityStatus OK");
    const char *it = buscar(buf, buf + n, "\"itag\":18,");
    const char *u = it ? buscar(it, buf + n, "\"url\":\"") : 0;
    comprobar(u != 0, "itag 18 con URL directa");
    if (!u) goto fin;
    static char url[4096];
    int k = 0;
    for (u += 7; u < buf + n && *u != '"' && k < (int)sizeof(url) - 1; u++) {
        if (u[0] == '\\' && u[1] == 'u' && u[2] == '0' && u[3] == '0' && u[4] == '2' && u[5] == '6') { url[k++] = '&'; u += 5; }
        else if (u[0] == '\\' && u[1] == '/') { url[k++] = '/'; u += 1; }
        else url[k++] = *u;
    }
    url[k] = 0;
    /* https://HOST/RUTA */
    char host[128]; int h = 0;
    const char *r = url + 8;
    while (*r && *r != '/' && h < 127) host[h++] = *r++;
    host[h] = 0;
    { char l[200]; char *q = poner(l, "video en "); poner(q, host); linea(l); }

    /* 3. Rango de 4 MiB */
    long id = abrir(host, 1);
    comprobar(id >= 0, "TLS con googlevideo");
    if (id < 0) goto fin;
    p = poner(peticion, "GET ");
    p = poner(p, r);
    p = poner(p, " HTTP/1.1\r\nHost: ");
    p = poner(p, host);
    p = poner(p, "\r\nUser-Agent: com.google.android.youtube/" VERSION " (Linux; U; Android 11) gzip\r\n"
                 "Range: bytes=0-4194303\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
    n = enviar(id, peticion, (uint32_t)(p - peticion)) ? -1 : recibir_todo(id, buf, cap, &ms);
    cerrar(id);
    {
        char l[200]; char *q = poner(l, "rango 0-4 MiB: HTTP "); q = poner_num(q, estado_http(buf, n));
        q = poner(q, ", "); q = poner_num(q, n); q = poner(q, " B en "); q = poner_num(q, ms); q = poner(q, " ms = ");
        q = poner_num(q, ms ? n * 8 / ms / 1000 : 0); poner(q, " Mbit/s");
        linea(l);
    }
    comprobar(estado_http(buf, n) == 206, "googlevideo responde 206 Partial Content");
    const char *cab = buscar(buf, buf + n, "\r\n\r\n");
    comprobar(cab && cab + 12 < buf + n && buscar(cab, cab + 16, "ftyp"), "el cuerpo empieza con la caja 'ftyp' de MP4");

fin:
    linea(fallos ? "RESULTADO=FALLO" : "RESULTADO=PASS");
    return fallos;
}
