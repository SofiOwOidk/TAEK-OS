#ifndef CONTROLADORES_RED_CRIPTO_H
#define CONTROLADORES_RED_CRIPTO_H

#include <stdint.h>
#include <stddef.h>

// Primitivas criptográficas para TLS 1.3 y seguridad en red de TAEK-OS:
// - HMAC-SHA256 (RFC 4231)
// - HKDF-SHA256 Extract y Expand / Expand-Label (RFC 5869 / RFC 8446)
// - ChaCha20 y Poly1305 (RFC 8439)
// - AEAD ChaCha20-Poly1305 (RFC 8439 / RFC 7905)
// - X25519 Curve25519 intercambio de claves (RFC 7748)
// - Generador pseudoaleatorio de entropía por hardware (RDRAND / RDTSC)

void cripto_hmac_sha256(const void *clave, size_t clave_len,
                        const void *datos, size_t datos_len,
                        uint8_t salida[32]);

void cripto_hkdf_extract(const uint8_t *sal, size_t sal_len,
                         const uint8_t *ikm, size_t ikm_len,
                         uint8_t prk[32]);

int  cripto_hkdf_expand(const uint8_t prk[32],
                        const uint8_t *info, size_t info_len,
                        uint8_t *okm, size_t okm_len);

int  cripto_hkdf_expand_label(const uint8_t secreto[32],
                              const char *etiqueta,
                              const uint8_t *contexto, size_t contexto_len,
                              size_t longitud_salida,
                              uint8_t *salida);

void cripto_chacha20(const uint8_t clave[32], const uint8_t nonce[12], uint32_t contador,
                     const uint8_t *entrada, size_t longitud, uint8_t *salida);

int  cripto_aead_cifrar(const uint8_t clave[32], const uint8_t nonce[12],
                        const uint8_t *aad, size_t aad_len,
                        const uint8_t *texto_plano, size_t longitud,
                        uint8_t *texto_cifrado, uint8_t tag[16]);

int  cripto_aead_descifrar(const uint8_t clave[32], const uint8_t nonce[12],
                          const uint8_t *aad, size_t aad_len,
                          const uint8_t *texto_cifrado, size_t longitud,
                          const uint8_t tag[16], uint8_t *texto_plano);

int  cripto_x25519(uint8_t salida[32], const uint8_t clave_privada[32], const uint8_t clave_publica[32]);
void cripto_x25519_base(uint8_t salida[32], const uint8_t clave_privada[32]);

/* 0 en exito, <0 si falta entropia; el llamador debe comprobar el resultado. */
int cripto_azar_bytes(void *buf, size_t n);

#endif // CONTROLADORES_RED_CRIPTO_H
