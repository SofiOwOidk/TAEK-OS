#ifndef CONTROLADORES_RED_NAVEGADOR_H
#define CONTROLADORES_RED_NAVEGADOR_H

#include <stdint.h>
#include <stddef.h>

/*
 * Navegador web ligero para la consola de TAEK-OS.
 *
 * Soporta esquemas 'http://' y 'https://' (TLS 1.3 nativo con cifrado ChaCha20-Poly1305).
 * Descarga el documento, analiza la estructura HTML (títulos, encabezados H1-H3,
 * párrafos, listas, enlaces hipertexto y entidades), y lo renderiza de forma
 * clara y con colores en la pantalla de la laptop.
 */
void navegador_abrir_url(const char *url_o_host);

#endif // CONTROLADORES_RED_NAVEGADOR_H
