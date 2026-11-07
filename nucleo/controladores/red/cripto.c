#include "cripto.h"
#include "../../base/sha256.h"
#include "../../base/memoria.h"
#include "tls_port/taek_tls_port.h"

typedef unsigned __int128 uint128_t;

// --- HMAC-SHA256 (RFC 4231) ---
void cripto_hmac_sha256(const void *clave, size_t clave_len,
                        const void *datos, size_t datos_len,
                        uint8_t salida[32]) {
    uint8_t k[64];
    memset(k, 0, 64);
    if (clave_len > 64) {
        struct sha256 s;
        sha256_iniciar(&s);
        sha256_actualizar(&s, clave, clave_len);
        sha256_final(&s, k);
    } else {
        if (clave_len && clave) memcpy(k, clave, clave_len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5C;
    }
    struct sha256 s;
    uint8_t inner[32];
    sha256_iniciar(&s);
    sha256_actualizar(&s, ipad, 64);
    if (datos_len && datos) sha256_actualizar(&s, datos, datos_len);
    sha256_final(&s, inner);

    sha256_iniciar(&s);
    sha256_actualizar(&s, opad, 64);
    sha256_actualizar(&s, inner, 32);
    sha256_final(&s, salida);
}

// --- HKDF-SHA256 (RFC 5869) ---
void cripto_hkdf_extract(const uint8_t *sal, size_t sal_len,
                         const uint8_t *ikm, size_t ikm_len,
                         uint8_t prk[32]) {
    static const uint8_t cero[32] = {0};
    if (!sal || sal_len == 0) {
        sal = cero;
        sal_len = 32;
    }
    cripto_hmac_sha256(sal, sal_len, ikm, ikm_len, prk);
}

int cripto_hkdf_expand(const uint8_t prk[32],
                       const uint8_t *info, size_t info_len,
                       uint8_t *okm, size_t okm_len) {
    uint8_t t[32];
    uint8_t counter = 1;
    size_t pos = 0;
    while (pos < okm_len) {
        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 32; i++) {
            ipad[i] = prk[i] ^ 0x36;
            opad[i] = prk[i] ^ 0x5C;
        }
        for (int i = 32; i < 64; i++) {
            ipad[i] = 0x36;
            opad[i] = 0x5C;
        }
        struct sha256 s;
        sha256_iniciar(&s);
        sha256_actualizar(&s, ipad, 64);
        if (counter > 1) sha256_actualizar(&s, t, 32);
        if (info && info_len) sha256_actualizar(&s, info, info_len);
        sha256_actualizar(&s, &counter, 1);
        uint8_t inner[32];
        sha256_final(&s, inner);

        sha256_iniciar(&s);
        sha256_actualizar(&s, opad, 64);
        sha256_actualizar(&s, inner, 32);
        sha256_final(&s, t);

        size_t copiar = (okm_len - pos > 32) ? 32 : (okm_len - pos);
        memcpy(okm + pos, t, copiar);
        pos += copiar;
        counter++;
    }
    return 0;
}

int cripto_hkdf_expand_label(const uint8_t secreto[32],
                             const char *etiqueta,
                             const uint8_t *contexto, size_t contexto_len,
                             size_t longitud_salida,
                             uint8_t *salida) {
    uint8_t info[256];
    size_t et_len = 0;
    while (etiqueta[et_len]) et_len++;
    size_t total_et_len = 6 + et_len;

    info[0] = (uint8_t)(longitud_salida >> 8);
    info[1] = (uint8_t)(longitud_salida & 0xFF);
    info[2] = (uint8_t)total_et_len;
    memcpy(info + 3, "tls13 ", 6);
    memcpy(info + 9, etiqueta, et_len);
    size_t p = 9 + et_len;
    info[p++] = (uint8_t)contexto_len;
    if (contexto && contexto_len) {
        memcpy(info + p, contexto, contexto_len);
        p += contexto_len;
    }
    return cripto_hkdf_expand(secreto, info, p, salida, longitud_salida);
}

// --- ChaCha20 (RFC 8439) ---
#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define CHACHA_QR(a, b, c, d) \
    a += b; d ^= a; d = ROTL32(d, 16); \
    c += d; b ^= c; b = ROTL32(b, 12); \
    a += b; d ^= a; d = ROTL32(d, 8);  \
    c += d; b ^= c; b = ROTL32(b, 7);

static void chacha20_bloque(const uint32_t clave[8], const uint32_t nonce[3], uint32_t contador, uint8_t salida[64]) {
    uint32_t x[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
        clave[0], clave[1], clave[2], clave[3],
        clave[4], clave[5], clave[6], clave[7],
        contador, nonce[0], nonce[1], nonce[2]
    };
    uint32_t orig[16];
    memcpy(orig, x, sizeof(x));
    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8],  x[12]);
        CHACHA_QR(x[1], x[5], x[9],  x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);

        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8],  x[13]);
        CHACHA_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) {
        uint32_t v = x[i] + orig[i];
        salida[i * 4 + 0] = (uint8_t)(v & 0xFF);
        salida[i * 4 + 1] = (uint8_t)((v >> 8) & 0xFF);
        salida[i * 4 + 2] = (uint8_t)((v >> 16) & 0xFF);
        salida[i * 4 + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
}

void cripto_chacha20(const uint8_t clave[32], const uint8_t nonce[12], uint32_t contador,
                     const uint8_t *entrada, size_t longitud, uint8_t *salida) {
    uint32_t k[8], n[3];
    for (int i = 0; i < 8; i++) {
        k[i] = (uint32_t)clave[i*4] | ((uint32_t)clave[i*4+1] << 8) |
               ((uint32_t)clave[i*4+2] << 16) | ((uint32_t)clave[i*4+3] << 24);
    }
    for (int i = 0; i < 3; i++) {
        n[i] = (uint32_t)nonce[i*4] | ((uint32_t)nonce[i*4+1] << 8) |
               ((uint32_t)nonce[i*4+2] << 16) | ((uint32_t)nonce[i*4+3] << 24);
    }
    uint8_t bloque[64];
    while (longitud > 0) {
        chacha20_bloque(k, n, contador++, bloque);
        size_t take = longitud < 64 ? longitud : 64;
        for (size_t i = 0; i < take; i++) salida[i] = entrada[i] ^ bloque[i];
        entrada += take;
        salida += take;
        longitud -= take;
    }
}

// --- Poly1305 (RFC 8439 - 26-bit limbs robustos) ---
struct poly1305 {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
    size_t leftover;
    uint8_t buffer[16];
};

static inline uint32_t poly_get32_le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void poly1305_bloque(struct poly1305 *ctx, const uint8_t *m, size_t bytes, uint32_t hibit) {
    uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    while (bytes >= 16) {
        uint32_t t0 = poly_get32_le(m + 0);
        uint32_t t1 = poly_get32_le(m + 4);
        uint32_t t2 = poly_get32_le(m + 8);
        uint32_t t3 = poly_get32_le(m + 12);

        h0 += (t0)                      & 0x3ffffff;
        h1 += ((t0 >> 26) | (t1 << 6))  & 0x3ffffff;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        h4 += (t3 >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff; h0 += c * 5;
        c = (h0 >> 26); h0 &= 0x3ffffff; h1 += c;

        m += 16;
        bytes -= 16;
    }

    ctx->h[0] = h0; ctx->h[1] = h1; ctx->h[2] = h2; ctx->h[3] = h3; ctx->h[4] = h4;
}

static void poly1305_iniciar(struct poly1305 *ctx, const uint8_t key[32]) {
    ctx->leftover = 0;
    ctx->h[0] = 0; ctx->h[1] = 0; ctx->h[2] = 0; ctx->h[3] = 0; ctx->h[4] = 0;

    uint32_t t0 = poly_get32_le(key + 0);
    uint32_t t1 = poly_get32_le(key + 4);
    uint32_t t2 = poly_get32_le(key + 8);
    uint32_t t3 = poly_get32_le(key + 12);

    ctx->r[0] = (t0)                        & 0x3ffffff;
    ctx->r[1] = ((t0 >> 26) | (t1 << 6))    & 0x3ffff03;
    ctx->r[2] = ((t1 >> 20) | (t2 << 12))   & 0x3ffc0ff;
    ctx->r[3] = ((t2 >> 14) | (t3 << 18))   & 0x3f03fff;
    ctx->r[4] = (t3 >> 8)                   & 0x00fffff;

    ctx->pad[0] = poly_get32_le(key + 16);
    ctx->pad[1] = poly_get32_le(key + 20);
    ctx->pad[2] = poly_get32_le(key + 24);
    ctx->pad[3] = poly_get32_le(key + 28);
}

static void poly1305_actualizar(struct poly1305 *ctx, const uint8_t *m, size_t bytes) {
    if (ctx->leftover) {
        size_t want = 16 - ctx->leftover;
        if (want > bytes) want = bytes;
        memcpy(ctx->buffer + ctx->leftover, m, want);
        bytes -= want; m += want; ctx->leftover += want;
        if (ctx->leftover < 16) return;
        poly1305_bloque(ctx, ctx->buffer, 16, 1 << 24);
        ctx->leftover = 0;
    }
    if (bytes >= 16) {
        size_t take = bytes & ~15ULL;
        poly1305_bloque(ctx, m, take, 1 << 24);
        m += take; bytes -= take;
    }
    if (bytes) {
        memcpy(ctx->buffer, m, bytes);
        ctx->leftover = bytes;
    }
}

static void poly1305_final(struct poly1305 *ctx, uint8_t mac[16]) {
    if (ctx->leftover) {
        ctx->buffer[ctx->leftover] = 1;
        for (size_t i = ctx->leftover + 1; i < 16; i++) ctx->buffer[i] = 0;
        poly1305_bloque(ctx, ctx->buffer, 16, 0);
    }

    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];
    uint32_t c;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1 << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    uint64_t f0 = ((uint64_t)h0)       | ((uint64_t)h1 << 26);
    uint64_t f1 = ((uint64_t)h1 >> 6)  | ((uint64_t)h2 << 20);
    uint64_t f2 = ((uint64_t)h2 >> 12) | ((uint64_t)h3 << 14);
    uint64_t f3 = ((uint64_t)h3 >> 18) | ((uint64_t)h4 << 8);

    uint64_t w0 = (f0 & 0xFFFFFFFFULL) | ((f1 & 0xFFFFFFFFULL) << 32);
    uint64_t w1 = (f2 & 0xFFFFFFFFULL) | ((f3 & 0xFFFFFFFFULL) << 32);

    uint64_t pad0 = (uint64_t)ctx->pad[0] | ((uint64_t)ctx->pad[1] << 32);
    uint64_t pad1 = (uint64_t)ctx->pad[2] | ((uint64_t)ctx->pad[3] << 32);

    uint128_t f = ((uint128_t)w1 << 64) | w0;
    uint128_t pad = ((uint128_t)pad1 << 64) | pad0;
    f += pad;

    uint64_t out0 = (uint64_t)f;
    uint64_t out1 = (uint64_t)(f >> 64);
    for (int i = 0; i < 8; i++) mac[i] = (uint8_t)(out0 >> (i * 8));
    for (int i = 0; i < 8; i++) mac[8 + i] = (uint8_t)(out1 >> (i * 8));
}

// --- AEAD ChaCha20-Poly1305 (RFC 8439) ---
int cripto_aead_cifrar(const uint8_t clave[32], const uint8_t nonce[12],
                       const uint8_t *aad, size_t aad_len,
                       const uint8_t *texto_plano, size_t longitud,
                       uint8_t *texto_cifrado, uint8_t tag[16]) {
    uint8_t poly_key[64];
    uint32_t k[8], n[3];
    for (int i = 0; i < 8; i++) {
        k[i] = (uint32_t)clave[i*4] | ((uint32_t)clave[i*4+1] << 8) |
               ((uint32_t)clave[i*4+2] << 16) | ((uint32_t)clave[i*4+3] << 24);
    }
    for (int i = 0; i < 3; i++) {
        n[i] = (uint32_t)nonce[i*4] | ((uint32_t)nonce[i*4+1] << 8) |
               ((uint32_t)nonce[i*4+2] << 16) | ((uint32_t)nonce[i*4+3] << 24);
    }
    chacha20_bloque(k, n, 0, poly_key);
    cripto_chacha20(clave, nonce, 1, texto_plano, longitud, texto_cifrado);

    struct poly1305 p;
    poly1305_iniciar(&p, poly_key);
    if (aad && aad_len) {
        poly1305_actualizar(&p, aad, aad_len);
        if (aad_len % 16) {
            static const uint8_t pad[16] = {0};
            poly1305_actualizar(&p, pad, 16 - (aad_len % 16));
        }
    }
    if (texto_cifrado && longitud) {
        poly1305_actualizar(&p, texto_cifrado, longitud);
        if (longitud % 16) {
            static const uint8_t pad[16] = {0};
            poly1305_actualizar(&p, pad, 16 - (longitud % 16));
        }
    }
    uint8_t lens[16];
    uint64_t al = aad_len, cl = longitud;
    for (int i = 0; i < 8; i++) lens[i] = (uint8_t)(al >> (i * 8));
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)(cl >> (i * 8));
    poly1305_actualizar(&p, lens, 16);
    poly1305_final(&p, tag);
    return 0;
}

int cripto_aead_descifrar(const uint8_t clave[32], const uint8_t nonce[12],
                          const uint8_t *aad, size_t aad_len,
                          const uint8_t *texto_cifrado, size_t longitud,
                          const uint8_t tag[16], uint8_t *texto_plano) {
    uint8_t poly_key[64];
    uint32_t k[8], n[3];
    for (int i = 0; i < 8; i++) {
        k[i] = (uint32_t)clave[i*4] | ((uint32_t)clave[i*4+1] << 8) |
               ((uint32_t)clave[i*4+2] << 16) | ((uint32_t)clave[i*4+3] << 24);
    }
    for (int i = 0; i < 3; i++) {
        n[i] = (uint32_t)nonce[i*4] | ((uint32_t)nonce[i*4+1] << 8) |
               ((uint32_t)nonce[i*4+2] << 16) | ((uint32_t)nonce[i*4+3] << 24);
    }
    chacha20_bloque(k, n, 0, poly_key);

    struct poly1305 p;
    poly1305_iniciar(&p, poly_key);
    if (aad && aad_len) {
        poly1305_actualizar(&p, aad, aad_len);
        if (aad_len % 16) {
            static const uint8_t pad[16] = {0};
            poly1305_actualizar(&p, pad, 16 - (aad_len % 16));
        }
    }
    if (texto_cifrado && longitud) {
        poly1305_actualizar(&p, texto_cifrado, longitud);
        if (longitud % 16) {
            static const uint8_t pad[16] = {0};
            poly1305_actualizar(&p, pad, 16 - (longitud % 16));
        }
    }
    uint8_t lens[16];
    uint64_t al = aad_len, cl = longitud;
    for (int i = 0; i < 8; i++) lens[i] = (uint8_t)(al >> (i * 8));
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)(cl >> (i * 8));
    poly1305_actualizar(&p, lens, 16);
    uint8_t calc_tag[16];
    poly1305_final(&p, calc_tag);

    unsigned diff = 0;
    for (int i = 0; i < 16; i++) diff |= (calc_tag[i] ^ tag[i]);
    if (diff != 0) return -1;

    cripto_chacha20(clave, nonce, 1, texto_cifrado, longitud, texto_plano);
    return 0;
}

// --- X25519 (RFC 7748) ---
typedef uint64_t fe51[5];

static void fe51_frombytes(fe51 h, const uint8_t s[32]) {
    uint64_t r0 = (uint64_t)s[0] | ((uint64_t)s[1] << 8) | ((uint64_t)s[2] << 16) | ((uint64_t)s[3] << 24) |
                  ((uint64_t)s[4] << 32) | ((uint64_t)s[5] << 40) | ((uint64_t)s[6] << 48) | ((uint64_t)s[7] << 56);
    uint64_t r1 = (uint64_t)s[8] | ((uint64_t)s[9] << 8) | ((uint64_t)s[10] << 16) | ((uint64_t)s[11] << 24) |
                  ((uint64_t)s[12] << 32) | ((uint64_t)s[13] << 40) | ((uint64_t)s[14] << 48) | ((uint64_t)s[15] << 56);
    uint64_t r2 = (uint64_t)s[16] | ((uint64_t)s[17] << 8) | ((uint64_t)s[18] << 16) | ((uint64_t)s[19] << 24) |
                  ((uint64_t)s[20] << 32) | ((uint64_t)s[21] << 40) | ((uint64_t)s[22] << 48) | ((uint64_t)s[23] << 56);
    uint64_t r3 = (uint64_t)s[24] | ((uint64_t)s[25] << 8) | ((uint64_t)s[26] << 16) | ((uint64_t)s[27] << 24) |
                  ((uint64_t)s[28] << 32) | ((uint64_t)s[29] << 40) | ((uint64_t)s[30] << 48) | ((uint64_t)s[31] << 56);

    uint64_t mask = 0x7FFFFFFFFFFFFULL;
    h[0] = r0 & mask;
    h[1] = ((r0 >> 51) | (r1 << 13)) & mask;
    h[2] = ((r1 >> 38) | (r2 << 26)) & mask;
    h[3] = ((r2 >> 25) | (r3 << 39)) & mask;
    h[4] = (r3 >> 12) & 0x7FFFFFFFFFFFFULL;
}

static void fe51_tobytes(uint8_t s[32], const fe51 h_in) {
    fe51 h;
    memcpy(h, h_in, sizeof(fe51));
    uint64_t q = (19 * h[4] + (1ULL << 50)) >> 51;
    q = (h[0] + 19 * q) >> 51;
    q = (h[1] + q) >> 51;
    q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51;
    q = (h[4] + q) >> 51;

    h[0] += 19 * q;
    uint64_t c;
    c = h[0] >> 51; h[0] &= 0x7FFFFFFFFFFFFULL; h[1] += c;
    c = h[1] >> 51; h[1] &= 0x7FFFFFFFFFFFFULL; h[2] += c;
    c = h[2] >> 51; h[2] &= 0x7FFFFFFFFFFFFULL; h[3] += c;
    c = h[3] >> 51; h[3] &= 0x7FFFFFFFFFFFFULL; h[4] += c;
    c = h[4] >> 51; h[4] &= 0x7FFFFFFFFFFFFULL; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= 0x7FFFFFFFFFFFFULL; h[1] += c;

    uint64_t u0 = h[0] | (h[1] << 51);
    uint64_t u1 = (h[1] >> 13) | (h[2] << 38);
    uint64_t u2 = (h[2] >> 26) | (h[3] << 25);
    uint64_t u3 = (h[3] >> 39) | (h[4] << 12);

    for (int i = 0; i < 8; i++) s[i]      = (uint8_t)(u0 >> (i * 8));
    for (int i = 0; i < 8; i++) s[8 + i]  = (uint8_t)(u1 >> (i * 8));
    for (int i = 0; i < 8; i++) s[16 + i] = (uint8_t)(u2 >> (i * 8));
    for (int i = 0; i < 8; i++) s[24 + i] = (uint8_t)(u3 >> (i * 8));
}

static void fe51_add(fe51 h, const fe51 f, const fe51 g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
}

static void fe51_sub(fe51 h, const fe51 f, const fe51 g) {
    h[0] = f[0] + 0x7FFFFFFFFFFEDULL * 4 - g[0];
    h[1] = f[1] + 0x7FFFFFFFFFFFFULL * 4 - g[1];
    h[2] = f[2] + 0x7FFFFFFFFFFFFULL * 4 - g[2];
    h[3] = f[3] + 0x7FFFFFFFFFFFFULL * 4 - g[3];
    h[4] = f[4] + 0x7FFFFFFFFFFFFULL * 4 - g[4];
}

static void fe51_mul(fe51 h, const fe51 f, const fe51 g) {
    uint128_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    uint128_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    uint128_t g1_19 = g1 * 19, g2_19 = g2 * 19, g3_19 = g3 * 19, g4_19 = g4 * 19;

    uint128_t h0 = f0 * g0 + f4 * g1_19 + f3 * g2_19 + f2 * g3_19 + f1 * g4_19;
    uint128_t h1 = f0 * g1 + f1 * g0    + f4 * g2_19 + f3 * g3_19 + f2 * g4_19;
    uint128_t h2 = f0 * g2 + f1 * g1    + f2 * g0    + f4 * g3_19 + f3 * g4_19;
    uint128_t h3 = f0 * g3 + f1 * g2    + f2 * g1    + f3 * g0    + f4 * g4_19;
    uint128_t h4 = f0 * g4 + f1 * g3    + f2 * g2    + f3 * g1    + f4 * g0;

    uint64_t mask = 0x7FFFFFFFFFFFFULL;
    uint64_t c;
    c = (uint64_t)(h0 >> 51); h[0] = (uint64_t)h0 & mask; h1 += c;
    c = (uint64_t)(h1 >> 51); h[1] = (uint64_t)h1 & mask; h2 += c;
    c = (uint64_t)(h2 >> 51); h[2] = (uint64_t)h2 & mask; h3 += c;
    c = (uint64_t)(h3 >> 51); h[3] = (uint64_t)h3 & mask; h4 += c;
    c = (uint64_t)(h4 >> 51); h[4] = (uint64_t)h4 & mask; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= mask; h[1] += c;
}

static void fe51_sqr(fe51 h, const fe51 f) {
    fe51_mul(h, f, f);
}

static void fe51_mul121665(fe51 h, const fe51 f) {
    uint128_t h0 = (uint128_t)f[0] * 121665;
    uint128_t h1 = (uint128_t)f[1] * 121665;
    uint128_t h2 = (uint128_t)f[2] * 121665;
    uint128_t h3 = (uint128_t)f[3] * 121665;
    uint128_t h4 = (uint128_t)f[4] * 121665;

    uint64_t mask = 0x7FFFFFFFFFFFFULL;
    uint64_t c;
    c = (uint64_t)(h0 >> 51); h[0] = (uint64_t)h0 & mask; h1 += c;
    c = (uint64_t)(h1 >> 51); h[1] = (uint64_t)h1 & mask; h2 += c;
    c = (uint64_t)(h2 >> 51); h[2] = (uint64_t)h2 & mask; h3 += c;
    c = (uint64_t)(h3 >> 51); h[3] = (uint64_t)h3 & mask; h4 += c;
    c = (uint64_t)(h4 >> 51); h[4] = (uint64_t)h4 & mask; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= mask; h[1] += c;
}

static void fe51_inv(fe51 out, const fe51 z) {
    fe51 t0, t1, t2, t3;
    fe51_sqr(t0, z);
    fe51_sqr(t1, t0); fe51_sqr(t1, t1);
    fe51_mul(t1, z, t1);
    fe51_mul(t0, t0, t1);
    fe51_sqr(t2, t0);
    fe51_mul(t1, t1, t2);
    fe51_sqr(t2, t1); for (int i = 1; i < 5; i++) fe51_sqr(t2, t2);
    fe51_mul(t1, t2, t1);
    fe51_sqr(t2, t1); for (int i = 1; i < 10; i++) fe51_sqr(t2, t2);
    fe51_mul(t2, t2, t1);
    fe51_sqr(t3, t2); for (int i = 1; i < 20; i++) fe51_sqr(t3, t3);
    fe51_mul(t2, t3, t2);
    fe51_sqr(t2, t2); for (int i = 1; i < 10; i++) fe51_sqr(t2, t2);
    fe51_mul(t1, t2, t1);
    fe51_sqr(t2, t1); for (int i = 1; i < 50; i++) fe51_sqr(t2, t2);
    fe51_mul(t2, t2, t1);
    fe51_sqr(t3, t2); for (int i = 1; i < 100; i++) fe51_sqr(t3, t3);
    fe51_mul(t2, t3, t2);
    fe51_sqr(t2, t2); for (int i = 1; i < 50; i++) fe51_sqr(t2, t2);
    fe51_mul(t1, t2, t1);
    fe51_sqr(t1, t1); fe51_sqr(t1, t1); fe51_sqr(t1, t1); fe51_sqr(t1, t1); fe51_sqr(t1, t1);
    fe51_mul(out, t1, t0);
}

static void fe51_cswap(uint64_t swap, fe51 a, fe51 b) {
    uint64_t mask = 0 - swap;
    for (int i = 0; i < 5; i++) {
        uint64_t t = mask & (a[i] ^ b[i]);
        a[i] ^= t;
        b[i] ^= t;
    }
}

int cripto_x25519(uint8_t salida[32], const uint8_t clave_privada[32], const uint8_t clave_publica[32]) {
    uint8_t k[32];
    memcpy(k, clave_privada, 32);
    k[0] &= 248;
    k[31] &= 127;
    k[31] |= 64;

    fe51 x1, x2, z2, x3, z3, a, b, c, d, da, cb, aa, bb, e;
    fe51_frombytes(x1, clave_publica);

    memset(x2, 0, sizeof(x2)); x2[0] = 1;
    memset(z2, 0, sizeof(z2));
    memcpy(x3, x1, sizeof(x1));
    memset(z3, 0, sizeof(z3)); z3[0] = 1;

    uint64_t swap = 0;
    for (int i = 254; i >= 0; i--) {
        uint64_t bit = (k[i / 8] >> (i & 7)) & 1;
        swap ^= bit;
        fe51_cswap(swap, x2, x3);
        fe51_cswap(swap, z2, z3);
        swap = bit;

        fe51_add(a, x2, z2);
        fe51_sqr(aa, a);
        fe51_sub(b, x2, z2);
        fe51_sqr(bb, b);
        fe51_sub(e, aa, bb);

        fe51_add(c, x3, z3);
        fe51_sub(d, x3, z3);
        fe51_mul(da, d, a);
        fe51_mul(cb, c, b);

        fe51_add(x3, da, cb);
        fe51_sqr(x3, x3);
        fe51_sub(z3, da, cb);
        fe51_sqr(z3, z3);
        fe51_mul(z3, z3, x1);

        fe51_mul(x2, aa, bb);
        fe51_mul121665(z2, e);
        fe51_add(z2, z2, aa);
        fe51_mul(z2, z2, e);
    }
    fe51_cswap(swap, x2, x3);
    fe51_cswap(swap, z2, z3);

    fe51 z2_inv;
    fe51_inv(z2_inv, z2);
    fe51 res;
    fe51_mul(res, x2, z2_inv);
    fe51_tobytes(salida, res);
    return 0;
}

void cripto_x25519_base(uint8_t salida[32], const uint8_t clave_privada[32]) {
    static const uint8_t base9[32] = {9};
    cripto_x25519(salida, clave_privada, base9);
}

/* Se propaga el fallo: nunca sustituir entropia por un contador de ciclos. */
int cripto_azar_bytes(void *buf,size_t n) {
    return taek_tls_random(0,buf,n);
}
