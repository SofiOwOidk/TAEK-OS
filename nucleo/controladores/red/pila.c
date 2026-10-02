#include "red.h"
#include "../consola.h"
#include "../../arquitectura/x86_64/serial.h"
#include "../../base/memoria.h"
#include "../../base/tiempo.h"

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
};
static struct arp_entrada g_arp[8];

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
};
static struct red_tcp_conexion g_tcp;
static unsigned char g_tcp_ocupada;

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

void red_pila_sondeo(void) {
    const struct red_info *info = red_obtener_info();
    if (!info->controlador_listo) return;

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
            if (red_be16(arp->tipo_proto) == RED_ETH_IPV4 && arp->largo_hw == 6 && arp->largo_proto == 4) {
                for (int i = 0; i < 8; i++) {
                    if (!g_arp[i].valida || red_ip_igual(g_arp[i].ip, arp->emisor_ip)) {
                        memcpy(g_arp[i].ip, arp->emisor_ip, 4);
                        memcpy(g_arp[i].mac, arp->emisor_mac, 6);
                        g_arp[i].valida = 1;
                        break;
                    }
                }
            }
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
int red_arp_resolver(const uint8_t ip[4], uint8_t mac_salida[6], int timeout_ms) {
    for (int i = 0; i < 8; i++) {
        if (g_arp[i].valida && red_ip_igual(g_arp[i].ip, ip)) {
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
    ip->checksum = 0;
    ip->checksum = red_checksum(ip, 20);

    uint8_t *icmp = g_trama + sizeof(struct red_eth) + ihl;
    memcpy(icmp, rx + sizeof(struct red_eth) + ihl, carga_len);
    icmp[0] = 0;   // Eco reply
    icmp[2] = 0;
    icmp[3] = 0;
    uint16_t ck = red_checksum(icmp, carga_len);
    icmp[2] = (uint8_t)(ck >> 8);
    icmp[3] = (uint8_t)(ck & 0xFF);

    red_enviar_trama(g_trama, sizeof(struct red_eth) + ihl + carga_len);
}

int red_ping(const uint8_t ip[4], int intentos, int timeout_ms) {
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

static void red_procesar_dhcp(const uint8_t *datos, uint32_t longitud) {
    if (longitud < 240) return;
    const struct red_dhcp *dh = (const struct red_dhcp *)datos;
    if (dh->op != 2) return;
    if (red_be32(dh->xid) != g_dhcp_xid) return;

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

    if (tipo != g_dhcp_esperado) return;

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

    red_enviar_trama(g_trama, sizeof(struct red_eth) + 20 + sizeof(struct red_udp) + dhcp_len);

    g_dhcp_esperado = esperado;
    g_dhcp_recibido = 0;
    uint64_t inicio = tiempo_obtener_milisegundos();
    while ((int)(tiempo_obtener_milisegundos() - inicio) < timeout_ms) {
        red_pila_sondeo();
        if (g_dhcp_recibido) return 0;
        esperar_milisegundos(1);
    }
    return -1;
}

int red_dhcp(int timeout_ms) {
    const struct red_info *info = red_obtener_info();
    if (!info->controlador_listo) return -1;
    if (!info->enlace_activo) red_enlace_actualizar();

    g_dhcp_xid = (uint32_t)rdtsc() ^ 0x5441454Bu;
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

int red_resolver_nombre(const char *nombre, uint8_t ip_salida[4], int timeout_ms) {
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
    tcp->ventana = red_be16(64240);
    tcp->urgente = 0;
    if (longitud) memcpy(g_payload + sizeof(struct red_tcp), datos, longitud);

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

static void red_procesar_tcp(struct red_ipv4 *ip, uint8_t *tcp_carga, uint32_t carga_len) {
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
        if (seq == g_tcp.ack) {
            uint32_t espacio = g_tcp.rx_capacidad - g_tcp.rx_longitud;
            uint32_t copiar = datos_len < espacio ? datos_len : espacio;
            if (copiar < datos_len) g_tcp.rx_desbordado = 1;
            if (copiar && g_tcp.rx_destino) {
                memcpy(g_tcp.rx_destino + g_tcp.rx_longitud, datos, copiar);
                g_tcp.rx_longitud += copiar;
            }
            g_tcp.ack += datos_len;
        }
        red_tcp_enviar_segmento(NULL, 0, RED_TCP_ACK);
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
    memset(&g_tcp, 0, sizeof(g_tcp));
    memcpy(g_tcp.remota_ip, ip, 4);
    g_tcp.puerto_remoto = puerto;
    g_tcp.puerto_local = (uint16_t)(40000 + (rdtsc() % 20000));
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

static void red_tcp_temporizador(void) {
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
    uint8_t ip[4];
    if (red_resolver_nombre(host, ip, timeout_ms) != 0) return -1;
    if (red_tcp_conectar(ip, puerto, timeout_ms) != 0) return -2;

    char peticion[512];
    int n = 0;
    const char *metodo = "GET ";
    while (*metodo) peticion[n++] = *metodo++;
    if (!ruta || !ruta[0]) { peticion[n++] = '/'; }
    else { for (const char *p = ruta; *p && n < 400; p++) peticion[n++] = *p; }
    peticion[n++] = ' ';
    peticion[n++] = 'H'; peticion[n++] = 'T'; peticion[n++] = 'T';
    peticion[n++] = 'P'; peticion[n++] = '/'; peticion[n++] = '1';
    peticion[n++] = '.'; peticion[n++] = '1'; peticion[n++] = '\r';
    peticion[n++] = '\n';
    const char *host_hdr = "Host: ";
    while (*host_hdr) peticion[n++] = *host_hdr++;
    for (const char *p = host; *p && n < 480; p++) peticion[n++] = *p;
    peticion[n++] = '\r'; peticion[n++] = '\n';
    const char *ua = "User-Agent: TAEK-OS/1.0\r\nConnection: close\r\n\r\n";
    while (*ua && n < 510) peticion[n++] = *ua++;

    // Preparar el búfer de recepción ANTES de enviar, por si la respuesta
    // llega en la misma ventana que el ACK de la petición.
    uint32_t tope = capacidad > 0 ? capacidad - 1 : 0;
    g_tcp.rx_destino = salida;
    g_tcp.rx_capacidad = tope;
    g_tcp.rx_longitud = 0;
    g_tcp.rx_completo = 0;

    if (red_tcp_enviar(peticion, (uint32_t)n, timeout_ms) != 0) {
        red_tcp_cerrar();
        return -3;
    }

    red_tcp_esperar(red_tcp_cond_cerrado, timeout_ms);
    red_tcp_cerrar();

    if (salida && capacidad > 0) salida[g_tcp.rx_longitud < capacidad ? g_tcp.rx_longitud : capacidad - 1] = '\0';
    return (int)g_tcp.rx_longitud;
}

int red_http_obtener(const char *host, const char *ruta, uint16_t puerto,
                     char *salida, uint32_t capacidad, int timeout_ms) {
    if (__atomic_test_and_set(&g_tcp_ocupada, __ATOMIC_ACQUIRE)) return -16;
    int resultado = red_http_obtener_interno(host, ruta, puerto, salida, capacidad, timeout_ms);
    __atomic_clear(&g_tcp_ocupada, __ATOMIC_RELEASE);
    return resultado;
}

static int red_tcp_tiempo_restante(uint64_t inicio, int timeout_ms) {
    uint64_t transcurrido = tiempo_obtener_milisegundos() - inicio;
    return transcurrido >= (uint64_t)timeout_ms ? 0 : timeout_ms - (int)transcurrido;
}

static int red_tcp_cond_respuesta(void) {
    return g_tcp.rx_longitud == g_tcp.rx_capacidad || g_tcp.rx_desbordado ||
           !g_tcp.activa || g_tcp.estado == 4;
}

int red_tcp_intercambiar(const uint8_t ip[4], uint16_t puerto,
                        const void *cabecera, uint32_t cabecera_len,
                        const void *datos, uint32_t longitud,
                        void *respuesta, uint32_t respuesta_len, int timeout_ms) {
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

void red_configurar_vacia(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    memset(g_arp, 0, sizeof(g_arp));
}

int red_ip_estatica(const uint8_t ip[4], const uint8_t mascara[4],
                    const uint8_t puerta[4], const uint8_t dns[4]) {
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
void red_autodiagnostico(void) {
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
