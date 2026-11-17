#ifndef CONTROLADORES_RED_TLS_H
#define CONTROLADORES_RED_TLS_H

#include <stdint.h>
#include <stddef.h>

// Códigos de resultado y error para TLS 1.3
#define TLS_OK                 0
#define TLS_ERR_PARAM         -1
#define TLS_ERR_RED_NOCONFIG  -2
#define TLS_ERR_DNS           -3
#define TLS_ERR_TCP           -4
#define TLS_ERR_TX            -5
#define TLS_ERR_RX            -6
#define TLS_ERR_PROTOCOLO     -7
#define TLS_ERR_NO_X25519     -8
#define TLS_ERR_AUTH_FALLIDA  -9
#define TLS_ERR_TIMEOUT       -10
#define TLS_ERR_CANCELADO     -11
#define TLS_ERR_BUFER         -12
#define TLS_ERR_RELOJ         -13
#define TLS_ERR_ENTROPIA      -14
#define TLS_ERR_OCUPADO       -15

/*
 * Cliente HTTPS TLS 1.3 con Mbed TLS y autenticacion X.509 obligatoria:
 * CA, nombre/SAN (incluida IPv4), vigencia y CertificateVerify.
 * No entrega respuestas parciales ante fallo de autenticacion.
 *
 * host:       Dirección IPv4 en texto (ej. "1.1.1.1") o nombre DNS (ej. "example.com")
 * ruta:       Ruta HTTP solicitada (ej. "/" o "/index.html"). Si es NULL se asume "/".
 * puerto:     Puerto TCP (ej. 443; si es 0 se usa 443 por defecto).
 * salida:     Búfer de destino para la respuesta HTTP completa (cabeceras y cuerpo).
 * capacidad:  Tamaño en bytes del búfer de salida.
 * timeout_ms: Tiempo límite total en milisegundos.
 *
 * Devuelve la cantidad de bytes recibidos (>0) o un código de error TLS_ERR_* (<0).
 */
int red_https_obtener(const char *host, const char *ruta, uint16_t puerto,
                      char *salida, uint32_t capacidad, int timeout_ms);

/* H183: sesiones TLS 1.3 por manejador (hasta 4) sobre conexiones TCP 1-7; no pausan el stream de consola.
 * abrir devuelve el id (0-3) o un TLS_ERR_*; recibir no bloquea (0 = aun no hay datos). */
int  red_tls_abrir(const char *host, const uint8_t ip[4], uint16_t puerto, int timeout_ms);
int  red_tls_enviar(int id, const void *datos, uint32_t n, int timeout_ms);
int  red_tls_recibir(int id, void *datos, uint32_t capacidad);
void red_tls_cerrar(int id);
void red_tls_cerrar_todas(void);

#endif // CONTROLADORES_RED_TLS_H
