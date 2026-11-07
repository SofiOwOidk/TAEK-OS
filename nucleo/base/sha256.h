#ifndef TAEK_SHA256_H
#define TAEK_SHA256_H
#include <stdint.h>
#include <stddef.h>
struct sha256 { uint32_t h[8]; uint64_t bytes; unsigned usados; uint8_t bloque[64]; };
void sha256_iniciar(struct sha256 *);
void sha256_actualizar(struct sha256 *,const void *,size_t);
void sha256_final(struct sha256 *,uint8_t[32]);
#endif
