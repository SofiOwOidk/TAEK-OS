/* Criptografia del suplicante WPA2-PSK de TAEK (IEEE 802.11-2020 12.7.1.2 PRF,
 * J.4 PBKDF2 para la PSK, RFC 3394 AES Key Wrap). Usa el mbedTLS del nucleo. */
#include "wifi_wpa.h"
#include "mbedtls/md.h"
#include "mbedtls/aes.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "red_cerrojo.h"   /* H211: cerrojo de la red (Fase C) */

static int hmac_sha1(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t out[20]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md) return -1;
    return mbedtls_md_hmac(md, k, kl, d, dl, out);
}

int wpa_pmk(const char *frase, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32]) { RED_EXCLUSIVA();
    size_t fl = 0;
    while (frase[fl]) fl++;
    if (fl < 8 || fl > 63 || ssid_len > 32) return -1;
    uint8_t sal[36], u[20], t[20];
    for (uint32_t bloque = 1; bloque <= 2; bloque++) {          /* 32 B = 2 bloques de SHA-1 */
        for (size_t i = 0; i < ssid_len; i++) sal[i] = ssid[i];
        sal[ssid_len] = (uint8_t)(bloque >> 24); sal[ssid_len + 1] = (uint8_t)(bloque >> 16);
        sal[ssid_len + 2] = (uint8_t)(bloque >> 8); sal[ssid_len + 3] = (uint8_t)bloque;
        if (hmac_sha1((const uint8_t *)frase, fl, sal, ssid_len + 4, u)) return -1;
        for (int i = 0; i < 20; i++) t[i] = u[i];
        for (int it = 1; it < 4096; it++) {
            if (hmac_sha1((const uint8_t *)frase, fl, u, 20, u)) return -1;
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        size_t base = (bloque - 1) * 20, cuantos = bloque == 1 ? 20 : 12;
        for (size_t i = 0; i < cuantos; i++) pmk[base + i] = t[i];
    }
    mbedtls_platform_zeroize(u, sizeof(u));
    mbedtls_platform_zeroize(t, sizeof(t));
    return 0;
}

static int menor6(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return a[i] < b[i];
    return 0;
}
static int menor32(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 32; i++) if (a[i] != b[i]) return a[i] < b[i];
    return 0;
}

int wpa_ptk(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
            const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]) { RED_EXCLUSIVA();
    static const char etiqueta[] = "Pairwise key expansion";
    uint8_t d[22 + 1 + 6 + 6 + 32 + 32 + 1], h[20];
    size_t n = 0;
    for (size_t i = 0; i < 22; i++) d[n++] = (uint8_t)etiqueta[i];
    d[n++] = 0;
    const uint8_t *m1 = menor6(aa, spa) ? aa : spa, *m2 = m1 == aa ? spa : aa;
    for (int i = 0; i < 6; i++) d[n++] = m1[i];
    for (int i = 0; i < 6; i++) d[n++] = m2[i];
    const uint8_t *n1 = menor32(anonce, snonce) ? anonce : snonce, *n2 = n1 == anonce ? snonce : anonce;
    for (int i = 0; i < 32; i++) d[n++] = n1[i];
    for (int i = 0; i < 32; i++) d[n++] = n2[i];
    size_t hecho = 0;
    for (uint8_t i = 0; hecho < 48; i++) {
        d[n] = i;                                               /* contador */
        if (hmac_sha1(pmk, 32, d, n + 1, h)) return -1;
        for (int k = 0; k < 20 && hecho < 48; k++) ptk[hecho++] = h[k];
    }
    mbedtls_platform_zeroize(h, sizeof(h));
    return 0;
}

int wpa_mic(const uint8_t kck[16], const uint8_t *eapol, size_t n, uint8_t mic[16]) { RED_EXCLUSIVA();
    uint8_t h[20];
    if (hmac_sha1(kck, 16, eapol, n, h)) return -1;
    for (int i = 0; i < 16; i++) mic[i] = h[i];
    return 0;
}

int wpa_unwrap(const uint8_t kek[16], const uint8_t *in, size_t n, uint8_t *out) { RED_EXCLUSIVA();
    if (n < 16 || (n % 8)) return -1;
    size_t bloques = n / 8 - 1;
    uint8_t a[8], b[16];
    for (int i = 0; i < 8; i++) a[i] = in[i];
    for (size_t i = 0; i < bloques * 8; i++) out[i] = in[8 + i];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_dec(&aes, kek, 128)) { mbedtls_aes_free(&aes); return -1; }
    for (int j = 5; j >= 0; j--) {
        for (size_t i = bloques; i >= 1; i--) {
            uint64_t t = (uint64_t)bloques * (uint64_t)j + i;
            for (int k = 0; k < 8; k++) b[k] = a[k] ^ (uint8_t)(t >> (56 - 8 * k));
            for (int k = 0; k < 8; k++) b[8 + k] = out[(i - 1) * 8 + k];
            mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, b, b);
            for (int k = 0; k < 8; k++) a[k] = b[k];
            for (int k = 0; k < 8; k++) out[(i - 1) * 8 + k] = b[8 + k];
        }
    }
    mbedtls_aes_free(&aes);
    for (int k = 0; k < 8; k++) if (a[k] != 0xA6) return -1;    /* IV por defecto A6A6A6A6A6A6A6A6 */
    return 0;
}

/* ================================================================== H164: WPA3-Personal */

int wpa_kdf_sha256(const uint8_t *k, size_t kl, const char *etiqueta,
                   const uint8_t *ctx, size_t cl, uint8_t *out, size_t bits) { RED_EXCLUSIVA();
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    size_t el = 0, bytes = (bits + 7) / 8, hecho = 0;
    while (etiqueta[el]) el++;
    uint8_t h[32], largo[2] = { (uint8_t)bits, (uint8_t)(bits >> 8) };
    mbedtls_md_context_t c;
    mbedtls_md_init(&c);
    int r = mbedtls_md_setup(&c, md, 1);
    for (uint16_t i = 1; !r && hecho < bytes; i++) {
        uint8_t ib[2] = { (uint8_t)i, (uint8_t)(i >> 8) };
        r = mbedtls_md_hmac_starts(&c, k, kl);
        if (!r) r = mbedtls_md_hmac_update(&c, ib, 2);
        if (!r) r = mbedtls_md_hmac_update(&c, (const uint8_t *)etiqueta, el);
        if (!r && cl) r = mbedtls_md_hmac_update(&c, ctx, cl);
        if (!r) r = mbedtls_md_hmac_update(&c, largo, 2);
        if (!r) r = mbedtls_md_hmac_finish(&c, h);
        for (size_t j = 0; !r && j < 32 && hecho < bytes; j++) out[hecho++] = h[j];
    }
    mbedtls_md_free(&c);
    mbedtls_platform_zeroize(h, sizeof(h));
    return r ? -1 : 0;
}

int wpa_ptk_sha256(const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                   const uint8_t anonce[32], const uint8_t snonce[32], uint8_t ptk[48]) { RED_EXCLUSIVA();
    uint8_t d[76];
    const uint8_t *m1 = menor6(aa, spa) ? aa : spa, *m2 = m1 == aa ? spa : aa;
    const uint8_t *n1 = menor32(anonce, snonce) ? anonce : snonce, *n2 = n1 == anonce ? snonce : anonce;
    for (int i = 0; i < 6; i++) { d[i] = m1[i]; d[6 + i] = m2[i]; }
    for (int i = 0; i < 32; i++) { d[12 + i] = n1[i]; d[44 + i] = n2[i]; }
    return wpa_kdf_sha256(pmk, 32, "Pairwise key expansion", d, sizeof(d), ptk, 384);
}

static void cmac_doblar(uint8_t b[16]) {
    uint8_t acarreo = b[0] & 0x80;
    for (int i = 0; i < 15; i++) b[i] = (uint8_t)((b[i] << 1) | (b[i + 1] >> 7));
    b[15] = (uint8_t)(b[15] << 1);
    if (acarreo) b[15] ^= 0x87;
}

int wpa_mic_cmac(const uint8_t kck[16], const uint8_t *d, size_t n, uint8_t mic[16]) { RED_EXCLUSIVA();
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, kck, 128)) { mbedtls_aes_free(&aes); return -1; }
    uint8_t k1[16] = { 0 }, k2[16], x[16] = { 0 }, u[16];
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, k1, k1);    /* L = E(0) */
    cmac_doblar(k1);
    for (int i = 0; i < 16; i++) k2[i] = k1[i];
    cmac_doblar(k2);
    size_t nb = n ? (n + 15) / 16 : 1;
    int completo = n && (n % 16 == 0);
    for (size_t b = 0; b + 1 < nb; b++) {
        for (int i = 0; i < 16; i++) x[i] ^= d[b * 16 + i];
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, x, x);
    }
    size_t base = (nb - 1) * 16, resto = n - base;
    for (int i = 0; i < 16; i++) {
        uint8_t m = (size_t)i < resto ? d[base + i] : ((size_t)i == resto ? 0x80 : 0);
        u[i] = (uint8_t)(m ^ (completo ? k1[i] : k2[i]));
        x[i] ^= u[i];
    }
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, x, mic);
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(k1, 16); mbedtls_platform_zeroize(k2, 16);
    return 0;
}

/* ---------------------------------------------------------------------------- CCMP-128
 * CCM (RFC 3610) con M = 8, L = 2, como lo usa CCMP (802.11-2020 12.5.3.3).
 * H165: generalizado a tramas de datos (QoS, 4 direcciones) para el enlace por WiFi. */

/* Largo de la cabecera MAC segun FC: 24, +6 con 4 direcciones, +2 con QoS, +4 con HTC. */
size_t wpa_largo_cabecera(const uint8_t *t) { RED_EXCLUSIVA();
    size_t h = 24;
    int datos = ((t[0] >> 2) & 3) == 2, qos = datos && (t[0] & 0x80);
    if (datos && (t[1] & 3) == 3) h += 6;
    if (qos) h += 2;
    if ((t[1] & 0x80) && (qos || ((t[0] >> 2) & 3) == 0)) h += 4;   /* +HTC */
    return h;
}

/* Nonce y AAD (12.5.3.3.3-4). Devuelve el largo de la AAD (22..30). */
static size_t ccmp_nonce_aad(const uint8_t *t, size_t hl, uint64_t pn, uint8_t nonce[13], uint8_t aad[32]) {
    int datos = ((t[0] >> 2) & 3) == 2, qos = datos && (t[0] & 0x80), a4 = datos && (t[1] & 3) == 3;
    size_t k = 0;
    aad[k++] = datos ? (uint8_t)(t[0] & 0x8F) : t[0];                       /* subtipo b4-b6 en datos */
    uint8_t fc1 = (uint8_t)((t[1] & ~0x38) | 0x40);                          /* sin Retry/PwrMgt/MoreData */
    if (qos) fc1 &= (uint8_t)~0x80;                                          /* Order en QoS */
    aad[k++] = fc1;
    for (int i = 0; i < 18; i++) aad[k++] = t[4 + i];
    aad[k++] = t[22] & 0x0F; aad[k++] = 0;                                   /* SC: solo el fragmento */
    if (a4) for (int i = 0; i < 6; i++) aad[k++] = t[24 + i];
    uint8_t prio = 0;
    if (qos) {
        size_t q = a4 ? 30 : 24;
        prio = t[q] & 0x0F;
        aad[k++] = prio; aad[k++] = 0;                                       /* QC: solo el TID */
    }
    (void)hl;
    nonce[0] = datos ? prio : 0x10;                                          /* bit Management en gestion */
    for (int i = 0; i < 6; i++) nonce[1 + i] = t[10 + i];                    /* A2 */
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (40 - 8 * i));
    return k;
}

/* CBC-MAC (T, 8 B sin cifrar) sobre B0 | AAD | mensaje en claro. */
static void ccm_mac(mbedtls_aes_context *aes, const uint8_t nonce[13], const uint8_t *aad, size_t al,
                    const uint8_t *p, size_t pl, uint8_t x[16]) {
    x[0] = 0x59;
    for (int i = 0; i < 13; i++) x[1 + i] = nonce[i];
    x[14] = (uint8_t)(pl >> 8); x[15] = (uint8_t)pl;
    mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_ENCRYPT, x, x);
    uint8_t ab[32] = { 0 };
    ab[1] = (uint8_t)al;
    for (size_t i = 0; i < al; i++) ab[2 + i] = aad[i];
    for (size_t bl = 0; bl < (al + 2 + 15) / 16; bl++) {
        for (int i = 0; i < 16; i++) x[i] ^= ab[bl * 16 + i];
        mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_ENCRYPT, x, x);
    }
    for (size_t off = 0; off < pl; off += 16) {
        for (size_t i = 0; i < 16 && off + i < pl; i++) x[i] ^= p[off + i];
        mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_ENCRYPT, x, x);
    }
}

/* CTR: out = in ^ E(A_i), i >= 1; s0 = E(A_0) para el MIC. */
static void ccm_ctr(mbedtls_aes_context *aes, const uint8_t nonce[13], const uint8_t *in, size_t pl,
                    uint8_t *out, uint8_t s0[16]) {
    uint8_t b[16], s[16];
    b[0] = 0x01;
    for (int i = 0; i < 13; i++) b[1 + i] = nonce[i];
    uint16_t ctr = 1;
    for (size_t off = 0; off < pl; off += 16, ctr++) {
        b[14] = (uint8_t)(ctr >> 8); b[15] = (uint8_t)ctr;
        mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_ENCRYPT, b, s);
        for (size_t i = 0; i < 16 && off + i < pl; i++) out[off + i] = (uint8_t)(in[off + i] ^ s[i]);
    }
    b[14] = 0; b[15] = 0;
    mbedtls_aes_crypt_ecb(aes, MBEDTLS_AES_ENCRYPT, b, s0);
    mbedtls_platform_zeroize(s, 16);
}

size_t wpa_ccmp_cifrar(const uint8_t tk[16], uint64_t pn, uint8_t keyid,
                       const uint8_t *t, size_t n, uint8_t *o) { RED_EXCLUSIVA();
    size_t hl = wpa_largo_cabecera(t);
    if (n < hl || n - hl > 2304) return 0;
    size_t pl = n - hl;
    uint8_t nonce[13], aad[32], x[16], s0[16];
    size_t al = ccmp_nonce_aad(t, hl, pn, nonce, aad);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, tk, 128)) { mbedtls_aes_free(&aes); return 0; }
    ccm_mac(&aes, nonce, aad, al, t + hl, pl, x);
    /* Salida: cabecera con Protected y cabecera CCMP (PN0 PN1 0 ExtIV|keyid PN2..PN5).
     * 'o' y 't' no deben solaparse. */
    ccm_ctr(&aes, nonce, t + hl, pl, o + hl + 8, s0);
    for (size_t i = 0; i < hl; i++) o[i] = t[i];
    o[1] |= 0x40;
    o[hl] = (uint8_t)pn; o[hl + 1] = (uint8_t)(pn >> 8); o[hl + 2] = 0; o[hl + 3] = (uint8_t)(0x20 | (keyid << 6));
    o[hl + 4] = (uint8_t)(pn >> 16); o[hl + 5] = (uint8_t)(pn >> 24); o[hl + 6] = (uint8_t)(pn >> 32); o[hl + 7] = (uint8_t)(pn >> 40);
    for (int i = 0; i < 8; i++) o[hl + 8 + pl + i] = (uint8_t)(x[i] ^ s0[i]);
    mbedtls_aes_free(&aes);
    mbedtls_platform_zeroize(s0, 16);
    return n + 16;
}

int wpa_ccmp_descifrar(const uint8_t tk[16], const uint8_t *t, size_t n, uint8_t *claro,
                       size_t *largo_claro, uint64_t *pn_salida) { RED_EXCLUSIVA();
    size_t hl = wpa_largo_cabecera(t);
    if (n < hl + 16 || !(t[hl + 3] & 0x20)) return -1;                       /* ExtIV obligatorio */
    size_t pl = n - hl - 16;
    if (pl > 2304) return -1;
    const uint8_t *c = t + hl;
    uint64_t pn = (uint64_t)c[0] | ((uint64_t)c[1] << 8) | ((uint64_t)c[4] << 16) | ((uint64_t)c[5] << 24) |
                  ((uint64_t)c[6] << 32) | ((uint64_t)c[7] << 40);
    uint8_t nonce[13], aad[32], x[16], s0[16];
    size_t al = ccmp_nonce_aad(t, hl, pn, nonce, aad);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, tk, 128)) { mbedtls_aes_free(&aes); return -1; }
    ccm_ctr(&aes, nonce, c + 8, pl, claro, s0);
    ccm_mac(&aes, nonce, aad, al, claro, pl, x);
    mbedtls_aes_free(&aes);
    uint8_t dif = 0;
    for (int i = 0; i < 8; i++) dif |= (uint8_t)((x[i] ^ s0[i]) ^ c[8 + pl + i]);
    mbedtls_platform_zeroize(s0, 16);
    if (dif) { mbedtls_platform_zeroize(claro, pl); return -2; }
    *largo_claro = pl;
    if (pn_salida) *pn_salida = pn;
    return 0;
}

size_t wpa_ccmp_gestion(const uint8_t tk[16], uint64_t pn, uint8_t keyid,
                        const uint8_t *t, size_t n, uint8_t *o) { RED_EXCLUSIVA();
    return wpa_ccmp_cifrar(tk, pn, keyid, t, n, o);
}

/* --------------------------------------------------------------- SAE (grupo 19) */
static wpa_azar_fn g_azar;
static int sae_rng(void *p, unsigned char *b, size_t n) { (void)p; return g_azar && !g_azar(b, n) ? 0 : -1; }

static int hmac_sha256(const uint8_t *k, size_t kl, const uint8_t *d, size_t dl, uint8_t out[32]) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    return mbedtls_md_hmac(md, k, kl, d, dl, out);
}

static int punto_leer(const mbedtls_ecp_group *g, mbedtls_ecp_point *pt, const uint8_t xy[64]) {
    uint8_t b[65];
    b[0] = 0x04;
    for (int i = 0; i < 64; i++) b[1 + i] = xy[i];
    return mbedtls_ecp_point_read_binary(g, pt, b, 65);
}
static int punto_escribir(const mbedtls_ecp_group *g, const mbedtls_ecp_point *pt, uint8_t xy[64]) {
    uint8_t b[65];
    size_t ol = 0;
    int r = mbedtls_ecp_point_write_binary(g, pt, MBEDTLS_ECP_PF_UNCOMPRESSED, &ol, b, sizeof(b));
    if (r || ol != 65) return -1;
    for (int i = 0; i < 64; i++) xy[i] = b[1 + i];
    return 0;
}
/* y <- p - y sobre la representacion binaria */
static int punto_negar(const mbedtls_ecp_group *g, uint8_t xy[64]) {
    mbedtls_mpi y;
    mbedtls_mpi_init(&y);
    int r = mbedtls_mpi_read_binary(&y, xy + 32, 32);
    if (!r && mbedtls_mpi_cmp_int(&y, 0) != 0) r = mbedtls_mpi_sub_mpi(&y, &g->P, &y);
    if (!r) r = mbedtls_mpi_write_binary(&y, xy + 32, 32);
    mbedtls_mpi_free(&y);
    return r;
}

int sae_pwe(struct wpa_sae *s, const uint8_t mia[6], const uint8_t par[6], const char *clave, size_t cl) { RED_EXCLUSIVA();
    if (cl == 0 || cl > 128) return -1;
    mbedtls_ecp_group g;
    mbedtls_mpi x, rhs, t, y, e;
    mbedtls_ecp_group_init(&g);
    mbedtls_mpi_init(&x); mbedtls_mpi_init(&rhs); mbedtls_mpi_init(&t); mbedtls_mpi_init(&y); mbedtls_mpi_init(&e);
    uint8_t dir[12], msg[129], semilla[32], valor[32], p_bin[32];
    int r = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1);
    if (!r) r = mbedtls_mpi_write_binary(&g.P, p_bin, 32);
    /* e = (p + 1) / 4: p = 3 mod 4, asi que la raiz es rhs^e */
    if (!r) r = mbedtls_mpi_add_int(&e, &g.P, 1);
    if (!r) r = mbedtls_mpi_shift_r(&e, 2);
    int mayor = 0;
    for (int i = 0; i < 6; i++) if (mia[i] != par[i]) { mayor = mia[i] > par[i]; break; }
    for (int i = 0; i < 6; i++) { dir[i] = mayor ? mia[i] : par[i]; dir[6 + i] = mayor ? par[i] : mia[i]; }
    for (size_t i = 0; i < cl; i++) msg[i] = (uint8_t)clave[i];
    s->contador_pwe = 0;
    for (uint32_t c = 1; !r && (c <= 40 || !s->contador_pwe) && c < 200; c++) {
        msg[cl] = (uint8_t)c;
        r = hmac_sha256(dir, 12, msg, cl + 1, semilla);
        if (!r) r = wpa_kdf_sha256(semilla, 32, "SAE Hunting and Pecking", p_bin, 32, valor, 256);
        if (r) break;
        if (mbedtls_mpi_read_binary(&x, valor, 32) || mbedtls_mpi_cmp_mpi(&x, &g.P) >= 0) continue;
        /* rhs = x^3 - 3x + b mod p */
        r = mbedtls_mpi_mul_mpi(&t, &x, &x);
        if (!r) r = mbedtls_mpi_mod_mpi(&t, &t, &g.P);
        if (!r) r = mbedtls_mpi_mul_mpi(&rhs, &t, &x);
        if (!r) r = mbedtls_mpi_mul_int(&t, &x, 3);
        if (!r) r = mbedtls_mpi_sub_mpi(&rhs, &rhs, &t);
        if (!r) r = mbedtls_mpi_add_mpi(&rhs, &rhs, &g.B);
        if (!r) r = mbedtls_mpi_mod_mpi(&rhs, &rhs, &g.P);
        if (!r) r = mbedtls_mpi_exp_mod(&y, &rhs, &e, &g.P, 0);
        if (!r) r = mbedtls_mpi_mul_mpi(&t, &y, &y);
        if (!r) r = mbedtls_mpi_mod_mpi(&t, &t, &g.P);
        if (r) break;
        if (mbedtls_mpi_cmp_mpi(&t, &rhs) != 0 || s->contador_pwe) continue;   /* no residuo, o ya hallado */
        s->contador_pwe = c;
        /* y con el bit menos significativo igual al de la semilla */
        if ((uint8_t)mbedtls_mpi_get_bit(&y, 0) != (semilla[31] & 1)) r = mbedtls_mpi_sub_mpi(&y, &g.P, &y);
        if (!r) r = mbedtls_mpi_write_binary(&x, s->pwe, 32);
        if (!r) r = mbedtls_mpi_write_binary(&y, s->pwe + 32, 32);
    }
    if (!r && !s->contador_pwe) r = -1;
    mbedtls_platform_zeroize(msg, sizeof(msg));
    mbedtls_platform_zeroize(semilla, sizeof(semilla));
    mbedtls_platform_zeroize(valor, sizeof(valor));
    mbedtls_mpi_free(&x); mbedtls_mpi_free(&rhs); mbedtls_mpi_free(&t); mbedtls_mpi_free(&y); mbedtls_mpi_free(&e);
    mbedtls_ecp_group_free(&g);
    return r ? -1 : 0;
}

int sae_commit(struct wpa_sae *s, const uint8_t *rand_fijo, const uint8_t *mask_fijo, wpa_azar_fn azar) { RED_EXCLUSIVA();
    g_azar = azar;
    mbedtls_ecp_group g;
    mbedtls_ecp_point pwe, el;
    mbedtls_mpi rnd, mask, esc;
    mbedtls_ecp_group_init(&g); mbedtls_ecp_point_init(&pwe); mbedtls_ecp_point_init(&el);
    mbedtls_mpi_init(&rnd); mbedtls_mpi_init(&mask); mbedtls_mpi_init(&esc);
    int r = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1);
    if (!r) r = punto_leer(&g, &pwe, s->pwe);
    for (int intento = 0; !r && intento < 8; intento++) {
        if (rand_fijo) r = mbedtls_mpi_read_binary(&rnd, rand_fijo, 32);
        else r = mbedtls_mpi_random(&rnd, 2, &g.N, sae_rng, 0);
        if (!r && mask_fijo) r = mbedtls_mpi_read_binary(&mask, mask_fijo, 32);
        else if (!r) r = mbedtls_mpi_random(&mask, 2, &g.N, sae_rng, 0);
        if (!r) r = mbedtls_mpi_add_mpi(&esc, &rnd, &mask);
        if (!r) r = mbedtls_mpi_mod_mpi(&esc, &esc, &g.N);
        if (!r && mbedtls_mpi_cmp_int(&esc, 1) > 0) break;          /* escalar > 1 */
        if (rand_fijo) r = -1;
    }
    if (!r) r = mbedtls_ecp_mul(&g, &el, &mask, &pwe, sae_rng, 0);
    if (!r) r = punto_escribir(&g, &el, s->elemento);
    if (!r) r = punto_negar(&g, s->elemento);                      /* elemento = -(mask * PWE) */
    if (!r) r = mbedtls_mpi_write_binary(&esc, s->escalar, 32);
    if (!r) r = mbedtls_mpi_write_binary(&rnd, s->rand, 32);
    mbedtls_mpi_free(&rnd); mbedtls_mpi_free(&mask); mbedtls_mpi_free(&esc);
    mbedtls_ecp_point_free(&pwe); mbedtls_ecp_point_free(&el); mbedtls_ecp_group_free(&g);
    return r ? -1 : 0;
}

int sae_procesar(struct wpa_sae *s, const uint8_t escalar_par[32], const uint8_t elemento_par[64], wpa_azar_fn azar) { RED_EXCLUSIVA();
    g_azar = azar;
    /* Reflejo: el AP no puede devolver nuestro propio commit. */
    int igual = 1;
    for (int i = 0; i < 32; i++) if (escalar_par[i] != s->escalar[i]) igual = 0;
    for (int i = 0; i < 64; i++) if (elemento_par[i] != s->elemento[i]) igual = 0;
    if (igual) return -2;
    mbedtls_ecp_group g;
    mbedtls_ecp_point pwe, pe, t, k;
    mbedtls_mpi ep, rnd, uno, ctx;
    mbedtls_ecp_group_init(&g);
    mbedtls_ecp_point_init(&pwe); mbedtls_ecp_point_init(&pe); mbedtls_ecp_point_init(&t); mbedtls_ecp_point_init(&k);
    mbedtls_mpi_init(&ep); mbedtls_mpi_init(&rnd); mbedtls_mpi_init(&uno); mbedtls_mpi_init(&ctx);
    uint8_t kx[64], semilla[32], cero[32] = { 0 }, ctxb[32], llaves[64];
    int r = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1);
    if (!r) r = mbedtls_mpi_read_binary(&ep, escalar_par, 32);
    if (!r && (mbedtls_mpi_cmp_int(&ep, 1) <= 0 || mbedtls_mpi_cmp_mpi(&ep, &g.N) >= 0)) r = -3;   /* 1 < s < r */
    if (!r) r = punto_leer(&g, &pe, elemento_par);
    if (!r && mbedtls_ecp_check_pubkey(&g, &pe)) r = -4;                                         /* en la curva */
    if (!r) r = punto_leer(&g, &pwe, s->pwe);
    if (!r) r = mbedtls_mpi_lset(&uno, 1);
    /* K = rand * (escalar_par * PWE + elemento_par) */
    if (!r) r = mbedtls_ecp_muladd(&g, &t, &ep, &pwe, &uno, &pe);
    if (!r) r = mbedtls_mpi_read_binary(&rnd, s->rand, 32);
    if (!r) r = mbedtls_ecp_mul(&g, &k, &rnd, &t, sae_rng, 0);
    if (!r && mbedtls_ecp_is_zero(&k)) r = -5;
    if (!r) r = punto_escribir(&g, &k, kx);
    /* keyseed = HMAC-SHA256(0^32, k); contexto = (escalar + escalar_par) mod r */
    if (!r) r = hmac_sha256(cero, 32, kx, 32, semilla);
    if (!r) r = mbedtls_mpi_read_binary(&ctx, s->escalar, 32);
    if (!r) r = mbedtls_mpi_add_mpi(&ctx, &ctx, &ep);
    if (!r) r = mbedtls_mpi_mod_mpi(&ctx, &ctx, &g.N);
    if (!r) r = mbedtls_mpi_write_binary(&ctx, ctxb, 32);
    if (!r) r = wpa_kdf_sha256(semilla, 32, "SAE KCK and PMK", ctxb, 32, llaves, 512);
    if (!r) {
        for (int i = 0; i < 32; i++) { s->kck[i] = llaves[i]; s->pmk[i] = llaves[32 + i]; }
        for (int i = 0; i < 16; i++) s->pmkid[i] = ctxb[i];
        for (int i = 0; i < 32; i++) s->escalar_par[i] = escalar_par[i];
        for (int i = 0; i < 64; i++) s->elemento_par[i] = elemento_par[i];
    }
    mbedtls_platform_zeroize(kx, sizeof(kx)); mbedtls_platform_zeroize(semilla, sizeof(semilla));
    mbedtls_platform_zeroize(llaves, sizeof(llaves));
    mbedtls_mpi_free(&ep); mbedtls_mpi_free(&rnd); mbedtls_mpi_free(&uno); mbedtls_mpi_free(&ctx);
    mbedtls_ecp_point_free(&pwe); mbedtls_ecp_point_free(&pe); mbedtls_ecp_point_free(&t); mbedtls_ecp_point_free(&k);
    mbedtls_ecp_group_free(&g);
    return r ? (r < 0 && r >= -5 ? r : -1) : 0;
}

void sae_confirm(const struct wpa_sae *s, uint16_t sc, int propio, uint8_t out[32]) { RED_EXCLUSIVA();
    uint8_t d[2 + 32 + 64 + 32 + 64];
    const uint8_t *e1 = propio ? s->escalar : s->escalar_par, *p1 = propio ? s->elemento : s->elemento_par;
    const uint8_t *e2 = propio ? s->escalar_par : s->escalar, *p2 = propio ? s->elemento_par : s->elemento;
    d[0] = (uint8_t)sc; d[1] = (uint8_t)(sc >> 8);
    for (int i = 0; i < 32; i++) { d[2 + i] = e1[i]; d[98 + i] = e2[i]; }
    for (int i = 0; i < 64; i++) { d[34 + i] = p1[i]; d[130 + i] = p2[i]; }
    hmac_sha256(s->kck, 32, d, sizeof(d), out);
}
