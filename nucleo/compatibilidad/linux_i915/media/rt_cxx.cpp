// Parte propia del runtime C++ de Media Driver. El ABI real (__dynamic_cast, type_info, arboles, listas, hash) viene de las
// fuentes de libstdc++ del mismo commit que las cabeceras. Aqui solo queda lo que depende del sistema.
//
// Excepciones: no hay desenrollado. Lanzar una es un colapso con nombre, nunca un fallo silencioso. Solo 3 unidades las usan.
#define _GLIBCXX_DEFINING_STRING_INSTANTIATIONS 1
#include <chrono>
#include <fstream>
#include <new>
#include <string>
#include "taek_media.h"

extern "C" void *malloc(size_t);
extern "C" void free(void *);
extern "C" void *memalign(size_t, size_t);

template class std::__cxx11::basic_string<char>;

namespace std {
const nothrow_t nothrow{};
void terminate() noexcept { taek_media_trampa("std::terminate"); }
void __throw_bad_alloc() { taek_media_trampa("std::bad_alloc"); }
void __throw_bad_array_new_length() { taek_media_trampa("std::bad_array_new_length"); }
void __throw_bad_cast() { taek_media_trampa("std::bad_cast"); }
void __throw_logic_error(const char *m) { taek_media_trampa(m); }
void __throw_length_error(const char *m) { taek_media_trampa(m); }
void __throw_out_of_range(const char *m) { taek_media_trampa(m); }
void __throw_out_of_range_fmt(const char *m, ...) { taek_media_trampa(m); }
void __throw_invalid_argument(const char *m) { taek_media_trampa(m); }
void __throw_system_error(int) { taek_media_trampa("std::system_error"); }
namespace chrono { inline namespace _V2 {
steady_clock::time_point steady_clock::now() noexcept { return time_point(duration(static_cast<long>(taek_media_ns()))); }
} }
}  // namespace std

// Flujos de archivo: TAEK no tiene archivos para Media Driver, asi que abrir siempre falla, como con un archivo inexistente.
// Solo se construye lo que el codigo inline de las cabeceras lee: el vptr con el desplazamiento de la base virtual y el estado.
// Portar iostream y locale completos no daria nada mas. Cualquier otra operacion sigue siendo una trampa con nombre.
namespace {
using Ios = std::basic_ios<char>;
const size_t ESTADO = 32;   // ios_base::_M_streambuf_state: la autoprueba comprueba que rdstate() lo lee aqui.
const long tabla_if[] = {(long)(sizeof(std::ifstream) - sizeof(Ios)), 0, 0, 0, 0};
const long tabla_of[] = {(long)(sizeof(std::ofstream) - sizeof(Ios)), 0, 0, 0, 0};
void iniciar_flujo(void *o, const long *tabla, size_t base, int estado) {
    *static_cast<const long **>(o) = tabla + 3;
    *reinterpret_cast<int *>(static_cast<char *>(o) + base + ESTADO) = estado;
}
}  // namespace
extern "C" {
void ifs_c(void *o) __asm__("_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1Ev");
void ifs_cp(void *o, const char *, int) __asm__("_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode");
void ifs_d(void *o) __asm__("_ZNSt14basic_ifstreamIcSt11char_traitsIcEED1Ev");
void ofs_c(void *o) __asm__("_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev");
void ofs_cs(void *o, const void *, int) __asm__("_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1ERKNSt7__cxx1112basic_stringIcS1_SaIcEEESt13_Ios_Openmode");
void ofs_d(void *o) __asm__("_ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev");
void *fb_open(void *, const char *, int) __asm__("_ZNSt13basic_filebufIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode");
void *fb_close(void *) __asm__("_ZNSt13basic_filebufIcSt11char_traitsIcEE5closeEv");
bool bf_abierto(const void *) __asm__("_ZNKSt12__basic_fileIcE7is_openEv");
void ios_clear(void *, int) __asm__("_ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate");
void ifs_c(void *o) { iniciar_flujo(o, tabla_if, tabla_if[0], 0); }
void ifs_cp(void *o, const char *, int) { iniciar_flujo(o, tabla_if, tabla_if[0], std::ios_base::failbit); }
void ifs_d(void *) {}
void ofs_c(void *o) { iniciar_flujo(o, tabla_of, tabla_of[0], 0); }
void ofs_cs(void *o, const void *, int) { iniciar_flujo(o, tabla_of, tabla_of[0], std::ios_base::failbit); }
void ofs_d(void *) {}
void *fb_open(void *, const char *, int) { return nullptr; }
void *fb_close(void *) { return nullptr; }
bool bf_abierto(const void *) { return false; }
void ios_clear(void *ios, int estado) { *reinterpret_cast<int *>(static_cast<char *>(ios) + ESTADO) = estado; }
// Destructor integrado en el driver: lee estas VTT para reponer los vptr y solo necesita el desplazamiento de la base virtual.
// Todas las entradas valen la misma tabla porque el istream u ostream es el primer subobjeto.
extern const long *const vtt_if[8] __asm__("_ZTTSt14basic_ifstreamIcSt11char_traitsIcEE");
extern const long *const vtt_of[8] __asm__("_ZTTSt14basic_ofstreamIcSt11char_traitsIcEE");
const long *const vtt_if[8] = {tabla_if + 3, tabla_if + 3, tabla_if + 3, tabla_if + 3, tabla_if + 3, tabla_if + 3, tabla_if + 3, tabla_if + 3};
const long *const vtt_of[8] = {tabla_of + 3, tabla_of + 3, tabla_of + 3, tabla_of + 3, tabla_of + 3, tabla_of + 3, tabla_of + 3, tabla_of + 3};
void fb_d2(void *) __asm__("_ZNSt13basic_filebufIcSt11char_traitsIcEED2Ev");
void iosb_d2(void *) __asm__("_ZNSt8ios_baseD2Ev");
void bf_d1(void *) __asm__("_ZNSt12__basic_fileIcED1Ev");
void loc_d1(void *) __asm__("_ZNSt6localeD1Ev");
void fb_d2(void *) {}
void iosb_d2(void *) {}
void bf_d1(void *) {}    // Sin archivo abierto ni locale construido: nada que soltar.
void loc_d1(void *) {}
}

void *operator new(size_t n) { void *p = malloc(n); if (!p) taek_media_trampa("operator new sin memoria"); return p; }
void *operator new[](size_t n) { return operator new(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { free(p); }

extern "C" {
void *__dso_handle = nullptr;
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }   // Los modulos no se descargan.
void __cxa_pure_virtual() { taek_media_trampa("__cxa_pure_virtual"); }
void __cxa_bad_typeid() { taek_media_trampa("__cxa_bad_typeid"); }
void __cxa_bad_cast() { taek_media_trampa("__cxa_bad_cast"); }
void *__cxa_begin_catch(void *) noexcept { taek_media_trampa("__cxa_begin_catch: sin excepciones"); }
void __cxa_end_catch() { taek_media_trampa("__cxa_end_catch: sin excepciones"); }
void __cxa_rethrow() { taek_media_trampa("__cxa_rethrow: sin excepciones"); }
void *__cxa_allocate_exception(size_t) noexcept { taek_media_trampa("__cxa_allocate_exception: sin excepciones"); }
void __cxa_throw(void *, void *, void (*)(void *)) { taek_media_trampa("__cxa_throw: sin excepciones"); }
void _Unwind_Resume(void *) { taek_media_trampa("_Unwind_Resume: sin excepciones"); }
int __gxx_personality_v0() { taek_media_trampa("__gxx_personality_v0: sin excepciones"); }

// Estaticos locales: un CPU y fibras cooperativas. Volver a entrar durante el constructor seria una dependencia circular.
int __cxa_guard_acquire(unsigned char *g) {
    if (g[0]) return 0;
    if (g[1]) taek_media_trampa("__cxa_guard_acquire: inicializacion recursiva");
    g[1] = 1;
    return 1;
}
void __cxa_guard_release(unsigned char *g) { g[0] = 1; g[1] = 0; }
void __cxa_guard_abort(unsigned char *g) { g[1] = 0; }
}
