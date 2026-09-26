#!/usr/bin/env bash
set -euo pipefail

raiz="$(cd "$(dirname "$0")/.." && pwd)"
temporal="$(mktemp -d)"
trap 'rm -rf -- "$temporal"' EXIT

# Crear imagen FAT32 de 64M
truncate -s 64M "$temporal/fat32.img"
mkfs.fat -F 32 -n TAEK_FAT "$temporal/fat32.img" >/dev/null

# Crear archivos de prueba en host y copiarlos a la imagen FAT32 con mtools
echo "Bienvenido a TAEK OS. Este es un archivo de prueba leeme.txt en FAT32." > "$temporal/leeme.txt"
# Crear archivo de 10 000 bytes
python3 -c 'import sys; sys.stdout.write("".join([chr(ord("A") + (i % 26)) for i in range(10000)]))' > "$temporal/largo.txt"

mcopy -i "$temporal/fat32.img" "$temporal/leeme.txt" ::/leeme.txt
mcopy -i "$temporal/fat32.img" "$temporal/largo.txt" ::/largo.txt
mmd -i "$temporal/fat32.img" ::/subcarpeta

# Compilar el test con AddressSanitizer y UndefinedBehaviorSanitizer
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    "$raiz/tests/pruebas_fat32_host.c" "$raiz/nucleo/controladores/fat32.c" \
    -o "$temporal/pruebas_fat32_host"

# Ejecutar el test
"$temporal/pruebas_fat32_host" "$temporal/fat32.img"

# Validar integridad final con fsck.fat
fsck.fat -n "$temporal/fat32.img"
