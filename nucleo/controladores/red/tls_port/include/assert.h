#ifndef TAEK_TLS_ASSERT_H
#define TAEK_TLS_ASSERT_H
#ifndef NDEBUG
#error "El puerto de Mbed TLS requiere la configuracion de release NDEBUG"
#endif
#define assert(expresion) ((void)0)
#endif
