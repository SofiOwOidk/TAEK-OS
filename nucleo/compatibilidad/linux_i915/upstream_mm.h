#ifndef TAEK_UPSTREAM_MM_H
#define TAEK_UPSTREAM_MM_H
/* Frontera C interna sin layouts Linux expuestos. No es UAPI/GGTT ownership. */
struct taek_upstream_mm_resultado {
    unsigned checks,fallos,limpio,avisos;
    unsigned long long bajo,alto;
};
int taek_upstream_drm_mm_probar(struct taek_upstream_mm_resultado *out);
int intel_diagnostico_upstream_mm(void);
#endif
