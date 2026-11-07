/* Secuencia del lado Linux: lo que i915_init + i915_pci_probe hacen, usando solo funciones
 * originales y en su orden, hasta el modo pedido. El modo GT llega a i915_gem_init sin pasar por
 * intel_display_driver_probe_*: el display sigue siendo del firmware y de la consola de TaekOS.
 * pci_enable_device no se llama: escribiria el registro de comandos, que el firmware ya dejo listo. */
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/workqueue.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/wait_bit.h>
#include "i915_drv.h"
#include "i915_reg.h"
#include "gt/intel_gt_regs.h"
#include "gt/intel_gt.h"
#include "intel_uncore.h"
#include "../upstream_early.h"
#include "../upstream_wq.h"
#include <drm/i915_drm.h>

extern struct taek_early_resultado *taek_early_resultado_interno(void);
extern struct pci_dev *taek_early_pci_crear(unsigned devfn);
extern void taek_early_devres_liberar(struct device *);
extern void taek_early_mmio_modo(int escritura, int simulado);
extern const struct pci_device_id *taek_original_pci_match(u16, u16);
extern int taek_original_drm_core_init(void);
extern int taek_original_module_init(void);
extern struct drm_i915_private *taek_original_create(struct pci_dev *, const struct pci_device_id *);
extern int taek_original_early_probe(struct drm_i915_private *);
extern void taek_original_late_release(struct drm_i915_private *);
extern int taek_original_mmio_a(struct drm_i915_private *);
extern int taek_original_mmio_b(struct drm_i915_private *);
extern int taek_original_mmio_c(struct drm_i915_private *);
extern void taek_original_mmio_release(struct drm_i915_private *);
extern void taek_original_mmio_fin(struct drm_i915_private *);
extern int taek_original_mmio_d(struct drm_i915_private *);
extern void taek_original_sin_pipes(struct drm_i915_private *);
extern int taek_original_gem_h(struct drm_i915_private *, u64, u64, u64);
extern int taek_original_gt_estado(struct drm_i915_private *, u64 *, u64 *, int *);
extern int taek_original_hola(struct drm_i915_private *, u32, u32, char *, size_t);
extern void taek_original_gt_cerrar(struct drm_i915_private *);
extern void taek_original_hola_detalle(int);
extern void taek_original_hola_fuera(int);
extern int taek_media_probar(struct drm_device *, int, char *, size_t, unsigned *);
extern int taek_media_op(struct drm_device *, void *);
extern int taek_original_gt_ocioso(struct drm_i915_private *, long);
extern void taek_original_sin_captura(void);
extern int taek_original_lote(struct drm_i915_private *, u32, char *, size_t);
extern int taek_original_cuelgue(struct drm_i915_private *, char *, size_t);
extern int taek_original_gem_g(struct drm_i915_private *, u64, u64, u64);
extern void taek_original_gem_g_fin(struct drm_i915_private *);
extern void taek_original_gem_g_datos(struct drm_i915_private *, u64 *, u64 *, u64 *, int *, int *, int *);
extern void taek_original_sin_display(int);
extern int taek_original_hw_f(struct drm_i915_private *);
extern void taek_original_hw_f_fin(struct drm_i915_private *);
extern void taek_original_hw_f_datos(struct drm_i915_private *, u64 *, u64 *, unsigned *, unsigned *, unsigned *, unsigned *);
extern int taek_original_hw_e(struct drm_i915_private *, int);
extern void taek_original_hw_e_fin(struct drm_i915_private *);
extern void taek_original_hw_e_datos(struct drm_i915_private *, u64 *, u64 *, u64 *, u64 *, u64 *, int *);
extern void taek_early_robada_detectar(struct pci_dev *);
extern void taek_early_ggtt_escritura(int);
extern void taek_early_pci_simular_gmch(int);
extern unsigned long taek_early_fb_fisica(void);

static int inicios_hechos;

static void fase(long f) {
    taek_early_resultado_interno()->fase = f;
    pr_info("[EARLY] fase %ld\n", f);
}

/* Registros que no deben cambiar: la imagen que el firmware dejo en pantalla. */
static const struct { const char *n; u32 r; } reg_pantalla[] = {
    {"PIPE_A_CONF", 0x70008}, {"PIPE_A_HTOTAL", 0x60000}, {"PIPE_A_SRC", 0x6001c},
    {"PLANE_1A_CTL", 0x70180}, {"PLANE_1A_STRIDE", 0x70188}, {"PLANE_1A_SIZE", 0x70190}, {"PLANE_1A_SURF", 0x7019c},
    {"PWR_WELL_BIOS", 0x45400}, {"PWR_WELL_DRV", 0x45404}, {"DDI_A_BUF", 0x64000}, {"PP_STATUS", 0xc7200},
};
static const struct { const char *n; u32 r; } reg_gt[] = {
    {"FW_RENDER", 0xa278}, {"FW_GT", 0xa188}, {"FW_MEDIA", 0xa270},
    {"FW_ACK_RENDER", 0xd84}, {"FW_ACK_GT", 0x130044}, {"FW_ACK_MEDIA", 0xd88},
    {"RC_CONTROL", 0xa090}, {"RC_STATE", 0xa094}, {"RPSTAT1", 0xa01c}, {"RP_LIMITS", 0x138148},
    {"VCS0_HEAD", 0x12034}, {"VCS0_CTL", 0x1203c}, {"RCS0_CTL", 0x203c},
};
/* Registros de Gen9: VCS0 tiene base 0x12000. */
#define REG_GDRST 0x941c
#define REG_VCS0_HWSTAM 0x12098
#define REG_PIPEA_FRMCOUNT 0x70040
static u32 pantalla_a[ARRAY_SIZE(reg_pantalla)];

static void volcar(struct intel_uncore *u, const char *cuando) {
    pr_info("[MMIO] -- %s\n", cuando);
    for (unsigned i = 0; i < ARRAY_SIZE(reg_gt); i++)
        pr_info("[MMIO] %s %06x=%08x\n", reg_gt[i].n, reg_gt[i].r, __raw_uncore_read32(u, _MMIO(reg_gt[i].r)));
}
static void pantalla_tomar(struct intel_uncore *u) {
    for (unsigned i = 0; i < ARRAY_SIZE(reg_pantalla); i++) {
        pantalla_a[i] = __raw_uncore_read32(u, _MMIO(reg_pantalla[i].r));
        pr_info("[MMIO] %s %06x=%08x\n", reg_pantalla[i].n, reg_pantalla[i].r, pantalla_a[i]);
    }
}
static unsigned pantalla_comparar(struct intel_uncore *u) {
    unsigned cambios = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(reg_pantalla); i++) {
        u32 v = __raw_uncore_read32(u, _MMIO(reg_pantalla[i].r));
        if (v != pantalla_a[i]) {
            pr_err("[MMIO] PANTALLA CAMBIO %s %08x -> %08x\n", reg_pantalla[i].n, pantalla_a[i], v);
            cambios++;
        }
    }
    return cambios;
}

/* Forcewake original: pedir, comprobar acuse y leer con el GT despierto; soltar y ver el acuse caer. */
static int probar_forcewake(struct drm_i915_private *i915, struct intel_uncore *u, unsigned *fallos) {
    pr_info("[MMIO] dominios forcewake=%x\n", u->fw_domains);
    volcar(u, "antes de forcewake");
    intel_uncore_forcewake_get(u, FORCEWAKE_ALL);
    u32 render = __raw_uncore_read32(u, _MMIO(0xd84)), gt = __raw_uncore_read32(u, _MMIO(0x130044)), media = __raw_uncore_read32(u, _MMIO(0xd88));
    volcar(u, "con forcewake");
    u32 id = FORCEWAKE_KERNEL;
    if (!(render & id)) *fallos |= 1;
    if (!(gt & id)) *fallos |= 2;
    if (!(media & id)) *fallos |= 4;
    intel_uncore_forcewake_put(u, FORCEWAKE_ALL);
    /* El soltado real ocurre al caducar el hrtimer en raiz: hay que ceder la CPU. */
    msleep(30);
    volcar(u, "tras soltar");
    if (__raw_uncore_read32(u, _MMIO(0xd84)) & id) *fallos |= 8;
    if (__raw_uncore_read32(u, _MMIO(0x130044)) & id) *fallos |= 16;
    if (__raw_uncore_read32(u, _MMIO(0xd88)) & id) *fallos |= 32;
    (void)i915;
    pr_info("[MMIO] forcewake acuses render=%08x gt=%08x media=%08x fallos=%x\n", render, gt, media, *fallos);
    return *fallos ? -EIO : 0;
}

/* Quien es dueno de cada rango de la GGTT antes de que el shim la toque (M13). La tabla esta en la
 * mitad alta del BAR0; se mapea sin permiso de escritura y se recorre entera, sin cambiar nada.
 * Deja en huella un resumen de toda la tabla: si cambia, alguien la escribio. */
static u64 ggtt_huella, ggtt_pantalla_fisica;
static int ggtt_inventario(struct pci_dev *pdev, u32 superficie, u32 ocupa_bytes) {
    resource_size_t bar = pci_resource_start(pdev, 0), lon = pci_resource_len(pdev, 0);
    u32 gmch, bdsm;
    pci_read_config_dword(pdev, 0x50, &gmch);
    pci_read_config_dword(pdev, 0x5c, &bdsm);
    u32 ggms = (gmch >> 6) & 3;
    u64 tabla = ggms ? (u64)1 << (20 + ggms) : 0;
    pr_info("[GGTT] GMCH=%08x BDSM=%08x tabla=%llu MiB entradas=%llu\n", gmch, bdsm, tabla >> 20, tabla / 8);
    if (!tabla || tabla > lon / 2) { pr_err("[GGTT] tamano de tabla inesperado\n"); return -EINVAL; }
    void __iomem *gsm = ioremap(bar + lon / 2, tabla);
    if (!gsm) return -ENOMEM;
    u64 presentes = 0, primera = 0, ultima = 0, bloques = 0, previa = 0;
    u64 n = tabla / 8;
    ggtt_huella = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < n; i++) {
        u64 pte = readq(gsm + i * 8);
        ggtt_huella = (ggtt_huella ^ pte) * 0x100000001b3ull;
        if (!(pte & 1)) { previa = 0; continue; }
        if (!presentes) primera = i;
        ultima = i;
        presentes++;
        u64 fis = pte & 0x7ffffff000ull;
        if (!previa || fis != previa + 4096) {
            if (bloques < 6) pr_info("[GGTT] bloque en entrada %llu: fisica=%llx pte=%llx\n", i, fis, pte);
            bloques++;
        }
        previa = fis;
    }
    u64 inicio = superficie >> 12, paginas = DIV_ROUND_UP(ocupa_bytes, 4096);
    u64 pte_sup = readq(gsm + inicio * 8);
    u64 fis0 = pte_sup & 0x7ffffff000ull;
    ggtt_pantalla_fisica = fis0;
    unsigned contiguas = 0;
    for (u64 i = 0; i < paginas && inicio + i < n; i++) {
        u64 p = readq(gsm + (inicio + i) * 8);
        if ((p & 1) && (p & 0x7ffffff000ull) == fis0 + i * 4096) contiguas++;
    }
    iounmap(gsm);
    pr_info("[GGTT] presentes=%llu primera=%llu ultima=%llu bloques=%llu\n", presentes, primera, ultima, bloques);
    pr_info("[GGTT] pantalla: entrada=%llu paginas=%llu contiguas=%u fisica=%llx huella=%llx\n", inicio, paginas, contiguas, fis0, ggtt_huella);
    return 0;
}

static unsigned pantalla_viva(struct intel_uncore *u, const char *cuando);
static struct drm_i915_private *i915_retenido;
static u32 hola_semilla = 0x5a17c0de;

/* Cierre del dispositivo. Al terminar, ninguna entrada de la GGTT fuera de la pantalla puede apuntar a
 * memoria ya devuelta: se dejan no presentes, y se comprueba por un mapeo nuevo que la pantalla sigue. */
static u64 cierre_sup, cierre_paginas;
static u64 *cierre_foto;
static int cerrar_gt(struct drm_i915_private *i915, struct pci_dev *pdev, unsigned *fallos) {
    resource_size_t bar = pci_resource_start(pdev, 0), lon = pci_resource_len(pdev, 0);
    int ocioso = taek_original_gt_ocioso(i915, 2000);
    pr_info("[GPU] cierre: ocioso=%d\n", ocioso);
    if (ocioso) return ocioso;   /* Sin quiescencia no se devuelve memoria: se conserva y se reinicia. */
    taek_original_gt_cerrar(i915);
    taek_early_devres_liberar(&pdev->dev);
    i915_retenido = NULL;
    void __iomem *gsm = ioremap(bar + lon / 2, 8ull << 20);
    void __iomem *regs = ioremap(bar, 2ull << 20);
    if (!gsm || !regs) return -ENOMEM;
    u64 n = (8ull << 20) / 8, ini = cierre_sup >> 12, limpias = 0, pantalla_mal = 0;
    for (u64 i = 0; i < n; i++) {
        if (i >= ini && i < ini + cierre_paginas) { if (readq(gsm + i * 8) != cierre_foto[i - ini]) pantalla_mal++; continue; }
        writeq(0, gsm + i * 8);
        limpias++;
    }
    /* Vaciado de la cache de traducciones de la GGTT, como gen8_ggtt_invalidate. */
    writel(1, regs + 0x101008);
    (void)readl(regs + 0x101008);
    u32 a = readl(regs + REG_PIPEA_FRMCOUNT);
    msleep(100);
    u32 b = readl(regs + REG_PIPEA_FRMCOUNT);
    pr_info("[GPU] cierre: entradas_limpias=%llu pantalla_cambiadas=%llu fotogramas=%u\n", limpias, pantalla_mal, b - a);
    if (pantalla_mal) *fallos |= 32768;
    if (a == b) *fallos |= 2048;
    iounmap(regs);
    iounmap(gsm);
    kfree(cierre_foto);
    cierre_foto = NULL;
    return 0;
}

static int probar_hola(struct drm_i915_private *i915, unsigned *fallos) {
    char t[100] = "";
    taek_original_hola_detalle(1);
    int mal = taek_original_hola(i915, hola_semilla, 0, t, sizeof(t));
    taek_original_hola_detalle(0);
    hola_semilla = hola_semilla * 31 + 7;
    pr_info("[GPU] escritura por motor:%s fallos=%d\n", t, mal);
    if (mal) { *fallos |= 65536; return mal < 0 ? mal : -EIO; }
    return 0;
}

/* i915_gem_init completo, con la pantalla reservada y el resto de la tabla en scratch. Aqui la GPU
 * ejecuta por primera vez: lee contextos y anillos, y guarda su estado en memoria. No se desmonta:
 * se sale reiniciando. Las entradas de la pantalla se comparan con su valor anterior. */
static int probar_gt(struct drm_i915_private *i915, struct pci_dev *pdev, struct intel_uncore *u, u64 sup, u64 ocupa, u64 fis0, unsigned *fallos) {
    resource_size_t bar = pci_resource_start(pdev, 0), lon = pci_resource_len(pdev, 0);
    u64 n = DIV_ROUND_UP(ocupa, 4096), ini = sup >> 12;
    u64 *foto = kmalloc_array(n, sizeof(*foto), GFP_KERNEL);
    void __iomem *gsm = ioremap(bar + lon / 2, 8ull << 20);
    if (!foto || !gsm) return -ENOMEM;
    for (u64 i = 0; i < n; i++) foto[i] = readq(gsm + (ini + i) * 8);
    u64 t0 = local_clock();
    int rc = taek_original_gem_h(i915, sup, ocupa, fis0);
    u64 ms = (local_clock() - t0) / 1000000;
    u64 motores = 0, defecto = 0;
    int mojado = -1;
    if (!rc) taek_original_gt_estado(i915, &motores, &defecto, &mojado);
    pr_info("[GPU] i915_gem_init rc=%d en %llu ms motores=%llx con_estado=%llx wedged=%d\n", rc, ms, motores, defecto, mojado);
    u64 distintas = 0, presentes = 0, scratch_ok = 0;
    for (u64 i = 0; i < n; i++) if (readq(gsm + (ini + i) * 8) != foto[i]) distintas++;
    /* Fuera de la pantalla no debe quedar ninguna entrada del contenido anterior sin revisar. */
    for (u64 i = ini + n; i < (8ull << 20) / 8; i += 4096) if (readq(gsm + i * 8) & 1) presentes++; else scratch_ok++;
    pr_info("[GPU] pantalla entradas_cambiadas=%llu muestra_fuera: presentes=%llu sin_presente=%llu\n", distintas, presentes, scratch_ok);
    if (distintas) *fallos |= 32768;
    if (pantalla_comparar(u)) *fallos |= 64;
    if (!pantalla_viva(u, "con la GPU iniciada")) *fallos |= 2048;
    iounmap(gsm);
    if (rc) { kfree(foto); return rc; }
    cierre_foto = foto; cierre_sup = sup; cierre_paginas = n;
    i915_retenido = i915;
    rc = probar_hola(i915, fallos);
    if (pantalla_comparar(u)) *fallos |= 64;
    return rc;
}

/* Primeras escrituras a la tabla. Se guarda la foto completa, se compara entrada por entrada y al
 * terminar se deja como estaba: al desmontar, la pagina scratch a la que apuntarian deja de existir. */
static int probar_gem_ggtt(struct drm_i915_private *i915, struct pci_dev *pdev, struct intel_uncore *u, u64 sup, u64 ocupa, u64 fis0, unsigned *fallos) {
    resource_size_t bar = pci_resource_start(pdev, 0), lon = pci_resource_len(pdev, 0);
    u64 tabla = 8ull << 20, n = tabla / 8;
    unsigned orden = get_order(tabla);
    u64 *foto = (u64 *)__get_free_pages(GFP_KERNEL, orden);
    if (!foto) return -ENOMEM;
    void __iomem *gsm = ioremap(bar + lon / 2, tabla);
    if (!gsm) { free_pages((unsigned long)foto, orden); return -ENOMEM; }
    for (u64 i = 0; i < n; i++) foto[i] = readq(gsm + i * 8);
    int rc = taek_original_gem_g(i915, sup, ocupa, fis0);
    pr_info("[GGTT] i915_init_ggtt rc=%d\n", rc);
    u64 cambios = 0, en_pantalla = 0, primera = 0, ultima = 0, ini = sup >> 12, fin = ini + DIV_ROUND_UP(ocupa, 4096);
    for (u64 i = 0; i < n; i++) {
        u64 v = readq(gsm + i * 8);
        if (v == foto[i]) continue;
        if (!cambios) primera = i;
        ultima = i;
        if (cambios < 8) pr_info("[GGTT] entrada %llu: %llx -> %llx\n", i, foto[i], v);
        cambios++;
        if (i >= ini && i < fin) en_pantalla++;
    }
    pr_info("[GGTT] entradas cambiadas=%llu primera=%llu ultima=%llu en_pantalla=%llu\n", cambios, primera, ultima, en_pantalla);
    if (en_pantalla) *fallos |= 32768;
    if (!rc) {
        u64 cap, cap_t, sesgo;
        int guc, huc, disp;
        taek_original_gem_g_datos(i915, &cap, &cap_t, &sesgo, &guc, &huc, &disp);
        pr_info("[GGTT] captura=%llx+%llx sesgo=%llx guc=%d huc=%d display=%d\n", cap, cap_t, sesgo, guc, huc, disp);
    }
    if (pantalla_comparar(u)) *fallos |= 64;
    if (!pantalla_viva(u, "con la GGTT escrita")) *fallos |= 2048;
    u64 restauradas = 0;
    for (u64 i = 0; i < n; i++) if (readq(gsm + i * 8) != foto[i]) { writeq(foto[i], gsm + i * 8); restauradas++; }
    pr_info("[GGTT] restauradas=%llu\n", restauradas);
    iounmap(gsm);
    free_pages((unsigned long)foto, orden);
    taek_original_gem_g_fin(i915);
    return rc;
}

/* Inicio de hw_probe. Sin una sola escritura a la tabla: su mapeo no tiene permiso. */
static int probar_ggtt(struct drm_i915_private *i915, struct pci_dev *pdev, struct intel_uncore *u, unsigned *fallos, int simulada, int seguir) {
    u32 tam = __raw_uncore_read32(u, _MMIO(0x70190)), zancada = __raw_uncore_read32(u, _MMIO(0x70188));
    u32 sup = __raw_uncore_read32(u, _MMIO(0x7019c)) & ~0xfffu;
    u32 ocupa = (((tam >> 16) & 0xfff) + 1) * (zancada & 0x3ff) * 64;
    u64 antes = 0;
    if (!simulada) {
        int r = ggtt_inventario(pdev, sup, ocupa);
        if (r) return r;
        antes = ggtt_huella;
    }
    taek_early_robada_detectar(pdev);
    taek_early_ggtt_escritura(seguir >= 2);
    int rc = taek_original_hw_e(i915, simulada);
    if (rc) { pr_err("[GGTT] hw_probe parcial: rc=%d\n", rc); return rc; }
    u64 total, mapeable, gmadr, s_dma, s_pte;
    int vtd;
    taek_original_hw_e_datos(i915, &total, &mapeable, &gmadr, &s_dma, &s_pte, &vtd);
    pr_info("[GGTT] total=%llu MiB apertura=%llu MiB en %llx vtd=%d\n", total >> 20, mapeable >> 20, gmadr, vtd);
    pr_info("[GGTT] scratch dma=%llx pte=%llx robada=%llx+%llx\n", s_dma, s_pte, (u64)intel_graphics_stolen_res.start, (u64)resource_size(&intel_graphics_stolen_res));
    u64 fb = taek_early_fb_fisica();
    pr_info("[GGTT] framebuffer firmware=%llx apertura+superficie=%llx\n", fb, gmadr + sup);
    if (!simulada) {
        if (fb != gmadr + sup) *fallos |= 8192;
        ggtt_inventario(pdev, sup, ocupa);
        if (ggtt_huella != antes) { pr_err("[GGTT] LA TABLA CAMBIO %llx -> %llx\n", antes, ggtt_huella); *fallos |= 4096; }
    }
    if (!seguir) { taek_original_hw_e_fin(i915); return 0; }

    /* Etapa F. intel_ggtt_init_fences pone a cero los 32 fences: si el firmware usara alguno no se
     * sigue, porque cambiaria como se ve la memoria a traves de la apertura. */
    fase(TAEK_EARLY_ETAPA_F);
    unsigned activos = 0;
    for (unsigned i = 0; i < 32; i++) {
        u32 lo = __raw_uncore_read32(u, _MMIO(0x100000 + i * 8)), hi = __raw_uncore_read32(u, _MMIO(0x100004 + i * 8));
        if (lo & 1) { activos++; pr_info("[GGTT] fence %u activo: %08x %08x\n", i, hi, lo); }
    }
    pr_info("[GGTT] fences activos del firmware=%u\n", activos);
    if (activos) { *fallos |= 16384; taek_original_hw_e_fin(i915); return -EBUSY; }
    rc = taek_original_hw_f(i915);
    if (rc) {
        /* Sin desmontaje parcial fiable de un probe a medias: se retiene y se sale reiniciando. */
        pr_err("[GGTT] resto de hw_probe: rc=%d\n", rc);
        return rc;
    }
    u64 usable, reservada;
    unsigned fences, dram, swz, regiones;
    taek_original_hw_f_datos(i915, &usable, &reservada, &fences, &dram, &swz, &regiones);
    pr_info("[GGTT] robada usable=%llu MiB reservada=%llu KiB regiones=%u\n", usable >> 20, reservada >> 10, regiones);
    pr_info("[GGTT] fences=%u swizzle_x=%u dram_tipo=%u\n", fences, swz, dram);
    if (seguir == 3) {
        fase(TAEK_EARLY_ETAPA_H);
        return probar_gt(i915, pdev, u, sup, ocupa, ggtt_pantalla_fisica, fallos);
    }
    if (seguir == 2) {
        fase(TAEK_EARLY_ETAPA_G);
        rc = probar_gem_ggtt(i915, pdev, u, sup, ocupa, ggtt_pantalla_fisica, fallos);
        if (rc) pr_err("[GGTT] gem: rc=%d\n", rc);
    }
    if (!simulada) {
        ggtt_inventario(pdev, sup, ocupa);
        if (ggtt_huella != antes) { pr_err("[GGTT] LA TABLA CAMBIO %llx -> %llx\n", antes, ggtt_huella); *fallos |= 4096; }
    }
    taek_original_hw_f_fin(i915);
    return rc;
}

/* El contador de fotogramas solo avanza si el display sigue barriendo la pantalla. */
static unsigned pantalla_viva(struct intel_uncore *u, const char *cuando) {
    u32 a = __raw_uncore_read32(u, _MMIO(REG_PIPEA_FRMCOUNT));
    msleep(100);
    u32 b = __raw_uncore_read32(u, _MMIO(REG_PIPEA_FRMCOUNT));
    pr_info("[MMIO] fotogramas %s: %08x -> %08x (%u en 100 ms)\n", cuando, a, b, b - a);
    return b != a;
}

/* Reset original. La marca en un registro del motor demuestra el efecto: sin ella, un reset que
 * no hiciera nada pareceria correcto. Forcewake se mantiene para que no se pierda por dormir. */
static int probar_reset(struct drm_i915_private *i915, struct intel_uncore *u, unsigned *fallos) {
    intel_uncore_forcewake_get(u, FORCEWAKE_ALL);
    u32 base = __raw_uncore_read32(u, _MMIO(REG_VCS0_HWSTAM)), marca = base ^ 0x00000f00;
    __raw_uncore_write32(u, _MMIO(REG_VCS0_HWSTAM), marca);
    u32 leida = __raw_uncore_read32(u, _MMIO(REG_VCS0_HWSTAM));
    u32 gd0 = __raw_uncore_read32(u, _MMIO(REG_GDRST));
    u64 t0 = local_clock();
    int rc = taek_original_mmio_d(i915);
    u64 ns = local_clock() - t0;
    u32 tras = __raw_uncore_read32(u, _MMIO(REG_VCS0_HWSTAM));
    u32 gd1 = __raw_uncore_read32(u, _MMIO(REG_GDRST));
    if (tras == leida && !rc) __raw_uncore_write32(u, _MMIO(REG_VCS0_HWSTAM), base);
    intel_uncore_forcewake_put(u, FORCEWAKE_ALL);
    pr_info("[MMIO] reset rc=%d en %lu us; GDRST %08x -> %08x\n", rc, (unsigned long)(ns / 1000), gd0, gd1);
    pr_info("[MMIO] marca HWSTAM base=%08x marca=%08x leida=%08x tras_reset=%08x\n", base, marca, leida, tras);
    if (leida != marca) *fallos |= 256;   /* La marca no se escribio: la prueba no demuestra nada. */
    if (tras == marca) *fallos |= 512;    /* Sobrevivio: el reset no tuvo efecto en el motor. */
    if (gd1) *fallos |= 1024;
    msleep(30);
    return rc;
}

static void *media_peticion;
void taek_early_media_peticion(void *p) { media_peticion = p; }

int taek_early_ejecutar(int modo, int simular_5917, int soltar, unsigned long fallo_n) {
    struct taek_early_resultado *r = taek_early_resultado_interno();
    int rc;
    /* Por fotograma: sin tocar el resultado de las ordenes de diagnostico. */
    if (modo == TAEK_EARLY_MEDIA_OP) return i915_retenido ? taek_media_op(&i915_retenido->drm, media_peticion) : -ENODEV;
    r->rc = 0; r->simulada = 0; r->platform = 0; r->subplatform = 0; r->step_gt = 0;
    r->pch_tipo = 0; r->pch_id = 0; r->gt_count = 0; r->mem_regiones_ok = 0;
    r->modo = modo; r->mmio_fallos = 0;
    if (modo == TAEK_EARLY_GT_HOLA) {
        unsigned f = 0;
        if (!i915_retenido) { r->rc = -ENODEV; return -ENODEV; }
        struct pci_dev *d = to_pci_dev(i915_retenido->drm.dev);
        r->vendor = d->vendor; r->device = d->device; r->revision = d->revision;
        rc = probar_hola(i915_retenido, &f);
        r->mmio_fallos = f;
        r->rc = rc;
        fase(TAEK_EARLY_FIN);
        return rc;
    }
    if (modo == TAEK_EARLY_GT_CERRAR) {
        unsigned f = 0;
        if (!i915_retenido) { r->rc = -ENODEV; return -ENODEV; }
        struct pci_dev *d = to_pci_dev(i915_retenido->drm.dev);
        r->vendor = d->vendor; r->device = d->device; r->revision = d->revision;
        rc = cerrar_gt(i915_retenido, d, &f);
        r->mmio_fallos = f;
        r->rc = rc;
        fase(TAEK_EARLY_FIN);
        return rc ?: (f ? -EIO : 0);
    }
    if (modo == TAEK_EARLY_GT_LOTE || modo == TAEK_EARLY_GT_CUELGUE) {
        char t[220] = "";
        unsigned f = 0;
        if (!i915_retenido) { r->rc = -ENODEV; return -ENODEV; }
        struct pci_dev *d = to_pci_dev(i915_retenido->drm.dev);
        struct intel_uncore *u = to_gt(i915_retenido)->uncore;
        r->vendor = d->vendor; r->device = d->device; r->revision = d->revision;
        int mal = modo == TAEK_EARLY_GT_LOTE ? taek_original_lote(i915_retenido, hola_semilla, t, sizeof(t))
                                             : taek_original_cuelgue(i915_retenido, t, sizeof(t));
        hola_semilla = hola_semilla * 31 + 7;
        /* La linea puede superar el limite de printk: se parte al imprimir. */
        pr_info("[GPU] %s fallos=%d\n", modo == TAEK_EARLY_GT_LOTE ? "lote" : "cuelgue", mal);
        for (size_t i = 0; t[i]; i += 100) pr_info("[GPU] %.100s\n", t + i);
        u64 motores, defecto;
        int mojado;
        taek_original_gt_estado(i915_retenido, &motores, &defecto, &mojado);
        pr_info("[GPU] wedged=%d\n", mojado);
        if (pantalla_comparar(u)) f |= 64;
        if (!pantalla_viva(u, "tras la prueba")) f |= 2048;
        rc = mal < 0 ? mal : mal || mojado ? -EIO : 0;
        r->mmio_fallos = f;
        r->rc = rc;
        fase(TAEK_EARLY_FIN);
        return rc;
    }
    if (modo == TAEK_EARLY_GT_MAL) {
        char t[100] = "";
        if (!i915_retenido) { r->rc = -ENODEV; return -ENODEV; }
        struct pci_dev *d = to_pci_dev(i915_retenido->drm.dev);
        r->vendor = d->vendor; r->device = d->device; r->revision = d->revision;
        int mal = taek_original_hola(i915_retenido, hola_semilla, 0x01010101, t, sizeof(t));
        pr_info("[GPU] control negativo:%s detectados=%d\n", t, mal);
        /* El resultado correcto aqui es que todos los motores salgan mal. */
        rc = mal == 4 ? 0 : -EIO;
        r->rc = rc;
        fase(TAEK_EARLY_FIN);
        return rc;
    }
    if (modo == TAEK_EARLY_GT_GUARDA) {
        char t[100] = "";
        if (!i915_retenido) { r->rc = -ENODEV; return -ENODEV; }
        struct pci_dev *d = to_pci_dev(i915_retenido->drm.dev);
        r->vendor = d->vendor; r->device = d->device; r->revision = d->revision;
        taek_original_hola_fuera(1);
        int mal = taek_original_hola(i915_retenido, hola_semilla, 0, t, sizeof(t));
        taek_original_hola_fuera(0);
        unsigned rotas = 0;
        for (const char *p = t; (p = strstr(p, "GUARDA_ROTA")); p++) rotas++;
        pr_info("[GPU] control de guardas:%s detectados=%d guardas=%u\n", t, mal, rotas);
        /* El resultado correcto aqui es que todos los motores rompan una guarda. */
        rc = mal == 4 && rotas == 4 ? 0 : -EIO;
        r->rc = rc;
        fase(TAEK_EARLY_FIN);
        return rc;
    }
    if (modo == TAEK_EARLY_MEDIA_RT || modo == TAEK_EARLY_MEDIA_INIT || modo == TAEK_EARLY_MEDIA_CAPS || modo == TAEK_EARLY_MEDIA_IDR ||
        modo == TAEK_EARLY_MEDIA_MAL || modo == TAEK_EARLY_MEDIA_VIDEO) {
        char t[160] = "";
        unsigned pila = 0;
        if (!i915_retenido) { r->rc = -ENODEV; return -ENODEV; }
        struct pci_dev *d = to_pci_dev(i915_retenido->drm.dev);
        r->vendor = d->vendor; r->device = d->device; r->revision = d->revision;
        rc = taek_media_probar(&i915_retenido->drm, 1 + (int)(modo - TAEK_EARLY_MEDIA_RT), t, sizeof(t), &pila);
        pr_info("[GPU] media: %s pila=%u rc=%d\n", t, pila, rc);
        r->rc = rc;
        fase(TAEK_EARLY_FIN);
        return rc;
    }
    if (modo == TAEK_EARLY_GT_CARGA) {
        unsigned f = 0, rondas = 0;
        char t[100];
        if (!i915_retenido) { r->rc = -ENODEV; return -ENODEV; }
        struct pci_dev *d = to_pci_dev(i915_retenido->drm.dev);
        struct intel_uncore *u = to_gt(i915_retenido)->uncore;
        r->vendor = d->vendor; r->device = d->device; r->revision = d->revision;
        u64 t0 = local_clock();
        rc = 0;
        for (; rondas < 2500 && !rc; rondas++) {
            t[0] = 0;
            int mal = taek_original_hola(i915_retenido, hola_semilla, 0, t, sizeof(t));
            hola_semilla = hola_semilla * 31 + 7;
            if (mal) { pr_err("[GPU] carga: ronda %u:%s\n", rondas, t); rc = mal < 0 ? mal : -EIO; }
            if (!(rondas & 63)) cond_resched();
        }
        u64 us = (local_clock() - t0) / 1000;
        int oc = taek_original_gt_ocioso(i915_retenido, 2000);
        pr_info("[GPU] carga: rondas=%u trabajos=%u en %llu ms ocioso=%d\n", rondas, rondas * 4, us / 1000, oc);
        /* Tras la carga los temporizadores de reposo deben haber podido correr. */
        msleep(1500);
        if (pantalla_comparar(u)) f |= 64;
        if (!pantalla_viva(u, "tras la carga")) f |= 2048;
        volcar(u, "tras la carga");
        if (!rc && oc) rc = oc;
        r->mmio_fallos = f;
        r->rc = rc;
        fase(TAEK_EARLY_FIN);
        return rc;
    }
    if (i915_retenido) { r->rc = -EBUSY; return -EBUSY; }
    if (!inicios_hechos) {
        fase(TAEK_EARLY_WQ);
        wait_bit_init();
        workqueue_init_early();
        workqueue_init();
        fase(TAEK_EARLY_DRM_CORE);
        rc = taek_original_drm_core_init();
        if (rc) { r->rc = rc; return rc; }
        fase(TAEK_EARLY_MODULO);
        rc = taek_original_module_init();
        if (rc) { r->rc = rc; return rc; }
        inicios_hechos = 1;
    }
    fase(TAEK_EARLY_MATCH);
    struct pci_dev *pdev = taek_early_pci_crear(PCI_DEVFN(2, 0));
    if (!pdev) { r->rc = -ENODEV; return -ENODEV; }
    /* Siempre del hardware: una ronda simulada anterior no debe contar como identidad real. */
    u32 id_real;
    pci_read_config_dword(pdev, PCI_VENDOR_ID, &id_real);
    u8 rev_real;
    pci_read_config_byte(pdev, PCI_REVISION_ID, &rev_real);
    r->vendor = id_real & 0xffff; r->device = id_real >> 16; r->revision = rev_real;
    if ((id_real & 0xffff) != PCI_VENDOR_ID_INTEL) {
        if (!simular_5917) { r->rc = -ENODEV; return -ENODEV; }
        /* Solo QEMU: GPU no Intel en 00:02.0; la identidad se declara simulada en el resultado. */
        pdev->vendor = PCI_VENDOR_ID_INTEL; pdev->device = 0x5917; pdev->revision = 2;
        pdev->resource[0].start = 0xe0000000;
        pdev->resource[0].end = 0xe0ffffff;
        pdev->resource[0].flags = IORESOURCE_MEM;
        pdev->resource[2].start = 0xd0000000;
        pdev->resource[2].end = 0xdfffffff;
        pdev->resource[2].flags = IORESOURCE_MEM;
        taek_early_pci_simular_gmch(1);
        r->simulada = 1;
    } else if (simular_5917) { r->rc = -EPERM; return -EPERM; }
    pdev->class = PCI_CLASS_DISPLAY_VGA << 8;
    const struct pci_device_id *ent = taek_original_pci_match(pdev->vendor, pdev->device);
    if (!ent) { r->rc = -ENODEV; return -ENODEV; }
    if (modo != TAEK_EARLY_SOLO_EARLY && !r->simulada) {
        u16 cmd;
        pci_read_config_word(pdev, PCI_COMMAND, &cmd);
        pr_info("[MMIO] PCI_COMMAND=%04x BAR0=%lx+%lx BAR2=%lx+%lx\n", cmd, (unsigned long)pci_resource_start(pdev, 0),
                (unsigned long)pci_resource_len(pdev, 0), (unsigned long)pci_resource_start(pdev, 2), (unsigned long)pci_resource_len(pdev, 2));
        if (!(cmd & PCI_COMMAND_MEMORY)) { pr_err("[MMIO] decodificacion de memoria apagada\n"); r->rc = -EIO; return -EIO; }
    }
    /* Parametro original de i915: el driver no toca el display y la consola de TaekOS sigue siendo suya. */
    taek_original_sin_display(0);
    if (modo == TAEK_EARLY_GT) taek_original_sin_captura();
    fase(TAEK_EARLY_CREATE);
    if (!fallo_n) taek_wq_fallo_limpiar();
    taek_wq_fallo_en(fallo_n);
    struct drm_i915_private *i915 = taek_original_create(pdev, ent);
    if (IS_ERR(i915)) {
        /* Igual que el nucleo de drivers tras un probe fallido: se liberan los recursos gestionados. */
        taek_wq_fallo_en(0);
        taek_early_devres_liberar(&pdev->dev);
        r->rc = PTR_ERR(i915);
        return r->rc;
    }
    /* Antes de early_probe: i915 tambien decide por HAS_DISPLAY si atiende al PCH. */
    if (modo == TAEK_EARLY_GT) taek_original_sin_pipes(i915);
    fase(TAEK_EARLY_PROBE);
    rc = taek_original_early_probe(i915);
    taek_wq_fallo_en(0);
    r->rc = rc;
    if (rc) { taek_early_devres_liberar(&pdev->dev); return rc; }
    r->platform = INTEL_INFO(i915)->platform;
    r->subplatform = RUNTIME_INFO(i915)->platform_mask[0];
    r->step_gt = RUNTIME_INFO(i915)->step.graphics_step;
    r->pch_tipo = INTEL_PCH_TYPE(i915);
    r->pch_id = INTEL_PCH_ID(i915);
    r->gt_count = to_gt(i915)->i915 == i915 ? 1 : 0;
    r->mem_regiones_ok = i915->bdev.funcs ? 1 : 0;
    pr_info("[EARLY] early_probe OK: plataforma=%u pch=%u\n", r->platform, r->pch_tipo);

    int mmio_a = 0, mmio_b = 0;
    if (modo >= TAEK_EARLY_MMIO_LECTURA) {
        struct intel_uncore *u = to_gt(i915)->uncore;
        unsigned fallos = 0;
        /* Etapa A: la CPU no puede escribir el BAR: la pagina no tiene permiso. */
        fase(TAEK_EARLY_ETAPA_A);
        int solo_lectura = modo == TAEK_EARLY_GGTT_LECTURA;
        taek_early_mmio_modo(!solo_lectura && modo >= TAEK_EARLY_MMIO_FW, r->simulada);
        rc = taek_original_mmio_a(i915);
        mmio_a = 1;
        if (rc) { r->rc = rc; goto soltar; }
        pantalla_tomar(u);
        volcar(u, "solo lectura");
        if (pantalla_comparar(u)) fallos |= 64;
        if (solo_lectura) {
            /* Superficie del plano 1A en GGTT; alto*zancada: la zancada va en bloques de 64 bytes. */
            u32 tam = __raw_uncore_read32(u, _MMIO(0x70190)), zancada = __raw_uncore_read32(u, _MMIO(0x70188));
            rc = ggtt_inventario(pdev, __raw_uncore_read32(u, _MMIO(0x7019c)) & ~0xfffu, (((tam >> 16) & 0xfff) + 1) * (zancada & 0x3ff) * 64);
            if (rc) r->rc = rc;
            if (pantalla_comparar(u)) fallos |= 64;
            goto cierre;
        }
        /* QEMU no tiene GPU que responda a forcewake: la simulada va directa al probe de la GGTT. */
        if (modo >= TAEK_EARLY_GGTT_PROBE && r->simulada) {
            fase(TAEK_EARLY_ETAPA_E);
            rc = probar_ggtt(i915, pdev, u, &fallos, 1, modo == TAEK_EARLY_HW_PROBE ? 1 : 0);
            if (rc) r->rc = rc;
            goto cierre;
        }
        if (modo >= TAEK_EARLY_MMIO_FW && !rc) {
            fase(TAEK_EARLY_ETAPA_B);
            rc = taek_original_mmio_b(i915);
            mmio_b = 1;
            if (!rc) rc = probar_forcewake(i915, u, &fallos);
            if (!rc && pantalla_comparar(u)) { fallos |= 64; rc = -EIO; }
            if (rc) r->rc = rc;
        }
        if (modo >= TAEK_EARLY_MMIO_INFO && !rc) {
            fase(TAEK_EARLY_ETAPA_C);
            rc = taek_original_mmio_c(i915);
            if (rc) r->rc = rc;
            else {
                const struct sseu_dev_info *s = &to_gt(i915)->info.sseu;
                pr_info("[MMIO] motores=%llx vdbox=%u vebox=%u reloj_gt=%u\n", (unsigned long long)to_gt(i915)->info.engine_mask,
                        (unsigned)hweight32(VDBOX_MASK(to_gt(i915))), (unsigned)hweight32(VEBOX_MASK(to_gt(i915))), to_gt(i915)->clock_frequency);
                pr_info("[MMIO] slices=%x subslices=%u eus=%u rawclk=%u\n", (unsigned)s->slice_mask, (unsigned)s->max_subslices,
                        (unsigned)s->eu_total, RUNTIME_INFO(i915)->rawclk_freq);
                if (pantalla_comparar(u)) { fallos |= 64; r->rc = -EIO; }
            }
        }
        if (modo >= TAEK_EARLY_MMIO_RESET && !rc && !r->rc) {
            fase(TAEK_EARLY_ETAPA_D);
            if (!pantalla_viva(u, "antes del reset")) fallos |= 128;
            rc = probar_reset(i915, u, &fallos);
            if (rc) r->rc = rc;
            if (pantalla_comparar(u)) fallos |= 64;
            if (!pantalla_viva(u, "tras el reset")) fallos |= 2048;
            volcar(u, "tras el reset");
        }
        if (modo >= TAEK_EARLY_GGTT_PROBE && !rc && !r->rc) {
            fase(TAEK_EARLY_ETAPA_E);
            rc = probar_ggtt(i915, pdev, u, &fallos, 0, modo == TAEK_EARLY_GT ? 3 : modo == TAEK_EARLY_GEM_GGTT ? 2 : modo == TAEK_EARLY_HW_PROBE ? 1 : 0);
            if (rc) r->rc = rc;
            if (pantalla_comparar(u)) fallos |= 64;
            if (!pantalla_viva(u, "tras el probe de la GGTT")) fallos |= 2048;
        }
cierre:
        r->mmio_fallos = fallos;
        if (fallos && !r->rc) r->rc = -EIO;
    }
soltar:
    if (soltar) {
        fase(TAEK_EARLY_RELEASE);
        if (mmio_b) taek_original_mmio_release(i915);
        if (mmio_a) taek_original_mmio_fin(i915);
        taek_original_late_release(i915);
        fase(TAEK_EARLY_DEVRES);
        taek_early_devres_liberar(&pdev->dev);
    }
    fase(TAEK_EARLY_FIN);
    return r->rc;
}
void taek_early_resultado_leer(struct taek_early_resultado *o) { *o = *taek_early_resultado_interno(); }
extern void taek_early_bombear_tareas(void);
void taek_early_bombear(void) { taek_early_bombear_tareas(); }
