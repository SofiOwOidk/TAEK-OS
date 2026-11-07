/* DMAR/device scopes: Intel VT-d spec 5.0, seccion 8; ACPICA actbl1.h de
 * Linux v6.6.78. Sin acceso PCI/MMIO, sin escrituras ni autorizacion GPU. */
#include "iommu.h"
#ifdef TAEK_HOST_TEST
#include <string.h>
#else
#include "../base/memoria.h"
#endif

#define DMAR_MAX_BYTES (1024u * 1024u)
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) {
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}
static uint64_t le64(const uint8_t *p) {
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}
static int scopes(const uint8_t *p, size_t bytes, iommu_scope_t *out, uint8_t *count) {
    *count = 0;
    while (bytes) {
        if (bytes < 6 || p[1] < 8 || p[1] > bytes || (p[1] - 6) % 2 ||
            !p[0] || p[0] > 5 || le16(p + 2) || *count == IOMMU_MAX_SCOPES)
            return -1;
        unsigned steps = (p[1] - 6) / 2;
        if (steps > IOMMU_MAX_PASOS) return -1;
        iommu_scope_t *s = &out[(*count)++];
        s->tipo = p[0]; s->id_enumeracion = p[4]; s->bus_inicio = p[5];
        s->conteo_pasos = steps;
        for (unsigned i = 0; i < steps; ++i) {
            if (p[6 + i * 2] > 31 || p[7 + i * 2] > 7) return -1;
            s->pasos[i] = (iommu_paso_pci_t){p[6 + i * 2], p[7 + i * 2]};
        }
        bytes -= p[1]; p += p[1];
    }
    return 0;
}
int iommu_dmar_analizar(const void *data, size_t bytes, iommu_estado_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    const uint8_t *p = data;
    if (!p || bytes < 48 || memcmp(p, "DMAR", 4)) goto fail;
    uint32_t length = le32(p + 4);
    if (length < 48 || length > bytes || length > DMAR_MAX_BYTES || p[36] >= 64) goto fail;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) sum += p[i];
    if (sum) goto fail;
    out->ancho_direccion_host = p[36] + 1; out->banderas_dmar = p[37];
    for (size_t pos = 48; pos < length;) {
        if (length - pos < 4) goto fail;
        const uint8_t *sub = p + pos;
        unsigned type = le16(sub), size = le16(sub + 2);
        if (size < 4 || size > length - pos) goto fail;
        if (type == 0) {
            if (size < 16 || out->conteo_drhd == MAX_UNIDADES_DRHD) goto fail;
            iommu_unidad_drhd_t *u = &out->drhd[out->conteo_drhd++];
            u->segmento_pci = le16(sub + 6); u->abarca_todos_pci = sub[4] & 1;
            u->mmio_fisica = le64(sub + 8);
            if (!u->mmio_fisica || (u->mmio_fisica & 4095) ||
                u->mmio_fisica >= UINT64_C(0x10000000000000) ||
                scopes(sub + 16, size - 16, u->scopes, &u->conteo_scopes)) goto fail;
            if (!u->abarca_todos_pci && !u->conteo_scopes) goto fail;
            for (unsigned i = 0; i + 1 < out->conteo_drhd; ++i)
                if (out->drhd[i].mmio_fisica == u->mmio_fisica ||
                    (u->abarca_todos_pci && out->drhd[i].abarca_todos_pci &&
                     out->drhd[i].segmento_pci == u->segmento_pci)) goto fail;
        } else if (type == 1) {
            if (size < 24 || out->conteo_rmrr == MAX_REGIONES_RMRR) goto fail;
            iommu_region_rmrr_t *r = &out->rmrr[out->conteo_rmrr++];
            r->segmento_pci = le16(sub + 6); r->dir_base_fisica = le64(sub + 8);
            r->dir_limite_fisica = le64(sub + 16);
            if (r->dir_base_fisica > r->dir_limite_fisica ||
                (r->dir_base_fisica & 4095) || (r->dir_limite_fisica & 4095) != 4095 ||
                r->dir_limite_fisica >= UINT64_C(0x10000000000000) ||
                scopes(sub + 24, size - 24, r->scopes, &r->conteo_scopes) ||
                !r->conteo_scopes) goto fail;
        }
        pos += size;
    }
    if (!out->conteo_drhd) goto fail;
    out->tabla_dmar_detectada = 1; out->inventario_completo = 1;
    return 0;
fail:
    memset(out, 0, sizeof(*out)); out->error_descubrimiento = 1;
    return -1;
}
static int valid_route(const iommu_ruta_pci_t *r) {
    if (!r || !r->conteo_pasos || r->conteo_pasos > IOMMU_MAX_PASOS) return 0;
    for (unsigned i = 0; i < r->conteo_pasos; ++i)
        if (r->pasos[i].dispositivo > 31 || r->pasos[i].funcion > 7) return 0;
    return 1;
}
static int covers(const iommu_scope_t *s, const iommu_ruta_pci_t *r) {
    if ((s->tipo != 1 && s->tipo != 2) || s->bus_inicio != r->bus_raiz ||
        s->conteo_pasos > r->conteo_pasos || !s->conteo_pasos ||
        s->conteo_pasos > IOMMU_MAX_PASOS ||
        (s->tipo == 1 && s->conteo_pasos != r->conteo_pasos)) return 0;
    return !memcmp(s->pasos, r->pasos, s->conteo_pasos * sizeof(s->pasos[0]));
}
int iommu_drhd_por_ruta(const iommu_estado_t *s, const iommu_ruta_pci_t *r, uint32_t *idx) {
    if (!idx) return -1;
    *idx = UINT32_MAX;
    if (!s || !s->inventario_completo || s->error_descubrimiento || !valid_route(r) ||
        !s->tabla_dmar_detectada || s->conteo_drhd > MAX_UNIDADES_DRHD) return -1;
    int explicit_unit = -1, fallback = -1;
    for (unsigned i = 0; i < s->conteo_drhd; ++i) {
        const iommu_unidad_drhd_t *u = &s->drhd[i];
        if (u->segmento_pci != r->segmento) continue;
        if (u->conteo_scopes > IOMMU_MAX_SCOPES) return -1;
        if (u->abarca_todos_pci) {
            if (fallback >= 0) return -2;
            fallback = i; continue;
        }
        for (unsigned j = 0; j < u->conteo_scopes; ++j) {
            if (!covers(&u->scopes[j], r)) continue;
            if (explicit_unit >= 0 && explicit_unit != (int)i) return -2;
            explicit_unit = i;
        }
    }
    int found = explicit_unit >= 0 ? explicit_unit : fallback;
    if (found < 0) return -1;
    *idx = found; return 0;
}
int iommu_dma_directo_observado(const iommu_estado_t *s, const iommu_ruta_pci_t *r,
                              uint32_t *idx) {
    if (iommu_drhd_por_ruta(s, r, idx) || !s->drhd[*idx].activa) return -1;
    return !s->drhd[*idx].traduccion_activa;
}
