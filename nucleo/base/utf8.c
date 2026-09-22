#include "utf8.h"

uint32_t utf8_decodificar(const char **ptr) {
    const uint8_t *s = (const uint8_t *)*ptr;
    if (!s || *s == 0) return 0;

    uint32_t codepoint = 0;
    if ((*s & 0x80) == 0) {
        codepoint = *s++;
    } else if ((*s & 0xE0) == 0xC0) {
        codepoint = (*s++ & 0x1F) << 6;
        if ((*s & 0xC0) == 0x80) codepoint |= (*s++ & 0x3F);
    } else if ((*s & 0xF0) == 0xE0) {
        codepoint = (*s++ & 0x0F) << 12;
        if ((*s & 0xC0) == 0x80) codepoint |= (*s++ & 0x3F) << 6;
        if ((*s & 0xC0) == 0x80) codepoint |= (*s++ & 0x3F);
    } else if ((*s & 0xF8) == 0xF0) {
        codepoint = (*s++ & 0x07) << 18;
        if ((*s & 0xC0) == 0x80) codepoint |= (*s++ & 0x3F) << 12;
        if ((*s & 0xC0) == 0x80) codepoint |= (*s++ & 0x3F) << 6;
        if ((*s & 0xC0) == 0x80) codepoint |= (*s++ & 0x3F);
    } else {
        s++;
    }

    *ptr = (const char *)s;
    return codepoint;
}

uint8_t unicode_a_cp437(uint32_t cp) {
    if (cp < 128) return (uint8_t)cp;

    switch (cp) {
        case 0x00F1: return 0xA4; // ñ
        case 0x00D1: return 0xA5; // Ñ
        case 0x00E1: return 0xA0; // á
        case 0x00E9: return 0x82; // é
        case 0x00ED: return 0xA1; // í
        case 0x00F3: return 0xA2; // ó
        case 0x00FA: return 0xA3; // ú
        case 0x00C1: return 'A';  // Á -> A
        case 0x00C9: return 0x90; // É
        case 0x00CD: return 'I';  // Í -> I
        case 0x00D3: return 'O';  // Ó -> O
        case 0x00DA: return 'U';  // Ú -> U
        case 0x00FC: return 0x81; // ü
        case 0x00DC: return 0x9A; // Ü
        case 0x00A1: return 0xAD; // ¡
        case 0x00BF: return 0xA8; // ¿
        default:     return '?';
    }
}
