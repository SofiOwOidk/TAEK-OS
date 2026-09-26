#!/usr/bin/env bash
set -euo pipefail

# Ejecutar desde cualquier directorio; la imagen temporal vive en FS nativo WSL.
raiz="$(cd "$(dirname "$0")/.." && pwd)"
temporal="$(mktemp -d)"
trap 'rm -rf -- "$temporal"' EXIT

truncate -s 64M "$temporal/exfat.img"
mkfs.exfat -n TAEK_TEST "$temporal/exfat.img" >/dev/null
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$raiz/tests/pruebas_exfat_host.c" "$raiz/nucleo/controladores/exfat.c" \
    -o "$temporal/pruebas_exfat_host"
"$temporal/pruebas_exfat_host" "$temporal/exfat.img"
fsck.exfat -n "$temporal/exfat.img"

truncate -s 64M "$temporal/exfat-fallo.img"
mkfs.exfat -n TAEK_TEST "$temporal/exfat-fallo.img" >/dev/null
"$temporal/pruebas_exfat_host" "$temporal/exfat-fallo.img" --fallo-io
