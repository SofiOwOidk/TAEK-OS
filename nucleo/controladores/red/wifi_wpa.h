#ifndef CONTROLADORES_RED_WIFI_WPA_H
#define CONTROLADORES_RED_WIFI_WPA_H

#include <stdint.h>
#include <stddef.h>

/* Criptografia del suplicante WPA2-PSK/WPA3-SAE propio de TAEK (IEEE 802.11-2020 12.7),
 * sobre el mbedTLS del nucleo (SHA-1/HMAC y AES). AKM 00-0F-AC:2 (PSK),
 * descriptor de clave version 2: MIC HMAC-SHA1-128, datos con AES Key Wrap. */

/* PMK = PBKDF2-HMAC-SHA1(frase, SSID, 4096, 32). 0 = OK. */
int wpa_pmk(const char *frase, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]);

/* PTK = PRF-384(PMK, "Pairwise key expansion", min(AA,SPA)|max(AA,SPA)|min(AN,SN)|max(AN,SN)).
 * KCK = ptk[0..15], KEK = ptk[16..31], TK = ptk[32..47]. */
int wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
            const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);

/* MIC = HMAC-SHA1(KCK, trama EAPOL con el MIC en cero)[0..15]. */
int wpa_mic(const uint8_t kck[16], const uint8_t *eapol, size_t n, uint8_t mic[16]);

/* AES Key Unwrap (RFC 3394) con KEK. n = largo cifrado (multiplo de 8, >= 16);
 * escribe n - 8 bytes. 0 = OK, -1 = IV de integridad incorrecto. */
int wpa_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out);

/* ------------------------------------------------------------------ H164: WPA3-Personal
 * AKM 00-0F-AC:8 (SAE), descriptor de clave version 0 (definido por el AKM):
 * PTK por KDF-SHA256, MIC AES-128-CMAC, datos con AES Key Wrap. PMF (802.11w):
 * tramas de gestion unicast protegidas con CCMP-128 (bit Management del nonce). */

/* KDF-Hash-Length de 802.11-2020 12.7.1.6.2 con SHA-256:
 * HMAC(K, i_le16 | etiqueta | contexto | bits_le16), i = 1, 2, ... */
int wpa_kdf_sha256(const uint8_t *k, size_t kl, const char *etiqueta,
                   const uint8_t *ctx, size_t cl, uint8_t *out, size_t bits);

/* PTK = KDF-SHA256-384(PMK, "Pairwise key expansion", min/max(AA,SPA) | min/max(AN,SN)). */
int wpa_ptk_sha256(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                   const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]);

/* MIC = AES-128-CMAC(KCK, trama EAPOL con el MIC en cero) (RFC 4493). */
int wpa_mic_cmac(const uint8_t kck[16], const uint8_t *eapol, size_t n, uint8_t mic[16]);

/* CCMP-128 de una trama de gestion (cabecera de 24 B + cuerpo en claro, n bytes).
 * Escribe cabecera con Protected | cabecera CCMP (8) | cuerpo cifrado | MIC (8).
 * Devuelve el largo escrito (n + 16) o 0 si falla. */
size_t wpa_ccmp_gestion(const uint8_t tk[16], uint64_t pn, uint8_t keyid,
                        const uint8_t *trama, size_t n, uint8_t *salida);

/* H165: CCMP-128 general (gestion y datos, con QoS o 4 direcciones). trama = cabecera MAC en
 * claro + cuerpo; salida = cabecera con Protected | CCMP (8) | cuerpo cifrado | MIC (8). */
size_t wpa_largo_cabecera(const uint8_t *trama);
size_t wpa_ccmp_cifrar(const uint8_t tk[16], uint64_t pn, uint8_t keyid,
                       const uint8_t *trama, size_t n, uint8_t *salida);
/* Verifica el MIC y descifra el cuerpo en 'claro' (n - cabecera - 16 bytes). 0 = OK,
 * -1 = formato, -2 = MIC incorrecto. Devuelve el PN para la proteccion contra repeticion. */
int wpa_ccmp_descifrar(const uint8_t tk[16], const uint8_t *trama, size_t n, uint8_t *claro,
                       size_t *largo_claro, uint64_t *pn);

/* SAE (Dragonfly, 802.11-2020 12.4) en el grupo 19 (P-256), PWE por hunting-and-pecking. */
struct wpa_sae {
    uint8_t pwe[64];                       /* x | y */
    uint8_t rand[32];
    uint8_t escalar[32], elemento[64];     /* nuestro commit */
    uint8_t escalar_par[32], elemento_par[64];
    uint8_t kck[32], pmk[32], pmkid[16];
    uint32_t contador_pwe;                 /* vuelta en que aparecio el PWE */
};
typedef int (*wpa_azar_fn)(void *buf, size_t n);   /* 0 = OK */

/* PWE con las MAC de ambos lados y la clave. 0 = OK. */
int sae_pwe(struct wpa_sae *s, const uint8_t mia[6], const uint8_t par[6], const char *clave, size_t cl);
/* rand y mask al azar (o fijos, para vectores de prueba): escalar y elemento del commit. */
int sae_commit(struct wpa_sae *s, const uint8_t *rand_fijo, const uint8_t *mask_fijo, wpa_azar_fn azar);
/* Commit del AP: valida, calcula K, KCK, PMK y PMKID. 0 = OK; <0 = rechazo. */
int sae_procesar(struct wpa_sae *s, const uint8_t escalar_par[32], const uint8_t elemento_par[64], wpa_azar_fn azar);
/* confirm = HMAC-SHA256(KCK, sc | escalar1 | elemento1 | escalar2 | elemento2);
 * propio = 1: el nuestro primero (para enviar); 0: el del AP primero (para verificar). */
void sae_confirm(const struct wpa_sae *s, uint16_t sc, int propio, uint8_t out[32]);

#endif
