// Entrada de Media Driver en TAEK: constructores globales, autoprueba del runtime C++ e inicializacion del driver VA.
#include <fstream>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_prot.h>
#include <va/va_backend_vpp.h>
#include <va/va_drmcommon.h>
#include "taek_media.h"

extern "C" VAStatus __vaDriverInit_1_22(VADriverContextP ctx);
extern "C" int snprintf(char *, size_t, const char *, ...);
extern "C" void *memset(void *, int, size_t);
extern "C" void ifs_c(void *) __asm__("_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1Ev");
extern "C" void ifs_cp(void *, const char *, int) __asm__("_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode");
extern "C" void ifs_d(void *) __asm__("_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev");
extern "C" void ofs_cs(void *, const void *, int) __asm__("_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode");
extern "C" void ofs_d(void *) __asm__("_ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev");

typedef void (*ctor_t)();
__attribute__((section(".taek_ctors_ini"), used)) static ctor_t ctors_ini[1] = {nullptr};
__attribute__((section(".taek_ctors_fin"), used)) static ctor_t ctors_fin[1] = {nullptr};
static int ctors_hechos = -1;

static int correr_ctors() {
    if (ctors_hechos >= 0) return ctors_hechos;
    int n = 0;
    // Son dos objetos distintos para el compilador: se ocultan sus direcciones para poder recorrer lo que hay entre ellos.
    ctor_t *c = ctors_ini + 1, *fin = ctors_fin;
    asm volatile("" : "+r"(c), "+r"(fin));
    for (; c != fin; c++) {
        (*c)();
        n++;
    }
    ctors_hechos = n;
    return n;
}

// Jerarquias con los casos que distinguen a __dynamic_cast: bajada simple, cruzada, base virtual, base no publica y fallo.
namespace prueba {
struct B { virtual ~B() {} long b = 1; };
struct D : B { long d = 2; };
struct Otra : B { long o = 3; };
struct X { virtual ~X() {} long x = 4; };
struct M : D, X { long m = 5; };
struct VB { virtual ~VB() {} long v = 6; };
struct V1 : virtual VB { long a = 7; };
struct V2 : virtual VB { long c = 8; };
struct Rombo : V1, V2 { long r = 9; };
struct Privada : private B { B *yo() { return this; } };
struct Y1 : B {};
struct Y2 : B {};
struct Ambigua : Y1, Y2 {};
}  // namespace prueba

static unsigned autoprueba() {
    using namespace prueba;
    unsigned mal = 0;
    D d; Otra o; M m; Rombo r; Privada pr; Ambigua am;
    B *b = &d;
    if (dynamic_cast<D *>(b) != &d) mal |= 1;
    if (dynamic_cast<Otra *>(b)) mal |= 2;
    b = &o;
    if (dynamic_cast<D *>(b)) mal |= 2;
    X *x = &m;
    if (dynamic_cast<D *>(x) != static_cast<D *>(&m)) mal |= 4;
    if (dynamic_cast<M *>(x) != &m) mal |= 4;
    VB *v = &r;
    if (dynamic_cast<Rombo *>(v) != &r) mal |= 8;
    if (dynamic_cast<V2 *>(static_cast<V1 *>(&r)) != static_cast<V2 *>(&r)) mal |= 8;
    if (dynamic_cast<void *>(v) != static_cast<void *>(&r)) mal |= 8;
    if (dynamic_cast<Privada *>(pr.yo())) mal |= 16;
    B *y1 = static_cast<Y1 *>(&am);
    if (dynamic_cast<Y2 *>(y1) != static_cast<Y2 *>(&am)) mal |= 32;

    std::map<int, int> t;
    for (int i = 0; i < 2000; i++) t[(i * 7919) % 2003] = i;
    int previo = -1, cuenta = 0;
    for (auto &e : t) { if (e.first <= previo) mal |= 64; previo = e.first; cuenta++; }
    if (cuenta != 2000) mal |= 64;
    for (int i = 0; i < 2000; i += 2) t.erase((i * 7919) % 2003);
    if (t.size() != 1000 || t.count(0)) mal |= 64;

    std::list<int> l;
    for (int i = 0; i < 100; i++) l.push_back(i);
    l.reverse();
    if (l.front() != 99 || l.back() != 0 || l.size() != 100) mal |= 128;

    std::unordered_map<std::string, int> h;
    for (int i = 0; i < 500; i++) h[std::string("clave larga para el heap ") + std::to_string(i)] = i;
    if (h.size() != 500 || h["clave larga para el heap 321"] != 321) mal |= 256;

    std::string s("abc");
    s += "defghijklmnopqrstuvwxyz0123456789";
    if (s.size() != 36 || s.find("xyz") != 23 || s.find_last_of('c') != 2) mal |= 512;

    // Abrir un archivo falla igual que con un archivo inexistente. Se construye por los simbolos que llama el driver: con el
    // constructor normal el compilador lo integraria aqui y no se probaria esa ruta.
    alignas(16) static char m1[sizeof(std::ifstream)], m2[sizeof(std::ifstream)], m3[sizeof(std::ofstream)];
    ifs_c(m1);
    auto &e1 = *reinterpret_cast<std::ifstream *>(m1);
    if (!e1.good() || e1.is_open()) mal |= 1024;
    e1.open("/etc/igfx_user_feature_next.txt");
    if (e1.good() || !e1.fail()) mal |= 1024;
    e1.close();
    e1.std::ifstream::~basic_ifstream();   // Sin pasar por la tabla: integrado por el compilador, igual que en el driver.
    ifs_d(m1);
    ifs_cp(m2, "/etc/otro.txt", std::ios_base::in);
    auto &e2 = *reinterpret_cast<std::ifstream *>(m2);
    if (e2.good() || e2.is_open()) mal |= 1024;
    ifs_d(m2);
    std::string ruta("/tmp/salida");
    ofs_cs(m3, &ruta, std::ios_base::out);
    auto &e3 = *reinterpret_cast<std::ofstream *>(m3);
    if (e3.good() || e3.is_open()) mal |= 2048;
    ofs_d(m3);
    return mal;
}

extern "C" void taek_h264_contexto(VADriverContextP);
extern "C" int taek_h264_caps(char *, size_t);
extern "C" int taek_h264_decodificar(unsigned, unsigned, char *, size_t);
extern "C" int taek_h264_op(struct taek_video_peticion *);
static int driver_listo;

static int iniciar_driver(char *texto, size_t bytes) {
    static VADriverContext ctx;
    static VADriverVTable vt;
    static VADriverVTableVPP vpp;
    static VADriverVTableProt prot;
    static drm_state drm;
    memset(&ctx, 0, sizeof(ctx));
    drm.fd = taek_media_abrir_drm();
    if (drm.fd < 0) return drm.fd;
    drm.auth_type = VA_DRM_AUTH_CUSTOM;
    ctx.vtable = &vt;
    ctx.vtable_vpp = &vpp;
    ctx.vtable_prot = &prot;
    ctx.drm_state = &drm;
    ctx.display_type = VA_DISPLAY_DRM_RENDERNODES;
    ctx.version_major = 1;
    ctx.version_minor = 22;
    VAStatus st = __vaDriverInit_1_22(&ctx);
    snprintf(texto, bytes, "vaDriverInit=%d vendor=%s perfiles=%d entrypoints=%d", st, ctx.str_vendor ? ctx.str_vendor : "-", ctx.max_profiles, ctx.max_entrypoints);
    if (st == VA_STATUS_SUCCESS) { taek_h264_contexto(&ctx); driver_listo = 1; }
    return st == VA_STATUS_SUCCESS ? 0 : -5;
}

extern "C" int taek_media_entrada(int orden, char *texto, size_t bytes) {
    int n = correr_ctors();
    if (orden == TAEK_MEDIA_RT) {
        unsigned mal = autoprueba();
        snprintf(texto, bytes, "constructores=%d autoprueba_fallos=%x", n, mal);
        return mal ? -5 : 0;
    }
    if (orden == TAEK_MEDIA_INIT) return iniciar_driver(texto, bytes);
    if (!driver_listo) { snprintf(texto, bytes, "driver sin iniciar"); return -19; }
    if (orden == TAEK_MEDIA_CAPS) return taek_h264_caps(texto, bytes);
    if (orden == TAEK_MEDIA_IDR) return taek_h264_decodificar(1, 0, texto, bytes);
    if (orden == TAEK_MEDIA_MAL) return taek_h264_decodificar(1, 1, texto, bytes);
    if (orden == TAEK_MEDIA_VIDEO) return taek_h264_decodificar(300, 0, texto, bytes);
    if (orden == TAEK_MEDIA_OP) return taek_h264_op(reinterpret_cast<struct taek_video_peticion *>(texto));
    return -22;
}
