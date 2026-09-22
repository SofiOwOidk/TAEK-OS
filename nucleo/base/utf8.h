#ifndef BASE_UTF8_H
#define BASE_UTF8_H

#include <stdint.h>

uint32_t utf8_decodificar(const char **ptr);
uint8_t  unicode_a_cp437(uint32_t codepoint);

#endif // BASE_UTF8_H
