#ifndef CONTROLADORES_RED_H
#define CONTROLADORES_RED_H

#include <stdint.h>
#include <stddef.h>

/*
 * Subsistema de red cableada (Ethernet) de TAEK OS.
 *
 * Implementa un controlador para la familia Intel e1000/e1000e, que cubre
 * tanto los chips discretos 82540EM / 82574L (emulados por QEMU) como los
 * controladores integrados en el chipset I217/I218/I219 (equipos portátiles
 * Intel modernos, p. ej. el I219-LM de un Dell Latitude 5490).
 *
 * Sobre el controlador se apoya una pila IPv4 mínima pero funcional:
 * Ethernet + ARP + IPv4 + ICMP + UDP + DHCP + DNS + TCP + HTTP. Permite
 * obtener dirección por DHCP, resolver nombres, hacer ping y descargar
 * páginas HTTP sencillas.
 */

// Base virtual donde se mapea el MMIO del controlador (evita colisiones con
// APIC 0x1000000, IOMMU 0x2000000, ACPI 0x3000000, xHCI 0x4000000 y HDA 0x5000000).
#define RED_MMIO_VIRTUAL_BASE 0xFFFFFE0006000000ULL

#define RED_MAX_TRAMA 1536
#define RED_MTU       1500
#define RED_MAX_MAC   6
#define RED_MAX_DNS   2

// Familias de silicio soportadas
enum {
    RED_FAMILIA_8254X = 0,   // e1000 clásico (82540EM, 82545EM, 82541/82547...)
    RED_FAMILIA_8257X = 1,   // e1000e discreto (82571/82572/82573/82574/82583)
    RED_FAMILIA_ICH   = 2    // e1000e integrado PCH (82579 / I217 / I218 / I219)
};

struct red_info {
    int      presente;
    int      controlador_listo;
    uint8_t  bus;
    uint8_t  ranura;
    uint8_t  funcion;
    uint16_t id_proveedor;
    uint16_t id_dispositivo;
    uint64_t mmio_fisica;
    uint64_t mmio_virtual;
    uint64_t mmio_tamano;
    uint8_t  mac[RED_MAX_MAC];
    uint8_t  familia;
    const char *modelo;
    int      enlace_activo;
    uint32_t velocidad_mbps;
    int      full_duplex;
    int      reinicio_omitido;   // 1 cuando se preservó el estado del firmware (ICH)
    uint64_t rx_paquetes;
    uint64_t tx_paquetes;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_descartados;
    uint64_t tx_errores;
    uint32_t ultimo_error;
};

struct red_config {
    int     configurada;
    int     por_dhcp;
    uint8_t ip[4];
    uint8_t mascara[4];
    uint8_t puerta[4];
    uint8_t dns[RED_MAX_DNS][4];
    uint8_t servidor_dhcp[4];
};

// ---------------- Controlador (capa de hardware) ----------------

// Detecta e inicializa el controlador. Devuelve 0 si quedó operativo.
int  red_iniciar(void);
const struct red_info *red_obtener_info(void);

// Relee el registro STATUS para actualizar enlace/velocidad. Devuelve 1 si hay enlace.
int  red_enlace_actualizar(void);

// Envía una trama Ethernet completa (incluida cabecera). 0 = aceptada por el TX.
int  red_enviar_trama(const void *datos, uint32_t longitud);

// Extrae una trama recibida. Devuelve 1 si se copió una, 0 si no hay ninguna.
int  red_recibir_trama(void *buffer, uint32_t capacidad, uint32_t *longitud);

// ---------------- Pila de red ----------------

// Procesa tramas pendientes: ARP, IPv4, ICMP, UDP, TCP y temporizadores.
void red_pila_sondeo(void);

const struct red_config *red_obtener_config(void);
void red_configurar_vacia(void);

// Configuración dinámica por DHCP (bloqueante). 0 = concedida.
int  red_dhcp(int timeout_ms);

// Configuración estática. dns admite NULL (sin DNS).
int  red_ip_estatica(const uint8_t ip[4], const uint8_t mascara[4],
                     const uint8_t puerta[4], const uint8_t dns[4]);

// Resuelve la MAC de una IPv4 (consulta/actualiza la caché ARP). 0 = ok.
int  red_arp_resolver(const uint8_t ip[4], uint8_t mac_salida[6], int timeout_ms);

// Resuelve un nombre por DNS. 0 = ok.
int  red_resolver_nombre(const char *nombre, uint8_t ip_salida[4], int timeout_ms);

// Ping ICMP: devuelve cantidad de respuestas recibidas (>=0) o negativo si falló.
int  red_ping(const uint8_t ip[4], int intentos, int timeout_ms);

// Petición HTTP GET sencilla (Connection: close). Devuelve bytes recibidos o negativo.
int  red_http_obtener(const char *host, const char *ruta, uint16_t puerto,
                      char *salida, uint32_t capacidad, int timeout_ms);

// Transferencia TCP sincronica por la LAN; uso desde la terminal, no desde IRQ.
// Timeout total, respuesta de longitud exacta y exclusion frente al cliente HTTP.
int red_tcp_intercambiar(const uint8_t ip[4], uint16_t puerto,
                        const void *cabecera, uint32_t cabecera_len,
                        const void *datos, uint32_t longitud,
                        void *respuesta, uint32_t respuesta_len, int timeout_ms);
// Snapshot de log + metadatos; solo retorna 0 si el receptor confirma persistencia.
int red_telemetria_enviar(const uint8_t ip[4], uint16_t puerto,
                         uint32_t *bytes_enviados, uint64_t *bytes_perdidos);

#define RED_MENSAJE_MAX 240
void red_mensajes_destino(const uint8_t ip[4], uint16_t puerto);
void red_mensajes_obtener_destino(uint8_t ip[4], uint16_t *puerto);
int red_mensaje_transmitir(const char *mensaje);
// 1 = mensaje confirmado, 0 = cola vacia, negativo = fallo.
int red_mensaje_recibir(char *salida, uint32_t capacidad);

// Utilidades de texto
const char *red_ip_a_texto(const uint8_t ip[4], char *buffer, size_t tamano);
int  red_texto_a_ip(const char *texto, uint8_t ip_salida[4]);

// Autodiagnóstico completo por serial/consola: enlace, DHCP, ping, DNS y HTTP.
void red_autodiagnostico(void);

#endif // CONTROLADORES_RED_H
