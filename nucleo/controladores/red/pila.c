#include "red.h"
#include "stream.h"
#include "http_respuesta.h"
#include "tls_port/taek_tls_port.h"
#include "../consola.h"
#include "../../arquitectura/x86_64/serial.h"
#include "../../base/memoria.h"
#include "../../base/tiempo.h"
#include "../../base/atencion.h"
#include "red_cerrojo.h"
#include "../../base/hilos.h"   /* H211: cerrojo de la red (Fase C) */

// =====================================================================
//  Pila IPv4 mínima de TAEK OS: Ethernet + ARP + IPv4 + ICMP + UDP +
//  DHCP + DNS + TCP + HTTP. Todo el tráfico se procesa por sondeo.
// =====================================================================

#define RED_ETH_ARP   0x0806
#define RED_ETH_IPV4  0x0800

#define RED_IP_ICMP   1
#define RED_IP_UDP    17
#define RED_IP_TCP    6

#define RED_TCP_MAX_SEGMENTO (RED_MTU - 40)
#define RED_DHCP_BUFFER      1024
#define RED_DNS_BUFFER       1024

#define RED_TCP_REENVIO_MS   1000
#define RED_TCP_REINTENTOS   8

struct __attribute__((packed)) red_eth {
    uint8_t  destino[6];
    uint8_t  origen[6];
    uint16_t tipo;
};

struct __attribute__((packed)) red_arp {
    uint16_t tipo_hw;
    uint16_t tipo_proto;
    uint8_t  largo_hw;
    uint8_t  largo_proto;
    uint16_t operacion;
    uint8_t  emisor_mac[6];
    uint8_t  emisor_ip[4];
    uint8_t  objetivo_mac[6];
    uint8_t  objetivo_ip[4];
};

struct __attribute__((packed)) red_ipv4 {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t longitud_total;
    uint16_t id;
    uint16_t banderas_fragmento;
    uint8_t  ttl;
    uint8_t  protocolo;
    uint16_t checksum;
    uint8_t  origen[4];
    uint8_t  destino[4];
};

struct __attribute__((packed)) red_icmp {
    uint8_t  tipo;
    uint8_t  codigo;
    uint16_t checksum;
    uint16_t id;
    uint16_t secuencia;
};

struct __attribute__((packed)) red_udp {
    uint16_t origen_port;
    uint16_t destino_port;
    uint16_t longitud;
    uint16_t checksum;
};

struct __attribute__((packed)) red_tcp {
    uint16_t origen_port;
    uint16_t destino_port;
    uint32_t secuencia;
    uint32_t acuse;
    uint8_t  offset_reservado;
    uint8_t  banderas;
    uint16_t ventana;
    uint16_t checksum;
    uint16_t urgente;
};

struct __attribute__((packed)) red_dhcp {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t segundos;
    uint16_t banderas;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magico;
    uint8_t  opciones[312];
};

#define RED_TCP_FIN 0x01
#define RED_TCP_SYN 0x02
#define RED_TCP_RST 0x04
#define RED_TCP_PSH 0x08
#define RED_TCP_ACK 0x10

// ---------------------------------------------------------------- Byte order
static inline uint16_t red_be16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint32_t red_be32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}

static int red_ip_igual(const uint8_t a[4], const uint8_t b[4]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int red_ip_cero(const uint8_t a[4]) {
    return a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0;
}

// ------------------------------------------------------------- Checksums
static uint32_t red_suma(const void *datos, uint32_t longitud, uint32_t suma) {
    const uint8_t *p = (const uint8_t *)datos;
    while (longitud > 1) {
        suma += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        longitud -= 2;
    }
    if (longitud) suma += ((uint32_t)p[0] << 8);
    return suma;
}

static uint16_t red_finalizar_suma(uint32_t suma) {
    while (suma >> 16) suma = (suma & 0xFFFF) + (suma >> 16);
    return (uint16_t)(~suma & 0xFFFF);
}

static uint16_t red_checksum(const void *datos, uint32_t longitud) {
    return red_finalizar_suma(red_suma(datos, longitud, 0));
}

// ------------------------------------------------------------------- Estado
static struct red_config g_cfg;

struct arp_entrada {
    uint8_t ip[4];
    uint8_t mac[6];
    int     valida;
    uint64_t ultimo_uso;
};
static struct arp_entrada g_arp[8];

static void red_arp_aprender(const struct red_arp *arp) {
    if (red_ip_cero(arp->emisor_ip)) return; // una sonda DHCP no es un vecino 0.0.0.0
    int elegido = -1, libre = -1, antiguo = 0;
    for (int i = 0; i < 8; i++) {
        if (g_arp[i].valida && red_ip_igual(g_arp[i].ip, arp->emisor_ip)) {
            elegido = i;
            break;
        }
        if (!g_arp[i].valida && libre < 0) libre = i;
        if (g_arp[i].ultimo_uso < g_arp[antiguo].ultimo_uso) antiguo = i;
    }
    // RFC 826: actualizar vecinos conocidos; incorporar nuevos solo si somos
    // el objetivo. Broadcasts entre terceros no deben llenar nuestra cache.
    if (elegido < 0) {
        if (!g_cfg.configurada || !red_ip_igual(arp->objetivo_ip, g_cfg.ip)) return;
        elegido = libre >= 0 ? libre : antiguo;
    }
    memcpy(g_arp[elegido].ip, arp->emisor_ip, 4);
    memcpy(g_arp[elegido].mac, arp->emisor_mac, 6);
    g_arp[elegido].valida = 1;
    g_arp[elegido].ultimo_uso = tiempo_obtener_milisegundos();
}

/* H151: la cache ARP (8 vecinos, se reemplaza el menos usado) a la vista, y vaciarla a pedido. */
int red_arp_listar(uint8_t ip[][4], uint8_t mac[][6], uint64_t edad_ms[], int max) { RED_EXCLUSIVA();
    int n = 0;
    uint64_t ahora = tiempo_obtener_milisegundos();
    for (int i = 0; i < 8 && n < max; i++) {
        if (!g_arp[i].valida) continue;
        memcpy(ip[n], g_arp[i].ip, 4); memcpy(mac[n], g_arp[i].mac, 6);
        edad_ms[n] = ahora - g_arp[i].ultimo_uso;
        n++;
    }
    return n;
}
int red_arp_vaciar(void) { RED_EXCLUSIVA();
    int n = 0;
    for (int i = 0; i < 8; i++) n += g_arp[i].valida != 0;
    memset(g_arp, 0, sizeof(g_arp));
    return n;
}

static uint16_t g_ip_id = 1;

// Ping
static uint16_t g_ping_id = 0;
static uint16_t g_ping_secuencia = 0;
static int      g_ping_respuestas = 0;
static int      g_ping_ultimo_rtt = -1;
static uint64_t g_ping_envio_ms = 0;
static int      g_ping_activo = 0;

// DHCP
static uint32_t g_dhcp_xid = 0;
static int      g_dhcp_esperado = 0;      // 2 = OFFER, 5 = ACK
static int      g_dhcp_recibido = 0;
static uint8_t  g_dhcp_ip[4];
static uint8_t  g_dhcp_mascara[4];
static uint8_t  g_dhcp_puerta[4];
static uint8_t  g_dhcp_dns[4];
static uint8_t  g_dhcp_servidor[4];

// DNS
static uint16_t g_dns_id = 0;
static uint16_t g_dns_puerto_local = 0;
static volatile int g_dns_recibido = 0;
static uint8_t  g_dns_ip[4];

// TCP
struct red_tcp_conexion {
    int      activa;
    uint8_t  remota_ip[4];
    uint16_t puerto_remoto;
    uint16_t puerto_local;
    uint32_t seq;
    uint32_t ack;
    uint32_t isn;
    int      estado;            // 0 cerrado, 1 syn_enviado, 2 establecido, 3 fin_enviado, 4 remoto cerrado
    int      ack_de_datos;
    int      fin_recibido;
    uint32_t seq_ultimo;
    uint32_t len_ultimo;
    int      ultimo_era_fin;
    uint8_t  ultimo_flags;
    int      reintentos;
    uint64_t ultimo_envio_ms;
    uint8_t  ultimo_payload[RED_TCP_MAX_SEGMENTO];
    char    *rx_destino;
    uint32_t rx_capacidad;
    uint32_t rx_longitud;
    int      rx_completo;
    int      rx_desbordado;
    uint32_t rx_ini;            // H177: inicio del anillo del flujo
    uint8_t  ws_ok;             // H177: escalado de ventana negociado (RFC 7323), factor 2^RED_TCP_WS
    uint8_t  sin_ack;           // H177: segmentos en orden aun sin ACK (H178: cada g_tcp_ack_cada)
    int      flujo;             // H183: modo flujo (anillo, ACK retardado); antes era global
    int      en_uso;            // H183: manejador reservado (1-7)
    char    *anillo;            // H183: anillo propio del manejador (se conserva entre usos)
    // H184: trozos recibidos fuera de orden, ya escritos en su lugar del anillo [ini, fin) en numeros de
    // secuencia, y SACK (RFC 2018) si el otro lado lo permitio en su SYN.
    uint32_t trozo_ini[8], trozo_fin[8];
    uint8_t  n_trozos;
    uint8_t  sack_ok;
};
// H183: tabla de conexiones. La 0 es la de siempre (stream de consola, archivos, HTTP/HTTPS del nucleo), con
// su cerrojo g_tcp_ocupada; la 1-7 se piden por manejador (red_tcp_abrir) y no pausan el stream. 'g_tcp' es
// la conexion en curso: los segmentos entrantes se reparten por IP y puertos y el codigo de siempre sigue
// escribiendo g_tcp. Regla: no imprimir en consola con g_tcp_actual fuera de la 0 (la consola usa el stream).
#define RED_TCP_CONEXIONES 8
static struct red_tcp_conexion g_tcp_tabla[RED_TCP_CONEXIONES];
static struct red_tcp_conexion *g_tcp_actual = &g_tcp_tabla[0];
#define g_tcp (*g_tcp_actual)
#define g_tcp_flujo (g_tcp.flujo)
static unsigned char g_tcp_ocupada;
static uint32_t g_tcp_recortes, g_tcp_fuera_orden;   // H184: diagnostico
void red_tcp_contadores(uint32_t *recortes, uint32_t *fuera_orden) { *recortes = g_tcp_recortes; *fuera_orden = g_tcp_fuera_orden; }
// H177: anillo de 256 KiB (antes 32 KiB lineal con desplazamiento byte a byte). Con escalado x8 la
// ventana anunciada llega a 256 KiB: por WiFi a ~3 ms de RTT da techo de ~700 Mbit/s.
#define RED_TCP_WS 4                 // x16: anuncios hasta 1 MiB; el tope real es g_tcp_ventana (H182)
// H178: ACK cada N segmentos en orden (ACK "estirado") y siempre al vaciar una rafaga. Por WiFi cada
// trama de subida cuesta ~200 us de aire fijos (DIFS, backoff, preambulo, ACK de capa 2) sin importar
// la tasa: con N = 2 los ACK de TCP se comian ~85 % del aire a 100 Mbit/s. 'red ack N' lo cambia.
static uint32_t g_tcp_ack_cada = 48;   // H180: medido en H179 con CCMP por hardware: 32 -> 364, 48 -> 387, 64 -> 379 Mbit/s
int red_tcp_ack_cada(int n) { if (n >= 1 && n <= 64) g_tcp_ack_cada = (uint32_t)n; return (int)g_tcp_ack_cada; }
static char g_tcp_flujo_rx[1048576];   // H182: 1 MiB de anillo; se anuncia como mucho g_tcp_ventana
// H182: ventana anunciada maxima, ajustable con 'red ventana N' (KiB). H181 midio 256 KiB como la mejor.
static uint32_t g_tcp_ventana = 262144;
int red_tcp_ventana_kib(int kib) { if (kib >= 16 && kib <= 1024) g_tcp_ventana = (uint32_t)kib << 10; return (int)(g_tcp_ventana >> 10); }

// Búferes de trabajo (reutilizados; operación sincrónica)
static uint8_t g_trama[RED_MAX_TRAMA];
static uint8_t g_payload[RED_MTU];

// ------------------------------------------------------------- Utilidades IP
static int red_misma_subred(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & g_cfg.mascara[i]) != (g_cfg.ip[i] & g_cfg.mascara[i])) return 0;
    }
    return 1;
}

static void red_calcular_broadcast(uint8_t salida[4]) {
    for (int i = 0; i < 4; i++) salida[i] = g_cfg.ip[i] | (uint8_t)~g_cfg.mascara[i];
}

static void red_log(const char *s) {
    consola_imprimir(s);
}

static void red_log_linea(const char *s) {
    consola_imprimir_linea(s);
}

static void red_log_dec(uint64_t v) {
    consola_imprimir_dec(v);
}

// ------------------------------------------------------------------ sondeo
static void red_procesar_dhcp(const uint8_t *datos, uint32_t longitud);
static void red_procesar_dns(const uint8_t *datos, uint32_t longitud);
static void red_procesar_tcp(struct red_ipv4 *ip, uint8_t *tcp_carga, uint32_t carga_len);
static void red_tcp_temporizador(void);
static void red_icmp_responder(const uint8_t *rx, uint32_t carga_len);

void red_pila_sondeo(void) { if (red_cede_el_sondeo()) return; RED_EXCLUSIVA();
    const struct red_info *info = red_obtener_info();
    if (!info->controlador_listo) return;
    atencion_marcar(&atencion_red);   /* H204, Fase 0: hueco entre dos pasadas por la pila */

    // Búfer local: evita que una transmisión (que puede reentrar vía ARP)
    // pise la trama que se está procesando.
    uint8_t trama[RED_MAX_TRAMA];

    for (int guarda = 0; guarda < 64; guarda++) {
        uint32_t len = 0;
        if (!red_recibir_trama(trama, sizeof(trama), &len)) break;
        if (len < sizeof(struct red_eth)) continue;

        struct red_eth *eth = (struct red_eth *)trama;
        uint16_t tipo = red_be16(eth->tipo);

        if (tipo == RED_ETH_ARP) {
            if (len < sizeof(struct red_eth) + sizeof(struct red_arp)) continue;
            struct red_arp *arp = (struct red_arp *)(trama + sizeof(struct red_eth));
            uint16_t operacion=red_be16(arp->operacion);
            if(red_be16(arp->tipo_hw)!=1||red_be16(arp->tipo_proto)!=RED_ETH_IPV4||
               arp->largo_hw!=6||arp->largo_proto!=4||(operacion!=1&&operacion!=2))continue;
            red_arp_aprender(arp);
            if (red_be16(arp->operacion) == 1 && g_cfg.configurada &&
                red_ip_igual(arp->objetivo_ip, g_cfg.ip)) {
                struct red_eth *resp_eth = (struct red_eth *)g_trama;
                struct red_arp *resp = (struct red_arp *)(g_trama + sizeof(struct red_eth));
                memcpy(resp_eth->destino, arp->emisor_mac, 6);
                memcpy(resp_eth->origen, info->mac, 6);
                resp_eth->tipo = red_be16(RED_ETH_ARP);
                resp->tipo_hw = red_be16(1);
                resp->tipo_proto = red_be16(RED_ETH_IPV4);
                resp->largo_hw = 6;
                resp->largo_proto = 4;
                resp->operacion = red_be16(2);
                memcpy(resp->emisor_mac, info->mac, 6);
                memcpy(resp->emisor_ip, g_cfg.ip, 4);
                memcpy(resp->objetivo_mac, arp->emisor_mac, 6);
                memcpy(resp->objetivo_ip, arp->emisor_ip, 4);
                red_enviar_trama(g_trama, sizeof(struct red_eth) + sizeof(struct red_arp));
            }
        } else if (tipo == RED_ETH_IPV4) {
            if (len < sizeof(struct red_eth) + sizeof(struct red_ipv4)) continue;
            struct red_ipv4 *ip = (struct red_ipv4 *)(trama + sizeof(struct red_eth));
            if ((ip->ver_ihl >> 4) != 4) continue;
            uint32_t ihl = (uint32_t)(ip->ver_ihl & 0x0F) * 4;
            if (ihl < 20) continue;
            if (len < sizeof(struct red_eth) + ihl) continue;
            if (red_checksum(ip, ihl) != 0) continue;
            uint16_t total = red_be16(ip->longitud_total);
            if (total < ihl || total > len - sizeof(struct red_eth)) continue;

            uint8_t *carga = trama + sizeof(struct red_eth) + ihl;
            uint32_t carga_len = total - ihl;

            uint8_t broadcast[4];
            red_calcular_broadcast(broadcast);
            int para_nosotros = (g_cfg.configurada && red_ip_igual(ip->destino, g_cfg.ip)) ||
                                red_ip_igual(ip->destino, broadcast) ||
                                red_ip_igual(ip->destino, (const uint8_t[4]){255,255,255,255});
            // H169: durante el DHCP el servidor puede responder por unicast a la IP que va a
            // conceder (p. ej. la concesion previa de esta MAC), aunque pidamos broadcast.
            if (!para_nosotros && g_dhcp_esperado && ip->protocolo == RED_IP_UDP &&
                carga_len >= sizeof(struct red_udp) &&
                red_be16(((struct red_udp *)carga)->destino_port) == 68) para_nosotros = 1;
            if (!para_nosotros) continue;

            if (ip->protocolo == RED_IP_ICMP && carga_len >= sizeof(struct red_icmp)) {
                struct red_icmp *icmp = (struct red_icmp *)carga;
                if (icmp->tipo == 8 && carga_len >= sizeof(struct red_icmp) + 4) {
                    red_icmp_responder(trama, carga_len);
                } else if (icmp->tipo == 0) {
                    uint16_t id = red_be16(icmp->id);
                    uint16_t sec = red_be16(icmp->secuencia);
                    if (g_ping_activo && id == g_ping_id && sec == g_ping_secuencia) {
                        g_ping_respuestas++;
                        g_ping_ultimo_rtt = (int)(tiempo_obtener_milisegundos() - g_ping_envio_ms);
                        g_ping_activo = 0;
                    }
                }
            } else if (ip->protocolo == RED_IP_UDP && carga_len >= sizeof(struct red_udp)) {
                struct red_udp *udp = (struct red_udp *)carga;
                uint16_t dp = red_be16(udp->destino_port);
                uint16_t sp = red_be16(udp->origen_port);
                uint32_t udp_len = red_be16(udp->longitud);
                if (udp_len < sizeof(struct red_udp) || udp_len > carga_len) udp_len = carga_len;
                uint8_t *datos = carga + sizeof(struct red_udp);
                uint32_t datos_len = udp_len - sizeof(struct red_udp);
                if (dp == 68) red_procesar_dhcp(datos, datos_len);
                else if (sp == 53) red_procesar_dns(datos, datos_len);
            } else if (ip->protocolo == RED_IP_TCP && carga_len >= sizeof(struct red_tcp)) {
                red_procesar_tcp(ip, carga, carga_len);
            }
        }
    }

    red_tcp_temporizador();
}

// ------------------------------------------- Envío IPv4 (con resolución ARP)
static int red_enviar_ipv4_mac(const uint8_t destino_mac[6], const uint8_t destino_ip[4],
                               uint8_t protocolo, const void *payload, uint32_t payload_len) {
    const struct red_info *info = red_obtener_info();
    if (payload_len > RED_MAX_TRAMA - sizeof(struct red_eth) - 20) return -1;

    struct red_eth *eth = (struct red_eth *)g_trama;
    memcpy(eth->destino, destino_mac, 6);
    memcpy(eth->origen, info->mac, 6);
    eth->tipo = red_be16(RED_ETH_IPV4);

    struct red_ipv4 *ip = (struct red_ipv4 *)(g_trama + sizeof(struct red_eth));
    memset(ip, 0, sizeof(*ip));
    ip->ver_ihl = 0x45;
    ip->tos = 0;
    ip->longitud_total = red_be16((uint16_t)(20 + payload_len));
    ip->id = red_be16(g_ip_id++);
    ip->banderas_fragmento = red_be16(0x4000); // No fragmentar
    ip->ttl = 64;
    ip->protocolo = protocolo;
    ip->origen[0] = g_cfg.configurada ? g_cfg.ip[0] : 0;
    ip->origen[1] = g_cfg.configurada ? g_cfg.ip[1] : 0;
    ip->origen[2] = g_cfg.configurada ? g_cfg.ip[2] : 0;
    ip->origen[3] = g_cfg.configurada ? g_cfg.ip[3] : 0;
    memcpy(ip->destino, destino_ip, 4);
    ip->checksum = 0;
    ip->checksum = red_be16(red_checksum(ip, 20));

    memcpy(g_trama + sizeof(struct red_eth) + 20, payload, payload_len);
    return red_enviar_trama(g_trama, sizeof(struct red_eth) + 20 + payload_len);
}

static int red_enviar_ipv4(const uint8_t destino_ip[4], uint8_t protocolo,
                           const void *payload, uint32_t payload_len) {
    if (!g_cfg.configurada) return -1;

    uint8_t mac_destino[6];
    uint8_t siguiente[4];

    if (red_ip_igual(destino_ip, (const uint8_t[4]){255,255,255,255})) {
        memset(mac_destino, 0xFF, 6);
    } else {
        if (red_misma_subred(destino_ip)) {
            memcpy(siguiente, destino_ip, 4);
        } else {
            memcpy(siguiente, g_cfg.puerta, 4);
        }
        if (red_arp_resolver(siguiente, mac_destino, 2000) != 0) return -1;
    }
    return red_enviar_ipv4_mac(mac_destino, destino_ip, protocolo, payload, payload_len);
}

// ------------------------------------------------------------------- ARP
int red_arp_resolver(const uint8_t ip[4], uint8_t mac_salida[6], int timeout_ms) { RED_EXCLUSIVA();
    for (int i = 0; i < 8; i++) {
        if (g_arp[i].valida && red_ip_igual(g_arp[i].ip, ip)) {
            g_arp[i].ultimo_uso=tiempo_obtener_milisegundos();
            memcpy(mac_salida, g_arp[i].mac, 6);
            return 0;
        }
    }

    const struct red_info *info = red_obtener_info();
    struct red_eth *eth = (struct red_eth *)g_trama;
    struct red_arp *arp = (struct red_arp *)(g_trama + sizeof(struct red_eth));

    memset(g_trama, 0, sizeof(struct red_eth) + sizeof(struct red_arp));
    memset(eth->destino, 0xFF, 6);
    memcpy(eth->origen, info->mac, 6);
    eth->tipo = red_be16(RED_ETH_ARP);
    arp->tipo_hw = red_be16(1);
    arp->tipo_proto = red_be16(RED_ETH_IPV4);
    arp->largo_hw = 6;
    arp->largo_proto = 4;
    arp->operacion = red_be16(1);
    memcpy(arp->emisor_mac, info->mac, 6);
    memcpy(arp->emisor_ip, g_cfg.ip, 4);
    memset(arp->objetivo_mac, 0, 6);
    memcpy(arp->objetivo_ip, ip, 4);

    red_enviar_trama(g_trama, sizeof(struct red_eth) + sizeof(struct red_arp));

    uint64_t inicio = tiempo_obtener_milisegundos();
    while ((int)(tiempo_obtener_milisegundos() - inicio) < timeout_ms) {
        red_pila_sondeo();
        for (int i = 0; i < 8; i++) {
            if (g_arp[i].valida && red_ip_igual(g_arp[i].ip, ip)) {
                g_arp[i].ultimo_uso=tiempo_obtener_milisegundos();
                memcpy(mac_salida, g_arp[i].mac, 6);
                return 0;
            }
        }
        esperar_milisegundos(1);
    }
    return -1;
}

// ------------------------------------------------------------------- ICMP
static void red_icmp_responder(const uint8_t *rx, uint32_t carga_len) {
    const struct red_info *info = red_obtener_info();
    const struct red_eth *rx_eth = (const struct red_eth *)rx;
    uint32_t ihl = (uint32_t)(rx[sizeof(struct red_eth)] & 0x0F) * 4;
    if (ihl < 20) return;

    struct red_eth *eth = (struct red_eth *)g_trama;
    memcpy(eth->destino, rx_eth->origen, 6);
    memcpy(eth->origen, info->mac, 6);
    eth->tipo = red_be16(RED_ETH_IPV4);

    struct red_ipv4 *ip = (struct red_ipv4 *)(g_trama + sizeof(struct red_eth));
    memcpy(ip, rx + sizeof(struct red_eth), 20);
    uint8_t tmp[4];
    memcpy(tmp, ip->origen, 4);
    memcpy(ip->origen, ip->destino, 4);
    memcpy(ip->destino, tmp, 4);
    /* La respuesta no replica opciones IP: cabecera fija de 20 bytes. */
    ip->ver_ihl = 0x45;
    ip->longitud_total = red_be16((uint16_t)(20 + carga_len));
    ip->checksum = 0;
    /* red_checksum devuelve orden de host; el campo viaja en orden de red. */
    ip->checksum = red_be16(red_checksum(ip, 20));

    uint8_t *icmp = g_trama + sizeof(struct red_eth) + 20;
    memcpy(icmp, rx + sizeof(struct red_eth) + ihl, carga_len);
    icmp[0] = 0;   // Eco reply
    icmp[2] = 0;
    icmp[3] = 0;
    uint16_t ck = red_checksum(icmp, carga_len);
    icmp[2] = (uint8_t)(ck >> 8);
    icmp[3] = (uint8_t)(ck & 0xFF);

    red_enviar_trama(g_trama, sizeof(struct red_eth) + 20 + carga_len);
}

int red_ping(const uint8_t ip[4], int intentos, int timeout_ms) { RED_EXCLUSIVA();
    if (!g_cfg.configurada) return -1;
    g_ping_id = (uint16_t)(rdtsc() & 0xFFFF);
    g_ping_respuestas = 0;

    for (int n = 1; n <= intentos; n++) {
        g_ping_secuencia = (uint16_t)n;
        g_ping_ultimo_rtt = -1;
        g_ping_activo = 1;
        g_ping_envio_ms = tiempo_obtener_milisegundos();

        uint8_t *carga = g_payload;
        struct red_icmp *icmp = (struct red_icmp *)carga;
        icmp->tipo = 8;
        icmp->codigo = 0;
        icmp->id = red_be16(g_ping_id);
        icmp->secuencia = red_be16(g_ping_secuencia);
        // 32 bytes de datos de relleno
        for (int i = 0; i < 32; i++) carga[sizeof(struct red_icmp) + i] = (uint8_t)(i + n);
        icmp->checksum = 0;
        icmp->checksum = red_be16(red_checksum(carga, sizeof(struct red_icmp) + 32));

        if (red_enviar_ipv4(ip, RED_IP_ICMP, carga, sizeof(struct red_icmp) + 32) != 0) {
            g_ping_activo = 0;
            return -1;
        }

        uint64_t inicio = tiempo_obtener_milisegundos();
        while ((int)(tiempo_obtener_milisegundos() - inicio) < timeout_ms) {
            red_pila_sondeo();
            if (!g_ping_activo) break;
            esperar_milisegundos(1);
        }
        g_ping_activo = 0;

        if (g_ping_ultimo_rtt >= 0) {
            red_log("  [PING] Respuesta desde ");
            char buf[20];
            red_log(red_ip_a_texto(ip, buf, sizeof(buf)));
            red_log(" tiempo=");
            red_log_dec((uint64_t)g_ping_ultimo_rtt);
            red_log_linea(" ms");
        } else {
            red_log_linea("  [PING] Tiempo de espera agotado.");
        }
        esperar_milisegundos(200);
    }
    return g_ping_respuestas;
}

// -------------------------------------------------------------------- UDP
static int red_enviar_udp(const uint8_t destino_ip[4], uint16_t origen_port,
                          uint16_t destino_port, const void *datos, uint32_t longitud) {
    struct red_udp *udp = (struct red_udp *)g_payload;
    udp->origen_port = red_be16(origen_port);
    udp->destino_port = red_be16(destino_port);
    udp->longitud = red_be16((uint16_t)(sizeof(struct red_udp) + longitud));
    udp->checksum = 0; // Opcional en IPv4
    memcpy(g_payload + sizeof(struct red_udp), datos, longitud);
    return red_enviar_ipv4(destino_ip, RED_IP_UDP, g_payload, sizeof(struct red_udp) + longitud);
}

// ------------------------------------------------------------------- DHCP
static uint32_t red_dhcp_construir(uint8_t tipo, const uint8_t pedir_ip[4], const uint8_t servidor[4]) {
    const struct red_info *info = red_obtener_info();
    struct red_dhcp *dh = (struct red_dhcp *)g_payload;
    memset(dh, 0, 240);
    dh->op = 1;       // BOOTREQUEST
    dh->htype = 1;    // Ethernet
    dh->hlen = 6;
    dh->xid = red_be32(g_dhcp_xid);
    dh->banderas = red_be16(0x8000); // Broadcast
    memcpy(dh->chaddr, info->mac, 6);
    dh->magico = red_be32(0x63825363);

    uint8_t *op = dh->opciones;
    uint32_t i = 0;
    op[i++] = 53; op[i++] = 1; op[i++] = tipo;
    if (pedir_ip && !red_ip_cero(pedir_ip)) {
        op[i++] = 50; op[i++] = 4; memcpy(&op[i], pedir_ip, 4); i += 4;
    }
    if (servidor && !red_ip_cero(servidor)) {
        op[i++] = 54; op[i++] = 4; memcpy(&op[i], servidor, 4); i += 4;
    }
    // Lista de parámetros solicitados
    op[i++] = 55; op[i++] = 4; op[i++] = 1; op[i++] = 3; op[i++] = 6; op[i++] = 15;
    op[i++] = 255;
    return 240 + i;
}

// H172: contadores de respuestas DHCP para diagnosticar (red_dhcp_contadores).
static uint32_t g_dhcp_vistos, g_dhcp_xid_mal, g_dhcp_tipo_mal;
static uint8_t  g_dhcp_ultimo_tipo;
void red_dhcp_contadores(uint32_t *vistos, uint32_t *xid_mal, uint32_t *tipo_mal, uint8_t *ultimo_tipo) { RED_EXCLUSIVA();
    *vistos = g_dhcp_vistos; *xid_mal = g_dhcp_xid_mal; *tipo_mal = g_dhcp_tipo_mal; *ultimo_tipo = g_dhcp_ultimo_tipo;
}

static void red_procesar_dhcp(const uint8_t *datos, uint32_t longitud) {
    if (longitud < 240) return;
    const struct red_dhcp *dh = (const struct red_dhcp *)datos;
    if (dh->op != 2) return;
    g_dhcp_vistos++;
    if (red_be32(dh->xid) != g_dhcp_xid) { g_dhcp_xid_mal++; return; }

    uint32_t off = 240;
    uint8_t tipo = 0;
    uint8_t mascara[4] = {0,0,0,0};
    uint8_t puerta[4] = {0,0,0,0};
    uint8_t dns[4] = {0,0,0,0};
    uint8_t servidor[4] = {0,0,0,0};

    while (off < longitud) {
        uint8_t code = datos[off++];
        if (code == 255) break;
        if (code == 0) continue;
        if (off >= longitud) break;
        uint8_t len = datos[off++];
        if (off + len > longitud) break;
        const uint8_t *val = datos + off;
        if (code == 53 && len >= 1) tipo = val[0];
        else if (code == 1 && len >= 4) memcpy(mascara, val, 4);
        else if (code == 3 && len >= 4) memcpy(puerta, val, 4);
        else if (code == 6 && len >= 4) memcpy(dns, val, 4);
        else if (code == 54 && len >= 4) memcpy(servidor, val, 4);
        off += len;
    }

    g_dhcp_ultimo_tipo = tipo;
    if (tipo != g_dhcp_esperado) { g_dhcp_tipo_mal++; return; }

    memcpy(g_dhcp_ip, dh->yiaddr, 4);
    if (!red_ip_cero(mascara)) memcpy(g_dhcp_mascara, mascara, 4);
    if (!red_ip_cero(puerta)) memcpy(g_dhcp_puerta, puerta, 4);
    if (!red_ip_cero(dns)) memcpy(g_dhcp_dns, dns, 4);
    if (!red_ip_cero(servidor)) memcpy(g_dhcp_servidor, servidor, 4);
    g_dhcp_recibido = 1;
}

static int red_dhcp_emitir(uint8_t tipo, const uint8_t pedir_ip[4], const uint8_t servidor[4], int esperado, int timeout_ms) {
    const struct red_info *info = red_obtener_info();
    uint32_t dhcp_len = red_dhcp_construir(tipo, pedir_ip, servidor);

    struct red_eth *eth = (struct red_eth *)g_trama;
    memset(eth->destino, 0xFF, 6);
    memcpy(eth->origen, info->mac, 6);
    eth->tipo = red_be16(RED_ETH_IPV4);

    struct red_ipv4 *ip = (struct red_ipv4 *)(g_trama + sizeof(struct red_eth));
    memset(ip, 0, 20);
    ip->ver_ihl = 0x45;
    ip->longitud_total = red_be16((uint16_t)(20 + sizeof(struct red_udp) + dhcp_len));
    ip->id = red_be16(g_ip_id++);
    ip->ttl = 64;
    ip->protocolo = RED_IP_UDP;
    memset(ip->origen, 0, 4);
    memset(ip->destino, 0xFF, 4);
    ip->checksum = red_be16(red_checksum(ip, 20));

    struct red_udp *udp = (struct red_udp *)(g_trama + sizeof(struct red_eth) + 20);
    udp->origen_port = red_be16(68);
    udp->destino_port = red_be16(67);
    udp->longitud = red_be16((uint16_t)(sizeof(struct red_udp) + dhcp_len));
    udp->checksum = 0;
    memcpy((uint8_t *)udp + sizeof(struct red_udp), g_payload, dhcp_len);

    // H168: copia propia; red_pila_sondeo reutiliza g_trama (respuestas ARP). Se reenvia cada
    // 1,5 s con el mismo xid (RFC 2131 4.1): por WiFi una sola trama perdida tumbaba el paso.
    static uint8_t copia[RED_MAX_TRAMA];
    uint32_t largo = sizeof(struct red_eth) + 20 + sizeof(struct red_udp) + dhcp_len;
    if (largo > sizeof(copia)) return -1;
    memcpy(copia, g_trama, largo);
    g_dhcp_esperado = esperado;
    g_dhcp_recibido = 0;
    red_enviar_trama(copia, largo);
    uint64_t inicio = tiempo_obtener_milisegundos(), reenvio = inicio + 1500;
    while ((int)(tiempo_obtener_milisegundos() - inicio) < timeout_ms) {
        red_pila_sondeo();
        if (g_dhcp_recibido) { g_dhcp_esperado = 0; return 0; }
        if (tiempo_obtener_milisegundos() >= reenvio) { red_enviar_trama(copia, largo); reenvio += 1500; }
        esperar_milisegundos(1);
    }
    g_dhcp_esperado = 0;
    return -1;
}

int red_dhcp(int timeout_ms) { RED_EXCLUSIVA();
    const struct red_info *info = red_obtener_info();
    if (!info->controlador_listo) return -1;
    if (!info->enlace_activo) red_enlace_actualizar();

    // H172: el mismo xid durante 2 min: un servidor lento (prueba la IP antes de ofrecerla)
    // responde al intento anterior, y con un xid nuevo esa oferta se descartaba.
    static uint64_t xid_ms;
    uint64_t ahora_xid = tiempo_obtener_milisegundos();
    if (!g_dhcp_xid || ahora_xid - xid_ms > 120000) g_dhcp_xid = (uint32_t)rdtsc() ^ 0x5441454Bu;
    xid_ms = ahora_xid;
    int por_intento = timeout_ms / 3;
    if (por_intento < 1000) por_intento = 1000;

    uint8_t cero[4] = {0,0,0,0};

    // DESCUBRIR
    if (red_dhcp_emitir(1, cero, cero, 2, por_intento) != 0) return -2;
    // PETICIÓN
    if (red_dhcp_emitir(3, g_dhcp_ip, g_dhcp_servidor, 5, por_intento) != 0) return -3;

    memcpy(g_cfg.ip, g_dhcp_ip, 4);
    if (red_ip_cero(g_dhcp_mascara)) { g_dhcp_mascara[0]=255; g_dhcp_mascara[1]=255; g_dhcp_mascara[2]=255; g_dhcp_mascara[3]=0; }
    memcpy(g_cfg.mascara, g_dhcp_mascara, 4);
    memcpy(g_cfg.puerta, g_dhcp_puerta, 4);
    memcpy(g_cfg.dns[0], g_dhcp_dns, 4);
    memset(g_cfg.dns[1], 0, 4);
    memcpy(g_cfg.servidor_dhcp, g_dhcp_servidor, 4);
    g_cfg.configurada = 1;
    g_cfg.por_dhcp = 1;
    memset(g_arp, 0, sizeof(g_arp));
    return 0;
}

// -------------------------------------------------------------------- DNS
static uint32_t red_dns_codificar_nombre(uint8_t *destino, const char *nombre) {
    uint32_t pos = 0;
    const char *p = nombre;
    while (*p) {
        const char *inicio = p;
        while (*p && *p != '.') p++;
        uint32_t len = (uint32_t)(p - inicio);
        if (len == 0 || len > 63) break;
        destino[pos++] = (uint8_t)len;
        memcpy(destino + pos, inicio, len);
        pos += len;
        if (*p == '.') p++;
    }
    destino[pos++] = 0;
    return pos;
}

static uint32_t red_dns_saltar_nombre(const uint8_t *datos, uint32_t longitud, uint32_t pos) {
    while (pos < longitud) {
        uint8_t len = datos[pos];
        if (len == 0) return pos + 1;
        if ((len & 0xC0) == 0xC0) return pos + 2; // puntero de compresión
        pos += 1 + len;
    }
    return pos;
}

static void red_procesar_dns(const uint8_t *datos, uint32_t longitud) {
    if (longitud < 12) return;
    uint16_t id = (uint16_t)((datos[0] << 8) | datos[1]);
    if (id != g_dns_id) return;
    uint16_t flags = (uint16_t)((datos[2] << 8) | datos[3]);
    if ((flags & 0x8000) == 0) return;         // no es respuesta
    uint16_t qd = (uint16_t)((datos[4] << 8) | datos[5]);
    uint16_t an = (uint16_t)((datos[6] << 8) | datos[7]);

    uint32_t pos = 12;
    for (uint16_t i = 0; i < qd; i++) {
        pos = red_dns_saltar_nombre(datos, longitud, pos);
        pos += 4;
        if (pos > longitud) return;
    }

    for (uint16_t i = 0; i < an; i++) {
        pos = red_dns_saltar_nombre(datos, longitud, pos);
        if (pos + 10 > longitud) return;
        uint16_t tipo = (uint16_t)((datos[pos] << 8) | datos[pos + 1]);
        uint16_t rdlen = (uint16_t)((datos[pos + 8] << 8) | datos[pos + 9]);
        pos += 10;
        if (pos + rdlen > longitud) return;
        if (tipo == 1 && rdlen == 4) {
            memcpy(g_dns_ip, datos + pos, 4);
            g_dns_recibido = 1;
            return;
        }
        pos += rdlen;
    }
}

int red_resolver_nombre(const char *nombre, uint8_t ip_salida[4], int timeout_ms) { RED_EXCLUSIVA();
    if (!g_cfg.configurada) return -1;

    // ¿Ya es una IP literal?
    uint8_t literal[4];
    if (red_texto_a_ip(nombre, literal) == 0) {
        memcpy(ip_salida, literal, 4);
        return 0;
    }

    if (red_ip_cero(g_cfg.dns[0])) return -2;

    g_dns_id = (uint16_t)(rdtsc() & 0xFFFF);
    g_dns_puerto_local = (uint16_t)(20000 + (rdtsc() % 20000));
    g_dns_recibido = 0;

    uint8_t consulta[256];
    memset(consulta, 0, 12);
    consulta[0] = (uint8_t)(g_dns_id >> 8);
    consulta[1] = (uint8_t)(g_dns_id);
    consulta[2] = 0x01; // Recursión deseada
    consulta[5] = 0x01; // QDCOUNT = 1
    uint32_t pos = 12;
    pos += red_dns_codificar_nombre(consulta + pos, nombre);
    consulta[pos++] = 0x00; consulta[pos++] = 0x01; // Tipo A
    consulta[pos++] = 0x00; consulta[pos++] = 0x01; // Clase IN

    if (red_enviar_udp(g_cfg.dns[0], g_dns_puerto_local, 53, consulta, pos) != 0) return -3;

    uint64_t inicio = tiempo_obtener_milisegundos();
    while ((int)(tiempo_obtener_milisegundos() - inicio) < timeout_ms) {
        red_pila_sondeo();
        if (g_dns_recibido) {
            memcpy(ip_salida, g_dns_ip, 4);
            return 0;
        }
        esperar_milisegundos(1);
    }
    return -4;
}

// -------------------------------------------------------------------- TCP
static uint16_t red_tcp_checksum(const struct red_ipv4 *ip, const uint8_t *segmento, uint32_t longitud) {
    uint32_t suma = 0;
    suma = red_suma(ip->origen, 4, suma);
    suma = red_suma(ip->destino, 4, suma);
    suma += RED_IP_TCP;
    suma += (uint16_t)longitud;
    suma = red_suma(segmento, longitud, suma);
    return red_finalizar_suma(suma);
}

static int red_tcp_enviar_segmento_seq(const uint8_t *datos, uint32_t longitud, uint8_t banderas, uint32_t seq) {
    struct red_tcp *tcp = (struct red_tcp *)g_payload;
    memset(tcp, 0, sizeof(*tcp));
    tcp->origen_port = red_be16(g_tcp.puerto_local);
    tcp->destino_port = red_be16(g_tcp.puerto_remoto);
    tcp->secuencia = red_be32(seq);
    tcp->acuse = red_be32(g_tcp.ack);
    tcp->offset_reservado = 0x50; // 5 * 4 = 20 bytes, sin opciones
    tcp->banderas = banderas;
    uint32_t libre = g_tcp_flujo ? g_tcp.rx_capacidad - g_tcp.rx_longitud : 64240;
    if (g_tcp_flujo) {                           // H182: tope configurable
        uint32_t tope = g_tcp_ventana > g_tcp.rx_longitud ? g_tcp_ventana - g_tcp.rx_longitud : 0;
        if (tope < libre) libre = tope;
    }
    // H177: en el SYN la ventana no se escala; despues, si el otro lado acepto WS, va >> RED_TCP_WS.
    uint32_t campo = (!(banderas & RED_TCP_SYN) && g_tcp.ws_ok) ? libre >> RED_TCP_WS : libre;
    tcp->ventana = red_be16((uint16_t)(campo > 0xFFFF ? 0xFFFF : campo));
    tcp->urgente = 0;
    uint32_t opc = 0;
    if (banderas & RED_TCP_SYN) {                // H177: MSS 1460, NOP, Window Scale; H184: NOP NOP SACK permitido
        uint8_t *o = g_payload + sizeof(struct red_tcp);
        o[0] = 2; o[1] = 4; o[2] = (uint8_t)(1460 >> 8); o[3] = (uint8_t)(1460 & 0xFF);
        o[4] = 1; o[5] = 3; o[6] = 3; o[7] = RED_TCP_WS;
        o[8] = 1; o[9] = 1; o[10] = 4; o[11] = 2;
        opc = 12;
        tcp->offset_reservado = 0x80;
    } else if (g_tcp.sack_ok && g_tcp.n_trozos && (banderas & RED_TCP_ACK) && !longitud) {
        // H184: hasta 3 bloques SACK con los trozos fuera de orden (el mas reciente primero).
        uint8_t *o = g_payload + sizeof(struct red_tcp);
        uint32_t k = g_tcp.n_trozos < 3 ? g_tcp.n_trozos : 3;
        o[0] = 1; o[1] = 1; o[2] = 5; o[3] = (uint8_t)(2 + 8 * k);
        for (uint32_t i = 0; i < k; i++) {
            uint32_t j = g_tcp.n_trozos - 1 - i, a = g_tcp.trozo_ini[j], b = g_tcp.trozo_fin[j];
            uint8_t *q = o + 4 + 8 * i;
            q[0] = (uint8_t)(a >> 24); q[1] = (uint8_t)(a >> 16); q[2] = (uint8_t)(a >> 8); q[3] = (uint8_t)a;
            q[4] = (uint8_t)(b >> 24); q[5] = (uint8_t)(b >> 16); q[6] = (uint8_t)(b >> 8); q[7] = (uint8_t)b;
        }
        opc = 4 + 8 * k;
        tcp->offset_reservado = (uint8_t)(((20 + opc) / 4) << 4);
    }
    if (longitud) memcpy(g_payload + sizeof(struct red_tcp) + opc, datos, longitud);
    longitud += opc;
    g_tcp.sin_ack = 0;                           // todo segmento lleva el ACK acumulado

    // Pseudo-cabecera IPv4 para el checksum TCP.
    struct red_ipv4 pseudo;
    memset(&pseudo, 0, sizeof(pseudo));
    memcpy(pseudo.origen, g_cfg.ip, 4);
    memcpy(pseudo.destino, g_tcp.remota_ip, 4);
    tcp->checksum = 0;
    tcp->checksum = red_be16(red_tcp_checksum(&pseudo, g_payload, (uint32_t)sizeof(struct red_tcp) + longitud));

    return red_enviar_ipv4(g_tcp.remota_ip, RED_IP_TCP, g_payload, (uint32_t)sizeof(struct red_tcp) + longitud);
}

static int red_tcp_enviar_segmento(const uint8_t *datos, uint32_t longitud, uint8_t banderas) {
    return red_tcp_enviar_segmento_seq(datos, longitud, banderas, g_tcp.seq);
}

static void red_tcp_guardar_ultimo(const uint8_t *datos, uint32_t longitud, uint8_t banderas) {
    g_tcp.seq_ultimo = g_tcp.seq;
    g_tcp.len_ultimo = longitud;
    g_tcp.ultimo_era_fin = (banderas & RED_TCP_FIN) ? 1 : 0;
    g_tcp.ultimo_flags = banderas;
    if (longitud && datos) memcpy(g_tcp.ultimo_payload, datos, longitud);
    g_tcp.reintentos = 0;
    g_tcp.ultimo_envio_ms = tiempo_obtener_milisegundos();
    g_tcp.ack_de_datos = 0;
}

static void red_procesar_tcp_una(struct red_ipv4 *ip, uint8_t *tcp_carga, uint32_t carga_len);
// H183: reparto por conexion.
static void red_procesar_tcp(struct red_ipv4 *ip, uint8_t *tcp_carga, uint32_t carga_len) {
    if (carga_len < sizeof(struct red_tcp)) return;
    const struct red_tcp *t = (const struct red_tcp *)tcp_carga;
    uint16_t dp = red_be16(t->destino_port), sp = red_be16(t->origen_port);
    for (int i = 0; i < RED_TCP_CONEXIONES; i++) {
        struct red_tcp_conexion *c = &g_tcp_tabla[i];
        if (!c->activa || c->puerto_local != dp || c->puerto_remoto != sp || !red_ip_igual(ip->origen, c->remota_ip)) continue;
        struct red_tcp_conexion *previa = g_tcp_actual;
        g_tcp_actual = c;
        red_procesar_tcp_una(ip, tcp_carga, carga_len);
        g_tcp_actual = previa;
        return;
    }
}
// H184: trozos fuera de orden. Se mantienen ordenados por llegada (el ultimo, el mas reciente: va primero en SACK).
static void red_tcp_anotar_trozo(uint32_t a, uint32_t b) {
    for (uint32_t i = 0; i < g_tcp.n_trozos; i++) {           // fundir con uno que se toque o solape
        if ((int32_t)(a - g_tcp.trozo_fin[i]) <= 0 && (int32_t)(b - g_tcp.trozo_ini[i]) >= 0) {
            uint32_t na = (int32_t)(a - g_tcp.trozo_ini[i]) < 0 ? a : g_tcp.trozo_ini[i];
            uint32_t nb = (int32_t)(b - g_tcp.trozo_fin[i]) > 0 ? b : g_tcp.trozo_fin[i];
            for (uint32_t j = i; j + 1 < g_tcp.n_trozos; j++) { g_tcp.trozo_ini[j] = g_tcp.trozo_ini[j + 1]; g_tcp.trozo_fin[j] = g_tcp.trozo_fin[j + 1]; }
            g_tcp.n_trozos--;
            red_tcp_anotar_trozo(na, nb);                          // puede tocar a otro mas
            return;
        }
    }
    if (g_tcp.n_trozos == 8) {                                  // lleno: se olvida el mas viejo (se reenviara)
        for (uint32_t j = 0; j + 1 < 8; j++) { g_tcp.trozo_ini[j] = g_tcp.trozo_ini[j + 1]; g_tcp.trozo_fin[j] = g_tcp.trozo_fin[j + 1]; }
        g_tcp.n_trozos--;
    }
    g_tcp.trozo_ini[g_tcp.n_trozos] = a; g_tcp.trozo_fin[g_tcp.n_trozos] = b; g_tcp.n_trozos++;
}
// Con el ACK avanzado, absorber los trozos que ahora quedan contiguos (sus bytes ya estan en el anillo).
static void red_tcp_unir_trozos(void) {
    for (int otra = 1; otra; ) {
        otra = 0;
        for (uint32_t i = 0; i < g_tcp.n_trozos; i++) {
            if ((int32_t)(g_tcp.trozo_ini[i] - g_tcp.ack) > 0) continue;   // aun hay hueco antes
            if ((int32_t)(g_tcp.trozo_fin[i] - g_tcp.ack) > 0) {
                uint32_t extra = g_tcp.trozo_fin[i] - g_tcp.ack;
                g_tcp.rx_longitud += extra; g_tcp.ack += extra;
            }
            for (uint32_t j = i; j + 1 < g_tcp.n_trozos; j++) { g_tcp.trozo_ini[j] = g_tcp.trozo_ini[j + 1]; g_tcp.trozo_fin[j] = g_tcp.trozo_fin[j + 1]; }
            g_tcp.n_trozos--;
            otra = 1;
            break;
        }
    }
}
static void red_procesar_tcp_una(struct red_ipv4 *ip, uint8_t *tcp_carga, uint32_t carga_len) {
    if (!g_tcp.activa) return;
    if (!red_ip_igual(ip->origen, g_tcp.remota_ip)) return;
    if (carga_len < sizeof(struct red_tcp) || red_tcp_checksum(ip, tcp_carga, carga_len) != 0) return;

    struct red_tcp *tcp = (struct red_tcp *)tcp_carga;
    if (red_be16(tcp->destino_port) != g_tcp.puerto_local) return;
    if (red_be16(tcp->origen_port) != g_tcp.puerto_remoto) return;

    uint8_t banderas = tcp->banderas & 0x3F;
    uint32_t doff = (uint32_t)(tcp->offset_reservado >> 4) * 4;
    if (doff < 20 || doff > carga_len) return;
    uint32_t datos_len = carga_len - doff;
    uint8_t *datos = tcp_carga + doff;
    uint32_t seq = red_be32(tcp->secuencia);
    uint32_t ackno = red_be32(tcp->acuse);

    if (banderas & RED_TCP_RST) {
        g_tcp.estado = 0;
        g_tcp.activa = 0;
        return;
    }

    if (banderas & RED_TCP_SYN) {
        if (g_tcp.estado == 1 && (banderas & RED_TCP_ACK) && ackno == g_tcp.isn + 1) {
            // H177: Window Scale (kind 3) en el SYN-ACK = escalado aceptado en ambos sentidos.
            for (uint32_t q = 20; q < doff; ) {
                uint8_t k = tcp_carga[q];
                if (k == 0) break;
                if (k == 1) { q++; continue; }
                if (q + 1 >= doff || tcp_carga[q + 1] < 2) break;
                if (k == 3 && tcp_carga[q + 1] == 3) g_tcp.ws_ok = 1;
                if (k == 4 && tcp_carga[q + 1] == 2) g_tcp.sack_ok = 1;      // H184
                q += tcp_carga[q + 1];
            }
            g_tcp.ack = seq + 1;
            g_tcp.seq = g_tcp.isn + 1;
            g_tcp.estado = 2;
            g_tcp.ack_de_datos = 1;
            red_tcp_enviar_segmento(NULL, 0, RED_TCP_ACK);
        }
        return;
    }

    if ((banderas & RED_TCP_ACK) && g_tcp.estado >= 2) {
        uint32_t esperado = g_tcp.seq_ultimo + g_tcp.len_ultimo +
            ((g_tcp.ultimo_flags & (RED_TCP_FIN | RED_TCP_SYN)) ? 1 : 0);
        // Stop-and-wait: aceptar solo el ACK exacto, tambien al envolver uint32.
        if (ackno == esperado) g_tcp.ack_de_datos = 1;
    }

    if (datos_len > 0) {
        // H184: segmento que se solapa con lo ya recibido (retransmision reempaquetada, RFC 9293 3.10.7.4):
        // recortar el prefijo repetido y quedarse con el resto. Antes se descartaba entero y, con servidores
        // de internet, el reenvio identico volvia a descartarse con espera exponencial (pausas de 10 s).
        int32_t repetido = (int32_t)(g_tcp.ack - seq);
        if (g_tcp.estado >= 2 && repetido > 0 && (uint32_t)repetido < datos_len) {
            datos += repetido; datos_len -= (uint32_t)repetido; seq = g_tcp.ack;
            g_tcp_recortes++;
        }
        if (seq == g_tcp.ack) {
            uint32_t espacio = g_tcp.rx_capacidad - g_tcp.rx_longitud;
            uint32_t copiar = datos_len < espacio ? datos_len : espacio;
            if (copiar < datos_len && !g_tcp_flujo) g_tcp.rx_desbordado = 1;
            if (copiar && g_tcp.rx_destino && g_tcp_flujo) {      // H177: anillo
                uint32_t pos = (g_tcp.rx_ini + g_tcp.rx_longitud) % g_tcp.rx_capacidad;
                uint32_t a = g_tcp.rx_capacidad - pos < copiar ? g_tcp.rx_capacidad - pos : copiar;
                memcpy(g_tcp.rx_destino + pos, datos, a);
                if (copiar > a) memcpy(g_tcp.rx_destino, datos + a, copiar - a);
                g_tcp.rx_longitud += copiar;
            } else if (copiar && g_tcp.rx_destino) {
                memcpy(g_tcp.rx_destino + g_tcp.rx_longitud, datos, copiar);
                g_tcp.rx_longitud += copiar;
            }
            // Flujo: confirmar solo bytes retenidos. Una ventana llena no debe
            // ACKear y perder la cola de una descarga binaria grande.
            g_tcp.ack += g_tcp_flujo ? copiar : datos_len;
            // H184: si habia trozos adelantados, este segmento pudo cerrar el hueco: absorberlos y ACK inmediato.
            int habia_trozos = g_tcp_flujo && g_tcp.n_trozos;
            if (habia_trozos) red_tcp_unir_trozos();
            // H177: ACK retardado en el flujo (RFC 1122 4.2.3.2: al menos cada 2 segmentos completos);
            // red_tcp_flujo_recibir confirma lo pendiente en cada sondeo.
            if (!habia_trozos && g_tcp_flujo && copiar == datos_len && ++g_tcp.sin_ack < g_tcp_ack_cada && !(banderas & RED_TCP_PSH)) goto sin_ack;
        } else if ((int32_t)(seq - g_tcp.ack) > 0 && g_tcp_flujo && g_tcp.rx_destino) {
            // H184: adelantado. Se escribe en su lugar del anillo (si cabe en el espacio libre) y se anota.
            g_tcp_fuera_orden++;
            uint32_t off = seq - g_tcp.ack, libre = g_tcp.rx_capacidad - g_tcp.rx_longitud;
            if (off + datos_len <= libre) {
                uint32_t pos = (g_tcp.rx_ini + g_tcp.rx_longitud + off) % g_tcp.rx_capacidad;
                uint32_t a = g_tcp.rx_capacidad - pos < datos_len ? g_tcp.rx_capacidad - pos : datos_len;
                memcpy(g_tcp.rx_destino + pos, datos, a);
                if (datos_len > a) memcpy(g_tcp.rx_destino, datos + a, datos_len - a);
                red_tcp_anotar_trozo(seq, seq + datos_len);
            }
        }
        red_tcp_enviar_segmento(NULL, 0, RED_TCP_ACK);
    sin_ack:;
    }

    if ((banderas & RED_TCP_FIN) && seq + datos_len == g_tcp.ack && !g_tcp.fin_recibido) {
        g_tcp.ack += 1;
        g_tcp.fin_recibido = 1;
        g_tcp.rx_completo = 1;
        g_tcp.estado = 4;
        red_tcp_enviar_segmento(NULL, 0, RED_TCP_ACK);
    }
}

static int red_tcp_esperar(int (*condicion)(void), int timeout_ms) {
    uint64_t inicio = tiempo_obtener_milisegundos();
    while ((int)(tiempo_obtener_milisegundos() - inicio) < timeout_ms) {
        red_pila_sondeo();
        if (condicion()) return 0;
        esperar_milisegundos(1);
    }
    red_pila_sondeo();
    return condicion() ? 0 : -1;
}

static int red_tcp_cond_establecido(void) { return g_tcp.estado == 2; }
static int red_tcp_cond_ack(void) { return g_tcp.ack_de_datos || g_tcp.estado == 0 || g_tcp.estado == 4; }
static int red_tcp_cond_cerrado(void) { return g_tcp.estado == 0 || g_tcp.estado == 4 || g_tcp.rx_completo; }

static int red_tcp_conectar(const uint8_t ip[4], uint16_t puerto, int timeout_ms) {
    int en_uso = g_tcp.en_uso; char *anillo = g_tcp.anillo;            // H183: datos del manejador
    memset(&g_tcp, 0, sizeof(g_tcp));
    g_tcp.en_uso = en_uso; g_tcp.anillo = anillo;
    memcpy(g_tcp.remota_ip, ip, 4);
    g_tcp.puerto_remoto = puerto;
    for (int intento = 0; intento < 16; intento++) {                    // H183: puerto local unico en la tabla
        g_tcp.puerto_local = (uint16_t)(40000 + (rdtsc() % 20000));
        int repetido = 0;
        for (int i = 0; i < RED_TCP_CONEXIONES; i++)
            if (&g_tcp_tabla[i] != g_tcp_actual && g_tcp_tabla[i].activa && g_tcp_tabla[i].puerto_local == g_tcp.puerto_local) repetido = 1;
        if (!repetido) break;
    }
    g_tcp.isn = (uint32_t)rdtsc();
    g_tcp.seq = g_tcp.isn;
    g_tcp.ack = 0;
    g_tcp.estado = 1;
    g_tcp.activa = 1;
    red_tcp_guardar_ultimo(NULL, 0, RED_TCP_SYN);

    if (red_tcp_enviar_segmento(NULL, 0, RED_TCP_SYN) != 0) {
        g_tcp.activa = 0;
        return -1;
    }
    if (red_tcp_esperar(red_tcp_cond_establecido, timeout_ms) != 0) {
        g_tcp.activa = 0;
        g_tcp.estado = 0;
        return -2;
    }
    return 0;
}

static int red_tcp_enviar(const void *datos, uint32_t longitud, int timeout_ms) {
    if (!g_tcp.activa || g_tcp.estado != 2 || longitud > RED_TCP_MAX_SEGMENTO) return -1;
    red_tcp_guardar_ultimo((const uint8_t *)datos, longitud, RED_TCP_ACK | RED_TCP_PSH);
    g_tcp.seq += longitud;
    if (red_tcp_enviar_segmento_seq((const uint8_t *)datos, longitud,
                                  RED_TCP_ACK | RED_TCP_PSH, g_tcp.seq_ultimo) != 0) return -1;
    if (red_tcp_esperar(red_tcp_cond_ack, timeout_ms) != 0) return -2;
    if (!g_tcp.ack_de_datos) return -3;
    return 0;
}

static void red_tcp_cerrar(void) {
    if (!g_tcp.activa) return;
    if (g_tcp.estado == 2 || g_tcp.estado == 4) {
        red_tcp_enviar_segmento(NULL, 0, RED_TCP_ACK | RED_TCP_FIN);
        red_tcp_guardar_ultimo(NULL, 0, RED_TCP_ACK | RED_TCP_FIN);
        g_tcp.seq += 1;
        g_tcp.estado = 3;
        red_tcp_esperar(red_tcp_cond_cerrado, 500);
    }
    g_tcp.activa = 0;
    g_tcp.estado = 0;
}

static void red_tcp_temporizador_una(void);
static void red_tcp_temporizador(void) {           // H183: todas las conexiones
    for (int i = 0; i < RED_TCP_CONEXIONES; i++) {
        if (!g_tcp_tabla[i].activa) continue;
        struct red_tcp_conexion *previa = g_tcp_actual;
        g_tcp_actual = &g_tcp_tabla[i];
        red_tcp_temporizador_una();
        g_tcp_actual = previa;
    }
}
static void red_tcp_temporizador_una(void) {
    if (!g_tcp.activa || g_tcp.ack_de_datos) return;
    if (g_tcp.estado == 0) return;
    if ((int)(tiempo_obtener_milisegundos() - g_tcp.ultimo_envio_ms) < RED_TCP_REENVIO_MS) return;
    if (g_tcp.reintentos >= RED_TCP_REINTENTOS) {
        g_tcp.estado = 0;
        g_tcp.activa = 0;
        return;
    }
    g_tcp.reintentos++;
    g_tcp.ultimo_envio_ms = tiempo_obtener_milisegundos();
    red_tcp_enviar_segmento_seq(g_tcp.ultimo_payload, g_tcp.len_ultimo,
                               g_tcp.ultimo_flags, g_tcp.seq_ultimo);
}

// -------------------------------------------------------------------- HTTP
static int red_http_obtener_interno(const char *host, const char *ruta, uint16_t puerto,
                     char *salida, uint32_t capacidad, int timeout_ms) {
    if(!host || !*host || !salida || capacidad<64)return -4;
    if(!ruta || !*ruta)ruta="/";
    if(ruta[0]!='/')return -4;
    unsigned h=0,p=0;
    for(const char *c=host;*c;c++)if(++h>253 || !((*c>='a' && *c<='z') || (*c>='A' && *c<='Z') || (*c>='0' && *c<='9') || *c=='.' || *c=='-'))return -4;
    for(const char *c=ruta;*c;c++)if(++p>2048 || (unsigned char)*c<=32 || (unsigned char)*c==127 || *c=='#')return -4;
    if(!puerto)puerto=80;
    salida[0]=0;
    uint8_t ip[4];
    if (red_resolver_nombre(host, ip, timeout_ms) != 0) return -1;
    if (red_tcp_conectar(ip, puerto, timeout_ms) != 0) return -2;

    char peticion[3072];
    int n=taek_tls_snprintf(peticion,sizeof(peticion),"GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: TAEK-OS/1.0\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",ruta,host,(unsigned)puerto);
    if(n<0 || (unsigned)n>=sizeof(peticion)){red_tcp_cerrar();return -4;}

    // Preparar el búfer de recepción ANTES de enviar, por si la respuesta
    // llega en la misma ventana que el ACK de la petición.
    uint32_t tope = capacidad > 0 ? capacidad - 1 : 0;
    g_tcp.rx_destino = salida;
    g_tcp.rx_capacidad = tope;
    g_tcp.rx_longitud = 0;
    g_tcp.rx_completo = 0;

    for(int pos=0;pos<n;){
        unsigned segmento=(unsigned)(n-pos)>RED_TCP_MAX_SEGMENTO?RED_TCP_MAX_SEGMENTO:(unsigned)(n-pos);
        if(red_tcp_enviar(peticion+pos,segmento,timeout_ms)){red_tcp_cerrar();return -3;}
        pos+=(int)segmento;
    }

    int timeout=red_tcp_esperar(red_tcp_cond_cerrado, timeout_ms);
    int cerrado=g_tcp.estado==4;
    int desbordado=g_tcp.rx_desbordado;
    red_tcp_cerrar();

    if (salida && capacidad > 0) salida[g_tcp.rx_longitud < capacidad ? g_tcp.rx_longitud : capacidad - 1] = '\0';
    struct http_respuesta r;
    if(desbordado || http_respuesta_validar(salida,g_tcp.rx_longitud,!timeout && cerrado,&r)!=1){memset(salida,0,g_tcp.rx_longitud+1);return -5;}
    return (int)g_tcp.rx_longitud;
}

int red_http_obtener(const char *host, const char *ruta, uint16_t puerto,
                     char *salida, uint32_t capacidad, int timeout_ms) { RED_EXCLUSIVA();
    /* H143: primero se pausa la terminal por LAN, que es quien tiene la unica conexion TCP; antes se miraba si estaba
     * libre y, con la LAN activa, HTTP devolvia siempre -16 (HTTPS ya lo hacia en este orden). */
    red_stream_pausar(1);
    if (__atomic_test_and_set(&g_tcp_ocupada, __ATOMIC_ACQUIRE)) { red_stream_pausar(0); return -16; }
    int resultado = red_http_obtener_interno(host, ruta, puerto, salida, capacidad, timeout_ms);
    __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
    red_stream_pausar(0);
    return resultado;
}

static int red_tcp_tiempo_restante(uint64_t inicio, int timeout_ms) {
    uint64_t transcurrido = tiempo_obtener_milisegundos() - inicio;
    return transcurrido >= (uint64_t)timeout_ms ? 0 : timeout_ms - (int)transcurrido;
}

int red_tcp_flujo_abrir_timeout(const uint8_t ip[4], uint16_t puerto, int timeout_ms) { RED_EXCLUSIVA();
    if (!ip || !puerto || !g_cfg.configurada) return -2;
    if (__atomic_test_and_set(&g_tcp_ocupada, __ATOMIC_ACQUIRE)) return -16;
    uint8_t mac[6];
    uint64_t inicio = tiempo_obtener_milisegundos();
    const uint8_t *siguiente = red_misma_subred(ip) ? ip : g_cfg.puerta;
    int arp_timeout = timeout_ms / 2;
    if (arp_timeout < 200) arp_timeout = 200;
    if (arp_timeout > 2000) arp_timeout = 2000;
    if (red_arp_resolver(siguiente, mac, arp_timeout) != 0) goto fallo;
    int restante = red_tcp_tiempo_restante(inicio, timeout_ms);
    if (!restante || red_tcp_conectar(ip, puerto, restante) != 0) goto fallo;
    g_tcp.rx_destino = g_tcp_flujo_rx;
    g_tcp.rx_capacidad = sizeof(g_tcp_flujo_rx);
    g_tcp.rx_longitud = 0;
    g_tcp.rx_ini = 0;
    g_tcp.rx_desbordado = 0;
    g_tcp_flujo = 1;
    return 0;
fallo:
    g_tcp.activa = 0; g_tcp.estado = 0;
    __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
    return -3;
}

int red_tcp_flujo_abrir(const uint8_t ip[4], uint16_t puerto) { RED_EXCLUSIVA();
    return red_tcp_flujo_abrir_timeout(ip, puerto, 1500);
}

/* H189: apertura sin bloquear para el stream de consola. Antes, con el receptor caido, cada reintento esperaba
 * hasta 1,5 s dentro de consola/teclado y el sistema entero se congelaba ~1 s de cada 2,5 s.
 * iniciar: 0 SYN enviado, 1 falta ARP (pedido ya enviado; reintentar), <0 fallo. El cerrojo queda tomado con 0. */
int red_tcp_flujo_iniciar(const uint8_t ip[4], uint16_t puerto) { RED_EXCLUSIVA();
    if (!ip || !puerto || !g_cfg.configurada) return -2;
    if (__atomic_test_and_set(&g_tcp_ocupada, __ATOMIC_ACQUIRE)) return -16;
    uint8_t mac[6];
    const uint8_t *siguiente = red_misma_subred(ip) ? ip : g_cfg.puerta;
    red_pila_sondeo();                     /* procesa la respuesta ARP del intento anterior (y ping, DHCP...) */
    if (red_arp_resolver(siguiente, mac, 5) != 0) {           /* envia la peticion; la respuesta se toma al volver */
        __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
        return 1;
    }
    int en_uso = g_tcp.en_uso; char *anillo = g_tcp.anillo;
    memset(&g_tcp, 0, sizeof(g_tcp));
    g_tcp.en_uso = en_uso; g_tcp.anillo = anillo;
    memcpy(g_tcp.remota_ip, ip, 4);
    g_tcp.puerto_remoto = puerto;
    for (int intento = 0; intento < 16; intento++) {
        g_tcp.puerto_local = (uint16_t)(40000 + (rdtsc() % 20000));
        int repetido = 0;
        for (int i = 0; i < RED_TCP_CONEXIONES; i++)
            if (&g_tcp_tabla[i] != g_tcp_actual && g_tcp_tabla[i].activa && g_tcp_tabla[i].puerto_local == g_tcp.puerto_local) repetido = 1;
        if (!repetido) break;
    }
    g_tcp.isn = (uint32_t)rdtsc();
    g_tcp.seq = g_tcp.isn;
    g_tcp.estado = 1;
    g_tcp.activa = 1;
    red_tcp_guardar_ultimo(NULL, 0, RED_TCP_SYN);
    if (red_tcp_enviar_segmento(NULL, 0, RED_TCP_SYN) != 0) {
        g_tcp.activa = 0; g_tcp.estado = 0;
        __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
        return -3;
    }
    return 0;
}
/* 1 establecida (queda como flujo), 0 aun no, <0 rechazada o caida (cerrojo liberado). */
int red_tcp_flujo_sondear(void) { RED_EXCLUSIVA();
    red_pila_sondeo();
    if (g_tcp.estado == 2) {
        g_tcp.rx_destino = g_tcp_flujo_rx;
        g_tcp.rx_capacidad = sizeof(g_tcp_flujo_rx);
        g_tcp.rx_longitud = 0;
        g_tcp.rx_ini = 0;
        g_tcp.rx_desbordado = 0;
        g_tcp_flujo = 1;
        return 1;
    }
    if (!g_tcp.activa || g_tcp.estado != 1) { red_tcp_flujo_abandonar(); return -1; }
    return 0;
}
/* Corta un intento en curso (plazo vencido). */
void red_tcp_flujo_abandonar(void) { RED_EXCLUSIVA();
    g_tcp.activa = 0; g_tcp.estado = 0;
    __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
}

int red_tcp_flujo_enviar(const void *datos, uint32_t longitud) { RED_EXCLUSIVA();
    if (!g_tcp_flujo || !g_tcp.activa || g_tcp.estado != 2 ||
        !datos || !longitud || longitud > RED_TCP_MAX_SEGMENTO) return -1;
    if (!g_tcp.ack_de_datos) return 1;
    red_tcp_guardar_ultimo(datos, longitud, RED_TCP_ACK | RED_TCP_PSH);
    g_tcp.seq += longitud;
    return red_tcp_enviar_segmento_seq(datos, longitud, RED_TCP_ACK | RED_TCP_PSH, g_tcp.seq_ultimo);
}

int red_tcp_flujo_recibir(void *datos, uint32_t capacidad) { RED_EXCLUSIVA();
    if (!g_tcp_flujo || !datos || !capacidad) return -1;
    red_pila_sondeo();
    uint32_t n = g_tcp.rx_longitud < capacidad ? g_tcp.rx_longitud : capacidad;
    if (n) {                                     // H177: lectura del anillo
        uint32_t cap = g_tcp.rx_capacidad, a = cap - g_tcp.rx_ini < n ? cap - g_tcp.rx_ini : n;
        uint32_t antes = cap - g_tcp.rx_longitud;
        memcpy(datos, g_tcp.rx_destino + g_tcp.rx_ini, a);
        if (n > a) memcpy((uint8_t *)datos + a, g_tcp.rx_destino, n - a);
        g_tcp.rx_ini = (g_tcp.rx_ini + n) % cap;
        g_tcp.rx_longitud -= n;
        // ACK si hay segmentos sin confirmar o si la ventana se reabre de forma apreciable.
        if (g_tcp.activa && g_tcp.estado == 2 && (g_tcp.sin_ack >= g_tcp_ack_cada || antes < 2 * RED_TCP_MAX_SEGMENTO || n >= cap / 4))
            red_tcp_enviar_segmento(NULL, 0, RED_TCP_ACK);
        return (int)n;
    }
    if (g_tcp.sin_ack && g_tcp.activa && g_tcp.estado == 2) red_tcp_enviar_segmento(NULL, 0, RED_TCP_ACK);
    if (!g_tcp.activa || g_tcp.estado != 2 || g_tcp.rx_desbordado) return -1;
    return 0;
}

void red_tcp_flujo_cerrar(void) { RED_EXCLUSIVA();
    if (!g_tcp_flujo) return;
    if (g_tcp.activa && g_tcp.estado == 2) red_tcp_enviar_segmento(NULL, 0, RED_TCP_RST | RED_TCP_ACK);
    g_tcp.activa = 0; g_tcp.estado = 0; g_tcp_flujo = 0;
    __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
}

// ------------------------------------------------- H183: conexiones por manejador (1-7)
// Igual que el flujo de la conexion 0 (anillo, ventana escalada, ACK estirado), pero cada una con su anillo
// de 256 KiB y sin pausar el stream de consola. El envio sigue siendo de un segmento a la vez.
#define RED_TCP_ANILLO_H (256u * 1024u)
static struct red_tcp_conexion *red_tcp_h(int id) {
    if (id < 1 || id >= RED_TCP_CONEXIONES || !g_tcp_tabla[id].en_uso) return 0;
    return &g_tcp_tabla[id];
}

int red_tcp_abrir(const uint8_t ip[4], uint16_t puerto, int timeout_ms) { RED_EXCLUSIVA();
    if (!ip || !puerto || !g_cfg.configurada) return -2;
    int id = -1;
    for (int i = 1; i < RED_TCP_CONEXIONES; i++) if (!g_tcp_tabla[i].en_uso) { id = i; break; }
    if (id < 0) return -16;
    struct red_tcp_conexion *c = &g_tcp_tabla[id];
    if (!c->anillo) c->anillo = asignar_memoria(RED_TCP_ANILLO_H);
    if (!c->anillo) return -12;
    c->en_uso = 1;
    uint8_t mac[6];
    uint64_t inicio = tiempo_obtener_milisegundos();
    const uint8_t *siguiente = red_misma_subred(ip) ? ip : g_cfg.puerta;
    int arp = timeout_ms / 2;
    if (arp < 200) arp = 200;
    if (arp > 2000) arp = 2000;
    if (red_arp_resolver(siguiente, mac, arp) != 0) { c->en_uso = 0; return -3; }
    int restante = red_tcp_tiempo_restante(inicio, timeout_ms);
    struct red_tcp_conexion *previa = g_tcp_actual;
    g_tcp_actual = c;
    int r = restante ? red_tcp_conectar(ip, puerto, restante) : -2;
    if (!r) {
        g_tcp.rx_destino = g_tcp.anillo;
        g_tcp.rx_capacidad = RED_TCP_ANILLO_H;
        g_tcp.rx_longitud = 0;
        g_tcp.rx_ini = 0;
        g_tcp.rx_desbordado = 0;
        g_tcp.flujo = 1;
    } else {
        g_tcp.activa = 0; g_tcp.estado = 0; g_tcp.en_uso = 0;
    }
    g_tcp_actual = previa;
    return r ? -3 : id;
}

// 0 = segmento aceptado, 1 = el anterior aun sin ACK (reintentar), -1 = conexion caida o parametros.
int red_tcp_enviar_h(int id, const void *datos, uint32_t longitud) { RED_EXCLUSIVA();
    struct red_tcp_conexion *c = red_tcp_h(id);
    if (!c) return -1;
    struct red_tcp_conexion *previa = g_tcp_actual;
    g_tcp_actual = c;
    int r = red_tcp_flujo_enviar(datos, longitud);
    g_tcp_actual = previa;
    return r;
}

// Bytes leidos, 0 si no hay nada todavia, -1 si la conexion termino y no quedan datos.
int red_tcp_recibir_h(int id, void *datos, uint32_t capacidad) { RED_EXCLUSIVA();
    struct red_tcp_conexion *c = red_tcp_h(id);
    if (!c) return -1;
    struct red_tcp_conexion *previa = g_tcp_actual;
    g_tcp_actual = c;
    int r = red_tcp_flujo_recibir(datos, capacidad);
    if (r < 0 && g_tcp.estado == 4 && g_tcp.rx_longitud == 0) r = -1;   /* FIN del otro lado y anillo vacio */
    g_tcp_actual = previa;
    return r;
}

// 1 = establecida, 2 = el otro lado cerro (quedan datos por leer o no), 0 = caida o cerrada.
int red_tcp_estado_h(int id) { RED_EXCLUSIVA();
    struct red_tcp_conexion *c = red_tcp_h(id);
    if (!c || !c->activa) return 0;
    return c->estado == 2 ? 1 : c->estado == 4 ? 2 : 0;
}

void red_tcp_cerrar_h(int id) { RED_EXCLUSIVA();
    struct red_tcp_conexion *c = red_tcp_h(id);
    if (!c) return;
    struct red_tcp_conexion *previa = g_tcp_actual;
    g_tcp_actual = c;
    if (g_tcp.activa && (g_tcp.estado == 2 || g_tcp.estado == 4)) red_tcp_enviar_segmento(NULL, 0, RED_TCP_RST | RED_TCP_ACK);
    g_tcp.activa = 0; g_tcp.estado = 0; g_tcp.flujo = 0; g_tcp.en_uso = 0;
    g_tcp_actual = previa;
}

void red_tcp_cerrar_todas_h(void) { RED_EXCLUSIVA();
    for (int i = 1; i < RED_TCP_CONEXIONES; i++) if (g_tcp_tabla[i].en_uso) red_tcp_cerrar_h(i);
}

static int red_tcp_cond_respuesta(void) {
    return g_tcp.rx_longitud == g_tcp.rx_capacidad || g_tcp.rx_desbordado ||
           !g_tcp.activa || g_tcp.estado == 4;
}

int red_tcp_intercambiar(const uint8_t ip[4], uint16_t puerto,
                        const void *cabecera, uint32_t cabecera_len,
                        const void *datos, uint32_t longitud,
                        void *respuesta, uint32_t respuesta_len, int timeout_ms) { RED_EXCLUSIVA();
    if (!ip || !puerto || !cabecera || !cabecera_len || cabecera_len > RED_TCP_MAX_SEGMENTO ||
        !datos || !longitud || longitud > 128 * 1024 || !respuesta ||
        !respuesta_len || respuesta_len > RED_TCP_MAX_SEGMENTO ||
        timeout_ms <= 0 || timeout_ms > 60000) return -1;
    if (!g_cfg.configurada || !red_misma_subred(ip)) return -2;
    if (__atomic_test_and_set(&g_tcp_ocupada, __ATOMIC_ACQUIRE)) return -16;
    int resultado = -3;
    uint64_t inicio = tiempo_obtener_milisegundos();
    uint8_t mac[6];
    // Resolver antes de preparar TCP: ARP puede reentrar al sondeo de paquetes.
    int restante = red_tcp_tiempo_restante(inicio, timeout_ms);
    if (!restante || red_arp_resolver(ip, mac, restante < 2000 ? restante : 2000) != 0) goto salir;
    restante = red_tcp_tiempo_restante(inicio, timeout_ms);
    if (!restante || red_tcp_conectar(ip, puerto, restante) != 0) goto salir;
    g_tcp.rx_destino = respuesta;
    g_tcp.rx_capacidad = respuesta_len;
    restante = red_tcp_tiempo_restante(inicio, timeout_ms);
    if (!restante || red_tcp_enviar(cabecera, cabecera_len, restante) != 0) goto cerrar;
    for (uint32_t offset = 0; offset < longitud;) {
        uint32_t bloque = longitud - offset;
        if (bloque > RED_TCP_MAX_SEGMENTO) bloque = RED_TCP_MAX_SEGMENTO;
        restante = red_tcp_tiempo_restante(inicio, timeout_ms);
        if (!restante || red_tcp_enviar((const uint8_t *)datos + offset, bloque, restante) != 0) goto cerrar;
        offset += bloque;
    }
    restante = red_tcp_tiempo_restante(inicio, timeout_ms);
    if (restante && red_tcp_esperar(red_tcp_cond_respuesta, restante) == 0 &&
        g_tcp.rx_longitud == respuesta_len && !g_tcp.rx_desbordado) resultado = 0;
cerrar:
    red_tcp_cerrar();
salir:
    __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
    return resultado;
}

// --------------------------------------------------- Configuración y utilidades
const struct red_config *red_obtener_config(void) {
    return &g_cfg;
}

void red_configurar_vacia(void) { RED_EXCLUSIVA();
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(g_arp, 0, sizeof(g_arp));
}

int red_ip_estatica(const uint8_t ip[4], const uint8_t mascara[4],
                    const uint8_t puerta[4], const uint8_t dns[4]) { RED_EXCLUSIVA();
    if (!ip || !mascara || !puerta) return -1;
    memcpy(g_cfg.ip, ip, 4);
    memcpy(g_cfg.mascara, mascara, 4);
    memcpy(g_cfg.puerta, puerta, 4);
    memset(g_cfg.dns, 0, sizeof(g_cfg.dns));
    if (dns) memcpy(g_cfg.dns[0], dns, 4);
    g_cfg.configurada = 1;
    g_cfg.por_dhcp = 0;
    memset(g_arp, 0, sizeof(g_arp));
    return 0;
}

const char *red_ip_a_texto(const uint8_t ip[4], char *buffer, size_t tamano) {
    if (!buffer || tamano < 16) return "";
    int n = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        char temp[4];
        int d = 0;
        if (v == 0) temp[d++] = '0';
        while (v > 0) { temp[d++] = (char)('0' + (v % 10)); v /= 10; }
        while (d > 0 && (size_t)n < tamano - 1) buffer[n++] = temp[--d];
        if (i < 3 && (size_t)n < tamano - 1) buffer[n++] = '.';
    }
    buffer[n] = '\0';
    return buffer;
}

int red_texto_a_ip(const char *texto, uint8_t ip_salida[4]) {
    if (!texto) return -1;
    int valores[4] = {0,0,0,0};
    int componentes = 0;
    int actual = 0;
    int digitos = 0;
    for (const char *p = texto; ; p++) {
        if (*p >= '0' && *p <= '9') {
            actual = actual * 10 + (*p - '0');
            if (++digitos > 3 || actual > 255) return -1;
        } else if (*p == '.' || *p == '\0') {
            if (digitos == 0) return -1;
            valores[componentes++] = actual;
            actual = 0; digitos = 0;
            if (*p == '\0') break;
            if (componentes >= 4) return -1;
        } else {
            return -1;
        }
    }
    if (componentes != 4) return -1;
    for (int i = 0; i < 4; i++) ip_salida[i] = (uint8_t)valores[i];
    return 0;
}

// ------------------------------------------------------------- Autodiagnóstico
void red_autodiagnostico(void) { RED_EXCLUSIVA();
    red_log_linea("");
    red_log_linea("==============================================================");
    red_log_linea("  AUTODIAGNÓSTICO DE RED ETHERNET (e1000/e1000e) - TAEK OS");
    red_log_linea("==============================================================");

    const struct red_info *info = red_obtener_info();
    if (!info->presente) {
        red_log_linea("[RED] No se detectó ningún controlador Ethernet Intel soportado.");
        red_log_linea("      Verifica el cableado/PCI con 'lspci'.");
        return;
    }
    if (!info->controlador_listo) {
        red_log_linea("[RED] El controlador fue detectado pero no pudo inicializarse.");
        return;
    }

    char buf[20];
    char mac_txt[32];
    int m = 0;
    for (int i = 0; i < 6; i++) {
        const char *hex = "0123456789ABCDEF";
        mac_txt[m++] = hex[(info->mac[i] >> 4) & 0xF];
        mac_txt[m++] = hex[info->mac[i] & 0xF];
        if (i < 5) mac_txt[m++] = ':';
    }
    mac_txt[m] = '\0';

    red_log("  Modelo : "); red_log_linea(info->modelo);
    red_log("  MAC    : "); red_log_linea(mac_txt);
    red_log("  Enlace : ");
    red_log_linea(red_enlace_actualizar() ? "ACTIVO" : "SIN CABLE / CAÍDO");
    if (info->enlace_activo) {
        red_log("  Velocidad: "); red_log_dec(info->velocidad_mbps);
        red_log_linea(info->full_duplex ? " Mbps full-duplex" : " Mbps half-duplex");
    }

    if (!info->enlace_activo) {
        red_log_linea("  [AVISO] Conecta el cable Ethernet y reintenta ('red dhcp').");
        return;
    }

    red_log_linea("  Solicitando dirección por DHCP...");
    int r = red_dhcp(9000);
    if (r != 0) {
        red_log("  [DHCP] Falló (código "); red_log_dec((uint64_t)(-r));
        red_log_linea("). Sin configuración IP.");
        return;
    }

    red_log_linea("  [DHCP] Concedido [OK]");
    red_log("    IP      : "); red_log_linea(red_ip_a_texto(g_cfg.ip, buf, sizeof(buf)));
    red_log("    Máscara : "); red_log_linea(red_ip_a_texto(g_cfg.mascara, buf, sizeof(buf)));
    red_log("    Puerta  : "); red_log_linea(red_ip_a_texto(g_cfg.puerta, buf, sizeof(buf)));
    red_log("    DNS     : "); red_log_linea(red_ip_a_texto(g_cfg.dns[0], buf, sizeof(buf)));

    red_log_linea("  Resolviendo la puerta de enlace por ARP...");
    uint8_t mac_puerta[6];
    if (red_arp_resolver(g_cfg.puerta, mac_puerta, 3000) == 0) {
        red_log_linea("  [ARP] Puerta de enlace localizada [OK]");
    } else {
        red_log_linea("  [ARP] Sin respuesta de la puerta de enlace.");
    }

    red_log("  Haciendo ping a la puerta (");
    red_log(red_ip_a_texto(g_cfg.puerta, buf, sizeof(buf)));
    red_log_linea(")...");
    int resp = red_ping(g_cfg.puerta, 3, 1200);
    red_log("  [PING] Respuestas: "); red_log_dec((uint64_t)(resp < 0 ? 0 : resp));
    red_log_linea("/3");

    red_log_linea("  Resolviendo 'example.com' por DNS...");
    uint8_t ip_dns[4];
    if (red_resolver_nombre("example.com", ip_dns, 4000) == 0) {
        red_log("  [DNS] example.com -> "); red_log_linea(red_ip_a_texto(ip_dns, buf, sizeof(buf)));
    } else {
        red_log_linea("  [DNS] Sin resolución (puede no haber DNS en la red).");
    }

    red_log_linea("  Petición HTTP GET a example.com...");
    static char respuesta[2048];
    int leidos = red_http_obtener("example.com", "/", 80, respuesta, sizeof(respuesta), 8000);
    if (leidos > 0) {
        red_log("  [HTTP] Recibidos "); red_log_dec((uint64_t)leidos);
        red_log_linea(" bytes. Primeras líneas:");
        int mostrar = leidos < 240 ? leidos : 240;
        for (int i = 0; i < mostrar; i++) {
            char c = respuesta[i];
            if (c == '\r') continue;
            char uno[2] = { (c == '\n') ? '\n' : (c >= 32 && c < 127 ? c : '.'), '\0' };
            red_log(uno);
        }
        red_log_linea("");
    } else {
        red_log("  [HTTP] Sin respuesta (código "); red_log_dec((uint64_t)(-leidos));
        red_log_linea(").");
    }

    red_log_linea("--------------------------------------------------------------");
    red_log("  RX: "); red_log_dec(info->rx_paquetes);
    red_log(" tramas / "); red_log_dec(info->rx_bytes); red_log(" bytes");
    red_log("  |  TX: "); red_log_dec(info->tx_paquetes);
    red_log(" tramas / "); red_log_dec(info->tx_bytes); red_log_linea(" bytes");
    red_log_linea("==============================================================");
    red_log_linea("");
}

/* ------------------------------------------------------------------ H211, Fase C: hilo de red en CPU 3
 * Bombea la pila (red_pila_sondeo: tramas, ARP, DHCP, TCP y sus temporizadores) y atiende el stream de la consola
 * (red_stream_atender: envio al PC, teclas del PC, reconexion WiFi). Si hubo tramas, vuelve enseguida; si no, duerme
 * 1 ms. Va en CPU 3 y no en la 2 del plan: alli corre el hilo de audio de tiempo real, y el de red puede tener el
 * cerrojo mucho tiempo (descifrar WiFi, reconectar), cosa que en la misma CPU le quitaria el turno al audio. */
cerrojo_t g_cerrojo_red = CERROJO("red");
static evento_t g_ev_red = EVENTO("red_hilo");
static hilo_t *g_hilo_red;
static volatile int g_red_modo;
static uint64_t g_red_pasadas;

int red_hilo_activo(void) { return __atomic_load_n(&g_red_modo, __ATOMIC_ACQUIRE) && g_hilo_red; }
int red_cede_el_sondeo(void) { return red_hilo_activo() && !cerrojo_es_mio(&g_cerrojo_red); }

static void hilo_red(void *u) {
    (void)u;
    for (;;) {
        if (!__atomic_load_n(&g_red_modo, __ATOMIC_ACQUIRE)) { hilo_esperar(&g_ev_red); continue; }
        const struct red_info *info = red_obtener_info();
        uint64_t rx_antes = info ? info->rx_paquetes : 0;
        int tomado = 0;
        uint64_t limite = rdtsc() + tiempo_ciclos_por_ms() / 20u;
        while (!(tomado = cerrojo_intentar(&g_cerrojo_red)) && rdtsc() < limite) __asm__ volatile("pause");
        if (tomado) {
            red_pila_sondeo();
            red_stream_atender();
            g_red_pasadas++;
            cerrojo_soltar(&g_cerrojo_red);
        }
        if (tomado && info && info->rx_paquetes != rx_antes) hilo_ceder();   /* Hay trafico: otra vuelta ya */
        else hilo_dormir_ms(1);
    }
}

int red_hilo(int encender) {
    if (encender && !g_hilo_red) {
        g_hilo_red = hilo_crear(hilo_red, 0, "red", 3, HILO_NORMAL, 0);
        if (!g_hilo_red) return -1;
    }
    int antes = g_red_modo;
    __atomic_store_n(&g_red_modo, encender ? 1 : 0, __ATOMIC_RELEASE);
    if (encender && !antes) evento_senalar(&g_ev_red);
    return g_red_modo;
}
uint64_t red_hilo_pasadas(void) { return g_red_pasadas; }
void red_cerrojo_forzar(void) { cerrojo_forzar(&g_cerrojo_red, 200); }