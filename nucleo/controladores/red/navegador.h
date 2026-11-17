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

/* H143: solo la descarga, para programas de Anillo 3. url: entrada y, a la salida, la URL final tras las redirecciones
 * (cap bytes). Deja en *cuerpo y *n el cuerpo ya sin cabeceras ni trozos (en un bufer propio del navegador, valido
 * hasta la siguiente descarga) y en *estado el codigo HTTP. 0 bien; si no, 1 URL, 2 red (*detalle: su codigo),
 * 3 respuesta, 4 redireccion. */
int navegador_descargar(char *url, size_t cap, const char **cuerpo, size_t *n, int *estado, int *detalle);

#endif // CONTROLADORES_RED_NAVEGADOR_H
