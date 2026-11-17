/* Entrada de Iris (Mesa) en TAEK, M27: constructores, pantalla y contexto Gallium sobre el DRM de TAEK, y la primera prueba de
 * render: dos clear_render_target con colores sacados del reloj sobre una textura de 256x256, leidos y comparados pixel a
 * pixel por la CPU. La CPU nunca escribe esos valores: si coinciden, los escribio la GPU (motor de render, via BLORP).
 * Se compila con las mismas cabeceras que el target DRI de Mesa (ver compilar_iris_taek.py). */
#include <stdint.h>
#include <stddef.h>
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include "util/box.h"
#include "util/u_inlines.h"
#include "util/u_sampler.h"
#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/xmlconfig.h"
#include "util/driconf.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "taek_iris.h"
#include "taek_video_abi.h"

extern int open(const char *, int, ...);
extern int snprintf(char *, size_t, const char *, ...);
extern void free(void *);
extern void *memset(void *, int, size_t);
struct pipe_screen *iris_drm_screen_create(int fd, const struct pipe_screen_config *config);

static const driOptionDescription iris_driconf[] = {
#include "iris/driinfo_iris.h"
};

typedef void (*ctor_t)(void);
__attribute__((section(".taek_ctors_ini"), used)) static ctor_t ctors_ini[1] = {0};
__attribute__((section(".taek_ctors_fin"), used)) static ctor_t ctors_fin[1] = {0};
static int ctors_hechos = -1;
static int correr_ctors(void) {
    if (ctors_hechos >= 0) return ctors_hechos;
    int n = 0;
    ctor_t *c = ctors_ini + 1, *fin = ctors_fin;
    __asm__ volatile("" : "+r"(c), "+r"(fin));   /* objetos distintos para el compilador: recorrer lo que hay entre ellos */
    for (; c != fin; c++) { (*c)(); n++; }
    ctors_hechos = n;
    return n;
}

static struct pipe_screen *pantalla;
static struct pipe_context *ctx;
static driOptionCache info, opciones;
static int fd = -1;

static int iniciar(char *t, size_t n) {
    if (ctx) return 0;
    if (fd < 0) fd = open("/dev/dri/renderD128", 2);
    if (fd < 0) { snprintf(t, n, "sin DRM"); return -1; }
    if (!pantalla) {
        driParseOptionInfo(&info, iris_driconf, sizeof(iris_driconf) / sizeof(iris_driconf[0]));
        struct pipe_screen_config cfg = {.options = &opciones, .options_info = &info};
        pantalla = iris_drm_screen_create(fd, &cfg);
        if (!pantalla) { snprintf(t, n, "iris_drm_screen_create fallo"); return -2; }
    }
    ctx = pantalla->context_create(pantalla, NULL, 0);
    if (!ctx) { snprintf(t, n, "context_create fallo (%s)", pantalla->get_name(pantalla)); return -3; }
    return 0;
}

/* Un clear y su lectura. Devuelve los pixeles distintos del color pedido (0 = todo bien). */
static unsigned clear_y_leer(struct pipe_resource *res, struct pipe_surface *sup, uint32_t bgra, uint64_t *ns) {
    union pipe_color_union c;
    c.f[0] = (float)((bgra >> 16) & 0xff) / 255.0f;   /* R */
    c.f[1] = (float)((bgra >> 8) & 0xff) / 255.0f;    /* G */
    c.f[2] = (float)(bgra & 0xff) / 255.0f;           /* B */
    c.f[3] = (float)(bgra >> 24) / 255.0f;            /* A */
    uint64_t t0 = taek_media_ns();
    ctx->clear_render_target(ctx, sup, &c, 0, 0, res->width0, res->height0, false);
    struct pipe_fence_handle *f = NULL;
    ctx->flush(ctx, &f, 0);
    pantalla->fence_finish(pantalla, NULL, f, UINT64_MAX);
    pantalla->fence_reference(pantalla, &f, NULL);
    *ns = taek_media_ns() - t0;
    struct pipe_box caja;
    u_box_2d(0, 0, res->width0, res->height0, &caja);
    struct pipe_transfer *tr = NULL;
    uint8_t *p = ctx->texture_map(ctx, res, 0, PIPE_MAP_READ, &caja, &tr);
    if (!p) return res->width0 * res->height0;
    unsigned mal = 0;
    for (unsigned y = 0; y < res->height0; y++) {
        const uint32_t *fila = (const uint32_t *)(p + (size_t)y * tr->stride);
        for (unsigned x = 0; x < res->width0; x++) mal += fila[x] != bgra;
    }
    ctx->texture_unmap(ctx, tr);
    return mal;
}

static int prueba_clear(char *t, size_t n) {
    int r = iniciar(t, n);
    if (r) return r;
    struct pipe_resource tmpl = {0};
    tmpl.target = PIPE_TEXTURE_2D;
    tmpl.format = PIPE_FORMAT_B8G8R8A8_UNORM;
    tmpl.width0 = 256; tmpl.height0 = 256; tmpl.depth0 = 1; tmpl.array_size = 1;
    tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
    tmpl.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *res = pantalla->resource_create(pantalla, &tmpl);
    if (!res) { snprintf(t, n, "resource_create fallo"); return -4; }
    struct pipe_surface st = {0};
    st.format = tmpl.format;
    struct pipe_surface *sup = ctx->create_surface(ctx, res, &st);
    if (!sup) { pipe_resource_reference(&res, NULL); snprintf(t, n, "create_surface fallo"); return -5; }
    /* Dos colores distintos del reloj; alfa opaco para no depender de la mezcla. */
    uint64_t s = taek_media_ns() * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t a = 0xff000000u | (uint32_t)(s >> 40), b = 0xff000000u | ((uint32_t)(s >> 16) & 0xffffff);
    if ((a & 0xffffff) == (b & 0xffffff)) b ^= 0x5a5a5a;
    uint64_t ns1 = 0, ns2 = 0;
    unsigned mal1 = clear_y_leer(res, sup, a, &ns1);
    unsigned mal2 = clear_y_leer(res, sup, b, &ns2);
    snprintf(t, n, "%s colorA=%08x malos=%u %lluus colorB=%08x malos=%u %lluus pixeles=%u",
             pantalla->get_name(pantalla), a, mal1, (unsigned long long)(ns1 / 1000), b, mal2, (unsigned long long)(ns2 / 1000),
             tmpl.width0 * tmpl.height0);
    ctx->surface_destroy(ctx, sup);
    pipe_resource_reference(&res, NULL);
    return mal1 || mal2 ? -6 : 0;
}

/* ------------------------------------------------------------------ M28: triangulo con shaders propios
 * VS: posicion y color de dos atributos (GENERIC0/1) a POS y VAR0. FS: VAR0 interpolado a DATA0. Armados con nir_builder y
 * con las convenciones de tgsi_to_nir (driver_location por orden), y las mismas pasadas finales antes de finalize_nir. */
static nir_variable *variable(nir_shader *s, nir_variable_mode m, int lugar, int orden, const char *nombre) {
    nir_variable *v = nir_variable_create(s, m, glsl_vec4_type(), nombre);
    v->data.location = lugar;
    v->data.driver_location = orden;
    if (m == nir_var_shader_in && s->info.stage == MESA_SHADER_FRAGMENT) v->data.interpolation = INTERP_MODE_SMOOTH;
    return v;
}
static void *crear_shader(int fs) {
    gl_shader_stage etapa = fs ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX;
    const nir_shader_compiler_options *o = pantalla->get_compiler_options(pantalla, fs ? PIPE_SHADER_FRAGMENT : PIPE_SHADER_VERTEX);
    nir_builder b = nir_builder_init_simple_shader(etapa, o, fs ? "taek_fs" : "taek_vs");
    nir_shader *s = b.shader;
    if (!fs) {
        nir_variable *pos = variable(s, nir_var_shader_in, VERT_ATTRIB_GENERIC0, 0, "pos");
        nir_variable *col = variable(s, nir_var_shader_in, VERT_ATTRIB_GENERIC1, 1, "col");
        nir_variable *opos = variable(s, nir_var_shader_out, VARYING_SLOT_POS, 0, "gl_Position");
        nir_variable *ocol = variable(s, nir_var_shader_out, VARYING_SLOT_VAR0, 1, "v_col");
        nir_store_var(&b, opos, nir_load_var(&b, pos), 0xf);
        nir_store_var(&b, ocol, nir_load_var(&b, col), 0xf);
        s->num_inputs = 2; s->num_outputs = 2;
    } else {
        nir_variable *col = variable(s, nir_var_shader_in, VARYING_SLOT_VAR0, 0, "v_col");
        nir_variable *sal = variable(s, nir_var_shader_out, FRAG_RESULT_DATA0, 0, "color");
        nir_store_var(&b, sal, nir_load_var(&b, col), 0xf);
        s->num_inputs = 1; s->num_outputs = 1;
    }
    NIR_PASS(_, s, nir_lower_vars_to_ssa);
    NIR_PASS(_, s, nir_lower_global_vars_to_local);
    NIR_PASS(_, s, nir_split_var_copies);
    NIR_PASS(_, s, nir_lower_var_copies);
    NIR_PASS(_, s, nir_lower_system_values);
    nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
    if (pantalla->finalize_nir) free(pantalla->finalize_nir(pantalla, s));
    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = s;
    return fs ? ctx->create_fs_state(ctx, &st) : ctx->create_vs_state(ctx, &st);
}

static uint64_t azar;
static float al_azar(void) { azar = azar * 6364136223846793005ull + 1442695040888963407ull; return (float)((azar >> 40) & 0xff) / 255.0f; }
static unsigned a_byte(float v) { if (v < 0) v = 0; if (v > 1) v = 1; return (unsigned)(v * 255.0f + 0.5f); }

static int prueba_triangulo(char *t, size_t n) {
    int r = iniciar(t, n);
    if (r) return r;
    enum { L = 256 };
    azar = taek_media_ns() | 1;
    /* Vertices en pixeles de ventana (centros en .5): con cuartos de pixel ningun borde pasa justo por un centro salvo
     * casualidad, y esos se cuentan aparte como dudosos. */
    const float v[3][2] = {{40.25f, 30.375f}, {215.625f, 70.125f}, {100.375f, 225.875f}};
    float c[3][4], fondo[4] = {al_azar(), al_azar(), al_azar(), 1.0f};
    for (int i = 0; i < 3; i++) { c[i][0] = al_azar(); c[i][1] = al_azar(); c[i][2] = al_azar(); c[i][3] = 1.0f; }
    float datos[3][8];
    for (int i = 0; i < 3; i++) {   /* viewport: escala L/2 y traslacion L/2, sin invertir y */
        datos[i][0] = (v[i][0] - L / 2.0f) / (L / 2.0f); datos[i][1] = (v[i][1] - L / 2.0f) / (L / 2.0f);
        datos[i][2] = 0.0f; datos[i][3] = 1.0f;
        for (int k = 0; k < 4; k++) datos[i][4 + k] = c[i][k];
    }

    struct pipe_resource tmpl = {0};
    tmpl.target = PIPE_TEXTURE_2D; tmpl.format = PIPE_FORMAT_B8G8R8A8_UNORM;
    tmpl.width0 = L; tmpl.height0 = L; tmpl.depth0 = 1; tmpl.array_size = 1;
    tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW; tmpl.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *rt = pantalla->resource_create(pantalla, &tmpl);
    struct pipe_resource bt = {0};
    bt.target = PIPE_BUFFER; bt.format = PIPE_FORMAT_R8_UNORM; bt.width0 = sizeof(datos); bt.height0 = 1; bt.depth0 = 1; bt.array_size = 1;
    bt.bind = PIPE_BIND_VERTEX_BUFFER; bt.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *vb = pantalla->resource_create(pantalla, &bt);
    if (!rt || !vb) { snprintf(t, n, "resource_create fallo"); return -4; }
    ctx->buffer_subdata(ctx, vb, PIPE_MAP_WRITE, 0, sizeof(datos), datos);

    uint64_t t0 = taek_media_ns();
    void *vs = crear_shader(0), *fs = crear_shader(1);
    uint64_t t_sh = taek_media_ns() - t0;
    if (!vs || !fs) { snprintf(t, n, "shaders: vs=%p fs=%p", vs, fs); return -7; }

    struct pipe_blend_state bl = {0};
    bl.rt[0].colormask = PIPE_MASK_RGBA;
    struct pipe_rasterizer_state ra = {0};
    ra.half_pixel_center = 1; ra.depth_clip_near = 1; ra.depth_clip_far = 1; ra.front_ccw = 1;
    ra.cull_face = PIPE_FACE_NONE; ra.fill_front = PIPE_POLYGON_MODE_FILL; ra.fill_back = PIPE_POLYGON_MODE_FILL;
    ra.line_width = 1.0f; ra.point_size = 1.0f;
    struct pipe_depth_stencil_alpha_state dsa = {0};
    struct pipe_vertex_element ve[2] = {0};
    ve[0].src_offset = 0;  ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[0].src_stride = 32;
    ve[1].src_offset = 16; ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[1].src_stride = 32;
    void *cso_bl = ctx->create_blend_state(ctx, &bl), *cso_ra = ctx->create_rasterizer_state(ctx, &ra);
    void *cso_dsa = ctx->create_depth_stencil_alpha_state(ctx, &dsa), *cso_ve = ctx->create_vertex_elements_state(ctx, 2, ve);
    ctx->bind_blend_state(ctx, cso_bl); ctx->bind_rasterizer_state(ctx, cso_ra);
    ctx->bind_depth_stencil_alpha_state(ctx, cso_dsa); ctx->bind_vertex_elements_state(ctx, cso_ve);
    ctx->bind_vs_state(ctx, vs); ctx->bind_fs_state(ctx, fs);
    struct pipe_vertex_buffer vbuf = {0};
    pipe_resource_reference(&vbuf.buffer.resource, vb);   /* set_vertex_buffers se queda con esta referencia */
    ctx->set_vertex_buffers(ctx, 1, &vbuf);
    struct pipe_framebuffer_state fb = {0};
    fb.width = L; fb.height = L; fb.layers = 1; fb.nr_cbufs = 1;
    fb.cbufs[0].format = tmpl.format; fb.cbufs[0].texture = rt;
    ctx->set_framebuffer_state(ctx, &fb);
    struct pipe_viewport_state vp = {{L / 2.0f, L / 2.0f, 0.5f}, {L / 2.0f, L / 2.0f, 0.5f}};
    ctx->set_viewport_states(ctx, 0, 1, &vp);
    ctx->set_sample_mask(ctx, ~0u);

    union pipe_color_union cf;
    for (int k = 0; k < 4; k++) cf.f[k] = fondo[k];
    t0 = taek_media_ns();
    ctx->clear(ctx, PIPE_CLEAR_COLOR0, NULL, &cf, 0.0, 0);
    struct pipe_draw_info di = {0};
    di.mode = MESA_PRIM_TRIANGLES; di.instance_count = 1;
    struct pipe_draw_start_count_bias dr = {0, 3, 0};
    ctx->draw_vbo(ctx, &di, 0, NULL, &dr, 1);
    struct pipe_fence_handle *f = NULL;
    ctx->flush(ctx, &f, 0);
    pantalla->fence_finish(pantalla, NULL, f, UINT64_MAX);
    pantalla->fence_reference(pantalla, &f, NULL);
    uint64_t t_gpu = taek_media_ns() - t0;

    /* Referencia independiente en la CPU: funciones de borde en el centro de cada pixel y baricentricas. */
    struct pipe_box caja;
    u_box_2d(0, 0, L, L, &caja);
    struct pipe_transfer *tr = NULL;
    uint8_t *p = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &caja, &tr);
    unsigned cubiertos = 0, fuera_mal = 0, dentro_mal = 0, dudosos = 0, dif_max = 0;
    uint32_t bfondo = 0xff000000u | a_byte(fondo[0]) << 16 | a_byte(fondo[1]) << 8 | a_byte(fondo[2]);
    float area = (v[1][0] - v[0][0]) * (v[2][1] - v[0][1]) - (v[1][1] - v[0][1]) * (v[2][0] - v[0][0]);
    for (int y = 0; p && y < L; y++) {
        const uint32_t *fila = (const uint32_t *)(p + (size_t)y * tr->stride);
        for (int x = 0; x < L; x++) {
            float px = x + 0.5f, py = y + 0.5f, e[3];
            int duda = 0;
            for (int i = 0; i < 3; i++) {
                const float *a = v[(i + 1) % 3], *b = v[(i + 2) % 3];
                e[i] = ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) / area;   /* baricentrica del vertice i */
                float largo = __builtin_sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
                if (__builtin_fabsf(e[i] * area / largo) < 0.01f) duda = 1;
            }
            if (duda) { dudosos++; continue; }
            int dentro = e[0] > 0 && e[1] > 0 && e[2] > 0;
            uint32_t got = fila[x];
            if (!dentro) { fuera_mal += got != bfondo; continue; }
            cubiertos++;
            unsigned peor = 0;
            for (int k = 0; k < 3; k++) {
                unsigned esp = a_byte(e[0] * c[0][k] + e[1] * c[1][k] + e[2] * c[2][k]);
                unsigned obt = (got >> (16 - 8 * k)) & 0xff;   /* R en 16, G en 8, B en 0 */
                unsigned d = esp > obt ? esp - obt : obt - esp;
                if (d > peor) peor = d;
            }
            if (peor > dif_max) dif_max = peor;
            dentro_mal += peor > 2 || (got >> 24) != 0xff;
        }
    }
    if (p) ctx->texture_unmap(ctx, tr);
    snprintf(t, n, "triangulo %s cubiertos=%u dentro_mal=%u fuera_mal=%u dudosos=%u dif_max=%u shaders=%lluus gpu=%lluus mapa=%s",
             pantalla->get_name(pantalla), cubiertos, dentro_mal, fuera_mal, dudosos, dif_max,
             (unsigned long long)(t_sh / 1000), (unsigned long long)(t_gpu / 1000), p ? "si" : "no");

    ctx->set_vertex_buffers(ctx, 0, NULL);
    struct pipe_framebuffer_state vacio = {0};
    ctx->set_framebuffer_state(ctx, &vacio);
    ctx->bind_vs_state(ctx, NULL); ctx->bind_fs_state(ctx, NULL);
    ctx->delete_vs_state(ctx, vs); ctx->delete_fs_state(ctx, fs);
    ctx->bind_blend_state(ctx, NULL); ctx->bind_rasterizer_state(ctx, NULL);
    ctx->bind_depth_stencil_alpha_state(ctx, NULL); ctx->bind_vertex_elements_state(ctx, NULL);
    ctx->delete_blend_state(ctx, cso_bl); ctx->delete_rasterizer_state(ctx, cso_ra);
    ctx->delete_depth_stencil_alpha_state(ctx, cso_dsa); ctx->delete_vertex_elements_state(ctx, cso_ve);
    pipe_resource_reference(&vb, NULL);
    pipe_resource_reference(&rt, NULL);
    return !p ? -8 : (dentro_mal || fuera_mal || !cubiertos) ? -6 : 0;
}

/* ------------------------------------------------------------------ M28: textura y presentacion
 * FS: muestrea un sampler 2D (binding 0) en la coordenada que trae VAR0. */
static void *crear_fs_textura(void) {
    const nir_shader_compiler_options *o = pantalla->get_compiler_options(pantalla, PIPE_SHADER_FRAGMENT);
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, o, "taek_fs_tex");
    nir_shader *s = b.shader;
    nir_variable *tc = variable(s, nir_var_shader_in, VARYING_SLOT_VAR0, 0, "v_tc");
    nir_variable *sal = variable(s, nir_var_shader_out, FRAG_RESULT_DATA0, 0, "color");
    nir_variable *smp = nir_variable_create(s, nir_var_uniform, glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT), "tex");
    smp->data.binding = 0; smp->data.explicit_binding = true; smp->data.driver_location = 0;
    nir_deref_instr *d = nir_build_deref_var(&b, smp);
    nir_def *uv = nir_channels(&b, nir_load_var(&b, tc), 0x3);
    nir_store_var(&b, sal, nir_tex_deref(&b, d, d, uv), 0xf);
    s->num_inputs = 1; s->num_outputs = 1;
    NIR_PASS(_, s, nir_lower_vars_to_ssa);
    NIR_PASS(_, s, nir_lower_global_vars_to_local);
    NIR_PASS(_, s, nir_split_var_copies);
    NIR_PASS(_, s, nir_lower_var_copies);
    NIR_PASS(_, s, nir_lower_system_values);
    /* Sin GLSL no hay enlazador que numere las texturas: se bajan siempre los derefs a indices (texture_index 0) para que
     * gather_info marque textures_used y el driver ponga la textura en su tabla de bindings. Sin esto se muestrea "nada" (0). */
    NIR_PASS(_, s, nir_lower_samplers);
    s->info.num_textures = 1;
    nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
    /* gather_info no los calcula: los pone el enlazador de GLSL o tgsi_to_nir (tgsi_to_nir.c:1110-1113). Iris arma con ellos
     * la tabla de bindings y los estados de sampler (iris_program.c:1423, iris_state.c:2669). */
    BITSET_SET(s->info.textures_used, 0);
    BITSET_SET(s->info.samplers_used, 0);
    if (pantalla->finalize_nir) free(pantalla->finalize_nir(pantalla, s));
    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = s;
    return ctx->create_fs_state(ctx, &st);
}

/* Cuadrado (tira de 4 vertices) girado 'ang' radianes y escalado 'esc' alrededor del centro; coordenadas de textura en VAR0. */
static void cuadrado(float (*d)[8], float ang, float esc) {
    static const float esq[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    float c = __builtin_cosf(ang), s = __builtin_sinf(ang);
    for (int i = 0; i < 4; i++) {
        float x = esq[i][0] * esc, y = esq[i][1] * esc;
        d[i][0] = x * c - y * s; d[i][1] = x * s + y * c; d[i][2] = 0; d[i][3] = 1;
        d[i][4] = (esq[i][0] + 1) * 0.5f; d[i][5] = (esq[i][1] + 1) * 0.5f; d[i][6] = 0; d[i][7] = 1;
    }
}

static int prueba_textura(char *t, size_t n) {
    int r = iniciar(t, n);
    if (r) return r;
    enum { L = 256, T = 64, CUADROS = 240 };
    azar = taek_media_ns() | 1;
    /* Textura de origen: cada texel al azar, alfa 255. La CPU la escribe; el destino solo puede salir de muestrearla. */
    static uint32_t src[T * T];
    for (int i = 0; i < T * T; i++) src[i] = 0xff000000u | a_byte(al_azar()) << 16 | a_byte(al_azar()) << 8 | a_byte(al_azar());
    struct pipe_resource tt = {0};
    tt.target = PIPE_TEXTURE_2D; tt.format = PIPE_FORMAT_B8G8R8A8_UNORM; tt.width0 = T; tt.height0 = T; tt.depth0 = 1; tt.array_size = 1;
    tt.bind = PIPE_BIND_SAMPLER_VIEW; tt.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource tmpl = tt;
    tmpl.width0 = L; tmpl.height0 = L; tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
    struct pipe_resource bt = {0};
    bt.target = PIPE_BUFFER; bt.format = PIPE_FORMAT_R8_UNORM; bt.width0 = 4 * 8 * sizeof(float); bt.height0 = 1; bt.depth0 = 1; bt.array_size = 1;
    bt.bind = PIPE_BIND_VERTEX_BUFFER; bt.usage = PIPE_USAGE_STREAM;
    struct pipe_resource *tex = pantalla->resource_create(pantalla, &tt), *rt = pantalla->resource_create(pantalla, &tmpl);
    struct pipe_resource *vb = pantalla->resource_create(pantalla, &bt);
    if (!tex || !rt || !vb) { snprintf(t, n, "resource_create fallo"); return -4; }
    struct pipe_box caja;
    u_box_2d(0, 0, T, T, &caja);
    ctx->texture_subdata(ctx, tex, 0, PIPE_MAP_WRITE, &caja, src, T * 4, T * T * 4);

    uint64_t t0 = taek_media_ns();
    void *vs = crear_shader(0), *fs = crear_fs_textura();
    uint64_t t_sh = taek_media_ns() - t0;
    if (!vs || !fs) { snprintf(t, n, "shaders: vs=%p fs=%p", vs, fs); return -7; }

    struct pipe_sampler_view vt;
    memset(&vt, 0, sizeof(vt));
    u_sampler_view_default_template(&vt, tex, tex->format);
    struct pipe_sampler_view *vista = ctx->create_sampler_view(ctx, tex, &vt);
    struct pipe_sampler_state ss = {0};
    ss.wrap_s = ss.wrap_t = ss.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
    ss.min_img_filter = ss.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
    ss.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
    void *muestreo = ctx->create_sampler_state(ctx, &ss);
    struct pipe_blend_state bl = {0};
    bl.rt[0].colormask = PIPE_MASK_RGBA;
    struct pipe_rasterizer_state ra = {0};
    ra.half_pixel_center = 1; ra.depth_clip_near = 1; ra.depth_clip_far = 1; ra.front_ccw = 1;
    ra.cull_face = PIPE_FACE_NONE; ra.line_width = 1.0f; ra.point_size = 1.0f;
    struct pipe_depth_stencil_alpha_state dsa = {0};
    struct pipe_vertex_element ve[2] = {0};
    ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[0].src_stride = 32;
    ve[1].src_offset = 16; ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[1].src_stride = 32;
    void *cso_bl = ctx->create_blend_state(ctx, &bl), *cso_ra = ctx->create_rasterizer_state(ctx, &ra);
    void *cso_dsa = ctx->create_depth_stencil_alpha_state(ctx, &dsa), *cso_ve = ctx->create_vertex_elements_state(ctx, 2, ve);
    ctx->bind_blend_state(ctx, cso_bl); ctx->bind_rasterizer_state(ctx, cso_ra);
    ctx->bind_depth_stencil_alpha_state(ctx, cso_dsa); ctx->bind_vertex_elements_state(ctx, cso_ve);
    ctx->bind_vs_state(ctx, vs); ctx->bind_fs_state(ctx, fs);
    ctx->set_sampler_views(ctx, PIPE_SHADER_FRAGMENT, 0, 1, 0, &vista);
    ctx->bind_sampler_states(ctx, PIPE_SHADER_FRAGMENT, 0, 1, &muestreo);
    struct pipe_vertex_buffer vbuf = {0};
    pipe_resource_reference(&vbuf.buffer.resource, vb);
    ctx->set_vertex_buffers(ctx, 1, &vbuf);
    struct pipe_framebuffer_state fb = {0};
    fb.width = L; fb.height = L; fb.layers = 1; fb.nr_cbufs = 1;
    fb.cbufs[0].format = tmpl.format; fb.cbufs[0].texture = rt;
    ctx->set_framebuffer_state(ctx, &fb);
    struct pipe_viewport_state vp = {{L / 2.0f, L / 2.0f, 0.5f}, {L / 2.0f, L / 2.0f, 0.5f}};
    ctx->set_viewport_states(ctx, 0, 1, &vp);
    ctx->set_sample_mask(ctx, ~0u);

    union pipe_color_union fondo = {.f = {0.04f, 0.05f, 0.08f, 1.0f}};
    /* Diagnostico por pasos (H192): origen subido, y destino tras solo el clear. */
    {
        char d[200];
        struct pipe_transfer *tr = NULL;
        u_box_2d(0, 0, T, T, &caja);
        const uint32_t *q = ctx->texture_map(ctx, tex, 0, PIPE_MAP_READ, &caja, &tr);
        unsigned bien = 0;
        if (q) { for (int y = 0; y < T; y++) for (int x = 0; x < T; x++) bien += ((const uint32_t *)((const uint8_t *)q + (size_t)y * tr->stride))[x] == src[y * T + x]; ctx->texture_unmap(ctx, tr); }
        ctx->clear(ctx, PIPE_CLEAR_COLOR0, NULL, &fondo, 0.0, 0);
        struct pipe_fence_handle *f = NULL;
        ctx->flush(ctx, &f, 0);
        pantalla->fence_finish(pantalla, NULL, f, UINT64_MAX);
        pantalla->fence_reference(pantalla, &f, NULL);
        u_box_2d(0, 0, L, L, &caja);
        const uint32_t *w = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &caja, &tr);
        uint32_t w0 = w ? w[0] : 0xdeadbeef;
        if (w) ctx->texture_unmap(ctx, tr);
        snprintf(d, sizeof(d), "iris textura: origen_bien=%u/%u mapa_origen=%s tras_clear(0,0)=%08x",
                 bien, T * T, q ? "si" : "no", w0);
        taek_media_log(d);
    }
    struct pipe_draw_info di = {0};
    di.mode = MESA_PRIM_TRIANGLE_STRIP; di.instance_count = 1;
    struct pipe_draw_start_count_bias dr = {0, 4, 0};
    u_box_2d(0, 0, L, L, &caja);
    unsigned malos = 0, presentados = 0, sin_pantalla = 0;
    uint64_t ns_gpu = 0, ns_mapa = 0, ns_pant = 0, t_ini = taek_media_ns();
    for (int k = 0; k < CUADROS; k++) {
        float v[4][8];
        /* Cuadro 0: sin girar y a pantalla completa, para verificar. Despues gira una vuelta cada 120 cuadros. */
        cuadrado(v, k ? (float)k * 6.2831853f / 120.0f : 0.0f, k ? 0.68f : 1.0f);
        ctx->buffer_subdata(ctx, vb, PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, 0, sizeof(v), v);
        uint64_t a = taek_media_ns();
        ctx->clear(ctx, PIPE_CLEAR_COLOR0, NULL, &fondo, 0.0, 0);
        ctx->draw_vbo(ctx, &di, 0, NULL, &dr, 1);
        struct pipe_fence_handle *f = NULL;
        ctx->flush(ctx, &f, 0);
        pantalla->fence_finish(pantalla, NULL, f, UINT64_MAX);
        pantalla->fence_reference(pantalla, &f, NULL);
        uint64_t b = taek_media_ns();
        struct pipe_transfer *tr = NULL;
        uint8_t *p = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &caja, &tr);
        uint64_t c = taek_media_ns();
        if (!p) { malos = L * L; break; }
        if (k == 0) {
            /* Diagnostico ante un fallo sistematico: orientacion, canales o textura sin muestrear. */
            unsigned vflip = 0, hflip = 0, fondo_n = 0, ceros = 0, sin_rb = 0;
            for (int y = 0; y < L; y++) {
                const uint32_t *fila = (const uint32_t *)(p + (size_t)y * tr->stride);
                for (int x = 0; x < L; x++) {
                    uint32_t g = fila[x], e = src[(y / 4) * T + x / 4];
                    malos += g != e;
                    vflip += g == src[((L - 1 - y) / 4) * T + x / 4];
                    hflip += g == src[(y / 4) * T + (L - 1 - x) / 4];
                    ceros += g == 0;
                    fondo_n += (g & 0xffffff) == (0x0a0d14u & 0xffffff);
                    sin_rb += g == ((e & 0xff00ff00u) | (e >> 16 & 0xff) | (e & 0xff) << 16);
                }
            }
            const uint32_t *f0 = (const uint32_t *)p, *fu = (const uint32_t *)(p + (size_t)(L - 1) * tr->stride);
            taek_media_log("iris textura: diagnostico del cuadro 0");
            char d[200];
            snprintf(d, sizeof(d), "obt(0,0)=%08x esp=%08x obt(255,255)=%08x esp=%08x stride=%u vflip=%u hflip=%u rb=%u ceros=%u fondo~=%u",
                     f0[0], src[0], fu[L - 1], src[T * T - 1], tr->stride, vflip, hflip, sin_rb, ceros, fondo_n);
            taek_media_log(d);
        }
        if (taek_media_pantalla(p, L, L, tr->stride, -17, 16) == 0) presentados++; else sin_pantalla++;
        uint64_t e = taek_media_ns();
        ctx->texture_unmap(ctx, tr);
        ns_gpu += b - a; ns_mapa += c - b; ns_pant += e - c;
    }
    uint64_t total = taek_media_ns() - t_ini;
    snprintf(t, n, "textura %ux%u->%ux%u malos=%u cuadros=%u presentados=%u sin_pantalla=%u ms_total=%llu "
             "por_cuadro_us=%llu gpu_us=%llu mapa_us=%llu pantalla_us=%llu shaders_us=%llu",
             T, T, L, L, malos, CUADROS, presentados, sin_pantalla, (unsigned long long)(total / 1000000),
             (unsigned long long)(total / CUADROS / 1000), (unsigned long long)(ns_gpu / CUADROS / 1000),
             (unsigned long long)(ns_mapa / CUADROS / 1000), (unsigned long long)(ns_pant / CUADROS / 1000),
             (unsigned long long)(t_sh / 1000));

    ctx->set_vertex_buffers(ctx, 0, NULL);
    ctx->set_sampler_views(ctx, PIPE_SHADER_FRAGMENT, 0, 0, 1, NULL);
    struct pipe_framebuffer_state vacio = {0};
    ctx->set_framebuffer_state(ctx, &vacio);
    ctx->bind_vs_state(ctx, NULL); ctx->bind_fs_state(ctx, NULL);
    ctx->delete_vs_state(ctx, vs); ctx->delete_fs_state(ctx, fs);
    ctx->delete_sampler_state(ctx, muestreo);
    pipe_sampler_view_reference(&vista, NULL);
    ctx->bind_blend_state(ctx, NULL); ctx->bind_rasterizer_state(ctx, NULL);
    ctx->bind_depth_stencil_alpha_state(ctx, NULL); ctx->bind_vertex_elements_state(ctx, NULL);
    ctx->delete_blend_state(ctx, cso_bl); ctx->delete_rasterizer_state(ctx, cso_ra);
    ctx->delete_depth_stencil_alpha_state(ctx, cso_dsa); ctx->delete_vertex_elements_state(ctx, cso_ve);
    pipe_resource_reference(&vb, NULL);
    pipe_resource_reference(&rt, NULL);
    pipe_resource_reference(&tex, NULL);
    return malos ? -6 : 0;
}

/* ------------------------------------------------------------------ H193: presentacion sin copia
 * El plano de pantalla escanea directamente la superficie de Iris: lo que dibuja la GPU es lo que se ve, sin lectura ni copia
 * de la CPU. Un triangulo de colores gira ~4 s sobre un fondo fijo; luego el plano vuelve a la superficie del firmware. */
static int prueba_pantalla(char *t, size_t n) {
    int r = iniciar(t, n);
    if (r) return r;
    unsigned an = 0, al = 0;
    r = taek_media_plano_info(&an, &al);
    if (r) { snprintf(t, n, "plano de pantalla no apto (%d)", r); return -10; }
    struct pipe_resource tmpl = {0};
    tmpl.target = PIPE_TEXTURE_2D; tmpl.format = PIPE_FORMAT_B8G8R8A8_UNORM;
    tmpl.width0 = an; tmpl.height0 = al; tmpl.depth0 = 1; tmpl.array_size = 1;
    tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_LINEAR | PIPE_BIND_SHARED;
    tmpl.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *rt = pantalla->resource_create(pantalla, &tmpl);
    struct pipe_resource bt = {0};
    bt.target = PIPE_BUFFER; bt.format = PIPE_FORMAT_R8_UNORM; bt.width0 = 3 * 8 * sizeof(float); bt.height0 = 1; bt.depth0 = 1; bt.array_size = 1;
    bt.bind = PIPE_BIND_VERTEX_BUFFER; bt.usage = PIPE_USAGE_STREAM;
    struct pipe_resource *vb = pantalla->resource_create(pantalla, &bt);
    if (!rt || !vb) { snprintf(t, n, "resource_create fallo (%ux%u)", an, al); return -4; }
    struct winsys_handle wh;
    memset(&wh, 0, sizeof(wh));
    wh.type = WINSYS_HANDLE_TYPE_KMS;
    if (!pantalla->resource_get_handle(pantalla, ctx, rt, &wh, 0)) { snprintf(t, n, "resource_get_handle fallo"); return -11; }

    void *vs = crear_shader(0), *fs = crear_shader(1);
    if (!vs || !fs) { snprintf(t, n, "shaders: vs=%p fs=%p", vs, fs); return -7; }
    struct pipe_blend_state bl = {0};
    bl.rt[0].colormask = PIPE_MASK_RGBA;
    struct pipe_rasterizer_state ra = {0};
    ra.half_pixel_center = 1; ra.depth_clip_near = 1; ra.depth_clip_far = 1; ra.front_ccw = 1;
    ra.cull_face = PIPE_FACE_NONE; ra.line_width = 1.0f; ra.point_size = 1.0f;
    struct pipe_depth_stencil_alpha_state dsa = {0};
    struct pipe_vertex_element ve[2] = {0};
    ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[0].src_stride = 32;
    ve[1].src_offset = 16; ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[1].src_stride = 32;
    void *cso_bl = ctx->create_blend_state(ctx, &bl), *cso_ra = ctx->create_rasterizer_state(ctx, &ra);
    void *cso_dsa = ctx->create_depth_stencil_alpha_state(ctx, &dsa), *cso_ve = ctx->create_vertex_elements_state(ctx, 2, ve);
    ctx->bind_blend_state(ctx, cso_bl); ctx->bind_rasterizer_state(ctx, cso_ra);
    ctx->bind_depth_stencil_alpha_state(ctx, cso_dsa); ctx->bind_vertex_elements_state(ctx, cso_ve);
    ctx->bind_vs_state(ctx, vs); ctx->bind_fs_state(ctx, fs);
    struct pipe_vertex_buffer vbuf = {0};
    pipe_resource_reference(&vbuf.buffer.resource, vb);
    ctx->set_vertex_buffers(ctx, 1, &vbuf);
    struct pipe_framebuffer_state fb = {0};
    fb.width = an; fb.height = al; fb.layers = 1; fb.nr_cbufs = 1;
    fb.cbufs[0].format = tmpl.format; fb.cbufs[0].texture = rt;
    ctx->set_framebuffer_state(ctx, &fb);
    struct pipe_viewport_state vp = {{an / 2.0f, al / 2.0f, 0.5f}, {an / 2.0f, al / 2.0f, 0.5f}};
    ctx->set_viewport_states(ctx, 0, 1, &vp);
    ctx->set_sample_mask(ctx, ~0u);

    azar = taek_media_ns() | 1;
    float col[3][3];
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) col[i][k] = al_azar();
    union pipe_color_union fondo = {.f = {0.04f, 0.05f, 0.08f, 1.0f}};
    uint32_t bfondo = 0xff000000u | a_byte(fondo.f[0]) << 16 | a_byte(fondo.f[1]) << 8 | a_byte(fondo.f[2]);
    struct pipe_draw_info di = {0};
    di.mode = MESA_PRIM_TRIANGLES; di.instance_count = 1;
    struct pipe_draw_start_count_bias dr = {0, 3, 0};
    float aspecto = (float)al / (float)an;
    struct pipe_fence_handle *previa = NULL;
    unsigned cuadros = 0, ggtt = 0;
    int mostrado = -1;
    uint64_t t_ini = 0, t_fin = 0;
    for (;;) {
        float ang = (float)cuadros * 0.02f, d[3][8];
        for (int i = 0; i < 3; i++) {
            float a = ang + (float)i * 2.0943951f;   /* 120 grados */
            d[i][0] = 0.75f * __builtin_cosf(a) * aspecto; d[i][1] = 0.75f * __builtin_sinf(a); d[i][2] = 0; d[i][3] = 1;
            d[i][4] = col[i][0]; d[i][5] = col[i][1]; d[i][6] = col[i][2]; d[i][7] = 1;
        }
        ctx->buffer_subdata(ctx, vb, PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, 0, sizeof(d), d);
        ctx->clear(ctx, PIPE_CLEAR_COLOR0, NULL, &fondo, 0.0, 0);
        ctx->draw_vbo(ctx, &di, 0, NULL, &dr, 1);
        struct pipe_fence_handle *f = NULL;
        ctx->flush(ctx, &f, 0);
        /* Un cuadro en vuelo como mucho: se espera el anterior. */
        if (previa) { pantalla->fence_finish(pantalla, NULL, previa, UINT64_MAX); pantalla->fence_reference(pantalla, &previa, NULL); }
        previa = f;
        cuadros++;
        if (mostrado < 0) {   /* el primer cuadro ya esta: recien ahora el plano pasa a esta superficie */
            pantalla->fence_finish(pantalla, NULL, previa, UINT64_MAX);
            mostrado = taek_media_plano_mostrar(wh.handle, wh.stride, &ggtt);
            if (mostrado) break;
            t_ini = taek_media_ns();
        }
        t_fin = taek_media_ns();
        if (t_fin - t_ini >= 4000000000ull || cuadros >= 2400) break;
    }
    if (previa) { pantalla->fence_finish(pantalla, NULL, previa, UINT64_MAX); pantalla->fence_reference(pantalla, &previa, NULL); }
    /* La esquina (0,0) queda fuera del triangulo: debe ser el fondo, leido de la misma superficie que escaneaba el plano. */
    uint32_t esquina = 0;
    if (!mostrado) {
        struct pipe_box caja;
        u_box_2d(0, 0, 1, 1, &caja);
        struct pipe_transfer *tr = NULL;
        const uint32_t *p = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &caja, &tr);
        if (p) { esquina = p[0]; ctx->texture_unmap(ctx, tr); }
    }
    int restaurado = mostrado ? -99 : taek_media_plano_restaurar();
    uint64_t ms = (t_fin - t_ini) / 1000000;
    snprintf(t, n, "pantalla %ux%u paso=%u ggtt=%08x mostrar=%d cuadros=%u ms=%llu fps=%llu esquina=%08x esperada=%08x restaurar=%d",
             an, al, wh.stride, ggtt, mostrado, cuadros, (unsigned long long)ms,
             (unsigned long long)(ms ? cuadros * 1000ull / ms : 0), esquina, bfondo, restaurado);

    ctx->set_vertex_buffers(ctx, 0, NULL);
    struct pipe_framebuffer_state vacio = {0};
    ctx->set_framebuffer_state(ctx, &vacio);
    ctx->bind_vs_state(ctx, NULL); ctx->bind_fs_state(ctx, NULL);
    ctx->delete_vs_state(ctx, vs); ctx->delete_fs_state(ctx, fs);
    ctx->bind_blend_state(ctx, NULL); ctx->bind_rasterizer_state(ctx, NULL);
    ctx->bind_depth_stencil_alpha_state(ctx, NULL); ctx->bind_vertex_elements_state(ctx, NULL);
    ctx->delete_blend_state(ctx, cso_bl); ctx->delete_rasterizer_state(ctx, cso_ra);
    ctx->delete_depth_stencil_alpha_state(ctx, cso_dsa); ctx->delete_vertex_elements_state(ctx, cso_ve);
    pipe_resource_reference(&vb, NULL);
    pipe_resource_reference(&rt, NULL);
    return mostrado ? -12 : restaurado ? -13 : esquina != bfondo ? -6 : 0;
}

/* ------------------------------------------------------------------ H194: video NV12 de Media Driver, sin copia
 * Coeficientes como el reproductor del kernel (h264/imagen.c): matriz VUI 1 = BT.709, 9 = BT.2020, otra = BT.601. */
struct coef { float rv, gu, gv, bu, ky, oy, kc; };
static struct coef coeficientes(int matriz, int completo) {
    struct coef c = {1.402f, 0.344136f, 0.714136f, 1.772f, 1, 0, 1};
    if (matriz == 1) { c.rv = 1.5748f; c.gu = 0.187324f; c.gv = 0.468124f; c.bu = 1.8556f; }
    if (matriz == 9) { c.rv = 1.4746f; c.gu = 0.164553f; c.gv = 0.571353f; c.bu = 1.8814f; }
    if (completo) { c.ky = 1; c.oy = 0; c.kc = 1; }
    else { c.ky = 255.0f / 219.0f; c.oy = -16.0f / 219.0f; c.kc = 255.0f / 224.0f; }
    return c;
}
extern int taek_iris_dmabuf(uint32_t handle, uint64_t bytes);
extern void taek_iris_dmabuf_soltar(int fd);

static void *crear_fs_yuv(struct coef k) {
    const nir_shader_compiler_options *o = pantalla->get_compiler_options(pantalla, PIPE_SHADER_FRAGMENT);
    nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_FRAGMENT, o, "taek_fs_nv12");
    nir_shader *s = b.shader;
    nir_variable *tc = variable(s, nir_var_shader_in, VARYING_SLOT_VAR0, 0, "v_tc");
    nir_variable *sal = variable(s, nir_var_shader_out, FRAG_RESULT_DATA0, 0, "color");
    const struct glsl_type *t2d = glsl_sampler_type(GLSL_SAMPLER_DIM_2D, false, false, GLSL_TYPE_FLOAT);
    nir_variable *sy = nir_variable_create(s, nir_var_uniform, t2d, "tex_y");
    nir_variable *suv = nir_variable_create(s, nir_var_uniform, t2d, "tex_uv");
    sy->data.binding = 0; sy->data.explicit_binding = true; sy->data.driver_location = 0;
    suv->data.binding = 1; suv->data.explicit_binding = true; suv->data.driver_location = 1;
    nir_def *uv = nir_channels(&b, nir_load_var(&b, tc), 0x3);
    nir_deref_instr *dy = nir_build_deref_var(&b, sy), *duv = nir_build_deref_var(&b, suv);
    nir_def *y = nir_channel(&b, nir_tex_deref(&b, dy, dy, uv), 0);
    nir_def *c = nir_tex_deref(&b, duv, duv, uv);
    nir_def *yy = nir_fadd_imm(&b, nir_fmul_imm(&b, y, k.ky), k.oy);
    nir_def *cb = nir_fmul_imm(&b, nir_fadd_imm(&b, nir_channel(&b, c, 0), -128.0 / 255.0), k.kc);
    nir_def *cr = nir_fmul_imm(&b, nir_fadd_imm(&b, nir_channel(&b, c, 1), -128.0 / 255.0), k.kc);
    nir_def *r = nir_fadd(&b, yy, nir_fmul_imm(&b, cr, k.rv));
    nir_def *g = nir_fsub(&b, nir_fsub(&b, yy, nir_fmul_imm(&b, cb, k.gu)), nir_fmul_imm(&b, cr, k.gv));
    nir_def *bl = nir_fadd(&b, yy, nir_fmul_imm(&b, cb, k.bu));
    nir_store_var(&b, sal, nir_vec4(&b, r, g, bl, nir_imm_float(&b, 1.0f)), 0xf);
    s->num_inputs = 1; s->num_outputs = 1;
    NIR_PASS(_, s, nir_lower_vars_to_ssa);
    NIR_PASS(_, s, nir_lower_global_vars_to_local);
    NIR_PASS(_, s, nir_split_var_copies);
    NIR_PASS(_, s, nir_lower_var_copies);
    NIR_PASS(_, s, nir_lower_system_values);
    NIR_PASS(_, s, nir_lower_samplers);
    s->info.num_textures = 2;
    nir_shader_gather_info(s, nir_shader_get_entrypoint(s));
    BITSET_SET(s->info.textures_used, 0); BITSET_SET(s->info.textures_used, 1);
    BITSET_SET(s->info.samplers_used, 0); BITSET_SET(s->info.samplers_used, 1);
    if (pantalla->finalize_nir) free(pantalla->finalize_nir(pantalla, s));
    struct pipe_shader_state st = {0};
    st.type = PIPE_SHADER_IR_NIR;
    st.ir.nir = s;
    return ctx->create_fs_state(ctx, &st);
}
static struct pipe_resource *importar_plano(int fd, const struct taek_video_exportado *e, enum pipe_format f, unsigned w, unsigned h, unsigned off) {
    struct pipe_resource tmpl = {0};
    tmpl.target = PIPE_TEXTURE_2D; tmpl.format = f; tmpl.width0 = w; tmpl.height0 = h; tmpl.depth0 = 1; tmpl.array_size = 1;
    tmpl.bind = PIPE_BIND_SAMPLER_VIEW; tmpl.usage = PIPE_USAGE_DEFAULT;
    struct winsys_handle wh;
    memset(&wh, 0, sizeof(wh));
    wh.type = WINSYS_HANDLE_TYPE_FD; wh.handle = (unsigned)fd; wh.stride = e->pitch; wh.offset = off;
    wh.modifier = e->modificador; wh.format = f; wh.plane = 0;   /* el tiling que declaro Media Driver al exportar */
    return pantalla->resource_from_handle(pantalla, &tmpl, &wh, 0);
}

static int prueba_video(char *t, size_t n) {
    int r = iniciar(t, n);
    if (r) return r;
    struct taek_video_exportado e;
    if (taek_media_exportado(&e)) { snprintf(t, n, "Media Driver no expuso ninguna superficie"); return -14; }
    unsigned W = e.ancho + e.recorte_x, H = e.alto + e.recorte_y;   /* tamano codificado (1080 se codifica como 1088) */
    int fd = taek_iris_dmabuf(e.handle, e.bytes);
    struct pipe_resource *ry = importar_plano(fd, &e, PIPE_FORMAT_R8_UNORM, W, H, e.off_y);
    struct pipe_resource *ruv = importar_plano(fd, &e, PIPE_FORMAT_R8G8_UNORM, W / 2, H / 2, e.off_uv);
    if (!ry || !ruv) { snprintf(t, n, "importar fallo: y=%p uv=%p handle=%u bytes=%llu", ry, ruv, e.handle, e.bytes); return -15; }

    /* Destino: del tamano del cuadro. Si coincide con la pantalla, ademas se muestra en el plano. */
    unsigned pa = 0, pl = 0;
    int pantalla_ok = !taek_media_plano_info(&pa, &pl) && pa == e.ancho && pl == e.alto;
    struct pipe_resource tmpl = {0};
    tmpl.target = PIPE_TEXTURE_2D; tmpl.format = PIPE_FORMAT_B8G8R8A8_UNORM;
    tmpl.width0 = e.ancho; tmpl.height0 = e.alto; tmpl.depth0 = 1; tmpl.array_size = 1;
    tmpl.bind = PIPE_BIND_RENDER_TARGET | (pantalla_ok ? PIPE_BIND_SCANOUT | PIPE_BIND_LINEAR | PIPE_BIND_SHARED : PIPE_BIND_SAMPLER_VIEW);
    tmpl.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *rt = pantalla->resource_create(pantalla, &tmpl);
    struct pipe_resource bt = {0};
    bt.target = PIPE_BUFFER; bt.format = PIPE_FORMAT_R8_UNORM; bt.width0 = 4 * 8 * sizeof(float); bt.height0 = 1; bt.depth0 = 1; bt.array_size = 1;
    bt.bind = PIPE_BIND_VERTEX_BUFFER; bt.usage = PIPE_USAGE_DEFAULT;
    struct pipe_resource *vb = pantalla->resource_create(pantalla, &bt);
    if (!rt || !vb) { snprintf(t, n, "resource_create fallo"); return -4; }
    /* Cuadrado a toda la salida; coordenadas de textura sobre la zona recortada del cuadro codificado. */
    float u0 = (float)e.recorte_x / W, v0 = (float)e.recorte_y / H, u1 = (float)(e.recorte_x + e.ancho) / W, v1 = (float)(e.recorte_y + e.alto) / H;
    float d[4][8] = {{-1, -1, 0, 1, u0, v0, 0, 1}, {1, -1, 0, 1, u1, v0, 0, 1}, {-1, 1, 0, 1, u0, v1, 0, 1}, {1, 1, 0, 1, u1, v1, 0, 1}};
    ctx->buffer_subdata(ctx, vb, PIPE_MAP_WRITE, 0, sizeof(d), d);

    struct coef k = coeficientes(e.matriz_color, e.rango_completo);
    uint64_t t0 = taek_media_ns();
    void *vs = crear_shader(0), *fs = crear_fs_yuv(k);
    uint64_t t_sh = taek_media_ns() - t0;
    if (!vs || !fs) { snprintf(t, n, "shaders: vs=%p fs=%p", vs, fs); return -7; }
    struct pipe_sampler_view vt, *vistas[2];
    memset(&vt, 0, sizeof(vt)); u_sampler_view_default_template(&vt, ry, ry->format); vistas[0] = ctx->create_sampler_view(ctx, ry, &vt);
    memset(&vt, 0, sizeof(vt)); u_sampler_view_default_template(&vt, ruv, ruv->format); vistas[1] = ctx->create_sampler_view(ctx, ruv, &vt);
    struct pipe_sampler_state ss = {0};
    ss.wrap_s = ss.wrap_t = ss.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
    ss.min_img_filter = ss.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
    ss.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
    void *m0 = ctx->create_sampler_state(ctx, &ss), *m1 = ctx->create_sampler_state(ctx, &ss), *muestreos[2] = {m0, m1};
    struct pipe_blend_state bl = {0};
    bl.rt[0].colormask = PIPE_MASK_RGBA;
    struct pipe_rasterizer_state ra = {0};
    ra.half_pixel_center = 1; ra.depth_clip_near = 1; ra.depth_clip_far = 1; ra.front_ccw = 1;
    ra.cull_face = PIPE_FACE_NONE; ra.line_width = 1.0f; ra.point_size = 1.0f;
    struct pipe_depth_stencil_alpha_state dsa = {0};
    struct pipe_vertex_element ve[2] = {0};
    ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[0].src_stride = 32;
    ve[1].src_offset = 16; ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[1].src_stride = 32;
    void *cso_bl = ctx->create_blend_state(ctx, &bl), *cso_ra = ctx->create_rasterizer_state(ctx, &ra);
    void *cso_dsa = ctx->create_depth_stencil_alpha_state(ctx, &dsa), *cso_ve = ctx->create_vertex_elements_state(ctx, 2, ve);
    ctx->bind_blend_state(ctx, cso_bl); ctx->bind_rasterizer_state(ctx, cso_ra);
    ctx->bind_depth_stencil_alpha_state(ctx, cso_dsa); ctx->bind_vertex_elements_state(ctx, cso_ve);
    ctx->bind_vs_state(ctx, vs); ctx->bind_fs_state(ctx, fs);
    ctx->set_sampler_views(ctx, PIPE_SHADER_FRAGMENT, 0, 2, 0, vistas);
    ctx->bind_sampler_states(ctx, PIPE_SHADER_FRAGMENT, 0, 2, muestreos);
    struct pipe_vertex_buffer vbuf = {0};
    pipe_resource_reference(&vbuf.buffer.resource, vb);
    ctx->set_vertex_buffers(ctx, 1, &vbuf);
    struct pipe_framebuffer_state fb = {0};
    fb.width = e.ancho; fb.height = e.alto; fb.layers = 1; fb.nr_cbufs = 1;
    fb.cbufs[0].format = tmpl.format; fb.cbufs[0].texture = rt;
    ctx->set_framebuffer_state(ctx, &fb);
    struct pipe_viewport_state vp = {{e.ancho / 2.0f, e.alto / 2.0f, 0.5f}, {e.ancho / 2.0f, e.alto / 2.0f, 0.5f}};
    ctx->set_viewport_states(ctx, 0, 1, &vp);
    ctx->set_sample_mask(ctx, ~0u);
    struct pipe_draw_info di = {0};
    di.mode = MESA_PRIM_TRIANGLE_STRIP; di.instance_count = 1;
    struct pipe_draw_start_count_bias dr = {0, 4, 0};
    t0 = taek_media_ns();
    ctx->draw_vbo(ctx, &di, 0, NULL, &dr, 1);
    struct pipe_fence_handle *f = NULL;
    ctx->flush(ctx, &f, 0);
    pantalla->fence_finish(pantalla, NULL, f, UINT64_MAX);
    pantalla->fence_reference(pantalla, &f, NULL);
    uint64_t t_gpu = taek_media_ns() - t0;

    /* Referencia de la CPU con la misma formula sobre la copia que hizo Media Driver del mismo cuadro. */
    struct pipe_box caja;
    u_box_2d(0, 0, e.ancho, e.alto, &caja);
    struct pipe_transfer *tr = NULL;
    const uint8_t *p = ctx->texture_map(ctx, rt, 0, PIPE_MAP_READ, &caja, &tr);
    unsigned malos = 0, dif_max = 0;
    for (unsigned y = 0; p && e.y && y < e.alto; y++) {
        const uint32_t *fila = (const uint32_t *)(p + (size_t)y * tr->stride);
        for (unsigned x = 0; x < e.ancho; x++) {
            float Y = e.y[(size_t)y * e.ancho + x] / 255.0f;
            const uint8_t *c = e.uv + (size_t)(y / 2) * e.ancho + (x & ~1u);
            float yy = Y * k.ky + k.oy, cb = (c[0] / 255.0f - 128.0f / 255.0f) * k.kc, cr = (c[1] / 255.0f - 128.0f / 255.0f) * k.kc;
            unsigned esp[3] = {a_byte(yy + k.rv * cr), a_byte(yy - k.gu * cb - k.gv * cr), a_byte(yy + k.bu * cb)};
            uint32_t g = fila[x];
            unsigned obt[3] = {(g >> 16) & 0xff, (g >> 8) & 0xff, g & 0xff}, peor = 0;
            for (int i = 0; i < 3; i++) { unsigned dd = esp[i] > obt[i] ? esp[i] - obt[i] : obt[i] - esp[i]; if (dd > peor) peor = dd; }
            if (peor > dif_max) dif_max = peor;
            malos += peor > 2;
        }
    }
    uint32_t centro = p ? ((const uint32_t *)(p + (size_t)(e.alto / 2) * tr->stride))[e.ancho / 2] : 0;
    if (p) ctx->texture_unmap(ctx, tr);

    int mostrado = -1, restaurado = -99;
    unsigned ggtt = 0;
    if (pantalla_ok && p && !malos) {
        struct winsys_handle wh;
        memset(&wh, 0, sizeof(wh));
        wh.type = WINSYS_HANDLE_TYPE_KMS;
        if (pantalla->resource_get_handle(pantalla, ctx, rt, &wh, 0)) {
            mostrado = taek_media_plano_mostrar(wh.handle, wh.stride, &ggtt);
            if (!mostrado) { taek_media_dormir_us(3000000); restaurado = taek_media_plano_restaurar(); }
        }
    }
    snprintf(t, n, "video NV12 %ux%u (codificado %ux%u) pitch=%u off_uv=%u mod=%llx matriz=%d completo=%d malos=%u dif_max=%u centro=%08x "
             "shaders_us=%llu gpu_us=%llu en_pantalla=%d restaurar=%d",
             e.ancho, e.alto, W, H, e.pitch, e.off_uv, e.modificador, e.matriz_color, e.rango_completo, malos, dif_max, centro,
             (unsigned long long)(t_sh / 1000), (unsigned long long)(t_gpu / 1000), mostrado, restaurado);

    ctx->set_vertex_buffers(ctx, 0, NULL);
    ctx->set_sampler_views(ctx, PIPE_SHADER_FRAGMENT, 0, 0, 2, NULL);
    struct pipe_framebuffer_state vacio = {0};
    ctx->set_framebuffer_state(ctx, &vacio);
    ctx->bind_vs_state(ctx, NULL); ctx->bind_fs_state(ctx, NULL);
    ctx->delete_vs_state(ctx, vs); ctx->delete_fs_state(ctx, fs);
    ctx->delete_sampler_state(ctx, m0); ctx->delete_sampler_state(ctx, m1);
    pipe_sampler_view_reference(&vistas[0], NULL); pipe_sampler_view_reference(&vistas[1], NULL);
    ctx->bind_blend_state(ctx, NULL); ctx->bind_rasterizer_state(ctx, NULL);
    ctx->bind_depth_stencil_alpha_state(ctx, NULL); ctx->bind_vertex_elements_state(ctx, NULL);
    ctx->delete_blend_state(ctx, cso_bl); ctx->delete_rasterizer_state(ctx, cso_ra);
    ctx->delete_depth_stencil_alpha_state(ctx, cso_dsa); ctx->delete_vertex_elements_state(ctx, cso_ve);
    pipe_resource_reference(&vb, NULL); pipe_resource_reference(&rt, NULL);
    pipe_resource_reference(&ry, NULL); pipe_resource_reference(&ruv, NULL);
    taek_iris_dmabuf_soltar(fd);
    if (!p) return -8;
    if (malos) return -6;
    return pantalla_ok && (mostrado || restaurado) ? -13 : 0;
}

/* ------------------------------------------------------------------ H195: presentacion del reproductor
 * Cada fotograma: la superficie NV12 de Media Driver (importada una vez por handle) se dibuja escalada en el bufer trasero de
 * dos bufers de escaneo, y el plano pasa a ese bufer en el vblank. Sin copias de la CPU. */
static struct {
    int listo, mostrando, cur, fs_matriz, fs_completo;
    unsigned an, al, handle[2], paso[2];
    struct pipe_resource *buf[2], *vb;
    void *vs, *fs, *bl, *ra, *dsa, *ve, *m0, *m1;
    /* H196: capa del programa (OSD) */
    struct pipe_resource *osd, *vb_osd;
    struct pipe_sampler_view *v_osd;
    unsigned osd_w, osd_h, osd_version, osd_subidas;
    int osd_hay;
    void *fs_osd, *bl_osd;
} pv;

/* H196: la capa del programa sobre el video, en (x, y) de pantalla. ARGB premultiplicado: ONE, ONE_MINUS_SRC_ALPHA, igual que la
 * mezcla de la CPU en osd_pintar. Se sube a la GPU solo cuando cambia su version. */
static int osd_dibujar(struct taek_video_peticion *r) {
    if (!pv.fs_osd) {
        pv.fs_osd = crear_fs_textura();
        struct pipe_blend_state bl = {0};
        bl.rt[0].blend_enable = 1;
        bl.rt[0].rgb_func = bl.rt[0].alpha_func = PIPE_BLEND_ADD;
        bl.rt[0].rgb_src_factor = bl.rt[0].alpha_src_factor = PIPE_BLENDFACTOR_ONE;
        bl.rt[0].rgb_dst_factor = bl.rt[0].alpha_dst_factor = PIPE_BLENDFACTOR_INV_SRC_ALPHA;
        bl.rt[0].colormask = PIPE_MASK_RGBA;
        pv.bl_osd = ctx->create_blend_state(ctx, &bl);
        struct pipe_resource bt = {0};
        bt.target = PIPE_BUFFER; bt.format = PIPE_FORMAT_R8_UNORM; bt.width0 = 4 * 8 * sizeof(float); bt.height0 = 1; bt.depth0 = 1; bt.array_size = 1;
        bt.bind = PIPE_BIND_VERTEX_BUFFER; bt.usage = PIPE_USAGE_STREAM;
        pv.vb_osd = pantalla->resource_create(pantalla, &bt);
        if (!pv.fs_osd || !pv.bl_osd || !pv.vb_osd) return -7;
    }
    if (!pv.osd || pv.osd_w != r->osd_w || pv.osd_h != r->osd_h) {   /* tamano nuevo: textura nueva */
        pipe_sampler_view_reference(&pv.v_osd, NULL);
        pipe_resource_reference(&pv.osd, NULL);
        struct pipe_resource t = {0};
        t.target = PIPE_TEXTURE_2D; t.format = PIPE_FORMAT_B8G8R8A8_UNORM; t.width0 = r->osd_w; t.height0 = r->osd_h;
        t.depth0 = 1; t.array_size = 1; t.bind = PIPE_BIND_SAMPLER_VIEW; t.usage = PIPE_USAGE_DEFAULT;
        pv.osd = pantalla->resource_create(pantalla, &t);
        if (!pv.osd) return -4;
        struct pipe_sampler_view vt;
        memset(&vt, 0, sizeof(vt)); u_sampler_view_default_template(&vt, pv.osd, pv.osd->format);
        pv.v_osd = ctx->create_sampler_view(ctx, pv.osd, &vt);
        pv.osd_w = r->osd_w; pv.osd_h = r->osd_h; pv.osd_hay = 0;
    }
    if (!pv.osd_hay || pv.osd_version != r->osd_version) {
        struct pipe_box caja;
        u_box_2d(0, 0, r->osd_w, r->osd_h, &caja);
        ctx->texture_subdata(ctx, pv.osd, 0, PIPE_MAP_WRITE, &caja, r->osd_px, r->osd_w * 4, (uintptr_t)r->osd_w * r->osd_h * 4);
        pv.osd_version = r->osd_version; pv.osd_hay = 1; pv.osd_subidas++;
    }
    float x0 = (float)r->osd_x / pv.an * 2 - 1, x1 = (float)(r->osd_x + r->osd_w) / pv.an * 2 - 1;
    float y0 = (float)r->osd_y / pv.al * 2 - 1, y1 = (float)(r->osd_y + r->osd_h) / pv.al * 2 - 1;
    float d[4][8] = {{x0, y0, 0, 1, 0, 0, 0, 1}, {x1, y0, 0, 1, 1, 0, 0, 1}, {x0, y1, 0, 1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1, 0, 1}};
    ctx->buffer_subdata(ctx, pv.vb_osd, PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, 0, sizeof(d), d);
    ctx->bind_blend_state(ctx, pv.bl_osd);
    ctx->bind_fs_state(ctx, pv.fs_osd);
    ctx->set_sampler_views(ctx, PIPE_SHADER_FRAGMENT, 0, 1, 1, &pv.v_osd);   /* 1 vista; suelta la segunda (UV) */
    void *m = pv.m0;
    ctx->bind_sampler_states(ctx, PIPE_SHADER_FRAGMENT, 0, 1, &m);
    struct pipe_vertex_buffer vbuf = {0};
    pipe_resource_reference(&vbuf.buffer.resource, pv.vb_osd);
    ctx->set_vertex_buffers(ctx, 1, &vbuf);
    struct pipe_draw_info di = {0};
    di.mode = MESA_PRIM_TRIANGLE_STRIP; di.instance_count = 1;
    struct pipe_draw_start_count_bias dr = {0, 4, 0};
    ctx->draw_vbo(ctx, &di, 0, NULL, &dr, 1);
    return 0;
}
#define IMP_MAX 48
static struct { unsigned handle; struct pipe_resource *ry, *ruv; struct pipe_sampler_view *vy, *vuv; } imp[IMP_MAX];
static unsigned nimp;

static void presentar_fin(void) {
    if (pv.mostrando) { taek_media_plano_restaurar(); pv.mostrando = 0; }
    if (!ctx) return;
    ctx->set_sampler_views(ctx, PIPE_SHADER_FRAGMENT, 0, 0, 2, NULL);
    ctx->set_vertex_buffers(ctx, 0, NULL);
    struct pipe_framebuffer_state vacio = {0};
    ctx->set_framebuffer_state(ctx, &vacio);
    for (unsigned i = 0; i < nimp; i++) {
        pipe_sampler_view_reference(&imp[i].vy, NULL); pipe_sampler_view_reference(&imp[i].vuv, NULL);
        pipe_resource_reference(&imp[i].ry, NULL); pipe_resource_reference(&imp[i].ruv, NULL);   /* Iris cierra su handle */
    }
    nimp = 0;
    if (!pv.listo) return;
    ctx->bind_vs_state(ctx, NULL); ctx->bind_fs_state(ctx, NULL);
    ctx->delete_vs_state(ctx, pv.vs);
    if (pv.fs) ctx->delete_fs_state(ctx, pv.fs);
    ctx->bind_blend_state(ctx, NULL); ctx->bind_rasterizer_state(ctx, NULL);
    ctx->bind_depth_stencil_alpha_state(ctx, NULL); ctx->bind_vertex_elements_state(ctx, NULL);
    ctx->delete_blend_state(ctx, pv.bl); ctx->delete_rasterizer_state(ctx, pv.ra);
    ctx->delete_depth_stencil_alpha_state(ctx, pv.dsa); ctx->delete_vertex_elements_state(ctx, pv.ve);
    ctx->delete_sampler_state(ctx, pv.m0); ctx->delete_sampler_state(ctx, pv.m1);
    pipe_resource_reference(&pv.vb, NULL);
    pipe_resource_reference(&pv.buf[0], NULL); pipe_resource_reference(&pv.buf[1], NULL);
    if (pv.fs_osd) ctx->delete_fs_state(ctx, pv.fs_osd);
    if (pv.bl_osd) ctx->delete_blend_state(ctx, pv.bl_osd);
    pipe_sampler_view_reference(&pv.v_osd, NULL);
    pipe_resource_reference(&pv.osd, NULL); pipe_resource_reference(&pv.vb_osd, NULL);
    memset(&pv, 0, sizeof(pv));
}

static int presentar_preparar(char *m, size_t n) {
    char t[160];
    if (iniciar(t, sizeof(t))) { snprintf(m, n, "iris: %s", t); return -1; }
    if (taek_media_plano_info(&pv.an, &pv.al)) { snprintf(m, n, "plano de pantalla no apto"); return -10; }
    for (int i = 0; i < 2; i++) {
        struct pipe_resource tmpl = {0};
        tmpl.target = PIPE_TEXTURE_2D; tmpl.format = PIPE_FORMAT_B8G8R8A8_UNORM;
        tmpl.width0 = pv.an; tmpl.height0 = pv.al; tmpl.depth0 = 1; tmpl.array_size = 1;
        tmpl.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT | PIPE_BIND_LINEAR | PIPE_BIND_SHARED;
        tmpl.usage = PIPE_USAGE_DEFAULT;
        pv.buf[i] = pantalla->resource_create(pantalla, &tmpl);
        struct winsys_handle wh;
        memset(&wh, 0, sizeof(wh));
        wh.type = WINSYS_HANDLE_TYPE_KMS;
        if (!pv.buf[i] || !pantalla->resource_get_handle(pantalla, ctx, pv.buf[i], &wh, 0)) { snprintf(m, n, "bufer de escaneo %d", i); return -4; }
        pv.handle[i] = wh.handle; pv.paso[i] = wh.stride;
    }
    struct pipe_resource bt = {0};
    bt.target = PIPE_BUFFER; bt.format = PIPE_FORMAT_R8_UNORM; bt.width0 = 4 * 8 * sizeof(float); bt.height0 = 1; bt.depth0 = 1; bt.array_size = 1;
    bt.bind = PIPE_BIND_VERTEX_BUFFER; bt.usage = PIPE_USAGE_STREAM;
    pv.vb = pantalla->resource_create(pantalla, &bt);
    pv.vs = crear_shader(0);
    struct pipe_blend_state bl = {0};
    bl.rt[0].colormask = PIPE_MASK_RGBA;
    struct pipe_rasterizer_state ra = {0};
    ra.half_pixel_center = 1; ra.depth_clip_near = 1; ra.depth_clip_far = 1; ra.front_ccw = 1;
    ra.cull_face = PIPE_FACE_NONE; ra.line_width = 1.0f; ra.point_size = 1.0f;
    struct pipe_depth_stencil_alpha_state dsa = {0};
    struct pipe_vertex_element ve[2] = {0};
    ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[0].src_stride = 32;
    ve[1].src_offset = 16; ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT; ve[1].src_stride = 32;
    /* Escalado: filtro lineal (el video casi nunca coincide con la pantalla). */
    struct pipe_sampler_state ss = {0};
    ss.wrap_s = ss.wrap_t = ss.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
    ss.min_img_filter = ss.mag_img_filter = PIPE_TEX_FILTER_LINEAR;
    ss.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
    pv.bl = ctx->create_blend_state(ctx, &bl); pv.ra = ctx->create_rasterizer_state(ctx, &ra);
    pv.dsa = ctx->create_depth_stencil_alpha_state(ctx, &dsa); pv.ve = ctx->create_vertex_elements_state(ctx, 2, ve);
    pv.m0 = ctx->create_sampler_state(ctx, &ss); pv.m1 = ctx->create_sampler_state(ctx, &ss);
    if (!pv.vb || !pv.vs) { snprintf(m, n, "vb o vs"); return -7; }
    pv.fs_matriz = -1;
    pv.listo = 1;
    return 0;
}

static int presentar_fotograma(struct taek_video_peticion *r) {
    if (!pv.listo) { int e = presentar_preparar(r->motivo, sizeof(r->motivo)); if (e) return e; }
    uint64_t t0 = taek_media_ns();
    if (!pv.fs || pv.fs_matriz != r->matriz_color || pv.fs_completo != r->rango_completo) {
        if (pv.fs) { ctx->bind_fs_state(ctx, NULL); ctx->delete_fs_state(ctx, pv.fs); }
        pv.fs = crear_fs_yuv(coeficientes(r->matriz_color, r->rango_completo));
        pv.fs_matriz = r->matriz_color; pv.fs_completo = r->rango_completo;
        if (!pv.fs) { snprintf(r->motivo, sizeof(r->motivo), "fs NV12"); return -7; }
    }
    unsigned W = r->ancho + r->recorte_x, H = r->alto + r->recorte_y, i = 0;
    while (i < nimp && imp[i].handle != r->iris_handle) i++;
    if (i == nimp) {   /* superficie nueva: se importa una vez */
        if (nimp >= IMP_MAX) { snprintf(r->motivo, sizeof(r->motivo), "demasiadas superficies"); return -16; }
        struct taek_video_exportado e;
        memset(&e, 0, sizeof(e));
        e.pitch = r->iris_pitch; e.off_y = r->iris_off_y; e.off_uv = r->iris_off_uv; e.modificador = r->iris_modificador;
        int fd = taek_iris_dmabuf(r->iris_handle, r->iris_bytes);
        imp[i].handle = r->iris_handle;
        imp[i].ry = importar_plano(fd, &e, PIPE_FORMAT_R8_UNORM, W, H, e.off_y);
        imp[i].ruv = importar_plano(fd, &e, PIPE_FORMAT_R8G8_UNORM, W / 2, H / 2, e.off_uv);
        taek_iris_dmabuf_soltar(fd);
        if (!imp[i].ry || !imp[i].ruv) { snprintf(r->motivo, sizeof(r->motivo), "importar NV12 (handle %u)", r->iris_handle); return -15; }
        struct pipe_sampler_view vt;
        memset(&vt, 0, sizeof(vt)); u_sampler_view_default_template(&vt, imp[i].ry, imp[i].ry->format);
        imp[i].vy = ctx->create_sampler_view(ctx, imp[i].ry, &vt);
        memset(&vt, 0, sizeof(vt)); u_sampler_view_default_template(&vt, imp[i].ruv, imp[i].ruv->format);
        imp[i].vuv = ctx->create_sampler_view(ctx, imp[i].ruv, &vt);
        nimp++;
    }
    /* Rectangulo destino: el pedido, o pantalla completa conservando la proporcion. */
    float dx, dy, dw, dh;
    if (r->dest_ancho > 0 && r->dest_alto > 0) { dx = r->dest_x; dy = r->dest_y; dw = r->dest_ancho; dh = r->dest_alto; }
    else {
        float e1 = (float)pv.an / r->ancho, e2 = (float)pv.al / r->alto, e = e1 < e2 ? e1 : e2;
        dw = r->ancho * e; dh = r->alto * e; dx = (pv.an - dw) / 2; dy = (pv.al - dh) / 2;
    }
    float x0 = dx / pv.an * 2 - 1, x1 = (dx + dw) / pv.an * 2 - 1, y0 = dy / pv.al * 2 - 1, y1 = (dy + dh) / pv.al * 2 - 1;
    float u0 = (float)r->recorte_x / W, v0 = (float)r->recorte_y / H, u1 = (float)(r->recorte_x + r->ancho) / W, v1 = (float)(r->recorte_y + r->alto) / H;
    float d[4][8] = {{x0, y0, 0, 1, u0, v0, 0, 1}, {x1, y0, 0, 1, u1, v0, 0, 1}, {x0, y1, 0, 1, u0, v1, 0, 1}, {x1, y1, 0, 1, u1, v1, 0, 1}};
    ctx->buffer_subdata(ctx, pv.vb, PIPE_MAP_WRITE | PIPE_MAP_DISCARD_WHOLE_RESOURCE, 0, sizeof(d), d);

    ctx->bind_blend_state(ctx, pv.bl); ctx->bind_rasterizer_state(ctx, pv.ra);
    ctx->bind_depth_stencil_alpha_state(ctx, pv.dsa); ctx->bind_vertex_elements_state(ctx, pv.ve);
    ctx->bind_vs_state(ctx, pv.vs); ctx->bind_fs_state(ctx, pv.fs);
    struct pipe_sampler_view *vistas[2] = {imp[i].vy, imp[i].vuv};
    void *muestreos[2] = {pv.m0, pv.m1};
    ctx->set_sampler_views(ctx, PIPE_SHADER_FRAGMENT, 0, 2, 0, vistas);
    ctx->bind_sampler_states(ctx, PIPE_SHADER_FRAGMENT, 0, 2, muestreos);
    struct pipe_vertex_buffer vbuf = {0};
    pipe_resource_reference(&vbuf.buffer.resource, pv.vb);
    ctx->set_vertex_buffers(ctx, 1, &vbuf);
    /* H201: el bufer donde se va a dibujar puede seguir en pantalla si el cambio anterior aun no se aplico; se espera solo
     * en ese caso (casi nunca: el vblank llega antes que el fotograma siguiente). */
    taek_media_plano_esperar();
    struct pipe_framebuffer_state fb = {0};
    fb.width = pv.an; fb.height = pv.al; fb.layers = 1; fb.nr_cbufs = 1;
    fb.cbufs[0].format = PIPE_FORMAT_B8G8R8A8_UNORM; fb.cbufs[0].texture = pv.buf[pv.cur];
    ctx->set_framebuffer_state(ctx, &fb);
    struct pipe_viewport_state vp = {{pv.an / 2.0f, pv.al / 2.0f, 0.5f}, {pv.an / 2.0f, pv.al / 2.0f, 0.5f}};
    ctx->set_viewport_states(ctx, 0, 1, &vp);
    ctx->set_sample_mask(ctx, ~0u);
    union pipe_color_union negro = {.f = {0, 0, 0, 1}};
    ctx->clear(ctx, PIPE_CLEAR_COLOR0, NULL, &negro, 0.0, 0);
    struct pipe_draw_info di = {0};
    di.mode = MESA_PRIM_TRIANGLE_STRIP; di.instance_count = 1;
    struct pipe_draw_start_count_bias dr = {0, 4, 0};
    ctx->draw_vbo(ctx, &di, 0, NULL, &dr, 1);
    if (r->osd_visible && r->osd_px && r->osd_w && r->osd_h && r->osd_x + r->osd_w <= pv.an && r->osd_y + r->osd_h <= pv.al) {
        int e = osd_dibujar(r);
        if (e) { snprintf(r->motivo, sizeof(r->motivo), "OSD: %d", e); return e; }
    }
    struct pipe_fence_handle *f = NULL;
    ctx->flush(ctx, &f, 0);
    pantalla->fence_finish(pantalla, NULL, f, UINT64_MAX);
    pantalla->fence_reference(pantalla, &f, NULL);
    /* El bufer esta listo: el plano pasa a el en el proximo vblank. H201: sin esperarlo aqui. */
    unsigned ggtt = 0;
    int rc = taek_media_plano_mostrar_async(pv.handle[pv.cur], pv.paso[pv.cur], &ggtt);
    if (rc) { snprintf(r->motivo, sizeof(r->motivo), "plano: %d", rc); return -12; }
    pv.mostrando = 1;
    pv.cur ^= 1;
    r->ns_iris = taek_media_ns() - t0;
    return 0;
}

int taek_iris_entrada(int orden, char *texto, size_t bytes) {
    int c = correr_ctors();
    if (orden == TAEK_IRIS_RT) { snprintf(texto, bytes, "constructores=%d", c); return 0; }
    if (orden == TAEK_IRIS_INIT) {
        int r = iniciar(texto, bytes);
        if (!r) snprintf(texto, bytes, "pantalla=%s contexto listo", pantalla->get_name(pantalla));
        return r;
    }
    if (orden == TAEK_IRIS_CLEAR) return prueba_clear(texto, bytes);
    if (orden == TAEK_IRIS_TRIANGULO) return prueba_triangulo(texto, bytes);
    if (orden == TAEK_IRIS_TEXTURA) return prueba_textura(texto, bytes);
    if (orden == TAEK_IRIS_PANTALLA) return prueba_pantalla(texto, bytes);
    if (orden == TAEK_IRIS_VIDEO) return prueba_video(texto, bytes);
    if (orden == TAEK_IRIS_OP) {
        struct taek_video_peticion *r = (struct taek_video_peticion *)(void *)texto;
        if (r->orden == TAEK_VIDEO_PRESENTAR_IRIS) return presentar_fotograma(r);
        if (r->orden == TAEK_VIDEO_IRIS_FIN) { presentar_fin(); return 0; }
        return -22;
    }
    snprintf(texto, bytes, "orden %d desconocida", orden);
    return -22;
}
